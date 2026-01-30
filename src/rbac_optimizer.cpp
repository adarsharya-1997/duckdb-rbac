#include "rbac_optimizer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
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
	// Walk the plan tree to check permissions
	WalkPlan(*plan);
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

void RegisterRBACOptimizer(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &callback_manager = ExtensionCallbackManager::Get(db);
	callback_manager.Register(RBACOptimizerExtension::Create());
}

} // namespace duckdb
