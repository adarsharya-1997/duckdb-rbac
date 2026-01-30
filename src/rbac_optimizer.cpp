#include "rbac_optimizer.hpp"
#include "rbac_state.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Expression Binding Helper (for row policies)
//===--------------------------------------------------------------------===//

class SimpleExpressionBinder {
public:
	SimpleExpressionBinder(LogicalGet &get) : get(get) {
		for (idx_t i = 0; i < get.names.size(); i++) {
			column_map[get.names[i]] = i;
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

		auto it = column_map.find(col_name);
		if (it == column_map.end()) {
			throw BinderException("Column '%s' not found in table", col_name);
		}

		idx_t col_idx = it->second;
		LogicalType col_type = get.returned_types[col_idx];

		return make_uniq<BoundColumnRefExpression>(
			col_type,
			ColumnBinding(get.table_index, col_idx)
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

	LogicalGet &get;
	case_insensitive_map_t<idx_t> column_map;
};

static unique_ptr<Expression> ParseAndBindExpression(const string &expr_str, LogicalGet &get) {
	auto expressions = Parser::ParseExpressionList(expr_str);
	if (expressions.empty()) {
		throw BinderException("Failed to parse expression: %s", expr_str);
	}
	SimpleExpressionBinder binder(get);
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
		// Still run legacy spike code for spike tests
		WalkPlanWithParentLegacy(plan);
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

			// TODO (Phase 5): Inject row policy filters here
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
                                                 const string &filter_expr) {
	auto bound_expr = ParseAndBindExpression(filter_expr, get);
	auto filter = make_uniq<LogicalFilter>(std::move(bound_expr));
	filter->children.push_back(std::move(op_ptr));
	op_ptr = std::move(filter);
}

//===--------------------------------------------------------------------===//
// Legacy Spike Code (kept for spike tests during transition)
//===--------------------------------------------------------------------===//

void RBACOptimizerExtension::WalkPlanWithParentLegacy(unique_ptr<LogicalOperator> &op_ptr) {
	auto &op = *op_ptr;

	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		auto table = get.GetTable();

		if (table) {
			string table_name = table->name;

			// Spike 0.2: Block access to table named "blocked"
			if (table_name == "blocked") {
				throw PermissionException("Access denied to table 'blocked'");
			}

			// Spike 0.3: Inject filter for "filtered_table"
			if (table_name == "filtered_table") {
				InjectHardcodedFilter(op_ptr, get);
				return;
			}

			// Spike 0.5: Inject parsed filter for "policy_test"
			if (table_name == "policy_test") {
				InjectParsedFilter(op_ptr, get, "id > 0");
				return;
			}

			// Spike 0.5: bad column reference
			if (table_name == "policy_bad_column") {
				InjectParsedFilter(op_ptr, get, "nonexistent_column > 0");
				return;
			}

			// Spike 0.5: string filter
			if (table_name == "policy_string_filter") {
				InjectParsedFilter(op_ptr, get, "status = 'active'");
				return;
			}

			// Spike 0.6A: Log columns for col_test
			if (table_name == "col_test") {
				fprintf(stderr, "[Spike 0.6A] col_test scan - column_ids: [");
				auto &col_ids = get.GetColumnIds();
				for (idx_t i = 0; i < col_ids.size(); i++) {
					if (i > 0) fprintf(stderr, ", ");
					idx_t col_idx = col_ids[i].GetPrimaryIndex();
					fprintf(stderr, "%s", get.names[col_idx].c_str());
				}
				fprintf(stderr, "] (total: %zu)\n", col_ids.size());
				return;
			}

			// Spike 0.6B: Filter column_ids for col_test_filter
			if (table_name == "col_test_filter") {
				FilterColumnIds(get, "c_secret");
				return;
			}
		}
	}

	for (auto &child : op.children) {
		WalkPlanWithParentLegacy(child);
	}
}

void RBACOptimizerExtension::InjectHardcodedFilter(unique_ptr<LogicalOperator> &op_ptr, LogicalGet &get) {
	idx_t id_col_idx = DConstants::INVALID_INDEX;
	for (idx_t i = 0; i < get.names.size(); i++) {
		if (get.names[i] == "id") {
			id_col_idx = i;
			break;
		}
	}

	if (id_col_idx == DConstants::INVALID_INDEX) {
		return;
	}

	auto col_ref = make_uniq<BoundColumnRefExpression>(
		LogicalType::INTEGER,
		ColumnBinding(get.table_index, id_col_idx)
	);
	auto constant = make_uniq<BoundConstantExpression>(Value::INTEGER(0));
	auto comparison = make_uniq<BoundComparisonExpression>(
		ExpressionType::COMPARE_GREATERTHAN,
		std::move(col_ref),
		std::move(constant)
	);

	auto filter = make_uniq<LogicalFilter>(std::move(comparison));
	filter->children.push_back(std::move(op_ptr));
	op_ptr = std::move(filter);
}

void RBACOptimizerExtension::FilterColumnIds(LogicalGet &get, const string &forbidden_column) {
	idx_t forbidden_idx = DConstants::INVALID_INDEX;
	for (idx_t i = 0; i < get.names.size(); i++) {
		if (get.names[i] == forbidden_column) {
			forbidden_idx = i;
			break;
		}
	}

	if (forbidden_idx == DConstants::INVALID_INDEX) {
		return;
	}

	auto &col_ids = get.GetMutableColumnIds();

	fprintf(stderr, "[Spike 0.6B] Before filter - column_ids: [");
	for (idx_t i = 0; i < col_ids.size(); i++) {
		if (i > 0) fprintf(stderr, ", ");
		fprintf(stderr, "%s", get.names[col_ids[i].GetPrimaryIndex()].c_str());
	}
	fprintf(stderr, "]\n");

	vector<ColumnIndex> new_col_ids;
	for (auto &col_id : col_ids) {
		if (col_id.GetPrimaryIndex() != forbidden_idx) {
			new_col_ids.push_back(col_id);
		}
	}

	get.SetColumnIds(std::move(new_col_ids));

	fprintf(stderr, "[Spike 0.6B] After filter - column_ids: [");
	auto &updated_ids = get.GetColumnIds();
	for (idx_t i = 0; i < updated_ids.size(); i++) {
		if (i > 0) fprintf(stderr, ", ");
		fprintf(stderr, "%s", get.names[updated_ids[i].GetPrimaryIndex()].c_str());
	}
	fprintf(stderr, "]\n");
}

void RegisterRBACOptimizer(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &callback_manager = ExtensionCallbackManager::Get(db);
	callback_manager.Register(RBACOptimizerExtension::Create());
}

} // namespace duckdb
