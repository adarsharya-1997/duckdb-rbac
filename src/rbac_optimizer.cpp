#include "rbac_optimizer.hpp"
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

namespace duckdb {

//===--------------------------------------------------------------------===//
// Expression Binding Helper (Spike 0.5)
//===--------------------------------------------------------------------===//

// Simple expression binder that converts ParsedExpression to bound Expression
// for use with a specific LogicalGet. Only handles simple cases needed for row policies.
class SimpleExpressionBinder {
public:
	SimpleExpressionBinder(LogicalGet &get) : get(get) {
		// Build column name -> index map
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
		// Get column name (handle both qualified and unqualified)
		string col_name;
		if (expr.column_names.size() == 1) {
			col_name = expr.column_names[0];
		} else if (expr.column_names.size() >= 2) {
			// Take last component (column name)
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

// Parse and bind an expression string against a LogicalGet
static unique_ptr<Expression> ParseAndBindExpression(const string &expr_str, LogicalGet &get) {
	// Parse the expression
	auto expressions = Parser::ParseExpressionList(expr_str);
	if (expressions.empty()) {
		throw BinderException("Failed to parse expression: %s", expr_str);
	}

	// Bind the first expression
	SimpleExpressionBinder binder(get);
	return binder.Bind(*expressions[0]);
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
	// Walk the plan tree to check permissions and inject filters
	WalkPlanWithParent(plan);
}

void RBACOptimizerExtension::WalkPlan(LogicalOperator &op) {
	// Check if this is a table scan (LogicalGet)
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();

		// Try to get the table being scanned
		auto table = get.GetTable();
		if (table) {
			string table_name = table->name;

			// Spike 0.2: Block access to table named "blocked"
			if (table_name == "blocked") {
				throw PermissionException("Access denied to table 'blocked'");
			}
		}
	}

	// Recursively check all children
	for (auto &child : op.children) {
		WalkPlan(*child);
	}
}

void RBACOptimizerExtension::WalkPlanWithParent(unique_ptr<LogicalOperator> &op_ptr) {
	auto &op = *op_ptr;

	// Check if this is a table scan (LogicalGet)
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();

		// Try to get the table being scanned
		auto table = get.GetTable();
		if (table) {
			string table_name = table->name;

			// Spike 0.2: Block access to table named "blocked"
			if (table_name == "blocked") {
				throw PermissionException("Access denied to table 'blocked'");
			}

			// Spike 0.3: Inject filter for table named "filtered_table" (hardcoded expression)
			if (table_name == "filtered_table") {
				InjectHardcodedFilter(op_ptr, get);
				// After injection, op_ptr now points to the filter, return
				return;
			}

			// Spike 0.5: Inject filter for table named "policy_test" (parsed expression)
			if (table_name == "policy_test") {
				InjectParsedFilter(op_ptr, get, "id > 0");
				return;
			}

			// Spike 0.5 error case: bad column reference
			if (table_name == "policy_bad_column") {
				InjectParsedFilter(op_ptr, get, "nonexistent_column > 0");
				return;
			}

			// Spike 0.5: more complex expression with name column
			if (table_name == "policy_string_filter") {
				InjectParsedFilter(op_ptr, get, "status = 'active'");
				return;
			}

			// ===== Spike 0.6: SELECT * Column-Level Permission Feasibility =====

			// Part A: Log what columns the optimizer sees for col_test
			if (table_name == "col_test") {
				// Just log - don't modify anything
				// This confirms optimizer sees expanded column list, not SELECT *
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

			// Part B: Try to filter out 'c_secret' column from col_test_filter
			if (table_name == "col_test_filter") {
				FilterColumnIds(get, "c_secret");
				return;
			}
		}
	}

	// Recursively process all children
	for (auto &child : op.children) {
		WalkPlanWithParent(child);
	}
}

void RBACOptimizerExtension::InjectHardcodedFilter(unique_ptr<LogicalOperator> &op_ptr, LogicalGet &get) {
	// Find the "id" column index
	idx_t id_col_idx = DConstants::INVALID_INDEX;
	for (idx_t i = 0; i < get.names.size(); i++) {
		if (get.names[i] == "id") {
			id_col_idx = i;
			break;
		}
	}

	if (id_col_idx == DConstants::INVALID_INDEX) {
		// No "id" column found, skip filter injection
		return;
	}

	// Create: id > 0
	// Left side: column reference to "id"
	auto col_ref = make_uniq<BoundColumnRefExpression>(
		LogicalType::INTEGER,
		ColumnBinding(get.table_index, id_col_idx)
	);

	// Right side: constant 0
	auto constant = make_uniq<BoundConstantExpression>(Value::INTEGER(0));

	// Comparison: id > 0
	auto comparison = make_uniq<BoundComparisonExpression>(
		ExpressionType::COMPARE_GREATERTHAN,
		std::move(col_ref),
		std::move(constant)
	);

	// Create LogicalFilter with the comparison
	auto filter = make_uniq<LogicalFilter>(std::move(comparison));

	// Move the LogicalGet to be the child of the filter
	filter->children.push_back(std::move(op_ptr));

	// Replace the original pointer with the filter
	op_ptr = std::move(filter);
}

void RBACOptimizerExtension::InjectParsedFilter(unique_ptr<LogicalOperator> &op_ptr, LogicalGet &get,
                                                 const string &filter_expr) {
	// Spike 0.5: Parse and bind the filter expression dynamically
	auto bound_expr = ParseAndBindExpression(filter_expr, get);

	// Create LogicalFilter with the bound expression
	auto filter = make_uniq<LogicalFilter>(std::move(bound_expr));

	// Move the LogicalGet to be the child of the filter
	filter->children.push_back(std::move(op_ptr));

	// Replace the original pointer with the filter
	op_ptr = std::move(filter);
}

void RBACOptimizerExtension::FilterColumnIds(LogicalGet &get, const string &forbidden_column) {
	// Spike 0.6B: Try to remove a column from the scan
	// Find the column index to remove
	idx_t forbidden_idx = DConstants::INVALID_INDEX;
	for (idx_t i = 0; i < get.names.size(); i++) {
		if (get.names[i] == forbidden_column) {
			forbidden_idx = i;
			break;
		}
	}

	if (forbidden_idx == DConstants::INVALID_INDEX) {
		// Column not in table definition - nothing to filter
		return;
	}

	// Get mutable reference to column_ids and filter out the forbidden column
	auto &col_ids = get.GetMutableColumnIds();

	fprintf(stderr, "[Spike 0.6B] Before filter - column_ids: [");
	for (idx_t i = 0; i < col_ids.size(); i++) {
		if (i > 0) fprintf(stderr, ", ");
		fprintf(stderr, "%s", get.names[col_ids[i].GetPrimaryIndex()].c_str());
	}
	fprintf(stderr, "]\n");

	// Remove the forbidden column from column_ids
	vector<ColumnIndex> new_col_ids;
	for (auto &col_id : col_ids) {
		if (col_id.GetPrimaryIndex() != forbidden_idx) {
			new_col_ids.push_back(col_id);
		}
	}

	// Replace column_ids using SetColumnIds
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
