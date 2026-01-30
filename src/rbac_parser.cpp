#include "rbac_parser.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

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

void RegisterRBACParser(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &callback_manager = ExtensionCallbackManager::Get(db);
	callback_manager.Register(RBACParserExtension());
}

} // namespace duckdb
