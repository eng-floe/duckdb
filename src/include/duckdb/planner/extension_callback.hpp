//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/extension_callback.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/main/extension_callback_manager.hpp"

namespace duckdb {
class AttachedDatabase;
class Identifier;
struct DBConfig;
class ClientContext;
class DatabaseInstance;
class ErrorData;

class ExtensionCallback {
public:
	virtual ~ExtensionCallback() {
	}

	//! Called when a new connection is opened
	virtual void OnConnectionOpened(ClientContext &context) {
	}
	//! Called when a connection is closed
	virtual void OnConnectionClosed(ClientContext &context) {
	}
	//! Called before an extension starts loading
	virtual void OnBeginExtensionLoad(DatabaseInstance &db, const string &name) {
	}
	//! Called after an extension is finished loading
	virtual void OnExtensionLoaded(DatabaseInstance &db, const string &name) {
	}
	//! Called after an extension fails to load loading
	virtual void OnExtensionLoadFail(DatabaseInstance &db, const string &name, const ErrorData &error) {
	}
	//! The database `name` this extension provides to `context`'s connection, asked for a name no attached
	//! database has. A provided database belongs to the connection: other connections never see it. It has no
	//! default table: `FROM <name>` reads the default table of an attached database alone. The extension keeps
	//! it no longer than the database instance.
	virtual shared_ptr<AttachedDatabase> ProvideDatabase(ClientContext &context, const Identifier &name) {
		return nullptr;
	}
	//! Whether ProvideDatabase is asked for a lone qualifier, the `x` of `x.name`, which names a database when one
	//! has the name and a schema otherwise. An extension that cannot tell cheaply opts out: `x` then names an
	//! attached database or a schema, and a database it provides is named in full.
	virtual bool ProvidesDatabaseForLoneQualifier() const {
		return true;
	}
	//! Adds to `result` the databases this extension provides to `context`'s connection, asked wherever its
	//! databases are listed. With `max_db_count`, the listing need not be complete: it names candidates, such as
	//! an error's suggestions, and no more than that many are kept.
	virtual void ProvideDatabases(ClientContext &context, optional_idx max_db_count,
	                              vector<shared_ptr<AttachedDatabase>> &result) {
	}

	static void Register(DBConfig &config, shared_ptr<ExtensionCallback> extension);
	static ExtensionCallbackIteratorHelper<shared_ptr<ExtensionCallback>> Iterate(ClientContext &context) {
		return ExtensionCallbackManager::Get(context).ExtensionCallbacks();
	}
	static ExtensionCallbackIteratorHelper<shared_ptr<ExtensionCallback>> Iterate(DatabaseInstance &db) {
		return ExtensionCallbackManager::Get(db).ExtensionCallbacks();
	}
};

} // namespace duckdb
