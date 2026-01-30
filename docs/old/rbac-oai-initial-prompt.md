## Goal

Implement **RBAC as a DuckDB extension** that provides:

* **CLS (Column-Level Security)** via `GRANT/REVOKE` on tables + specific columns
* **RLS (Row-Level Security)** via row policies (filters) applied automatically to scans / DML
* **Embedded model**: identity is provided by the host application (no in-DB users table, no authentication, no masking, no quotas)

The extension should do **all policy storage + logic**, while DuckDB core only adds the **minimum hooks/APIs** required to enforce correctly in the binder/planner/executor.

---

## High-level architecture

### Identity model (embedded)

* Host app sets session identity:

  * `SET rbac.user = 'alice'`
  * `SET rbac.roles = 'analyst,finance'` (optional; can be empty)
* No `users` table; “user” is an external string principal.

### Extension responsibilities

* Provide `GRANT/REVOKE`, `CREATE ROLE`, `GRANT role`, `CREATE ROW POLICY`, etc. (native syntax via parser extension; parser fallback multi-statement issue was fixed by DuckDB PR #7868).
* Store all policy state inside DuckDB as tables under an internal schema (e.g., `rbac.*`).
* Evaluate effective privileges and row filters for `(rbac.user, rbac.roles)` with caching + invalidation.

### Core responsibilities

* Expose stable hooks for:

  * privilege checks (table/column) including `SELECT *` expansion
  * row policy injection (RLS filter) + optional `WITH CHECK` enforcement
  * prepared statement recheck on policy changes
  * internal bypass so the extension can read `rbac.*` tables without recursion

---

## Minimal “catalog” surface needed (extension-owned)

We decided **not** to implement ClickHouse-style `system.users` equivalents (auth/hosts/default DB etc.). We strictly need:

1. `rbac.roles`

* role names (optionally IDs)
* minimal metadata

2. `rbac.role_membership`

* edges: role → member (member can be USER string or ROLE for nesting)
* optionally `admin_option` (can omit in v1 if you don’t need delegation)

3. `rbac.privileges`

* grants for table-level and column-level privileges
* columns:

  * `grantee_type` in {USER, ROLE}
  * `grantee`
  * `catalog/schema/object_name/object_type`
  * `privilege` in {SELECT, INSERT, UPDATE, DELETE}
  * `column_name` nullable (NULL => table-level)
  * timestamps/audit fields optional

4. `rbac.row_policies`

* RLS policies per table (and optionally per action)
* store predicate(s) as SQL strings to be parsed/bound:

  * `using_expr_sql` (row visibility filter)
  * optionally `with_check_sql` (insert/update check)
* policy scoping:

  * applies to USER, ROLE, or ALL
  * optionally “restrictive” flag (for AND semantics); otherwise OR-only MVP

Also provide **views/table functions** for introspection (like ClickHouse `system.grants`, `system.role_grants`, `system.row_policies`, `system.enabled_roles`) but computed on demand rather than stored.

---

## Enforcement semantics (what must be true)

### Column-level security (CLS)

Enforced in binder:

* Binding `FROM t`: require `SELECT` on table **or** allow column-level selection only (policy-defined; recommended: require SELECT at least at some level to access the table at all).
* Binding `t.c`: require `SELECT(c)` or table-level `SELECT`.
* DML:

  * `INSERT INTO t(cols...)`: require `INSERT` on table OR `INSERT(cols...)`
  * `UPDATE t SET c=...`: require `UPDATE(c)` or table-level `UPDATE`
  * `DELETE FROM t`: require `DELETE` on table
* **Critical**: `SELECT *` / `t.*` must not leak. Binder must consult RBAC at star-expansion time.

  * recommended behavior: expand only **visible columns** returned by RBAC provider; if none, error.

### Row-level security (RLS)

Enforced by injecting filters into plans:

* For `SELECT`: attach a `USING` predicate as a filter on each base table scan covered by a policy.
* For `UPDATE/DELETE`: apply the same filter to the target scan so you can only modify/delete visible rows.
* For `INSERT/UPDATE`: recommended to enforce a `WITH CHECK` predicate over new row values (otherwise you can insert rows outside your permitted slice).

  * MVP simplification: if no explicit `WITH CHECK`, reuse `USING` as the check.

Default-deny row behavior:

* If **any** policy exists for a table and no policy applies to the principal, return `FALSE` filter (see zero rows). This matches common RLS models and ClickHouse’s “policies define access; uncovered users see nothing”.

Policy combination:

* MVP: OR all applicable policies.
* Optional ClickHouse-like: `(OR permissive) AND (AND restrictive)`.

---

## Core changes required (minimum set)

### 1) Principal retrieval

A stable way for extension + core hooks to read current principal:

* `ClientContext` → `{user, roles}` (backed by settings `rbac.user`, `rbac.roles`)

### 2) Authorization hook for privileges + columns + star expansion

Add an interface callable from binder:

* `CheckPrivilege(object_type, privilege, catalog/schema/name, columns[])`
* plus a way for star expansion to get `visible_columns` for a table.

Binder must call:

* on table/view binding (SELECT and DML targets)
* on column binding
* on `*` expansion

### 3) Row policy hook (RLS injection)

Add an interface callable from binder:

* `GetRowPolicy(table, action)` → returns:

  * `using_filter` expression (nullable)
  * optional `with_check` expression
  * `policies_exist_for_table` (to implement default-deny rows)

Binder must apply:

* inject `using_filter` into scan / logical filter
* enforce `with_check` for insert/update (recommended)

### 4) Prepared statement safety (policy-change invalidation)

Minimal mechanism:

* global `authz_epoch` counter in core (uint64)
* extension bumps epoch on any policy/grant change
* prepared statements store epoch at bind time
* on execute: if epoch changed → force rebind or recheck dependencies

### 5) Internal bypass to avoid recursion

Extension must be able to read `rbac.*` tables without triggering auth checks:

* scoped bypass (`RunWithInternalAccess`) or whitelist schema `rbac` for internal reads.

### Optional: DDL lifecycle hook

If you want “owner gets rights” / default privileges:

* `OnCreateObject` hook so extension can add implicit grants.
  Not strictly required for MVP.

---

## Extension implementation plan (for an agent with code access)

### A) Parser + DDL execution (native syntax)

* Implement custom parser extension that recognizes:

  * `CREATE ROLE`, `DROP ROLE`
  * `GRANT role TO <user|role>` / `REVOKE role FROM ...`
  * `GRANT <privileges> ON <object> [ (columns) ] TO <user|role>`
  * `REVOKE ...`
  * `CREATE ROW POLICY ... ON <table> USING <expr> [WITH CHECK <expr>] TO ...`
  * `DROP ROW POLICY ...`
* Execution of these statements translates to inserts/deletes/updates in `rbac.*` tables.
* After any change: bump `authz_epoch`.

(If parser extension returns “not mine” for other statements, DuckDB handles them normally; multi-statement fallback problems were improved by PR #7868.)

### B) RBAC policy engine

* Read principal from session.
* Expand active roles via transitive closure over `rbac.role_membership`.
* Evaluate privilege checks:

  * table-level allow OR per-column allow
  * implement star visible-columns list
* Evaluate row policies:

  * fetch policies for table + action where principal matches
  * parse predicates from stored SQL into expression AST
  * combine (OR, optional restrictive AND)
  * if policies exist but none match → return `FALSE`

### C) Caching

* Cache per `(user, active_roles, authz_epoch)`:

  * expanded role set
  * effective privilege map per object
  * row policy expression per table/action
* Invalidate on epoch change.

### D) Introspection

Provide views/table functions:

* `rbac.roles`, `rbac.role_grants`, `rbac.grants`, `rbac.row_policies`
* `rbac.enabled_roles()` computed for current session
* optionally `rbac.show_effective_grants(principal)` for debugging.

---

## Explicit non-features (do not implement)

* no masking policies
* no quotas/settings profiles
* no authentication/passwords/host restrictions inside DuckDB
* no attempt to sandbox extensions (extensions run with host privileges)

---

## Key correctness points to watch in DuckDB code

* Ensure `SELECT *` expansion is filtered by RBAC provider.
* Ensure RLS filters are injected on every base scan (joins/subqueries included).
* Ensure view expansion does not bypass checks (invoker semantics).
* Ensure prepared statements are invalidated/rechecked when policies change.
* Ensure internal reads of `rbac.*` don’t recurse into auth checks.

This is the minimal, extension-first design: core provides only the enforcement hooks and invalidation primitives; the extension owns all policy state and logic.
