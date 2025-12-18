ClickHouse summary of **RBAC + column-level privileges + row-level security**, written so you can **verify it directly in ClickHouse code**

## 1) Core concepts ClickHouse uses

### Users vs roles

* A **user** is an account you can authenticate as and that carries account-specific settings (auth, defaults, grantee constraints, etc.). This is surfaced in `system.users`. ([ClickHouse][1])
* A **role** is a named bundle of privileges that can be granted to users **or to other roles** (role nesting / hierarchy). Roles are surfaced in `system.roles` and assignments in `system.role_grants`. ([ClickHouse][2])
* `GRANT` in ClickHouse is used for both **granting privileges** and **assigning roles** to users/roles. ([ClickHouse][3])

### Active roles and defaults

* Roles can be activated per session with `SET ROLE` and defaulted at login with `SET DEFAULT ROLE`. ([ClickHouse][4])
* Current/active roles are observable via:

  * `system.current_roles` (roles active for current user) ([ClickHouse][5])
  * `system.enabled_roles` (active roles incl. nested grants, plus flags for current/default/admin option) ([ClickHouse][6])

---

## 2) SQL surface you should expect to find/verify

### GRANT/REVOKE basics (privileges + role assignment)

* `GRANT` grants privileges to users/roles and assigns roles to users/roles. ([ClickHouse][3])
* `REVOKE` removes privileges, and supports **partial revokes**. ([ClickHouse][7])

### Column-level privileges

ClickHouse supports syntax like:

```sql
GRANT SELECT (x, y) ON db.table TO john WITH GRANT OPTION;
```

Semantics explicitly documented:

* `john` may `SELECT x`, `SELECT y`, `SELECT x,y`
* may **not** `SELECT z`
* and generally **cannot** do `SELECT *` (exception only if the table contains *only* those granted columns). ([ClickHouse][3])

### Partial revokes

ClickHouse documents revoking “part of a privilege” (e.g., revoke access to a table/DB/columns out of a broader wildcard grant). ([ClickHouse][7])

### Row-level security (row policies)

ClickHouse supports `CREATE ROW POLICY ... USING ...`:

* It “creates a row policy, i.e. a filter used to determine which rows a user can read from a table.” ([ClickHouse][8])
* General RBAC overview explicitly defines row policy as a filter for rows available to a user/role. ([ClickHouse][9])

---

## 3) “Catalog tables” (ClickHouse’s RBAC metadata exposure)

These are the main `system.*` tables you’d query (and whose contents should correspond to Access entities stored/loaded by AccessControl).

### `system.users`

Holds configured users and many account settings. Relevant RBAC-related columns include default role configuration and “grantees” constraints:

* `default_roles_all`, `default_roles_list`, `default_roles_except`
* `grantees_any`, `grantees_list`, `grantees_except` ([ClickHouse][1])

`ALTER USER ... GRANTEES ...` controls who a user can grant privileges to when they have `WITH GRANT OPTION` (e.g., `GRANTEES ANY EXCEPT ...`). ([ClickHouse][10])

### `system.roles`

Contains configured roles:

* `name`, `id`, `storage` (path configured by `access_control_path`). ([ClickHouse][2])

### `system.role_grants`

Role membership edges (role → user and role → role), including default-role and admin-option flags:

* `user_name` (nullable), `role_name` (nullable)
* `granted_role_name`
* `granted_role_is_default`
* `with_admin_option` ([ClickHouse][11])

### `system.grants`

Privileges granted to users/roles, including column-level scope and partial-revoke representation:

* `user_name` / `role_name` (nullable depending on grantee)
* `access_type`
* `database`, `table`, `column`
* `is_partial_revoke` (0 = grant row; 1 = partial revoke row)
* `grant_option` ([ClickHouse][12])

### `system.row_policies`

Row policy metadata (filters + applicability):

* includes `apply_to_all`, `apply_to_list`, `apply_to_except` (which roles/users it applies to) ([ClickHouse][13])
* and a “restrictive vs permissive” signal via `is_restrictive` in the system table schema (see the GitHub doc for the full column list). ([GitHub][14])

### Session/debug helpers

* `system.current_roles`: active roles for current user; changes with `SET ROLE`. ([ClickHouse][5])
* `system.enabled_roles`: “all active roles at the moment,” including current role + roles granted to it, with `is_current` and `is_default`. ([ClickHouse][6])

---

## 4) How this maps to ClickHouse source code (places to verify)

### AccessControl as the central manager

ClickHouse has a dedicated Access subsystem under `src/Access/`. The header `src/Access/AccessControl.h` states it “manages access control entities” and you can see the Access-related class ecosystem around it (Users, Roles, EnabledRoles, RowPolicyCache, etc.). ([GitHub][15])

Practical verification target:

* `AccessControl` is the object that owns/aggregates access storages (e.g., config-based storage, replicated storage, memory storage) and provides access entity retrieval and update operations.

### ContextAccess as “effective access for a session/query”

`src/Access/ContextAccess.cpp` exists and is the implementation backing the `DB::ContextAccess` class used to check privileges for a session context. ([GitHub][16])
This is where you should expect logic like:

* take user + enabled roles
* compute effective AccessRights
* check whether a given query action is allowed

### The data structure representing a grant element

ClickHouse represents grants in a structured form; in `AccessRightsElement.h` (from stable release doxygen), `AccessRightsElement` includes fields like:

* `columns`
* `wildcard`
* `grant_option`
* `is_partial_revoke`
  (and `access_flags`, db/table fields, etc.). ([Fossies][17])

This aligns directly with `system.grants` having `column`, `grant_option`, `is_partial_revoke`. ([ClickHouse][12])

### Role enabling / row policy enabling are separate computed layers

From the file list in the Access subsystem you can see dedicated components such as:

* `EnabledRoles`, `GrantedRoles`, `RoleCache`
* `EnabledRowPolicies`, `RowPolicyCache` ([GitHub][15])

This matches the operational model:

* roles can be assigned and then *enabled* per session (`SET ROLE` / default roles)
* row policies can be defined and then *enabled* based on current user/roles

---

## 5) End-to-end “how to validate via SQL” (examples)

### A) Roles, assignment, and session activation

```sql
CREATE ROLE analyst;
CREATE USER alice IDENTIFIED WITH sha256_password BY 'secret';

GRANT analyst TO alice;
SET DEFAULT ROLE analyst TO alice;

-- In alice’s session:
SET ROLE analyst;

SELECT * FROM system.current_roles;
SELECT * FROM system.enabled_roles;
```

`SET ROLE` / `SET DEFAULT ROLE` semantics are documented. ([ClickHouse][4])
`system.current_roles` / `system.enabled_roles` are documented. ([ClickHouse][5])

### B) Column-level SELECT

```sql
GRANT SELECT (x, y) ON db.table TO analyst;

-- analyst can:
SELECT x FROM db.table;
SELECT y FROM db.table;
SELECT x, y FROM db.table;

-- generally cannot:
SELECT * FROM db.table;
```

The `GRANT SELECT(x,y)` + `SELECT *` restriction is explicitly documented. ([ClickHouse][3])

### C) Partial revoke representation

```sql
GRANT SELECT ON db.table TO analyst;
REVOKE SELECT (secret_col) ON db.table FROM analyst;

SELECT user_name, role_name, access_type, database, table, column,
       grant_option, is_partial_revoke
FROM system.grants
WHERE role_name = 'analyst' AND database = 'db' AND table = 'table';
```

`system.grants` includes `column`, `grant_option`, and `is_partial_revoke` (partial revoke rows). ([ClickHouse][12])
Partial revokes are described in `REVOKE` docs. ([ClickHouse][7])

### D) Row policy definition + inspection

```sql
CREATE ROW POLICY tenant_42
ON mydb.events
USING tenant_id = 42
TO analyst;

SELECT name, database, table, select_filter, is_restrictive,
       apply_to_all, apply_to_list, apply_to_except
FROM system.row_policies
WHERE database = 'mydb' AND table = 'events';
```

Row policies are documented and `system.row_policies` columns include applicability sets. ([ClickHouse][8])


[1]: https://clickhouse.com/docs/operations/system-tables/users?utm_source=chatgpt.com "system.users - ClickHouse Docs"
[2]: https://clickhouse.com/docs/operations/system-tables/roles?utm_source=chatgpt.com "system.roles | ClickHouse Docs"
[3]: https://clickhouse.com/docs/sql-reference/statements/grant?utm_source=chatgpt.com "GRANT Statement - ClickHouse Docs"
[4]: https://clickhouse.com/docs/sql-reference/statements/set-role?utm_source=chatgpt.com "SET ROLE Statement - ClickHouse Docs"
[5]: https://clickhouse.com/docs/operations/system-tables/current_roles?utm_source=chatgpt.com "system.current_roles | ClickHouse Docs"
[6]: https://clickhouse.com/docs/operations/system-tables/enabled_roles?utm_source=chatgpt.com "system.enabled_roles - ClickHouse Docs"
[7]: https://clickhouse.com/docs/sql-reference/statements/revoke?utm_source=chatgpt.com "REVOKE Statement - ClickHouse Docs"
[8]: https://clickhouse.com/docs/sql-reference/statements/create/row-policy?utm_source=chatgpt.com "CREATE ROW POLICY - ClickHouse Docs"
[9]: https://clickhouse.com/docs/operations/access-rights?utm_source=chatgpt.com "Access Control and Account Management | ClickHouse Docs"
[10]: https://clickhouse.com/docs/sql-reference/statements/alter/user?utm_source=chatgpt.com "ALTER USER - ClickHouse Docs"
[11]: https://clickhouse.com/docs/operations/system-tables/role_grants?utm_source=chatgpt.com "system.role_grants | ClickHouse Docs"
[12]: https://clickhouse.com/docs/operations/system-tables/grants?utm_source=chatgpt.com "system.grants | ClickHouse Docs"
[13]: https://clickhouse.com/docs/operations/system-tables/row_policies?utm_source=chatgpt.com "system.row_policies - ClickHouse Docs"
[14]: https://github.com/ClickHouse/ClickHouse/blob/master/docs/en/operations/system-tables/row_policies.md?utm_source=chatgpt.com "ClickHouse/row_policies.md at master - GitHub"
[15]: https://github.com/ClickHouse/ClickHouse/blob/master/src/Access/AccessControl.h?utm_source=chatgpt.com "ClickHouse/src/Access/AccessControl.h at master - GitHub"
[16]: https://github.com/ClickHouse/ClickHouse/blob/master/src/Access/ContextAccess.cpp?utm_source=chatgpt.com "ClickHouse/src/Access/ContextAccess.cpp at master - GitHub"
[17]: https://fossies.org/dox/ClickHouse-25.7.1.3997-stable/AccessRightsElement_8h_source.html?utm_source=chatgpt.com "ClickHouse: src/Access/Common/AccessRightsElement.h Source File ..."
