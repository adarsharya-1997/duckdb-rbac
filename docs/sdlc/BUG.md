## BUGS

Bugs/issues found in the implementation. With suitable documentation of action taken to fix them.

### Known Limitations (Tested)
- Role names are case-sensitive (FR-5 not yet implemented). Tests: `ddl_syntax.test`, `identifiers_limitations.test`.
- `SELECT *` fails if any forbidden column exists (MVP limitation). Tests: `enforcement_column.test`.
- Identity is immutable once set. Tests: `core_extension_load.test`.

## State (`src/rbac_state.cpp`, `src/include/rbac_state.hpp`)

### 1) Superuser elevation paths (high)
- Default state starts as superuser and `rbac_set_identity` accepts `is_superuser` from user input.
- Impact: RBAC can be bypassed by omission or self-elevation.
- References: `src/include/rbac_state.hpp`, `src/rbac_state.cpp`
- Plan/Options:
- Option A (secure default): set `is_superuser = false` and require explicit identity.
- Option B (fail-closed): deny access if `initialized == false`.
- Option C: ignore user-provided `is_superuser` and derive it from roles/admin flag.

### 2) Effective roles silently ignore query errors (medium)
- Membership lookup errors are ignored and partial roles are returned.
- Impact: incorrect permission checks without surfacing failures.
- References: `src/rbac_state.cpp`
- Plan/Options:
- Option A: throw on query errors.
- Option B: log and fail closed (no roles) on errors.
- Option C: cache last known good roles for transient failures.

### 3) RBAC metadata tables writable by any user (high)
- RBAC metadata lives in normal tables with no write access control.
- Impact: direct `INSERT/UPDATE/DELETE` can bypass RBAC DDL.
- References: `src/rbac_state.cpp`, `src/rbac_optimizer.cpp`
- Plan/Options:
- Option A: block writes to `duckdb_*` RBAC tables for non-admins.
- Option B: move metadata into protected system tables.
- Option C: enforce writes only through RBAC DDL table function.

### 4) Identity input validation and dedup missing (low)
- `rbac_set_identity` accepts empty names and duplicate roles.
- Impact: invalid or redundant identity data.
- References: `src/rbac_state.cpp`
- Plan/Options:
- Option A: enforce non-empty, trimmed identifiers and max length.
- Option B: deduplicate roles on assignment.
- Option C: reject unknown/invalid role names.

### 5) Introspection view creation ignores errors (low)
- View creation does not check for errors.
- Impact: missing views can go unnoticed.
- References: `src/rbac_state.cpp`
- Plan/Options:
- Option A: use `ExecuteOrThrow`.
- Option B: log errors and fail in non-test builds.

## Parser (`src/rbac_parser.cpp`, `src/include/rbac_parser.hpp`)

### 6) RBAC DDL authorization missing (high)
- Any user can execute RBAC DDL; no admin check in `rbac_ddl_execute`.
- Impact: role/grant/policy changes by untrusted users.
- References: `src/rbac_parser.cpp`
- Plan/Options:
- Option A: require `is_superuser` for RBAC DDL.
- Option B: introduce an admin role and check membership.
- Option C: per-statement permissions (e.g., role owner can grant).

### 7) DDL is non-transactional (medium)
- DDL executes multiple statements without explicit transaction boundaries.
- Impact: partial updates on failure.
- References: `src/rbac_parser.cpp`
- Plan/Options:
- Option A: wrap each DDL in a transaction.
- Option B: reuse the `ClientContext` transaction instead of new connections.
- Option C: add compensation logic on failure.

### 8) Manual parsing and identifier handling (medium)
- DDL parsing relies on string slicing; quoted identifiers and flexible syntax are not supported.
- Impact: valid SQL can be misparsed or rejected.
- References: `src/rbac_parser.cpp`
- Plan/Options:
- Option A: reuse DuckDB parser structures.
- Option B: token-based parsing with proper identifier handling.
- Option C: narrow syntax and document constraints.

### 9) Policy expression validation is inconsistent (medium)
- CREATE-time validation only checks column existence; unsupported expressions fail at query time.
- Impact: policies can be created that later break queries.
- References: `src/rbac_parser.cpp`, `src/rbac_optimizer.cpp`
- Plan/Options:
- Option A: validate with the same whitelist as the optimizer binder.
- Option B: expand binder support for more expressions/functions.
- Option C: reject unsupported expressions with clear errors.

### 10) Parser errors expose internals (low)
- Raw exception messages are returned to users.
- Impact: internal details may leak.
- References: `src/rbac_parser.cpp`
- Plan/Options:
- Option A: sanitize user-facing errors.
- Option B: log details server-side and return generic errors.

## Optimizer (`src/rbac_optimizer.cpp`, `src/include/rbac_optimizer.hpp`)

### 11) Enforcement only covers SELECT/LogicalGet (high)
- Only `LogicalGet` with a table pointer is enforced.
- Impact: writes and non-table scans can bypass RBAC.
- References: `src/rbac_optimizer.cpp`
- Plan/Options:
- Option A: extend enforcement to write operators and other scans.
- Option B: deny queries with unrecognized scan operators for non-superusers.
- Option C: plan-wide audit of base relations.

### 12) Column grants checked before row-policy columns are added (medium)
- Row policies can add columns after permission checks.
- Impact: users can be forced to read non-granted columns for policy evaluation.
- References: `src/rbac_optimizer.cpp`
- Plan/Options:
- Option A: inject policies first, then re-check columns.
- Option B: allow policy columns only if user has grants on them.
- Option C: evaluate policies in a restricted scan path.

### 13) System-table bypass is name-only (low)
- RBAC system tables bypass checks by name, not schema.
- Impact: similarly named tables in other schemas could bypass checks.
- References: `src/rbac_optimizer.cpp`
- Plan/Options:
- Option A: check both schema and table name.
- Option B: use internal catalog identifiers.

### 14) Query error handling hides failures (low)
- `QueryHasRows()` returns false on error.
- Impact: errors are masked as "no privilege".
- References: `src/rbac_optimizer.cpp`
- Plan/Options:
- Option A: throw on errors with clear messages.
- Option B: log details and fail closed.

### 15) Row-policy errors leak internal details (low)
- Binder exceptions are included verbatim in user errors.
- Impact: schema/internal details may be exposed.
- References: `src/rbac_optimizer.cpp`
- Plan/Options:
- Option A: sanitize errors and return generic policy failure messages.
- Option B: log detailed errors separately.

### 16) Performance: repeated connections and IN lists (low)
- Permission checks build large IN clauses and create new `Connection` objects.
- Impact: overhead increases with complex role graphs and large role sets.
- References: `src/rbac_state.cpp`, `src/rbac_optimizer.cpp`
- Plan/Options:
- Option A: reuse `ClientContext` connections or cache per context.
- Option B: cache effective roles and invalidate on RBAC DDL.
- Option C: use parameterized queries or temp tables for role lists.
