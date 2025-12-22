# DuckDB RBAC Extension: High-Level Design

**Version:** 1.0  
**Last Updated:** December 2024

---

## 1. Overview

An extension that adds Role-Based Access Control to DuckDB:
- **Table-level access**: Who can query which tables
- **Column-level access**: Who can see which columns
- **Row-level security**: Filter rows based on user/role

---

## 2. Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         User Query                               │
└──────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────┐
│                    DuckDB Query Pipeline                         │
│                                                                  │
│   Parse ──▶ Bind ──▶ Optimize ──▶ Execute                       │
│     │                    │                                       │
│     ▼                    ▼                                       │
│  ┌──────────┐      ┌──────────┐                                 │
│  │ Parser   │      │ Optimizer│                                 │
│  │ Extension│      │ Extension│                                 │
│  │          │      │          │                                 │
│  │ Custom   │      │ Check    │                                 │
│  │ DDL      │      │ Access   │                                 │
│  └──────────┘      │ Inject   │                                 │
│                    │ Filters  │                                 │
│                    └──────────┘                                 │
└──────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────┐
│                    RBAC Storage (DuckDB Tables)                  │
│                                                                  │
│   duckdb_roles │ duckdb_table_privileges │ duckdb_row_policies  │
└──────────────────────────────────────────────────────────────────┘
```

---

## 3. Core Components

| Component | Purpose | DuckDB Hook |
|-----------|---------|-------------|
| **DDL Handler** | Parse CREATE ROLE, GRANT, etc. | `ParserExtension` |
| **Access Enforcer** | Check permissions, inject filters | `OptimizerExtension` |
| **Session State** | Store user identity per connection | `ClientContextState` |
| **Storage** | Persist roles, grants, policies | DuckDB tables |

---

## 4. Data Model

```
┌─────────────┐     ┌───────────────────┐
│   Roles     │     │  Role Membership  │
│             │◀────│                   │
│ role_name   │     │ role_name         │
└─────────────┘     │ member_role       │
       │            └───────────────────┘
       │
       ▼
┌───────────────────┐     ┌─────────────────────┐
│ Table Privileges  │     │ Column Privileges   │
│                   │     │                     │
│ grantee           │     │ grantee             │
│ table_name        │     │ table_name          │
│ privilege         │     │ column_name         │
└───────────────────┘     └─────────────────────┘

┌─────────────────────────┐
│ Row Policies            │
│                         │
│ policy_name             │
│ table_name              │
│ grantee                 │
│ filter_expression       │
└─────────────────────────┘
```

---

## 5. Query Flow

```
1. Connection opened
   → Store identity: { user: "alice", roles: ["analyst"], superuser: false }

2. Query: SELECT * FROM orders WHERE amount > 100

3. Parser
   → Standard DuckDB parsing (not our extension)

4. Binder
   → Resolves tables, expands SELECT *

5. Optimizer Extension (OUR CODE)
   → Check: Is user superuser? → Skip if yes
   → Check: Does analyst have SELECT on orders? → Yes
   → Check: Does analyst have SELECT on all columns? → Yes
   → Lookup: Row policies for (orders, analyst) → "region = 'US'"
   → Inject: LogicalFilter with "region = 'US'"

6. Execute
   → Returns filtered results
```

---

## 6. Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Extension vs Core | Extension | Keep core simple, not all users need RBAC |
| Grant storage | Name-based | Survives table drop/recreate |
| Policy binding | Query-time | Handles schema evolution |
| SELECT * behavior | Error on forbidden | MVP limitation (optimizer runs after binding) |
| Role hierarchy | Flat (MVP) | Simplicity first |
| Policy combination | OR (permissive) | AND (restrictive) deferred |

---

## 7. SQL Syntax

```sql
-- Roles
CREATE ROLE analyst;
DROP ROLE analyst;
GRANT analyst TO alice;

-- Table privileges
GRANT SELECT ON orders TO analyst;
REVOKE SELECT ON orders FROM analyst;

-- Column privileges
GRANT SELECT (id, customer) ON orders TO analyst;

-- Row policies
CREATE ROW POLICY region_filter ON orders
    USING (region = 'US')
    TO analyst;

-- Introspection
SELECT * FROM duckdb_roles;
SELECT * FROM duckdb_table_privileges;
SELECT * FROM duckdb_row_policies;
```

---

## 8. MVP Scope

### Included

- CREATE/DROP ROLE
- GRANT/REVOKE SELECT (table and column)
- CREATE/DROP ROW POLICY
- Per-connection identity
- Introspection tables
- `current_user()` function

### Excluded (Future)

- Role hierarchy
- INSERT/UPDATE/DELETE privileges
- DDL privileges
- GRANT OPTION
- Restrictive (AND) policies
- Silent SELECT * filtering

---

## 9. Known Limitations

| Limitation | Impact | Workaround |
|------------|--------|------------|
| SELECT * errors on forbidden columns | Must use explicit column list | Query `duckdb_effective_privileges` first |
| No role hierarchy | Duplicate grants | Use flat role structure |

---

## 10. Security Model

```
┌─────────────────────────────────────────────────────────┐
│                    Session Identity                     │
│                                                         │
│   Set by app at connection time (immutable)            │
│   { user: string, roles: string[], superuser: bool }   │
└─────────────────────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────┐
│                   Permission Check                      │
│                                                         │
│   1. Superuser? → Allow all                            │
│   2. Table grant exists? → Continue / Deny             │
│   3. Column grants exist? → Check each / Deny          │
│   4. Row policies exist? → Inject filter / Allow all   │
└─────────────────────────────────────────────────────────┘
```

---

## 11. References

| Document | Content |
|----------|---------|
| `PRD.md` | Product requirements |
| `LLD.md` | Detailed technical design |
| `QnA.md` | Requirements Q&A (112 questions) |
| `RFC.md` | RFC for DuckDB maintainers |

