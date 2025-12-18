# 🔐 DuckDB RBAC Extension

[![DuckDB](https://img.shields.io/badge/DuckDB-v1.1.0-yellow)](https://duckdb.org)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Status](https://img.shields.io/badge/Status-MVP-orange)]()

**Role-Based Access Control for DuckDB** — Table, column, and row-level security as an extension.

---

## ✨ Features

- **🔒 Table-Level Access** — Control which roles can query which tables
- **📊 Column-Level Access** — Restrict visibility to specific columns
- **🔍 Row-Level Security** — Filter rows based on user identity and policies
- **👤 Session Identity** — Per-connection user and role context
- **📋 Introspection** — Query permissions via system tables
- **🔧 Pure Extension** — No DuckDB core modifications required

---

## 🚀 Quick Start

### Installation

```sql
-- Install from repository (coming soon)
INSTALL rbac FROM community;
LOAD rbac;

-- Or load from local build
LOAD '/path/to/rbac.duckdb_extension';
```

### Basic Usage

```sql
-- Create roles
CREATE ROLE analyst;
CREATE ROLE admin;

-- Grant table access
GRANT SELECT ON sales TO analyst;
GRANT SELECT ON customers TO analyst;

-- Grant column access (restrict to specific columns)
GRANT SELECT (id, name, email) ON users TO analyst;

-- Create row-level policy
CREATE ROW POLICY region_filter ON sales
    USING (region = current_user_region())
    TO analyst;

-- View permissions
SELECT * FROM duckdb_roles;
SELECT * FROM duckdb_table_privileges;
SELECT * FROM duckdb_row_policies;
```

---

## 📖 Documentation

| Document | Description |
|----------|-------------|
| [High-Level Design](docs/rbac-hld.md) | Architecture overview (5 min read) |
| [Full Design Doc](docs/rbac-design.md) | Detailed technical specification |
| [SQL Reference](docs/sql-reference.md) | Complete syntax documentation |
| [Integration Guide](docs/integration.md) | Embedding in your application |

---

## 🔧 SQL Reference

### Role Management

```sql
CREATE ROLE role_name;
DROP ROLE role_name;
GRANT role_name TO member_role;
REVOKE role_name FROM member_role;
```

### Table Privileges

```sql
GRANT SELECT ON table_name TO role_name;
REVOKE SELECT ON table_name FROM role_name;
```

### Column Privileges

```sql
GRANT SELECT (col1, col2) ON table_name TO role_name;
REVOKE SELECT (col1) ON table_name FROM role_name;
```

### Row Policies

```sql
CREATE ROW POLICY policy_name ON table_name
    FOR SELECT
    USING (filter_expression)
    TO role_name [, role_name ...];

DROP ROW POLICY policy_name ON table_name;
```

### Built-in Functions

```sql
SELECT current_user();    -- Returns session user name
SELECT current_roles();   -- Returns active roles
```

### System Tables

| Table | Description |
|-------|-------------|
| `duckdb_roles` | All defined roles |
| `duckdb_role_members` | Role membership |
| `duckdb_table_privileges` | Table-level grants |
| `duckdb_column_privileges` | Column-level grants |
| `duckdb_row_policies` | Row policy definitions |
| `duckdb_effective_privileges` | Current user's access (view) |

---

## 🔌 Integration

### C++ Embedding

```cpp
#include "duckdb.hpp"

duckdb::DuckDB db;
duckdb::Connection conn(db);

// Load extension
conn.Query("LOAD 'rbac'");

// Set user identity (call once per connection)
conn.Query("SELECT rbac_init('alice', 'analyst,viewer', false)");

// Now all queries are access-controlled
auto result = conn.Query("SELECT * FROM sales");
```

### Python

```python
import duckdb

conn = duckdb.connect()
conn.execute("LOAD 'rbac'")
conn.execute("SELECT rbac_init('alice', 'analyst', false)")

# Access-controlled query
df = conn.execute("SELECT * FROM sales").fetchdf()
```

---

## 🏗️ Building from Source

### Prerequisites

- CMake 3.14+
- C++17 compiler
- DuckDB source (for extension development)

### Build

```bash
git clone https://github.com/yourorg/duckdb-rbac.git
cd duckdb-rbac

# Configure
mkdir build && cd build
cmake .. -DDUCKDB_DIR=/path/to/duckdb

# Build
make -j$(nproc)

# Test
make test
```

---

## 📊 How It Works

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   Parser    │────▶│   Binder    │────▶│  Optimizer  │
│             │     │             │     │             │
│  (Custom    │     │             │     │  (Access    │
│   DDL)      │     │             │     │   Check)    │
└─────────────┘     └─────────────┘     └─────────────┘
       │                                       │
       ▼                                       ▼
┌─────────────────────────────────────────────────────┐
│              RBAC Storage (DuckDB Tables)           │
│                                                     │
│  duckdb_roles │ duckdb_privileges │ duckdb_policies │
└─────────────────────────────────────────────────────┘
```

1. **Parser Extension** catches custom DDL (`CREATE ROLE`, `GRANT`, etc.)
2. **Optimizer Extension** checks permissions before query execution
3. **Row policies** are injected as additional WHERE filters
4. **Permissions** stored in regular DuckDB tables

---

## ⚠️ Known Limitations

| Limitation | Description | Workaround |
|------------|-------------|------------|
| SELECT * | Errors if table has forbidden columns | Use explicit column list |
| Role hierarchy | Not supported in MVP | Use flat role structure |
| Write privileges | Only SELECT supported | Application handles writes |

---

## 🗺️ Roadmap

- [x] Table-level privileges
- [x] Column-level privileges
- [x] Row-level policies (permissive)
- [x] Introspection tables
- [ ] Silent SELECT * filtering
- [ ] Role hierarchy
- [ ] Restrictive (AND) policies
- [ ] Column masking
- [ ] GRANT OPTION

---

## 🤝 Contributing

Contributions are welcome! Please read our [Contributing Guide](CONTRIBUTING.md) first.

```bash
# Run tests
make test

# Run linter
make lint

# Format code
make format
```

---

## 📄 License

MIT License — see [LICENSE](LICENSE) for details.

---

## 🙏 Acknowledgments

- Inspired by [PostgreSQL RLS](https://www.postgresql.org/docs/current/ddl-rowsecurity.html) and [ClickHouse RBAC](https://clickhouse.com/docs/en/guides/sre/user-management/index)
- Built on [DuckDB](https://duckdb.org) — the fast in-process analytics database
- Thanks to the DuckDB community for the excellent extension API

---

## 📬 Contact

- **Issues**: [GitHub Issues](https://github.com/yourorg/duckdb-rbac/issues)
- **Discussions**: [GitHub Discussions](https://github.com/yourorg/duckdb-rbac/discussions)
- **Email**: your-email@example.com



