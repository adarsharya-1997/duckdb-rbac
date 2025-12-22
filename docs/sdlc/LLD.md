# DuckDB RBAC Extension: Technical Design Document

**Version:** 1.1 (MVP - Updated with DuckDB Extension Reality)  
**Last Updated:** December 2024  
**Reference:** See `QnA.md` for full requirements Q&A (112 questions)  
**Extension API Reference:** See `RFC.md` for DuckDB extension approach

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture](#2-architecture)
3. [Data Model](#3-data-model)
4. [SQL Syntax Specification](#4-sql-syntax-specification)
5. [Permission Check Algorithm](#5-permission-check-algorithm)
6. [Row Policy Evaluation](#6-row-policy-evaluation)
7. [Column Access Control](#7-column-access-control)
8. [Session Identity Management](#8-session-identity-management)
9. [Introspection System](#9-introspection-system)
10. [Error Handling](#10-error-handling)
11. [Implementation Phases](#11-implementation-phases)
12. [Test Scenarios](#12-test-scenarios)
13. [Future Considerations (v2)](#13-future-considerations-v2)
14. [Appendix: ClickHouse/PostgreSQL Comparison](#14-appendix-clickhousepostgresql-comparison)

---

## 1. Overview

### 1.1 Purpose

Implement Role-Based Access Control (RBAC) as a DuckDB extension, providing:

- **Table-level privileges**: Control which roles can SELECT from which tables
- **Column-level privileges**: Control which columns are visible to each role
- **Row-level security (RLS)**: Filter rows based on policy expressions per role

### 1.2 Design Principles

| Principle | Description |
|-----------|-------------|
| **Minimal invasion** | Work as an extension, not core modification |
| **Name-based grants** | Grants reference table/column names, surviving DDL changes |
| **External authentication** | Extension receives identity; doesn't manage credentials |
| **Explicit grants** | Roles and tables must exist before granting |
| **Fail-safe** | Default deny for tables; detailed error messages |
| **Open-source ready** | Generic design, not tied to specific applications |

### 1.3 Scope Summary

**In Scope (MVP):**
- CREATE/DROP ROLE
- Role membership (flat, no hierarchy)
- GRANT/REVOKE SELECT on tables
- GRANT/REVOKE SELECT on columns
- CREATE/DROP ROW POLICY with USING clause
- System tables for introspection
- current_user() function

**Out of Scope (v2):**
- INSERT/UPDATE/DELETE privileges
- DDL privileges
- Role hierarchy
- GRANT OPTION / ADMIN OPTION
- Restrictive (AND) policies
- Partial revokes
- Wildcard grants
- Column masking
- Views

---

## 2. Architecture

### 2.1 DuckDB Extension Hooks (Actual)

Based on analysis of DuckDB's extension API, here are the **actual hooks** we will use:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              DuckDB Core Pipeline                           │
│                                                                             │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐  │
│  │   Parser    │───▶│   Binder    │───▶│  Optimizer  │───▶│  Executor   │  │
│  └──────┬──────┘    └─────────────┘    └──────┬──────┘    └─────────────┘  │
│         │                                      │                            │
│         ▼                                      ▼                            │
│  ┌──────────────┐                      ┌──────────────┐                    │
│  │ ParserExt    │                      │ OptimizerExt │                    │
│  │ parse_func   │                      │ pre_optimize │                    │
│  │ (fallback)   │                      │              │                    │
│  └──────────────┘                      └──────────────┘                    │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                     RBAC Extension Components                        │  │
│  │                                                                      │  │
│  │  ParserExtension::parse_function                                    │  │
│  │    → Catches: CREATE ROLE, DROP ROLE, GRANT, REVOKE, CREATE POLICY  │  │
│  │    → Returns: TableFunction that executes the DDL                   │  │
│  │                                                                      │  │
│  │  OptimizerExtension::pre_optimize_function                          │  │
│  │    → Walks LogicalOperator tree                                     │  │
│  │    → Checks table/column access on LogicalGet nodes                 │  │
│  │    → Injects LogicalFilter for row policies                         │  │
│  │    → Throws exception on permission denied                          │  │
│  │                                                                      │  │
│  │  ExtensionCallback::OnConnectionOpened                              │  │
│  │    → Installs RBACState in ClientContext::registered_state          │  │
│  │    → Stores: user_name, roles, is_superuser                         │  │
│  │                                                                      │  │
│  │  ScalarFunction: current_user()                                     │  │
│  │    → Returns session's user name from RBACState                     │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                        RBAC Storage (DuckDB Tables)                  │  │
│  │                                                                      │  │
│  │  duckdb_roles │ duckdb_role_members │ duckdb_table_privileges │ ... │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Hook-to-Requirement Mapping

| Requirement | DuckDB Hook | Notes |
|-------------|-------------|-------|
| Custom DDL (CREATE ROLE, GRANT, etc.) | `ParserExtension::parse_function` | Fallback when DuckDB parser fails |
| Per-connection identity | `ExtensionCallback` + `ClientContextState` | Stored in `registered_state` |
| Table access check | `OptimizerExtension::pre_optimize_function` | Walk plan, find LogicalGet |
| Column access check | `OptimizerExtension::pre_optimize_function` | Check column_ids in LogicalGet |
| Row filter injection | `OptimizerExtension::pre_optimize_function` | Wrap with LogicalFilter |
| `current_user()` function | `ExtensionLoader::RegisterFunction` | ScalarFunction |
| System tables | Execute CREATE TABLE from extension | Standard DuckDB tables |

### 2.3 MVP Limitation: SELECT * Behavior

**Important:** DuckDB expands `SELECT *` to explicit columns during binding, BEFORE our optimizer hook fires.

**MVP Behavior:**
- `SELECT *` with forbidden columns → **ERROR** (not silent filter)
- `SELECT explicit_col` with forbidden column → **ERROR**
- Both are treated identically (we cannot distinguish them)

**User Workaround:**
```sql
-- Check allowed columns first
SELECT * FROM duckdb_effective_privileges WHERE table_name = 'orders';
-- Then query explicitly
SELECT id, customer, amount FROM orders;
```

**Phase 2:** Silent `SELECT *` filtering via parser override or minimal core change.

### 2.4 Key Components

| Component | DuckDB Mechanism | Responsibility |
|-----------|------------------|----------------|
| **RBACParserExtension** | `ParserExtension` | Parses CREATE ROLE, GRANT, etc. |
| **RBACOptimizerExtension** | `OptimizerExtension` | Checks permissions, injects filters |
| **RBACState** | `ClientContextState` | Per-connection identity storage |
| **RBACExtensionCallback** | `ExtensionCallback` | Installs RBACState on connection open |
| **PrivilegeChecker** | Internal class | Queries system tables for grants |
| **PolicyEvaluator** | Internal class | Combines and binds policy expressions |

### 2.5 Extension Lifecycle (Actual)

```
1. LOAD 'rbac';
   └─▶ Extension entry point called
   └─▶ Register ParserExtension in DBConfig::parser_extensions
   └─▶ Register OptimizerExtension in DBConfig::optimizer_extensions
   └─▶ Register ExtensionCallback in DBConfig::extension_callbacks
   └─▶ Register current_user() ScalarFunction
   └─▶ Create system tables if not exist (via ClientContext query)

2. Connection established
   └─▶ ExtensionCallback::OnConnectionOpened fires
   └─▶ RBACState created in context.registered_state
   └─▶ App sets identity via connection properties (user, roles, superuser)

3. Query executed (RBAC DDL)
   └─▶ DuckDB parser fails on "CREATE ROLE foo"
   └─▶ ParserExtension::parse_function catches it
   └─▶ Returns ExtensionStatement with TableFunction
   └─▶ TableFunction executes: INSERT INTO duckdb_roles...

4. Query executed (SELECT)
   └─▶ Parser produces SQLStatement
   └─▶ Binder produces LogicalOperator tree (SELECT * already expanded)
   └─▶ OptimizerExtension::pre_optimize_function fires
   └─▶   - Check superuser flag → skip if true
   └─▶   - Walk plan for LogicalGet nodes
   └─▶   - For each table: check table grant
   └─▶   - For each column: check column grant
   └─▶   - Lookup row policies, inject LogicalFilter
   └─▶   - Throw exception if permission denied
   └─▶ Built-in optimizers run
   └─▶ Executor runs

5. Connection closed
   └─▶ RBACState destroyed automatically
```

### 2.4 Superuser Bypass

```
if (session.is_superuser) {
    // Skip all RBAC checks
    return ALLOW;
}
// Proceed with normal checks
```

Superuser is determined by external flag passed at connection creation. If RBAC extension is not loaded, no checks occur (backwards compatible).

---

## 3. Data Model

### 3.1 Entity Relationship Diagram

```
┌─────────────────┐       ┌─────────────────────┐
│  duckdb_roles   │       │ duckdb_role_members │
├─────────────────┤       ├─────────────────────┤
│ role_name (PK)  │◀──────│ role_name (FK)      │
│ created_at      │       │ member_role (FK)    │──▶ duckdb_roles
│ description     │       │ granted_at          │
└─────────────────┘       └─────────────────────┘
        │
        │
        ▼
┌───────────────────────────┐     ┌─────────────────────────────┐
│ duckdb_table_privileges   │     │ duckdb_column_privileges    │
├───────────────────────────┤     ├─────────────────────────────┤
│ grantee (FK to roles)     │     │ grantee (FK to roles)       │
│ table_name                │     │ table_name                  │
│ privilege ('SELECT')      │     │ column_name                 │
│ granted_at                │     │ granted_at                  │
│ granted_by                │     │ granted_by                  │
└───────────────────────────┘     └─────────────────────────────┘


┌─────────────────────────────────┐
│ duckdb_row_policies             │
├─────────────────────────────────┤
│ policy_name                     │
│ table_name                      │
│ grantee (FK to roles)           │
│ filter_expression (TEXT)        │
│ is_permissive (BOOL, always T)  │
│ created_at                      │
│ created_by                      │
└─────────────────────────────────┘
```

### 3.2 Table Definitions

#### 3.2.1 duckdb_roles

Stores all role definitions.

```sql
CREATE TABLE duckdb_roles (
    role_name       VARCHAR PRIMARY KEY,
    created_at      TIMESTAMP DEFAULT current_timestamp,
    description     VARCHAR
);
```

**Constraints:**
- `role_name` is case-insensitive (stored lowercase)
- Must be unique

#### 3.2.2 duckdb_role_members

Stores role membership edges (user→role or role→role in future).

```sql
CREATE TABLE duckdb_role_members (
    role_name       VARCHAR NOT NULL,
    member_role     VARCHAR NOT NULL,
    granted_at      TIMESTAMP DEFAULT current_timestamp,
    granted_by      VARCHAR,
    PRIMARY KEY (role_name, member_role),
    FOREIGN KEY (role_name) REFERENCES duckdb_roles(role_name),
    FOREIGN KEY (member_role) REFERENCES duckdb_roles(role_name)
);
```

**Semantics:**
- `member_role` is a member of `role_name`
- `member_role` inherits all privileges of `role_name`
- MVP: `member_role` must be a "user role" (passed in from outside)
- v2: Can be another defined role (hierarchy)

#### 3.2.3 duckdb_table_privileges

Stores table-level grants.

```sql
CREATE TABLE duckdb_table_privileges (
    grantee         VARCHAR NOT NULL,
    table_name      VARCHAR NOT NULL,
    privilege       VARCHAR NOT NULL DEFAULT 'SELECT',
    granted_at      TIMESTAMP DEFAULT current_timestamp,
    granted_by      VARCHAR,
    PRIMARY KEY (grantee, table_name, privilege),
    FOREIGN KEY (grantee) REFERENCES duckdb_roles(role_name)
);
```

**Semantics:**
- MVP: `privilege` is always 'SELECT'
- Table-level grant means access to ALL columns (current and future)
- Grants reference table by name (survives DROP/RECREATE)

#### 3.2.4 duckdb_column_privileges

Stores column-level grants.

```sql
CREATE TABLE duckdb_column_privileges (
    grantee         VARCHAR NOT NULL,
    table_name      VARCHAR NOT NULL,
    column_name     VARCHAR NOT NULL,
    granted_at      TIMESTAMP DEFAULT current_timestamp,
    granted_by      VARCHAR,
    PRIMARY KEY (grantee, table_name, column_name),
    FOREIGN KEY (grantee) REFERENCES duckdb_roles(role_name)
);
```

**Semantics:**
- Column grants are additive (GRANT more columns = access more)
- If NO column grants exist for (grantee, table), table-level grant applies
- If ANY column grants exist, ONLY those columns are accessible

#### 3.2.5 duckdb_row_policies

Stores row-level security policies.

```sql
CREATE TABLE duckdb_row_policies (
    policy_name         VARCHAR NOT NULL,
    table_name          VARCHAR NOT NULL,
    grantee             VARCHAR NOT NULL,
    filter_expression   VARCHAR NOT NULL,
    is_permissive       BOOLEAN DEFAULT TRUE,
    created_at          TIMESTAMP DEFAULT current_timestamp,
    created_by          VARCHAR,
    PRIMARY KEY (policy_name, table_name),
    FOREIGN KEY (grantee) REFERENCES duckdb_roles(role_name)
);
```

**Semantics:**
- `filter_expression` is a SQL boolean expression
- Can reference columns of `table_name` and `current_user()` function
- No subqueries allowed in filter_expression
- Multiple policies for same (table, role) are OR'd together
- MVP: `is_permissive` is always TRUE (AND semantics deferred)

### 3.3 Name-Based Reference Design

**Critical:** Grants and policies reference tables/columns by **name**, not internal ID.

**Rationale:** Supports table refresh pattern:
```sql
DROP TABLE positions;
ALTER TABLE positions_new RENAME TO positions;
-- Grants on 'positions' survive and apply to new table
```

**Implications:**
1. Grants may become "orphaned" when tables are dropped
2. Orphaned grants don't cause errors; they just don't match
3. New tables with same name inherit existing grants
4. Introspection views filter out orphaned grants

### 3.4 Case Sensitivity

All identifiers (role names, table names, column names) are:
- Stored in **lowercase** (normalized on insert)
- Compared **case-insensitively**

This matches DuckDB's default identifier behavior.

---

## 4. SQL Syntax Specification

### 4.1 Role Management

#### CREATE ROLE

```sql
CREATE ROLE role_name;
```

**Behavior:**
- Creates a new role with the given name
- Error if role already exists
- Role name is normalized to lowercase

**Example:**
```sql
CREATE ROLE analyst;
CREATE ROLE risk_viewer;
```

#### DROP ROLE

```sql
DROP ROLE role_name;
```

**Behavior:**
- Deletes the role
- Error if role has existing grants (RESTRICT semantics)
- Must revoke all grants first

**Example:**
```sql
REVOKE ALL ON ALL TABLES FROM analyst;  -- Future: helper syntax
DROP ROLE analyst;
```

#### GRANT role TO role (Membership)

```sql
GRANT role_name TO member_role;
```

**Behavior:**
- Makes `member_role` a member of `role_name`
- `member_role` inherits all privileges of `role_name`
- Both roles must exist
- MVP: `member_role` should be a "user role" (passed from external auth)

**Example:**
```sql
CREATE ROLE data_access;
GRANT SELECT ON orders TO data_access;

CREATE ROLE alice;  -- Represents external user
GRANT data_access TO alice;
-- Now alice has SELECT on orders via data_access
```

#### REVOKE role FROM role

```sql
REVOKE role_name FROM member_role;
```

**Behavior:**
- Removes membership edge
- `member_role` no longer inherits from `role_name`

### 4.2 Table Privileges

#### GRANT SELECT ON table

```sql
GRANT SELECT ON table_name TO role_name;
```

**Behavior:**
- Grants SELECT privilege on all columns (current and future)
- Table must exist at grant time
- Role must exist

**Example:**
```sql
GRANT SELECT ON orders TO analyst;
GRANT SELECT ON customers TO analyst;
```

#### REVOKE SELECT ON table

```sql
REVOKE SELECT ON table_name FROM role_name;
```

**Behavior:**
- Removes table-level SELECT privilege
- Also removes any column-level privileges for that table

### 4.3 Column Privileges

#### GRANT SELECT (columns) ON table

```sql
GRANT SELECT (col1, col2, ...) ON table_name TO role_name;
```

**Behavior:**
- Grants SELECT on specific columns only
- Table and columns must exist
- Additive: can grant more columns in subsequent statements
- Column grants override table-level "all columns" semantics

**Example:**
```sql
GRANT SELECT (order_id, customer_id, amount) ON orders TO analyst;
GRANT SELECT (status) ON orders TO analyst;  -- Adds to existing
-- analyst can now see: order_id, customer_id, amount, status
```

#### REVOKE SELECT (columns) ON table

```sql
REVOKE SELECT (col1, col2, ...) ON table_name FROM role_name;
```

**Behavior:**
- Removes access to specific columns
- If no columns remain, user has no access to table (unless table-level grant exists)

### 4.4 Row Policies

#### CREATE ROW POLICY

```sql
CREATE ROW POLICY policy_name ON table_name
    FOR SELECT
    USING (filter_expression)
    TO role_name [, role_name, ...];
```

**Components:**
- `policy_name`: Unique name for the policy (scoped to table)
- `table_name`: Table this policy applies to (must exist)
- `FOR SELECT`: Specifies this is for read operations (MVP: always SELECT)
- `USING (expr)`: Boolean expression that filters rows
- `TO roles`: Which roles this policy applies to

**Filter Expression Rules:**
- Can reference any column of the table (even ones user can't SELECT)
- Can use `current_user()` function
- Can use literals and built-in scalar functions
- Cannot contain subqueries
- Must be a valid boolean expression

**Example:**
```sql
-- Only show rows where region matches user's region
CREATE ROW POLICY region_filter ON orders
    FOR SELECT
    USING (region = current_user_region())
    TO regional_sales;

-- Only show active records
CREATE ROW POLICY active_only ON customers
    FOR SELECT
    USING (status = 'active')
    TO standard_user;

-- Combination: user can see their own orders or public orders
CREATE ROW POLICY own_orders ON orders
    FOR SELECT
    USING (owner = current_user() OR is_public = true)
    TO analyst;
```

#### DROP ROW POLICY

```sql
DROP ROW POLICY policy_name ON table_name;
```

**Behavior:**
- Removes the policy
- Users previously restricted by this policy may now see more rows

---

## 5. Permission Check Algorithm

### 5.1 Overall Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                     Query: SELECT ... FROM t                    │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Step 1: Is user superuser?                                     │
│  └─▶ YES: Skip all checks, allow query                         │
│  └─▶ NO: Continue to Step 2                                     │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Step 2: Get user's effective roles                             │
│  └─▶ Direct roles (from session context)                        │
│  └─▶ + Inherited roles (from duckdb_role_members)              │
│  └─▶ Result: Set<RoleName>                                      │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Step 3: For each table in query, check table access            │
│  └─▶ Does any effective role have SELECT on table?              │
│  └─▶ NO: Error "Permission denied on table 't'"                 │
│  └─▶ YES: Continue to Step 4                                    │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Step 4: For each column referenced, check column access        │
│  └─▶ Compute allowed columns for user on table                  │
│  └─▶ If column not in allowed set: Error                        │
│  └─▶ (SELECT * already expanded by binder - all columns checked)│
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Step 5: Compute row filter from policies                       │
│  └─▶ Find all policies for (table, effective_roles)            │
│  └─▶ If no policies: no filter (see all rows)                  │
│  └─▶ If policies exist: OR them together                        │
│  └─▶ Inject filter into query                                   │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Step 6: Execute modified query                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 Effective Roles Computation

```python
def get_effective_roles(session_roles: Set[str]) -> Set[str]:
    """
    Compute all roles a user effectively has, including inherited.
    
    For MVP (flat model), this is simple.
    For v2 (hierarchy), this would be transitive closure.
    """
    effective = set(session_roles)
    
    # Add roles that session_roles are members of
    for role in session_roles:
        # Query: SELECT role_name FROM duckdb_role_members 
        #        WHERE member_role = :role
        inherited = query_inherited_roles(role)
        effective.update(inherited)
    
    # v2: Recurse for transitive closure
    return effective
```

### 5.3 Table Access Check

```python
def check_table_access(table_name: str, effective_roles: Set[str]) -> bool:
    """
    Check if any effective role has SELECT on the table.
    """
    # Query: SELECT 1 FROM duckdb_table_privileges
    #        WHERE table_name = :table_name
    #        AND privilege = 'SELECT'
    #        AND grantee IN :effective_roles
    #        LIMIT 1
    return exists_table_grant(table_name, effective_roles)
```

### 5.4 Column Access Computation

```python
def get_allowed_columns(table_name: str, effective_roles: Set[str]) -> Set[str]:
    """
    Compute which columns the user can access on a table.
    
    Rules:
    1. If table-level grant exists and NO column grants: all columns
    2. If column grants exist: only those columns (union across roles)
    """
    # Check for column-level grants
    column_grants = query_column_grants(table_name, effective_roles)
    
    if column_grants:
        # User has explicit column grants - return those only
        return set(column_grants)
    else:
        # No column grants - check for table-level grant
        if has_table_grant(table_name, effective_roles):
            # Table-level grant means all columns
            return get_all_columns(table_name)
        else:
            # No access at all
            return set()
```

### 5.5 Column Reference Checking

All column references in the query must be checked, not just SELECT list:

```python
def check_column_references(query_ast, table_name: str, allowed_columns: Set[str]):
    """
    Check all column references in query against allowed set.
    
    Locations to check:
    - SELECT list
    - WHERE clause
    - ORDER BY clause
    - GROUP BY clause
    - HAVING clause
    - JOIN conditions
    - Expressions (computed columns, CASE, etc.)
    """
    for column_ref in extract_all_column_refs(query_ast, table_name):
        if column_ref.lower() not in allowed_columns:
            raise PermissionError(
                f"Permission denied on column '{column_ref}' of table '{table_name}'"
            )
```

---

## 6. Row Policy Evaluation

### 6.1 Policy Lookup

```python
def get_applicable_policies(table_name: str, effective_roles: Set[str]) -> List[Policy]:
    """
    Find all row policies that apply to this (table, user) combination.
    """
    # Query: SELECT policy_name, filter_expression 
    #        FROM duckdb_row_policies
    #        WHERE table_name = :table_name
    #        AND grantee IN :effective_roles
    return query_policies(table_name, effective_roles)
```

### 6.2 Policy Combination

```python
def combine_policies(policies: List[Policy]) -> Optional[Expression]:
    """
    Combine multiple policies into a single filter expression.
    
    MVP: All policies are permissive, combined with OR.
    v2: Permissive OR'd, then AND'd with restrictive.
    """
    if not policies:
        # No policies = no filter (see all rows)
        return None
    
    if len(policies) == 1:
        return parse_expression(policies[0].filter_expression)
    
    # OR all policies together
    combined = policies[0].filter_expression
    for policy in policies[1:]:
        combined = f"({combined}) OR ({policy.filter_expression})"
    
    return parse_expression(combined)
```

### 6.3 Filter Injection

The combined filter is injected by wrapping the LogicalGet with a LogicalFilter:

```python
def inject_row_filter(plan: LogicalOperator, table_name: str, 
                      filter_text: str, context: ClientContext):
    """
    Inject row policy filter into the logical plan.
    
    Called from OptimizerExtension::pre_optimize_function.
    
    Original plan: LogicalGet(orders) -> LogicalProjection
    With policy USING (region = 'US'):
    Modified: LogicalGet(orders) -> LogicalFilter(region='US') -> LogicalProjection
    """
    for logical_get in find_logical_gets(plan, table_name):
        # Parse and bind the filter expression at query time
        # (Policy stored as text, bound against current table schema)
        filter_expr = parse_and_bind_expression(filter_text, table_name, context)
        
        # Create LogicalFilter node
        filter_node = LogicalFilter(filter_expr)
        
        # Insert between LogicalGet and its parent
        insert_filter_above(logical_get, filter_node)
```

### 6.4 Policy Expression Binding (Query-Time)

**Design Decision:** Policy expressions are stored as text and bound at query time.

```python
def parse_and_bind_expression(filter_text: str, table_name: str, 
                               context: ClientContext) -> Expression:
    """
    Parse policy filter text and bind against current table schema.
    
    Why query-time binding:
    - Handles schema evolution (columns added/renamed)
    - Matches PostgreSQL/ClickHouse behavior
    - Simpler to implement (no serialized expression format)
    
    The expression is bound with table's column bindings from the LogicalGet.
    """
    # Parse the SQL expression
    parsed = parse_expression(filter_text)
    
    # Bind against table schema (resolve column references)
    bound = bind_expression(parsed, table_name, context)
    
    return bound
```

### 6.4 Policy Expression Execution Context

Policy expressions run with **elevated privileges** (security definer):

- Can reference any column of the table
- Even columns the user cannot SELECT
- This allows policies like `USING (secret_level <= user_clearance())`

### 6.5 current_user() Function

The extension provides a `current_user()` scalar function:

```python
def current_user() -> str:
    """Returns the current session's user name."""
    return session_context.user_name
```

Can be used in policy expressions:
```sql
CREATE ROW POLICY own_data ON records
    USING (owner = current_user())
    TO all_users;
```

---

## 7. Column Access Control

### 7.1 MVP Limitation: No Silent SELECT * Filtering

**Important:** In MVP, we **cannot** silently filter `SELECT *` to allowed columns.

**Why:** DuckDB expands `SELECT *` during binding (before our optimizer hook). By the time we see the plan, it's already `SELECT col1, col2, col3, forbidden_col`.

**MVP Behavior:**
```sql
-- Table: orders (id, customer, amount, secret_notes)
-- User has access to: (id, customer, amount)

SELECT * FROM orders;
-- ERROR: Permission denied on column 'secret_notes' of table 'orders'
-- (Because binding expanded * to include secret_notes)

SELECT id, customer, amount FROM orders;
-- SUCCESS: Returns 3 columns
```

**Phase 2 Solution Options:**
1. Parser override to rewrite `SELECT *` before binding
2. Minimal DuckDB core change to add column-access callback during binding

### 7.2 Column Reference Validation (MVP)

All column references in the query are checked against allowed set:

```python
def check_column_access(plan: LogicalOperator, table_name: str, 
                        allowed_columns: Set[str], effective_roles: Set[str]):
    """
    Check all columns referenced for a table in the plan.
    Called from OptimizerExtension::pre_optimize_function.
    
    The plan already has binding completed, so we see actual column IDs.
    """
    for logical_get in find_logical_gets(plan, table_name):
        # Get columns from the scan
        for col_id in logical_get.column_ids:
            col_name = get_column_name(table_name, col_id)
            if col_name.lower() not in allowed_columns:
                raise PermissionException(
                    f"User lacks SELECT privilege on column '{col_name}' "
                    f"of table '{table_name}'"
                )
```

### 7.3 Column Access in Different Query Parts

| Location | How We See It | Handling |
|----------|---------------|----------|
| SELECT list | In LogicalProjection | Check via column bindings |
| WHERE clause | In LogicalFilter expressions | Check expression column refs |
| ORDER BY | In LogicalOrder | Check sort column refs |
| GROUP BY | In LogicalAggregate | Check group column refs |
| HAVING | In LogicalAggregate filter | Check expression column refs |
| JOIN ON | In LogicalComparisonJoin | Check both sides |
| Expressions | In any expression tree | Walk and check all ColumnBindings |

### 7.4 Implementation Note

The optimizer extension sees `LogicalGet` nodes which contain:
- `column_ids`: Which columns the scan will read
- `projection_ids`: Which columns are actually used downstream

We check `column_ids` against the user's allowed columns.

---

## 8. Session Identity Management

### 8.1 Session Context Structure

```cpp
struct RBACSessionContext {
    std::string user_name;           // e.g., "alice"
    std::set<std::string> roles;     // e.g., {"analyst", "viewer"}
    bool is_superuser;               // Bypass all checks if true
    
    // Cached for performance
    std::set<std::string> effective_roles;  // Including inherited
    bool effective_roles_computed;
};
```

### 8.2 Identity Setting

Identity is set via connection context at connection creation time.

**Option A: Connection properties (requires DuckDB support)**
```cpp
// If DuckDB supports custom connection properties readable by extensions:
connection.SetProperty("rbac.user", "alice");
connection.SetProperty("rbac.roles", "analyst,viewer");
connection.SetProperty("rbac.superuser", "false");
```

**Option B: Initialization function (extension API)**
```cpp
// Call immediately after connection creation:
duckdb_rbac_set_identity(connection, "alice", {"analyst", "viewer"}, false);
```

**Option C: SQL-based initialization (may need protection)**
```sql
-- Called once per connection by trusted app code:
SELECT rbac_init('alice', 'analyst,viewer', false);
-- Subsequent calls should error or be ignored
```

**Note:** Exact mechanism TBD based on DuckDB capabilities. May require minimal core change.

### 8.3 Identity Immutability

Once set, identity cannot be changed:

```sql
-- After rbac_init() has been called:
SELECT rbac_init('bob', 'admin', true);  -- ERROR: identity already set

-- SET statements should not work:
SET rbac.user = 'bob';  -- ERROR or ignored
```

The extension tracks whether identity has been initialized in `RBACState`.

### 8.4 current_user() and current_roles()

Extension provides functions to query current identity:

```sql
SELECT current_user();        -- Returns: 'alice'
SELECT current_roles();       -- Returns: ['analyst', 'viewer']
```

### 8.5 Default Identity (CLI)

When running from CLI without explicit identity:
- If extension is not loaded: normal DuckDB behavior (no RBAC)
- If extension is loaded with no identity: treat as superuser (backwards compatible)

---

## 9. Introspection System

### 9.1 System Tables

These are the raw data tables (described in Section 3):

| Table | Description |
|-------|-------------|
| `duckdb_roles` | All defined roles |
| `duckdb_role_members` | Role membership edges |
| `duckdb_table_privileges` | Table-level grants |
| `duckdb_column_privileges` | Column-level grants |
| `duckdb_row_policies` | Row policy definitions |

### 9.2 System Views

Views provide filtered/computed perspectives:

#### duckdb_effective_privileges

Shows what the current user can actually access:

```sql
CREATE VIEW duckdb_effective_privileges AS
SELECT 
    tp.table_name,
    tp.privilege,
    COALESCE(
        (SELECT LIST(cp.column_name) 
         FROM duckdb_column_privileges cp 
         WHERE cp.table_name = tp.table_name 
         AND cp.grantee IN (SELECT role FROM current_effective_roles())),
        'ALL'
    ) AS columns,
    tp.grantee AS via_role
FROM duckdb_table_privileges tp
WHERE tp.grantee IN (SELECT role FROM current_effective_roles())
AND EXISTS (SELECT 1 FROM information_schema.tables t WHERE t.table_name = tp.table_name);
```

#### duckdb_my_roles

Shows current user's effective roles:

```sql
CREATE VIEW duckdb_my_roles AS
SELECT role FROM current_effective_roles();
```

#### duckdb_valid_grants

Shows only grants on existing tables (filters orphans):

```sql
CREATE VIEW duckdb_valid_grants AS
SELECT tp.* 
FROM duckdb_table_privileges tp
WHERE EXISTS (
    SELECT 1 FROM information_schema.tables t 
    WHERE t.table_name = tp.table_name
);
```

### 9.3 Introspection Examples

```sql
-- What roles exist?
SELECT * FROM duckdb_roles;

-- What can I access?
SELECT * FROM duckdb_effective_privileges;

-- What row policies affect table 'orders'?
SELECT * FROM duckdb_row_policies WHERE table_name = 'orders';

-- Who has access to table 'customers'?
SELECT grantee FROM duckdb_table_privileges WHERE table_name = 'customers';

-- What are my active roles?
SELECT * FROM duckdb_my_roles;
```

---

## 10. Error Handling

### 10.1 Error Types

| Error | Message Format | Example |
|-------|----------------|---------|
| Role not found | `Role '{role}' does not exist` | `Role 'analyst' does not exist` |
| Role exists | `Role '{role}' already exists` | `Role 'analyst' already exists` |
| Table not found | `Table '{table}' does not exist` | `Table 'orders' does not exist` |
| Column not found | `Column '{col}' does not exist in table '{table}'` | `Column 'foo' does not exist in table 'orders'` |
| Table permission denied | `User '{user}' lacks SELECT privilege on table '{table}'` | `User 'alice' lacks SELECT privilege on table 'secret'` |
| Column permission denied | `User '{user}' lacks SELECT privilege on column '{col}' of table '{table}'` | `User 'alice' lacks SELECT privilege on column 'salary' of table 'employees'` |
| Role has grants | `Cannot drop role '{role}': role has existing grants` | `Cannot drop role 'analyst': role has existing grants` |
| Invalid policy | `Invalid policy expression: {error}` | `Invalid policy expression: column 'xyz' not found` |

### 10.2 Error Context

All RBAC errors include context for debugging:

```
RBAC Error: User 'alice' lacks SELECT privilege on column 'salary' of table 'employees'.
  User: alice
  Roles: [analyst, viewer]
  Required: SELECT(salary) ON employees
```

### 10.3 Honest vs Hidden Errors

We use **honest errors** (reveal table existence):
- "Permission denied on table 'secret_project'" (reveals it exists)
- NOT "Table 'secret_project' does not exist" (would hide existence)

Rationale: Users can discover table names via information_schema anyway.

---

## 11. Implementation Phases

### Phase 0: Validation Spikes (Day 1)

**Goal:** Prove DuckDB extension hooks work as expected

| Spike | What to Test | Success Criteria |
|-------|--------------|------------------|
| **Spike 1** | ParserExtension catches `CREATE ROLE foo;` | Custom table function executes |
| **Spike 2** | OptimizerExtension can throw exception | Query aborts with clean error message |
| **Spike 3** | OptimizerExtension can inject LogicalFilter | Query results are filtered |
| **Spike 4** | ClientContextState stores identity | `current_user()` returns stored value |

**Deliverable:** Confidence that architecture is viable

### Phase 1: Foundation (Days 2-3)

**Goal:** Extension skeleton with identity and storage

- [ ] Extension entry point (`DUCKDB_CPP_EXTENSION_ENTRY`)
- [ ] Register `ExtensionCallback` for connection lifecycle
- [ ] Implement `RBACState : ClientContextState`
- [ ] Connection property handling for identity (may need core change)
- [ ] Register `current_user()` ScalarFunction
- [ ] Create system tables on first load
- [ ] Register `OptimizerExtension` (empty for now)
- [ ] Register `ParserExtension` (empty for now)

**Deliverable:** Extension loads, identity stored, `current_user()` works

### Phase 2: Custom DDL (Days 3-5)

**Goal:** CREATE/DROP ROLE, GRANT/REVOKE work

- [ ] Implement `ParserExtension::parse_function`
- [ ] Parse `CREATE ROLE name` → TableFunction
- [ ] Parse `DROP ROLE name` → TableFunction
- [ ] Parse `GRANT SELECT ON table TO role` → TableFunction
- [ ] Parse `REVOKE SELECT ON table FROM role` → TableFunction
- [ ] Parse `GRANT role TO role` → TableFunction
- [ ] Parse `REVOKE role FROM role` → TableFunction
- [ ] Validation: role exists, table exists
- [ ] Insert/delete from system tables

**Deliverable:** All DDL works, system tables populated

### Phase 3: Permission Enforcement (Days 5-7)

**Goal:** Table and column access control

- [ ] Implement `OptimizerExtension::pre_optimize_function`
- [ ] Walk `LogicalOperator` tree to find `LogicalGet`
- [ ] Check table access against `duckdb_table_privileges`
- [ ] Check column access against `duckdb_column_privileges`
- [ ] Compute allowed columns (table-level vs column-level grants)
- [ ] Throw `PermissionException` on violation
- [ ] Handle JOINs (check all tables)
- [ ] Handle subqueries and CTEs

**Deliverable:** Queries blocked without proper grants

### Phase 4: Row Policies (Days 7-9)

**Goal:** Row-level security works

- [ ] Parse `CREATE ROW POLICY ... USING (expr) TO role`
- [ ] Parse `DROP ROW POLICY name ON table`
- [ ] Validate policy expression at creation time
- [ ] Lookup applicable policies in optimizer
- [ ] Combine multiple policies with OR
- [ ] Parse and bind filter expression at query time
- [ ] Inject `LogicalFilter` into plan
- [ ] Verify policy expressions can reference all columns

**Deliverable:** Row filtering works

### Phase 5: Polish & Integration (Days 9-10)

**Goal:** Production-ready MVP

- [ ] Create introspection views
- [ ] Refine error messages with context
- [ ] Handle edge cases (recursive CTEs, self-joins)
- [ ] Superuser bypass working
- [ ] Test with TAP integration
- [ ] Documentation

**Deliverable:** MVP complete

### Known Limitations (Phase 2 / Future)

| Limitation | Status | Solution |
|------------|--------|----------|
| SELECT * with forbidden columns errors | MVP | Parser override or core change |
| Role hierarchy | Deferred | Transitive closure in roles query |
| Restrictive (AND) policies | Deferred | Add `is_permissive` flag handling |
| GRANT OPTION | Deferred | Track grantor in grants table |

---

## 12. Test Scenarios

### 12.1 Basic Permissions

```sql
-- Setup
CREATE ROLE analyst;
CREATE ROLE viewer;
CREATE TABLE orders (id INT, customer VARCHAR, amount DECIMAL, secret_notes VARCHAR);

-- Test: No grant = denied
-- As user with role 'analyst' (no grants yet)
SELECT * FROM orders;  -- ERROR: permission denied on table 'orders'

-- Test: Table grant = allowed
GRANT SELECT ON orders TO analyst;
SELECT * FROM orders;  -- SUCCESS: all 4 columns (table grant = all columns)

-- Test: Column grant = restricted
REVOKE SELECT ON orders FROM analyst;
GRANT SELECT (id, customer, amount) ON orders TO analyst;

-- MVP LIMITATION: SELECT * errors because binding expands to all columns
SELECT * FROM orders;  -- ERROR: permission denied on column 'secret_notes'

-- Explicit columns work
SELECT id, customer, amount FROM orders;  -- SUCCESS: 3 columns

-- Forbidden column explicit = error
SELECT secret_notes FROM orders;  -- ERROR: permission denied on column 'secret_notes'

-- Forbidden column in WHERE = error  
SELECT id FROM orders WHERE secret_notes = 'x';  -- ERROR: permission denied on column 'secret_notes'
```

### 12.2 Role Membership

```sql
-- Setup
CREATE ROLE data_access;
CREATE ROLE power_user;
GRANT SELECT ON orders TO data_access;
GRANT data_access TO power_user;

-- Test: Inherited access
-- As user with role 'power_user'
SELECT * FROM orders;  -- Should succeed via data_access
```

### 12.3 Row Policies

```sql
-- Setup
CREATE ROLE regional_sales;
GRANT SELECT ON orders TO regional_sales;
INSERT INTO orders VALUES (1, 'Alice', 100, 'US'), (2, 'Bob', 200, 'EU');

CREATE ROW POLICY us_only ON orders
    FOR SELECT
    USING (region = 'US')
    TO regional_sales;

-- Test: Policy filters rows
-- As user with role 'regional_sales'
SELECT * FROM orders;  -- Should only return US row

-- Test: Multiple policies OR together
CREATE ROLE eu_access;
GRANT SELECT ON orders TO eu_access;
CREATE ROW POLICY eu_only ON orders
    FOR SELECT
    USING (region = 'EU')
    TO eu_access;

-- As user with roles ['regional_sales', 'eu_access']
SELECT * FROM orders;  -- Should return both US and EU rows
```

### 12.4 JOINs

```sql
-- Setup
CREATE TABLE customers (id INT, name VARCHAR, tier VARCHAR);
CREATE ROLE basic;
GRANT SELECT ON orders TO basic;
-- Note: no grant on customers

-- Test: JOIN requires access to both tables
-- As user with role 'basic'
SELECT o.*, c.name 
FROM orders o JOIN customers c ON o.customer_id = c.id;
-- Should ERROR: permission denied on customers
```

### 12.5 Edge Cases

```sql
-- Test: Table drop/recreate preserves grants
CREATE TABLE temp_data (x INT);
GRANT SELECT ON temp_data TO analyst;
DROP TABLE temp_data;
CREATE TABLE temp_data (x INT, y INT);
-- As analyst:
SELECT * FROM temp_data;  -- Should succeed (grant survived)

-- Test: Self-join applies same policy
CREATE ROW POLICY self_policy ON employees
    USING (department = 'SALES')
    TO viewer;
SELECT e1.name, e2.name AS manager
FROM employees e1 JOIN employees e2 ON e1.manager_id = e2.id;
-- Both e1 and e2 should be filtered by policy

-- Test: Recursive CTE
WITH RECURSIVE subs AS (
    SELECT * FROM employees WHERE id = 1
    UNION ALL
    SELECT e.* FROM employees e JOIN subs s ON e.manager_id = s.id
)
SELECT * FROM subs;
-- Policy should apply to all employee references
```

### 12.6 Error Cases

```sql
-- Test: Create role that exists
CREATE ROLE analyst;
CREATE ROLE analyst;  -- Should ERROR: already exists

-- Test: Grant to non-existent role
GRANT SELECT ON orders TO nonexistent;  -- Should ERROR: role not found

-- Test: Grant on non-existent table
GRANT SELECT ON nonexistent TO analyst;  -- Should ERROR: table not found

-- Test: Drop role with grants
CREATE ROLE temp;
GRANT SELECT ON orders TO temp;
DROP ROLE temp;  -- Should ERROR: has grants

-- Test: Invalid policy expression
CREATE ROW POLICY bad ON orders USING (nonexistent_column = 1) TO analyst;
-- Should ERROR: column not found
```

---

## 13. Future Considerations (v2)

### 13.1 Role Hierarchy

Allow roles to contain other roles:

```sql
CREATE ROLE senior_analyst;
GRANT analyst TO senior_analyst;  -- senior_analyst inherits analyst
```

Implementation: Transitive closure in effective roles computation.

### 13.2 Restrictive Policies

Add AND semantics for policies:

```sql
CREATE ROW POLICY must_be_active ON users
    AS RESTRICTIVE
    USING (status = 'active')
    TO everyone;
```

All restrictive policies AND together, then AND with permissive OR result.

### 13.3 GRANT OPTION

Allow users to grant privileges to others:

```sql
GRANT SELECT ON orders TO analyst WITH GRANT OPTION;
-- analyst can now: GRANT SELECT ON orders TO other_role;
```

### 13.4 DDL Privileges

Control who can CREATE/DROP/ALTER:

```sql
GRANT CREATE TABLE ON DATABASE main TO developer;
GRANT DROP TABLE ON orders TO admin;
```

### 13.5 Column Masking

Return masked values instead of denying access:

```sql
CREATE MASK ssn_mask ON users.ssn
    USING (CASE WHEN has_role('pii_viewer') THEN ssn ELSE '***-**-' || RIGHT(ssn, 4) END);
```

### 13.6 Partial Revokes

Revoke subset of broader grant:

```sql
GRANT SELECT ON *.* TO analyst;  -- All tables
REVOKE SELECT ON secret.* FROM analyst;  -- Except secret schema
```

### 13.7 PUBLIC Role

Built-in role representing all users:

```sql
GRANT SELECT ON public_data TO PUBLIC;  -- Everyone can see
```

### 13.8 Write Policies

Separate USING (reads) from WITH CHECK (writes):

```sql
CREATE ROW POLICY insert_own ON records
    FOR INSERT
    WITH CHECK (owner = current_user());
```

---

## 14. Appendix: ClickHouse/PostgreSQL Comparison

### 14.1 Feature Comparison

| Feature | ClickHouse | PostgreSQL | DuckDB RBAC (MVP) |
|---------|------------|------------|-------------------|
| Role hierarchy | ✅ | ✅ | ❌ (v2) |
| SET ROLE | ✅ | ✅ | ❌ |
| GRANT OPTION | ✅ | ✅ | ❌ (v2) |
| ADMIN OPTION | ✅ | ✅ | ❌ (v2) |
| Column privileges | ✅ | ✅ | ✅ |
| Row policies | ✅ | ✅ | ✅ |
| Permissive (OR) | ✅ | ✅ | ✅ |
| Restrictive (AND) | ✅ | ✅ | ❌ (v2) |
| Partial revokes | ✅ | ✅ | ❌ (v2) |
| Wildcards | ✅ (prefix) | ❌ | ❌ (v2) |
| Quotas | ✅ | ❌ | ❌ |
| Settings profiles | ✅ | ❌ | ❌ |
| Name-based grants | ✅ | ❌ (OID) | ✅ |

### 14.2 Syntax Comparison

| Operation | ClickHouse | PostgreSQL | DuckDB RBAC |
|-----------|------------|------------|-------------|
| Create role | `CREATE ROLE x` | `CREATE ROLE x` | `CREATE ROLE x` |
| Grant role | `GRANT x TO y` | `GRANT x TO y` | `GRANT x TO y` |
| Grant SELECT | `GRANT SELECT ON t TO r` | `GRANT SELECT ON t TO r` | `GRANT SELECT ON t TO r` |
| Grant columns | `GRANT SELECT(a,b) ON t TO r` | `GRANT SELECT(a,b) ON t TO r` | `GRANT SELECT(a,b) ON t TO r` |
| Row policy | `CREATE ROW POLICY p ON t USING e TO r` | `CREATE POLICY p ON t TO r USING (e)` | `CREATE ROW POLICY p ON t FOR SELECT USING (e) TO r` |

### 14.3 Key Design Decisions from CH/PG

**From ClickHouse:**
- Name-based grants (survives DDL)
- ROW POLICY syntax
- Permissive/restrictive policy types
- Policy expressions access all columns

**From PostgreSQL:**
- Explicit role creation required
- RESTRICT semantics for DROP ROLE
- Table must exist for grants
- Detailed error messages

---

## Document History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | Dec 2024 | Initial MVP design |
| 1.1 | Dec 2024 | Updated with actual DuckDB extension hooks; documented SELECT * MVP limitation; added spike validation phase; clarified query-time policy binding |

---

## Quick Reference Card

```sql
-- ROLES
CREATE ROLE name;
DROP ROLE name;
GRANT role TO member;
REVOKE role FROM member;

-- TABLE PRIVILEGES
GRANT SELECT ON table TO role;
REVOKE SELECT ON table FROM role;

-- COLUMN PRIVILEGES  
GRANT SELECT (col1, col2) ON table TO role;
REVOKE SELECT (col1) ON table FROM role;

-- ROW POLICIES
CREATE ROW POLICY name ON table FOR SELECT USING (expr) TO role;
DROP ROW POLICY name ON table;

-- INTROSPECTION
SELECT * FROM duckdb_roles;
SELECT * FROM duckdb_role_members;
SELECT * FROM duckdb_table_privileges;
SELECT * FROM duckdb_column_privileges;
SELECT * FROM duckdb_row_policies;
SELECT * FROM duckdb_effective_privileges;
SELECT * FROM duckdb_my_roles;

-- FUNCTIONS
SELECT current_user();
SELECT current_roles();
```

