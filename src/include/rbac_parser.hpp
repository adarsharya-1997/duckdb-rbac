#pragma once

#include "duckdb.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

class ExtensionLoader;

//! Parse data for RBAC DDL statements
struct RBACParseData : public ParserExtensionParseData {
	enum class StatementType { CREATE_ROLE, DROP_ROLE, UNKNOWN };

	StatementType statement_type;
	string role_name;

	RBACParseData(StatementType type, string name)
		: statement_type(type), role_name(std::move(name)) {}

	unique_ptr<ParserExtensionParseData> Copy() const override {
		return make_uniq<RBACParseData>(statement_type, role_name);
	}

	string ToString() const override {
		switch (statement_type) {
		case StatementType::CREATE_ROLE:
			return "CREATE ROLE " + role_name;
		case StatementType::DROP_ROLE:
			return "DROP ROLE " + role_name;
		default:
			return "UNKNOWN RBAC STATEMENT";
		}
	}
};

//! RBAC Parser Extension - intercepts RBAC DDL that DuckDB doesn't recognize
class RBACParserExtension : public ParserExtension {
public:
	RBACParserExtension();

	//! Parse function: called when DuckDB's parser fails
	static ParserExtensionParseResult ParseFunction(ParserExtensionInfo *info, const string &query);

	//! Plan function: converts parsed data into a TableFunction
	static ParserExtensionPlanResult PlanFunction(ParserExtensionInfo *info, ClientContext &context,
	                                               unique_ptr<ParserExtensionParseData> parse_data);
};

//! Register the RBAC parser extension
void RegisterRBACParser(ExtensionLoader &loader);

} // namespace duckdb
