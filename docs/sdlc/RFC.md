# RFC: Role-Based Access Control (RBAC) Extension for DuckDB

**Author:** [Your Name]  
**Date:** December 2024  
**Status:** Draft / Seeking Feedback  
**Target:** DuckDB Maintainers

---

## Summary

We propose implementing Role-Based Access Control (RBAC) as a DuckDB extension, providing table-level privileges, column-level privileges, and row-level security (RLS). This RFC outlines our approach using existing extension APIs and identifies areas where we'd appreciate guidance or potential collaboration.

---

## Motivation

### The Problem

DuckDB has no built-in access control mechanism. While this simplicity is a strength for many use cases, it presents challenges for organizations embedding DuckDB in multi-tenant or security-sensitive applications.

Currently, applications that need access control must:
- Implement authorization in the application layer (fragile, easy to bypass)
- Rewrite SQL queries to inject WHERE clauses (complex, error-prone)
- Maintain separate metadata about who can access what (out of sync with actual data)

### Our Use Case

We embed DuckDB in an analytics platform where:
- Multiple users query the same database
- Users should only see data relevant to their team/region
- Some columns contain sensitive information (PII, etc.)
- Authorization must be enforced consistently regardless of query complexity

### Why an Extension?

We believe RBAC fits well as an extension rather than core functionality because:
- Not all DuckDB users need access control
- Different deployments may want different authorization models
- Extension approach keeps the core simple
- Allows iteration without core release cycles

---

## Proposed Design

### Feature Overview

| Feature | Description |
|---------|-------------|
| **Roles** | Named entities that permissions are granted to |
| **Table privileges** | `GRANT SELECT ON table TO role` |
| **Column privileges** | `GRANT SELECT (col1, col2) ON table TO role` |
| **Row policies** | `CREATE ROW POLICY ... USING (filter_expr) TO role` |
| **Session identity** | Per-connection user name, roles, superuser flag |
| **Introspection** | System tables for querying permissions |

### SQL Syntax (ClickHouse/PostgreSQL-compatible)

```sql
-- Role management
CREATE ROLE analyst;
DROP ROLE analyst;
GRANT analyst TO alice;

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

---

## Implementation Approach

Based on our analysis of DuckDB's extension API, we plan to use the following hooks:

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

Installed via `ExtensionCallback::OnConnectionOpened`.

### 4. Storage in DuckDB Tables

Permissions stored in regular DuckDB tables (created by extension):

```sql
-- Created on extension load
CREATE TABLE IF NOT EXISTS duckdb_roles (
    role_name VARCHAR PRIMARY KEY,
    created_at TIMESTAMP DEFAULT current_timestamp
);

CREATE TABLE IF NOT EXISTS duckdb_table_privileges (
    grantee VARCHAR,
    table_name VARCHAR,
    privilege VARCHAR DEFAULT 'SELECT',
    PRIMARY KEY (grantee, table_name, privilege)
);

-- etc.
```

---

## Known Limitation: SELECT * Behavior

### The Problem

DuckDB expands `SELECT *` to an explicit column list during binding, **before** our optimizer extension runs.

**Example:**
```sql
-- Table: orders (id, customer, amount, secret_notes)
-- User has access to: (id, customer, amount)

SELECT * FROM orders;
-- After binding: SELECT id, customer, amount, secret_notes FROM orders
-- Our hook sees all 4 columns, can't tell it was SELECT *
```

### Current Plan (MVP)

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

## Questions for DuckDB Maintainers

### Extension API Questions

1. **OperatorExtension timing:** Does `OperatorExtension::Bind` fire before or after `SELECT *` expansion? Could it be used for column filtering?

2. **Expression binding:** For row policies, we need to inject bound expressions into the plan. Is there a recommended way to parse and bind an expression string against a table's schema from an optimizer extension?

3. **Connection properties:** Is there a mechanism for extensions to read custom connection properties set by the embedding application? We need to pass user identity.

4. **Error propagation:** We plan to throw exceptions from `pre_optimize_function`. Is this the recommended way to abort queries with errors?

### Design Questions

5. **Core hooks appetite:** If extension-only approaches prove too complex, would you be open to minimal, targeted hooks (like a column-access callback during binding)?

6. **Prior art:** Are you aware of other RBAC/RLS implementations for DuckDB we should look at? Or any internal discussions about access control?

7. **Future direction:** Is access control something DuckDB might add natively? We want to ensure our extension approach doesn't conflict with future plans.

---

## Alternatives Considered

### Alternative 1: Query Rewriting in Application Layer

Rewrite SQL queries before sending to DuckDB (add WHERE clauses, remove columns).

**Rejected because:**
- Complex to handle all SQL constructs (CTEs, subqueries, etc.)
- Easy to miss cases, leading to security holes
- Maintenance burden grows with query complexity

### Alternative 2: Wrap All Tables in Views

Create views that filter rows/columns, grant access only to views.

**Rejected because:**
- Doesn't scale (one view per user/role)
- View definition visible to users
- Complex maintenance

### Alternative 3: Virtual Tables via Table Functions

Implement each "protected table" as a table function that checks permissions.

**Rejected because:**
- Doesn't work for existing tables
- Poor query optimization (table functions are opaque to optimizer)
- Must reimplement table scan logic

---

## Implementation Plan

| Phase | Deliverable | Duration |
|-------|-------------|----------|
| Spikes | Validate extension hooks work | 1 day |
| Foundation | Extension skeleton, identity | 2 days |
| DDL | CREATE ROLE, GRANT, etc. | 3 days |
| Enforcement | Permission checks, row filters | 3 days |
| Polish | Introspection, error messages | 2 days |
| **Total** | MVP | ~11 days |

---

## Open Questions Summary

| # | Question | Context |
|---|----------|---------|
| 1 | OperatorExtension timing? | SELECT * filtering |
| 2 | Expression binding from extension? | Row policy injection |
| 3 | Custom connection properties? | User identity passing |
| 4 | Exception for errors? | Error handling pattern |
| 5 | Appetite for minimal core hooks? | If extension-only insufficient |
| 6 | Prior art / existing implementations? | Avoid duplicating work |
| 7 | Future access control plans? | Alignment |

---

## Conclusion

We believe RBAC can be implemented primarily as an extension using DuckDB's existing hooks. The main uncertainty is handling `SELECT *` elegantly, where we'd appreciate guidance on the best approach.

We're committed to:
- Clean, minimal implementation
- Following DuckDB patterns and conventions
- Contributing any core changes back if needed
- Documenting the extension for community use

We'd welcome feedback on this RFC, particularly on the questions above. Thank you for building such an excellent database!

---

## Appendix: Extension Hook Summary

| Hook | Location | Purpose in Our Design |
|------|----------|----------------------|
| `ParserExtension::parse_function` | `DBConfig::parser_extensions` | Custom DDL parsing |
| `ParserExtension::plan_function` | (with above) | Plan DDL as TableFunction |
| `OptimizerExtension::pre_optimize_function` | `DBConfig::optimizer_extensions` | Permission checks, filter injection |
| `ExtensionCallback::OnConnectionOpened` | `DBConfig::extension_callbacks` | Install per-connection state |
| `ClientContextState` | `context.registered_state` | Store user identity |
| `ExtensionLoader::RegisterFunction` | Extension load | `current_user()` function |

---

## Contact

[Your contact information]

We're happy to discuss further, provide code samples, or iterate on this proposal based on feedback.

