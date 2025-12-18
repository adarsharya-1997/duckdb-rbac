# ClickHouse RBAC research notes (architecture + key mechanisms)

This document summarizes how ClickHouse implements role-based access control (RBAC), including column-level privileges and row-level security (row policies). It is written for reuse in other systems, so it avoids ClickHouse-specific file paths and instead focuses on concepts, data structures, and execution flow.

---

## 1) Core primitives: “access entities”

ClickHouse models authorization state as a set of typed **entities** with stable IDs (UUIDs):

- **User**: authentication + account settings + grants + role memberships.
- **Role**: named bundle of grants + role memberships (roles can grant other roles).
- **RowPolicy**: row-level filters scoped to database/table, assigned to users/roles.
- **SettingsProfile**, **Quota**, etc. (not RBAC per se, but evaluated in the same “effective session state” pipeline).

A central “AccessControl” manager provides:

- entity lookup by id/name,
- CRUD operations,
- subscription/invalidation for changes,
- computation of session-effective state (enabled roles, enabled row policies, effective rights).

Key idea: **authorization is not computed from raw tables each time**; instead, ClickHouse builds **cached “effective snapshots”** per session parameters and invalidates them on changes.

---

## 2) Storage model: multiple backends, same semantics

Access metadata can be sourced from multiple storages layered together (e.g., config + disk + replicated). The important design trait is that **the evaluation model is independent of the storage backend**.

### 2.1 Disk-backed storage

- Each entity is persisted as a single text file named by UUID, containing SQL-like ATTACH statements.
- Writes are atomic (write to `*.tmp` then rename).
- Separate index files maintain name→UUID mapping per entity type (to speed lookups and enforce uniqueness). If these index files are corrupted/missing, the storage can rebuild them by scanning entity files.

### 2.2 Replicated storage (ZooKeeper/Keeper)

- Entity definitions are stored as strings (the same “ATTACH statements” format).
- ZooKeeper layout uses two znodes per entity:
  - one znode keyed by UUID containing the full definition,
  - one znode keyed by (type, escaped name) containing the UUID.
- Updates use optimistic concurrency via ZooKeeper stat versions (CAS), and multi-ops to keep UUID+name mapping consistent.

### 2.3 Config-backed storage

- A read-only source for users/roles defined in configuration.
- Implemented as “parse config → materialize entities in an in-memory storage,” but **no writes** are allowed.

---

## 3) Grants data model: structured “grant elements”

Privileges are not just strings; they are represented as **structured grant elements** with explicit scope and options.

Conceptually (simplified):

```text
GrantElement {
  action_flags: set<Action>,
  scope: Global | DB(db) | Table(db, table) | Column(db, table, columns...),
  parameter: optional string (for parameterized global privileges),
  wildcard: bool (prefix semantics),
  with_grant_option: bool,
  is_partial_revoke: bool
}
```

### 3.1 Column-level privileges

Column-level privileges are first-class: the element can carry an explicit column set.

Semantics relevant in ClickHouse:

- A user may have `SELECT` on specific columns only.
- “Trivial” queries like `SELECT count() FROM t` do not name columns; ClickHouse treats this as allowed if the user has SELECT on **at least one column** (so it doesn’t accidentally deny because the planner chose a different minimal column).

### 3.2 Wildcards are prefix-based, not SQL LIKE

Wildcards are implemented as prefix matches on identifiers (e.g. `db*`), and the underlying data structure explicitly supports “prefix nodes” distinct from exact nodes.

### 3.3 Partial revokes

ClickHouse supports *partial revokes* (revoking a subset out of a broader grant). These are represented explicitly via `is_partial_revoke` on grant elements and are preserved in introspection output.

A key rule enforced in the implementation:

- “partial revoke” entries are **produced by REVOKE** and must not be “granted”.

---

## 4) AccessRights internal structure: radix tree with cached min/max

Effective privileges for an entity (user/role) are stored in an `AccessRights` structure.

The crucial implementation detail: it is a **RADIX tree** over names / name components, built to efficiently represent:

- global grants,
- db-level, table-level, and column-level grants,
- prefix wildcards,
- and partial revokes.

Each node caches:

- `flags`: effective flags at that node, computed roughly as:
  - **(inherited flags − partial revokes) ∪ explicit grants**
- `min_flags_with_children`: intersection-like cache used to answer “is this granted for everything under here?”
- `max_flags_with_children`: union-like cache used to early-reject checks.

Why this matters:

- permission checks become fast (often O(depth) with pruning),
- wildcard semantics are handled precisely (including interactions like “grant prefix, revoke exact”).

### 4.1 How wildcard vs exact is distinguished

A notable subtlety: wildcard grants/revokes are applied to the **parent of an exact-leaf node**, not to the leaf itself. That makes queries like the following behave correctly:

```text
GRANT SELECT ON foo*
REVOKE SELECT ON foo

isGranted(SELECT, "foo")          -> false
isGrantedWildcard(SELECT, "foo")  -> true
isGrantedWildcard(SELECT, "foobar")-> true
```

This is not a superficial formatting choice; it’s a structural representation to make prefix rules consistent.

### 4.2 Turning the tree back into “elements” (for SHOW GRANTS / persistence)

When the system needs a human-readable representation (or when persisting to the ATTACH format), it walks the tree and rebuilds `GrantElement`-like records.

Two noteworthy behaviors:

1) **Ordering**: normal grants are sorted before partial revokes; “with grant option” entries are ordered after corresponding non-grant-option entries.

2) **Column compaction**: when many entries differ only by column, they are merged into a single element with a column list (e.g. `SELECT(x, y)` instead of `SELECT(x)` + `SELECT(y)`). Partial-revoke status is part of the grouping boundary (so a revoke doesn’t get merged with grants).

---

## 5) Roles: granted vs enabled, plus admin option

ClickHouse distinguishes:

- **granted roles** (membership edges; can be role→user or role→role), from
- **enabled roles** (the set active in the current session, based on defaults + `SET ROLE`).

This is important because the session can choose to enable only a subset of roles.

### 5.1 Enabled roles are computed as a closure with caching

Enabled roles are computed as a transitive closure over the role graph. The result is a snapshot structure that includes:

- current roles,
- enabled roles (including nested grants),
- enabled roles with **admin option**,
- role-id → role-name map,
- aggregated access rights from those roles,
- aggregated settings contributed by roles.

This snapshot is cached by (current_roles, current_roles_with_admin_option).

### 5.2 ADMIN OPTION vs GRANT OPTION

ClickHouse separates:

- **GRANT OPTION**: permission to grant/revoke privileges.
- **ADMIN OPTION**: permission to grant/revoke roles.

The check paths and error messages treat these distinctly. There’s also a “ROLE_ADMIN” privilege that can short-circuit admin option checks.

---

## 6) Row-level security: row policies and filter mixing

Row policies are stored separately from privileges. The effective row filter depends on (user_id, enabled_roles).

### 6.1 Two kinds of policies: permissive and restrictive

Policies are tagged as either:

- **permissive**: combined by OR,
- **restrictive**: combined by AND.

Then permissive result is added into the restrictive set (so the final is basically an AND of restrictions, where one of the restrictions may be “OR(permissive)”).

There is an important behavior knob:

- if a user has **no permissive policies**, the system can treat that as either “allow all” or “deny all” depending on configuration.

(ClickHouse’s implementation explicitly threads this behavior toggle into the filter mixer.)

### 6.2 Database-level and table-level policies

Policies can be defined at:

- database scope (db.*),
- table scope (db.table).

Database-level policies are treated as a base layer and are inherited by table-level policy evaluation.

### 6.3 Filter parsing and caching

- Each policy stores filter strings per operation type (e.g. SELECT filter vs others).
- Filters are parsed to AST once and cached.
- If the same string appears for multiple filter types, the parsed AST pointer is reused.
- If parsing fails, the error is logged and the filter is effectively skipped.

### 6.4 Applying row policies to queries

Row policy filtering is integrated in query analysis/planning by generating filter actions (expression DAG) from the policy AST.

A noteworthy safety measure:

- the policy expression is analyzed in a *separate mini query context* (conceptually: `SELECT <policy_expr>, <needed_cols> FROM db.table`) to avoid alias injection and to compute required columns precisely.

The resulting filter expression/actions become part of the query plan (like an extra WHERE filter).

---

## 7) The “effective session access” object

ClickHouse’s privilege checks go through a session-bound object (often thought of as `ContextAccess`):

- It holds the resolved user, enabled roles snapshot, effective access rights, enabled row policies, quotas, and settings.
- It is cached per “session parameters” (user id, current roles, default role mode, address, read-only mode, etc.).
- It subscribes to changes in underlying entities; when something changes it recomputes its derived state.

### 7.1 Computation pipeline

At a high level:

1) resolve user;
2) compute enabled roles (closure + caching);
3) compute effective rights as `user.access ∪ roles_info.access`;
4) compute “implicit rights” (see below) as an additional layer;
5) compute enabled row policies for (user, enabled roles);
6) compute settings/quota (not RBAC but same pattern);
7) serve `checkAccess` / `isGranted` APIs to interpreters.

### 7.2 Implicit rights layer

ClickHouse adds a layer of “implicit grants” derived from existing privileges, for usability and consistency.

Examples of implicit expansions:

- certain DDL privileges imply corresponding view privileges,
- column/table/dictionary privileges imply relevant SHOW_* privileges,
- special-casing for system / information_schema visibility depending on configuration.

This is not just convenience: it affects system tables exposure and “introspection” semantics.

---

## 8) GRANT/REVOKE execution flow (mutation semantics)

GRANT/REVOKE statements are interpreted as updates to user/role entities:

- The target entity is cloned.
- Its `access` set is updated:
  - revoke applies first,
  - grant applies next.
- Its `granted_roles` set is updated:
  - role revokes/grants are applied,
  - admin option is tracked separately.
- The updated entity is written back through the selected storage backend.

Notable correctness details in the flow:

- Empty database names are replaced with the current database *before* replication/execution, explicitly to prevent privilege escalation.
- For cluster execution of GRANT/REVOKE, required-access checks are intentionally less precise (because the coordinator cannot know the exact state on every shard).

---

## 9) Introspection: RBAC state is queryable

ClickHouse exposes RBAC state through system tables (users, roles, grants, role_grants, row_policies, enabled_roles, current_roles, etc.).

Important property: these tables are not just logs; they are projections of the in-memory entity state.

For example:

- grants are materialized by iterating the normalized grant elements and emitting one row per (action, scope, column, options), including `is_partial_revoke` and `grant_option`.
- role grants are materialized by iterating membership edges, including default-role and admin-option flags.

---

## 10) Behavioral/compatibility toggles

ClickHouse includes configuration flags that alter RBAC semantics for compatibility and staged rollout. Examples:

- whether users without row policies can read rows by default,
- whether selecting from system / information_schema requires explicit grants,
- whether table engines require explicit grants,
- compatibility for certain access types and parameterized privileges.

These toggles are read during AccessControl initialization and then consulted during checks and formatting.

---

## 11) Practical takeaways (design constraints implied by implementation)

From the mechanisms above, ClickHouse RBAC implies these constraints:

- Privilege evaluation must support: exact scope + column scope + prefix wildcards + partial revokes.
- Role closure must be precomputed and cached; role changes must invalidate dependent sessions.
- Row policy evaluation must mix permissive/restrictive policies and support db-level inheritance.
- Persistence must be able to round-trip the *semantics* of grants (including partial revokes) and role memberships.
- Introspection is an explicit, supported API surface: state is queryable and must match effective behavior.
