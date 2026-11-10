#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_profiler.hpp"

using namespace duckdb;

TEST_CASE("Test that the profiler tree map is not handed out after its plan is freed", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));
	REQUIRE_NO_FAIL(con.Query("SET profiling_coverage = 'ALL'"));
	REQUIRE_NO_FAIL(con.Query("SELECT SUM(i) FROM range(1000) t(i) WHERE i % 7 = 0"));

	auto &profiler = QueryProfiler::Get(*con.context);
	bool has_tree_map = true;
	profiler.GetTreeMapUnderLock(
	    [&](optional_ptr<const QueryProfiler::TreeMap> tree_map) { has_tree_map = tree_map != nullptr; });
	REQUIRE(!has_tree_map);
}
