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

---

## Progress Summary

| Phase | Status | Tests Passing |
|-------|--------|---------------|
| Phase 0: Spikes | ⬜ Not Started | N/A |
| Phase 1: Foundation | ⬜ Not Started | 0/6 |
| Phase 2: DDL Parsing | ⬜ Not Started | 0/31 |
| Phase 3: Storage & Grants | ⬜ Not Started | 0/20 |
| Phase 4: Enforcement | ⬜ Not Started | 0/15 |
| Phase 5: Row Policies | ⬜ Not Started | 0/16 |
| Phase 6: Polish | ⬜ Not Started | 0/44 |

**Overall:** 0/132 tests passing

---

## Phase 0: Validation Spikes

**Goal:** Prove DuckDB extension hooks work as expected before committing to implementation.

**Duration:** 1 day  
**Dependencies:** None

### Recommended Execution Order

Execute spikes in this order (each builds on the previous):

1. **Spike 0.4** (ClientContextState) → easiest, no parser/optimizer hooks yet
2. **Spike 0.2** (OptimizerExtension throws) → introduces optimizer hook
3. **Spike 0.3** (OptimizerExtension injects filter) → extends optimizer work
4. **Spike 0.1** (ParserExtension catches DDL) → separate subsystem
5. **Spike 0.5** (Expression binding) → hardest, builds on 0.3

### File Organization

Create spike code in `src/` (refactor to proper structure in Phase 1):

```
src/
├── include/
│   ├── quack_extension.hpp      # existing
│   └── rbac_state.hpp           # NEW: Spike 0.4 - session state
├── quack_extension.cpp          # existing - modify to register hooks
├── rbac_state.cpp               # NEW: Spike 0.4 implementation
├── rbac_optimizer.cpp           # NEW: Spikes 0.2, 0.3, 0.5
└── rbac_parser.cpp              # NEW: Spike 0.1
```

### Test Commands

```bash
# Build after changes
just build

# Run spike tests (create test/sql/rbac/00_spikes.test)
./build/debug/test/unittest "test/sql/rbac/00_spikes.test"

# Interactive testing (useful for debugging)
just run
# Then in DuckDB shell:
#   LOAD 'quack';
#   <run test queries>
```

### Tasks

---

#### Spike 0.4: ClientContextState stores identity

**Implementation:**
1. Create `RBACState : ClientContextState` with `user_name`, `roles`, `is_superuser`
2. Create `RBACExtensionCallback : ExtensionCallback` with `OnConnectionOpened`
3. In `LoadInternal()`, register callback via `DBConfig::GetConfig(db).extension_callbacks.push_back(...)`
4. Create scalar function `rbac_current_user()` that retrieves state from `context.registered_state->Get<RBACState>("rbac")`

**Test SQL:**
```sql
LOAD 'quack';
SELECT rbac_current_user();  
-- Expected: 'default_user' or hardcoded value
```

**Note (why this matters):** DuckDB provides `ClientContext::registered_state` (`RegisteredStateManager`) and connection lifecycle callbacks (`ExtensionCallback::OnConnectionOpened/Closed`) that we can use to ensure state exists per connection.

- [ ] Spike 0.4 complete

---

#### Spike 0.2: OptimizerExtension can throw exception

**Implementation:**
1. Create `RBACOptimizerExtension` with `pre_optimize_function` set
2. Walk plan looking for `LogicalGet` nodes (use `LogicalOperatorType::LOGICAL_GET`)
3. If table name == "blocked", throw `PermissionException("Access denied to table 'blocked'")`
4. Register via `DBConfig::GetConfig(db).optimizer_extensions.push_back(...)`

**Test SQL:**
```sql
LOAD 'quack';
CREATE TABLE allowed (id INT);
CREATE TABLE blocked (id INT);
SELECT * FROM allowed;   -- Should succeed
SELECT * FROM blocked;   -- Should fail with "Access denied to table 'blocked'"
```

**Note (why this matters):** `pre_optimize_function` is confirmed to run before built-in optimizers and can throw; we'll use this mechanism for permission enforcement failures.

- [ ] Spike 0.2 complete

---

#### Spike 0.3: OptimizerExtension can inject LogicalFilter

**Implementation:**
1. Extend optimizer hook from Spike 0.2
2. When finding `LogicalGet` on table named "filtered_table":
   - Create a hardcoded `BoundComparisonExpression` for `id > 0`
   - Create `LogicalFilter` with that expression
   - Insert filter between `LogicalGet` and its parent (rewire child pointers)

**Test SQL:**
```sql
LOAD 'quack';
CREATE TABLE filtered_table (id INT);
INSERT INTO filtered_table VALUES (-1), (0), (1), (2);
SELECT * FROM filtered_table;
-- Expected: only rows where id > 0, i.e., (1), (2)
```

**Note (why this matters):** `LogicalFilter` exists and plan mutation is supported; we'll reuse this for row policies, but binding a correct filter expression is the hard part (see Spike 0.5).

- [ ] Spike 0.3 complete

---

#### Spike 0.1: ParserExtension catches custom DDL

**Implementation:**
1. Create parse function that checks if query starts with `CREATE ROLE` (case-insensitive)
2. Extract role name, return `ParserExtensionParseData` subclass with it
3. Create plan function that returns a `TableFunction` which prints/returns "Role created: <name>"
4. Register via `DBConfig::GetConfig(db).parser_extensions.push_back(...)`

**Test SQL:**
```sql
LOAD 'quack';
CREATE ROLE test_role;
-- Expected: returns result like "Role created: test_role"
-- (DuckDB's native parser doesn't recognize CREATE ROLE, so our extension catches it)
```

**Note (why this matters):** In DuckDB, `ParserExtension::parse_function` is only invoked for statements DuckDB fails to parse, so this spike confirms interception works for RBAC DDL and clarifies the compatibility risk if DuckDB adds native ROLE/GRANT syntax later.

- [ ] Spike 0.1 complete

---

#### Spike 0.5: Bind a policy expression against a table scan

**Implementation:**
1. Parse expression string using `Parser::ParseExpressionList("id > 0")`
2. In optimizer hook, find base-table `LogicalGet` (where `GetTable() != nullptr`)
3. Use `input.optimizer.binder` to create expression binder
4. Bind parsed expression against `LogicalGet`'s column bindings
5. Inject bound expression via `LogicalFilter`

**Test SQL:**
```sql
LOAD 'quack';
CREATE TABLE policy_test (id INT, name VARCHAR);
INSERT INTO policy_test VALUES (1, 'visible'), (0, 'hidden'), (2, 'also_visible');
SELECT * FROM policy_test;
-- Expected: only rows where id > 0, i.e., (1, 'visible'), (2, 'also_visible')
-- This proves we can dynamically bind and inject policy expressions
```

**Note (why this matters):** Row policy parsing is easy, but *binding* to correct `ColumnBinding`/scope during optimizer-time rewriting is the highest-risk integration point; we want this proven early.

- [ ] Spike 0.5 complete

---

### Spike Test File

Create `test/sql/rbac/00_spikes.test`:

```sql
# name: test/sql/rbac/00_spikes.test
# description: Validation spikes for RBAC extension hooks
# group: [rbac]

require quack

# Spike 0.4: Session state
query I
SELECT rbac_current_user()
----
default_user

# Spike 0.2: Optimizer throws on blocked table
statement ok
CREATE TABLE allowed (id INT)

statement ok
CREATE TABLE blocked (id INT)

statement ok
SELECT * FROM allowed

statement error
SELECT * FROM blocked
----
Access denied

# Spike 0.3: Filter injection
statement ok
CREATE TABLE filtered_table (id INT)

statement ok
INSERT INTO filtered_table VALUES (-1), (0), (1), (2)

query I
SELECT * FROM filtered_table ORDER BY id
----
1
2

# Spike 0.1: Parser catches CREATE ROLE
query I
CREATE ROLE test_role
----
Role created: test_role

# Spike 0.5: Expression binding (uses policy_test table)
statement ok
CREATE TABLE policy_test (id INT, name VARCHAR)

statement ok
INSERT INTO policy_test VALUES (1, 'visible'), (0, 'hidden'), (2, 'also_visible')

query IT
SELECT * FROM policy_test ORDER BY id
----
1	visible
2	also_visible
```

### Acceptance Criteria

- [ ] All 5 spikes demonstrate expected behavior
- [ ] `test/sql/rbac/00_spikes.test` passes
- [ ] Document any surprises or limitations discovered
- [ ] Decision: Proceed with implementation or adjust approach

---

## Phase 1: Foundation

**Goal:** Extension skeleton with identity management and system tables.

**Duration:** 2 days  
**Dependencies:** Phase 0 completed  
**Test File:** `test/sql/rbac/00_extension_load.test`

### Tasks

#### 1.1 Extension Entry Point
- [ ] Create extension entry point (`DUCKDB_CPP_EXTENSION_ENTRY`)
- [ ] Extension loads without error via `LOAD 'rbac'` or `require rbac`
- [ ] Register extension in DuckDB's extension system

#### 1.2 Session State Management
- [ ] Implement `RBACState : ClientContextState`
  ```cpp
  struct RBACState : ClientContextState {
      string user_name;
      vector<string> roles;
      bool is_superuser = true;  // Default to superuser for backwards compat
      bool initialized = false;
  };
  ```
- [ ] Register `ExtensionCallback` for connection lifecycle
- [ ] Install `RBACState` in `ClientContext::registered_state` on connection open
- [ ] Implement mechanism to set identity (connection properties or init function)

#### 1.3 Scalar Functions
- [ ] Register `current_user()` ScalarFunction
  - Returns `RBACState.user_name`
  - Returns default value if not initialized
- [ ] Register `current_roles()` ScalarFunction
  - Returns `RBACState.roles` as LIST

#### 1.4 System Tables (Schema)
- [ ] Create `duckdb_roles` table on extension load
  ```sql
  CREATE TABLE IF NOT EXISTS duckdb_roles (
      role_name VARCHAR PRIMARY KEY,
      created_at TIMESTAMP DEFAULT current_timestamp,
      description VARCHAR
  );
  ```
- [ ] Create `duckdb_role_members` table
- [ ] Create `duckdb_table_privileges` table
- [ ] Create `duckdb_column_privileges` table
- [ ] Create `duckdb_row_policies` table

#### 1.5 Register Extension Hooks (Empty)
- [ ] Register `OptimizerExtension` (empty implementation for now)
- [ ] Register `ParserExtension` (empty implementation for now)

### Acceptance Criteria

```
test/sql/rbac/00_extension_load.test - ALL PASS
```

- [ ] Extension loads successfully
- [ ] `current_user()` returns a value
- [ ] `current_roles()` returns a value  
- [ ] System tables exist and are queryable
- [ ] Default session is superuser (can do anything)

---

## Phase 2: DDL Parsing

**Goal:** Parse all RBAC DDL statements (CREATE ROLE, GRANT, etc.)

**Duration:** 3 days  
**Dependencies:** Phase 1 completed  
**Test Files:** `test/sql/rbac/01_roles.test`, `test/sql/rbac/09_ddl_syntax.test`

### Tasks

#### 2.1 Parser Extension Framework
- [ ] Implement `ParserExtension::parse_function`
- [ ] Detect RBAC keywords: CREATE ROLE, DROP ROLE, GRANT, REVOKE, CREATE ROW POLICY, DROP ROW POLICY
- [ ] Return `ParserExtensionParseData` with parsed statement info
- [ ] Implement `ParserExtension::plan_function` to return TableFunction
  - **Note (why change):** `plan_function` must return `ParserExtensionPlanResult` (TableFunction + parameters + statement properties). We should also set `modified_databases` and `return_type` appropriately so DuckDB correctly treats these as modifying statements where applicable.

#### 2.2 CREATE ROLE / DROP ROLE
- [ ] Parse `CREATE ROLE role_name`
- [ ] Parse `DROP ROLE role_name`
- [ ] TableFunction: INSERT into `duckdb_roles`
- [ ] TableFunction: DELETE from `duckdb_roles`
- [ ] Validation: Role doesn't already exist (CREATE)
- [ ] Validation: Role exists (DROP)
- [ ] Validation: Role has no grants (DROP) - FR-3

#### 2.3 GRANT/REVOKE Role Membership
- [ ] Parse `GRANT role_name TO member_role`
- [ ] Parse `REVOKE role_name FROM member_role`
- [ ] TableFunction: INSERT/DELETE from `duckdb_role_members`
- [ ] Validation: Both roles exist

#### 2.4 GRANT/REVOKE SELECT ON Table
- [ ] Parse `GRANT SELECT ON table_name TO role_name`
- [ ] Parse `REVOKE SELECT ON table_name FROM role_name`
- [ ] Parse with schema prefix: `main.table_name`
- [ ] TableFunction: INSERT/DELETE from `duckdb_table_privileges`
- [ ] Validation: Table exists (FR-8)
- [ ] Validation: Role exists

#### 2.5 GRANT/REVOKE SELECT (columns) ON Table
- [ ] Parse `GRANT SELECT (col1, col2) ON table_name TO role_name`
- [ ] Parse `REVOKE SELECT (col1) ON table_name FROM role_name`
- [ ] TableFunction: INSERT/DELETE from `duckdb_column_privileges`
- [ ] Validation: Table exists
- [ ] Validation: Columns exist
- [ ] Validation: Role exists

#### 2.6 CREATE/DROP ROW POLICY
- [ ] Parse `CREATE ROW POLICY name ON table FOR SELECT USING (expr) TO role`
- [ ] Parse `DROP ROW POLICY name ON table`
- [ ] TableFunction: INSERT/DELETE from `duckdb_row_policies`
- [ ] Validation: Table exists
- [ ] Validation: Role exists
- [ ] Validation: Expression is valid (columns exist)
- [ ] Validation: Policy name unique per table

### Acceptance Criteria

```
test/sql/rbac/01_roles.test - ALL PASS
test/sql/rbac/09_ddl_syntax.test - ALL PASS
```

- [ ] All RBAC DDL statements parse correctly
- [ ] Grants are stored in system tables
- [ ] Validation errors are clear and helpful
- [ ] Case insensitivity works (FR-5)

---

## Phase 3: Storage & Grants Management

**Goal:** Grants correctly stored, retrieved, and visible via introspection.

**Duration:** 2 days  
**Dependencies:** Phase 2 completed  
**Test Files:** `test/sql/rbac/02_table_privileges.test`, `test/sql/rbac/03_column_privileges.test`, `test/sql/rbac/06_introspection.test`

### Tasks

#### 3.1 Grant Storage
- [ ] Table grants stored with (grantee, table_name, privilege)
- [ ] Column grants stored with (grantee, table_name, column_name)
- [ ] Grants survive across queries (persistent in tables)
- [ ] Grants are idempotent (duplicate grant = no error)

#### 3.2 Name-Based Grant Persistence (FR-9)
- [ ] Grants reference tables by name, not OID
- [ ] Grants survive table DROP/RECREATE cycle
- [ ] Orphaned grants remain in base tables
- [ ] Test: Drop table, recreate with same name, grant still applies

#### 3.3 Effective Roles Computation
- [ ] Implement `get_effective_roles(session_roles)` function
- [ ] Include direct roles from session
- [ ] Include inherited roles from `duckdb_role_members`
- [ ] Cache result in `RBACState` for performance

#### 3.4 Introspection Views
- [ ] Create `duckdb_effective_privileges` view
  - Shows current user's accessible tables/columns
  - Filters out orphaned grants (tables that don't exist)
- [ ] Create `duckdb_my_roles` view
  - Shows current user's effective roles

### Acceptance Criteria

```
test/sql/rbac/02_table_privileges.test - ALL PASS
test/sql/rbac/03_column_privileges.test - ALL PASS  
test/sql/rbac/06_introspection.test - ALL PASS
```

- [ ] Grants stored correctly in system tables
- [ ] Grants survive DDL operations
- [ ] Introspection views return correct data
- [ ] Inherited permissions work via role membership

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

