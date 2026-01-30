#include "rbac_state.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// RBACState
//===--------------------------------------------------------------------===//

shared_ptr<RBACState> RBACState::Get(ClientContext &context) {
	return context.registered_state->GetOrCreate<RBACState>("rbac");
}

vector<string> RBACState::GetEffectiveRoles(ClientContext &context) {
	auto rbac_state = Get(context);

	// Start with direct roles from session
	unordered_set<string> effective_set;
	for (const auto &role : rbac_state->roles) {
		effective_set.insert(role);
	}

	// If no roles, return empty
	if (effective_set.empty()) {
		return {};
	}

	// Query duckdb_role_members to find inherited roles (transitive closure)
	// We use a worklist algorithm to find all reachable roles
	Connection con(*context.db);

	vector<string> worklist(effective_set.begin(), effective_set.end());
	while (!worklist.empty()) {
		string current = worklist.back();
		worklist.pop_back();

		// Find roles that 'current' is a member of
		string sql = StringUtil::Format(
		    "SELECT role_name FROM duckdb_role_members WHERE member = '%s'",
		    StringUtil::Replace(current, "'", "''"));
		auto result = con.Query(sql);

		if (!result->HasError()) {
			// Iterate through results
			for (auto &row : *result) {
				string inherited_role = row.GetValue<string>(0);
				if (effective_set.find(inherited_role) == effective_set.end()) {
					effective_set.insert(inherited_role);
					worklist.push_back(inherited_role);
				}
			}
		}
	}

	// Convert set to sorted vector
	vector<string> effective_roles(effective_set.begin(), effective_set.end());
	std::sort(effective_roles.begin(), effective_roles.end());
	return effective_roles;
}

//===--------------------------------------------------------------------===//
// RBACExtensionCallback
//===--------------------------------------------------------------------===//

void RBACExtensionCallback::OnConnectionOpened(ClientContext &context) {
	// Ensure RBACState exists for this connection
	// Default state: superuser with user_name "default_user"
	RBACState::Get(context);
}

void RBACExtensionCallback::OnConnectionClosed(ClientContext &context) {
	// State will be cleaned up automatically when context is destroyed
	// Nothing special needed here
}

//===--------------------------------------------------------------------===//
// Scalar Functions
//===--------------------------------------------------------------------===//

static void RBACCurrentUserFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto rbac_state = RBACState::Get(context);

	// Return the current user name
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, rbac_state->user_name);
}

static void RBACCurrentRolesFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto rbac_state = RBACState::Get(context);

	// Return roles as a LIST of VARCHAR
	auto list_size = rbac_state->roles.size();
	auto list_data = ListVector::GetData(result);

	// Set the list entry
	list_data[0].offset = 0;
	list_data[0].length = list_size;

	// Add the role strings to the child vector
	auto &child_vector = ListVector::GetEntry(result);
	ListVector::SetListSize(result, list_size);

	auto child_data = FlatVector::GetData<string_t>(child_vector);
	for (idx_t i = 0; i < list_size; i++) {
		child_data[i] = StringVector::AddString(child_vector, rbac_state->roles[i]);
	}

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

static void RBACIsSuperuserFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto rbac_state = RBACState::Get(context);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<bool>(result);
	result_data[0] = rbac_state->is_superuser;
}

static void RBACEffectiveRolesFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();

	// Get effective roles (direct + inherited)
	auto effective_roles = RBACState::GetEffectiveRoles(context);

	// Return roles as a LIST of VARCHAR
	auto list_size = effective_roles.size();
	auto list_data = ListVector::GetData(result);

	// Set the list entry
	list_data[0].offset = 0;
	list_data[0].length = list_size;

	// Add the role strings to the child vector
	auto &child_vector = ListVector::GetEntry(result);
	ListVector::SetListSize(result, list_size);

	auto child_data = FlatVector::GetData<string_t>(child_vector);
	for (idx_t i = 0; i < list_size; i++) {
		child_data[i] = StringVector::AddString(child_vector, effective_roles[i]);
	}

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

//===--------------------------------------------------------------------===//
// rbac_set_identity() Table Function
//===--------------------------------------------------------------------===//

struct RBACSetIdentityBindData : public TableFunctionData {
	string user_name;
	vector<string> roles;
	bool is_superuser;
};

struct RBACSetIdentityGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> RBACSetIdentityBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("result");

	auto bind_data = make_uniq<RBACSetIdentityBindData>();

	// Get parameters: user_name VARCHAR, roles LIST(VARCHAR), is_superuser BOOLEAN
	bind_data->user_name = input.inputs[0].GetValue<string>();

	// Extract roles from LIST
	auto &roles_list = input.inputs[1];
	auto list_size = ListValue::GetChildren(roles_list).size();
	for (idx_t i = 0; i < list_size; i++) {
		auto &child = ListValue::GetChildren(roles_list)[i];
		bind_data->roles.push_back(child.GetValue<string>());
	}

	bind_data->is_superuser = input.inputs[2].GetValue<bool>();

	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> RBACSetIdentityInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<RBACSetIdentityGlobalState>();
}

static void RBACSetIdentityExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<RBACSetIdentityBindData>();
	auto &gstate = data.global_state->Cast<RBACSetIdentityGlobalState>();

	if (gstate.done) {
		return;
	}

	// Get the RBAC state
	auto rbac_state = RBACState::Get(context);

	// Check if already initialized (identity is immutable once set)
	if (rbac_state->initialized) {
		throw InvalidInputException("RBAC identity already set for this connection. Identity is immutable once initialized.");
	}

	// Set the identity
	rbac_state->user_name = bind_data.user_name;
	rbac_state->roles = bind_data.roles;
	rbac_state->is_superuser = bind_data.is_superuser;
	rbac_state->initialized = true;

	// Return confirmation
	output.SetCardinality(1);
	string result = "Identity set: user='" + bind_data.user_name + "', roles=[";
	for (idx_t i = 0; i < bind_data.roles.size(); i++) {
		if (i > 0) result += ", ";
		result += "'" + bind_data.roles[i] + "'";
	}
	result += "], is_superuser=" + string(bind_data.is_superuser ? "true" : "false");
	output.SetValue(0, 0, Value(result));

	gstate.done = true;
}

//===--------------------------------------------------------------------===//
// System Tables Creation
//===--------------------------------------------------------------------===//

void CreateRBACSystemTables(DatabaseInstance &db) {
	// Create a connection to execute DDL
	Connection con(db);

	// Create duckdb_roles table
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS duckdb_roles (
			role_name VARCHAR PRIMARY KEY,
			created_at TIMESTAMP DEFAULT current_timestamp
		)
	)");

	// Create duckdb_role_members table (for user->role and role->role membership)
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS duckdb_role_members (
			role_name VARCHAR NOT NULL,
			member VARCHAR NOT NULL,
			granted_at TIMESTAMP DEFAULT current_timestamp,
			PRIMARY KEY (role_name, member)
		)
	)");

	// Create duckdb_table_privileges table
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS duckdb_table_privileges (
			grantee VARCHAR NOT NULL,
			table_schema VARCHAR NOT NULL DEFAULT 'main',
			table_name VARCHAR NOT NULL,
			privilege_type VARCHAR NOT NULL DEFAULT 'SELECT',
			granted_at TIMESTAMP DEFAULT current_timestamp,
			PRIMARY KEY (grantee, table_schema, table_name, privilege_type)
		)
	)");

	// Create duckdb_column_privileges table
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS duckdb_column_privileges (
			grantee VARCHAR NOT NULL,
			table_schema VARCHAR NOT NULL DEFAULT 'main',
			table_name VARCHAR NOT NULL,
			column_name VARCHAR NOT NULL,
			privilege_type VARCHAR NOT NULL DEFAULT 'SELECT',
			granted_at TIMESTAMP DEFAULT current_timestamp,
			PRIMARY KEY (grantee, table_schema, table_name, column_name, privilege_type)
		)
	)");

	// Create duckdb_row_policies table
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS duckdb_row_policies (
			policy_name VARCHAR NOT NULL,
			table_schema VARCHAR NOT NULL DEFAULT 'main',
			table_name VARCHAR NOT NULL,
			policy_type VARCHAR NOT NULL DEFAULT 'PERMISSIVE',
			command VARCHAR NOT NULL DEFAULT 'SELECT',
			filter_expression VARCHAR NOT NULL,
			grantee VARCHAR NOT NULL,
			created_at TIMESTAMP DEFAULT current_timestamp,
			PRIMARY KEY (policy_name, table_schema, table_name)
		)
	)");
}

//===--------------------------------------------------------------------===//
// Introspection Views Creation
//===--------------------------------------------------------------------===//

void CreateRBACIntrospectionViews(DatabaseInstance &db) {
	Connection con(db);

	// Create duckdb_my_roles view - shows current user's effective roles
	// Uses UNNEST to expand the list returned by rbac_effective_roles()
	con.Query(R"(
		CREATE OR REPLACE VIEW duckdb_my_roles AS
		SELECT UNNEST(rbac_effective_roles()) AS role_name
	)");

	// Create duckdb_effective_privileges view - shows current user's accessible tables/columns
	// Filters out orphaned grants (tables/columns that don't exist)
	// Only shows grants for the current user's effective roles
	con.Query(R"(
		CREATE OR REPLACE VIEW duckdb_effective_privileges AS
		WITH my_roles AS (
			SELECT UNNEST(rbac_effective_roles()) AS role_name
		),
		-- Table-level privileges (role has full table access)
		table_privs AS (
			SELECT
				tp.grantee,
				tp.table_schema,
				tp.table_name,
				NULL::VARCHAR AS column_name,
				tp.privilege_type,
				'TABLE' AS grant_level
			FROM duckdb_table_privileges tp
			JOIN my_roles mr ON tp.grantee = mr.role_name
			WHERE EXISTS (
				SELECT 1 FROM information_schema.tables t
				WHERE t.table_schema = tp.table_schema
				AND t.table_name = tp.table_name
			)
		),
		-- Column-level privileges
		column_privs AS (
			SELECT
				cp.grantee,
				cp.table_schema,
				cp.table_name,
				cp.column_name,
				cp.privilege_type,
				'COLUMN' AS grant_level
			FROM duckdb_column_privileges cp
			JOIN my_roles mr ON cp.grantee = mr.role_name
			WHERE EXISTS (
				SELECT 1 FROM information_schema.columns c
				WHERE c.table_schema = cp.table_schema
				AND c.table_name = cp.table_name
				AND c.column_name = cp.column_name
			)
		)
		SELECT * FROM table_privs
		UNION ALL
		SELECT * FROM column_privs
		ORDER BY table_schema, table_name, column_name NULLS FIRST
	)");
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterRBACScalarFunctions(ExtensionLoader &loader) {
	// rbac_current_user() -> VARCHAR
	auto current_user_func = ScalarFunction("rbac_current_user", {}, LogicalType::VARCHAR, RBACCurrentUserFunction);
	loader.RegisterFunction(current_user_func);

	// rbac_current_roles() -> LIST(VARCHAR)
	auto current_roles_func = ScalarFunction("rbac_current_roles", {}, LogicalType::LIST(LogicalType::VARCHAR),
	                                          RBACCurrentRolesFunction);
	loader.RegisterFunction(current_roles_func);

	// rbac_is_superuser() -> BOOLEAN
	auto is_superuser_func = ScalarFunction("rbac_is_superuser", {}, LogicalType::BOOLEAN, RBACIsSuperuserFunction);
	loader.RegisterFunction(is_superuser_func);

	// rbac_effective_roles() -> LIST(VARCHAR) - includes inherited roles
	auto effective_roles_func = ScalarFunction("rbac_effective_roles", {}, LogicalType::LIST(LogicalType::VARCHAR),
	                                            RBACEffectiveRolesFunction);
	loader.RegisterFunction(effective_roles_func);

	// rbac_set_identity(user VARCHAR, roles LIST(VARCHAR), is_superuser BOOLEAN) -> VARCHAR
	TableFunction set_identity_func("rbac_set_identity",
	                                 {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR), LogicalType::BOOLEAN},
	                                 RBACSetIdentityExecute, RBACSetIdentityBind, RBACSetIdentityInit);
	loader.RegisterFunction(set_identity_func);
}

} // namespace duckdb
