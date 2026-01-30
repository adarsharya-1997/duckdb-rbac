#define DUCKDB_EXTENSION_MAIN

#include "quack_extension.hpp"
#include "rbac_state.hpp"
#include "rbac_optimizer.hpp"
#include "rbac_parser.hpp"
#include "duckdb.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();

	// Register extension callback for connection lifecycle (ensures RBACState exists)
	auto &callback_manager = ExtensionCallbackManager::Get(db);
	callback_manager.Register(make_shared_ptr<RBACExtensionCallback>());

	// Register RBAC scalar functions (rbac_current_user, rbac_current_roles, rbac_is_superuser, rbac_set_identity)
	RegisterRBACScalarFunctions(loader);

	// Register RBAC optimizer extension (permission checks, row policy injection)
	RegisterRBACOptimizer(loader);

	// Register RBAC parser extension (intercepts CREATE ROLE, GRANT, etc.)
	RegisterRBACParser(loader);

	// Create RBAC system tables (duckdb_roles, duckdb_table_privileges, etc.)
	CreateRBACSystemTables(db);

	// Create RBAC introspection views (duckdb_my_roles, duckdb_effective_privileges)
	// Must be called after scalar functions are registered
	CreateRBACIntrospectionViews(db);
}

void QuackExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string QuackExtension::Name() {
	return "quack";
}

std::string QuackExtension::Version() const {
#ifdef EXT_VERSION_QUACK
	return EXT_VERSION_QUACK;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(quack, loader) {
	duckdb::LoadInternal(loader);
}
}
