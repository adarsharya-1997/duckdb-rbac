# RBAC Test Plan (portable, inspired by ClickHouse)

This document is a **test plan you can reuse in another database** (e.g. DuckDB) when implementing RBAC.

It is intentionally **system-agnostic**:
- It describes *what to test* and *how to structure tests*.
- It avoids relying on ClickHouse-specific SQL, system tables, or configuration formats.

## 1. What this testing approach excels at

ClickHouse’s RBAC coverage is strong because it tests RBAC at multiple layers, with different strengths:

1) **Core “rights algebra” unit tests**
- Validate the internal permission model (wildcards, object hierarchy, column-scoped rights, grant option, partial revoke, implicit rights).
- Fast feedback and excellent for edge-cases.

2) **SQL surface / golden-output tests**
- Treat RBAC as a public API: parse/format DCL, `SHOW`/introspection output, and stable error behavior.
- Makes regressions obvious via small diffs.

3) **End-to-end integration tests**
- Run the engine as a real server and validate effective behavior: query planning, privilege checks, policy filter injection, persistence across restart, multi-node replication.
- Captures interactions that unit tests miss (planner, optimizer, distributed execution, config reload).

If you copy this approach to another system, you get:
- Correctness of the access model
- Stability of the SQL/DCL interface
- Confidence that *real queries* are blocked/allowed correctly
- Defense against information leaks through errors or metadata

## 2. Test suite structure (recommended)

Organize tests into three suites that can run independently:

### A) Model-level unit tests (no server)
Target: the internal RBAC data structures and algorithms.
- Runs in milliseconds
- No SQL required
- Deterministic

### B) SQL-level tests (single node)
Target: DCL parsing and user-visible outputs.
- Use a harness that runs SQL and compares results against reference output
- Prefer “golden file” comparisons for `SHOW …` output and introspection

### C) Integration tests (server, optionally multi-node)
Target: runtime enforcement.
- Start a database instance (or cluster) with a known config
- Execute queries as different users/roles
- Assert results and error messages

## 3. Core RBAC domain model (portable terminology)

Use these neutral terms to write tests:

- **Principal**: a user, service account, or session identity.
- **Role**: a named set of privileges; can be granted to principals or other roles.
- **Privilege**: an action on an object (e.g. `SELECT` on a table, `CREATE` in a schema).
- **Grant option**: ability to grant a privilege to others.
- **Admin option**: ability to grant/revoke roles.
- **Object scope**: global / schema / table / column / function / resource.
- **Row policy (RLS)**: an additional predicate applied to reads (and possibly writes).
- **Introspection**: system views/functions exposing grants, roles, effective roles, policies.

## 4. Test matrix dimensions (use to ensure coverage)

When planning coverage, vary these dimensions systematically:

### 4.1 Object hierarchy and wildcards
- Global scope: `*`
- Schema scope: `schema.*`
- Table scope: `schema.table`
- Column scope: `schema.table(column)`
- Mixed: `schema.*` + specific-table overrides

### 4.2 Privilege types
At minimum:
- DML: `SELECT`, `INSERT`, `UPDATE`, `DELETE`
- DDL: `CREATE`, `ALTER`, `DROP`
- Metadata: `SHOW`, `DESCRIBE`, `EXPLAIN`
- “Resource” privileges (if present): file/network/URL/S3/etc.

### 4.3 Role mechanics
- Direct grants to users
- Role grants to users
- Role grants to roles (nesting)
- Default roles vs roles enabled per session
- `WITH ADMIN OPTION` semantics

### 4.4 Revocation semantics
- Full revoke vs partial revoke
- Column revoke under table-level grant
- Revoke of grant option
- REVOKE intersection semantics (only remove what exists)

### 4.5 RLS / row policy mechanics
- Multiple permissive policies (combine via OR)
- Multiple restrictive policies (combine via AND)
- Permissive + restrictive (OR group ANDed with restrictive group)
- Schema-level policy fallback (policy on `schema.*` applies when table lacks one)
- Policy referencing columns, including computed/alias columns
- Failure modes: missing columns, invalid expression

### 4.6 Introspection and information disclosure
- What metadata is visible without privileges?
- Are error messages “too helpful” (leaking hidden schema/table names)?
- Do introspection views require explicit grants?

### 4.7 Persistence and distribution
- Persistence across restart
- Replication of RBAC entities in a cluster
- Consistency after config reload

## 5. Concrete test cases to copy (templates)

Each subsection below describes:
- **Goal**: what invariant to validate
- **Setup**: schema/data/users/roles
- **Assertions**: what must hold
- **Variants**: how to expand coverage

### 5.1 Column-precise privilege enforcement
**Goal**: ensure the engine requires privileges for every column actually read/used.

**Setup**
- Table `t(a, b)` with data
- Principal `u` with `SELECT(a)` only

**Assertions**
- `SELECT a FROM t` succeeds
- `SELECT b FROM t` fails
- `SELECT a FROM t WHERE b = 1` fails (predicate uses `b`)
- `SELECT a FROM t ORDER BY b` fails
- `SELECT count() FROM t` behavior is defined and tested (some engines allow it with any-column select; others require table-level select)

**Variants**
- Joins: needs privileges on join keys from both tables
- `SELECT *` requires all columns
- Computed columns / aliases: verify whether underlying columns require privilege
- Views: privilege is checked on base tables (or view definer, depending on model)

### 5.2 Wildcards and hierarchical matching
**Goal**: wildcard grants and schema/table matching behave predictably.

**Setup**
- Schemas `s1`, `s2`; tables `s1.t1`, `s1.t2`, `s2.t1`
- User `u`

**Assertions**
- Grant `SELECT ON s1.*` allows reading `s1.t1`, `s1.t2` but not `s2.t1`
- Grant `SELECT ON *.*` allows all
- Ensure wildcard grants are listed canonically in introspection / `SHOW GRANTS`

**Variants**
- Mixed: global grant + schema-level revoke
- Pattern-based objects (if supported)

### 5.3 Partial revoke semantics
**Goal**: allow “broad allow” + “narrow deny” patterns.

**Setup**
- Grant `SELECT` on `*.*`
- Revoke `SELECT` on `s1.*` (or table / column)

**Assertions**
- Access to `s1` is denied, others allowed
- Introspection distinguishes partial revokes from positive grants

**Variants**
- Column partial revokes under table grants
- Multiple revokes with overlaps

### 5.4 Grant option semantics
**Goal**: user can delegate privileges only with grant option.

**Setup**
- User `u1` has `SELECT ON s1.t1` with grant option
- User `u2` exists

**Assertions**
- `u1` can grant `SELECT` on `s1.t1` to `u2`
- Without grant option, the same operation fails

**Variants**
- Revoke grant option only (privilege remains)
- Grant option on wildcards

### 5.5 Role lifecycle and resolution
**Goal**: roles behave like composable privilege sets.

**Setup**
- Role `r_read` has `SELECT` on `s1.*`
- User `u` granted role

**Assertions**
- User can read `s1.*`
- Revoking role removes effective privilege
- Introspection shows direct role grants

**Variants**
- Role nesting: `r_admin` includes `r_read`
- Default roles applied only for new sessions (if applicable)
- Per-session role enabling/disabling changes effective privileges

### 5.6 Admin option for role grants
**Goal**: only principals with admin option can grant/revoke roles.

**Setup**
- Role `r1`
- User `u_admin` granted `r1` with admin option
- User `u_plain` granted `r1` without admin option

**Assertions**
- `u_admin` can grant `r1` to others
- `u_plain` cannot

**Variants**
- Revocation requires admin option too
- Delegation chains

### 5.7 RLS: permissive/restrictive combination
**Goal**: policy combination rules are correct.

**Setup**
- Table `t(x, y)` with rows 1..N
- Policy P1 permissive: `x = 1` applies to user `u`
- Policy P2 permissive: `x = 2` applies to user `u`
- Policy R1 restrictive: `y >= 10` applies to user `u`

**Assertions**
- Effective filter is `(x=1 OR x=2) AND (y>=10)`
- Adding/removing policies updates results predictably

**Variants**
- Schema-level policy + table-level policy (fallback/union)
- Policy on derived tables / views / table functions

### 5.8 RLS: planner/optimizer interactions
**Goal**: policies are enforced even when the optimizer rewrites queries.

**Setup**
- Table with indexes / partitions / projections (or analogous optimizer features)
- Row policy on that table

**Assertions**
- Queries using `WHERE`, `PREWHERE`, `FINAL`, `LIMIT`, joins still enforce policy
- `EXPLAIN` (if available) indicates policy application (or at least results prove it)

### 5.9 RLS: hardening against query tricks
**Goal**: user cannot bypass policy via aliasing / shadowing / scoping tricks.

**Setup**
- Policy references column `x`

**Assertions**
- Query forms that attempt to redefine `x` (aliases, WITH bindings, subqueries) cannot bypass filter

### 5.10 RLS failure modes
**Goal**: invalid policies don’t crash the system or silently leak.

**Assertions**
- Policy referencing missing column: query fails with a clear error (or policy is rejected at creation time)
- Policy expression invalid: creation fails or policy is skipped with logging (define desired behavior and test it)
- Dropping policy restores access

### 5.11 Introspection: correctness and access control
**Goal**: `SHOW GRANTS` / system views are correct and properly protected.

**Assertions**
- Introspection output is canonical/stable
- A user sees only what they’re allowed to see
- “effective roles” and “current roles” views match session state

**Variants**
- Parameterized/resource grants should be visible in a structured way

### 5.12 Information disclosure via errors
**Goal**: error messages do not leak existence of hidden objects.

**Setup**
- User has access to schema `s_visible` but not `s_hidden`

**Assertions**
- Typo suggestions / “did you mean …” do not mention objects the user cannot see
- Metadata queries (information_schema equivalents) respect visibility

### 5.13 Persistence across restart
**Goal**: RBAC entities and grants persist and round-trip.

**Assertions**
- After restart, roles/users/policies/grants are unchanged
- `SHOW CREATE …` or equivalent round-trips deterministically

### 5.14 Multi-storage / multi-source authority (optional)
If your system supports multiple sources of RBAC truth (e.g., config + catalog + external auth):

**Goal**: conflict resolution and precedence are deterministic.

**Assertions**
- Same role name defined in two sources resolves predictably
- Grants referencing an entity remain consistent after one source is removed

### 5.15 Replication / multi-node consistency (optional)
If your system supports distributed catalogs:

**Goal**: RBAC operations replicate and converge.

**Assertions**
- Creating users/roles/policies on node A appears on node B
- Grant/revoke replicates
- Failure handling: if coordination layer is down, system becomes read-only or fails safely

## 6. Test harness patterns worth copying

### 6.1 Golden output tests for DCL/introspection
- Store expected outputs for:
  - `SHOW GRANTS`
  - `SHOW CREATE ROLE/USER/POLICY`
  - `system_grants` / `system_role_grants` / policy listings
- This catches formatting, ordering, and canonicalization regressions.

### 6.2 Integration tests should assert both:
- **Correct result set** (data visibility)
- **Correct error message class** (permission denied vs missing object)

### 6.3 Config toggles as separate suites
If RBAC behavior depends on feature flags (e.g., “select from metadata requires grant”, “users without policy can read rows”), create a separate integration suite that runs with those toggles flipped.

## 7. Suggested “minimum viable RBAC test pack” for a new system

If you need to bootstrap quickly, start with:
1) Unit: wildcard + partial revoke + grant option algebra
2) Integration: column-level enforcement (WHERE/JOIN/ORDER BY)
3) Integration: role enabling + default roles
4) Integration: RLS permissive/restrictive combination + bypass-hardening
5) Golden: `SHOW GRANTS` canonical output
6) Security: metadata visibility + error-message non-leak

## 8. Known weak spots to explicitly add (common omissions)

ClickHouse’s tests are strong overall, but the following areas are easy to miss in any system—consider adding them early:
- Cyclic role grants (ensure termination or explicit rejection)
- Mid-session invalidation after GRANT/REVOKE (sessions should not keep stale rights)
- Invalid policy expressions loaded from storage (should fail safely)
- Recovery paths for persisted RBAC indexes (if you maintain name→id indexes)
