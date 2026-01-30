# RBAC Extension Spike Testing Results

**Date:** January 2025  
**Status:** All 6 spikes completed successfully  
**Test File:** `test/sql/rbac/00_spikes.test` (54 assertions passing)

---

## Executive Summary

All six validation spikes passed, confirming that the DuckDB extension API supports the RBAC implementation approach outlined in our design documents. Key findings:

1. **Session state management works** via `ClientContextState` and `ExtensionCallback`
2. **Permission enforcement is feasible** via `OptimizerExtension::pre_optimize_function`
3. **Row policy filter injection works** by modifying the logical plan
4. **Custom DDL parsing works** via `ParserExtension` (with caveats)
5. **Dynamic expression binding is achievable** with a custom binder
6. **Column-level SELECT * handling** via `parser_override` query rewriting

**Decision:** Proceed with full implementation.

---

## Spike Results

### Spike 0.4: ClientContextState Stores Identity

**Goal:** Verify per-connection state storage for RBAC identity.

**Approach:**
- Created `RBACState : ClientContextState` with `user_name`, `roles`, `is_superuser`
- Registered `RBACExtensionCallback : ExtensionCallback` for connection lifecycle
- Used `ClientContext::registered_state->GetOrCreate<RBACState>("rbac")`
- Created scalar functions: `rbac_current_user()`, `rbac_current_roles()`, `rbac_is_superuser()`

**Key Code:**
```cpp
// Registration in LoadInternal()
auto &callback_manager = ExtensionCallbackManager::Get(db);
callback_manager.Register(make_shared_ptr<RBACExtensionCallback>());

// State retrieval
shared_ptr<RBACState> RBACState::Get(ClientContext &context) {
    return context.registered_state->GetOrCreate<RBACState>("rbac");
}
```

**Findings:**
- ✅ `RegisteredStateManager` provides thread-safe per-connection storage
- ✅ `ExtensionCallback::OnConnectionOpened` fires reliably
- ✅ State persists across queries within the same connection
- ⚠️ Must use `ExtensionCallbackManager::Get(db).Register()` not `DBConfig::extension_callbacks`

**Implications for Implementation:**
- Session identity will be stored in `RBACState`
- Default state is superuser (backwards compatibility)
- State initialization can happen in `OnConnectionOpened` or lazily

---

### Spike 0.2: OptimizerExtension Can Throw Exception

**Goal:** Verify that permission checks can abort queries with clear errors.

**Approach:**
- Registered `OptimizerExtension` with `pre_optimize_function`
- Walked logical plan to find `LogicalGet` nodes
- Threw `PermissionException` when accessing table named "blocked"

**Key Code:**
```cpp
OptimizerExtension RBACOptimizerExtension::Create() {
    OptimizerExtension ext;
    ext.pre_optimize_function = PreOptimize;
    return ext;
}

void PreOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
    // Walk plan, find LogicalGet, check table name
    if (table_name == "blocked") {
        throw PermissionException("Access denied to table 'blocked'");
    }
}
```

**Findings:**
- ✅ `pre_optimize_function` runs before built-in optimizers
- ✅ Exceptions propagate cleanly to the user
- ✅ `LogicalGet::GetTable()` returns `TableCatalogEntry*` for base tables
- ✅ `PermissionException` produces appropriate error output
- ⚠️ `LogicalGet::GetTable()` returns `nullptr` for table functions - must handle gracefully

**Implications for Implementation:**
- Permission checks will run in `pre_optimize_function`
- Must check `GetTable() != nullptr` before accessing table metadata
- Error messages should include user name, table name, and missing privilege

---

### Spike 0.3: OptimizerExtension Can Inject LogicalFilter

**Goal:** Verify that row policies can be injected as filters in the logical plan.

**Approach:**
- Extended optimizer hook to find `LogicalGet` for specific table
- Created hardcoded `BoundComparisonExpression` for `id > 0`
- Created `LogicalFilter` and inserted it above `LogicalGet`
- Used parent pointer pattern to rewire the plan tree

**Key Code:**
```cpp
void InjectHardcodedFilter(unique_ptr<LogicalOperator> &op_ptr, LogicalGet &get) {
    // Create: id > 0
    auto col_ref = make_uniq<BoundColumnRefExpression>(
        LogicalType::INTEGER,
        ColumnBinding(get.table_index, id_col_idx)
    );
    auto constant = make_uniq<BoundConstantExpression>(Value::INTEGER(0));
    auto comparison = make_uniq<BoundComparisonExpression>(
        ExpressionType::COMPARE_GREATERTHAN,
        std::move(col_ref), std::move(constant)
    );
    
    auto filter = make_uniq<LogicalFilter>(std::move(comparison));
    filter->children.push_back(std::move(op_ptr));
    op_ptr = std::move(filter);
}
```

**Findings:**
- ✅ `LogicalFilter` can be inserted anywhere in the plan tree
- ✅ Filter is applied correctly during execution
- ✅ Column bindings use `ColumnBinding(table_index, column_index)`
- ✅ Plan modification via `unique_ptr` reference works cleanly
- ⚠️ Must use `get.names` and `get.returned_types` to look up column metadata

**Implications for Implementation:**
- Row policies will be injected as `LogicalFilter` above `LogicalGet`
- Multiple policies for same table can be OR'd in a single filter
- Filter injection happens before built-in optimizers (may be optimized further)

---

### Spike 0.1: ParserExtension Catches Custom DDL

**Goal:** Verify that RBAC DDL (CREATE ROLE, GRANT, etc.) can be intercepted.

**Approach:**
- Implemented `ParserExtension` with `parse_function` and `plan_function`
- Detected "CREATE ROLE" and "DROP ROLE" via string matching
- Returned `TableFunction` that outputs confirmation message

**Key Code:**
```cpp
ParserExtensionParseResult ParseFunction(ParserExtensionInfo *info, const string &query) {
    string upper = StringUtil::Upper(query);
    if (StringUtil::StartsWith(upper, "CREATE ROLE ")) {
        string role_name = /* extract from query */;
        return ParserExtensionParseResult(
            make_uniq<RBACParseData>(StatementType::CREATE_ROLE, role_name)
        );
    }
    return ParserExtensionParseResult();  // Let DuckDB handle it
}

ParserExtensionPlanResult PlanFunction(..., unique_ptr<ParserExtensionParseData> parse_data) {
    ParserExtensionPlanResult result;
    result.function = GetRBACDDLFunction();
    result.parameters.push_back(Value("Role created: " + role_name));
    return result;
}
```

**Findings:**
- ✅ `parse_function` is called when DuckDB's parser fails
- ✅ `plan_function` can return any `TableFunction`
- ✅ Custom DDL like "CREATE ROLE foo" is intercepted correctly
- ⚠️ **Important limitation:** DuckDB's parser partially recognizes some keywords. For example, "DROP ROLE;" (without a name) triggers DuckDB's syntax error, not our extension's error. The extension only catches statements DuckDB *completely* fails to parse.
- ⚠️ If DuckDB adds native ROLE/GRANT syntax in the future, our extension would stop intercepting

**Implications for Implementation:**
- RBAC DDL will work for well-formed statements
- Malformed RBAC DDL may show DuckDB parser errors instead of custom errors
- Consider using `parser_override` if full error control is critical
- Monitor DuckDB releases for potential syntax conflicts

---

### Spike 0.5: Bind a Policy Expression Against a Table Scan

**Goal:** Verify that policy expressions can be dynamically parsed and bound.

**Approach:**
- Created `SimpleExpressionBinder` that converts `ParsedExpression` to bound `Expression`
- Used `Parser::ParseExpressionList()` to parse expression strings
- Built column name → index map from `LogicalGet::names`
- Supported: `ColumnRefExpression`, `ConstantExpression`, `ComparisonExpression`

**Key Code:**
```cpp
class SimpleExpressionBinder {
public:
    SimpleExpressionBinder(LogicalGet &get) : get(get) {
        for (idx_t i = 0; i < get.names.size(); i++) {
            column_map[get.names[i]] = i;
        }
    }
    
    unique_ptr<Expression> BindColumnRef(ColumnRefExpression &expr) {
        string col_name = expr.column_names.back();
        idx_t col_idx = column_map[col_name];
        return make_uniq<BoundColumnRefExpression>(
            get.returned_types[col_idx],
            ColumnBinding(get.table_index, col_idx)
        );
    }
    // ... BindConstant, BindComparison
};

unique_ptr<Expression> ParseAndBindExpression(const string &expr_str, LogicalGet &get) {
    auto expressions = Parser::ParseExpressionList(expr_str);
    SimpleExpressionBinder binder(get);
    return binder.Bind(*expressions[0]);
}
```

**Findings:**
- ✅ `Parser::ParseExpressionList()` parses arbitrary SQL expressions
- ✅ Column resolution works via name lookup against `LogicalGet::names`
- ✅ Type information available from `LogicalGet::returned_types`
- ✅ Error handling works - bad column names throw `BinderException`
- ✅ String comparisons (`status = 'active'`) work correctly
- ⚠️ Only simple expressions supported; complex expressions (functions, subqueries, etc.) would need extended binder

**Tested Patterns:**
- `id > 0` - integer comparison ✅
- `status = 'active'` - string equality ✅
- `nonexistent_column > 0` - error handling ✅

**Implications for Implementation:**
- Row policy expressions can be stored as strings and bound at query time
- `SimpleExpressionBinder` can be extended for more expression types as needed
- `current_user()` function support will require special handling in the binder
- Complex expressions may require using DuckDB's full `ExpressionBinder`

---

## API Reference Summary

### Registration APIs (in `LoadInternal`)

```cpp
// Extension callback (connection lifecycle)
ExtensionCallbackManager::Get(db).Register(make_shared_ptr<MyCallback>());

// Optimizer extension
ExtensionCallbackManager::Get(db).Register(MyOptimizerExtension::Create());

// Parser extension
ExtensionCallbackManager::Get(db).Register(MyParserExtension());

// Scalar functions
loader.RegisterFunction(ScalarFunction(...));
```

### Key Classes

| Class | Purpose |
|-------|---------|
| `ClientContextState` | Base class for per-connection state |
| `ExtensionCallback` | Connection lifecycle hooks |
| `OptimizerExtension` | Pre/post optimizer hooks |
| `ParserExtension` | Custom DDL parsing |
| `LogicalGet` | Table scan operator (has `GetTable()`, `names`, `returned_types`) |
| `LogicalFilter` | Filter operator (takes `Expression`) |
| `BoundColumnRefExpression` | Bound column reference |
| `BoundComparisonExpression` | Bound comparison |
| `PermissionException` | Permission denied error |

---

### Spike 0.6: SELECT * Column-Level Permission Feasibility

**Goal:** Determine if column-level permissions can work with `SELECT *` without core changes.

**Background:**
DuckDB expands `SELECT *` to an explicit column list during binding, **before** the optimizer runs. This means the optimizer cannot distinguish `SELECT *` from `SELECT a, b, c`.

**Three Approaches Tested:**

| Part | Approach | Result |
|------|----------|--------|
| A | Confirm optimizer sees expanded columns | ✅ Confirmed |
| B | Filter `column_ids` in optimizer | ❌ Breaks bindings |
| C | Use `parser_override` to rewrite SQL | ✅ Works! |

**Part A: Optimizer Column Expansion (Confirmed)**

```
[Spike 0.6A] col_test scan - column_ids: [a, b, c_secret] (total: 3)  # SELECT *
[Spike 0.6A] col_test scan - column_ids: [a, b] (total: 2)            # SELECT a, b
```

The optimizer sees all columns for `SELECT *`, only requested columns for explicit lists.

**Part B: Optimizer Column Filtering (Failed)**

```cpp
// Attempted to remove c_secret from column_ids
auto &col_ids = get.GetMutableColumnIds();
// ... filter out forbidden column ...
get.SetColumnIds(std::move(new_col_ids));
```

**Result:** `INTERNAL Error: Failed to bind column reference "c_secret" [0.2]`

The error occurs because other parts of the query plan still reference the removed column. Modifying `column_ids` alone is insufficient.

**Part C: Parser Override (Success!)**

```cpp
ParserOverrideResult RBACParserExtension::ParserOverride(ParserExtensionInfo *info, const string &query) {
    // Detect "SELECT * FROM col_test_rewrite" and rewrite
    if (upper == "SELECT * FROM COL_TEST_REWRITE ORDER BY A") {
        string rewritten = "SELECT a, b FROM col_test_rewrite ORDER BY a";
        Parser parser;
        parser.ParseQuery(rewritten);
        
        ParserOverrideResult result;
        result.type = ParserExtensionResultType::PARSE_SUCCESSFUL;
        result.statements = std::move(parser.statements);
        return result;
    }
    // Fall through to normal parsing
    ParserOverrideResult result;
    result.type = ParserExtensionResultType::DISPLAY_ORIGINAL_ERROR;
    return result;
}
```

**Configuration Required:**
```sql
SET allow_parser_override_extension = 'fallback';
```

**Output:**
```
[Spike 0.6C] Intercepted: SELECT * FROM col_test_rewrite ORDER BY a
[Spike 0.6C] Rewriting to: SELECT a, b FROM col_test_rewrite ORDER BY a
```

Query returned only 2 columns as expected.

**Implications for Implementation:**

1. **Parser Override is the solution** for column-level `SELECT *` handling
2. Must use `fallback` mode (not `strict_when_supported`) to allow pass-through
3. Requires SQL parsing to detect `SELECT *` patterns and table names
4. Requires catalog lookup to determine allowed columns per user
5. Complexity: Must handle all SQL variations (joins, subqueries, CTEs, etc.)

**Alternative:** Accept limitation - error on forbidden columns in `SELECT *`, require explicit column lists. This is simpler and may be acceptable for many use cases.

---

## Files Created

```
src/
├── include/
│   ├── rbac_state.hpp      # RBACState, RBACExtensionCallback
│   ├── rbac_optimizer.hpp  # RBACOptimizerExtension
│   └── rbac_parser.hpp     # RBACParserExtension, RBACParseData
├── rbac_state.cpp          # Session state + scalar functions
├── rbac_optimizer.cpp      # Permission checks + filter injection
└── rbac_parser.cpp         # DDL parsing + parser_override

test/sql/rbac/
└── 00_spikes.test          # All spike tests (54 assertions)
```

---

## Conclusion

All 6 spikes passed. The DuckDB extension API provides the necessary hooks for implementing RBAC:

1. **Permission Enforcement:** `OptimizerExtension::pre_optimize_function` ✅
2. **Row-Level Security:** `LogicalFilter` injection ✅
3. **Custom DDL:** `ParserExtension` (with known limitations) ✅
4. **Session Identity:** `ClientContextState` ✅
5. **Expression Binding:** Custom binder approach ✅
6. **Column-Level SELECT *:** `parser_override` rewriting ✅

**Decision:** Proceed to Phase 1: Foundation
