# RFC: Role-Based Access Control (RBAC) Extension for DuckDB

DuckDB has no built-in access control mechanism. 

ClickHouse does: https://clickhouse.com/docs/operations/access-rights

### Our Use Case

We embed DuckDB in an application where:
- All data is stored in memory. No files.
- Multiple users query the same instance.
- Users should only see data relevant to them.
- Some columns contain sensitive information.

### Why an Extension?

Iterate quickly.
- The implementation is going to be fairly complex, and who's going to review that?
- It will be done entirely by AI agents, and duckdb has a policy against AI written PRs.

## Proposal

```sql
-- Role management
CREATE ROLE analyst;
DROP ROLE analyst;
GRANT analyst TO alice; -- alice is a user not a role

-- Table privileges
GRANT SELECT ON orders TO analyst;
REVOKE SELECT ON orders FROM analyst;

-- Column privileges
GRANT SELECT (id, customer, amount) ON orders TO analyst;

-- Row policies
CREATE ROW POLICY regional_filter ON orders
    FOR SELECT
    USING (region = current_user_region())
    TO analyst;

-- Introspection
SELECT * FROM duckdb_roles;
SELECT * FROM duckdb_table_privileges;
SELECT * FROM duckdb_row_policies;
```

## Approach

### 1. Custom DDL via ParserExtension

Since DuckDB doesn't parse `CREATE ROLE`, `GRANT`, etc., we'll use `ParserExtension::parse_function` as a fallback parser:

```cpp
class RBACParserExtension : public ParserExtension {
public:
    RBACParserExtension() {
        parse_function = ParseRBACStatement;
        plan_function = PlanRBACStatement;
    }
    
    static ParserExtensionParseResult ParseRBACStatement(
        ParserExtensionInfo *info, const string &query) {
        // Parse CREATE ROLE, GRANT, etc.
        // Return custom ParserExtensionParseData
    }
    
    static ParserExtensionPlanResult PlanRBACStatement(
        ParserExtensionInfo *info, ClientContext &context,
        unique_ptr<ParserExtensionParseData> parse_data) {
        // Return TableFunction that executes the DDL
    }
};
```

### 2. Permission Enforcement via OptimizerExtension

We'll use `OptimizerExtension::pre_optimize_function` to:
- Walk the `LogicalOperator` tree
- Find `LogicalGet` nodes (table scans)
- Check table/column permissions
- Inject `LogicalFilter` for row policies
- Throw exception on permission denied

```cpp
class RBACOptimizerExtension : public OptimizerExtension {
public:
    RBACOptimizerExtension() {
        pre_optimize_function = EnforceRBAC;
    }
    
    static void EnforceRBAC(OptimizerExtensionInput &input,
                           unique_ptr<LogicalOperator> &plan) {
        auto &context = input.context;
        auto *rbac_state = GetRBACState(context);
        
        if (rbac_state->is_superuser) {
            return;  // Bypass all checks
        }
        
        // Walk plan, check permissions, inject filters
        WalkAndEnforce(plan, rbac_state);
    }
};
```

### 3. Session State via ClientContextState

We'll store per-connection identity using `ClientContextState`:

```cpp
class RBACState : public ClientContextState {
public:
    string user_name;
    vector<string> roles;
    bool is_superuser = false;
    bool initialized = false;
    
    // Permission cache
    unordered_map<string, PermissionSet> table_permissions;
};
```

Installed via `ExtensionCallback::OnConnectionOpened`?

## Known Limitation: SELECT * Behavior

DuckDB expands `SELECT *` to an explicit column list during binding, **before** our optimizer extension runs.

**Example:**
```sql
-- Table: orders (id, customer, amount, secret_notes)
-- User has access to: (id, customer, amount)

SELECT * FROM orders;
-- After binding: SELECT id, customer, amount, secret_notes FROM orders
-- Our hook sees all 4 columns, can't tell it was SELECT *
```

For MVP, if any forbidden column appears in the bound query, we **error**:
```
Permission denied on column 'secret_notes' of table 'orders'
```

This means `SELECT *` on tables with restricted columns will fail. Users must use explicit column lists.

### Potential Solutions (Seeking Guidance)

We've considered several approaches and would appreciate DuckDB maintainers' input:

#### Option A: Parser Override

Use `ParserExtension::parser_override` to intercept all SQL, detect `SELECT *`, query catalog for allowed columns, and rewrite before DuckDB binds.

**Pros:** No core changes  
**Cons:** Significant complexity, must handle all SQL edge cases

#### Option B: Binder-Level Hook

We noticed `OperatorExtension` exists but are unsure if it fires before or after `*` expansion. Could this be used?

**Question:** Does `OperatorExtension::Bind` see the query before `SELECT *` is expanded?

#### Option C: Column Access Callback in Binder (Core Change)

A small hook in the binder that fires during `*` expansion:

```cpp
// Hypothetical hook in Binder::ExpandStarExpression
for (auto &col : table.columns) {
    if (column_access_callback && !column_access_callback(col)) {
        continue;  // Skip forbidden column
    }
    expanded_columns.push_back(col);
}
```

**Question:** Would such a hook be acceptable if we contributed it? We'd make it opt-in and minimal.

#### Option D: Accept Limitation

Document that `SELECT *` requires table-level access (all columns). This is actually reasonable for many use cases.

---

## Still researching

1. Does `OperatorExtension::Bind` fire before or after `SELECT *` expansion? Could it be used for column filtering?

2. For row policies, we need to inject bound expressions into the plan. Is there a recommended way to parse and bind an expression string against a table's schema from an optimizer extension?

3. Is there a mechanism for extensions to read custom connection properties set by the embedding application? We need to pass user identity.

4. We plan to throw exceptions from `pre_optimize_function`. Is this the recommended way to abort queries with errors?

## Conclusion

We believe RBAC can be implemented primarily as an extension using DuckDB's existing hooks. 
The main uncertainty is handling `SELECT *` elegantly, where we'd appreciate guidance on the best approach.

We'd welcome feedback on this RFC, particularly on the questions above.

Thank you for building such an excellent database!