#pragma once

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class ExtensionLoader;
class LogicalGet;
class ClientContext;

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
	//! Walk the plan tree with ability to modify (for filter injection)
	//! context is used for permission lookups
	static void WalkPlanWithParent(ClientContext &context, unique_ptr<LogicalOperator> &op_ptr,
	                               const vector<string> &effective_roles, const string &user_name);

	//! Check table-level permissions
	//! Returns true if access is allowed, throws PermissionException if denied
	static void CheckTablePermission(ClientContext &context, const string &schema_name, const string &table_name,
	                                 const vector<string> &effective_roles, const string &user_name);

	//! Check column-level permissions
	//! Returns the set of allowed columns, throws PermissionException if any referenced column is forbidden
	static void CheckColumnPermissions(ClientContext &context, const string &schema_name, const string &table_name,
	                                   LogicalGet &get, const vector<string> &effective_roles, const string &user_name);

	//! Inject a LogicalFilter with parsed expression string (for row policies)
	static void InjectParsedFilter(unique_ptr<LogicalOperator> &op_ptr, LogicalGet &get, const string &filter_expr);

	// ===== Spike code (kept for backward compatibility during transition) =====
	//! Legacy: Walk without context (for spike tests)
	static void WalkPlanWithParentLegacy(unique_ptr<LogicalOperator> &op_ptr);
	//! Legacy: Inject hardcoded filter (Spike 0.3)
	static void InjectHardcodedFilter(unique_ptr<LogicalOperator> &op_ptr, LogicalGet &get);
	//! Legacy: Filter column_ids (Spike 0.6B)
	static void FilterColumnIds(LogicalGet &get, const string &forbidden_column);
};

//! Register the RBAC optimizer extension
void RegisterRBACOptimizer(ExtensionLoader &loader);

} // namespace duckdb
