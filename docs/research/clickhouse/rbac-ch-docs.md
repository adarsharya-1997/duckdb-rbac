# ClickHouse RBAC design notes (implementation-facing summary)

This is a design-level summary of how role-based access control (RBAC) works in ClickHouse, based on the ClickHouse documentation.

## 1) Core objects

ClickHouse models authorization around a small set of first-class “access entities”:

- **Users**: principals that authenticate and open sessions.
- **Roles**: named bundles of privileges; assignable to users and to other roles.
- **Privileges**: permissions to run specific classes of queries/actions, scoped to objects.
- **Row policies**: row-level filters applied to reads (row-level security).
- **Settings profiles**: groups of runtime settings + constraints, assignable to users/roles.
- **Quotas**: resource-usage limits over time intervals, assignable to users/roles.

From a design standpoint, ClickHouse treats roles/policies/profiles/quotas similarly to tables: they can be created/altered/dropped via SQL and inspected via system metadata.

## 2) Privilege model

### 2.1 What a privilege grants

A privilege permits a user to execute a specific kind of operation (e.g., `SELECT`, `INSERT`, `CREATE TABLE`, `ALTER USER`, etc.). ClickHouse documents a large privilege set organized into a hierarchy (e.g., `ALL` contains `ACCESS MANAGEMENT`, which contains `CREATE USER`, `DROP ROLE`, `SHOW ACCESS`, etc.).

The key design aspect is that privileges have **scope** (where they apply) and **granularity** (how narrow the permission can be).

### 2.2 Scope and granularity

ClickHouse supports scoping privileges at different levels:

- **Global** (all databases and tables)
- **Database** (all tables within a database)
- **Table** (a specific table)
- **Column-level** for `SELECT` (e.g., granting `SELECT(x, y)`)

ClickHouse also supports wildcard scoping patterns. In addition to `*` (all), ClickHouse supports “prefix wildcard” patterns (e.g., granting on `db.my_tables*` so that newly created objects under that prefix inherit the grants).

Design implication: ClickHouse privileges are not just “action on object”, but “action on object-scope-pattern”, which can include a path prefix match.

### 2.3 Combination rules

Effective permissions for a session are essentially a **union**:

- Privileges granted directly to the user
- Privileges granted to any roles that are active for that session

This unioned set determines what queries the session may execute.

## 3) Roles and session role activation

### 3.1 Roles as privilege bundles

A role is a named collection of privileges. Users can be granted multiple roles, and roles can be granted to other roles (role hierarchy / inheritance).

### 3.2 Roles are not necessarily always “on”

A distinctive feature is that ClickHouse allows a user to enable a subset of their granted roles per session.

- A user can have **default roles** that are automatically activated at login.
- The user can change the active role set during a session via `SET ROLE`.

Effective privileges are computed from the currently enabled roles + direct user grants.

Design implication: ClickHouse’s model is closer to “role activation” (like `SET ROLE`) than pure static role membership.

### 3.3 Delegation controls for roles

When granting roles, ClickHouse distinguishes two important delegation/administration capabilities:

- **ADMIN OPTION for role grants**: allows the grantee to administer (grant/revoke) that role.
- **GRANT OPTION for privilege grants**: allows the grantee to grant onward privileges within their own scope.

This separation mirrors systems that distinguish “can use role” vs “can grant role”.

## 4) GRANT / REVOKE and delegation boundaries

### 4.1 Two kinds of `GRANT`

ClickHouse uses `GRANT` for:

1) granting **privileges** to users/roles
2) assigning **roles** to users/roles

There is also a `WITH REPLACE OPTION` behavior, which replaces previous grants/role assignments instead of appending.

### 4.2 Partial revokes

ClickHouse supports “partial revokes”: you can grant a broad privilege (e.g., `SELECT` on everything) and then revoke narrower portions (e.g., revoke on a specific database/table/column) to carve out exceptions.

Design implication: representing authorization state may require tracking both grants and negative exceptions (or a normalized form that can compute them).

### 4.3 “Who can I grant to?” (GRANTEES)

ClickHouse adds an additional delegation boundary: a user account can be configured with a **GRANTEES** list (or `ANY` / `NONE`) that constrains which users/roles it may grant privileges to, even if it has `GRANT OPTION`.

Design implication: this is a second axis of delegation control beyond the normal “do you have grant option” check.

## 5) Row-level security (row policies)

### 5.1 What row policies do

Row policies implement row-level security for reads:

- A policy attaches a boolean condition (filter) to a table (or database-wide pattern).
- The policy applies to a specified set of users/roles.
- Only rows satisfying the condition are visible.

ClickHouse emphasizes that row policies are meaningful only for users that cannot bypass them via write/DDL capabilities.

### 5.2 Default behavior when any policy exists

A non-obvious ClickHouse behavior: **once any row policy is defined for a table**, access to that table becomes policy-dependent for everyone.

- If a user has no matching policy, they may see **no rows**.
- To avoid this, you can define an explicit “allow all” policy for the remainder of users (e.g., a policy with condition `1` for `ALL EXCEPT ...`).

Design implication: policy existence flips a table from “open rows” to “policy-governed rows”; absent matching policy implies default-deny at row level.

### 5.3 Combining multiple policies

ClickHouse supports multiple policies per table and defines combination semantics:

- **Permissive** policies combine with logical **OR** (default).
- **Restrictive** policies combine with logical **AND**.

General form:

- visible = (OR of permissive conditions) AND (AND of restrictive conditions)

Design implication: this is more expressive than a single predicate per user/table and can model “must satisfy all constraints X plus at least one entitlement Y”.

## 6) Settings profiles and quotas (governance layer)

ClickHouse RBAC isn’t limited to query permissions; it also supports operational controls that are assignable to the same principals (users/roles).

### 6.1 Settings profiles

A settings profile is a named set of settings and constraints:

- Can be assigned to users/roles.
- Can restrict or pin certain settings (min/max/readonly/const-like constraints).

This is used for “soft authorization” such as limiting `readonly`, memory usage, or permitting certain settings changes.

### 6.2 Quotas

Quotas enforce or track resource usage over time intervals.

- Limits can be expressed per interval (minute/hour/day/etc.) and across multiple intervals.
- Quotas can be keyed (per user, per IP, per client-provided key, etc.), controlling how usage is shared/aggregated.

Design implication: quotas are not just per-user counters; they can be parameterized by keys supplied by the client.

## 7) Storage and management of access entities

ClickHouse can store access entities in different “access storages”:

- Local on-disk directory storage
- In-memory storage
- Replicated storage (backed by ZooKeeper)
- Read-only storages derived from static configuration (e.g., users.xml)
- LDAP-backed read-only user directory

Entities can be moved between storages (where supported).

Design implication: the access-control plane is designed to be **pluggable** and **replicable**, which matters for distributed deployments.

## 8) Introspection and auditability

ClickHouse provides both SQL statements and system metadata tables to inspect the RBAC configuration:

- Human-readable introspection:
  - list roles/users/policies/profiles/quotas
  - show grants
  - show “create” representation of access entities
  - show all access configuration (“show access”)

- Machine-readable inspection via system tables:
  - tables for privileges granted
  - tables for role membership / default roles / admin option
  - tables for row policies
  - tables for settings profiles and their elements
  - tables for quotas and quota limits/usages

Design implication: there is an explicit, queryable metadata model for RBAC, which makes it scriptable and testable.

## 9) Impersonation

ClickHouse supports executing queries “as another user” (impersonation), guarded by:

- a server-level enable setting, and
- an `IMPERSONATE` privilege that can be granted (including “any user” impersonation).

Design implication: impersonation is treated as a privileged capability distinct from normal DDL/DML privileges.

## 10) Design takeaways you might reuse in DuckDB

If you’re adding RBAC to DuckDB, ClickHouse suggests a few patterns worth considering:

- **Role activation**: separate “member-of role” from “active roles for this session” (supports least privilege during interactive use).
- **Delegation as two controls**:
  - privilege-grant delegation (`GRANT OPTION`)
  - role-admin delegation (`ADMIN OPTION`)
- **Policy composition**: support multiple row policies with OR/AND semantics (permissive vs restrictive) rather than a single predicate.
- **Exception carving**: support partial revokes (or an equivalent normal form) to express “allow broadly, deny narrowly”.
- **Governance entities**: consider non-privilege controls (settings constraints, quotas) as first-class assignable objects.
- **Inspectable metadata**: expose RBAC state via system tables, not only via DDL.
- **Pluggable persistence**: separate the logical RBAC model from how it is stored and replicated.
