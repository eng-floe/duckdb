#include "catch.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

//! A connection the test provides a database to, and that database, kept with the connection.
struct ProvidedTo : ClientContextState {
	shared_ptr<AttachedDatabase> database;
};

//! Provides an in-memory database named "provided" to the marked connections.
struct TestProvider : ExtensionCallback {
	vector<Identifier> asked;
	vector<optional_idx> listed;
	bool lone_qualifiers = true;

	bool ProvidesDatabaseForLoneQualifier() const override {
		return lone_qualifiers;
	}

	shared_ptr<AttachedDatabase> ProvideDatabase(ClientContext &context, const Identifier &name) override {
		asked.push_back(name);
		auto provided_to = context.registered_state->Get<ProvidedTo>("provided_to");
		if (!provided_to || name != Identifier("provided")) {
			return nullptr;
		}
		auto &database = provided_to->database;
		if (!database) {
			AttachInfo info;
			info.name = name;
			info.path = ":memory:";
			AttachOptions options(info.options, AccessMode::AUTOMATIC);
			database = DatabaseInstance::GetDatabase(context).CreateAttachedDatabase(context, info, options);
			database->Initialize(context);
			database->FinalizeLoad(context);
			database->oid = DatabaseManager::Get(context).NextOid();
		}
		return database;
	}

	void ProvideDatabases(ClientContext &context, optional_idx max_db_count,
	                      vector<shared_ptr<AttachedDatabase>> &result) override {
		listed.push_back(max_db_count);
		if (auto provided = ProvideDatabase(context, Identifier("provided"))) {
			result.push_back(std::move(provided));
		}
	}
};

} // namespace

TEST_CASE("Test databases an extension provides to a connection", "[api]") {
	DBConfig config;
	auto provider = make_shared_ptr<TestProvider>();
	ExtensionCallback::Register(config, provider);
	DuckDB db(nullptr, &config);
	Connection provided_to(db);
	provided_to.context->registered_state->GetOrCreate<ProvidedTo>("provided_to");
	Connection other(db);

	// The connection it is provided to has it, listed among its databases; another does not.
	const string listed = "SELECT count(*) FROM duckdb_databases() WHERE database_name = 'provided'";
	REQUIRE_NO_FAIL(provided_to.Query("CREATE TABLE provided.t AS SELECT 42 AS i"));
	auto result = provided_to.Query("SELECT i FROM provided.t");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	result = provided_to.Query(listed);
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE_FAIL(other.Query("SELECT i FROM provided.main.t"));
	result = other.Query(listed);
	REQUIRE(CHECK_COLUMN(result, 0, {0}));

	// An unqualified table name is not looked up as a provided database's default table.
	REQUIRE_NO_FAIL(provided_to.Query("CREATE TEMP TABLE local_t AS SELECT 1 AS i"));
	provider->asked.clear();
	REQUIRE_NO_FAIL(provided_to.Query("SELECT i FROM local_t"));
	for (auto &name : provider->asked) {
		REQUIRE(name != Identifier("local_t"));
	}

	// An extension that provides no lone qualifier is not asked for one: `provided.t` names a schema of the
	// connection's own database, and the provided database is named in full.
	provider->lone_qualifiers = false;
	REQUIRE_NO_FAIL(provided_to.Query("CREATE SCHEMA memory.provided"));
	REQUIRE_NO_FAIL(provided_to.Query("CREATE TABLE memory.provided.t AS SELECT 7 AS i"));
	provider->asked.clear();
	result = provided_to.Query("SELECT i FROM provided.t");
	REQUIRE(CHECK_COLUMN(result, 0, {7}));
	REQUIRE(provider->asked.empty());
	result = provided_to.Query("SELECT i FROM provided.main.t");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	// Nor once the transaction uses the provided database.
	REQUIRE_NO_FAIL(provided_to.Query("BEGIN"));
	result = provided_to.Query("SELECT i FROM provided.main.t");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	result = provided_to.Query("SELECT i FROM provided.t");
	REQUIRE(CHECK_COLUMN(result, 0, {7}));
	REQUIRE_NO_FAIL(provided_to.Query("COMMIT"));
	// Nor once a listing starts a transaction on every database, the provided one first made for the listing.
	provided_to.context->registered_state->Get<ProvidedTo>("provided_to")->database.reset();
	REQUIRE_NO_FAIL(provided_to.Query("SET immediate_transaction_mode = true"));
	REQUIRE_NO_FAIL(provided_to.Query("BEGIN"));
	result = provided_to.Query("SELECT i FROM provided.t");
	REQUIRE(CHECK_COLUMN(result, 0, {7}));
	REQUIRE_NO_FAIL(provided_to.Query("COMMIT"));
	REQUIRE_NO_FAIL(provided_to.Query("RESET immediate_transaction_mode"));
	provider->lone_qualifiers = true;

	// An attached database the transaction uses is still a lone qualifier's once another connection detaches it.
	REQUIRE_NO_FAIL(other.Query("ATTACH ':memory:' AS att"));
	REQUIRE_NO_FAIL(other.Query("CREATE TABLE att.t AS SELECT 3 AS i"));
	REQUIRE_NO_FAIL(provided_to.Query("BEGIN"));
	result = provided_to.Query("SELECT i FROM att.t");
	REQUIRE(CHECK_COLUMN(result, 0, {3}));
	REQUIRE_NO_FAIL(other.Query("DETACH att"));
	result = provided_to.Query("SELECT i FROM att.t");
	REQUIRE(CHECK_COLUMN(result, 0, {3}));
	REQUIRE_NO_FAIL(provided_to.Query("COMMIT"));

	// A missing table's suggestions list the databases with a count.
	provider->listed.clear();
	REQUIRE_FAIL(provided_to.Query("SELECT * FROM no_such_table"));
	REQUIRE(!provider->listed.empty());
	for (auto &max_db_count : provider->listed) {
		REQUIRE(max_db_count.IsValid());
	}
}
