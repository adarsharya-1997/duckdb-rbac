#include "rbac_parser.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include <regex>

namespace duckdb {

//===--------------------------------------------------------------------===//
// RBACParseData::ToString
//===--------------------------------------------------------------------===//

string RBACParseData::ToString() const {
	switch (statement_type) {
	case RBACStatementType::CREATE_ROLE:
		return "CREATE ROLE " + role_name;
	case RBACStatementType::DROP_ROLE:
		return "DROP ROLE " + role_name;
	case RBACStatementType::GRANT_ROLE:
		return "GRANT " + role_name + " TO " + member_name;
	case RBACStatementType::REVOKE_ROLE:
		return "REVOKE " + role_name + " FROM " + member_name;
	case RBACStatementType::GRANT_TABLE:
		return "GRANT SELECT ON " + table_name + " TO " + role_name;
	case RBACStatementType::REVOKE_TABLE:
		return "REVOKE SELECT ON " + table_name + " FROM " + role_name;
	case RBACStatementType::GRANT_COLUMN:
		return "GRANT SELECT (...) ON " + table_name + " TO " + role_name;
	case RBACStatementType::REVOKE_COLUMN:
		return "REVOKE SELECT (...) ON " + table_name + " FROM " + role_name;
	case RBACStatementType::CREATE_ROW_POLICY:
		return "CREATE ROW POLICY " + policy_name + " ON " + table_name;
	case RBACStatementType::DROP_ROW_POLICY:
		return "DROP ROW POLICY " + policy_name + " ON " + table_name;
	default:
		return "UNKNOWN RBAC STATEMENT";
	}
}

//===--------------------------------------------------------------------===//
// Table Function for RBAC DDL Execution
//===--------------------------------------------------------------------===//

struct RBACDDLBindData : public TableFunctionData {
	RBACStatementType statement_type;
	string role_name;
	string member_name;
	string schema_name;
	string table_name;
	vector<string> column_names;
	string policy_name;
	string filter_expression;
};

struct RBACDDLGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> RBACDDLBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("result");

	auto bind_data = make_uniq<RBACDDLBindData>();
	// Parameters are passed via named parameters set in PlanFunction
	// We'll extract them from the input
	idx_t param_idx = 0;
	bind_data->statement_type = static_cast<RBACStatementType>(input.inputs[param_idx++].GetValue<int32_t>());
	bind_data->role_name = input.inputs[param_idx++].GetValue<string>();
	bind_data->member_name = input.inputs[param_idx++].GetValue<string>();
	bind_data->schema_name = input.inputs[param_idx++].GetValue<string>();
	bind_data->table_name = input.inputs[param_idx++].GetValue<string>();
	bind_data->policy_name = input.inputs[param_idx++].GetValue<string>();
	bind_data->filter_expression = input.inputs[param_idx++].GetValue<string>();

	// Column names are passed as a comma-separated string
	string cols_str = input.inputs[param_idx++].GetValue<string>();
	if (!cols_str.empty()) {
		auto cols = StringUtil::Split(cols_str, ',');
		for (auto &col : cols) {
			StringUtil::Trim(col);
			if (!col.empty()) {
				bind_data->column_names.push_back(col);
			}
		}
	}

	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> RBACDDLInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<RBACDDLGlobalState>();
}

// Helper to execute SQL and return materialized result
static unique_ptr<MaterializedQueryResult> ExecuteSQL(ClientContext &context, const string &sql) {
	Connection con(*context.db);
	auto result = con.Query(sql);
	// Connection::Query returns MaterializedQueryResult
	return unique_ptr<MaterializedQueryResult>(static_cast<MaterializedQueryResult *>(result.release()));
}

// Helper to check if a query returned any rows
static bool HasRows(MaterializedQueryResult &result) {
	if (result.HasError()) {
		return false;
	}
	return result.RowCount() > 0;
}

static void RBACDDLExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<RBACDDLBindData>();
	auto &gstate = data.global_state->Cast<RBACDDLGlobalState>();

	if (gstate.done) {
		return;
	}

	string result_message;
	string sql;

	switch (bind_data.statement_type) {
	case RBACStatementType::CREATE_ROLE: {
		// Check if role already exists
		sql = StringUtil::Format("SELECT 1 FROM duckdb_roles WHERE role_name = '%s'",
		                         StringUtil::Replace(bind_data.role_name, "'", "''"));
		auto check_result = ExecuteSQL(context, sql);
		if (check_result->HasError()) {
			throw InvalidInputException("Failed to check role existence: %s", check_result->GetError());
		}
		if (HasRows(*check_result)) {
			throw InvalidInputException("Role '%s' already exists", bind_data.role_name);
		}

		// Insert the role
		sql = StringUtil::Format("INSERT INTO duckdb_roles (role_name) VALUES ('%s')",
		                         StringUtil::Replace(bind_data.role_name, "'", "''"));
		auto insert_result = ExecuteSQL(context, sql);
		if (insert_result->HasError()) {
			throw InvalidInputException("Failed to create role: %s", insert_result->GetError());
		}
		result_message = "Role created: " + bind_data.role_name;
		break;
	}

	case RBACStatementType::DROP_ROLE: {
		// Check if role exists
		sql = StringUtil::Format("SELECT 1 FROM duckdb_roles WHERE role_name = '%s'",
		                         StringUtil::Replace(bind_data.role_name, "'", "''"));
		auto check_result = ExecuteSQL(context, sql);
		if (check_result->HasError()) {
			throw InvalidInputException("Failed to check role existence: %s", check_result->GetError());
		}
		if (!HasRows(*check_result)) {
			throw InvalidInputException("Role '%s' does not exist", bind_data.role_name);
		}

		// Check if role has any grants (FR-3)
		sql = StringUtil::Format(
		    "SELECT 1 FROM duckdb_table_privileges WHERE grantee = '%s' "
		    "UNION ALL SELECT 1 FROM duckdb_column_privileges WHERE grantee = '%s' "
		    "UNION ALL SELECT 1 FROM duckdb_row_policies WHERE grantee = '%s' "
		    "UNION ALL SELECT 1 FROM duckdb_role_members WHERE role_name = '%s' OR member = '%s' "
		    "LIMIT 1",
		    StringUtil::Replace(bind_data.role_name, "'", "''"),
		    StringUtil::Replace(bind_data.role_name, "'", "''"),
		    StringUtil::Replace(bind_data.role_name, "'", "''"),
		    StringUtil::Replace(bind_data.role_name, "'", "''"),
		    StringUtil::Replace(bind_data.role_name, "'", "''"));
		auto grants_result = ExecuteSQL(context, sql);
		if (!grants_result->HasError() && HasRows(*grants_result)) {
			throw InvalidInputException("Cannot drop role '%s': role has existing grants or memberships. "
			                            "Remove grants first.", bind_data.role_name);
		}

		// Delete the role
		sql = StringUtil::Format("DELETE FROM duckdb_roles WHERE role_name = '%s'",
		                         StringUtil::Replace(bind_data.role_name, "'", "''"));
		auto delete_result = ExecuteSQL(context, sql);
		if (delete_result->HasError()) {
			throw InvalidInputException("Failed to drop role: %s", delete_result->GetError());
		}
		result_message = "Role dropped: " + bind_data.role_name;
		break;
	}

	case RBACStatementType::GRANT_ROLE: {
		// Check both roles exist
		for (const auto &rn : {bind_data.role_name, bind_data.member_name}) {
			sql = StringUtil::Format("SELECT 1 FROM duckdb_roles WHERE role_name = '%s'",
			                         StringUtil::Replace(rn, "'", "''"));
			auto check_result = ExecuteSQL(context, sql);
			if (check_result->HasError() || !HasRows(*check_result)) {
				throw InvalidInputException("Role '%s' does not exist", rn);
			}
		}

		// Insert membership (idempotent - use INSERT OR IGNORE pattern)
		sql = StringUtil::Format(
		    "INSERT INTO duckdb_role_members (role_name, member) "
		    "SELECT '%s', '%s' WHERE NOT EXISTS ("
		    "  SELECT 1 FROM duckdb_role_members WHERE role_name = '%s' AND member = '%s')",
		    StringUtil::Replace(bind_data.role_name, "'", "''"),
		    StringUtil::Replace(bind_data.member_name, "'", "''"),
		    StringUtil::Replace(bind_data.role_name, "'", "''"),
		    StringUtil::Replace(bind_data.member_name, "'", "''"));
		auto insert_result = ExecuteSQL(context, sql);
		if (insert_result->HasError()) {
			throw InvalidInputException("Failed to grant role: %s", insert_result->GetError());
		}
		result_message = "Role granted: " + bind_data.role_name + " TO " + bind_data.member_name;
		break;
	}

	case RBACStatementType::REVOKE_ROLE: {
		sql = StringUtil::Format(
		    "DELETE FROM duckdb_role_members WHERE role_name = '%s' AND member = '%s'",
		    StringUtil::Replace(bind_data.role_name, "'", "''"),
		    StringUtil::Replace(bind_data.member_name, "'", "''"));
		auto delete_result = ExecuteSQL(context, sql);
		if (delete_result->HasError()) {
			throw InvalidInputException("Failed to revoke role: %s", delete_result->GetError());
		}
		result_message = "Role revoked: " + bind_data.role_name + " FROM " + bind_data.member_name;
		break;
	}

	case RBACStatementType::GRANT_TABLE: {
		// Check role exists
		sql = StringUtil::Format("SELECT 1 FROM duckdb_roles WHERE role_name = '%s'",
		                         StringUtil::Replace(bind_data.role_name, "'", "''"));
		auto role_check = ExecuteSQL(context, sql);
		if (role_check->HasError() || !HasRows(*role_check)) {
			throw InvalidInputException("Role '%s' does not exist", bind_data.role_name);
		}

		// Check table exists (FR-8)
		sql = StringUtil::Format(
		    "SELECT 1 FROM information_schema.tables WHERE table_schema = '%s' AND table_name = '%s'",
		    StringUtil::Replace(bind_data.schema_name, "'", "''"),
		    StringUtil::Replace(bind_data.table_name, "'", "''"));
		auto table_check = ExecuteSQL(context, sql);
		if (table_check->HasError() || !HasRows(*table_check)) {
			throw InvalidInputException("Table '%s.%s' does not exist", bind_data.schema_name, bind_data.table_name);
		}

		// Insert grant (idempotent)
		sql = StringUtil::Format(
		    "INSERT INTO duckdb_table_privileges (grantee, table_schema, table_name, privilege_type) "
		    "SELECT '%s', '%s', '%s', 'SELECT' WHERE NOT EXISTS ("
		    "  SELECT 1 FROM duckdb_table_privileges WHERE grantee = '%s' AND table_schema = '%s' "
		    "  AND table_name = '%s' AND privilege_type = 'SELECT')",
		    StringUtil::Replace(bind_data.role_name, "'", "''"),
		    StringUtil::Replace(bind_data.schema_name, "'", "''"),
		    StringUtil::Replace(bind_data.table_name, "'", "''"),
		    StringUtil::Replace(bind_data.role_name, "'", "''"),
		    StringUtil::Replace(bind_data.schema_name, "'", "''"),
		    StringUtil::Replace(bind_data.table_name, "'", "''"));
		auto insert_result = ExecuteSQL(context, sql);
		if (insert_result->HasError()) {
			throw InvalidInputException("Failed to grant table privilege: %s", insert_result->GetError());
		}
		result_message = "Table privilege granted: SELECT ON " + bind_data.table_name + " TO " + bind_data.role_name;
		break;
	}

	case RBACStatementType::REVOKE_TABLE: {
		// Check role exists
		sql = StringUtil::Format("SELECT 1 FROM duckdb_roles WHERE role_name = '%s'",
		                         StringUtil::Replace(bind_data.role_name, "'", "''"));
		auto role_check = ExecuteSQL(context, sql);
		if (role_check->HasError() || !HasRows(*role_check)) {
			throw InvalidInputException("Role '%s' does not exist", bind_data.role_name);
		}

		sql = StringUtil::Format(
		    "DELETE FROM duckdb_table_privileges WHERE grantee = '%s' AND table_schema = '%s' "
		    "AND table_name = '%s' AND privilege_type = 'SELECT'",
		    StringUtil::Replace(bind_data.role_name, "'", "''"),
		    StringUtil::Replace(bind_data.schema_name, "'", "''"),
		    StringUtil::Replace(bind_data.table_name, "'", "''"));
		auto delete_result = ExecuteSQL(context, sql);
		if (delete_result->HasError()) {
			throw InvalidInputException("Failed to revoke table privilege: %s", delete_result->GetError());
		}
		result_message = "Table privilege revoked: SELECT ON " + bind_data.table_name + " FROM " + bind_data.role_name;
		break;
	}

	case RBACStatementType::GRANT_COLUMN: {
		// Check role exists
		sql = StringUtil::Format("SELECT 1 FROM duckdb_roles WHERE role_name = '%s'",
		                         StringUtil::Replace(bind_data.role_name, "'", "''"));
		auto role_check = ExecuteSQL(context, sql);
		if (role_check->HasError() || !HasRows(*role_check)) {
			throw InvalidInputException("Role '%s' does not exist", bind_data.role_name);
		}

		// Check table exists
		sql = StringUtil::Format(
		    "SELECT 1 FROM information_schema.tables WHERE table_schema = '%s' AND table_name = '%s'",
		    StringUtil::Replace(bind_data.schema_name, "'", "''"),
		    StringUtil::Replace(bind_data.table_name, "'", "''"));
		auto table_check = ExecuteSQL(context, sql);
		if (table_check->HasError() || !HasRows(*table_check)) {
			throw InvalidInputException("Table '%s.%s' does not exist", bind_data.schema_name, bind_data.table_name);
		}

		// Check columns exist and insert grants
		for (const auto &col : bind_data.column_names) {
			sql = StringUtil::Format(
			    "SELECT 1 FROM information_schema.columns WHERE table_schema = '%s' AND table_name = '%s' "
			    "AND column_name = '%s'",
			    StringUtil::Replace(bind_data.schema_name, "'", "''"),
			    StringUtil::Replace(bind_data.table_name, "'", "''"),
			    StringUtil::Replace(col, "'", "''"));
			auto col_check = ExecuteSQL(context, sql);
			if (col_check->HasError() || !HasRows(*col_check)) {
				throw InvalidInputException("Column '%s' does not exist in table '%s.%s'",
				                            col, bind_data.schema_name, bind_data.table_name);
			}

			// Insert column grant (idempotent)
			sql = StringUtil::Format(
			    "INSERT INTO duckdb_column_privileges (grantee, table_schema, table_name, column_name, privilege_type) "
			    "SELECT '%s', '%s', '%s', '%s', 'SELECT' WHERE NOT EXISTS ("
			    "  SELECT 1 FROM duckdb_column_privileges WHERE grantee = '%s' AND table_schema = '%s' "
			    "  AND table_name = '%s' AND column_name = '%s' AND privilege_type = 'SELECT')",
			    StringUtil::Replace(bind_data.role_name, "'", "''"),
			    StringUtil::Replace(bind_data.schema_name, "'", "''"),
			    StringUtil::Replace(bind_data.table_name, "'", "''"),
			    StringUtil::Replace(col, "'", "''"),
			    StringUtil::Replace(bind_data.role_name, "'", "''"),
			    StringUtil::Replace(bind_data.schema_name, "'", "''"),
			    StringUtil::Replace(bind_data.table_name, "'", "''"),
			    StringUtil::Replace(col, "'", "''"));
			auto insert_result = ExecuteSQL(context, sql);
			if (insert_result->HasError()) {
				throw InvalidInputException("Failed to grant column privilege: %s", insert_result->GetError());
			}
		}
		result_message = "Column privileges granted on " + bind_data.table_name + " TO " + bind_data.role_name;
		break;
	}

	case RBACStatementType::REVOKE_COLUMN: {
		// Check role exists
		sql = StringUtil::Format("SELECT 1 FROM duckdb_roles WHERE role_name = '%s'",
		                         StringUtil::Replace(bind_data.role_name, "'", "''"));
		auto role_check = ExecuteSQL(context, sql);
		if (role_check->HasError() || !HasRows(*role_check)) {
			throw InvalidInputException("Role '%s' does not exist", bind_data.role_name);
		}

		for (const auto &col : bind_data.column_names) {
			sql = StringUtil::Format(
			    "DELETE FROM duckdb_column_privileges WHERE grantee = '%s' AND table_schema = '%s' "
			    "AND table_name = '%s' AND column_name = '%s' AND privilege_type = 'SELECT'",
			    StringUtil::Replace(bind_data.role_name, "'", "''"),
			    StringUtil::Replace(bind_data.schema_name, "'", "''"),
			    StringUtil::Replace(bind_data.table_name, "'", "''"),
			    StringUtil::Replace(col, "'", "''"));
			auto delete_result = ExecuteSQL(context, sql);
			if (delete_result->HasError()) {
				throw InvalidInputException("Failed to revoke column privilege: %s", delete_result->GetError());
			}
		}
		result_message = "Column privileges revoked on " + bind_data.table_name + " FROM " + bind_data.role_name;
		break;
	}

	case RBACStatementType::CREATE_ROW_POLICY: {
		// Check role exists
		sql = StringUtil::Format("SELECT 1 FROM duckdb_roles WHERE role_name = '%s'",
		                         StringUtil::Replace(bind_data.role_name, "'", "''"));
		auto role_check = ExecuteSQL(context, sql);
		if (role_check->HasError() || !HasRows(*role_check)) {
			throw InvalidInputException("Role '%s' does not exist", bind_data.role_name);
		}

		// Check table exists
		sql = StringUtil::Format(
		    "SELECT 1 FROM information_schema.tables WHERE table_schema = '%s' AND table_name = '%s'",
		    StringUtil::Replace(bind_data.schema_name, "'", "''"),
		    StringUtil::Replace(bind_data.table_name, "'", "''"));
		auto table_check = ExecuteSQL(context, sql);
		if (table_check->HasError() || !HasRows(*table_check)) {
			throw InvalidInputException("Table '%s.%s' does not exist", bind_data.schema_name, bind_data.table_name);
		}

		// Get table columns for expression validation
		sql = StringUtil::Format(
		    "SELECT column_name FROM information_schema.columns WHERE table_schema = '%s' AND table_name = '%s'",
		    StringUtil::Replace(bind_data.schema_name, "'", "''"),
		    StringUtil::Replace(bind_data.table_name, "'", "''"));
		auto cols_result = ExecuteSQL(context, sql);
		unordered_set<string> table_columns;
		if (!cols_result->HasError()) {
			for (auto &row : *cols_result) {
				table_columns.insert(StringUtil::Lower(row.GetValue<string>(0)));
			}
		}

		// Validate filter expression (Q79: fail fast if column doesn't exist)
		// Try to parse the expression to extract column references
		try {
			auto expressions = Parser::ParseExpressionList(bind_data.filter_expression);
			if (expressions.empty()) {
				throw InvalidInputException("Invalid filter expression");
			}
			// Check column references in the expression
			std::function<void(ParsedExpression &)> validate_columns;
			validate_columns = [&](ParsedExpression &expr) {
				if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
					auto &col_ref = expr.Cast<ColumnRefExpression>();
					string col_name;
					if (!col_ref.column_names.empty()) {
						col_name = StringUtil::Lower(col_ref.column_names.back());
					}
					if (!col_name.empty() && table_columns.find(col_name) == table_columns.end()) {
						throw InvalidInputException("Column '%s' does not exist in table '%s.%s'",
						                            col_ref.column_names.back(),
						                            bind_data.schema_name, bind_data.table_name);
					}
				}
				// Recurse into children
				ParsedExpressionIterator::EnumerateChildren(expr, [&](ParsedExpression &child) {
					validate_columns(child);
				});
			};
			validate_columns(*expressions[0]);
		} catch (const ParserException &e) {
			throw InvalidInputException("Invalid filter expression: %s", e.what());
		}

		// Check policy doesn't already exist
		sql = StringUtil::Format(
		    "SELECT 1 FROM duckdb_row_policies WHERE policy_name = '%s' AND table_schema = '%s' AND table_name = '%s'",
		    StringUtil::Replace(bind_data.policy_name, "'", "''"),
		    StringUtil::Replace(bind_data.schema_name, "'", "''"),
		    StringUtil::Replace(bind_data.table_name, "'", "''"));
		auto policy_check = ExecuteSQL(context, sql);
		if (!policy_check->HasError() && HasRows(*policy_check)) {
			throw InvalidInputException("Row policy '%s' already exists on table '%s'",
			                            bind_data.policy_name, bind_data.table_name);
		}

		// Insert the policy
		sql = StringUtil::Format(
		    "INSERT INTO duckdb_row_policies (policy_name, table_schema, table_name, filter_expression, grantee) "
		    "VALUES ('%s', '%s', '%s', '%s', '%s')",
		    StringUtil::Replace(bind_data.policy_name, "'", "''"),
		    StringUtil::Replace(bind_data.schema_name, "'", "''"),
		    StringUtil::Replace(bind_data.table_name, "'", "''"),
		    StringUtil::Replace(bind_data.filter_expression, "'", "''"),
		    StringUtil::Replace(bind_data.role_name, "'", "''"));
		auto insert_result = ExecuteSQL(context, sql);
		if (insert_result->HasError()) {
			throw InvalidInputException("Failed to create row policy: %s", insert_result->GetError());
		}
		result_message = "Row policy created: " + bind_data.policy_name + " ON " + bind_data.table_name;
		break;
	}

	case RBACStatementType::DROP_ROW_POLICY: {
		// Check policy exists first
		sql = StringUtil::Format(
		    "SELECT 1 FROM duckdb_row_policies WHERE policy_name = '%s' AND table_schema = '%s' AND table_name = '%s'",
		    StringUtil::Replace(bind_data.policy_name, "'", "''"),
		    StringUtil::Replace(bind_data.schema_name, "'", "''"),
		    StringUtil::Replace(bind_data.table_name, "'", "''"));
		auto policy_check = ExecuteSQL(context, sql);
		if (policy_check->HasError() || !HasRows(*policy_check)) {
			throw InvalidInputException("Row policy '%s' does not exist on table '%s'",
			                            bind_data.policy_name, bind_data.table_name);
		}

		sql = StringUtil::Format(
		    "DELETE FROM duckdb_row_policies WHERE policy_name = '%s' AND table_schema = '%s' AND table_name = '%s'",
		    StringUtil::Replace(bind_data.policy_name, "'", "''"),
		    StringUtil::Replace(bind_data.schema_name, "'", "''"),
		    StringUtil::Replace(bind_data.table_name, "'", "''"));
		auto delete_result = ExecuteSQL(context, sql);
		if (delete_result->HasError()) {
			throw InvalidInputException("Failed to drop row policy: %s", delete_result->GetError());
		}
		result_message = "Row policy dropped: " + bind_data.policy_name + " ON " + bind_data.table_name;
		break;
	}

	default:
		result_message = "Unknown RBAC operation";
		break;
	}

	output.SetCardinality(1);
	output.SetValue(0, 0, Value(result_message));
	gstate.done = true;
}

static TableFunction GetRBACDDLFunction() {
	// Parameters: statement_type, role_name, member_name, schema_name, table_name, policy_name, filter_expr, columns
	TableFunction func("rbac_ddl_execute",
	                   {LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                    LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                   RBACDDLExecute, RBACDDLBind, RBACDDLInit);
	return func;
}

//===--------------------------------------------------------------------===//
// Parsing Helpers
//===--------------------------------------------------------------------===//
bool RBACParserExtension::TryParseCreateRole(const string &upper, const string &original, unique_ptr<RBACParseData> &out) {
	if (!StringUtil::StartsWith(upper, "CREATE ROLE ")) {
		return false;
	}

	string role_name = original.substr(12);
	StringUtil::Trim(role_name);

	if (role_name.empty()) {
		throw ParserException("CREATE ROLE requires a role name");
	}

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::CREATE_ROLE;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseDropRole(const string &upper, const string &original, unique_ptr<RBACParseData> &out) {
	if (!StringUtil::StartsWith(upper, "DROP ROLE ")) {
		return false;
	}

	string role_name = original.substr(10);
	StringUtil::Trim(role_name);

	if (role_name.empty()) {
		throw ParserException("DROP ROLE requires a role name");
	}

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::DROP_ROLE;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseGrantRole(const string &upper, const string &original, unique_ptr<RBACParseData> &out) {
	// GRANT role_name TO member_name
	// Must NOT have SELECT (that's table/column grant)
	if (!StringUtil::StartsWith(upper, "GRANT ") || StringUtil::Contains(upper, " SELECT")) {
		return false;
	}

	// Find TO keyword
	size_t to_pos = upper.find(" TO ");
	if (to_pos == string::npos) {
		return false;
	}

	string role_name = original.substr(6, to_pos - 6);
	StringUtil::Trim(role_name);

	string member_name = original.substr(to_pos + 4);
	StringUtil::Trim(member_name);

	if (role_name.empty() || member_name.empty()) {
		throw ParserException("GRANT role TO member requires both role and member names");
	}

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::GRANT_ROLE;
	out->role_name = role_name;
	out->member_name = member_name;
	return true;
}

bool RBACParserExtension::TryParseRevokeRole(const string &upper, const string &original, unique_ptr<RBACParseData> &out) {
	// REVOKE role_name FROM member_name
	if (!StringUtil::StartsWith(upper, "REVOKE ") || StringUtil::Contains(upper, " SELECT")) {
		return false;
	}

	size_t from_pos = upper.find(" FROM ");
	if (from_pos == string::npos) {
		return false;
	}

	string role_name = original.substr(7, from_pos - 7);
	StringUtil::Trim(role_name);

	string member_name = original.substr(from_pos + 6);
	StringUtil::Trim(member_name);

	if (role_name.empty() || member_name.empty()) {
		throw ParserException("REVOKE role FROM member requires both role and member names");
	}

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::REVOKE_ROLE;
	out->role_name = role_name;
	out->member_name = member_name;
	return true;
}

bool RBACParserExtension::TryParseGrantTable(const string &upper, const string &original, unique_ptr<RBACParseData> &out) {
	// GRANT SELECT ON table_name TO role_name (no parentheses for columns)
	if (!StringUtil::StartsWith(upper, "GRANT SELECT ON ")) {
		return false;
	}

	// Check for column list (parentheses)
	size_t select_pos = upper.find("SELECT");
	size_t on_pos = upper.find(" ON ");
	if (on_pos == string::npos) return false;

	// If there's a ( between SELECT and ON, it's a column grant
	string between = upper.substr(select_pos + 6, on_pos - select_pos - 6);
	if (StringUtil::Contains(between, "(")) {
		return false;  // Column grant, not table grant
	}

	size_t to_pos = upper.find(" TO ");
	if (to_pos == string::npos) {
		throw ParserException("GRANT SELECT ON table requires TO role_name");
	}

	string table_part = original.substr(on_pos + 4, to_pos - on_pos - 4);
	StringUtil::Trim(table_part);

	string role_name = original.substr(to_pos + 4);
	StringUtil::Trim(role_name);

	// Parse schema.table or just table
	string schema_name = "main";
	string table_name = table_part;
	size_t dot_pos = table_part.find('.');
	if (dot_pos != string::npos) {
		schema_name = table_part.substr(0, dot_pos);
		table_name = table_part.substr(dot_pos + 1);
	}

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::GRANT_TABLE;
	out->schema_name = schema_name;
	out->table_name = table_name;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseRevokeTable(const string &upper, const string &original, unique_ptr<RBACParseData> &out) {
	// REVOKE SELECT ON table_name FROM role_name
	if (!StringUtil::StartsWith(upper, "REVOKE SELECT ON ")) {
		return false;
	}

	// Check for column list
	size_t select_pos = upper.find("SELECT");
	size_t on_pos = upper.find(" ON ");
	if (on_pos == string::npos) return false;

	string between = upper.substr(select_pos + 6, on_pos - select_pos - 6);
	if (StringUtil::Contains(between, "(")) {
		return false;
	}

	size_t from_pos = upper.find(" FROM ");
	if (from_pos == string::npos) {
		throw ParserException("REVOKE SELECT ON table requires FROM role_name");
	}

	string table_part = original.substr(on_pos + 4, from_pos - on_pos - 4);
	StringUtil::Trim(table_part);

	string role_name = original.substr(from_pos + 6);
	StringUtil::Trim(role_name);

	string schema_name = "main";
	string table_name = table_part;
	size_t dot_pos = table_part.find('.');
	if (dot_pos != string::npos) {
		schema_name = table_part.substr(0, dot_pos);
		table_name = table_part.substr(dot_pos + 1);
	}

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::REVOKE_TABLE;
	out->schema_name = schema_name;
	out->table_name = table_name;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseGrantColumn(const string &upper, const string &original, unique_ptr<RBACParseData> &out) {
	// GRANT SELECT (col1, col2) ON table_name TO role_name
	if (!StringUtil::StartsWith(upper, "GRANT SELECT ") || !StringUtil::Contains(upper, "(")) {
		return false;
	}

	size_t open_paren = original.find('(');
	size_t close_paren = original.find(')');
	if (open_paren == string::npos || close_paren == string::npos || close_paren < open_paren) {
		return false;
	}

	// Extract columns
	string cols_str = original.substr(open_paren + 1, close_paren - open_paren - 1);
	vector<string> columns;
	auto col_parts = StringUtil::Split(cols_str, ',');
	for (auto &col : col_parts) {
		StringUtil::Trim(col);
		if (!col.empty()) {
			columns.push_back(col);
		}
	}

	if (columns.empty()) {
		throw ParserException("GRANT SELECT requires at least one column");
	}

	// Find ON and TO
	string rest = original.substr(close_paren + 1);
	string rest_upper = StringUtil::Upper(rest);

	size_t on_pos = rest_upper.find(" ON ");
	size_t to_pos = rest_upper.find(" TO ");
	if (on_pos == string::npos || to_pos == string::npos || to_pos < on_pos) {
		throw ParserException("GRANT SELECT (columns) requires ON table TO role");
	}

	string table_part = rest.substr(on_pos + 4, to_pos - on_pos - 4);
	StringUtil::Trim(table_part);

	string role_name = rest.substr(to_pos + 4);
	StringUtil::Trim(role_name);

	string schema_name = "main";
	string table_name = table_part;
	size_t dot_pos = table_part.find('.');
	if (dot_pos != string::npos) {
		schema_name = table_part.substr(0, dot_pos);
		table_name = table_part.substr(dot_pos + 1);
	}

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::GRANT_COLUMN;
	out->schema_name = schema_name;
	out->table_name = table_name;
	out->column_names = columns;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseRevokeColumn(const string &upper, const string &original, unique_ptr<RBACParseData> &out) {
	// REVOKE SELECT (col1, col2) ON table_name FROM role_name
	if (!StringUtil::StartsWith(upper, "REVOKE SELECT ") || !StringUtil::Contains(upper, "(")) {
		return false;
	}

	size_t open_paren = original.find('(');
	size_t close_paren = original.find(')');
	if (open_paren == string::npos || close_paren == string::npos || close_paren < open_paren) {
		return false;
	}

	string cols_str = original.substr(open_paren + 1, close_paren - open_paren - 1);
	vector<string> columns;
	auto col_parts = StringUtil::Split(cols_str, ',');
	for (auto &col : col_parts) {
		StringUtil::Trim(col);
		if (!col.empty()) {
			columns.push_back(col);
		}
	}

	string rest = original.substr(close_paren + 1);
	string rest_upper = StringUtil::Upper(rest);

	size_t on_pos = rest_upper.find(" ON ");
	size_t from_pos = rest_upper.find(" FROM ");
	if (on_pos == string::npos || from_pos == string::npos || from_pos < on_pos) {
		throw ParserException("REVOKE SELECT (columns) requires ON table FROM role");
	}

	string table_part = rest.substr(on_pos + 4, from_pos - on_pos - 4);
	StringUtil::Trim(table_part);

	string role_name = rest.substr(from_pos + 6);
	StringUtil::Trim(role_name);

	string schema_name = "main";
	string table_name = table_part;
	size_t dot_pos = table_part.find('.');
	if (dot_pos != string::npos) {
		schema_name = table_part.substr(0, dot_pos);
		table_name = table_part.substr(dot_pos + 1);
	}

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::REVOKE_COLUMN;
	out->schema_name = schema_name;
	out->table_name = table_name;
	out->column_names = columns;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseCreateRowPolicy(const string &upper, const string &original, unique_ptr<RBACParseData> &out) {
	// CREATE ROW POLICY name ON table FOR SELECT USING (expr) TO role
	if (!StringUtil::StartsWith(upper, "CREATE ROW POLICY ")) {
		return false;
	}

	// Find key positions
	size_t on_pos = upper.find(" ON ");
	size_t using_pos = upper.find(" USING ");
	size_t to_pos = upper.rfind(" TO ");  // Use rfind to get last TO

	if (on_pos == string::npos || using_pos == string::npos || to_pos == string::npos) {
		throw ParserException("CREATE ROW POLICY requires: name ON table FOR SELECT USING (expr) TO role");
	}

	// Extract policy name
	string policy_name = original.substr(18, on_pos - 18);
	StringUtil::Trim(policy_name);

	// Extract table name (between ON and FOR/USING)
	size_t for_pos = upper.find(" FOR ");
	size_t table_end = (for_pos != string::npos && for_pos < using_pos) ? for_pos : using_pos;
	string table_part = original.substr(on_pos + 4, table_end - on_pos - 4);
	StringUtil::Trim(table_part);

	// Extract USING expression (between parentheses)
	size_t expr_start = original.find('(', using_pos);
	size_t expr_end = original.rfind(')', to_pos);
	if (expr_start == string::npos || expr_end == string::npos || expr_end < expr_start) {
		throw ParserException("CREATE ROW POLICY: USING clause requires (expression)");
	}
	string filter_expr = original.substr(expr_start + 1, expr_end - expr_start - 1);
	StringUtil::Trim(filter_expr);

	// Extract role name
	string role_name = original.substr(to_pos + 4);
	StringUtil::Trim(role_name);

	// Parse schema.table
	string schema_name = "main";
	string table_name = table_part;
	size_t dot_pos = table_part.find('.');
	if (dot_pos != string::npos) {
		schema_name = table_part.substr(0, dot_pos);
		table_name = table_part.substr(dot_pos + 1);
	}

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::CREATE_ROW_POLICY;
	out->policy_name = policy_name;
	out->schema_name = schema_name;
	out->table_name = table_name;
	out->filter_expression = filter_expr;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseDropRowPolicy(const string &upper, const string &original, unique_ptr<RBACParseData> &out) {
	// DROP ROW POLICY name ON table
	if (!StringUtil::StartsWith(upper, "DROP ROW POLICY ")) {
		return false;
	}

	size_t on_pos = upper.find(" ON ");
	if (on_pos == string::npos) {
		throw ParserException("DROP ROW POLICY requires: name ON table");
	}

	string policy_name = original.substr(16, on_pos - 16);
	StringUtil::Trim(policy_name);

	string table_part = original.substr(on_pos + 4);
	StringUtil::Trim(table_part);

	string schema_name = "main";
	string table_name = table_part;
	size_t dot_pos = table_part.find('.');
	if (dot_pos != string::npos) {
		schema_name = table_part.substr(0, dot_pos);
		table_name = table_part.substr(dot_pos + 1);
	}

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::DROP_ROW_POLICY;
	out->policy_name = policy_name;
	out->schema_name = schema_name;
	out->table_name = table_name;
	return true;
}

//===--------------------------------------------------------------------===//
// RBACParserExtension
//===--------------------------------------------------------------------===//

RBACParserExtension::RBACParserExtension() {
	parse_function = ParseFunction;
	plan_function = PlanFunction;
	parser_override = ParserOverride;
}

ParserExtensionParseResult RBACParserExtension::ParseFunction(ParserExtensionInfo *info, const string &query) {
	// Normalize the query
	string trimmed = query;
	StringUtil::Trim(trimmed);
	string upper = StringUtil::Upper(trimmed);

	// Remove trailing semicolon
	if (StringUtil::EndsWith(upper, ";")) {
		upper = upper.substr(0, upper.length() - 1);
		trimmed = trimmed.substr(0, trimmed.length() - 1);
	}
	StringUtil::Trim(upper);
	StringUtil::Trim(trimmed);

	unique_ptr<RBACParseData> parse_data;

	// Try each parser in order
	try {
		if (TryParseCreateRole(upper, trimmed, parse_data) ||
		    TryParseDropRole(upper, trimmed, parse_data) ||
		    TryParseGrantColumn(upper, trimmed, parse_data) ||  // Check column before table
		    TryParseRevokeColumn(upper, trimmed, parse_data) ||
		    TryParseGrantTable(upper, trimmed, parse_data) ||
		    TryParseRevokeTable(upper, trimmed, parse_data) ||
		    TryParseGrantRole(upper, trimmed, parse_data) ||
		    TryParseRevokeRole(upper, trimmed, parse_data) ||
		    TryParseCreateRowPolicy(upper, trimmed, parse_data) ||
		    TryParseDropRowPolicy(upper, trimmed, parse_data)) {
			return ParserExtensionParseResult(std::move(parse_data));
		}
	} catch (Exception &e) {
		return ParserExtensionParseResult(e.what());
	}

	// Not a statement we handle
	return ParserExtensionParseResult();
}

ParserExtensionPlanResult RBACParserExtension::PlanFunction(ParserExtensionInfo *info, ClientContext &context,
                                                            unique_ptr<ParserExtensionParseData> parse_data) {
	auto &rbac_data = static_cast<RBACParseData &>(*parse_data);

	ParserExtensionPlanResult result;
	result.function = GetRBACDDLFunction();
	result.requires_valid_transaction = true;
	result.return_type = StatementReturnType::QUERY_RESULT;

	// Pack all data into parameters
	result.parameters.push_back(Value::INTEGER(static_cast<int32_t>(rbac_data.statement_type)));
	result.parameters.push_back(Value(rbac_data.role_name));
	result.parameters.push_back(Value(rbac_data.member_name));
	result.parameters.push_back(Value(rbac_data.schema_name));
	result.parameters.push_back(Value(rbac_data.table_name));
	result.parameters.push_back(Value(rbac_data.policy_name));
	result.parameters.push_back(Value(rbac_data.filter_expression));

	// Pack columns as comma-separated string
	string cols_str;
	for (size_t i = 0; i < rbac_data.column_names.size(); i++) {
		if (i > 0) cols_str += ",";
		cols_str += rbac_data.column_names[i];
	}
	result.parameters.push_back(Value(cols_str));

	return result;
}

//===--------------------------------------------------------------------===//
// Spike 0.6C: Parser Override (keep for now)
//===--------------------------------------------------------------------===//

ParserOverrideResult RBACParserExtension::ParserOverride(ParserExtensionInfo *info, const string &query) {
	string trimmed = query;
	StringUtil::Trim(trimmed);
	string upper = StringUtil::Upper(trimmed);

	if (StringUtil::EndsWith(upper, ";")) {
		upper = upper.substr(0, upper.length() - 1);
		StringUtil::Trim(upper);
	}

	// Spike 0.6C: SELECT * rewriting for col_test_rewrite
	if (upper == "SELECT * FROM COL_TEST_REWRITE" ||
	    upper == "SELECT * FROM COL_TEST_REWRITE ORDER BY A") {
		fprintf(stderr, "[Spike 0.6C] Intercepted: %s\n", query.c_str());

		string rewritten;
		if (upper == "SELECT * FROM COL_TEST_REWRITE ORDER BY A") {
			rewritten = "SELECT a, b FROM col_test_rewrite ORDER BY a";
		} else {
			rewritten = "SELECT a, b FROM col_test_rewrite";
		}

		fprintf(stderr, "[Spike 0.6C] Rewriting to: %s\n", rewritten.c_str());

		try {
			Parser parser;
			parser.ParseQuery(rewritten);

			ParserOverrideResult result;
			result.type = ParserExtensionResultType::PARSE_SUCCESSFUL;
			result.statements = std::move(parser.statements);
			return result;
		} catch (Exception &e) {
			ParserOverrideResult result;
			result.type = ParserExtensionResultType::DISPLAY_ORIGINAL_ERROR;
			result.error = ErrorData(e);
			return result;
		}
	}

	ParserOverrideResult result;
	result.type = ParserExtensionResultType::DISPLAY_ORIGINAL_ERROR;
	return result;
}

void RegisterRBACParser(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &callback_manager = ExtensionCallbackManager::Get(db);
	callback_manager.Register(RBACParserExtension());
}

} // namespace duckdb
