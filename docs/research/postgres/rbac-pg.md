# PostgreSQL RBAC Design (High-Level Report)

This report summarizes PostgreSQL’s role-based access control (RBAC) model from a *design and architecture* perspective, focusing on the internal representations and algorithms that implement `GRANT`/`REVOKE`, role membership, and permission checks. It intentionally avoids repository file paths so it can serve as input when designing RBAC from scratch (e.g., in DuckDB).

## 1. Conceptual Model

PostgreSQL’s access control is centered on two primitives:

1) **Roles**
- A role is a global identity in a database *cluster* (not per-database).
- A role can be used as a login identity (“user”) and/or as a grouping identity (“group role”).
- Roles can hold attributes (e.g., superuser, can create DB, can create roles).

2) **Privileges on objects**
- Most permissions are expressed as *privileges on database objects* (tables, schemas, functions, etc.).
- Privileges are granted to roles, optionally with a *grant option*.

A key design choice: PostgreSQL treats “users” and “groups” uniformly as roles. Grouping is implemented via **role membership**.

## 2. Two Kinds of Grants

PostgreSQL has two logically separate grant systems:

### 2.1 Object privilege grants
These are the classic `GRANT <priv> ON <object> TO <role>` and `REVOKE ...` operations.
- Data is stored with the object (in the object’s catalog row) as an ACL.
- Revocation can cascade along chains of grant options.

### 2.2 Role membership grants
These are `GRANT <role> TO <role>` / `REVOKE <role> FROM <role>`.
- Data is stored in a dedicated membership catalog.
- Membership grants carry options that affect whether privileges are usable implicitly and whether the member may grant membership further.

This split is important: *object privileges* live in per-object ACLs; *membership* lives in its own structure.

## 3. Storage Model (What Gets Persisted)

### 3.1 Roles
A role record stores:
- Role name and OID
- Role attributes (superuser, createdb, createrole, replication, bypassrls, canlogin, etc.)
- Authentication fields (password hash, expiration)

### 3.2 Role membership edges
Membership is stored as rows describing an edge:

- `roleid`: the granted role (the “group”)
- `member`: the member role (the “user” or another group)
- `grantor`: who granted this membership
- Options:
  - `admin_option`: member can grant/revoke this membership to others
  - `inherit_option`: member can *implicitly* use privileges of the granted role
  - `set_option`: member can `SET ROLE` to become the granted role

PostgreSQL prohibits membership loops and also tries to prevent cycles in the “admin option grant graph”, so that cascading revokes remain well-defined.

### 3.3 Object ACLs (the main permission state)
Privileges for objects are stored as an array-like structure of entries:

- Each entry is `(grantee, grantor, rights)`
- `rights` contains two bitsets:
  - actual privilege bits
  - grant-option bits (the right to grant that privilege onward)

Special identity:
- `PUBLIC` is represented as an implicit grantee meaning “all roles”.

Null vs empty:
- A **NULL ACL** in catalogs means “use the hardwired default privileges”, not “no privileges”.
- An **empty ACL** means “no privileges”.

### 3.4 Shared dependency tracking
PostgreSQL additionally records dependencies between objects and roles, to prevent dropping roles that are still referenced:
- “role is owner of object”
- “role is mentioned in object ACL”
- “role is mentioned in initial privileges record”

This is a cross-database (cluster-wide) mechanism because roles are cluster-wide.

## 4. Internal Data Structures

### 4.1 Privilege bitmasks
Privileges are represented as a bitmask type (64-bit in Postgres), but with a constraint that the stored ACL format effectively supports a fixed number of distinct rights (historically 32).

Design point:
- Lower bits: actual privileges (e.g., SELECT, INSERT, UPDATE, USAGE, EXECUTE, …)
- Upper bits: grant options for those privileges

### 4.2 ACL entry
An ACL entry (“AclItem”) is structurally:

- `grantee`: role identifier receiving privileges
- `grantor`: role identifier credited as the source of this grant
- `rights`: combined privilege + grant-option bits

ACLs are stored as a single-dimensional array of these entries.

### 4.3 Object addressing
PostgreSQL uses a generic “object address” triple:

- `classId` (which catalog/table defines the object type)
- `objectId` (OID)
- `objectSubId` (for sub-objects like columns)

This simplifies writing generic dependency and ownership code across many object kinds.

## 5. Execution Architecture (How GRANT/REVOKE Works)

### 5.1 Object GRANT/REVOKE pipeline
At a high level:

1) Parse `GRANT`/`REVOKE` into a `GrantStmt`.
2) Resolve target objects to stable internal IDs.
3) Resolve grantee role specifications into role IDs (including `PUBLIC`).
4) Convert textual privileges into an internal bitmask.
5) Dispatch by object type to a per-object-kind execution routine.
6) For each target object:
   - Load current ACL (or synthesize default ACL if NULL)
   - Determine *effective grantor identity*
   - Restrict requested privileges to what the grantor may legally grant
   - Produce a new ACL by merging in GRANT or applying REVOKE
   - Write the new ACL back to the catalog
   - Update shared dependency rows for roles referenced in the ACL

### 5.2 Selecting the “effective grantor”
A subtle design point: privileges should appear to “flow” from owners and explicit grant-option holders.

When executing a GRANT/REVOKE, Postgres may change the recorded grantor identity:
- If a user is a member of a role that holds the relevant grant options, the system can attribute the grant to that role.
- If multiple candidate roles exist, Postgres prefers a role that provides the largest subset of required grant options, with tie-breaking that tends to prefer “closer” membership ancestry.

Why this matters:
- Makes the grant graph coherent.
- Supports correct cascading revoke semantics.

### 5.3 Cascading revoke (GRANT OPTION chains)
PostgreSQL models grant-option chains explicitly through `(grantor, grantee)` pairs in ACL entries.

On `REVOKE GRANT OPTION` or revoking rights that removes grant options:
- The system recursively finds downstream grants that depended on the removed grant option.
- With `CASCADE`, dependent grants are removed.
- With `RESTRICT`, revocation can error if it would orphan dependent grants.

This is why ordering and cycle-prevention are important.

### 5.4 Column privileges
Column privileges are stored separately from table-level privileges.
- Granting column privileges updates a per-column ACL.
- Permission checks for many operations must consider both table-level and column-level grants.

## 6. Role Membership Semantics

Role membership provides privileges via two mechanisms:

1) **Inheritance (`INHERIT`)**
- If membership has inherit enabled, privileges of the granted role are automatically usable.
- Inheritance can stop at membership edges that have inherit disabled.

2) **Role switching (`SET ROLE`)**
- If membership has set enabled, a session can change its “current user identifier” to the granted role.
- After switching, privilege checks behave as if the target role were the login role for most SQL commands.

Membership grants also control who may manage membership:
- `ADMIN` option is required to grant/revoke membership in a role (except for special cases like superuser).

## 7. Permission Check APIs (C-level viewpoint)

PostgreSQL exposes a consistent set of internal APIs used throughout the backend to enforce RBAC.

### 7.1 ACL evaluation
There are generic entry points that:
- Load the object’s ACL from its catalog row (or synthesize defaults)
- Compute whether a given role holds required privileges

These APIs return either:
- a boolean (“ok / not ok”) style result, or
- a bitmask of which privileges are satisfied (useful for “any of these privileges”).

### 7.2 Role relationship queries
There are dedicated helpers for:
- “does role A have privileges of role B?” (privilege inheritance semantics)
- “can role A set role to B?” (SET ROLE semantics)
- “is role A admin of role B?” (membership administration)

These functions compute transitive closure over membership edges using different recursion rules depending on the question.

### 7.3 Grant execution
The GRANT/REVOKE executor has:
- a statement-level API for object grants
- a separate API for membership grants

Internally, both systems share concepts like:
- selecting an appropriate grantor identity
- maintaining dependencies so that referenced roles cannot be dropped

## 8. Default Privileges

PostgreSQL distinguishes:

- **Hardwired defaults** for each object type (used when the ACL field is NULL)
- **User-configured default ACLs** (ALTER DEFAULT PRIVILEGES), which affect newly created objects

Design detail:
- Default privileges are consulted *only at object creation*.
- A NULL ACL in a catalog means “use the hardwired defaults”, not “look up current default ACL settings”.

This avoids time-dependent privilege interpretation for existing objects.

## 9. Operational / Concurrency Notes

PostgreSQL’s GRANT implementation makes pragmatic concurrency tradeoffs:
- Target objects are typically resolved under a non-exclusive lock (enough to prevent disappearance but not enough to serialize with all concurrent DDL).
- That means GRANT can fail with “concurrent update” rather than block in some cases.

This choice limits lock table bloat and avoids disrupting unrelated background activity.

## 10. Design Takeaways for Implementing RBAC From Scratch (DuckDB-oriented)

If you’re designing RBAC from scratch, PostgreSQL highlights a few key constraints and choices:

1) **Separate “object privileges” vs “role membership”**
- Membership needs different options (inherit/set/admin) and different cycle/cascade behavior than object ACLs.

2) **Represent grants as edges with a recorded grantor**
- Storing `(grantor, grantee)` enables correct `REVOKE ... CASCADE` over grant-option chains.

3) **Default privileges must be time-invariant for existing objects**
- Avoid reinterpreting historical objects based on current defaults.

4) **Dependency tracking is essential**
- If roles are global, you need a cluster-wide way to prevent dropping roles that still appear in any ACLs or ownership.

5) **Grantor selection matters for usability**
- If a user inherits grant options from some role, the system can attribute GRANTs to that role for consistency.

6) **Cycle prevention is not just about membership loops**
- You also want to prevent cycles in “grant option” graphs (for privileges and for admin-option membership) to ensure cascading revokes terminate and produce predictable results.

7) **Consider explicit semantics for `SET ROLE` vs implicit inheritance**
- They solve different problems (session “becoming” another role vs accumulating privileges).
