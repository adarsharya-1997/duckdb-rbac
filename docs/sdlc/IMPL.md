# DuckDB RBAC Extension: Implementation Plan

**Version:** 1.0  
**Last Updated:** December 2024  
**Approach:** Test-Driven Development (TDD)

---

## Overview

This document tracks the implementation of the RBAC extension. Each phase has:
- **Tasks** with checkboxes for progress tracking
- **Test files** that validate completion
- **Dependencies** on previous phases
- **Acceptance criteria** (which tests must pass)

**Goal:** Make all tests in `test/sql/rbac/*.test` pass.

**Reference Documents:**
- `PRD.md` - Product requirements
- `LLD.md` - Technical design (Section 2 for architecture)
- `TEST.md` - Test specification
- `RFC.md` - DuckDB extension approach
- `SPIKE.md` - Spike testing results and API findings

---

## Progress Summary

| Phase | Status | Tests Passing |
|-------|--------|---------------|
| Phase 0: Spikes | ✅ Complete | 54 assertions |
| Phase 1: Foundation | ✅ Complete | 15 assertions |
| Phase 2: DDL Parsing | ✅ Complete | 50 assertions |
| Phase 3: Storage & Grants | ✅ Complete | 142 assertions |
| Phase 4: Enforcement | ⬜ Not Started | - |
| Phase 5: Row Policies | ⬜ Not Started | - |
| Phase 6: Polish | ⬜ Not Started | - |

**Overall:** 207 assertions passing (15+50+33+31+51+27)

---

## Phase 0: Validation Spikes ✅

**Status:** Complete  
**Test File:** `test/sql/rbac/00_spikes.test` (54 assertions)  
**Details:** See `docs/sdlc/SPIKE.md`

All 6 spikes passed, confirming that DuckDB's extension API supports our RBAC implementation:

| Spike | What We Proved |
|-------|----------------|
| 0.1 | `ParserExtension` catches custom DDL (CREATE ROLE, GRANT, etc.) |
| 0.2 | `OptimizerExtension` can throw `PermissionException` |
| 0.3 | `OptimizerExtension` can inject `LogicalFilter` for row policies |
| 0.4 | `ClientContextState` stores per-connection identity |
| 0.5 | Custom expression binder works for row policy filters |
| 0.6 | `parser_override` enables column-level `SELECT *` rewriting |

**Key Decisions from Spikes:**
- Use `ExtensionCallbackManager::Get(db).Register()` for hook registration
- Use custom `SimpleExpressionBinder` for row policy expression binding
- Use `parser_override` with `fallback` mode for column-level SELECT * handling
- `LogicalGet::GetTable()` can return nullptr - must handle gracefully

### Spike Code Cleanup Strategy

Spike code contains hardcoded test logic that must be replaced with real implementations as we progress. Clean up incrementally, phase by phase:

| Phase | Spike Code to Replace | Action |
|-------|----------------------|--------|
| **Phase 2** | CREATE ROLE / DROP ROLE stubs | Replace with real `INSERT INTO duckdb_roles` |
| **Phase 4** | "blocked" table hardcoded check | Replace with permission lookup from `duckdb_table_privileges` |
| **Phase 5** | "filtered_table", "policy_test" hardcoded filters | Replace with policy lookup from `duckdb_row_policies` |
| **Phase 6** | "col_test_*" parser_override logic | Replace with proper column-level enforcement |

**Spike Test File (`00_spikes.test`):**
- Keep passing until replaced by real tests
- Archive/retire once all features are tested in proper test files (`01_roles.test`, etc.)

**Approach:**
1. Implement real feature
2. Update tests to use real grants/policies
3. Remove corresponding spike hardcoded logic
4. Verify spike tests still pass (until retired)

---

## Phase 1: Foundation

**Goal:** Extension skeleton with identity management and system tables.

**Duration:** 2 days  
**Dependencies:** Phase 0 completed  
**Test File:** `test/sql/rbac/00_extension_load.test`

### Tasks

#### 1.1 Extension Entry Point
- [x] Create extension entry point (`DUCKDB_CPP_EXTENSION_ENTRY`) *(done in spikes)*
- [x] Extension loads without error via `LOAD 'quack'` or `require quack` *(done in spikes)*
- [x] Register extension in DuckDB's extension system *(done in spikes)*

#### 1.2 Session State Management
- [x] Implement `RBACState : ClientContextState` *(done in spikes)*
- [x] Register `ExtensionCallback` for connection lifecycle *(done in spikes)*
- [x] Install `RBACState` in `ClientContext::registered_state` on connection open *(done in spikes)*
- [x] Implement `rbac_set_identity(user, roles, is_superuser)` function

#### 1.3 Scalar Functions
- [x] Register `rbac_current_user()` ScalarFunction *(done in spikes)*
- [x] Register `rbac_current_roles()` ScalarFunction *(done in spikes)*
- [x] Register `rbac_is_superuser()` ScalarFunction *(done in spikes)*

#### 1.4 System Tables (Schema)
- [x] Create `duckdb_roles` table on extension load
- [x] Create `duckdb_role_members` table
- [x] Create `duckdb_table_privileges` table
- [x] Create `duckdb_column_privileges` table
- [x] Create `duckdb_row_policies` table

#### 1.5 Register Extension Hooks
- [x] Register `OptimizerExtension` *(done in spikes - has spike test logic)*
- [x] Register `ParserExtension` *(done in spikes - has spike test logic)*

### Acceptance Criteria

```
test/sql/rbac/00_extension_load.test - ALL PASS
```

- [x] Extension loads successfully
- [x] `rbac_current_user()` returns a value
- [x] `rbac_current_roles()` returns a value  
- [x] System tables exist and are queryable
- [x] Default session is superuser (can do anything)
- [x] `rbac_set_identity()` can change session identity (once only, immutable)

---

## Phase 2: DDL Parsing

**Goal:** Parse all RBAC DDL statements (CREATE ROLE, GRANT, etc.)

**Status:** ✅ Complete | 50 assertions  
**Dependencies:** Phase 1 completed  
**Test Files:** `test/sql/rbac/01_roles.test`

### Tasks

#### 2.1 Parser Extension Framework
- [x] Implement `ParserExtension::parse_function`
- [x] Detect RBAC keywords: CREATE ROLE, DROP ROLE, GRANT, REVOKE, CREATE ROW POLICY, DROP ROW POLICY
- [x] Return `ParserExtensionParseData` with parsed statement info
- [x] Implement `ParserExtension::plan_function` to return TableFunction

#### 2.2 CREATE ROLE / DROP ROLE
- [x] Parse `CREATE ROLE role_name`
- [x] Parse `DROP ROLE role_name`
- [x] TableFunction: INSERT into `duckdb_roles`
- [x] TableFunction: DELETE from `duckdb_roles`
- [x] Validation: Role doesn't already exist (CREATE)
- [x] Validation: Role exists (DROP)
- [x] Validation: Role has no grants (DROP) - FR-3

#### 2.3 GRANT/REVOKE Role Membership
- [x] Parse `GRANT role_name TO member_role`
- [x] Parse `REVOKE role_name FROM member_role`
- [x] TableFunction: INSERT/DELETE from `duckdb_role_members`
- [x] Validation: Both roles exist

#### 2.4 GRANT/REVOKE SELECT ON Table
- [x] Parse `GRANT SELECT ON table_name TO role_name`
- [x] Parse `REVOKE SELECT ON table_name FROM role_name`
- [x] Parse with schema prefix: `main.table_name`
- [x] TableFunction: INSERT/DELETE from `duckdb_table_privileges`
- [x] Validation: Table exists (FR-8)
- [x] Validation: Role exists

#### 2.5 GRANT/REVOKE SELECT (columns) ON Table
- [x] Parse `GRANT SELECT (col1, col2) ON table_name TO role_name`
- [x] Parse `REVOKE SELECT (col1) ON table_name FROM role_name`
- [x] TableFunction: INSERT/DELETE from `duckdb_column_privileges`
- [x] Validation: Table exists
- [x] Validation: Columns exist
- [x] Validation: Role exists

#### 2.6 CREATE/DROP ROW POLICY
- [x] Parse `CREATE ROW POLICY name ON table FOR SELECT USING (expr) TO role`
- [x] Parse `DROP ROW POLICY name ON table`
- [x] TableFunction: INSERT/DELETE from `duckdb_row_policies`
- [x] Validation: Table exists
- [x] Validation: Role exists
- [x] Validation: Policy name unique per table

### Acceptance Criteria

```
test/sql/rbac/01_roles.test - ALL PASS (50 assertions)
```

- [x] All RBAC DDL statements parse correctly
- [x] Grants are stored in system tables
- [x] Validation errors are clear and helpful
- [x] Case insensitivity works (FR-5)

---

## Phase 3: Storage & Grants Management

**Goal:** Grants correctly stored, retrieved, and visible via introspection.

**Status:** ✅ Complete | 142 assertions (33+31+51+27)  
**Dependencies:** Phase 2 completed  
**Test Files:** `02_table_privileges.test`, `03_column_privileges.test`, `06_introspection.test`, `06b_effective_roles.test`

### Tasks

#### 3.1 Grant Storage
- [x] Table grants stored with (grantee, table_name, privilege)
- [x] Column grants stored with (grantee, table_name, column_name)
- [x] Grants survive across queries (persistent in tables)
- [x] Grants are idempotent (duplicate grant = no error)

#### 3.2 Name-Based Grant Persistence (FR-9)
- [x] Grants reference tables by name, not OID
- [x] Grants survive table DROP/RECREATE cycle
- [x] Orphaned grants remain in base tables
- [x] Test: Drop table, recreate with same name, grant still applies

#### 3.3 Effective Roles Computation
- [x] Implement `RBACState::GetEffectiveRoles()` function
- [x] Include direct roles from session
- [x] Include inherited roles from `duckdb_role_members`
- [x] `rbac_effective_roles()` scalar function

#### 3.4 Introspection Views
- [x] Create `duckdb_effective_privileges` view
  - Shows current user's accessible tables/columns
  - Filters out orphaned grants (tables that don't exist)
- [x] Create `duckdb_my_roles` view
  - Shows current user's effective roles

### Acceptance Criteria

```
test/sql/rbac/02_table_privileges.test - ALL PASS (33 assertions)
test/sql/rbac/03_column_privileges.test - ALL PASS (31 assertions)
test/sql/rbac/06_introspection.test - ALL PASS (51 assertions)
test/sql/rbac/06b_effective_roles.test - ALL PASS (27 assertions)
```

- [x] Grants stored correctly in system tables
- [x] Grants survive DDL operations
- [x] Introspection views return correct data
- [x] Inherited permissions work via role membership

---

## Phase 4: Permission Enforcement

**Goal:** Queries fail without proper grants. This is the core security enforcement.

**Duration:** 3 days  
**Dependencies:** Phase 3 completed  
**Test File:** `test/sql/rbac/05_enforcement.test`

### Tasks

#### 4.1 Optimizer Extension Framework
- [ ] Implement `OptimizerExtension::pre_optimize_function`
- [ ] Get `RBACState` from `ClientContext`
- [ ] Early exit if `is_superuser = true`

#### 4.2 Plan Walking
- [ ] Walk `LogicalOperator` tree recursively
- [ ] Find all `LogicalGet` nodes (table scans)
- [ ] Extract table identity and referenced columns from each `LogicalGet`
  - Use `LogicalGet::GetTable()` when available (base tables); otherwise handle gracefully (table functions / non-table scans)
  - Treat column ids as `ColumnIndex` (not plain integers) and account for rowid/virtual/nested paths
  - **Note (why change):** In this DuckDB version `LogicalGet` stores `vector<ColumnIndex>`, and `GetTable()` can return null for non-catalog scans. Enforcement must not assume a simple `(table_name, vector<idx_t>)` model.

#### 4.3 Table Access Check
- [ ] For each table, check `duckdb_table_privileges`
- [ ] Query: Does any effective role have SELECT on this table?
- [ ] If no: throw `PermissionException` with clear message
- [ ] Message format: `"User 'X' lacks SELECT privilege on table 'Y'"`

#### 4.4 Column Access Check
- [ ] Compute allowed columns for (table, effective_roles)
- [ ] If table-level grant exists and NO column grants: all columns allowed
- [ ] If column grants exist: only those columns allowed
- [ ] Check every referenced column in `LogicalGet` (via `GetColumnIds()`/`GetColumnName(ColumnIndex)`)
- [ ] If forbidden column: throw `PermissionException`
- [ ] Message format: `"User 'X' lacks SELECT privilege on column 'Y' of table 'Z'"`

#### 4.5 Column References in Expressions
- [ ] Check columns in WHERE clause expressions
- [ ] Check columns in ORDER BY
- [ ] Check columns in GROUP BY
- [ ] Check columns in HAVING
- [ ] Check columns in JOIN conditions
- [ ] Check columns in computed expressions

#### 4.6 Superuser Bypass (FR-21)
- [ ] Superuser flag bypasses ALL permission checks
- [ ] No table checks, no column checks, no row policies
- [ ] Default identity (no init) = superuser

### Acceptance Criteria

```
test/sql/rbac/05_enforcement.test - ALL PASS
```

- [ ] Queries on tables without grants fail
- [ ] Queries on forbidden columns fail
- [ ] Column references in any clause are checked
- [ ] Superuser bypasses all checks
- [ ] Error messages are clear and include context

---

## Phase 5: Row Policies

**Goal:** Row-level security filters query results.

**Duration:** 2 days  
**Dependencies:** Phase 4 completed  
**Test File:** `test/sql/rbac/04_row_policies.test`

### Tasks

#### 5.1 Policy Lookup
- [ ] Query `duckdb_row_policies` for (table, effective_roles)
- [ ] Return list of applicable policies

#### 5.2 Policy Combination (FR-15)
- [ ] Multiple policies for same (table, role) OR'd together
- [ ] Multiple roles with policies OR'd together
- [ ] Build combined filter expression

#### 5.3 Expression Binding
- [ ] Parse policy `filter_expression` text at query time
- [ ] Bind against the current table scan so column references resolve correctly
- [ ] Resolve column references
- [ ] Handle `current_user()` function in expressions
  - **Note (why change):** Expression parsing is available (`Parser::ParseExpressionList`), but binding must produce a correct bound `Expression` over the `LogicalGet`’s bindings. This is why Spike 0.5 exists.

#### 5.4 Filter Injection
- [ ] Create `LogicalFilter` node with bound expression
- [ ] Insert filter above `LogicalGet` in plan
- [ ] Verify filter is applied before other operations

#### 5.5 Policy Expression Privileges (FR-16)
- [ ] Policy expressions run with elevated privileges
- [ ] Can reference columns user cannot SELECT
- [ ] This enables patterns like `USING (secret_level <= 2)`

#### 5.6 No Policy = No Filter (FR-18)
- [ ] If user has no applicable policies: see all rows
- [ ] Policies are opt-in restrictions

### Acceptance Criteria

```
test/sql/rbac/04_row_policies.test - ALL PASS
```

- [ ] Row policies filter query results
- [ ] Multiple policies OR together correctly
- [ ] `current_user()` works in policy expressions
- [ ] Policy expressions can reference any column
- [ ] No policy = all rows visible

---

## Phase 6: Polish & Edge Cases

**Goal:** Handle complex queries, improve error messages, complete introspection.

**Duration:** 2 days  
**Dependencies:** Phase 5 completed  
**Test Files:** `test/sql/rbac/07_complex_queries.test`, `test/sql/rbac/08_error_messages.test`

### Tasks

#### 6.1 Complex Query Support
- [ ] JOINs: Check permissions on ALL tables
- [ ] JOINs: Apply row policies to each table independently
- [ ] Self-joins: Same policy applies to all aliases
- [ ] Subqueries: Check permissions on subquery tables
- [ ] CTEs: Check permissions on CTE source tables
- [ ] Recursive CTEs: Policy applies to all references
- [ ] UNION/INTERSECT/EXCEPT: Check all branches

#### 6.2 Aggregations with Policies
- [ ] Aggregates compute on filtered rows
- [ ] COUNT(*) counts only visible rows
- [ ] SUM/AVG/etc. aggregate only visible data

#### 6.3 EXPLAIN Support
- [ ] EXPLAIN requires same permissions as the query
- [ ] Don't leak information via EXPLAIN

#### 6.4 Error Message Quality
- [ ] All errors include user name
- [ ] All errors include table name
- [ ] Column errors include column name
- [ ] Format: `"User 'X' lacks SELECT privilege on column 'Y' of table 'Z'"`

#### 6.5 Edge Cases
- [ ] Empty tables with policies
- [ ] NULL handling in policy expressions
- [ ] Very long policy expressions
- [ ] Many policies on same table
- [ ] User with many roles

### Acceptance Criteria

```
test/sql/rbac/07_complex_queries.test - ALL PASS
test/sql/rbac/08_error_messages.test - ALL PASS
```

- [ ] All complex query patterns work correctly
- [ ] Error messages are helpful and consistent
- [ ] No edge case crashes

---

## Final Checklist

When all phases complete:

- [ ] All test files pass:
  - [ ] `00_extension_load.test`
  - [ ] `01_roles.test`
  - [ ] `02_table_privileges.test`
  - [ ] `03_column_privileges.test`
  - [ ] `04_row_policies.test`
  - [ ] `05_enforcement.test`
  - [ ] `06_introspection.test`
  - [ ] `07_complex_queries.test`
  - [ ] `08_error_messages.test`
  - [ ] `09_ddl_syntax.test`

- [ ] Run `make test` - all tests pass
- [ ] Manual smoke test with sample data
- [ ] Code review / cleanup
- [ ] Update documentation if needed

---

## Quick Reference: File Locations

| Component | Location |
|-----------|----------|
| Extension source | `src/` |
| Test files | `test/sql/rbac/` |
| Design docs | `docs/sdlc/` |
| DuckDB source | `duckdb/` |

---

## Running Tests

```bash
# Run all tests
make test

# Run specific test file (if supported)
./build/release/test/unittest "test/sql/rbac/01_roles.test"

# Run with debug build
make test_debug
```

---

## Document History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | Dec 2024 | Initial implementation plan |

