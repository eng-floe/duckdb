#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_profiler.hpp"

#include <chrono>
#include <thread>

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

TEST_CASE("Test that a parked pipeline has reported to the profiler", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("PRAGMA enable_profiling = 'no_output'"));
	REQUIRE_NO_FAIL(con.Query("SET profiling_coverage = 'ALL'"));

	// nothing fetches the result, so its producers park once its buffer is full
	auto result = con.Submit("SELECT i FROM range(100000000) t(i)");
	REQUIRE(!result->HasError());
	auto &profiler = QueryProfiler::Get(*con.context);
	idx_t scanned = 0;
	for (idx_t attempt = 0; attempt < 200 && scanned == 0; attempt++) {
		profiler.GetTreeMapUnderLock([&](optional_ptr<const QueryProfiler::TreeMap> tree_map) {
			if (!tree_map) {
				return;
			}
			for (auto &entry : *tree_map) {
				if (entry.first.get().type == PhysicalOperatorType::TABLE_SCAN) {
					scanned = entry.second.get().GetOperatorMetrics().elements_returned;
				}
			}
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE(scanned > 0);
	REQUIRE(scanned < 100000000);
}
