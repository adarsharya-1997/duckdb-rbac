#pragma once

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class ExtensionLoader;

//! RBAC Optimizer Extension - hooks into the optimizer to enforce permissions
//! and inject row policy filters
class RBACOptimizerExtension {
public:
	//! Create and return an OptimizerExtension configured for RBAC
	static OptimizerExtension Create();

	//! Pre-optimize function: runs before DuckDB's built-in optimizers
	//! Used to check permissions and throw PermissionException if denied
	static void PreOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

private:
	//! Recursively walk the plan tree looking for LogicalGet nodes
	static void WalkPlan(LogicalOperator &op);
};

//! Register the RBAC optimizer extension
void RegisterRBACOptimizer(ExtensionLoader &loader);

} // namespace duckdb
