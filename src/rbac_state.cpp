#include "rbac_state.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// RBACState
//===--------------------------------------------------------------------===//

shared_ptr<RBACState> RBACState::Get(ClientContext &context) {
	return context.registered_state->GetOrCreate<RBACState>("rbac");
}

//===--------------------------------------------------------------------===//
// RBACExtensionCallback
//===--------------------------------------------------------------------===//

void RBACExtensionCallback::OnConnectionOpened(ClientContext &context) {
	// Ensure RBACState exists for this connection
	// Default state: superuser with user_name "default_user"
	RBACState::Get(context);
}

void RBACExtensionCallback::OnConnectionClosed(ClientContext &context) {
	// State will be cleaned up automatically when context is destroyed
	// Nothing special needed here
}

//===--------------------------------------------------------------------===//
// Scalar Functions
//===--------------------------------------------------------------------===//

static void RBACCurrentUserFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto rbac_state = RBACState::Get(context);

	// Return the current user name
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<string_t>(result);
	result_data[0] = StringVector::AddString(result, rbac_state->user_name);
}

static void RBACCurrentRolesFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto rbac_state = RBACState::Get(context);

	// Return roles as a LIST of VARCHAR
	auto list_size = rbac_state->roles.size();
	auto list_data = ListVector::GetData(result);

	// Set the list entry
	list_data[0].offset = 0;
	list_data[0].length = list_size;

	// Add the role strings to the child vector
	auto &child_vector = ListVector::GetEntry(result);
	ListVector::SetListSize(result, list_size);

	auto child_data = FlatVector::GetData<string_t>(child_vector);
	for (idx_t i = 0; i < list_size; i++) {
		child_data[i] = StringVector::AddString(child_vector, rbac_state->roles[i]);
	}

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

static void RBACIsSuperuserFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto rbac_state = RBACState::Get(context);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto result_data = ConstantVector::GetData<bool>(result);
	result_data[0] = rbac_state->is_superuser;
}

void RegisterRBACScalarFunctions(ExtensionLoader &loader) {
	// rbac_current_user() -> VARCHAR
	auto current_user_func = ScalarFunction("rbac_current_user", {}, LogicalType::VARCHAR, RBACCurrentUserFunction);
	loader.RegisterFunction(current_user_func);

	// rbac_current_roles() -> LIST(VARCHAR)
	auto current_roles_func = ScalarFunction("rbac_current_roles", {}, LogicalType::LIST(LogicalType::VARCHAR),
	                                          RBACCurrentRolesFunction);
	loader.RegisterFunction(current_roles_func);

	// rbac_is_superuser() -> BOOLEAN
	auto is_superuser_func = ScalarFunction("rbac_is_superuser", {}, LogicalType::BOOLEAN, RBACIsSuperuserFunction);
	loader.RegisterFunction(is_superuser_func);
}

} // namespace duckdb
