#include "rbac_parser.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/parser/parser.hpp"
#include <regex>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Table Function for RBAC DDL results
//===--------------------------------------------------------------------===//

struct RBACDDLBindData : public TableFunctionData {
	string message;

	explicit RBACDDLBindData(string msg) : message(std::move(msg)) {}
};

struct RBACDDLGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<FunctionData> RBACDDLBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("result");

	// Get message from parameters
	string message = input.inputs[0].GetValue<string>();
	return make_uniq<RBACDDLBindData>(message);
}

static unique_ptr<GlobalTableFunctionState> RBACDDLInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<RBACDDLGlobalState>();
}

static void RBACDDLExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<RBACDDLBindData>();
	auto &state = data.global_state->Cast<RBACDDLGlobalState>();

	if (state.done) {
		return;
	}

	output.SetCardinality(1);
	output.SetValue(0, 0, Value(bind_data.message));
	state.done = true;
}

static TableFunction GetRBACDDLFunction() {
	TableFunction func("rbac_ddl_result", {LogicalType::VARCHAR}, RBACDDLExecute, RBACDDLBind, RBACDDLInit);
	return func;
}

//===--------------------------------------------------------------------===//
// RBACParserExtension
//===--------------------------------------------------------------------===//

RBACParserExtension::RBACParserExtension() {
	parse_function = ParseFunction;
	plan_function = PlanFunction;
	parser_override = ParserOverride;  // Spike 0.6C: intercept all SQL for SELECT * rewriting
}

ParserExtensionParseResult RBACParserExtension::ParseFunction(ParserExtensionInfo *info, const string &query) {
	// Normalize the query for easier parsing
	string trimmed = query;
	StringUtil::Trim(trimmed);
	string upper = StringUtil::Upper(trimmed);

	// Remove trailing semicolon if present
	if (StringUtil::EndsWith(upper, ";")) {
		upper = upper.substr(0, upper.length() - 1);
		trimmed = trimmed.substr(0, trimmed.length() - 1);
	}
	StringUtil::Trim(upper);
	StringUtil::Trim(trimmed);

	// Check for CREATE ROLE
	if (StringUtil::StartsWith(upper, "CREATE ROLE ")) {
		string role_name = trimmed.substr(12); // "CREATE ROLE " is 12 chars
		StringUtil::Trim(role_name);
		if (role_name.empty()) {
			return ParserExtensionParseResult("CREATE ROLE requires a role name");
		}
		return ParserExtensionParseResult(
			make_uniq<RBACParseData>(RBACParseData::StatementType::CREATE_ROLE, role_name)
		);
	}

	// Check for DROP ROLE
	if (StringUtil::StartsWith(upper, "DROP ROLE ")) {
		string role_name = trimmed.substr(10); // "DROP ROLE " is 10 chars
		StringUtil::Trim(role_name);
		if (role_name.empty()) {
			return ParserExtensionParseResult("DROP ROLE requires a role name");
		}
		return ParserExtensionParseResult(
			make_uniq<RBACParseData>(RBACParseData::StatementType::DROP_ROLE, role_name)
		);
	}

	// Not a statement we handle - let DuckDB show its original error
	return ParserExtensionParseResult();
}

ParserExtensionPlanResult RBACParserExtension::PlanFunction(ParserExtensionInfo *info, ClientContext &context,
                                                            unique_ptr<ParserExtensionParseData> parse_data) {
	auto &rbac_data = static_cast<RBACParseData &>(*parse_data);

	ParserExtensionPlanResult result;
	result.function = GetRBACDDLFunction();
	result.requires_valid_transaction = true;
	result.return_type = StatementReturnType::QUERY_RESULT;

	switch (rbac_data.statement_type) {
	case RBACParseData::StatementType::CREATE_ROLE:
		result.parameters.push_back(Value("Role created: " + rbac_data.role_name));
		break;
	case RBACParseData::StatementType::DROP_ROLE:
		result.parameters.push_back(Value("Role dropped: " + rbac_data.role_name));
		break;
	default:
		result.parameters.push_back(Value("Unknown RBAC operation"));
		break;
	}

	return result;
}

//===--------------------------------------------------------------------===//
// Spike 0.6C: Parser Override for SELECT * Rewriting
//===--------------------------------------------------------------------===//

ParserOverrideResult RBACParserExtension::ParserOverride(ParserExtensionInfo *info, const string &query) {
	// Normalize the query
	string trimmed = query;
	StringUtil::Trim(trimmed);
	string upper = StringUtil::Upper(trimmed);

	// Remove trailing semicolon for matching
	if (StringUtil::EndsWith(upper, ";")) {
		upper = upper.substr(0, upper.length() - 1);
		StringUtil::Trim(upper);
	}

	// Spike 0.6C: Detect "SELECT * FROM col_test_rewrite" and rewrite to explicit columns
	// This simulates column-level filtering for SELECT *
	if (upper == "SELECT * FROM COL_TEST_REWRITE" ||
	    upper == "SELECT * FROM COL_TEST_REWRITE ORDER BY A") {

		fprintf(stderr, "[Spike 0.6C] Intercepted: %s\n", query.c_str());

		// Rewrite to exclude c_secret column
		string rewritten;
		if (upper == "SELECT * FROM COL_TEST_REWRITE ORDER BY A") {
			rewritten = "SELECT a, b FROM col_test_rewrite ORDER BY a";
		} else {
			rewritten = "SELECT a, b FROM col_test_rewrite";
		}

		fprintf(stderr, "[Spike 0.6C] Rewriting to: %s\n", rewritten.c_str());

		// Parse the rewritten query using DuckDB's native parser
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

	// Not a query we want to rewrite - return empty result to fall through to normal parsing
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
