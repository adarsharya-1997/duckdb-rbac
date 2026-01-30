#include "rbac_optimizer.hpp"
#include "rbac_state.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Expression Binding Helper (for row policies)
//===--------------------------------------------------------------------===//

class SimpleExpressionBinder {
public:
	SimpleExpressionBinder(LogicalGet &get, const string &current_user)
	    : get(get), current_user(current_user) {
		// Build map from column name to OUTPUT index (position in scanned columns)
		auto &column_ids = get.GetColumnIds();
		for (idx_t output_idx = 0; output_idx < column_ids.size(); output_idx++) {
			idx_t col_idx = column_ids[output_idx].GetPrimaryIndex();
			if (col_idx < get.names.size()) {
				scanned_column_map[get.names[col_idx]] = output_idx;
			}
		}
		// Build map from column name to table column index
		for (idx_t i = 0; i < get.names.size(); i++) {
			all_column_map[get.names[i]] = i;
		}
	}

	unique_ptr<Expression> Bind(ParsedExpression &expr) {
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::COLUMN_REF:
			return BindColumnRef(expr.Cast<ColumnRefExpression>());
		case ExpressionClass::CONSTANT:
			return BindConstant(expr.Cast<ConstantExpression>());
		case ExpressionClass::COMPARISON:
			return BindComparison(expr.Cast<ComparisonExpression>());
		case ExpressionClass::FUNCTION:
			return BindFunction(expr.Cast<FunctionExpression>());
		case ExpressionClass::CONJUNCTION:
			return BindConjunction(expr.Cast<ConjunctionExpression>());
		case ExpressionClass::CAST:
			return BindCast(expr.Cast<CastExpression>());
		default:
			throw NotImplementedException("SimpleExpressionBinder: unsupported expression type %s",
			                              ExpressionClassToString(expr.GetExpressionClass()));
		}
	}

private:
	unique_ptr<Expression> BindColumnRef(ColumnRefExpression &expr) {
		string col_name;
		if (expr.column_names.size() == 1) {
			col_name = expr.column_names[0];
		} else if (expr.column_names.size() >= 2) {
			col_name = expr.column_names.back();
		} else {
			throw BinderException("Invalid column reference");
		}

		// Check if column is already being scanned
		auto it = scanned_column_map.find(col_name);
		if (it != scanned_column_map.end()) {
			// Column already in scan, use existing binding
			idx_t output_idx = it->second;
			auto &column_ids = get.GetColumnIds();
			idx_t table_col_idx = column_ids[output_idx].GetPrimaryIndex();
			LogicalType col_type = get.returned_types[table_col_idx];
			return make_uniq<BoundColumnRefExpression>(
				col_type,
				ColumnBinding(get.table_index, output_idx)
			);
		}

		// Column not being scanned - need to add it for row policy filters
		// Find the column in all_column_map (table's column list)
		auto all_it = all_column_map.find(col_name);
		if (all_it == all_column_map.end()) {
			throw BinderException("Column '%s' not found in table", col_name);
		}

		idx_t table_col_idx = all_it->second;
		LogicalType col_type = get.returned_types[table_col_idx];

		// Add the column to the scan
		auto &mutable_column_ids = get.GetMutableColumnIds();
		idx_t new_output_idx = mutable_column_ids.size();
		mutable_column_ids.push_back(ColumnIndex(static_cast<column_t>(table_col_idx)));

		// Update our map
		scanned_column_map[col_name] = new_output_idx;

		return make_uniq<BoundColumnRefExpression>(
			col_type,
			ColumnBinding(get.table_index, new_output_idx)
		);
	}

	unique_ptr<Expression> BindConstant(ConstantExpression &expr) {
		return make_uniq<BoundConstantExpression>(expr.value);
	}

	unique_ptr<Expression> BindComparison(ComparisonExpression &expr) {
		auto left = Bind(*expr.left);
		auto right = Bind(*expr.right);
		return make_uniq<BoundComparisonExpression>(
			expr.type,
			std::move(left),
			std::move(right)
		);
	}

	unique_ptr<Expression> BindFunction(FunctionExpression &expr) {
		// Handle current_user() function - replace with constant
		if (StringUtil::Lower(expr.function_name) == "current_user" && expr.children.empty()) {
			return make_uniq<BoundConstantExpression>(Value(current_user));
		}
		// Handle rbac_current_user() as well
		if (StringUtil::Lower(expr.function_name) == "rbac_current_user" && expr.children.empty()) {
			return make_uniq<BoundConstantExpression>(Value(current_user));
		}
		throw NotImplementedException("SimpleExpressionBinder: unsupported function '%s'", expr.function_name);
	}

	unique_ptr<Expression> BindConjunction(ConjunctionExpression &expr) {
		auto conjunction = make_uniq<BoundConjunctionExpression>(expr.type);
		for (auto &child : expr.children) {
			conjunction->children.push_back(Bind(*child));
		}
		return conjunction;
	}

	unique_ptr<Expression> BindCast(CastExpression &expr) {
		auto child = Bind(*expr.child);
		// For constant casts (like boolean literals), evaluate at bind time
		if (child->type == ExpressionType::VALUE_CONSTANT) {
			auto &const_expr = child->Cast<BoundConstantExpression>();
			// Handle boolean casts specially (true/false as strings)
			if (expr.cast_type == LogicalType::BOOLEAN && const_expr.value.type().id() == LogicalTypeId::VARCHAR) {
				string val = StringUtil::Lower(const_expr.value.ToString());
				if (val == "true" || val == "t" || val == "1") {
					return make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
				} else if (val == "false" || val == "f" || val == "0") {
					return make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
				}
			}
			// Try generic cast
			try {
				Value casted = const_expr.value.DefaultCastAs(expr.cast_type);
				return make_uniq<BoundConstantExpression>(casted);
			} catch (...) {
				// If cast fails, just return the original
				return child;
			}
		}
		// For non-constant casts, just return the child
		return child;
	}

	LogicalGet &get;
	string current_user;
	case_insensitive_map_t<idx_t> scanned_column_map;  // column name -> output index
	case_insensitive_map_t<idx_t> all_column_map;      // column name -> table column index
};

static unique_ptr<Expression> ParseAndBindExpression(const string &expr_str, LogicalGet &get,
                                                      const string &current_user) {
	auto expressions = Parser::ParseExpressionList(expr_str);
	if (expressions.empty()) {
		throw BinderException("Failed to parse expression: %s", expr_str);
	}
	SimpleExpressionBinder binder(get, current_user);
	return binder.Bind(*expressions[0]);
}

// Helper to execute SQL and check for rows
static bool QueryHasRows(ClientContext &context, const string &sql) {
	Connection con(*context.db);
	auto result = con.Query(sql);
	if (result->HasError()) {
		return false;
	}
	auto &mat_result = result->Cast<MaterializedQueryResult>();
	return mat_result.RowCount() > 0;
}

//===--------------------------------------------------------------------===//
// RBACOptimizerExtension
//===--------------------------------------------------------------------===//

OptimizerExtension RBACOptimizerExtension::Create() {
	OptimizerExtension ext;
	ext.pre_optimize_function = PreOptimize;
	return ext;
}

void RBACOptimizerExtension::PreOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	auto &context = input.context;

	// Get RBAC state
	auto rbac_state = RBACState::Get(context);

	// Superuser bypasses ALL permission checks (FR-21)
	if (rbac_state->is_superuser) {
		return;
	}

	// Get effective roles for permission checking
	auto effective_roles = RBACState::GetEffectiveRoles(context);
	string user_name = rbac_state->user_name;

	// Walk the plan and check permissions
	WalkPlanWithParent(context, plan, effective_roles, user_name);
}

// Check if a table is an RBAC system table (should be readable by anyone)
static bool IsRBACSystemTable(const string &table_name) {
	return table_name == "duckdb_roles" ||
	       table_name == "duckdb_role_members" ||
	       table_name == "duckdb_table_privileges" ||
	       table_name == "duckdb_column_privileges" ||
	       table_name == "duckdb_row_policies";
}

void RBACOptimizerExtension::WalkPlanWithParent(ClientContext &context, unique_ptr<LogicalOperator> &op_ptr,
                                                 const vector<string> &effective_roles, const string &user_name) {
	auto &op = *op_ptr;

	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		auto table = get.GetTable();

		if (table) {
			string table_name = table->name;
			string schema_name = table->ParentSchema().name;

			// Skip permission checks on RBAC system tables (readable by anyone per Q104)
			if (IsRBACSystemTable(table_name)) {
				// System tables are always accessible
				return;
			}

			// Check table-level permission
			CheckTablePermission(context, schema_name, table_name, effective_roles, user_name);

			// Check column-level permissions
			CheckColumnPermissions(context, schema_name, table_name, get, effective_roles, user_name);

			// Inject row policy filters (Phase 5)
			InjectRowPolicies(context, op_ptr, get, schema_name, table_name, effective_roles, user_name);
			// After injection, op_ptr may have changed, return early
			return;
		}
	}

	// Recursively process children
	for (auto &child : op.children) {
		WalkPlanWithParent(context, child, effective_roles, user_name);
	}
}

void RBACOptimizerExtension::CheckTablePermission(ClientContext &context, const string &schema_name,
                                                   const string &table_name, const vector<string> &effective_roles,
                                                   const string &user_name) {
	// Build IN clause for effective roles
	if (effective_roles.empty()) {
		throw PermissionException("User '%s' lacks SELECT privilege on table '%s.%s'",
		                          user_name, schema_name, table_name);
	}

	string roles_in = "";
	for (idx_t i = 0; i < effective_roles.size(); i++) {
		if (i > 0) roles_in += ", ";
		roles_in += "'" + StringUtil::Replace(effective_roles[i], "'", "''") + "'";
	}

	// Check table-level grant
	string sql = StringUtil::Format(
	    "SELECT 1 FROM duckdb_table_privileges "
	    "WHERE table_schema = '%s' AND table_name = '%s' AND grantee IN (%s)",
	    StringUtil::Replace(schema_name, "'", "''"),
	    StringUtil::Replace(table_name, "'", "''"),
	    roles_in);

	if (QueryHasRows(context, sql)) {
		return; // Table-level access granted
	}

	// Check if there are any column-level grants (partial access is OK)
	sql = StringUtil::Format(
	    "SELECT 1 FROM duckdb_column_privileges "
	    "WHERE table_schema = '%s' AND table_name = '%s' AND grantee IN (%s)",
	    StringUtil::Replace(schema_name, "'", "''"),
	    StringUtil::Replace(table_name, "'", "''"),
	    roles_in);

	if (QueryHasRows(context, sql)) {
		return; // Column-level access exists - will be checked in CheckColumnPermissions
	}

	// No access at all
	throw PermissionException("User '%s' lacks SELECT privilege on table '%s.%s'",
	                          user_name, schema_name, table_name);
}

void RBACOptimizerExtension::CheckColumnPermissions(ClientContext &context, const string &schema_name,
                                                     const string &table_name, LogicalGet &get,
                                                     const vector<string> &effective_roles, const string &user_name) {
	if (effective_roles.empty()) {
		return; // Already handled in CheckTablePermission
	}

	string roles_in = "";
	for (idx_t i = 0; i < effective_roles.size(); i++) {
		if (i > 0) roles_in += ", ";
		roles_in += "'" + StringUtil::Replace(effective_roles[i], "'", "''") + "'";
	}

	// Check if user has table-level grant (all columns allowed)
	string sql = StringUtil::Format(
	    "SELECT 1 FROM duckdb_table_privileges "
	    "WHERE table_schema = '%s' AND table_name = '%s' AND grantee IN (%s)",
	    StringUtil::Replace(schema_name, "'", "''"),
	    StringUtil::Replace(table_name, "'", "''"),
	    roles_in);

	if (QueryHasRows(context, sql)) {
		// Table-level grant exists - all columns allowed
		return;
	}

	// Column-level access: build set of allowed columns
	sql = StringUtil::Format(
	    "SELECT column_name FROM duckdb_column_privileges "
	    "WHERE table_schema = '%s' AND table_name = '%s' AND grantee IN (%s)",
	    StringUtil::Replace(schema_name, "'", "''"),
	    StringUtil::Replace(table_name, "'", "''"),
	    roles_in);

	Connection con(*context.db);
	auto result = con.Query(sql);

	unordered_set<string> allowed_columns;
	if (!result->HasError()) {
		for (auto &row : *result) {
			allowed_columns.insert(row.GetValue<string>(0));
		}
	}

	// Check each column referenced in the LogicalGet
	auto &col_ids = get.GetColumnIds();
	for (auto &col_id : col_ids) {
		idx_t col_idx = col_id.GetPrimaryIndex();
		if (col_idx >= get.names.size()) {
			continue; // Skip system columns
		}
		string col_name = get.names[col_idx];

		if (allowed_columns.find(col_name) == allowed_columns.end()) {
			throw PermissionException("User '%s' lacks SELECT privilege on column '%s' of table '%s.%s'",
			                          user_name, col_name, schema_name, table_name);
		}
	}
}

void RBACOptimizerExtension::InjectParsedFilter(unique_ptr<LogicalOperator> &op_ptr, LogicalGet &get,
                                                 const string &filter_expr, const string &current_user) {
	auto bound_expr = ParseAndBindExpression(filter_expr, get, current_user);
	auto filter = make_uniq<LogicalFilter>(std::move(bound_expr));
	filter->children.push_back(std::move(op_ptr));
	op_ptr = std::move(filter);
}

void RBACOptimizerExtension::InjectRowPolicies(ClientContext &context, unique_ptr<LogicalOperator> &op_ptr,
                                                LogicalGet &get, const string &schema_name, const string &table_name,
                                                const vector<string> &effective_roles, const string &user_name) {
	// No roles = no policies apply (FR-18: no policy = see all rows)
	if (effective_roles.empty()) {
		return;
	}

	// Build IN clause for effective roles
	string roles_in = "";
	for (idx_t i = 0; i < effective_roles.size(); i++) {
		if (i > 0) roles_in += ", ";
		roles_in += "'" + StringUtil::Replace(effective_roles[i], "'", "''") + "'";
	}

	// Query all applicable row policies
	string sql = StringUtil::Format(
	    "SELECT filter_expression FROM duckdb_row_policies "
	    "WHERE table_schema = '%s' AND table_name = '%s' AND grantee IN (%s)",
	    StringUtil::Replace(schema_name, "'", "''"),
	    StringUtil::Replace(table_name, "'", "''"),
	    roles_in);

	Connection con(*context.db);
	auto result = con.Query(sql);

	if (result->HasError()) {
		return; // No policies or error - see all rows (FR-18)
	}

	// Collect all filter expressions
	vector<string> filter_expressions;
	for (auto &row : *result) {
		filter_expressions.push_back(row.GetValue<string>(0));
	}

	// No policies = see all rows (FR-18)
	if (filter_expressions.empty()) {
		return;
	}

	// Parse and bind each expression, then combine with OR (FR-15)
	vector<unique_ptr<Expression>> bound_expressions;
	for (const auto &expr_str : filter_expressions) {
		try {
			auto bound_expr = ParseAndBindExpression(expr_str, get, user_name);
			bound_expressions.push_back(std::move(bound_expr));
		} catch (const Exception &e) {
			// If a policy expression fails to bind (e.g., column not in scan),
			// throw an error - this is a security requirement
			throw PermissionException(
			    "Row policy cannot be applied: %s. "
			    "Ensure your query includes all columns referenced by row policies.",
			    e.what());
		}
	}

	if (bound_expressions.empty()) {
		return;
	}

	// Combine with OR if multiple expressions
	unique_ptr<Expression> combined;
	if (bound_expressions.size() == 1) {
		combined = std::move(bound_expressions[0]);
	} else {
		// Create OR conjunction - must add children one by one
		auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
		for (auto &expr : bound_expressions) {
			conjunction->children.push_back(std::move(expr));
		}
		combined = std::move(conjunction);
	}

	// Inject the filter
	auto filter = make_uniq<LogicalFilter>(std::move(combined));
	filter->children.push_back(std::move(op_ptr));
	op_ptr = std::move(filter);
}

void RegisterRBACOptimizer(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &callback_manager = ExtensionCallbackManager::Get(db);
	callback_manager.Register(RBACOptimizerExtension::Create());
}

} // namespace duckdb
