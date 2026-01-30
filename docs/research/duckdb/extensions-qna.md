**Prompt**

You have full access to duckdb codebase including many extensions present in @extension/ and external_extesion directories.

Look at the docs and @src/ and document what APIs are available for duckdb extensions. 

How they're used. What datastructures are exposed etc.

**Response**

**Two Extension APIs (and two ABIs)**

- **C++ extensions (C++ ABI, version-must-match)**: You compile against DuckDB’s C++ headers and export a C symbol entrypoint. Core types are in `src/include/duckdb/main/extension.hpp:18` and the loader API is `src/include/duckdb/main/extension/extension_loader.hpp:28`. This is what in-tree extensions use (e.g. JSON/TPCH).
- **C API “struct” extensions (C ABI via function-pointer table)**: You include `src/include/duckdb_extension.h:70` and initialize a global `duckdb_ext_api_v1` using the provided macros; your extension calls into DuckDB through that function table. The entrypoint helper macros are at `src/include/duckdb_extension.h:1411`. This is the more “binary-friendly” option, but note the header warning that it’s not yet fully stable (`src/include/duckdb_extension.h:14`).

---

**How C++ extensions are loaded**

- You typically export an init symbol using `DUCKDB_CPP_EXTENSION_ENTRY(name, loader)` from `src/include/duckdb/main/extension/extension_loader.hpp:119`. In-tree examples:
  - JSON registers types/functions/casts/macros/replacement scans in `extension/json/json_extension.cpp:34` and exports the entry at `extension/json/json_extension.cpp:100`.
  - TPCH registers table + pragma functions in `extension/tpch/tpch_extension.cpp:165` and exports the entry at `extension/tpch/tpch_extension.cpp:217`.

DuckDB also has an OO base class `duckdb::Extension` for statically linked / managed extensions (`src/include/duckdb/main/extension.hpp:18`), where you implement `Load(ExtensionLoader&)`.

---

**What you can register from a C++ extension (`ExtensionLoader`)**

All of these are methods on `duckdb::ExtensionLoader` (`src/include/duckdb/main/extension/extension_loader.hpp:40`):

- **Scalar functions**: `RegisterFunction(ScalarFunction|ScalarFunctionSet|CreateScalarFunctionInfo)` (`src/include/duckdb/main/extension/extension_loader.hpp:44`).
- **Aggregate functions**: `RegisterFunction(AggregateFunction|AggregateFunctionSet|CreateAggregateFunctionInfo)` (`src/include/duckdb/main/extension/extension_loader.hpp:50`).
- **Table functions**: `RegisterFunction(TableFunction|TableFunctionSet|CreateTableFunctionInfo)` (`src/include/duckdb/main/extension/extension_loader.hpp:55`).
- **Pragmas**: `RegisterFunction(PragmaFunction|PragmaFunctionSet)` (`src/include/duckdb/main/extension/extension_loader.hpp:60`).
- **COPY functions**: `RegisterFunction(CopyFunction)` (`src/include/duckdb/main/extension/extension_loader.hpp:69`) (type is defined in `src/include/duckdb/function/copy_function.hpp:186`).
- **Macros**: `RegisterFunction(CreateMacroInfo&)` (`src/include/duckdb/main/extension/extension_loader.hpp:71`) (JSON shows this in practice: `extension/json/json_extension.cpp:74`).
- **Collations**: `RegisterCollation(CreateCollationInfo&)` (`src/include/duckdb/main/extension/extension_loader.hpp:74`).
- **Custom logical types**: `RegisterType(type_name, LogicalType, bind_logical_type_function_t)` (`src/include/duckdb/main/extension/extension_loader.hpp:88`). The modifier-binding hook type is `bind_logical_type_function_t` (`src/include/duckdb/parser/parsed_data/create_type_info.hpp:23`).
- **Custom casts**: `RegisterCastFunction(source, target, …)` (`src/include/duckdb/main/extension/extension_loader.hpp:95`).
- **Secrets**:
  - Register secret “types” via `RegisterSecretType(SecretType)` (`src/include/duckdb/main/extension/extension_loader.hpp:92`), where `SecretType` is defined in `src/include/duckdb/main/secret/secret.hpp:80`.
  - Register secret providers via `RegisterFunction(CreateSecretFunction)` (`src/include/duckdb/main/extension/extension_loader.hpp:66`), where `CreateSecretFunction` is defined in `src/include/duckdb/main/secret/secret.hpp:50`.

---

**Execution-facing data structures exposed to extension authors (C++ API)**

These are the “core” building blocks you implement/consume when you define functions:

- **Types & values**
  - `LogicalType` (used everywhere for signatures; see usage in `extension/json/json_extension.cpp:36`).
  - `Value` (scalar runtime values; used heavily in bindings and in table function output, e.g. `extension/tpch/tpch_extension.cpp:111`).

- **Vectorized execution**
  - `DataChunk` + `Vector` are the core “columnar batch” units passed to scalar functions and produced by table functions. Table functions receive `DataChunk &output` in their scan callback (example: `extension/tpch/tpch_extension.cpp:100`).
  - Validity (NULL) handling is part of vectors; in C API you see it directly via validity bitmasks (see below).

- **Function binding and per-query state**
  - `FunctionData`: bind-time state returned from bind callbacks (TPCH defines a `TableFunctionData` subclass: `extension/tpch/tpch_extension.cpp:13`).
  - Table functions explicitly split state into:
    - `GlobalTableFunctionState` (`src/include/duckdb/function/table_function.hpp:58`) and
    - `LocalTableFunctionState` (`src/include/duckdb/function/table_function.hpp:82`)
    - plus bind input structs like `TableFunctionBindInput` (`src/include/duckdb/function/table_function.hpp:97`) and execution input `TableFunctionInput` (`src/include/duckdb/function/table_function.hpp:161`).
  - Scalar functions support bind callbacks and thread-local state via `FunctionLocalState` (`src/include/duckdb/function/scalar_function.hpp:22`) and `bind_scalar_function_t` (`src/include/duckdb/function/scalar_function.hpp:100`).

---

**Other extension hooks beyond “register functions” (C++ API)**

These are not `ExtensionLoader` methods, but are “extension points” exposed via `DBConfig`:

- **Replacement scans**: Add entries to `DBConfig::replacement_scans` (`src/include/duckdb/main/config.hpp:249`) using `ReplacementScan` (`src/include/duckdb/function/replacement_scan.hpp:55`). JSON does this to make `'file.json'` resolve to a JSON scan: `extension/json/json_extension.cpp:60`.
- **Parser extensions**: `DBConfig::parser_extensions` (`src/include/duckdb/main/config.hpp:266`) with types in `src/include/duckdb/parser/parser_extension.hpp:102`.
- **Optimizer extensions**: `DBConfig::optimizer_extensions` (`src/include/duckdb/main/config.hpp:268`) with types in `src/include/duckdb/optimizer/optimizer_extension.hpp:34`.
- **Binder/operator extensions**: `DBConfig::operator_extensions` (`src/include/duckdb/main/config.hpp:274`) with interface `OperatorExtension` (`src/include/duckdb/planner/operator_extension.hpp:29`).
- **Storage extensions**: `DBConfig::storage_extensions` (`src/include/duckdb/main/config.hpp:276`) with interface `StorageExtension` (`src/include/duckdb/storage/storage_extension.hpp:34`).
- **Lifecycle callbacks**: `DBConfig::extension_callbacks` (`src/include/duckdb/main/config.hpp:282`) with interface `ExtensionCallback` (`src/include/duckdb/planner/extension_callback.hpp:18`).
- **Extension-specific SET options**: `DBConfig::AddExtensionOption(...)` (`src/include/duckdb/main/config.hpp:305`).

---

**C API extension surface (`duckdb_extension.h`): what you get**

The C API extension model is: DuckDB passes you an “access” vtable so you can fetch (1) the current database handle and (2) a function-pointer table for the C API (`src/include/duckdb.h:901`). You usually:

- Declare the global API table: `DUCKDB_EXTENSION_GLOBAL` (`src/include/duckdb_extension.h:1382`)
- Initialize it in your entrypoint: `DUCKDB_EXTENSION_API_INIT(...)` (`src/include/duckdb_extension.h:1385`)
- Export an entrypoint using `DUCKDB_EXTENSION_ENTRYPOINT` (`src/include/duckdb_extension.h:1411`)

Once initialized, you can call the C API functions through the table, including registering:
- scalar/aggregate/table functions (`src/include/duckdb_extension.h:300`, `:319`, `:346`)
- replacement scans (`src/include/duckdb_extension.h:381`)
- (unstable) copy functions, filesystem, catalog, config options, etc. (see the `#ifdef DUCKDB_EXTENSION_API_VERSION_UNSTABLE` sections in `src/include/duckdb_extension.h:530` onward)

Concrete example: the demo C extension implements and registers a scalar function using chunks/vectors/validity:
- entrypoint: `extension/demo_capi/capi_demo.cpp:4`
- function implementation + registration: `extension/demo_capi/add_numbers.cpp:8` and `extension/demo_capi/add_numbers.cpp:43`

If you want, I can also produce a “cheat sheet” mapping: *feature → exact C++ type(s) and callback signatures* for ScalarFunction/TableFunction/AggregateFunction (including bind/init/state lifetimes).


---

**Prompt**

Can an extension register a callback that runs BEFORE a query executes?

**Response**

Yes — but only via the **C++ extension API**, not the C “struct” extension API.

**What exists**
- DuckDB has a per-connection hook interface `ClientContextState` with `QueryBegin(ClientContext &context)` that is called at the start of each query, before execution (and even before planning finishes).
  - Interface: `src/include/duckdb/main/client_context_state.hpp:50`
  - Invocation: `ClientContext::BeginQueryInternal` calls `state->QueryBegin(*this)` for all registered states at `src/main/client_context.cpp:199` (loop at `src/main/client_context.cpp:217`).

**How an extension wires it up**
- A state must be registered on each `ClientContext` via `context.registered_state` (`src/include/duckdb/main/client_context.hpp:82`).
- The usual way for an extension to do that is:
  1. Register an `ExtensionCallback` (connection lifecycle) in `DBConfig::extension_callbacks` (`src/include/duckdb/main/config.hpp:282`).
  2. In `ExtensionCallback::OnConnectionOpened(ClientContext &context)` (`src/include/duckdb/planner/extension_callback.hpp:18`), call `context.registered_state->GetOrCreate<MyState>("my_key")`.
  3. Implement `MyState::QueryBegin(...)` to run your “before query executes” code.

**C API note**
- The C extension interface (`src/include/duckdb_extension.h:1411`) does not expose a “before query” hook like this; it’s mainly for registering functions, types, etc., through the function-pointer table.

If you tell me whether you need this to run for *every* query or only for *specific* queries (e.g. only SELECT, or only when certain tables are referenced), I can point at the best place to filter (QueryBegin vs parser/optimizer extensions).


---

**Prompt**

Can an extension see the list of tables/columns a query accesses?

**Response**

Yes for **tables** (there’s an explicit API). For **columns**, not via a simple “official” public API, but you *can* derive them in C++ by inspecting the planned query (or, if you’re implementing a table function, DuckDB tells you which columns were projected).

**Tables (easy, supported)**
- **C API**: `duckdb_get_table_names(connection, query, qualified)` returns a `duckdb_value` of type `VARCHAR[]` (`src/include/duckdb.h:1070`).
- **C++ API**: `ClientContext::GetTableNames(query, qualified)` returns a set of table names (`src/include/duckdb/main/client_context.hpp:220`).

**Columns**
- **Inside a table function you implement**: you can see which columns the *caller* requested via projection pushdown.
  - `TableFunctionInitInput` includes `column_ids` and `projection_ids` (`src/include/duckdb/function/table_function.hpp:116`).
  - This is the “real” set of columns the engine plans to scan from your table function (after column pruning), so it’s usually what extension authors want.

- **For arbitrary queries (global “what columns does this query touch?”)**:
  - There is **no single stable helper** like `GetColumnNamesUsedByQuery()`.
  - In the **C++ extension API**, you can extract/traverse the logical plan and look for scan operators:
    - `ClientContext::ExtractPlan(query)` gives a `LogicalOperator` tree (`src/include/duckdb/main/client_context.hpp:188`).
    - Scan nodes are typically `LogicalGet` (`src/include/duckdb/planner/operator/logical_get.hpp:20`), which expose:
      - `GetTable()` to identify the base table (`src/include/duckdb/planner/operator/logical_get.hpp:66`)
      - bound column ids via `GetColumnIds()` (`src/include/duckdb/planner/operator/logical_get.hpp:79`)
      - plus `projection_ids` for “used outside the scan” (`src/include/duckdb/planner/operator/logical_get.hpp:41`)
  - You’d usually run this from a C++ hook that has query access (e.g., a `ClientContextState::QueryBegin`-style hook, or an optimizer extension) and then analyze the plan.

**Caveats**
- “Columns accessed” depends on the phase: raw SQL references vs post-binding expansion (`*`, views) vs post-optimizer pruning.
- If you need *accurate scanned columns*, inspect the plan after binding/optimization (or rely on `projection_ids` passed into your table function).

If you tell me whether you want **(a)** the columns referenced in SQL text, or **(b)** the columns actually scanned after pruning, I can point to the best hook/phase to compute it.


---

**Prompt**

Can an extension modify the query plan (inject WHERE clauses)?

**Response**

Yes — **in the C++ extension API**, an extension can rewrite the *logical* query plan, including injecting a WHERE-like filter.

**Where this is supported**
- Register an **optimizer extension** by pushing to `DBConfig::optimizer_extensions` (`src/include/duckdb/main/config.hpp:268`).
- DuckDB will call your callbacks with a mutable `unique_ptr<LogicalOperator> &plan`:
  - `pre_optimize_function`: runs **before** built-in optimizers (`src/include/duckdb/optimizer/optimizer_extension.hpp:40`, invoked in `src/optimizer/optimizer.cpp:317`)
  - `optimize_function`: runs **after** built-in optimizers (`src/include/duckdb/optimizer/optimizer_extension.hpp:36`, invoked in `src/optimizer/optimizer.cpp:328`)

**How “inject WHERE” works**
- A SQL `WHERE`/`HAVING` is represented in the logical plan as a `LogicalFilter` operator (`src/include/duckdb/planner/operator/logical_filter.hpp:15`).
- Your optimizer extension can wrap the current plan (or any subtree) in a `LogicalFilter` and attach your predicate expression(s). Conceptually:
  - create `LogicalFilter(predicate)`
  - set its child to the existing subtree
  - replace the subtree pointer with the new filter node

**Important caveats**
- This is **not available through the C “struct” extension API** (`duckdb_extension.h`) today; that API is mostly for registering functions/types etc., not plan rewrites.
- At the optimizer-extension stage the plan is already **bound** (names resolved, types assigned), so any injected predicate must be a correctly **bound expression** (or built using existing bound expressions / binder utilities). If you inject unbound expressions, you’ll crash or fail verification later.

If you tell me what kind of filter you want to inject (e.g. RLS by table name, tenant_id = current_setting('tenant'), time-window enforcement, etc.), I can outline the safest plan-walk strategy and where to grab the needed column bindings from scan nodes like `LogicalGet` (`src/include/duckdb/planner/operator/logical_get.hpp:20`).


---

**Prompt**

Can an extension rewrite SELECT * to explicit column list?

**Response**

Inside DuckDB’s planner, `SELECT *` is *already* rewritten to an explicit column list during binding — so by the time an extension sees the logical plan, there is no `*` anymore.

- The binder explicitly expands `*` in the SELECT list in `Binder::BindSelectNode`:
  - `src/planner/binder/query_node/bind_select_node.cpp:439` calls `ExpandStarExpressions(...)`
  - The expansion logic is in `src/planner/binder/expression/bind_star_expression.cpp:254` (`Binder::ExpandStarExpression`), which uses `bind_context.GenerateAllColumnExpressions(...)` to produce a concrete list of `ColumnRefExpression`s.

**So: can an extension rewrite `SELECT *` to explicit columns?**
- **Plan-level / execution-level:** effectively “yes, automatically” — if you use an optimizer extension to inspect/modify the plan, you will already see an explicit projection list (no star to rewrite).
- **SQL text / AST rewrite (turn `SELECT *` into `SELECT a,b,c` in the statement):** not via a small “hook” in the public extension API. The binder expansion happens internally.

**If you truly need a SQL-level rewrite**, the extension-shaped way to do it is:
- implement a **parser override** (`ParserExtension::parser_override` in `src/include/duckdb/parser/parser_extension.hpp:112`) and return a rewritten set of `SQLStatement`s (with an explicit select list you construct from catalog metadata). This is a “replace the parser” feature, not a lightweight callback.

If you describe your goal (e.g., “for auditing/logging”, “for view/materialization”, “to enforce column-level security”), I can tell you which approach fits best and what phase to target.


---

**Prompt**

Can an extension add new SQL statement types (CREATE ROLE, etc.)?

**Response**

Yes — but **not by adding new `StatementType` enum values**. Extensions can add **new SQL syntax** by producing an `EXTENSION_STATEMENT` that DuckDB routes to your code.

**How it works (C++ API)**
- DuckDB has a built-in statement kind `StatementType::EXTENSION_STATEMENT` (`src/include/duckdb/common/enums/statement_type.hpp:46`).
- If DuckDB’s normal parser fails to parse a statement, it will try registered **parser extensions** (`ParserExtension::parse_function`) on that statement (`src/parser/parser.cpp:372`).
- If your `parse_function` succeeds, DuckDB wraps it into an `ExtensionStatement` (`src/include/duckdb/parser/statement/extension_statement.hpp:16`, constructed in `src/parser/parser.cpp:381`).
- During binding, DuckDB calls your `plan_function` and expects you to return a `TableFunction` + parameters (`src/include/duckdb/parser/parser_extension.hpp:77`). Binding implementation is in `src/planner/binder/statement/bind_extension.cpp:7`.

So you can implement something like `CREATE ROLE ...` entirely in your extension:
- `parse_function`: recognize/parse the text `CREATE ROLE ...` and return custom `ParserExtensionParseData`
- `plan_function`: return a table function that performs the action (and optionally returns a result)

**Important limitations**
- This does **not** extend DuckDB’s built-in Postgres-derived grammar. It’s a fallback path when the core parser can’t parse the statement.
- If you need to intercept/replace statements that DuckDB *already* parses, you’d need a **parser override** (`ParserExtension::parser_override` in `src/include/duckdb/parser/parser_extension.hpp:112`) and the DB config option `allow_parser_override_extension` (`src/include/duckdb/main/config.hpp:115`).
- You still won’t “register a brand new `StatementType`”; your statement will be executed as `EXTENSION_STATEMENT`, typically implemented as a table-function-backed statement.

If you tell me whether you want `CREATE ROLE` to affect DuckDB’s *native* permissions/auth model (if any) vs a role system maintained by your extension (catalog tables, secrets, etc.), I can point to the right internal APIs/extensions patterns.


---

**Prompt**

Can an extension access per-connection state (like a session context)?

**Response**

Yes — in the C++ extension API, DuckDB exposes an explicit **per-connection state manager** you can use as a “session context”.

**Primary mechanism: `ClientContext::registered_state`**
- Each connection has a `ClientContext`, which owns `registered_state` (`src/include/duckdb/main/client_context.hpp:82`).
- `registered_state` is a `RegisteredStateManager` that lets you store arbitrary `shared_ptr<ClientContextState>` keyed by string (`src/include/duckdb/main/client_context_state.hpp:111`).
- Your state type derives from `ClientContextState` (`src/include/duckdb/main/client_context_state.hpp:50`), which gives you lifecycle hooks like:
  - `QueryBegin(ClientContext &context)` / `QueryEnd(...)`
  - transaction begin/commit/rollback
  - task start/stop
  - prepared-statement related callbacks (`src/include/duckdb/main/client_context_state.hpp:79`)

DuckDB calls `QueryBegin` for all registered states at the start of a query (`src/main/client_context.cpp:199`, loop at `src/main/client_context.cpp:217`).

**How an extension installs it**
- The normal pattern is to register an `ExtensionCallback` (connection lifecycle) and create/attach your state in `OnConnectionOpened`:
  - `ExtensionCallback` interface: `src/include/duckdb/planner/extension_callback.hpp:18`
  - DBConfig stores callbacks in `extension_callbacks`: `src/include/duckdb/main/config.hpp:282`
- In `OnConnectionOpened(ClientContext &context)`, do:
  - `context.registered_state->GetOrCreate<MyState>("my_extension_state", ctor_args...)` (API at `src/include/duckdb/main/client_context_state.hpp:113`)

That gives you a persistent object for that connection across queries.

**Also works “on demand”**
- Many extension entry points (bind/execute) already receive a `ClientContext &context` (e.g., table function bind/execute signatures). You can lazily create/fetch your session object there via `registered_state->GetOrCreate(...)`.

**C API note**
- The C “struct” extension API (`duckdb_extension.h`) does not provide an equivalent first-class “session state” facility. You typically emulate it by storing state in function bind data or in external/global structures keyed by `duckdb_connection` (with all the usual lifecycle/thread-safety caveats).

If you tell me whether you want the state to be **per connection** (session), **per query**, or **per prepared statement**, I can recommend the best hook (`ClientContextState` vs bind data vs prepared-statement callbacks).


---

**Prompt**

Can an extension create persistent tables in the database?

**Response**

Yes.

**Ways an extension can create persistent tables**
- **Execute DDL/DML through a `ClientContext`/connection**: from extension code that runs during query execution (e.g., a table function), you can run `CREATE TABLE ...` / `INSERT ...` and the results are persisted in the attached database (i.e., stored in the DB file, not just in-memory structures).
  - The `tpch` extension’s `dbgen` table function is an in-tree example: it creates schema + loads data during function execution (`extension/tpch/tpch_extension.cpp:66` calls into `CreateTPCHSchema` / `LoadTPCHData`).

- **Use the “extension statement” path**: a parser extension can parse custom syntax and then plan it as a table function (`ExtensionStatement`), which can then perform catalog changes/creates as part of execution (`src/planner/binder/statement/bind_extension.cpp:7`).

**Important caveats**
- Persistence depends on the database: tables are persistent in a file-backed DB; in `:memory:` they disappear when the DB is gone.
- You need the operation to run in a valid transaction (default for most statements), and you should correctly mark that the statement modifies the database (TPCH does this in bind when possible via `input.binder->GetStatementProperties().RegisterDBModify(...)` at `extension/tpch/tpch_extension.cpp:53`).
- If the DB is opened read-only, the usual rules apply: you won’t be able to create persistent tables.

If you tell me whether you mean “create tables automatically at extension load time” vs “create tables when the user calls a function/statement”, I can point to the best-supported pattern for each.


---

**Prompt**

Can an extension register scalar functions (for current_user())?

**Response**

Yes — extensions can register **scalar functions** (including 0-arg ones like `current_user()`).

**C++ extension API**
- Use `ExtensionLoader::RegisterFunction(ScalarFunction …)` (`src/include/duckdb/main/extension/extension_loader.hpp:44`).
- A scalar function implementation is `std::function<void(DataChunk &, ExpressionState &, Vector &)>` (`src/include/duckdb/function/scalar_function.hpp:98`), and you can set metadata like stability in the `ScalarFunction` constructor (`src/include/duckdb/function/scalar_function.hpp:127`).

So you can register a 0-arg `VARCHAR` function named `current_user` and have it read per-connection state from the `ClientContext` (either via bind callbacks, or by looking at your own `ClientContextState` stored in `context.registered_state`).

**C “struct extension” API**
- Also yes: the extension C API exposes builders like `duckdb_create_scalar_function` via the vtable in `src/include/duckdb_extension.h:300` (with convenience macro at `src/include/duckdb_extension.h:1007`), plus the related “set” APIs.

**One important distinction**
- The SQL-standard `CURRENT_USER` token (often usable *without parentheses*) is treated as a SQL-value-function in many systems. DuckDB has internal handling for “SQL value functions” in the binder (see `SelectBinder::GetSQLValueFunction` referenced in `src/planner/expression_binder/select_binder.cpp:50`).
- The extension API does **not** provide a clean “register a new SQL keyword/value-function” hook. If you need `CURRENT_USER` (no parentheses) syntax, you’d typically need a parser extension/override that rewrites it to `current_user()`.

If you want, tell me which surface you need:
- only `current_user()` as a regular function, or
- the keyword-style `CURRENT_USER` syntax too.


---

**Prompt**

What extension hooks/interfaces exist (Catalog, Binder, Optimizer)?

**Response**

**Catalog / “Database Objects” hooks**

- **Register catalog entries (functions, types, casts, macros, …)** via `ExtensionLoader`:
  - Register scalar/aggregate/table/pragma/copy functions: `src/include/duckdb/main/extension/extension_loader.hpp:44`
  - Register macros/collations/types/casts/secrets: `src/include/duckdb/main/extension/extension_loader.hpp:69`
  - This is the main “catalog API” most extensions use (it inserts entries into DuckDB’s catalogs).

- **Provide an entire custom Catalog (for ATTACH)** via `StorageExtension`:
  - `StorageExtension::attach` returns a `unique_ptr<Catalog>`: `src/include/duckdb/storage/storage_extension.hpp:28`
  - You can also provide a custom `TransactionManager`: `src/include/duckdb/storage/storage_extension.hpp:31`
  - This is the closest thing to a true “catalog plugin interface”: you are effectively implementing a catalog backend for an attached database.

- **Autoload integration** (not a hook, but relevant to extension/catalog behavior):
  - DuckDB can autoload extensions based on missing catalog entries/config names: `src/include/duckdb/catalog/catalog.hpp:389`

**Binder hook (“OperatorExtension”)**

- **OperatorExtension** is a binder-time extension point that receives the parsed `SQLStatement` and returns a `BoundStatement`:
  - Interface: `src/include/duckdb/planner/operator_extension.hpp:29`
  - Main hook: `bind_function_t` (`OperatorExtension::Bind`) signature: `src/include/duckdb/planner/operator_extension.hpp:23`
- Registered through `DBConfig::operator_extensions`: `src/include/duckdb/main/config.hpp:273`

This is where you hook *binding/planning* decisions for statements DuckDB already parsed.

**Optimizer hook (plan rewrite / injection)**

- **OptimizerExtension** can rewrite the *logical plan* (`unique_ptr<LogicalOperator> &plan`) either:
  - **before** built-in optimizers (`pre_optimize_function`): `src/include/duckdb/optimizer/optimizer_extension.hpp:40`
  - **after** built-in optimizers (`optimize_function`): `src/include/duckdb/optimizer/optimizer_extension.hpp:36`
- Registered through `DBConfig::optimizer_extensions`: `src/include/duckdb/main/config.hpp:268`
- DuckDB invokes these around the built-in optimizer pipeline in `src/optimizer/optimizer.cpp:317` and `src/optimizer/optimizer.cpp:328`

This is the right hook for “modify the query plan” operations (inject filters, rewrite joins, etc.).

**Parser hooks (new syntax / statement-like extensions)**

- **ParserExtension** supports:
  - `parse_function` (only called when DuckDB fails to parse a statement): `src/include/duckdb/parser/parser_extension.hpp:104`
  - `plan_function` (turn your parse result into a `TableFunction` execution): `src/include/duckdb/parser/parser_extension.hpp:108`
  - optional full `parser_override` (replace the parser): `src/include/duckdb/parser/parser_extension.hpp:112`
- Registered through `DBConfig::parser_extensions`: `src/include/duckdb/main/config.hpp:266`

This is how extensions add “new statement types” in practice (they become `EXTENSION_STATEMENT` internally).

**Lifecycle / connection hooks**

- **ExtensionCallback** (connection + extension load lifecycle):
  - Interface: `src/include/duckdb/planner/extension_callback.hpp:18`
  - Registered via `DBConfig::extension_callbacks`: `src/include/duckdb/main/config.hpp:282`

Common use: install per-connection state in `OnConnectionOpened`.

**Other notable extension points**

- **Replacement scans**: rewrite unknown table names into a `TableRef` (e.g. `SELECT * FROM 'file.csv'`):
  - API type: `src/include/duckdb/function/replacement_scan.hpp:55`
  - Registered via `DBConfig::replacement_scans`: `src/include/duckdb/main/config.hpp:249`

If you want, I can turn this into a “which hook to use for X” table (e.g., new SQL syntax → ParserExtension; rewrite logical plan → OptimizerExtension; implement external catalog/ATTACH → StorageExtension; etc.).


---

**Prompt**

At what stage can extensions intervene: Parse → Bind → Optimize → Execute?

**Response**

Here’s where extensions can intervene in DuckDB’s pipeline, mapped to the actual extension hook types you can register.

**Parse (SQL text → SQLStatement / AST)**
- **Parser override (replace parser for all SQL)**: `ParserExtension::parser_override` in `src/include/duckdb/parser/parser_extension.hpp:112`
  - Runs *before* DuckDB’s normal parser, depending on the setting `allow_parser_override_extension` (see logic in `src/parser/parser.cpp:244`).
  - Lets you return a full `vector<unique_ptr<SQLStatement>>` (i.e., you can rewrite *anything*, including syntax DuckDB normally parses).
- **Parser extension fallback (only if DuckDB fails to parse)**: `ParserExtension::parse_function` in `src/include/duckdb/parser/parser_extension.hpp:104`
  - Only called after DuckDB’s parser errors, on per-statement chunks (see `src/parser/parser.cpp:372`).
  - On success, DuckDB creates an `ExtensionStatement` (`src/include/duckdb/parser/statement/extension_statement.hpp:16`), i.e. a statement of type `EXTENSION_STATEMENT`.

**Bind / Plan (SQLStatement → bound logical plan)**
- **ExtensionStatement planning**: `ParserExtension::plan_function` in `src/include/duckdb/parser/parser_extension.hpp:108`
  - Your custom statement is “planned” into a `TableFunction` execution (`src/planner/binder/statement/bind_extension.cpp:7`).
- **Binder extension hook (statement-level binding override)**: `OperatorExtension` in `src/include/duckdb/planner/operator_extension.hpp:29`
  - Registered via `DBConfig::operator_extensions` (`src/include/duckdb/main/config.hpp:273`).
  - Receives the `SQLStatement &` during binding and returns a `BoundStatement` (`src/include/duckdb/planner/operator_extension.hpp:23`).
- **Replacement scans (name resolution hook during binding)**: `ReplacementScan` in `src/include/duckdb/function/replacement_scan.hpp:55`
  - Registered via `DBConfig::replacement_scans` (`src/include/duckdb/main/config.hpp:249`).
  - Triggered when a table name can’t be found, letting you replace it with a `TableRef` (e.g. `'file.csv'` → `read_csv(...)`).

Also within bind/planning, extensions intervene indirectly via the bind callbacks on the functions they register:
- scalar/table/aggregate function bind callbacks (e.g. `bind_scalar_function_t` in `src/include/duckdb/function/scalar_function.hpp:100`)
- table function bind/init (column projection pushdown etc.) (`src/include/duckdb/function/table_function.hpp:97`, `:116`)

**Optimize (logical plan rewrite)**
- **OptimizerExtension** in `src/include/duckdb/optimizer/optimizer_extension.hpp:34`
  - Registered via `DBConfig::optimizer_extensions` (`src/include/duckdb/main/config.hpp:268`).
  - Two stages:
    - `pre_optimize_function`: runs **before** built-in optimizers (`src/optimizer/optimizer.cpp:317`)
    - `optimize_function`: runs **after** built-in optimizers (`src/optimizer/optimizer.cpp:328`)
  - Both can mutate `unique_ptr<LogicalOperator> &plan` (e.g., inject `LogicalFilter`).

**Execute (physical plan → run)**
- Most “execution-stage” extensibility is through **functions you register**:
  - scalar/aggregate/table/copy functions: implemented as callbacks that run during execution (e.g. table function scan produces `DataChunk`s).
- **Per-connection / per-query lifecycle hooks** (not plan rewrite, but runs around execution):
  - `ClientContextState::QueryBegin/QueryEnd` etc. (`src/include/duckdb/main/client_context_state.hpp:53`)
  - These fire at query begin (`src/main/client_context.cpp:199`, loop at `:217`) and query end, transaction events, task start/stop, etc.
  - Typically installed using an `ExtensionCallback::OnConnectionOpened` hook (`src/include/duckdb/planner/extension_callback.hpp:23`) registered via `DBConfig::extension_callbacks` (`src/include/duckdb/main/config.hpp:282`).

**Extra: Catalog/Storage “attach time” (outside Parse→Execute, but important)**
- **StorageExtension** (`src/include/duckdb/storage/storage_extension.hpp:34`) registered via `DBConfig::storage_extensions` (`src/include/duckdb/main/config.hpp:276`)
  - Hooks when an attached database is created/opened: `attach(...)` returns a custom `Catalog` (`src/include/duckdb/storage/storage_extension.hpp:28`)
  - Can also provide a custom transaction manager.

**ABI note**
- The “Parse/Bind/Optimize” hooks above are effectively **C++ extension API** territory (they’re wired through `DBConfig` and internal types). The C “struct extension” API is primarily for registering executable objects (functions, replacement scans, etc.), not deep planner hooks.

If you want, I can give the *exact order* these fire for a single query (including where `ExtensionCallback` and `ClientContextState::QueryBegin` fit relative to parsing and optimization).


---

**Prompt**

Is there a "table scan" hook that fires when a table is accessed?

**Response**

Not as a first-class “event hook” like `OnTableScan(table_name)`.

DuckDB’s public extension points don’t expose a generic callback that *fires at runtime whenever any base table is scanned*. Base table access ends up as a `PhysicalTableScan` operator internally (`src/include/duckdb/execution/operator/scan/physical_table_scan.hpp:23`), but there’s no pluggable “scan listener” interface around it.

**What you can do instead (common patterns)**

- **Planning-time detection (Bind/Optimize stage):**
  - Get tables referenced from SQL: `ClientContext::GetTableNames(query, qualified)` (`src/include/duckdb/main/client_context.hpp:220`).
  - Or inspect the logical plan for scan nodes (`LogicalGet`) and their bound columns (`src/include/duckdb/planner/operator/logical_get.hpp:20`).
  - Then run your own “pre-exec” logic in `ClientContextState::QueryBegin` (`src/include/duckdb/main/client_context_state.hpp:53`).

- **Runtime hook only for scans you own:**
  - If the “table” is actually a **table function** you registered, your scan callback *is* the runtime hook (it’s invoked when accessed).
  - If you provide a custom attached database via **StorageExtension** (custom `Catalog` / storage backend), you can instrument your own scan implementation there (`src/include/duckdb/storage/storage_extension.hpp:28`).

- **Emulate a table-scan hook by rewriting the plan (C++ API):**
  - Use an `OptimizerExtension` to rewrite `LogicalGet` nodes, e.g. wrap/replace them with something you control (custom `LogicalExtensionOperator` from `src/include/duckdb/planner/operator/logical_extension_operator.hpp:18`, created via an `OperatorExtension` `src/include/duckdb/planner/operator_extension.hpp:29`).
  - This is powerful but non-trivial: you’re effectively inserting your own operator into the pipeline.

If you tell me whether you need the hook for **(a)** base tables, **(b)** only specific schemas/tables, or **(c)** only extension-provided tables (table functions), I can recommend the cleanest approach for that case.


---

**Prompt**

What hooks do in-tree extensions actually use in practice?

**Response**

Based on empirical analysis of the `extension/` directory, here's what hooks each in-tree extension actually uses:

| Extension | Hooks Used |
|-----------|-----------|
| **JSON** | `RegisterFunction` (scalar, table, pragma, copy, macro), `RegisterType`, `RegisterCast`, `replacement_scans` |
| **Parquet** | `RegisterFunction` (table, copy), `replacement_scans` |
| **ICU** | `RegisterFunction` (scalar, table), `RegisterCollation` |
| **Autocomplete** | `RegisterFunction` (table), **`parser_extensions`** (uses `parser_override`!) |
| **TPCH/TPCDS** | `RegisterFunction` (table, pragma) |
| **Demo C API** | `duckdb_register_scalar_function` via C API |

**Key finding**: Most in-tree extensions only use `RegisterFunction` and `replacement_scans`. The **autocomplete** extension is the only one using deeper hooks (`parser_extensions` with `parser_override`).

**Autocomplete's parser_override usage** (`extension/autocomplete/autocomplete_extension.cpp:805`):
```cpp
class PEGParserExtension : public ParserExtension {
public:
    PEGParserExtension() {
        parser_override = PEGParser;  // Replaces the entire parser
    }
    static ParserOverrideResult PEGParser(ParserExtensionInfo *info, const string &query) {
        // ... tokenize and transform to SQLStatement ...
        return ParserOverrideResult(std::move(result));
    }
};

// Registration in LoadInternal():
auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
config.parser_extensions.push_back(PEGParserExtension());
```

**No in-tree extensions use**:
- `optimizer_extensions` (plan rewriting)
- `operator_extensions` (binder hooks)
- `storage_extensions` (custom catalogs)
- `extension_callbacks` (lifecycle hooks)


---

**Prompt**

Is there a concrete example of an optimizer extension that modifies query plans?

**Response**

Yes — there's a test extension in `test/extension/loadable_extension_optimizer_demo.cpp` called `WaggleExtension` that demonstrates plan modification.

**The WaggleExtension example** (`test/extension/loadable_extension_optimizer_demo.cpp:30`):

```cpp
class WaggleExtension : public OptimizerExtension {
public:
    WaggleExtension() {
        optimize_function = WaggleOptimizeFunction;  // Runs AFTER built-in optimizers
    }

    static bool HasParquetScan(LogicalOperator &op) {
        if (op.type == LogicalOperatorType::LOGICAL_GET) {
            auto &get = op.Cast<LogicalGet>();
            return get.function.name == "parquet_scan";
        }
        for (auto &child : op.children) {
            if (HasParquetScan(*child)) return true;
        }
        return false;
    }

    static void WaggleOptimizeFunction(OptimizerExtensionInput &input, 
                                       unique_ptr<LogicalOperator> &plan) {
        if (!HasParquetScan(*plan)) {
            return;  // Only intercept queries with parquet_scan
        }
        
        // Serialize the plan and send over network...
        // Receive result chunks...
        
        // REPLACE the entire plan with a ColumnDataGet
        plan = make_uniq<LogicalColumnDataGet>(0, types, std::move(chunk_collection));
    }
};
```

**Registration** (`test/extension/loadable_extension_optimizer_demo.cpp:154`):
```cpp
DUCKDB_CPP_EXTENSION_ENTRY(loadable_extension_optimizer_demo, loader) {
    auto &db = loader.GetDatabaseInstance();
    auto &config = DBConfig::GetConfig(db);
    
    // Push the optimizer extension
    config.optimizer_extensions.push_back(WaggleExtension());
    
    // Add custom settings the extension uses
    config.AddExtensionOption("waggle_location_host", "host for remote callback", LogicalType::VARCHAR);
    config.AddExtensionOption("waggle_location_port", "port for remote callback", LogicalType::INTEGER);
}
```

**Key patterns demonstrated**:
1. Walk the plan tree to find specific operators (`LogicalGet` with a function name)
2. Access `ClientContext` via `input.context` for settings/state
3. **Completely replace** `plan` with a new operator tree
4. Use `config.AddExtensionOption()` for extension-specific settings


---

**Prompt**

Does DuckDB have any built-in user/permission/authorization concepts?

**Response**

**No.** DuckDB has **no built-in user/role/permission system**. There are no GRANT, REVOKE, CREATE USER, or CREATE ROLE statements.

**What does exist:**

1. **File-level access control** via `enable_external_access` setting:
   - `DBConfig::CanAccessFile(path, type)` (`src/main/config.cpp:810`) checks if a path is allowed
   - `OpenerFileSystem::VerifyCanAccessFile()` (`src/common/opener_file_system.cpp:13`) throws `PermissionException` if denied
   - Configured via:
     - `enable_external_access` (bool) — master switch for all external file access
     - `allowed_paths` — explicit file paths always allowed
     - `allowed_directories` — directory prefixes always allowed

2. **Hardcoded "user" identity** (stub implementation, `src/catalog/default/default_functions.cpp:11`):
   ```cpp
   static const DefaultMacro internal_macros[] = {
       {DEFAULT_SCHEMA, "current_role", {nullptr}, {{nullptr, nullptr}}, "'duckdb'"},
       {DEFAULT_SCHEMA, "current_user", {nullptr}, {{nullptr, nullptr}}, "'duckdb'"},
       {DEFAULT_SCHEMA, "user", {nullptr}, {{nullptr, nullptr}}, "current_user"},
       {DEFAULT_SCHEMA, "session_user", {nullptr}, {{nullptr, nullptr}}, "'duckdb'"},
       // ...
   };
   ```
   These just return the literal string `'duckdb'` — no actual authentication.

3. **Secret Manager** for credentials (`src/main/secret/secret_manager.cpp`):
   - Stores S3 keys, API tokens, etc.
   - **Not** for access control — just credential storage for external services.

4. **Database access mode** (`AccessMode::READ_ONLY` vs `READ_WRITE`):
   - Controls whether the database file can be modified
   - Not a user permission system

**Grep confirmation**:
- `GRANT|REVOKE|CREATE ROLE|DROP ROLE` → **No matches** in `src/`
- No `PermissionCheck`, `AccessControl`, or `Authorize` patterns for table/column access


---

**Prompt**

What extension hooks are available for implementing RBAC/RLS?

**Response**

**Summary of hooks relevant for RBAC implementation:**

| Hook | When it Runs | What it Can Do | Best For |
|------|--------------|----------------|----------|
| **`ClientContextState::QueryBegin`** | Before query planning | Read query, check permissions, abort if denied | Query-level access checks |
| **`OptimizerExtension::pre_optimize_function`** | After binding, before optimization | Modify `LogicalOperator` tree, inject `LogicalFilter` | **RLS (Row-Level Security)** |
| **`ParserExtension::parse_function`** | When DuckDB parser fails | Handle custom DDL like `CREATE ROLE` | Adding new SQL statements |
| **`ParserExtension::parser_override`** | Before DuckDB parser | Intercept/rewrite any SQL | DDL interception, query rewriting |
| **`ExtensionCallback::OnConnectionOpened`** | Connection creation | Set up per-connection RBAC state | Session initialization |
| **Scalar function (`current_user()`)** | During execution | Return current user identity | User context |

**Recommended RBAC implementation approach:**

1. **Per-connection state** (`ClientContextState`):
   ```cpp
   class RBACState : public ClientContextState {
   public:
       string current_user;
       vector<string> roles;
       // permission cache, etc.
       
       void QueryBegin(ClientContext &context) override {
           // Pre-query permission checks
       }
   };
   
   // Install via ExtensionCallback::OnConnectionOpened
   context.registered_state->GetOrCreate<RBACState>("rbac_state");
   ```

2. **RLS via OptimizerExtension** (inject WHERE clauses):
   ```cpp
   class RLSOptimizerExtension : public OptimizerExtension {
   public:
       RLSOptimizerExtension() {
           pre_optimize_function = InjectRLSFilters;
       }
       
       static void InjectRLSFilters(OptimizerExtensionInput &input, 
                                    unique_ptr<LogicalOperator> &plan) {
           // Walk plan, find LogicalGet nodes
           // For each table with RLS policy, wrap in LogicalFilter
           // e.g.: WHERE tenant_id = current_setting('app.tenant_id')
       }
   };
   ```

3. **Custom DDL** (CREATE ROLE, GRANT, etc.) via `ParserExtension`:
   ```cpp
   class RBACParserExtension : public ParserExtension {
   public:
       RBACParserExtension() {
           parse_function = ParseRBACStatement;
           plan_function = PlanRBACStatement;
       }
       
       static ParserExtensionParseResult ParseRBACStatement(ParserExtensionInfo *, const string &query) {
           if (StartsWithCI(query, "CREATE ROLE") || StartsWithCI(query, "GRANT")) {
               // Parse and return custom data
           }
           return ParserExtensionParseResult();  // Fall through to DuckDB parser
       }
   };
   ```

4. **Override `current_user()`** function:
   ```cpp
   ScalarFunction current_user_func("current_user", {}, LogicalType::VARCHAR, 
       [](DataChunk &args, ExpressionState &state, Vector &result) {
           auto &context = state.GetContext();
           auto rbac = context.registered_state->Get<RBACState>("rbac_state");
           result.SetValue(0, Value(rbac->current_user));
       });
   loader.RegisterFunction(current_user_func);
   ```

**Challenges:**
- ❌ No catalog-level permission hooks (can't intercept `Catalog::GetEntry()`)
- ❌ No `PhysicalTableScan` hook (must inject at logical plan level)
- ❌ Must implement entire user/role/permission model from scratch
- ⚠️ Bound expressions in optimizer must be correctly typed (use `Optimizer::BindScalarFunction` helper)
