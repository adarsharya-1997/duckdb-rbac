#pragma once

#include "duckdb.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace duckdb {

class ExtensionLoader;

//! RBACState holds per-connection RBAC identity information
//! Stored in ClientContext::registered_state under key "rbac"
struct RBACState : public ClientContextState {
	//! The current user name for this connection
	string user_name;
	//! The roles assigned to this connection
	vector<string> roles;
	//! Whether this connection has superuser privileges (bypasses all checks)
	bool is_superuser = true;
	//! Whether the identity has been explicitly initialized
	bool initialized = false;

	RBACState() : user_name("default_user"), is_superuser(true), initialized(false) {
	}

	//! Get RBACState from a ClientContext, creating it if it doesn't exist
	static shared_ptr<RBACState> Get(ClientContext &context);
};

//! RBACExtensionCallback handles connection lifecycle events
//! Ensures RBACState is initialized when a connection opens
class RBACExtensionCallback : public ExtensionCallback {
public:
	void OnConnectionOpened(ClientContext &context) override;
	void OnConnectionClosed(ClientContext &context) override;
};

//! Register RBAC scalar functions (rbac_current_user, etc.)
void RegisterRBACScalarFunctions(ExtensionLoader &loader);

} // namespace duckdb
