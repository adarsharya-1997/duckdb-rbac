# DuckDB RBAC Extension: Requirements Q&A

This document captures the requirements discussion for implementing Role-Based Access Control (RBAC) as a DuckDB extension. The design draws from ClickHouse and PostgreSQL RBAC implementations.

---

## 1. Core Architecture

### Q1: Is authentication handled externally, or does the extension manage user credentials?

**A1:** Authentication is handled externally. DuckDB receives a user principal from the embedding application.

### Q2: What DuckDB extension capabilities do we need (query interception, plan modification, etc.)?

**A2:** Deferred. We'll explore DuckDB's extension API after finalizing requirements.

### Q3: How does DuckDB handle session identity currently?

**A3:** Deferred. We'll explore DuckDB's mechanisms later.

---

## 2. Object Scope & Granularity

### Q4: What object granularity do we need—Global, Database, Schema, Table, Column?

**A4:** Table and Column levels only for MVP. We don't use schema/database levels in the embedded app.

### Q5: What privilege types are essential—SELECT, INSERT, UPDATE, DELETE, DDL, GRANT?

**A5:** SELECT only for MVP. Users in our system can only read; the application handles writes.

---

## 3. Role Model

### Q6: Do we need role hierarchy (roles containing other roles), or is flat user→roles sufficient?

**A6:** Flat for MVP. Role hierarchy can be added later.

### Q7: Do we need PostgreSQL-style INHERIT vs SET ROLE distinction?

**A7:** Keep it simple. All granted roles are always active (like ClickHouse's simpler model). No SET ROLE for MVP.

### Q8: Is ADMIN OPTION (grant roles) vs GRANT OPTION (grant privileges) distinction needed?

**A8:** Keep it simple for MVP. All grants are admin-only.

---

## 4. Row-Level Security (RLS)

### Q9: Is row-level security in scope for MVP?

**A9:** YES. RLS and CLS (column-level security) are major parts of the MVP.

### Q10: Are partial revokes (grant broadly, revoke narrowly) needed?

**A10:** Start simple with grants only. Partial revokes deferred to v2.

### Q11: Are wildcard/prefix grants (e.g., `db.prefix_*`) needed?

**A11:** No. Exact object names only for MVP.

---

## 5. Persistence & Storage

### Q12: How should RBAC state be persisted?

**A12:** Our use case is in-memory (embedded DuckDB), but for open-source appeal, store in DuckDB tables. Users can load grants from a SQL file at startup.

### Q13: Does this need to work in multi-node/replicated scenarios?

**A13:** No. DuckDB is embedded, everything is single-node.

---

## 6. Introspection & Error Handling

### Q14: Is queryable introspection (system tables, SHOW GRANTS) required?

**A14:** Yes, very much needed.

### Q15: What should happen when a permission check fails?

**A15:** Hard error. Query aborts with a clear message.

### Q16: Should superuser/admin bypass be built-in or handled externally?

**A16:** External. The embedding application marks certain principals as superuser; they bypass all RBAC checks.

---

## 7. Row Policy Syntax

### Q17: How should row policies be defined—inline on GRANT or as named objects?

**A17:** Named policy objects, mimicking ClickHouse syntax:
```sql
CREATE ROW POLICY policy_name ON table_name
    FOR SELECT
    USING (filter_expression)
    TO role_name;
```

### Q18: Do we need multiple policies per table with combination semantics?

**A18:** Yes. Multiple policies combined with OR (permissive) for MVP. AND (restrictive) deferred to v2.

### Q19: What happens when a user has no matching policy for a table?

**A19:** Discussed later—see Q46/Q48.

### Q20: What can appear in policy USING expressions—columns, functions, subqueries?

**A20:** Discussed later—see Q28.

---

## 8. Column-Level Security (CLS)

### Q21: What does CLS mean—column privileges, column masking, or both?

**A21:** Column privileges for MVP. Query fails if user explicitly references forbidden columns. `SELECT *` silently returns only accessible columns.

### Q22: If column masking, how are masks defined?

**A22:** Column masking deferred to v2.

---

## 9. Superuser & Admin Model

### Q23: How does the proposed external superuser model differ from ClickHouse/PostgreSQL?

**A23:** PostgreSQL and ClickHouse store superuser as an attribute on the user entity. Our model has superuser as an external flag on the principal passed in. This is simpler for embedded systems—no need to sync external auth with internal user records. Confirmed as the right approach.

### Q24: Should we use named policy objects or inline GRANT WHERE syntax?

**A24:** Named policy objects, using ClickHouse-style syntax.

### Q25: Is OR-only policy combination acceptable for MVP, deferring AND (restrictive)?

**A25:** Yes. OR combination for MVP, AND deferred.

---

## 10. SELECT Behavior

### Q26: What's the exact behavior for column access with SELECT *?

**A26:** Confirmed:
- `SELECT *` rewrites to allowed columns silently
- `SELECT a, b` where both allowed: works
- `SELECT a, b, c` where c forbidden: error
- No grant at all: "Permission denied"

### Q27: Should computed expressions on forbidden columns (e.g., `SELECT salary * 2`) fail?

**A27:** Yes. All references to forbidden columns must be denied.

---

## 11. Policy Expression Capabilities

### Q28: What can appear inside USING expressions in row policies?

**A28:** Any scalar expression DuckDB already supports, plus a `current_user()` function the extension provides. No subqueries for MVP. This matches ClickHouse/PostgreSQL capabilities.

### Q29: Is a `current_user()` function available, or does the extension provide it?

**A29:** The extension will provide it. Details explored during implementation.

### Q30: Should row policies apply to writes (INSERT/UPDATE/DELETE) or just SELECT?

**A30:** Just SELECT (reads) for MVP, matching ClickHouse's primary focus.

---

## 12. Grant Administration

### Q31: Should regular users be able to grant privileges (GRANT OPTION)?

**A31:** No. All grants are admin-only for MVP.

### Q32: Are DDL privileges (CREATE/DROP/ALTER) in scope?

**A32:** No. Just DML (SELECT only) for MVP.

---

## 13. External Superuser Details

### Q33: Is external superuser the right choice for embedded DuckDB?

**A33:** Yes. External superuser confirmed. App provides `{ user, roles, superuser }` to connection.

---

## 14. Entity Lifecycle

### Q34: Do roles need to be explicitly created, or are they implicit?

**A34:** Explicit. `CREATE ROLE` required before granting to it (matches ClickHouse/PostgreSQL).

### Q35: If granting on a non-existent table, should it error or succeed?

**A35:** Error. Table must exist at GRANT time.

---

## 15. Introspection Tables

### Q36: What system tables/views do we need?

**A36:** Tables:
- `duckdb_roles` - all roles
- `duckdb_role_members` - membership edges
- `duckdb_table_privileges` - table-level grants
- `duckdb_column_privileges` - column-level grants
- `duckdb_row_policies` - policy definitions

Views:
- `duckdb_effective_privileges` - computed: what current user can do
- `duckdb_my_roles` - computed: roles active for current user

### Q37: Do we need `SHOW GRANTS FOR role_name` syntax?

**A37:** Not for MVP. System tables are sufficient.

---

## 16. Grant Syntax Simplification

### Q38: Do we need INSERT/UPDATE/DELETE privileges given users only SELECT?

**A38:** No. Users only SELECT. The application handles writes via Arrow/Appender. RBAC only needs SELECT privileges.

### Q39: Is the simplified scope (SELECT-only, Table+Column) correct?

**A39:** Yes, looking better.

---

## 17. User-to-Role Binding

### Q40: How do we associate an incoming user with their roles?

**A40:** External mapping. TAP fetches permissions from Cypher, maps to DuckDB roles, passes to connection. Design should be agnostic to TAP specifics.

### Q41: Do we need to distinguish "users" from "roles"?

**A41:** No. Everything is just roles. Users are identified by their external principal; they have a set of roles.

### Q42: Can a user have multiple roles, and are there issues with that?

**A42:** Yes, multiple roles allowed. Effective permissions = union of all roles' permissions. No issues anticipated.

---

## 18. Default Behavior

### Q43: What happens when a user has no grants for a table?

**A43:** (See Q44)

### Q44: What's the complete column grant behavior?

**A44:** Confirmed:
- `GRANT SELECT ON table TO role` (no columns): all columns visible
- `GRANT SELECT (a, b) ON table TO role`: only a, b visible
- No grant at all: permission denied error

---

## 19. Row Policy TO Clause

### Q45: Does the TO clause exist in PostgreSQL and ClickHouse?

**A45:** Yes. Both have `TO role_name` clause. Semantics: the policy restricts what the named role can see.

### Q46: What happens when a user has no row policy for a table they can SELECT?

**A46:** After discussion: **No policy = see all rows**. Policies are opt-in restrictions. This is simpler than ClickHouse's default-deny model when policies exist.

---

## 20. TAP Integration (Deferred)

### Q47: How should Cypher permissions map to DuckDB roles?

**A47:** Deferred. Design should be agnostic to TAP. The embedding app handles mapping.

### Q48: Confirmed: no policy = see all rows?

**A48:** Yes. Policies are additive restrictions. No policy means no filter.

### Q49: Does the TAP integration pattern look right?

**A49:** Deferred. Focus on standalone RBAC extension design first.

---

## 21. TAP API Layer

### Q50: Does TAP API do user-specific table filtering?

**A50:** No. All authenticated users have the same table whitelist (all tables known to API).

### Q51: Does TAP API do column-level validation?

**A51:** Yes, currently via query rewriting (shoe-horned solution). Example: wrapping queries with CTEs that add WHERE clauses. This is what we want to move into DuckDB RBAC.

### Q52: Does TAP API do row-level filtering?

**A52:** No. It's too hard at that layer, which is why we want RLS in DuckDB.

### Q53: Could the RBAC extension be used standalone (CLI, other deployments)?

**A53:** Yes, definitely. Goal is to move authorization from TAP API to DuckDB. Plan to open-source if design is clean.

---

## 22. Switching to Default Deny

### Q54: How hard would it be to switch from "no policy = see all" to "no policy = see nothing"?

**A54:** Not hard. Same data model; just change one line of code (return null vs return FALSE). Could be a configuration option later.

---

## 23. TAP API Identity Passing

### Q55: How does TAP API pass user identity to TAP/DuckDB?

**A55:** Via Authorization Bearer token header. Currently assumes all authenticated users have full access, which is why we're building RBAC.

### Q56: Are "saved views" in TAP API actual DuckDB views or SQL strings?

**A56:** Just SQL files. Some use Jinja templating.

### Q57: Does TAP API need coordination with RBAC extension?

**A57:** No. RBAC should work standalone for other DuckDB users too.

---

## 24. Session Identity Mechanism

### Q58: How should user identity be set—connection property, SET statement, or extension function?

**A58:** Connection property (immutable after connection creation). This prevents users from changing their own identity via SQL.

### Q59: Can we pass roles now and add CREATE ROLE later?

**A59:** Yes. But decided to include CREATE ROLE in MVP since both ClickHouse and PostgreSQL have it.

---

## 25. Error Messages & Performance

### Q60: How detailed should error messages be?

**A60:** Detailed. ClickHouse and PostgreSQL both provide detailed errors (user, table, column, missing privilege). Helps debugging.

### Q61: Is there concern about query performance with many roles/policies OR'd together?

**A61:** Not for MVP. Neither ClickHouse nor PostgreSQL have explicit limits. In practice, users have 1-5 roles.

### Q62: Should `SELECT *` rewrite be silent or produce a warning?

**A62:** Silent rewrite (like ClickHouse).

---

## 26. Requirements Summary

### Q63: Does the requirements summary look complete?

**A63:** Yes, looking much better. Continue refining.

---

## 27. Connection Properties

### Q64: How should connection properties be set for user/roles?

**A64:** Via DuckDB's connection property mechanism. Extension API can be added later.

### Q65: Should granting to a non-existent role succeed or fail?

**A65:** Fail (role must exist). Both ClickHouse and PostgreSQL require the target to exist.

### Q66: Should there be a debug mode for RBAC evaluation?

**A66:** Yes. Detailed errors for users; debug logging for app developers (not exposed to end users).

### Q67: Is there an upper bound on roles per user?

**A67:** No specific limit. Don't worry about it for MVP.

### Q68: Should we use in-memory storage or DuckDB tables?

**A68:** DuckDB tables. Supports both persistence (for open-source users) and our in-memory pattern (run grants SQL at startup).

---

## 28. CREATE ROLE Decision

### Q69: Should CREATE ROLE be in MVP or deferred?

**A69:** Include CREATE ROLE in MVP. Both ClickHouse and PostgreSQL have it.

### Q70: Confirm: table must exist for grants?

**A70:** Yes.

### Q71: Confirm: table must exist for row policies?

**A71:** Yes. Consistent with grants.

---

## 29. DROP Behavior

### Q72: What happens when you DROP TABLE with existing grants/policies?

**A72:** **Name-based grants** (not OID-based). Grants survive table drops. This is critical for TAP's table refresh pattern:
```sql
DROP TABLE positions;
ALTER TABLE positions_new RENAME TO positions;
```
With name-based grants, the RBAC config survives the refresh cycle.

### Q73: What happens when you DROP ROLE with existing grants?

**A73:** RESTRICT implicit. Must clean up grants first. Prevents accidental data loss.

---

## 30. Views & JOINs

### Q74: Should grants and row policies apply to views?

**A74:** Tables only for MVP. Users can't create views in our setup anyway.

### Q75: For JOINs, are permissions checked on all tables?

**A75:** Yes. Need SELECT on all tables referenced. Row policies apply per-table independently.

---

## 31. Case Sensitivity & Schema

### Q76: Are role names case-sensitive?

**A76:** Case-insensitive (like DuckDB identifiers).

### Q77: Should the data model support role hierarchy even if MVP doesn't use it?

**A77:** Yes. Use same `duckdb_role_members` table for both user→role and role→role. MVP just doesn't allow role→role grants.

### Q78: Should MVP explicitly reject role-to-role grants?

**A78:** Yes, defer role hierarchy. Note that both ClickHouse and PostgreSQL support it.

### Q79: Should policy USING expressions be validated at creation time?

**A79:** Yes. Fail fast—error if column doesn't exist.

### Q80: Should there be built-in roles (PUBLIC, admin)?

**A80:** No built-in roles for MVP. ClickHouse has none by default either.

---

## 32. Name-Based Grants Details

### Q81: Should introspection show grants on non-existent tables (orphans)?

**A81:** Option C: Base table shows all grants. Filtered view (`duckdb_effective_privileges`) shows only valid ones.

### Q82: Should role-to-role grants be allowed in MVP?

**A82:** Defer. Note that both ClickHouse and PostgreSQL support it.

### Q83: Won't name-based grants cause typo issues?

**A83:** No. Table must exist at GRANT time (Q35), so typos are caught. Orphans only occur when tables are dropped later.

### Q84: Are there other DDL patterns to consider (partitioning, column renames)?

**A84:** No. No partitioning/inheritance. Columns never dropped. Users only SELECT.

---

## 33. CLI & Default Identity

### Q85: When running from CLI, what identity should the session have?

**A85:** Superuser by default. But note: if extension isn't loaded, there's no RBAC at all. RBAC only applies when extension is loaded.

### Q86: Should there be explicit orphan cleanup in MVP?

**A86:** No. Park for later. Orphaned grants don't break anything.

---

## 34. Column References

### Q87: Should forbidden columns in WHERE/ORDER BY/GROUP BY be denied?

**A87:** Yes! Columns shouldn't be referenced anywhere in the query if user lacks access.

### Q88: Does EXPLAIN require the same permissions as the query?

**A88:** Yes. Both ClickHouse and PostgreSQL require permissions for EXPLAIN.

---

## 35. System Tables & Export

### Q89: Should information_schema/system tables be filtered by access?

**A89:** No. See all tables for MVP. Users get denied when querying, but can see table names exist.

### Q90: Does SELECT permission imply ability to export (COPY TO)?

**A90:** Yes for MVP. TAP API restricts export anyway.

---

## 36. Policy Constraints

### Q91: Should subqueries be disallowed in policy USING expressions?

**A91:** Yes. No subqueries for MVP. Expressions can only reference the policy's table columns, `current_user()`, literals, and built-in functions.

### Q92: Do aggregates apply to filtered rows (post-policy)?

**A92:** Yes. Understood and expected. Users get accurate aggregates for their visible data only.

### Q93: Should error messages reveal table existence?

**A93:** Yes. Honest errors ("permission denied on X"). Users can discover table names via information_schema anyway.

---

## 37. Table Functions

### Q94: Should RBAC apply to table functions (`read_csv`, `range`, etc.)?

**A94:** No. Only base tables for MVP. TAP API explicitly allows these functions.

---

## 38. Policy Column Access

### Q95: Can policy USING expressions reference columns the user can't SELECT?

**A95:** Yes. Policies run with elevated privileges (security definer style, like PostgreSQL/ClickHouse). Otherwise row policies become too limited.

---

## 39. Schema Changes

### Q96: What happens when a column is dropped that has grants?

**A96:** Grant becomes orphaned (like table orphans). Columns are never dropped in our setup, but handle consistently.

### Q97: Does `GRANT SELECT ON table` include future columns?

**A97:** Yes. Table-level grant covers all columns, present and future.

---

## 40. Complex Queries

### Q98: Do row policies apply per-table in JOINs?

**A98:** Yes. Each table filtered independently before the join.

### Q99: Do self-joins apply the same row policy to all aliases?

**A99:** Yes. Same underlying table = same policy applies to all aliases.

### Q100: Do row policies apply in recursive CTEs?

**A100:** Yes. Policy applies to each reference of the base table in both base case and recursive case.

---

## Summary: MVP Scope

### Included in MVP

| Feature | Notes |
|---------|-------|
| External auth with superuser flag | App provides principal |
| CREATE/DROP ROLE | Explicit role creation |
| Role membership (flat) | `GRANT role TO role` for user→role only |
| SELECT on table | Only privilege needed |
| SELECT on columns | `GRANT SELECT (cols)` |
| Row policies (USING) | ClickHouse syntax, OR combination |
| `current_user()` function | For RLS expressions |
| Introspection tables | 5 tables + 2 views |
| Hard error on violation | Detailed messages |
| DuckDB table storage | Grants file loaded at startup |
| Name-based grants | Survive table drops |
| Case-insensitive identifiers | Match DuckDB behavior |

### Deferred to v2

| Feature | Notes |
|---------|-------|
| INSERT/UPDATE/DELETE privileges | Users can't write |
| DDL privileges | Users can't DDL |
| GRANT OPTION | Admin-only grants |
| Role hierarchy | Role→role grants |
| Restrictive policies (AND) | Only permissive (OR) for MVP |
| Partial revokes | Only grants for MVP |
| Wildcard grants | Exact names only |
| Column masking | Only column privileges |
| Views | Tables only |
| Subqueries in policies | Simple expressions only |
| Built-in roles (PUBLIC) | No special roles |
| Orphan cleanup utility | Manual cleanup if needed |
| Default deny mode | No policy = see all |

---

## SQL Syntax Reference

```sql
-- Roles
CREATE ROLE role_name;
DROP ROLE role_name;
GRANT role_name TO member_role;
REVOKE role_name FROM member_role;

-- Table privileges (SELECT only for MVP)
GRANT SELECT ON table_name TO role_name;
REVOKE SELECT ON table_name FROM role_name;

-- Column privileges
GRANT SELECT (col1, col2) ON table_name TO role_name;
REVOKE SELECT (col1) ON table_name FROM role_name;

-- Row policies
CREATE ROW POLICY policy_name ON table_name
    FOR SELECT
    USING (filter_expression)
    TO role_name [, role_name ...];

DROP ROW POLICY policy_name ON table_name;
```

---

## Introspection Tables

```sql
-- Base tables
SELECT * FROM duckdb_roles;
SELECT * FROM duckdb_role_members;
SELECT * FROM duckdb_table_privileges;
SELECT * FROM duckdb_column_privileges;
SELECT * FROM duckdb_row_policies;

-- Computed views
SELECT * FROM duckdb_effective_privileges;  -- current user's access
SELECT * FROM duckdb_my_roles;              -- current user's roles
```

---

## Behavioral Rules

1. **Table must exist** for GRANT/POLICY creation
2. **Role must exist** for GRANT target
3. **Name-based grants** survive table drop/recreate cycles
4. **Grants are case-insensitive** for identifiers
5. **Forbidden columns** denied anywhere in query (SELECT, WHERE, ORDER BY, etc.)
6. **SELECT *** rewrites silently to allowed columns
7. **Row policies OR together** when user has multiple matching policies
8. **No policy = see all rows** (policies are opt-in restrictions)
9. **Policy expressions see all columns** (security definer)
10. **EXPLAIN requires same permissions** as the underlying query
11. **Policy expressions bound at query time** (stored as text, parsed against current schema)
12. **Superuser flag in connection context** bypasses all checks
13. **GRANT/REVOKE auto-commit** (visible immediately)

---

## 41. DuckDB Extension Implementation

### Q101: How does the app set user identity on the connection?

**A101:** Connection property. The embedding app sets identity (user, roles, superuser flag) as connection properties. May require minimal DuckDB core change to support custom connection properties readable by extensions.

### Q102: What's the SELECT * behavior given DuckDB expands * during binding?

**A102:** We want `SELECT *` to work seamlessly (silently filter forbidden columns). This may require a minimal core change—approaches include parser override, OperatorExtension at binder level, or a new column-access callback during binding. Decision deferred until we explore DuckDB source.

### Q103: Should policy expressions be bound at creation time or query time?

**A103:** Query-time binding (like PostgreSQL/ClickHouse). Store filter expression as text, parse and bind at query time against current table schema. Simpler to implement and handles schema evolution.

### Q104: Do system tables need protection from direct modification?

**A104:** No. Same as PostgreSQL/ClickHouse: system tables are readable by anyone, modification happens only via DDL (`CREATE ROLE`, `GRANT`). Direct INSERT is technically possible but unsupported.

### Q105: How does the extension detect superuser?

**A105:** Superuser flag is part of connection context: `{ user, roles, superuser }`. Extension checks this flag first; if true, all permission checks are bypassed.

### Q106: Will DuckDB's parser accept our custom DDL?

**A106:** No—confirmed. `CREATE ROLE foo;` fails in DuckDB CLI with parser error. This means `ParserExtension::parse_function` (fallback) will catch our DDL syntax. ✓

---

## 42. Implementation Approach

### Q107: Which DuckDB extension hook should we use for RBAC enforcement?

**A107:** Multiple hooks needed:
- **ParserExtension::parse_function** — Custom DDL (CREATE ROLE, GRANT, etc.)
- **OptimizerExtension** — Row filter injection, possibly column checking
- **ExtensionCallback + ClientContextState** — Per-connection identity storage
- **Possibly OperatorExtension or parser_override** — For SELECT * handling

Not restricted to optimizer extension alone. Will explore DuckDB source to determine cleanest approach.

### Q108: How do extension hooks signal errors?

**A108:** Optimizer extensions throw C++ exceptions. The exception propagates up and aborts the query with an error message.

### Q109: Are GRANT/REVOKE statements transactional?

**A109:** Yes, they inherit DuckDB's transaction semantics since they're implemented as table modifications. This is acceptable.

### Q110: Testing strategy?

**A110:** Deferred. Will address during implementation.

### Q111: Should we add a minimal core change for column access control during binding?

**A111:** Possibly. Three approaches under consideration:
1. **Parser override** — Intercept SQL, rewrite `SELECT *` to explicit allowed columns
2. **OperatorExtension** — Hook at binder level if it fires before * expansion
3. **New core hook** — Add column-access callback during binding

Will explore DuckDB source to determine which is cleanest.

### Q112: Should GRANT/REVOKE auto-commit or be transactional?

**A112:** Auto-commit is acceptable. Grants are immediately visible after the statement completes.

