# DuckDB RBAC Extension: Product Requirements Document

**Version:** 1.0  
**Last Updated:** December 2024  
**Owner:** Product/Engineering  
**Status:** Approved for MVP

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Problem Statement](#2-problem-statement)
3. [Goals & Success Criteria](#3-goals--success-criteria)
4. [User Personas](#4-user-personas)
5. [User Stories](#5-user-stories)
6. [Functional Requirements](#6-functional-requirements)
7. [Non-Functional Requirements](#7-non-functional-requirements)
8. [Scope](#8-scope)
9. [Acceptance Criteria](#9-acceptance-criteria)
10. [Dependencies & Constraints](#10-dependencies--constraints)
11. [Risks](#11-risks)
12. [Timeline](#12-timeline)
13. [Open Questions (Resolved)](#13-open-questions-resolved)

---

## 1. Executive Summary

### 1.1 What We're Building

A Role-Based Access Control (RBAC) extension for DuckDB that enables:
- **Table-level access control**: Restrict which tables users can query
- **Column-level access control**: Restrict which columns users can see
- **Row-level security (RLS)**: Filter rows based on user identity/role

### 1.2 Why We're Building It

DuckDB has no built-in access control. Our analytics platform (TAP) embeds DuckDB and needs to:
- Prevent users from accessing sensitive data they shouldn't see
- Apply row-level filters based on user's team/region
- Move authorization logic from application layer into the database layer

### 1.3 Who Benefits

- **End Users**: Can safely query data without accidentally seeing restricted information
- **Platform Engineers**: Authorization is enforced at DB level, not scattered across app code
- **Security/Compliance**: Audit-friendly, declarative access policies

---

## 2. Problem Statement

### 2.1 Current State

- DuckDB has **no user/role/permission model**
- TAP currently enforces access control by:
  - Rewriting SQL queries to add WHERE clauses (fragile, complex)
  - Validating table names against a whitelist (coarse-grained)
  - Trusting application layer to filter columns (error-prone)

### 2.2 Pain Points

| Pain Point | Impact |
|------------|--------|
| Query rewriting is complex | Bugs, maintenance burden |
| Column filtering in app layer | Easy to forget, security risk |
| No row-level filtering | Users see all rows or none |
| Multiple code paths for access control | Inconsistent enforcement |
| Difficult to audit | Can't query "who can access what?" |

### 2.3 Desired State

- Single source of truth for access control (in DuckDB)
- Declarative policies (SQL-based, not code)
- Consistent enforcement regardless of access path
- Queryable permissions (introspection)

---

## 3. Goals & Success Criteria

### 3.1 Goals

| Goal | Description |
|------|-------------|
| **G1** | Users cannot access tables they don't have permission for |
| **G2** | Users cannot see columns they don't have permission for |
| **G3** | Row-level policies automatically filter query results |
| **G4** | Permissions are managed via SQL DDL (not config files) |
| **G5** | Current permissions are queryable via system tables |

### 3.2 Success Criteria

| Criteria | Measurement |
|----------|-------------|
| **Security** | Zero unauthorized data access in production |
| **Adoption** | TAP migrates from query rewriting to RBAC |
| **Usability** | Support team can manage permissions without code changes |
| **Performance** | <5ms overhead per query for permission checks |
| **Auditability** | Can answer "who can access table X?" in one query |

---

## 4. User Personas

### 4.1 End User (Analyst)

- Runs SELECT queries via TAP UI
- Should only see data relevant to their team/region
- Doesn't know (or care) about RBAC internals
- Expects clear error messages when access is denied

### 4.2 Support Engineer

- Manages permissions for users/teams
- Runs SQL commands to grant/revoke access
- Needs to troubleshoot "why can't user X see table Y?"
- Maintains a `grants.sql` file that's version-controlled

### 4.3 Platform Engineer

- Integrates RBAC extension into TAP
- Sets user identity on each connection
- Needs reliable, predictable behavior
- Wants minimal application-layer code

### 4.4 Security/Compliance

- Reviews access policies
- Needs audit trail of who has access to what
- Wants to verify policies are correctly applied

---

## 5. User Stories

### 5.1 Access Control

| ID | Story | Priority |
|----|-------|----------|
| US-1 | As an analyst, I can only query tables I have access to, so I don't accidentally see sensitive data | P0 |
| US-2 | As an analyst, I see only the columns I have access to, so PII is protected | P0 |
| US-3 | As an analyst, I see only rows matching my team/region, so I work with relevant data | P0 |
| US-4 | As an analyst, I get a clear error message when access is denied, so I know what to request | P0 |

### 5.2 Permission Management

| ID | Story | Priority |
|----|-------|----------|
| US-5 | As a support engineer, I can create roles via SQL, so I can organize permissions | P0 |
| US-6 | As a support engineer, I can grant table access to roles, so I control who sees what | P0 |
| US-7 | As a support engineer, I can grant column access to roles, so I control column visibility | P0 |
| US-8 | As a support engineer, I can create row policies, so I filter data by user attributes | P0 |
| US-9 | As a support engineer, I can revoke access, so I can respond to changing requirements | P0 |
| US-10 | As a support engineer, I can view all grants via SQL, so I can audit access | P0 |

### 5.3 Integration

| ID | Story | Priority |
|----|-------|----------|
| US-11 | As a platform engineer, I can set user identity on a connection, so RBAC knows who's querying | P0 |
| US-12 | As a platform engineer, superuser connections bypass RBAC, so admins can always access data | P0 |
| US-13 | As a platform engineer, I can load grants from a SQL file at startup, so permissions are reproducible | P1 |

---

## 6. Functional Requirements

### 6.1 Roles

| ID | Requirement |
|----|-------------|
| FR-1 | System shall support creating named roles via `CREATE ROLE name` |
| FR-2 | System shall support deleting roles via `DROP ROLE name` |
| FR-3 | System shall prevent dropping roles that have existing grants |
| FR-4 | System shall support role membership via `GRANT role TO member` |
| FR-5 | Role names shall be case-insensitive |

### 6.2 Table Privileges

| ID | Requirement |
|----|-------------|
| FR-6 | System shall support granting SELECT on tables via `GRANT SELECT ON table TO role` |
| FR-7 | System shall support revoking SELECT on tables via `REVOKE SELECT ON table FROM role` |
| FR-8 | Table must exist at grant time |
| FR-9 | Grants shall reference tables by name (survive table drop/recreate) |

### 6.3 Column Privileges

| ID | Requirement |
|----|-------------|
| FR-10 | System shall support granting SELECT on specific columns via `GRANT SELECT (cols) ON table TO role` |
| FR-11 | Column grants override table-level "all columns" behavior |
| FR-12 | Access to columns must be checked everywhere in query (SELECT, WHERE, ORDER BY, etc.) |
| FR-13 | Accessing a forbidden column shall result in an error |

### 6.4 Row Policies

| ID | Requirement |
|----|-------------|
| FR-14 | System shall support creating row policies via `CREATE ROW POLICY name ON table USING (expr) TO role` |
| FR-15 | Multiple policies for same (table, role) shall be combined with OR |
| FR-16 | Policy expressions can reference any column of the table |
| FR-17 | Policy expressions can use `current_user()` function |
| FR-18 | No policy for a user means they see all rows (policies are additive restrictions) |

### 6.5 Session Identity

| ID | Requirement |
|----|-------------|
| FR-19 | User identity (name, roles, superuser flag) shall be set per connection |
| FR-20 | Identity shall be immutable once set |
| FR-21 | Superuser flag shall bypass all permission checks |
| FR-22 | If no identity is set (e.g., CLI), user shall be treated as superuser |

### 6.6 Introspection

| ID | Requirement |
|----|-------------|
| FR-23 | System shall provide `duckdb_roles` table listing all roles |
| FR-24 | System shall provide `duckdb_table_privileges` table listing table grants |
| FR-25 | System shall provide `duckdb_column_privileges` table listing column grants |
| FR-26 | System shall provide `duckdb_row_policies` table listing policies |
| FR-27 | System shall provide `duckdb_effective_privileges` view for current user's access |

---

## 7. Non-Functional Requirements

### 7.1 Performance

| ID | Requirement |
|----|-------------|
| NFR-1 | Permission check overhead shall be <5ms per query |
| NFR-2 | System shall cache permission lookups per session |

### 7.2 Security

| ID | Requirement |
|----|-------------|
| NFR-3 | Users shall not be able to change their own identity via SQL |
| NFR-4 | Error messages shall be detailed (no security-through-obscurity) |

### 7.3 Compatibility

| ID | Requirement |
|----|-------------|
| NFR-5 | Extension shall work with DuckDB's embedded mode |
| NFR-6 | Syntax shall mimic ClickHouse/PostgreSQL where possible |
| NFR-7 | Grants shall survive table drop/recreate cycles |

### 7.4 Operability

| ID | Requirement |
|----|-------------|
| NFR-8 | Grants shall be loadable from a SQL file at startup |
| NFR-9 | Grant changes shall take effect immediately (auto-commit) |

---

## 8. Scope

### 8.1 In Scope (MVP)

| Feature | Description |
|---------|-------------|
| Roles | CREATE/DROP ROLE, flat membership (no hierarchy) |
| Table privileges | GRANT/REVOKE SELECT on tables |
| Column privileges | GRANT/REVOKE SELECT on specific columns |
| Row policies | CREATE/DROP ROW POLICY with USING clause |
| Identity | Per-connection user/roles/superuser |
| Introspection | System tables for roles, grants, policies |
| Functions | `current_user()`, `current_roles()` |

### 8.2 Out of Scope (Future)

| Feature | Reason |
|---------|--------|
| INSERT/UPDATE/DELETE privileges | Users only SELECT in TAP |
| DDL privileges (CREATE TABLE, etc.) | Users can't DDL in TAP |
| Role hierarchy | Added complexity, not needed initially |
| GRANT OPTION | All grants are admin-only for MVP |
| Restrictive (AND) policies | OR-only for MVP |
| Column masking | Deny access for MVP, mask later |
| Wildcard grants | Exact names only |
| Silent SELECT * filtering | MVP errors on forbidden columns |

### 8.3 MVP Limitations

| Limitation | User Impact | Workaround |
|------------|-------------|------------|
| `SELECT *` with forbidden columns errors | Must use explicit column list | Query `duckdb_effective_privileges` first |
| No role hierarchy | Duplicate grants across roles | Create shared base roles |

---

## 9. Acceptance Criteria

### 9.1 Table Access

```
GIVEN a user with role 'analyst'
AND 'analyst' has SELECT on table 'orders'
WHEN user queries SELECT * FROM orders
THEN query succeeds and returns all columns
```

```
GIVEN a user with role 'analyst'  
AND 'analyst' does NOT have SELECT on table 'secret'
WHEN user queries SELECT * FROM secret
THEN query fails with "Permission denied on table 'secret'"
```

### 9.2 Column Access

```
GIVEN a user with role 'analyst'
AND 'analyst' has SELECT on columns (id, name) of table 'users'
WHEN user queries SELECT id, name FROM users
THEN query succeeds and returns 2 columns
```

```
GIVEN a user with role 'analyst'
AND 'analyst' has SELECT on columns (id, name) of table 'users'
WHEN user queries SELECT ssn FROM users
THEN query fails with "Permission denied on column 'ssn' of table 'users'"
```

### 9.3 Row Policies

```
GIVEN a user 'alice' with role 'regional_sales'
AND row policy on 'orders': USING (region = 'US') TO regional_sales
WHEN alice queries SELECT * FROM orders
THEN query returns only rows where region = 'US'
```

```
GIVEN a user with roles ['role_a', 'role_b']
AND row policy on 'orders': USING (x = 1) TO role_a
AND row policy on 'orders': USING (y = 2) TO role_b
WHEN user queries SELECT * FROM orders
THEN query returns rows where (x = 1) OR (y = 2)
```

### 9.4 Superuser

```
GIVEN a user with superuser=true
WHEN user queries any table
THEN query succeeds regardless of grants
```

---

## 10. Dependencies & Constraints

### 10.1 Dependencies

| Dependency | Type | Impact |
|------------|------|--------|
| DuckDB extension API | Technical | Must use available hooks (parser, optimizer) |
| DuckDB C++ ABI | Technical | Must match DuckDB version |
| TAP integration | Integration | TAP must set identity on connections |

### 10.2 Constraints

| Constraint | Description |
|------------|-------------|
| Extension-only | Prefer no DuckDB core changes (minimal if needed) |
| SELECT-only | Users can only SELECT (no DML in TAP) |
| Single-node | No replication/cluster considerations |
| In-memory OK | TAP uses in-memory DuckDB; grants loaded at startup |

---

## 11. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| DuckDB hooks insufficient | Medium | High | Validate with spikes first |
| Performance overhead | Low | Medium | Cache permission lookups |
| Complex queries break (CTEs, subqueries) | Medium | Medium | Extensive testing |
| SELECT * limitation confuses users | Medium | Low | Document clearly, improve in v2 |

---

## 12. Timeline

| Phase | Duration | Deliverable |
|-------|----------|-------------|
| **Spikes** | 1 day | Validated extension hooks work |
| **Foundation** | 2 days | Extension loads, identity works |
| **DDL** | 3 days | CREATE ROLE, GRANT work |
| **Enforcement** | 3 days | Queries blocked without grants |
| **Row Policies** | 2 days | Row filtering works |
| **Polish** | 2 days | Introspection, error messages, edge cases |
| **Total** | ~13 days | MVP complete |

---

## 13. Open Questions (Resolved)

These questions were resolved during requirements gathering. See `rbac-qa.md` for full discussion.

| Question | Resolution |
|----------|------------|
| External vs internal superuser? | External (app sets flag) |
| SELECT * behavior? | MVP: error on forbidden columns |
| Policy binding: create-time or query-time? | Query-time (like PostgreSQL) |
| Grants tied to table OID or name? | Name (survives DDL) |
| GRANT transaction semantics? | Auto-commit |
| Role hierarchy in MVP? | No, deferred |
| Restrictive (AND) policies? | No, deferred (OR only) |
| Built-in roles (PUBLIC)? | No, deferred |

---

## Appendix: Reference Documents

| Document | Purpose |
|----------|---------|
| `rbac-qa.md` | Full requirements Q&A (112 questions) |
| `rbac-design.md` | Technical design document |
| `duck/duckdb-ext-qa.md` | DuckDB extension API analysis |
| `ch/rbac-ch-arch.md` | ClickHouse RBAC reference |
| `pg/rbac-pg.md` | PostgreSQL RBAC reference |

