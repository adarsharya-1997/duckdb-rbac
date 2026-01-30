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

namespace duckdb {

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

			// Spike 0.3: Inject filter for table named "filtered_table"
			if (table_name == "filtered_table") {
				InjectFilter(op_ptr, get);
				// After injection, op_ptr now points to the filter, return
				return;
			}
		}
	}

	// Recursively process all children
	for (auto &child : op.children) {
		WalkPlanWithParent(child);
	}
}

void RBACOptimizerExtension::InjectFilter(unique_ptr<LogicalOperator> &op_ptr, LogicalGet &get) {
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

void RegisterRBACOptimizer(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &callback_manager = ExtensionCallbackManager::Get(db);
	callback_manager.Register(RBACOptimizerExtension::Create());
}

} // namespace duckdb
