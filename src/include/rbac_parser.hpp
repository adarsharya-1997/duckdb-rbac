#pragma once

#include "duckdb.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/simplified_token.hpp"

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
// Token Stream for RBAC DDL Parsing
//===--------------------------------------------------------------------===//

//! RBACTokenStream wraps Parser::Tokenize() output for cleaner token-based parsing.
//! Handles keyword matching, identifier extraction (including quoted), and table references.
class RBACTokenStream {
public:
	explicit RBACTokenStream(const string &query);

	//! Check if more tokens are available
	bool HasMore() const;
	//! Move to next token
	void Advance();
	//! Get current token type
	SimplifiedTokenType CurrentType() const;
	//! Get text of current token from original query
	string CurrentText() const;
	//! Get remaining query text from current position
	string RemainingText() const;

	//! Check if current token is a keyword matching the given string (case-insensitive)
	bool IsKeyword(const string &kw) const;
	//! Match and consume a keyword, returns true if matched
	bool MatchKeyword(const string &kw);
	//! Expect a keyword, throws if not found
	void ExpectKeyword(const string &kw);

	//! Check if current token is an operator matching the given character
	bool IsOperator(char op) const;
	//! Match and consume an operator, returns true if matched
	bool MatchOperator(char op);

	//! Consume an identifier (handles quoted identifiers), throws if not found
	string ConsumeIdentifier();
	//! Consume a qualified table reference (schema.table), returns schema and table
	QualifiedName ConsumeTableRef();
	//! Consume a comma-separated list of identifiers within parentheses
	vector<string> ConsumeColumnList();
	//! Consume everything until a specific keyword (for USING expressions)
	string ConsumeUntilKeyword(const string &kw);

private:
	string query_;
	vector<SimplifiedToken> tokens_;
	idx_t pos_;

	//! Get the end position of current token
	idx_t CurrentEnd() const;
	//! Extract identifier text, handling quoted identifiers (strips quotes, unescapes)
	string ExtractIdentifier(idx_t start, idx_t end) const;
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
	// Token-based parsing helpers
	static bool TryParseCreateRole(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out);
	static bool TryParseDropRole(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out);
	static bool TryParseGrantRole(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out);
	static bool TryParseRevokeRole(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out);
	static bool TryParseGrantTable(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out);
	static bool TryParseRevokeTable(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out);
	static bool TryParseGrantColumn(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out);
	static bool TryParseRevokeColumn(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out);
	static bool TryParseCreateRowPolicy(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out);
	static bool TryParseDropRowPolicy(RBACTokenStream &tokens, unique_ptr<RBACParseData> &out);
};

//! Register the RBAC parser extension
void RegisterRBACParser(ExtensionLoader &loader);

} // namespace duckdb
