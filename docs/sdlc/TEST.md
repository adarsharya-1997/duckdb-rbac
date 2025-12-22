# DuckDB RBAC Extension: Test Specification

**Version:** 1.0  
**Last Updated:** December 2024  
**Traceability:** Requirements from `PRD.md`, detailed design from `LLD.md`

---

## Overview

This document defines acceptance test cases for the RBAC extension. Each test case is described in plain English with expected outcomes. Once reviewed, these will be implemented as `.test` files using DuckDB's sqllogictest format.

### Test Case Format

Each test case includes:
- **ID**: Unique identifier (T-XX-YY)
- **Description**: What we're testing in plain English
- **Preconditions**: Setup required before the test
- **Expected**: PASS (succeeds) or ERROR (fails with message)
- **Requirements**: Links to FR-x, US-x, NFR-x

---

## 1. Role Management

**Requirements:** FR-1, FR-2, FR-3, FR-4, FR-5, US-5

### 1.1 CREATE ROLE

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-01-01 | Create a new role with a valid name | PASS | FR-1 |
| T-01-02 | Create a role that already exists | ERROR: "Role 'x' already exists" | FR-1 |
| T-01-03 | Create role with uppercase name, query with lowercase | PASS (case-insensitive) | FR-5 |
| T-01-04 | Create role with mixed case, verify stored lowercase | PASS, verify in duckdb_roles | FR-5 |

### 1.2 DROP ROLE

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-01-05 | Drop an existing role with no grants | PASS | FR-2 |
| T-01-06 | Drop a role that doesn't exist | ERROR: "Role 'x' does not exist" | FR-2 |
| T-01-07 | Drop a role that has table grants | ERROR: "Cannot drop role 'x': role has existing grants" | FR-3 |
| T-01-08 | Drop a role that has column grants | ERROR: "Cannot drop role 'x': role has existing grants" | FR-3 |
| T-01-09 | Drop a role that has row policies | ERROR: "Cannot drop role 'x': role has existing grants" | FR-3 |
| T-01-10 | Drop a role that is a member of another role | ERROR: "Cannot drop role 'x': role has existing grants" | FR-3 |

### 1.3 Role Membership (GRANT role TO role)

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-01-11 | Grant role A to role B (both exist) | PASS | FR-4 |
| T-01-12 | Grant role to non-existent member role | ERROR: "Role 'x' does not exist" | FR-4 |
| T-01-13 | Grant non-existent role to a member | ERROR: "Role 'x' does not exist" | FR-4 |
| T-01-14 | Grant same membership twice | PASS (idempotent) or ERROR (decide) | FR-4 |
| T-01-15 | Revoke membership that exists | PASS | FR-4 |
| T-01-16 | Revoke membership that doesn't exist | ERROR or PASS (decide) | FR-4 |

---

## 2. Table Privileges

**Requirements:** FR-6, FR-7, FR-8, FR-9, US-6

### 2.1 GRANT SELECT ON table

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-02-01 | Grant SELECT on existing table to existing role | PASS | FR-6 |
| T-02-02 | Grant SELECT on non-existent table | ERROR: "Table 'x' does not exist" | FR-8 |
| T-02-03 | Grant SELECT to non-existent role | ERROR: "Role 'x' does not exist" | FR-6 |
| T-02-04 | Grant SELECT twice (idempotent) | PASS (no error on duplicate) | FR-6 |
| T-02-05 | Grant on table with schema prefix (main.tablename) | PASS | FR-6 |

### 2.2 REVOKE SELECT ON table

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-02-06 | Revoke SELECT that was granted | PASS | FR-7 |
| T-02-07 | Revoke SELECT that was never granted | PASS or ERROR (decide) | FR-7 |
| T-02-08 | Revoke SELECT from non-existent role | ERROR: "Role 'x' does not exist" | FR-7 |

### 2.3 Name-Based Grant Persistence

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-02-09 | Grant on table, drop table, recreate table - grant survives | PASS (user can query new table) | FR-9 |
| T-02-10 | Grant on table, drop table - grant visible in introspection as orphan | PASS (visible in base table) | FR-9 |

---

## 3. Column Privileges

**Requirements:** FR-10, FR-11, FR-12, FR-13, US-7

### 3.1 GRANT SELECT (columns) ON table

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-03-01 | Grant SELECT on specific columns to role | PASS | FR-10 |
| T-03-02 | Grant SELECT on non-existent column | ERROR: "Column 'x' does not exist" | FR-10 |
| T-03-03 | Grant SELECT on additional columns (additive) | PASS (both sets of columns accessible) | FR-10 |
| T-03-04 | Column grant overrides table-level "all columns" | User can only see granted columns | FR-11 |

### 3.2 REVOKE SELECT (columns) ON table

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-03-05 | Revoke SELECT on specific column | PASS (column no longer accessible) | FR-10 |
| T-03-06 | Revoke all columns - user loses table access | PASS (permission denied on table) | FR-10 |

### 3.3 Column Access Enforcement

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-03-07 | SELECT allowed column - succeeds | PASS | FR-12 |
| T-03-08 | SELECT forbidden column explicitly | ERROR: "Permission denied on column 'x'" | FR-13 |
| T-03-09 | SELECT * with forbidden columns | ERROR: "Permission denied on column 'x'" | FR-13 |
| T-03-10 | Forbidden column in WHERE clause | ERROR: "Permission denied on column 'x'" | FR-12 |
| T-03-11 | Forbidden column in ORDER BY | ERROR: "Permission denied on column 'x'" | FR-12 |
| T-03-12 | Forbidden column in GROUP BY | ERROR: "Permission denied on column 'x'" | FR-12 |
| T-03-13 | Forbidden column in HAVING | ERROR: "Permission denied on column 'x'" | FR-12 |
| T-03-14 | Forbidden column in JOIN condition | ERROR: "Permission denied on column 'x'" | FR-12 |
| T-03-15 | Forbidden column in expression (e.g., salary * 2) | ERROR: "Permission denied on column 'x'" | FR-12 |
| T-03-16 | Forbidden column in CASE expression | ERROR: "Permission denied on column 'x'" | FR-12 |
| T-03-17 | Forbidden column in subquery | ERROR: "Permission denied on column 'x'" | FR-12 |

---

## 4. Row Policies

**Requirements:** FR-14, FR-15, FR-16, FR-17, FR-18, US-8

### 4.1 CREATE ROW POLICY

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-04-01 | Create row policy with valid USING expression | PASS | FR-14 |
| T-04-02 | Create policy on non-existent table | ERROR: "Table 'x' does not exist" | FR-14 |
| T-04-03 | Create policy for non-existent role | ERROR: "Role 'x' does not exist" | FR-14 |
| T-04-04 | Create policy with invalid expression (bad column) | ERROR: "Column 'x' does not exist" | FR-14 |
| T-04-05 | Create policy with invalid expression (syntax error) | ERROR: syntax/parse error | FR-14 |
| T-04-06 | Create policy referencing current_user() | PASS | FR-17 |
| T-04-07 | Create policy with same name on same table | ERROR: "Policy 'x' already exists" | FR-14 |

### 4.2 DROP ROW POLICY

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-04-08 | Drop existing row policy | PASS | FR-14 |
| T-04-09 | Drop non-existent policy | ERROR: "Policy 'x' does not exist" | FR-14 |

### 4.3 Row Policy Enforcement

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-04-10 | Query table with row policy - only matching rows returned | PASS (filtered results) | FR-14 |
| T-04-11 | No policy for user's role - all rows visible | PASS (no filter applied) | FR-18 |
| T-04-12 | Policy expression uses current_user() - filters correctly | PASS (rows match current user) | FR-17 |
| T-04-13 | Policy expression references column user can't SELECT | PASS (policy runs elevated) | FR-16 |

### 4.4 Multiple Policies (OR Combination)

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-04-14 | Two policies for same role on same table - OR'd together | PASS (union of both filters) | FR-15 |
| T-04-15 | User has two roles, each with different policy - OR'd together | PASS (union of both filters) | FR-15 |
| T-04-16 | Three policies combining - all OR'd | PASS (union of all three) | FR-15 |

---

## 5. Session Identity

**Requirements:** FR-19, FR-20, FR-21, FR-22, US-11, US-12, NFR-1

### 5.1 Identity Setting

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-05-01 | Set identity on connection (user, roles, superuser=false) | PASS | FR-19 |
| T-05-02 | current_user() returns the set user name | PASS (correct name) | FR-19 |
| T-05-03 | current_roles() returns the set roles | PASS (correct roles) | FR-19 |
| T-05-04 | Attempt to change identity after it's set | ERROR or ignored | FR-20, NFR-1 |

### 5.2 Superuser Bypass

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-05-05 | Superuser queries table with no grants | PASS (bypasses checks) | FR-21 |
| T-05-06 | Superuser queries forbidden column | PASS (bypasses checks) | FR-21 |
| T-05-07 | Superuser queries table with row policy | PASS (no filter applied) | FR-21 |

### 5.3 Default Identity (No Identity Set)

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-05-08 | No identity set - treated as superuser | PASS (all queries work) | FR-22 |
| T-05-09 | Extension loaded in CLI with no identity - superuser | PASS | FR-22 |

---

## 6. Permission Enforcement (Integration)

**Requirements:** US-1, US-2, US-3, US-4

### 6.1 Table-Level Enforcement

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-06-01 | User with no grants queries any table | ERROR: "Permission denied on table 'x'" | US-1 |
| T-06-02 | User with grant on table A queries table A | PASS | US-1 |
| T-06-03 | User with grant on table A queries table B (no grant) | ERROR: "Permission denied on table 'B'" | US-1 |

### 6.2 Inherited Permissions

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-06-04 | User is member of role with grant - inherits access | PASS | FR-4 |
| T-06-05 | User is member of multiple roles - union of all grants | PASS | FR-4 |

### 6.3 Error Messages

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-06-06 | Permission denied error includes table name | ERROR message contains table name | US-4 |
| T-06-07 | Permission denied error includes column name | ERROR message contains column name | US-4 |
| T-06-08 | Permission denied error includes user name | ERROR message contains user name | US-4 |

---

## 7. Introspection

**Requirements:** FR-23, FR-24, FR-25, FR-26, FR-27, US-10

### 7.1 System Tables

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-07-01 | Query duckdb_roles - shows all created roles | PASS (correct data) | FR-23 |
| T-07-02 | Query duckdb_table_privileges - shows all table grants | PASS (correct data) | FR-24 |
| T-07-03 | Query duckdb_column_privileges - shows all column grants | PASS (correct data) | FR-25 |
| T-07-04 | Query duckdb_row_policies - shows all policies | PASS (correct data) | FR-26 |
| T-07-05 | Query duckdb_role_members - shows memberships | PASS (correct data) | FR-23 |

### 7.2 Effective Privileges View

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-07-06 | Query duckdb_effective_privileges - shows current user's access | PASS (correct for current user) | FR-27 |
| T-07-07 | Effective privileges excludes orphaned grants | PASS (only valid grants shown) | FR-27 |
| T-07-08 | Effective privileges shows inherited grants | PASS (includes via-role info) | FR-27 |

---

## 8. Complex Queries

**Requirements:** Edge cases from LLD Section 12.5

### 8.1 JOINs

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-08-01 | JOIN two tables - user has access to both | PASS | - |
| T-08-02 | JOIN two tables - user lacks access to one | ERROR: "Permission denied on table 'x'" | - |
| T-08-03 | JOIN with row policies - each table filtered independently | PASS (correct filtering) | - |
| T-08-04 | Self-join - same policy applies to both aliases | PASS (both filtered same way) | - |

### 8.2 Subqueries

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-08-05 | Subquery in FROM - permission checked on subquery table | ERROR if no access | - |
| T-08-06 | Subquery in WHERE - permission checked | ERROR if no access | - |
| T-08-07 | Correlated subquery - both tables checked | ERROR if no access to either | - |

### 8.3 CTEs

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-08-08 | WITH clause referencing table - permission checked | ERROR if no access | - |
| T-08-09 | Recursive CTE - policy applies to all references | PASS (filtered correctly) | - |
| T-08-10 | Multiple CTEs - each table checked | ERROR if any lacks access | - |

### 8.4 Aggregations

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-08-11 | COUNT(*) with row policy - counts only visible rows | PASS (filtered count) | - |
| T-08-12 | SUM/AVG with row policy - aggregates only visible rows | PASS (filtered aggregates) | - |
| T-08-13 | Aggregate on forbidden column | ERROR: "Permission denied on column 'x'" | - |

### 8.5 EXPLAIN

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-08-14 | EXPLAIN on query user can run | PASS | - |
| T-08-15 | EXPLAIN on query user cannot run | ERROR: same as running the query | - |

---

## 9. DDL Statements (RBAC DDL)

**Requirements:** Syntax from LLD Section 4

### 9.1 Syntax Validation

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-09-01 | CREATE ROLE with valid syntax | PASS | - |
| T-09-02 | DROP ROLE with valid syntax | PASS | - |
| T-09-03 | GRANT SELECT ON table TO role - valid syntax | PASS | - |
| T-09-04 | GRANT SELECT (cols) ON table TO role - valid syntax | PASS | - |
| T-09-05 | REVOKE SELECT ON table FROM role - valid syntax | PASS | - |
| T-09-06 | CREATE ROW POLICY ... USING ... TO - valid syntax | PASS | - |
| T-09-07 | DROP ROW POLICY name ON table - valid syntax | PASS | - |
| T-09-08 | GRANT role TO member - valid syntax | PASS | - |
| T-09-09 | REVOKE role FROM member - valid syntax | PASS | - |

---

## 10. Functions

**Requirements:** From PRD Section 8.1

### 10.1 current_user()

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-10-01 | SELECT current_user() - returns set user name | PASS (correct name) | - |
| T-10-02 | current_user() in row policy expression | PASS (evaluates correctly) | - |
| T-10-03 | current_user() when no identity set | PASS (returns default/empty) | - |

### 10.2 current_roles()

| ID | Description | Expected | Requirements |
|----|-------------|----------|--------------|
| T-10-04 | SELECT current_roles() - returns set roles | PASS (correct list) | - |
| T-10-05 | current_roles() with multiple roles | PASS (all roles returned) | - |

---

## Test Execution Checklist

Use this checklist to track implementation progress:

### Phase 1: Foundation
- [ ] T-05-01 to T-05-09 (Session Identity)
- [ ] T-10-01 to T-10-05 (Functions)

### Phase 2: DDL
- [ ] T-01-01 to T-01-16 (Role Management)
- [ ] T-09-01 to T-09-09 (Syntax Validation)

### Phase 3: Grants
- [ ] T-02-01 to T-02-10 (Table Privileges)
- [ ] T-03-01 to T-03-06 (Column Privileges - DDL)

### Phase 4: Enforcement
- [ ] T-06-01 to T-06-08 (Permission Enforcement)
- [ ] T-03-07 to T-03-17 (Column Access Enforcement)

### Phase 5: Row Policies
- [ ] T-04-01 to T-04-16 (Row Policies)

### Phase 6: Introspection
- [ ] T-07-01 to T-07-08 (Introspection)

### Phase 7: Edge Cases
- [ ] T-08-01 to T-08-15 (Complex Queries)

---

## Summary

| Category | Test Count |
|----------|------------|
| Role Management | 16 |
| Table Privileges | 10 |
| Column Privileges | 17 |
| Row Policies | 16 |
| Session Identity | 9 |
| Permission Enforcement | 8 |
| Introspection | 8 |
| Complex Queries | 15 |
| DDL Syntax | 9 |
| Functions | 5 |
| **Total** | **113** |

---

## Open Questions

1. **T-01-14, T-01-16**: Should duplicate grants be idempotent (PASS) or error?
2. **T-02-07**: Should revoking non-existent grant be idempotent (PASS) or error?
3. Should we test RBAC DDL from non-superuser? (Currently assumed admin-only)

---

## Document History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | Dec 2024 | Initial test specification |

