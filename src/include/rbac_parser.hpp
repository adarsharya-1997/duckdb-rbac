#pragma once

#include "duckdb.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

class ExtensionLoader;
class Parser;

//===--------------------------------------------------------------------===//
// RBAC Statement Types
//===--------------------------------------------------------------------===//

enum class RBACStatementType {
	CREATE_ROLE,
	DROP_ROLE,
	GRANT_ROLE,           // GRANT role TO member
	REVOKE_ROLE,          // REVOKE role FROM member
	GRANT_TABLE,          // GRANT SELECT ON table TO role
	REVOKE_TABLE,         // REVOKE SELECT ON table FROM role
	GRANT_COLUMN,         // GRANT SELECT (cols) ON table TO role
	REVOKE_COLUMN,        // REVOKE SELECT (cols) ON table FROM role
	CREATE_ROW_POLICY,    // CREATE ROW POLICY ... USING (expr) TO role
	DROP_ROW_POLICY,      // DROP ROW POLICY name ON table
	UNKNOWN
};

//===--------------------------------------------------------------------===//
// Parse Data for RBAC DDL statements
//===--------------------------------------------------------------------===//

struct RBACParseData : public ParserExtensionParseData {
	RBACStatementType statement_type = RBACStatementType::UNKNOWN;

	// Role management
	string role_name;
	string member_name;       // For GRANT role TO member

	// Table/column privileges
	string schema_name;       // Default: "main"
	string table_name;
	vector<string> column_names;  // For column-level grants

	// Row policies
	string policy_name;
	string filter_expression;

	RBACParseData() : schema_name("main") {}

	unique_ptr<ParserExtensionParseData> Copy() const override {
		auto copy = make_uniq<RBACParseData>();
		copy->statement_type = statement_type;
		copy->role_name = role_name;
		copy->member_name = member_name;
		copy->schema_name = schema_name;
		copy->table_name = table_name;
		copy->column_names = column_names;
		copy->policy_name = policy_name;
		copy->filter_expression = filter_expression;
		return copy;
	}

	string ToString() const override;
};

//===--------------------------------------------------------------------===//
// RBAC Parser Extension
//===--------------------------------------------------------------------===//

class RBACParserExtension : public ParserExtension {
public:
	RBACParserExtension();

	//! Parse function: called when DuckDB's parser fails
	static ParserExtensionParseResult ParseFunction(ParserExtensionInfo *info, const string &query);

	//! Plan function: converts parsed data into a TableFunction
	static ParserExtensionPlanResult PlanFunction(ParserExtensionInfo *info, ClientContext &context,
	                                               unique_ptr<ParserExtensionParseData> parse_data);

private:
	// Parsing helpers
	static bool TryParseCreateRole(const string &upper, const string &original, unique_ptr<RBACParseData> &out);
	static bool TryParseDropRole(const string &upper, const string &original, unique_ptr<RBACParseData> &out);
	static bool TryParseGrantRole(const string &upper, const string &original, unique_ptr<RBACParseData> &out);
	static bool TryParseRevokeRole(const string &upper, const string &original, unique_ptr<RBACParseData> &out);
	static bool TryParseGrantTable(const string &upper, const string &original, unique_ptr<RBACParseData> &out);
	static bool TryParseRevokeTable(const string &upper, const string &original, unique_ptr<RBACParseData> &out);
	static bool TryParseGrantColumn(const string &upper, const string &original, unique_ptr<RBACParseData> &out);
	static bool TryParseRevokeColumn(const string &upper, const string &original, unique_ptr<RBACParseData> &out);
	static bool TryParseCreateRowPolicy(const string &upper, const string &original, unique_ptr<RBACParseData> &out);
	static bool TryParseDropRowPolicy(const string &upper, const string &original, unique_ptr<RBACParseData> &out);
};

//! Register the RBAC parser extension
void RegisterRBACParser(ExtensionLoader &loader);

} // namespace duckdb
