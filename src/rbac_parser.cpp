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
// RBACTokenStream Implementation
//===--------------------------------------------------------------------===//

RBACTokenStream::RBACTokenStream(const string &query) : query_(query), pos_(0) {
	tokens_ = Parser::Tokenize(query);
}

bool RBACTokenStream::HasMore() const {
	return pos_ < tokens_.size();
}

void RBACTokenStream::Advance() {
	if (pos_ < tokens_.size()) {
		pos_++;
	}
}

SimplifiedTokenType RBACTokenStream::CurrentType() const {
	if (pos_ >= tokens_.size()) {
		throw ParserException("Unexpected end of statement");
	}
	return tokens_[pos_].type;
}

idx_t RBACTokenStream::CurrentEnd() const {
	if (pos_ + 1 < tokens_.size()) {
		return tokens_[pos_ + 1].start;
	}
	return query_.size();
}

string RBACTokenStream::CurrentText() const {
	if (pos_ >= tokens_.size()) {
		return "";
	}
	idx_t start = tokens_[pos_].start;
	idx_t end = CurrentEnd();
	// Trim whitespace from the end
	while (end > start && StringUtil::CharacterIsSpace(query_[end - 1])) {
		end--;
	}
	return query_.substr(start, end - start);
}

string RBACTokenStream::RemainingText() const {
	if (pos_ >= tokens_.size()) {
		return "";
	}
	return query_.substr(tokens_[pos_].start);
}

bool RBACTokenStream::IsKeyword(const string &kw) const {
	if (pos_ >= tokens_.size()) {
		return false;
	}
	if (tokens_[pos_].type != SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD) {
		return false;
	}
	return StringUtil::CIEquals(CurrentText(), kw);
}

bool RBACTokenStream::MatchKeyword(const string &kw) {
	if (IsKeyword(kw)) {
		Advance();
		return true;
	}
	return false;
}

void RBACTokenStream::ExpectKeyword(const string &kw) {
	if (!MatchKeyword(kw)) {
		throw ParserException("Expected keyword '%s'", kw);
	}
}

bool RBACTokenStream::IsOperator(char op) const {
	if (pos_ >= tokens_.size()) {
		return false;
	}
	if (tokens_[pos_].type != SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR) {
		return false;
	}
	string text = CurrentText();
	return text.size() == 1 && text[0] == op;
}

bool RBACTokenStream::MatchOperator(char op) {
	if (IsOperator(op)) {
		Advance();
		return true;
	}
	return false;
}

string RBACTokenStream::ExtractIdentifier(idx_t start, idx_t end) const {
	string text = query_.substr(start, end - start);
	// Trim whitespace
	StringUtil::Trim(text);

	if (text.empty()) {
		return text;
	}

	// Handle quoted identifier
	if (text.front() == '"' && text.back() == '"' && text.size() >= 2) {
		// Remove outer quotes
		text = text.substr(1, text.size() - 2);
		// Unescape doubled quotes ("" -> ")
		text = StringUtil::Replace(text, "\"\"", "\"");
	}
	return text;
}

string RBACTokenStream::ConsumeIdentifier() {
	if (pos_ >= tokens_.size()) {
		throw ParserException("Expected identifier");
	}

	auto type = tokens_[pos_].type;
	// Accept IDENTIFIER, KEYWORD (for reserved words used as identifiers), or STRING_CONSTANT (for quoted)
	if (type != SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER &&
	    type != SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD) {
		throw ParserException("Expected identifier, got %s", CurrentText());
	}

	string result = ExtractIdentifier(tokens_[pos_].start, CurrentEnd());
	if (result.empty()) {
		throw ParserException("Expected identifier");
	}

	// Validate: reject whitespace-only identifiers
	string trimmed = result;
	StringUtil::Trim(trimmed);
	if (trimmed.empty()) {
		throw ParserException("Identifier requires a role name");
	}

	Advance();
	return result;
}

QualifiedName RBACTokenStream::ConsumeTableRef() {
	if (pos_ >= tokens_.size()) {
		throw ParserException("Expected table reference");
	}

	// Collect tokens that form the qualified name (identifier . identifier . identifier)
	string qualified_str;
	bool expect_dot = false;

	while (HasMore()) {
		if (expect_dot) {
			if (IsOperator('.')) {
				qualified_str += '.';
				Advance();
				expect_dot = false;
			} else {
				break;  // End of qualified name
			}
		} else {
			auto type = CurrentType();
			if (type == SimplifiedTokenType::SIMPLIFIED_TOKEN_IDENTIFIER ||
			    type == SimplifiedTokenType::SIMPLIFIED_TOKEN_KEYWORD) {
				qualified_str += CurrentText();
				Advance();
				expect_dot = true;
			} else {
				break;
			}
		}
	}

	if (qualified_str.empty()) {
		throw ParserException("Expected table reference");
	}

	// Use DuckDB's QualifiedName::Parse for proper handling
	QualifiedName qn = QualifiedName::Parse(qualified_str);

	// Default schema to "main" if not specified
	if (qn.schema == INVALID_SCHEMA || qn.schema.empty()) {
		qn.schema = "main";
	}

	return qn;
}

vector<string> RBACTokenStream::ConsumeColumnList() {
	// Expect opening parenthesis
	if (!MatchOperator('(')) {
		throw ParserException("Expected '(' for column list");
	}

	vector<string> columns;

	while (HasMore() && !IsOperator(')')) {
		// Get column name
		string col = ConsumeIdentifier();
		columns.push_back(col);

		// Check for comma or end
		if (IsOperator(',')) {
			Advance();
		} else if (!IsOperator(')')) {
			throw ParserException("Expected ',' or ')' in column list");
		}
	}

	// Expect closing parenthesis
	if (!MatchOperator(')')) {
		throw ParserException("Expected ')' to close column list");
	}

	if (columns.empty()) {
		throw ParserException("Column list cannot be empty");
	}

	return columns;
}

string RBACTokenStream::ConsumeUntilKeyword(const string &kw) {
	if (pos_ >= tokens_.size()) {
		return "";
	}

	idx_t start = tokens_[pos_].start;
	idx_t end = start;

	// Find the keyword
	while (HasMore() && !IsKeyword(kw)) {
		end = CurrentEnd();
		Advance();
	}

	string result = query_.substr(start, end - start);
	StringUtil::Trim(result);
	return result;
}

//===--------------------------------------------------------------------===//
// Token-based Parsing Helpers
//===--------------------------------------------------------------------===//

bool RBACParserExtension::TryParseCreateRole(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out) {
	// CREATE ROLE name
	if (!tokens.MatchKeyword("CREATE")) {
		return false;
	}
	if (!tokens.MatchKeyword("ROLE")) {
		return false;
	}

	if (!tokens.HasMore()) {
		throw ParserException("CREATE ROLE requires a role name");
	}

	string role_name = tokens.ConsumeIdentifier();

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::CREATE_ROLE;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseDropRole(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out) {
	// DROP ROLE name
	if (!tokens.MatchKeyword("DROP")) {
		return false;
	}
	if (!tokens.MatchKeyword("ROLE")) {
		return false;
	}

	if (!tokens.HasMore()) {
		throw ParserException("DROP ROLE requires a role name");
	}

	string role_name = tokens.ConsumeIdentifier();

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::DROP_ROLE;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseGrantRole(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out) {
	// GRANT role_name TO member_name (no SELECT keyword)
	if (!tokens.MatchKeyword("GRANT")) {
		return false;
	}

	// If next token is SELECT, this is a table/column grant
	if (tokens.IsKeyword("SELECT")) {
		return false;
	}

	string role_name = tokens.ConsumeIdentifier();

	if (!tokens.MatchKeyword("TO")) {
		return false;  // Not a role grant
	}

	string member_name = tokens.ConsumeIdentifier();

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::GRANT_ROLE;
	out->role_name = role_name;
	out->member_name = member_name;
	return true;
}

bool RBACParserExtension::TryParseRevokeRole(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out) {
	// REVOKE role_name FROM member_name (no SELECT keyword)
	if (!tokens.MatchKeyword("REVOKE")) {
		return false;
	}

	// If next token is SELECT, this is a table/column revoke
	if (tokens.IsKeyword("SELECT")) {
		return false;
	}

	string role_name = tokens.ConsumeIdentifier();

	if (!tokens.MatchKeyword("FROM")) {
		return false;  // Not a role revoke
	}

	string member_name = tokens.ConsumeIdentifier();

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::REVOKE_ROLE;
	out->role_name = role_name;
	out->member_name = member_name;
	return true;
}

bool RBACParserExtension::TryParseGrantTable(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out) {
	// GRANT SELECT ON table TO role (no column list)
	if (!tokens.MatchKeyword("GRANT")) {
		return false;
	}
	if (!tokens.MatchKeyword("SELECT")) {
		return false;
	}

	// If there's a '(' next, it's a column grant
	if (tokens.IsOperator('(')) {
		return false;
	}

	tokens.ExpectKeyword("ON");

	QualifiedName table_ref = tokens.ConsumeTableRef();

	tokens.ExpectKeyword("TO");

	string role_name = tokens.ConsumeIdentifier();

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::GRANT_TABLE;
	out->schema_name = table_ref.schema;
	out->table_name = table_ref.name;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseRevokeTable(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out) {
	// REVOKE SELECT ON table FROM role (no column list)
	if (!tokens.MatchKeyword("REVOKE")) {
		return false;
	}
	if (!tokens.MatchKeyword("SELECT")) {
		return false;
	}

	// If there's a '(' next, it's a column revoke
	if (tokens.IsOperator('(')) {
		return false;
	}

	tokens.ExpectKeyword("ON");

	QualifiedName table_ref = tokens.ConsumeTableRef();

	tokens.ExpectKeyword("FROM");

	string role_name = tokens.ConsumeIdentifier();

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::REVOKE_TABLE;
	out->schema_name = table_ref.schema;
	out->table_name = table_ref.name;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseGrantColumn(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out) {
	// GRANT SELECT (col1, col2) ON table TO role
	if (!tokens.MatchKeyword("GRANT")) {
		return false;
	}
	if (!tokens.MatchKeyword("SELECT")) {
		return false;
	}

	// Must have column list
	if (!tokens.IsOperator('(')) {
		return false;
	}

	vector<string> columns = tokens.ConsumeColumnList();

	tokens.ExpectKeyword("ON");

	QualifiedName table_ref = tokens.ConsumeTableRef();

	tokens.ExpectKeyword("TO");

	string role_name = tokens.ConsumeIdentifier();

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::GRANT_COLUMN;
	out->schema_name = table_ref.schema;
	out->table_name = table_ref.name;
	out->column_names = columns;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseRevokeColumn(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out) {
	// REVOKE SELECT (col1, col2) ON table FROM role
	if (!tokens.MatchKeyword("REVOKE")) {
		return false;
	}
	if (!tokens.MatchKeyword("SELECT")) {
		return false;
	}

	// Must have column list
	if (!tokens.IsOperator('(')) {
		return false;
	}

	vector<string> columns = tokens.ConsumeColumnList();

	tokens.ExpectKeyword("ON");

	QualifiedName table_ref = tokens.ConsumeTableRef();

	tokens.ExpectKeyword("FROM");

	string role_name = tokens.ConsumeIdentifier();

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::REVOKE_COLUMN;
	out->schema_name = table_ref.schema;
	out->table_name = table_ref.name;
	out->column_names = columns;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseCreateRowPolicy(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out) {
	// CREATE ROW POLICY name ON table FOR SELECT USING (expr) TO role
	if (!tokens.MatchKeyword("CREATE")) {
		return false;
	}
	if (!tokens.MatchKeyword("ROW")) {
		return false;
	}
	if (!tokens.MatchKeyword("POLICY")) {
		return false;
	}

	string policy_name = tokens.ConsumeIdentifier();

	tokens.ExpectKeyword("ON");

	QualifiedName table_ref = tokens.ConsumeTableRef();

	// Optional: FOR SELECT
	if (tokens.MatchKeyword("FOR")) {
		tokens.ExpectKeyword("SELECT");
	}

	tokens.ExpectKeyword("USING");

	// Parse the expression in parentheses - we need to handle nested parens
	if (!tokens.MatchOperator('(')) {
		throw ParserException("USING clause requires (expression)");
	}

	// Collect everything until we find the matching close paren and TO keyword
	// The expression can contain nested parentheses, so we need to track depth
	string remaining = tokens.RemainingText();

	// Find the closing paren followed by TO
	int paren_depth = 1;
	idx_t expr_end = 0;
	for (idx_t i = 0; i < remaining.size(); i++) {
		if (remaining[i] == '(') {
			paren_depth++;
		} else if (remaining[i] == ')') {
			paren_depth--;
			if (paren_depth == 0) {
				expr_end = i;
				break;
			}
		}
	}

	if (paren_depth != 0) {
		throw ParserException("Unterminated expression in USING clause");
	}

	string filter_expr = remaining.substr(0, expr_end);
	StringUtil::Trim(filter_expr);

	// Skip past the expression tokens to find TO
	// We need to re-tokenize or skip ahead
	// For now, consume until we hit TO keyword
	while (tokens.HasMore() && !tokens.IsKeyword("TO")) {
		tokens.Advance();
	}

	tokens.ExpectKeyword("TO");

	string role_name = tokens.ConsumeIdentifier();

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::CREATE_ROW_POLICY;
	out->policy_name = policy_name;
	out->schema_name = table_ref.schema;
	out->table_name = table_ref.name;
	out->filter_expression = filter_expr;
	out->role_name = role_name;
	return true;
}

bool RBACParserExtension::TryParseDropRowPolicy(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out) {
	// DROP ROW POLICY name ON table
	if (!tokens.MatchKeyword("DROP")) {
		return false;
	}
	if (!tokens.MatchKeyword("ROW")) {
		return false;
	}
	if (!tokens.MatchKeyword("POLICY")) {
		return false;
	}

	string policy_name = tokens.ConsumeIdentifier();

	tokens.ExpectKeyword("ON");

	QualifiedName table_ref = tokens.ConsumeTableRef();

	out = make_uniq<RBACParseData>();
	out->statement_type = RBACStatementType::DROP_ROW_POLICY;
	out->policy_name = policy_name;
	out->schema_name = table_ref.schema;
	out->table_name = table_ref.name;
	return true;
}

//===--------------------------------------------------------------------===//
// RBACParserExtension
//===--------------------------------------------------------------------===//

RBACParserExtension::RBACParserExtension() {
	parse_function = ParseFunction;
	plan_function = PlanFunction;
}

ParserExtensionParseResult RBACParserExtension::ParseFunction(ParserExtensionInfo *info, const string &query) {
	// Normalize the query - remove trailing semicolon
	string trimmed = query;
	StringUtil::Trim(trimmed);
	if (StringUtil::EndsWith(trimmed, ";")) {
		trimmed = trimmed.substr(0, trimmed.length() - 1);
		StringUtil::Trim(trimmed);
	}

	unique_ptr<RBACParseData> parse_data;

	// Try each parser in order using fresh token streams
	// We need fresh streams because each TryParse consumes tokens
	try {
		{
			RBACTokenStream tokens(trimmed);
			if (TryParseCreateRole(tokens, parse_data)) {
				return ParserExtensionParseResult(std::move(parse_data));
			}
		}
		{
			RBACTokenStream tokens(trimmed);
			if (TryParseDropRole(tokens, parse_data)) {
				return ParserExtensionParseResult(std::move(parse_data));
			}
		}
		{
			RBACTokenStream tokens(trimmed);
			if (TryParseGrantColumn(tokens, parse_data)) {  // Check column before table
				return ParserExtensionParseResult(std::move(parse_data));
			}
		}
		{
			RBACTokenStream tokens(trimmed);
			if (TryParseRevokeColumn(tokens, parse_data)) {
				return ParserExtensionParseResult(std::move(parse_data));
			}
		}
		{
			RBACTokenStream tokens(trimmed);
			if (TryParseGrantTable(tokens, parse_data)) {
				return ParserExtensionParseResult(std::move(parse_data));
			}
		}
		{
			RBACTokenStream tokens(trimmed);
			if (TryParseRevokeTable(tokens, parse_data)) {
				return ParserExtensionParseResult(std::move(parse_data));
			}
		}
		{
			RBACTokenStream tokens(trimmed);
			if (TryParseGrantRole(tokens, parse_data)) {
				return ParserExtensionParseResult(std::move(parse_data));
			}
		}
		{
			RBACTokenStream tokens(trimmed);
			if (TryParseRevokeRole(tokens, parse_data)) {
				return ParserExtensionParseResult(std::move(parse_data));
			}
		}
		{
			RBACTokenStream tokens(trimmed);
			if (TryParseCreateRowPolicy(tokens, parse_data)) {
				return ParserExtensionParseResult(std::move(parse_data));
			}
		}
		{
			RBACTokenStream tokens(trimmed);
			if (TryParseDropRowPolicy(tokens, parse_data)) {
				return ParserExtensionParseResult(std::move(parse_data));
			}
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

void RegisterRBACParser(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &callback_manager = ExtensionCallbackManager::Get(db);
	callback_manager.Register(RBACParserExtension());
}

} // namespace duckdb
