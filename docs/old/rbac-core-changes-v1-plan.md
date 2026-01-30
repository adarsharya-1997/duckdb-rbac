# RBAC Core Changes Plan (DuckDB)

This document proposes **minimal, generic** DuckDB core changes that enable an RBAC extension to correctly enforce:

- Column-level security (CLS), including safe `SELECT *` expansion
- Row-level security (RLS) via injected `USING`/`WITH CHECK` predicates
- Prepared statement safety (rebind when policy state changes)
- An internal/bypass mechanism so the extension can read its own metadata tables

The goal is to keep the changes:

- **Minimally invasive** (small number of callsites in binder + small new structs)
- **Generic** (useful for other governance extensions: masking, tenancy isolation, auditing)
- **Extension-first** (policy storage + evaluation live in an extension)

---

## 1. Design Constraints and Existing Hook Points

### 1.1 Why optimizer-only rewriting is insufficient
Optimizer extensions exist (`DBConfig::optimizer_extensions`, `src/include/duckdb/optimizer/optimizer_extension.hpp`), and can rewrite logical plans.

However, RBAC requires **binder-time** enforcement for correctness:

- `SELECT *` / `t.*` must not expand to invisible columns
- DML requires column counts/type binding that happens before optimization
- View expansion and name binding must be subject to checks before planning

### 1.2 The binder has two central enforcement choke points
DuckDB already centralizes the core operations RBAC needs:

- Column binding: `BindContext::BindColumn` (`src/planner/bind_context.cpp`)
- Star expansion: `BindContext::GenerateAllColumnExpressions` (`src/planner/bind_context.cpp`)

Adding a security hook at these points covers a large portion of the surface area without scattering checks across many binders.

---

## 2. New Core Extension Surface: Security Extensions

Add a new extension vector to `DBConfig` alongside existing extension vectors.

### 2.1 `DBConfig::security_extensions`
**File:** `src/include/duckdb/main/config.hpp`

Add:

```cpp
vector<SecurityExtension> security_extensions;
```

This mirrors the structure of `optimizer_extensions` and avoids virtual interfaces.

### 2.2 New header: `duckdb/security/security_extension.hpp`
Introduce a small “function pointer + info” struct pattern consistent with `optimizer_extension.hpp`.

#### 2.2.1 Enums

```cpp
enum class SecurityAction : uint8_t {
  SELECT,
  INSERT,
  UPDATE,
  DELETE
};

enum class SecurityObjectType : uint8_t {
  TABLE,
  VIEW
};
```

#### 2.2.2 Object identity
Use a stable object identifier structure instead of passing catalog entries.

```cpp
struct QualifiedObjectName {
  string catalog;
  string schema;
  string name;
};
```

#### 2.2.3 Check hook

```cpp
struct SecurityCheckInput {
  ClientContext &context;
  Binder &binder;

  SecurityAction action;
  SecurityObjectType object_type;
  QualifiedObjectName object;

  // Empty => table-level check
  vector<string> columns;

  // true when check originated from STAR expansion
  bool is_star = false;

  // optional: whether this is the target of a DML statement
  bool is_dml_target = false;
};

struct SecurityCheckResult {
  bool allowed = true;
  string error_message; // optional if !allowed
};

typedef void (*security_check_t)(SecurityCheckInput &, SecurityCheckResult &);
```

This is generic enough for:

- deny access
- enforce allowlists
- enforce schema restrictions
- build audit events (by reading check inputs)

#### 2.2.4 Visible columns for `*`
To support **filtered star expansion** (required for CLS), add an optional hook.

```cpp
struct VisibleColumnsInput {
  ClientContext &context;
  Binder &binder;
  QualifiedObjectName object;
  SecurityAction action; // typically SELECT

  // All columns as currently visible by binding rules
  vector<string> all_columns;

  // If this is t.* we also provide the qualifier
  string relation_name;
};

struct VisibleColumnsResult {
  // indices into all_columns
  vector<idx_t> visible_column_indices;
};

typedef bool (*get_visible_columns_t)(VisibleColumnsInput &, VisibleColumnsResult &);
```

If the hook returns `false`, core uses default behavior (all columns visible).

#### 2.2.5 Row policy hook
RLS needs a way to inject predicates that are **bound by core**.

```cpp
struct RowPolicyRequest {
  ClientContext &context;
  Binder &binder;
  QualifiedObjectName object;
  SecurityAction action;

  // Alias used in this binding scope, so policy can be qualified
  string table_alias;
};

struct RowPolicyResult {
  bool policies_exist_for_table = false;

  // Parsed expressions (unbound). Binder will bind them in the correct scope.
  unique_ptr<ParsedExpression> using_expr;
  unique_ptr<ParsedExpression> with_check_expr;
};

typedef bool (*get_row_policy_t)(RowPolicyRequest &, RowPolicyResult &);
```

Notes:

- The extension can build expressions either by parsing stored SQL text or by constructing AST programmatically.
- Core remains responsible for binding the expression against the current table alias and scope.

#### 2.2.6 The full struct

```cpp
struct SecurityExtensionInfo {
  virtual ~SecurityExtensionInfo() = default;
};

struct SecurityExtension {
  security_check_t check_access = nullptr;
  get_visible_columns_t get_visible_columns = nullptr;
  get_row_policy_t get_row_policy = nullptr;

  shared_ptr<SecurityExtensionInfo> info;
};
```

---

## 3. Binder Call Sites (Minimal Set)

These are the minimal binder callsites that must consult the security extension to avoid bypasses.

### 3.1 Central read enforcement

#### 3.1.1 Column references
**File:** `src/planner/bind_context.cpp`

- In `BindContext::BindColumn(ColumnRefExpression &, idx_t depth)`
  - after resolving the binding alias/column name but before returning success
  - call `check_access(action=SELECT, columns={column})`

This covers:

- SELECT lists
- WHERE/HAVING/QUALIFY
- JOIN conditions
- RETURNING
- expressions inside INSERT/UPDATE RHS

#### 3.1.2 `SELECT *` / `t.*` expansion
**File:** `src/planner/bind_context.cpp`

- In `BindContext::GenerateAllColumnExpressions(StarExpression &, ...)`
  - before emitting the per-column expressions
  - ask the security extension for `get_visible_columns` for the binding/table and action=SELECT
  - emit only allowed columns

This is the critical hook to prevent leaks from `SELECT *`.

### 3.2 DML target checks (table-level)
DML target action (INSERT/UPDATE/DELETE) is only known in the statement binders.

#### DELETE
**File:** `src/planner/binder/statement/bind_delete.cpp`

- After confirming `LOGICAL_GET` and obtaining `TableCatalogEntry` (base table), call:
  - `check_access(action=DELETE, is_dml_target=true)`
- RLS injection:
  - request `get_row_policy(action=DELETE)` and inject `using_expr` as a filter on the target scan
  - must happen before binding the user `WHERE` clause

#### UPDATE
**File:** `src/planner/binder/statement/bind_update.cpp`

- After confirming base table, call:
  - `check_access(action=UPDATE, is_dml_target=true)`
- RLS injection:
  - `get_row_policy(action=UPDATE)` and inject `using_expr` before user WHERE is bound

#### INSERT
**File:** `src/planner/binder/statement/bind_insert.cpp`

- Right after retrieving target `TableCatalogEntry`, call:
  - `check_access(action=INSERT, is_dml_target=true)`
- WITH CHECK enforcement:
  - request `get_row_policy(action=INSERT)` and bind/enforce `with_check_expr`
  - enforcement point: after the source plan is constructed but before finalizing the insert plan

### 3.3 DML column-level checks (write permissions)

#### UPDATE SET columns
**File:** `src/planner/binder/statement/bind_update.cpp`

- In `Binder::BindUpdateSet(...)` loop, after resolving column existence and ensuring it is not generated:
  - `check_access(action=UPDATE, columns={colname}, is_dml_target=true)`

#### INSERT column list
**File:** `src/planner/binder/statement/bind_insert.cpp`

- In `Binder::BindInsertColumnList(...)`:
  - For explicit insert column lists: `check_access(action=INSERT, columns={each listed column}, is_dml_target=true)`
  - For insert-by-position without column list: treat as “write all physical columns” and check each

#### MERGE
**File:** `src/planner/binder/statement/bind_merge_into.cpp`

MERGE composes UPDATE/DELETE/INSERT actions and must check:

- table-level action rights based on contained actions
- per-action column-level rights
- RLS USING on target scan for UPDATE/DELETE
- WITH CHECK for INSERT and UPDATE where applicable

Implementation detail:

- After resolving target table and computing modification types, call the relevant table-level checks
- In `BindMergeAction`, after determining update/insert target columns, call the column-level checks
- Inject `using_expr` on the extracted target `LogicalGet` before binding actions

### 3.4 COPY inherits via SELECT/INSERT
- COPY TO binds a SELECT node; the central read hooks + star filtering cover it.
- COPY FROM generates and binds an InsertStatement; INSERT checks cover it.

---

## 4. Row Policy Binding/Injection Strategy

To keep changes minimal and generic, the binder should:

1. Request row policy expressions from the extension for the specific table/action.
2. Bind the returned `ParsedExpression` in the current scope using existing binders (e.g., WhereBinder-like binding).
3. Inject the result as:
   - `LogicalFilter` for USING
   - a CHECK step for WITH CHECK

### 4.1 Default-deny semantics
`RowPolicyResult::policies_exist_for_table` enables default-deny behavior:

- If `policies_exist_for_table=true` and `using_expr` is null, binder injects `FALSE` as the filter.

This matches common RLS models.

---

## 5. Prepared Statement Safety (Rebind on Policy Changes)

DuckDB already supports extension-driven rebind decisions via `ClientContextState` callbacks:

- `ClientContextState::CanRequestRebind` / `OnExecutePrepared` (`src/include/duckdb/main/client_context_state.hpp`)
- Execution path consults these hooks (`src/main/client_context.cpp`)

### 5.1 Minimal core addition: a stable “policy epoch” helper
Add a tiny core primitive for extensions to participate without reinventing concurrency.

#### Proposed core API
- `DatabaseInstance` holds an atomic `security_epoch` counter.
- Expose:

```cpp
uint64_t DatabaseInstance::GetSecurityEpoch() const;
void DatabaseInstance::BumpSecurityEpoch();
```

Extensions bump the epoch on GRANT/REVOKE/policy changes.

### 5.2 How rebinding works
- RBAC extension installs a `ClientContextState` on connection open that:
  - records epoch at prepare time (via `OnFinalizePrepare`) into the prepared statement (or its state)
  - requests rebind in `OnExecutePrepared` if `GetSecurityEpoch()` differs

This keeps policy invalidation out of the catalog versioning system and generic for other security-like extensions.

---

## 6. Scoped Bypass to Avoid Policy Recursion

Security extensions will need to read their own metadata tables (e.g. `rbac.*`) during binding.
Those reads must not recursively trigger security checks.

### 6.1 Proposed API
Add an RAII guard in `ClientContext`:

```cpp
class ScopedSecurityBypass {
public:
  explicit ScopedSecurityBypass(ClientContext &);
  ~ScopedSecurityBypass();
};

bool ClientContext::SecurityBypassActive() const;
```

### 6.2 Core behavior
- If bypass is active, the binder does not call security extension hooks.

This is safer than whitelisting schema names in SQL and avoids spoofing.

---

## 7. Error Handling and Diagnostics

### 7.1 Standard exception type
Use `ExceptionType::PERMISSION` (`src/include/duckdb/common/exception.hpp`) for denied access.

- If `SecurityCheckResult::allowed=false`, throw a `BinderException` (or a permission exception) with a consistent message.

### 7.2 Optional: trace hooks
Optionally, add a debug setting to log security hook invocations for explainability.

---

## 8. Implementation Order (Recommended)

1. Add `security_extension.hpp` and `DBConfig::security_extensions`
2. Add bypass guard to `ClientContext`
3. Add binder callsites:
   - `BindContext::BindColumn`
   - `BindContext::GenerateAllColumnExpressions`
4. Add DML callsites:
   - DELETE/UPDATE/INSERT/MERGE binders (table-level + write-columns + row policies)
5. Add optional `security_epoch` helper and document how extensions should use `ClientContextState` for prepared statements

---

## 9. What This Enables for an RBAC Extension

With these core hooks, an RBAC extension can:

- Implement `GRANT/REVOKE` and policy storage fully in SQL tables
- Enforce CLS reliably, including `SELECT *` safety
- Inject RLS predicates with correct binding semantics
- Prevent prepared-statement staleness after policy changes
- Read policy tables internally without recursion

This keeps core changes small and reusable while making RBAC feasible.
