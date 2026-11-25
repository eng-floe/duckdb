#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/execution/operator/filter/physical_filter.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/floe/floe_selectivity.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

#include <cmath>
#include <algorithm>
#include <functional>

using namespace duckdb;

namespace {

optional_ptr<LogicalGet> FindFilteredGet(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET && op.Cast<LogicalGet>().table_filters.HasFilters()) {
		return op.Cast<LogicalGet>();
	}
	for (auto &child : op.children) {
		auto found = FindFilteredGet(*child);
		if (found) {
			return found;
		}
	}
	return nullptr;
}

optional_ptr<LogicalFilter> FindLogicalFilter(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		return op.Cast<LogicalFilter>();
	}
	for (auto &child : op.children) {
		auto found = FindLogicalFilter(*child);
		if (found) {
			return found;
		}
	}
	return nullptr;
}

optional_ptr<PhysicalOperator> FindPhysical(PhysicalOperator &op, PhysicalOperatorType type) {
	if (op.type == type) {
		return op;
	}
	for (auto &child : op.children) {
		auto found = FindPhysical(child.get(), type);
		if (found) {
			return found;
		}
	}
	return nullptr;
}

//! The predicate of the only filter in `filters`.
Expression &GetOnlyPredicate(TableFilterSet &filters) {
	optional_ptr<Expression> result;
	idx_t count = 0;
	for (auto &entry : filters) {
		result = *entry.Filter().Cast<ExpressionFilter>().expr;
		count++;
	}
	REQUIRE(count == 1);
	return *result;
}

void CreateTables(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE big AS SELECT i % 100 AS k, i AS v FROM range(100000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE small AS SELECT i AS k FROM range(10) t(i)"));
}

} // namespace

TEST_CASE("A scan has the selectivity of its filter keeping the fewest rows", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	con.BeginTransaction();
	// DuckDB keeps 1 row in 100 for `k = 5` and 1 in 100000 for `v = 7`: the scan keeps the fewer, not their product.
	auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.k = small.k WHERE big.k = 5 AND big.v = 7");
	auto physical = PhysicalPlanGenerator(*con.context).Plan(std::move(logical));
	optional_ptr<PhysicalOperator> scan;
	std::function<void(PhysicalOperator &)> find_big = [&](PhysicalOperator &op) {
		if (op.type == PhysicalOperatorType::TABLE_SCAN && op.Cast<PhysicalTableScan>().table_filters &&
		    op.Cast<PhysicalTableScan>().table_filters->FilterCount() == 2) {
			scan = op;
		}
		for (auto &child : op.children) {
			find_big(child.get());
		}
	};
	find_big(physical->Root());
	REQUIRE(scan);
	vector<double> selectivities;
	for (auto &entry : *scan->Cast<PhysicalTableScan>().table_filters) {
		auto &selectivity = entry.Filter().Cast<ExpressionFilter>().expr->estimated_selectivity;
		REQUIRE(selectivity);
		selectivities.push_back(selectivity->selectivity);
	}
	REQUIRE(selectivities.size() == 2);
	auto fewest = *std::min_element(selectivities.begin(), selectivities.end());
	REQUIRE(fewest < *std::max_element(selectivities.begin(), selectivities.end()));
	REQUIRE(scan->estimated_selectivity);
	REQUIRE(scan->estimated_selectivity->selectivity == fewest);
	con.Rollback();
}

TEST_CASE("A scan filter DuckDB does not cut keeps every row by its default", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.k = small.k WHERE big.v > 5");
	auto get = FindFilteredGet(*logical);
	REQUIRE(get);
	auto &selectivity = GetOnlyPredicate(get->table_filters).estimated_selectivity;
	REQUIRE(selectivity);
	REQUIRE(selectivity->selectivity == 1);
	REQUIRE(selectivity->source == SelectivitySource::DEFAULT);
	// DuckDB applies its default to the scan.
	REQUIRE(get->estimated_selectivity);
	REQUIRE(get->estimated_selectivity->selectivity == RelationStatisticsHelper::DEFAULT_SELECTIVITY);
	REQUIRE(get->estimated_selectivity->source == SelectivitySource::DEFAULT);
}

TEST_CASE("A scan whose filters statistics prove keep every row keeps them all by statistics",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	// `k` is from 0 to 99.
	auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.v = small.k WHERE big.k >= 0");
	auto get = FindFilteredGet(*logical);
	REQUIRE(get);
	REQUIRE(get->estimated_selectivity);
	REQUIRE(get->estimated_selectivity->selectivity == 1);
	REQUIRE(get->estimated_selectivity->source == SelectivitySource::STATISTICS);
}

TEST_CASE("A scan filter's selectivity from a distinct count comes from statistics", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE named AS SELECT i % 100 AS k, 'n' || i AS s FROM range(100000) t(i)"));

	// The equality is estimated from the column's distinct count, the NOT LIKE by DuckDB's default; the scan keeps the
	// fewer rows, the equality's.
	auto logical = con.ExtractPlan("SELECT named.s FROM named JOIN small ON named.k + 1 = small.k "
	                               "WHERE named.k = 5 AND named.s NOT LIKE 'm_%x'");
	auto get = FindFilteredGet(*logical);
	REQUIRE(get);
	for (auto &entry : get->table_filters) {
		auto &selectivity = entry.Filter().Cast<ExpressionFilter>().expr->estimated_selectivity;
		auto column = get->names[get->GetColumnIndex(entry.GetIndex()).GetPrimaryIndex()];
		REQUIRE(selectivity);
		REQUIRE(selectivity->source == (column == "k" ? SelectivitySource::STATISTICS : SelectivitySource::DEFAULT));
	}
	REQUIRE(get->estimated_selectivity);
	REQUIRE(get->estimated_selectivity->source == SelectivitySource::STATISTICS);
}

TEST_CASE("A scan filter's source does not depend on how many rows it keeps", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);
	// On 3 rows with 2 distinct values an equality keeps most rows, as a filter DuckDB does not estimate would.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE tiny AS SELECT i % 2 AS k FROM range(3) t(i)"));
	auto logical = con.ExtractPlan("SELECT tiny.k FROM tiny JOIN small ON tiny.k + 1 = small.k WHERE tiny.k = 1");
	auto get = FindFilteredGet(*logical);
	REQUIRE(get);
	auto &selectivity = GetOnlyPredicate(get->table_filters).estimated_selectivity;
	REQUIRE(selectivity);
	REQUIRE(selectivity->source == SelectivitySource::STATISTICS);
}

TEST_CASE("A scan filter's conjunction has the source of its child keeping the fewest rows, statistics on a tie",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);
	auto filtered = [&](const string &predicate) {
		auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.v = small.k WHERE big.k " + predicate);
		auto get = FindFilteredGet(*logical);
		REQUIRE(get);
		return GetOnlyPredicate(get->table_filters).Copy();
	};
	ExpressionFilter conjunction(
	    make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, filtered("< 50"), filtered("= 5")));
	// On a column with one value the equality keeps every row, as the range DuckDB does not estimate does.
	auto stats = NumericStats::CreateEmpty(LogicalType::BIGINT);
	NumericStats::SetMin(stats, Value::BIGINT(5));
	NumericStats::SetMax(stats, Value::BIGINT(5));
	auto source = SelectivitySource::DEFAULT;
	REQUIRE(RelationStatisticsHelper::InspectTableFilter(4, conjunction, stats, source) == 4);
	REQUIRE(source == SelectivitySource::STATISTICS);
}

TEST_CASE("A filter operator has DuckDB's default selectivity, and its predicates none", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	// DuckDB estimates the filter once, whatever its predicates. The join's keys share their range, so statistics
	// propagation adds no filter that would make filter pushdown build this one again.
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE keys AS SELECT i AS k FROM range(100) t(i)"));
	auto logical = con.ExtractPlan("SELECT * FROM big JOIN keys ON big.k = keys.k "
	                               "WHERE random() < big.v AND random() < big.v + 1");
	auto filter = FindLogicalFilter(*logical);
	REQUIRE(filter);
	REQUIRE(filter->estimated_selectivity);
	REQUIRE(filter->estimated_selectivity->selectivity == RelationStatisticsHelper::DEFAULT_SELECTIVITY);
	REQUIRE(filter->estimated_selectivity->source == SelectivitySource::DEFAULT);
	for (auto &predicate : filter->expressions) {
		REQUIRE(!predicate->estimated_selectivity);
	}
}

TEST_CASE("A predicate's estimated selectivity is used instead of estimating it again", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	con.BeginTransaction();
	auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.v = small.k WHERE big.k = 5");
	auto get = FindFilteredGet(*logical);
	REQUIRE(get);
	// DuckDB estimates `k = 5` at 1 row in 100.
	GetOnlyPredicate(get->table_filters).estimated_selectivity = EstimatedSelectivity {0.5, SelectivitySource::DEFAULT};
	auto stats = RelationStatisticsHelper::ExtractGetStats(*get, *con.context);
	REQUIRE(stats.cardinality == 50000);
	con.Rollback();
}

TEST_CASE("A predicate's selectivity keeps at least one row of a scan", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	con.BeginTransaction();
	auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.v = small.k WHERE big.k = 5");
	auto get = FindFilteredGet(*logical);
	REQUIRE(get);
	// A selectivity DuckDB would not estimate here.
	GetOnlyPredicate(get->table_filters).estimated_selectivity =
	    EstimatedSelectivity {1e-9, SelectivitySource::STATISTICS};
	REQUIRE(RelationStatisticsHelper::ExtractGetStats(*get, *con.context).cardinality == 1);
	con.Rollback();
}

TEST_CASE("A scan filter added after join ordering is estimated at physical planning", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	con.BeginTransaction();
	auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.v = small.k WHERE big.k = 5");
	auto get = FindFilteredGet(*logical);
	REQUIRE(get);
	// As if a pass after join ordering had replaced the filter.
	GetOnlyPredicate(get->table_filters).estimated_selectivity.reset();
	auto rows = get->estimated_cardinality;
	REQUIRE(get->estimated_selectivity);
	auto selectivity = get->estimated_selectivity->selectivity;

	auto physical = PhysicalPlanGenerator(*con.context).Plan(std::move(logical));
	auto scan = FindPhysical(physical->Root(), PhysicalOperatorType::TABLE_SCAN);
	REQUIRE(scan);
	REQUIRE(GetOnlyPredicate(*scan->Cast<PhysicalTableScan>().table_filters).estimated_selectivity);
	// The scan keeps the rows DuckDB planned it with; its filter, estimated again, gives it the same selectivity.
	REQUIRE(scan->estimated_cardinality == rows);
	REQUIRE(scan->estimated_selectivity);
	REQUIRE(scan->estimated_selectivity->selectivity == selectivity);
	con.Rollback();
}

TEST_CASE("Estimating a filter added after join ordering keeps its scan's other selectivities",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	con.BeginTransaction();
	auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.v = small.k WHERE big.k = 5 AND big.v > 5");
	auto get = FindFilteredGet(*logical);
	REQUIRE(get);
	REQUIRE(get->table_filters.FilterCount() >= 2);
	// One filter as if added after join ordering; another with a selectivity DuckDB would not estimate.
	const EstimatedSelectivity kept {0.123, SelectivitySource::DEFAULT};
	idx_t index = 0;
	for (auto &entry : get->table_filters) {
		auto &selectivity = entry.Filter().Cast<ExpressionFilter>().expr->estimated_selectivity;
		if (index++ == 0) {
			selectivity.reset();
		} else {
			selectivity = kept;
		}
	}

	auto physical = PhysicalPlanGenerator(*con.context).Plan(std::move(logical));
	auto scan = FindPhysical(physical->Root(), PhysicalOperatorType::TABLE_SCAN);
	REQUIRE(scan);
	idx_t kept_count = 0;
	for (auto &entry : *scan->Cast<PhysicalTableScan>().table_filters) {
		auto &selectivity = entry.Filter().Cast<ExpressionFilter>().expr->estimated_selectivity;
		REQUIRE(selectivity);
		kept_count += selectivity->selectivity == kept.selectivity;
	}
	REQUIRE(kept_count == index - 1);
	con.Rollback();
}

TEST_CASE("A filter operator built after join ordering gets a selectivity at physical planning",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	// Statistics propagation filters `k` to `small`'s range; filter pushdown then builds the filter on `c` again.
	con.BeginTransaction();
	auto logical = con.ExtractPlan("SELECT * FROM (SELECT k, count(*) AS c FROM big GROUP BY k) a "
	                               "JOIN small ON a.k = small.k WHERE a.c > 5");
	auto logical_filter = FindLogicalFilter(*logical);
	REQUIRE(logical_filter);
	// As if built after join ordering.
	logical_filter->estimated_selectivity.reset();
	auto physical = PhysicalPlanGenerator(*con.context).Plan(std::move(logical));
	auto filter = FindPhysical(physical->Root(), PhysicalOperatorType::FILTER);
	REQUIRE(filter);
	// DuckDB's default for a filter, whatever the rows DuckDB planned it with.
	REQUIRE(filter->estimated_selectivity);
	REQUIRE(filter->estimated_selectivity->selectivity == RelationStatisticsHelper::DEFAULT_SELECTIVITY);
	REQUIRE(filter->estimated_selectivity->source == SelectivitySource::DEFAULT);
	con.Rollback();
}

TEST_CASE("A filter rebuilt after join ordering gets DuckDB's default share of its child's rows",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	// Statistics propagation filters `big.k` to `small`'s range, and filter pushdown then rebuilds the filter above
	// `big`.
	auto logical = con.ExtractPlan("SELECT * FROM big JOIN small ON big.k = small.k WHERE random() < big.v");
	auto filter = FindLogicalFilter(*logical);
	REQUIRE(filter);
	REQUIRE(filter->children[0]->type == LogicalOperatorType::LOGICAL_GET);
	REQUIRE(filter->children[0]->Cast<LogicalGet>().table_filters.HasFilters());
	auto child_rows = static_cast<double>(filter->children[0]->estimated_cardinality);
	REQUIRE(filter->estimated_cardinality == idx_t(child_rows * RelationStatisticsHelper::DEFAULT_SELECTIVITY));
}

TEST_CASE("A filter join ordering applies across relations has DuckDB's default selectivity",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	auto logical =
	    con.ExtractPlan("SELECT * FROM big JOIN small ON big.k = small.k WHERE big.v > 5 OR big.k + small.k > 3");
	optional_ptr<LogicalOperator> op = logical.get();
	while (op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		op = op->children[0].get();
	}
	optional_ptr<Expression> residual;
	for (auto &condition : op->Cast<LogicalComparisonJoin>().conditions) {
		if (!condition.IsComparison()) {
			residual = condition.GetJoinExpression();
		}
	}
	REQUIRE(residual);
	REQUIRE(residual->estimated_selectivity);
	REQUIRE(residual->estimated_selectivity->selectivity == RelationStatisticsHelper::DEFAULT_SELECTIVITY);
	REQUIRE(residual->estimated_selectivity->source == SelectivitySource::DEFAULT);
}

TEST_CASE("A predicate's copy keeps its selectivity", "[optimizer][estimation]") {
	BoundConstantExpression predicate(Value::BOOLEAN(true));
	predicate.estimated_selectivity = EstimatedSelectivity {0.5, SelectivitySource::DEFAULT};
	REQUIRE(predicate.Copy()->estimated_selectivity->selectivity == 0.5);
}

TEST_CASE("A plan's copy keeps its predicates' selectivities, not its operators'", "[optimizer][estimation]") {
	// As partitioned execution copies a plan after join ordering.
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);
	con.BeginTransaction();
	auto plan = con.ExtractPlan("SELECT * FROM big JOIN small ON big.k = small.k WHERE big.k = 5");
	auto get = FindFilteredGet(*plan);
	REQUIRE(get);
	auto &selectivity = GetOnlyPredicate(get->table_filters).estimated_selectivity;
	REQUIRE(selectivity);
	auto copy = plan->Copy(*con.context);
	auto copied_get = FindFilteredGet(*copy);
	REQUIRE(copied_get);
	auto &copied = GetOnlyPredicate(copied_get->table_filters).estimated_selectivity;
	REQUIRE(copied);
	REQUIRE(copied->selectivity == selectivity->selectivity);
	REQUIRE(get->estimated_selectivity);
	REQUIRE(!copied_get->estimated_selectivity);
	con.Rollback();
}

TEST_CASE("A scan partitioned execution slices keeps the selectivity of its filters", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	// Partitioned execution splits the scan of an ORDER BY when each slice has enough row groups per thread.
	REQUIRE_NO_FAIL(con.Query("SET threads = 1"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT i::INTEGER AS i, 'a' AS s FROM range(6000000) t(i)"));
	const auto query = "SELECT i FROM t WHERE s NOT LIKE '%x_y%' ORDER BY i";

	con.BeginTransaction();
	// The selectivity of the scan before it is split.
	auto unsplit_plan = con.ExtractPlan(query);
	auto &unsplit_get = *FindFilteredGet(*unsplit_plan);
	floe_estimation::EstimateScanFilters(unsplit_get, *con.context);
	auto &unsplit = unsplit_get.estimated_selectivity;
	REQUIRE(unsplit);
	auto physical = PhysicalPlanGenerator(*con.context).Plan(con.ExtractPlan(query));
	vector<reference<PhysicalOperator>> scans;
	std::function<void(PhysicalOperator &)> find = [&](PhysicalOperator &op) {
		if (op.type == PhysicalOperatorType::TABLE_SCAN) {
			scans.push_back(op);
		}
		for (auto &child : op.children) {
			find(child.get());
		}
	};
	find(physical->Root());
	// Slices read different parts of the table; their filters keep the same share of each.
	REQUIRE(scans.size() > 1);
	for (auto &scan : scans) {
		auto &selectivity = scan.get().estimated_selectivity;
		REQUIRE(selectivity);
		REQUIRE(selectivity->selectivity == unsplit->selectivity);
	}
	con.Rollback();
}

TEST_CASE("Each physical operator has the selectivity of the logical one it was planned for",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	// The filter's unused column `v` puts a projection above the physical filter, over the scan of `big`.
	con.BeginTransaction();
	auto logical =
	    con.ExtractPlan("SELECT big.k FROM big JOIN small ON big.k = small.k WHERE big.k = 5 AND random() < big.v");
	auto physical = PhysicalPlanGenerator(*con.context).Plan(std::move(logical));
	optional_ptr<PhysicalOperator> projection;
	std::function<void(PhysicalOperator &)> find = [&](PhysicalOperator &op) {
		if (op.type == PhysicalOperatorType::PROJECTION && op.children.size() == 1 &&
		    op.children[0].get().type == PhysicalOperatorType::FILTER) {
			projection = op;
		}
		for (auto &child : op.children) {
			find(child.get());
		}
	};
	find(physical->Root());
	// The projection keeps every row: only the filter has the selectivity.
	REQUIRE(projection);
	REQUIRE(!projection->estimated_selectivity);
	auto &filter = projection->children[0].get();
	REQUIRE(filter.estimated_selectivity);
	REQUIRE(filter.estimated_selectivity->source == SelectivitySource::DEFAULT);
	auto scan = FindPhysical(filter, PhysicalOperatorType::TABLE_SCAN);
	REQUIRE(scan);
	REQUIRE(scan->estimated_selectivity);
	REQUIRE(scan->estimated_selectivity->source == SelectivitySource::STATISTICS);
	con.Rollback();
}

TEST_CASE("An operator planned as its child's physical operator leaves it the child's selectivity",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	CreateTables(con);

	// The projection of `k` under the union is planned as the scan's projection.
	con.BeginTransaction();
	auto logical = con.ExtractPlan("SELECT k FROM big WHERE v < 10 UNION SELECT k FROM small");
	auto physical = PhysicalPlanGenerator(*con.context).Plan(std::move(logical));
	auto scan = FindPhysical(physical->Root(), PhysicalOperatorType::TABLE_SCAN);
	REQUIRE(scan);
	REQUIRE(scan->Cast<PhysicalTableScan>().table_filters);
	REQUIRE(scan->estimated_selectivity);
	con.Rollback();
}

TEST_CASE("A scan DuckDB splits into a filter over a scan has its selectivity once, on the filter",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);

	// The scan takes filters on the catalog's names only; the filter operator applies the others.
	con.BeginTransaction();
	auto logical = con.ExtractPlan("SELECT * FROM duckdb_tables() WHERE schema_name = 'main' AND estimated_size = 1");
	auto physical = PhysicalPlanGenerator(*con.context).Plan(std::move(logical));
	auto filter = FindPhysical(physical->Root(), PhysicalOperatorType::FILTER);
	REQUIRE(filter);
	auto scan = FindPhysical(*filter, PhysicalOperatorType::TABLE_SCAN);
	REQUIRE(scan);
	REQUIRE(scan->Cast<PhysicalTableScan>().table_filters);
	REQUIRE(filter->estimated_selectivity);
	REQUIRE(!scan->estimated_selectivity);
	con.Rollback();
}
