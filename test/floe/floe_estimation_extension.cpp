#include "catch.hpp"
#include "test_helpers.hpp"

#include <cmath>
#include <limits>
#include <map>
#include <set>

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/operator/filter/physical_filter.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/floe/floe_estimation_extension.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

using namespace duckdb;

namespace {

//! Names the method of an extension's estimate.
struct MethodNameInfo : ExtensionEstimateInfo {
	explicit MethodNameInfo(string name_p) : name(std::move(name_p)) {
	}
	string GetMethodName() const override {
		return name;
	}
	string name;
};

string GetMethodName(const optional<EstimatedSelectivity> &selectivity) {
	return selectivity && selectivity->extension_info ? selectivity->extension_info->GetMethodName() : "";
}

bool IsTable(LogicalOperator &op, const string &name) {
	return op.type == LogicalOperatorType::LOGICAL_GET && op.Cast<LogicalGet>().GetTable()->name == name;
}

//! Replaces one estimate of each kind: the scan of `big`, the join, and the aggregate.
struct FixedEstimates : EstimationExtension {
	void EstimateRelation(ClientContext &, LogicalOperator &op, RelationEstimate &estimate) override {
		if (IsTable(op, "big")) {
			estimate.cardinality = 7;
		}
	}
	void EstimateJoin(ClientContext &, const JoinEstimateInput &input, double &cardinality) override {
		if (input.relations.size() != 2) {
			return;
		}
		cardinality = 11;
		if (input.type == EstimatedJoinType::LEFT && input.left.size() == 1 && input.right.size() == 1 &&
		    input.predicates.size() == 1) {
			cardinality = 13;
		}
	}
	void EstimateOperator(ClientContext &, LogicalOperator &op, idx_t &cardinality) override {
		if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
			cardinality = 3;
		}
	}
};

//! Whether `type` is a join's.
bool IsJoin(LogicalOperatorType type) {
	return type >= LogicalOperatorType::LOGICAL_JOIN && type <= LogicalOperatorType::LOGICAL_DEPENDENT_JOIN;
}

//! Whether `type` is a join's.
bool IsJoin(PhysicalOperatorType type) {
	return (type >= PhysicalOperatorType::BLOCKWISE_NL_JOIN && type <= PhysicalOperatorType::ASOF_JOIN) ||
	       type == PhysicalOperatorType::RECURSIVE_KEY_JOIN;
}

//! Answers every estimate with its own numbers, and notes each join estimated from a relation without its rows. It
//! leaves a join's rows to its join answers.
struct EveryEstimate : EstimationExtension {
	static constexpr double SELECTIVITY = 0.5;
	static constexpr idx_t RELATION_ROWS = 1000;
	static constexpr idx_t JOIN_ROWS = 2000;
	static constexpr idx_t OPERATOR_ROWS = 3000;
	void EstimatePredicate(ClientContext &, LogicalOperator &, const Expression &,
	                       optional<EstimatedSelectivity> &selectivity) override {
		selectivity = EstimatedSelectivity {SELECTIVITY, SelectivitySource::EXTENSION};
	}
	void EstimateRelation(ClientContext &, LogicalOperator &, RelationEstimate &estimate) override {
		estimate.cardinality = RELATION_ROWS;
	}
	void EstimateJoin(ClientContext &, const JoinEstimateInput &input, double &cardinality) override {
		joins++;
		for (auto rows : input.relation_cardinalities) {
			if (rows != RELATION_ROWS) {
				missed.push_back("a join estimated from a relation of " + std::to_string(rows) + " rows");
			}
		}
		cardinality = JOIN_ROWS;
	}
	void EstimateOperator(ClientContext &, LogicalOperator &op, idx_t &cardinality) override {
		if (!IsJoin(op.type)) {
			cardinality = OPERATOR_ROWS;
		}
	}
	idx_t joins = 0;
	vector<string> missed;
};

//! Answers every relation's and operator's rows, and no selectivity.
struct RowsOnly : EstimationExtension {
	void EstimateRelation(ClientContext &, LogicalOperator &, RelationEstimate &estimate) override {
		estimate.cardinality = 7;
	}
	void EstimateOperator(ClientContext &, LogicalOperator &, idx_t &cardinality) override {
		cardinality = 7;
	}
};

//! Fails at every estimate.
struct FailingEstimates : EstimationExtension {
	void EstimatePredicate(ClientContext &, LogicalOperator &, const Expression &,
	                       optional<EstimatedSelectivity> &selectivity) override {
		selectivity = EstimatedSelectivity {0, SelectivitySource::EXTENSION};
		throw InvalidInputException("no statistics");
	}
	void EstimateRelation(ClientContext &, LogicalOperator &, RelationEstimate &estimate) override {
		estimate.cardinality = 1;
		throw InvalidInputException("no statistics");
	}
	void EstimateJoin(ClientContext &, const JoinEstimateInput &, double &cardinality) override {
		cardinality = 1;
		throw InvalidInputException("no statistics");
	}
	void EstimateOperator(ClientContext &, LogicalOperator &, idx_t &cardinality) override {
		cardinality = 1;
		throw InvalidInputException("no statistics");
	}
};

//! Answers every join with `answer`.
struct JoinAnswer : EstimationExtension {
	explicit JoinAnswer(double answer_p) : answer(answer_p) {
	}
	void EstimateJoin(ClientContext &, const JoinEstimateInput &, double &cardinality) override {
		cardinality = answer;
	}
	double answer;
};

//! Answers every filter with `answer`.
struct FilterAnswer : EstimationExtension {
	explicit FilterAnswer(double answer_p) : answer(answer_p) {
	}
	void EstimatePredicate(ClientContext &, LogicalOperator &, const Expression &,
	                       optional<EstimatedSelectivity> &selectivity) override {
		selectivity = EstimatedSelectivity {answer, SelectivitySource::EXTENSION};
	}
	double answer;
};

//! Gives `big.k` a distinct count: by default, every value distinct.
struct DistinctKeys : EstimationExtension {
	static constexpr idx_t BIG_ROWS = 100000;
	explicit DistinctKeys(idx_t count_p = BIG_ROWS) : count(count_p) {
	}
	void EstimateRelation(ClientContext &, LogicalOperator &op, RelationEstimate &estimate) override {
		if (!IsTable(op, "big")) {
			return;
		}
		auto &get = op.Cast<LogicalGet>();
		auto bindings = get.GetColumnBindings();
		for (idx_t i = 0; i < bindings.size() && i < estimate.column_distinct_counts.size(); i++) {
			if (get.GetColumnName(get.GetColumnIndex(bindings[i])) == "k") {
				estimate.column_distinct_counts[i] = count;
			}
		}
	}
	//! The distinct count it gives `big.k`.
	idx_t count;
};

//! Gives `big` `rows` rows.
struct RelationRows : EstimationExtension {
	explicit RelationRows(idx_t rows_p) : rows(rows_p) {
	}
	void EstimateRelation(ClientContext &, LogicalOperator &op, RelationEstimate &estimate) override {
		if (IsTable(op, "big")) {
			estimate.cardinality = rows;
		}
	}
	idx_t rows;
};

//! Estimates the predicates DuckDB cannot, or only assumes, on operators of type `type`.
struct UnestimatedFilters : EstimationExtension {
	UnestimatedFilters(LogicalOperatorType type_p, double answer_p) : type(type_p), answer(answer_p) {
	}
	void EstimatePredicate(ClientContext &, LogicalOperator &op, const Expression &,
	                       optional<EstimatedSelectivity> &selectivity) override {
		if (op.type == type && (!selectivity || selectivity->source == SelectivitySource::DEFAULT)) {
			selectivity =
			    EstimatedSelectivity {answer, SelectivitySource::EXTENSION, make_shared_ptr<MethodNameInfo>("filter")};
		}
	}
	LogicalOperatorType type;
	double answer;
};

//! Estimates a filter operator's `>` predicates only.
struct GreaterThanFilters : EstimationExtension {
	void EstimatePredicate(ClientContext &, LogicalOperator &op, const Expression &predicate,
	                       optional<EstimatedSelectivity> &selectivity) override {
		if (op.type == LogicalOperatorType::LOGICAL_FILTER &&
		    predicate.GetExpressionType() == ExpressionType::COMPARE_GREATERTHAN) {
			selectivity = EstimatedSelectivity {0.5, SelectivitySource::EXTENSION};
		}
	}
};

//! Keeps DuckDB's estimate of each scan filter it is asked about.
struct ReceivedScanFilters : EstimationExtension {
	void EstimatePredicate(ClientContext &, LogicalOperator &op, const Expression &,
	                       optional<EstimatedSelectivity> &selectivity) override {
		if (op.type == LogicalOperatorType::LOGICAL_GET) {
			received.push_back(selectivity);
		}
	}
	vector<optional<EstimatedSelectivity>> received;
};

//! Estimates the Top-N node and the scan filters DuckDB cannot, or only assumes.
struct AddedLater : EstimationExtension {
	void EstimatePredicate(ClientContext &, LogicalOperator &op, const Expression &,
	                       optional<EstimatedSelectivity> &selectivity) override {
		if (op.type == LogicalOperatorType::LOGICAL_GET &&
		    (!selectivity || selectivity->source == SelectivitySource::DEFAULT)) {
			selectivity = EstimatedSelectivity {0.3, SelectivitySource::EXTENSION};
		}
	}
	void EstimateOperator(ClientContext &, LogicalOperator &op, idx_t &cardinality) override {
		if (op.type == LogicalOperatorType::LOGICAL_TOP_N) {
			cardinality = 4;
		}
	}
};

//! Estimates a join as its inputs' rows, 1/100 for each predicate, and checks each side that is a join arrives with
//! its own answer for it.
struct ProductOfInputs : EstimationExtension {
	using Relations = std::set<const LogicalOperator *>;
	static Relations KeyOf(const vector<reference<LogicalOperator>> &operators) {
		Relations key;
		for (auto &op : operators) {
			key.insert(&op.get());
		}
		return key;
	}
	void CheckSide(const vector<reference<LogicalOperator>> &side, double cardinality) {
		if (side.size() < 2) {
			return;
		}
		join_sides++;
		auto answer = answers.find(KeyOf(side));
		matched += answer != answers.end() && answer->second == cardinality;
	}
	void EstimateJoin(ClientContext &, const JoinEstimateInput &input, double &cardinality) override {
		double rows = 1;
		if (input.left.empty()) {
			for (auto relation_rows : input.relation_cardinalities) {
				rows *= relation_rows;
			}
		} else {
			CheckSide(input.left, input.left_cardinality);
			CheckSide(input.right, input.right_cardinality);
			rows = input.left_cardinality * input.right_cardinality;
		}
		for (idx_t i = 0; i < input.predicates.size(); i++) {
			rows /= 100;
		}
		answers[KeyOf(input.relations)] = rows;
		cardinality = rows;
	}
	std::map<Relations, double> answers;
	idx_t join_sides = 0;
	idx_t matched = 0;
};

void MakeTables(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE big AS SELECT i % 100 AS k, i AS v FROM range(100000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE small AS SELECT i AS k FROM range(10) t(i)"));
}

void Register(Connection &con, shared_ptr<EstimationExtension> extension) {
	EstimationExtension::Register(DBConfig::GetConfig(*con.context), std::move(extension));
}

optional_ptr<LogicalOperator> FindLogical(LogicalOperator &op, const std::function<bool(LogicalOperator &)> &match) {
	if (match(op)) {
		return op;
	}
	for (auto &child : op.children) {
		auto found = FindLogical(*child, match);
		if (found) {
			return found;
		}
	}
	return nullptr;
}

optional_ptr<LogicalOperator> FindLogical(LogicalOperator &op, LogicalOperatorType type) {
	return FindLogical(op, [&](LogicalOperator &candidate) { return candidate.type == type; });
}

optional_ptr<PhysicalOperator> FindPhysical(PhysicalOperator &op,
                                            const std::function<bool(PhysicalOperator &)> &match) {
	if (match(op)) {
		return op;
	}
	for (auto &child : op.children) {
		auto found = FindPhysical(child.get(), match);
		if (found) {
			return found;
		}
	}
	return nullptr;
}

//! Notes in `missed` each predicate `expression` joins, or `expression` itself, without a selectivity from `source`.
void CheckPredicates(const Expression &expression, SelectivitySource source, const string &where,
                     vector<string> &missed) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION && !expression.estimated_selectivity) {
		for (auto &child : expression.Cast<BoundConjunctionExpression>().GetChildren()) {
			CheckPredicates(*child, source, where, missed);
		}
		return;
	}
	if (!expression.estimated_selectivity || expression.estimated_selectivity->source != source) {
		missed.push_back(where + " predicate " + expression.ToString());
	}
}

//! EveryEstimate's rows for an operator of `type`.
template <class T>
idx_t GetEveryEstimateRows(T type) {
	return IsJoin(type) ? EveryEstimate::JOIN_ROWS : EveryEstimate::OPERATOR_ROWS;
}

//! Notes in `missed` each operator of `op` without EveryEstimate's rows, `rows` for `op`, and each filtering one or
//! predicate without a selectivity from `source`.
void CheckPhysicalEstimates(PhysicalOperator &op, idx_t rows, SelectivitySource source, vector<string> &missed) {
	auto name = op.GetName();
	if (op.estimated_cardinality != rows) {
		missed.push_back(name + " of " + std::to_string(op.estimated_cardinality) + " rows");
	}
	if (op.type == PhysicalOperatorType::FILTER) {
		CheckPredicates(*op.Cast<PhysicalFilter>().expression, source, name, missed);
	}
	if (op.type == PhysicalOperatorType::TABLE_SCAN && op.Cast<PhysicalTableScan>().table_filters) {
		for (auto &entry : *op.Cast<PhysicalTableScan>().table_filters) {
			if (entry.Filter().filter_type == TableFilterType::EXPRESSION_FILTER) {
				CheckPredicates(*entry.Filter().Cast<ExpressionFilter>().expr, source, name, missed);
			}
		}
	}
	if (op.type == PhysicalOperatorType::FILTER ||
	    (op.type == PhysicalOperatorType::TABLE_SCAN && op.Cast<PhysicalTableScan>().table_filters)) {
		if (!op.estimated_selectivity || op.estimated_selectivity->source != source) {
			missed.push_back(name + " selectivity");
		}
	}
	for (auto &child : op.children) {
		CheckPhysicalEstimates(child.get(), GetEveryEstimateRows(child.get().type), source, missed);
	}
}

//! The estimated selectivity of the only predicate of `filters`.
const optional<EstimatedSelectivity> &GetOnlyFilterSelectivity(TableFilterSet &filters) {
	optional_ptr<const optional<EstimatedSelectivity>> result;
	idx_t count = 0;
	for (auto &entry : filters) {
		result = entry.Filter().Cast<ExpressionFilter>().expr->estimated_selectivity;
		count++;
	}
	REQUIRE(count == 1);
	return *result;
}

//! The plan of a scan of `big` with a filter, joined to `small` and aggregated, with `extensions` registered in order.
unique_ptr<LogicalOperator> PlanFixedQuery(Connection &con, vector<shared_ptr<EstimationExtension>> extensions) {
	MakeTables(con);
	for (auto &extension : extensions) {
		Register(con, std::move(extension));
	}
	return con.ExtractPlan("SELECT small.k, count(*) FROM big JOIN small ON big.k = small.k WHERE big.v > 5 "
	                       "GROUP BY small.k");
}

//! The physical plan of a Top-N, which replaces ORDER BY and LIMIT and adds an optional filter on `v` to the scan
//! after join ordering.
unique_ptr<PhysicalPlan> PlanTopN(Connection &con) {
	MakeTables(con);
	Register(con, make_shared_ptr<AddedLater>());
	return PhysicalPlanGenerator(*con.context)
	    .Plan(con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.k = small.k WHERE big.k = 5 "
	                          "ORDER BY big.v LIMIT 5"));
}

} // namespace

TEST_CASE("An estimation extension replaces a relation's rows", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto logical = PlanFixedQuery(con, {make_shared_ptr<FixedEstimates>()});
	auto scan = FindLogical(*logical, [](LogicalOperator &op) { return IsTable(op, "big"); });
	REQUIRE(scan);
	REQUIRE(scan->estimated_cardinality == 7);
}

TEST_CASE("An operator whose rows an estimation extension replaces keeps the selectivity of its predicates",
          "[optimizer][estimation]") {
	// The selectivities of the scan of `big` and of the filter on the aggregate.
	auto get_selectivities = [](vector<shared_ptr<EstimationExtension>> extensions) {
		DuckDB db(nullptr);
		Connection con(db);
		MakeTables(con);
		for (auto &extension : extensions) {
			Register(con, std::move(extension));
		}
		// The filter on the aggregate is rebuilt after join ordering, and estimated at physical planning.
		con.BeginTransaction();
		auto physical = PhysicalPlanGenerator(*con.context)
		                    .Plan(con.ExtractPlan("SELECT * FROM (SELECT k, count(*) AS c FROM big WHERE v > 5 "
		                                          "GROUP BY k) a JOIN small ON a.k = small.k WHERE a.c > 5"));
		auto filter = FindPhysical(physical->Root(),
		                           [](PhysicalOperator &op) { return op.type == PhysicalOperatorType::FILTER; });
		auto scan = FindPhysical(physical->Root(), [](PhysicalOperator &op) {
			return op.type == PhysicalOperatorType::TABLE_SCAN && op.Cast<PhysicalTableScan>().table_filters;
		});
		REQUIRE(scan);
		REQUIRE(filter);
		REQUIRE(scan->estimated_selectivity);
		REQUIRE(filter->estimated_selectivity);
		vector<EstimatedSelectivity> result {*scan->estimated_selectivity, *filter->estimated_selectivity};
		con.Rollback();
		return result;
	};
	auto before = get_selectivities({});
	auto after = get_selectivities({make_shared_ptr<RowsOnly>()});
	REQUIRE(before.size() == after.size());
	for (idx_t i = 0; i < before.size(); i++) {
		REQUIRE(after[i].selectivity == before[i].selectivity);
		REQUIRE(after[i].source == before[i].source);
	}
}

TEST_CASE("An estimation extension replaces a join's rows", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto logical = PlanFixedQuery(con, {make_shared_ptr<FixedEstimates>()});
	auto join = FindLogical(*logical, LogicalOperatorType::LOGICAL_COMPARISON_JOIN);
	REQUIRE(join);
	REQUIRE(join->estimated_cardinality == 11);
}

TEST_CASE("An estimation extension replaces an operator's rows", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto logical = PlanFixedQuery(con, {make_shared_ptr<FixedEstimates>()});
	auto aggregate = FindLogical(*logical, LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY);
	REQUIRE(aggregate);
	REQUIRE(aggregate->estimated_cardinality == 3);
}

TEST_CASE("A later estimation extension gets an earlier one's answer, and its own answer replaces it",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto logical = PlanFixedQuery(con, {make_shared_ptr<JoinAnswer>(11), make_shared_ptr<JoinAnswer>(17)});
	auto join = FindLogical(*logical, LogicalOperatorType::LOGICAL_COMPARISON_JOIN);
	REQUIRE(join);
	REQUIRE(join->estimated_cardinality == 17);
}

TEST_CASE("An estimation extension's invalid answer drops its answer only", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto logical = PlanFixedQuery(con, {make_shared_ptr<FilterAnswer>(0.3), make_shared_ptr<FilterAnswer>(-1)});
	auto scan = FindLogical(*logical, [](LogicalOperator &op) { return IsTable(op, "big"); });
	REQUIRE(scan);
	auto &filter = GetOnlyFilterSelectivity(scan->Cast<LogicalGet>().table_filters);
	REQUIRE(filter);
	REQUIRE(filter->selectivity == 0.3);
	REQUIRE(filter->source == SelectivitySource::EXTENSION);
}

TEST_CASE("An estimation extension's invalid answer keeps DuckDB's estimate", "[optimizer][estimation]") {
	const auto query = "SELECT big.v FROM big JOIN small ON big.k = small.k WHERE big.v > 5";
	auto get_estimates = [&](shared_ptr<EstimationExtension> extension) {
		DuckDB db(nullptr);
		Connection con(db);
		MakeTables(con);
		if (extension) {
			Register(con, std::move(extension));
		}
		auto logical = con.ExtractPlan(query);
		auto join = FindLogical(*logical, LogicalOperatorType::LOGICAL_COMPARISON_JOIN);
		auto get = FindLogical(*logical, [](LogicalOperator &op) { return IsTable(op, "big"); });
		auto &filter = GetOnlyFilterSelectivity(get->Cast<LogicalGet>().table_filters);
		return std::make_pair(join->estimated_cardinality, filter->source);
	};
	auto duckdb = get_estimates(nullptr);
	for (auto answer : {std::nan(""), -1.0, std::numeric_limits<double>::infinity()}) {
		REQUIRE(get_estimates(make_shared_ptr<JoinAnswer>(answer)) == duckdb);
	}
	for (auto answer : {std::nan(""), -0.5, 1.5}) {
		REQUIRE(get_estimates(make_shared_ptr<FilterAnswer>(answer)) == duckdb);
	}
}

TEST_CASE("An estimation extension sees the type and sides of a non-inner join", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	Register(con, make_shared_ptr<FixedEstimates>());

	auto logical = con.ExtractPlan("SELECT big.v, small.k FROM big LEFT JOIN small ON big.k = small.k");
	auto join = FindLogical(*logical, LogicalOperatorType::LOGICAL_COMPARISON_JOIN);
	REQUIRE(join);
	REQUIRE(join->estimated_cardinality == 13);
}

TEST_CASE("A failing estimation extension changes no estimate", "[optimizer][estimation]") {
	auto get_estimates = [](vector<shared_ptr<EstimationExtension>> extensions) {
		DuckDB db(nullptr);
		Connection con(db);
		auto logical = PlanFixedQuery(con, std::move(extensions));
		auto scan = FindLogical(*logical, [](LogicalOperator &op) { return IsTable(op, "big"); });
		auto &filter = GetOnlyFilterSelectivity(scan->Cast<LogicalGet>().table_filters);
		REQUIRE(filter);
		return vector<double> {
		    double(scan->estimated_cardinality),
		    double(FindLogical(*logical, LogicalOperatorType::LOGICAL_COMPARISON_JOIN)->estimated_cardinality),
		    double(FindLogical(*logical, LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY)->estimated_cardinality),
		    filter->selectivity, double(filter->source)};
	};
	REQUIRE(get_estimates({make_shared_ptr<FailingEstimates>()}) == get_estimates({}));
}

TEST_CASE("An estimation extension's distinct counts drive DuckDB's join estimate", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	const auto query = "SELECT big.v FROM big JOIN small ON big.k = small.k";
	auto before = con.ExtractPlan(query);
	Register(con, make_shared_ptr<DistinctKeys>());
	auto after = con.ExtractPlan(query);
	auto rows_before = FindLogical(*before, LogicalOperatorType::LOGICAL_COMPARISON_JOIN)->estimated_cardinality;
	auto rows_after = FindLogical(*after, LogicalOperatorType::LOGICAL_COMPARISON_JOIN)->estimated_cardinality;
	// Joining on a unique key keeps at most the rows of the other side.
	REQUIRE(rows_before > 10);
	REQUIRE(rows_after <= 10);
}

TEST_CASE("An estimation extension's distinct count holds in the rows a relation's filters keep",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	Register(con, make_shared_ptr<DistinctKeys>());
	auto logical = con.ExtractPlan("SELECT * FROM (SELECT k, count(*) FROM big WHERE v < 1000 GROUP BY k) a "
	                               "JOIN small USING (k)");
	auto scan = FindLogical(*logical, [](LogicalOperator &op) { return IsTable(op, "big"); });
	auto aggregate = FindLogical(*logical, LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY);
	REQUIRE(scan);
	REQUIRE(aggregate);
	// A key unique in the table is unique in the rows the filter keeps: each is a group of its own.
	REQUIRE(aggregate->estimated_cardinality == scan->estimated_cardinality);
}

//! The rows of a join on `big.k`, with `extensions` registered.
static idx_t GetKeyJoinRows(vector<shared_ptr<EstimationExtension>> extensions) {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	for (auto &extension : extensions) {
		Register(con, std::move(extension));
	}
	auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.k = small.k");
	return FindLogical(*logical, LogicalOperatorType::LOGICAL_COMPARISON_JOIN)->estimated_cardinality;
}

TEST_CASE("An estimation extension's distinct count of 0 is dropped", "[optimizer][estimation]") {
	REQUIRE(GetKeyJoinRows({make_shared_ptr<DistinctKeys>(0)}) == GetKeyJoinRows({}));
}

TEST_CASE("An estimation extension's distinct count above the table's rows counts as its rows",
          "[optimizer][estimation]") {
	REQUIRE(GetKeyJoinRows({make_shared_ptr<DistinctKeys>(DistinctKeys::BIG_ROWS * 1000)}) ==
	        GetKeyJoinRows({make_shared_ptr<DistinctKeys>(DistinctKeys::BIG_ROWS)}));
}

TEST_CASE("An estimation extension gets DuckDB's estimate of a scan filter, if it makes one",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	auto extension = make_shared_ptr<ReceivedScanFilters>();
	Register(con, extension);

	// DuckDB assumes a range keeps every row.
	con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.k = small.k WHERE big.v > 5");
	REQUIRE(!extension->received.empty());
	REQUIRE(extension->received[0]);
	REQUIRE(extension->received[0]->selectivity == 1.0);
	REQUIRE(extension->received[0]->source == SelectivitySource::DEFAULT);

	// The row id has no statistics: DuckDB does not estimate its filter.
	extension->received.clear();
	con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.k = small.k WHERE big.rowid > 5");
	REQUIRE(!extension->received.empty());
	for (auto &selectivity : extension->received) {
		REQUIRE(!selectivity);
	}
}

TEST_CASE("An estimation extension's scan filter selectivity sets the scan's rows", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	Register(con, make_shared_ptr<UnestimatedFilters>(LogicalOperatorType::LOGICAL_GET, 0.5));

	auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.k = small.k WHERE big.v > 5");
	auto get = FindLogical(*logical, [](LogicalOperator &op) { return IsTable(op, "big"); });
	REQUIRE(get);
	auto &filter = GetOnlyFilterSelectivity(get->Cast<LogicalGet>().table_filters);
	REQUIRE(filter);
	REQUIRE(filter->selectivity == 0.5);
	REQUIRE(filter->source == SelectivitySource::EXTENSION);
	REQUIRE(GetMethodName(filter) == "filter");
	REQUIRE(get->estimated_selectivity->selectivity == 0.5);
	REQUIRE(get->estimated_selectivity->source == SelectivitySource::EXTENSION);
	REQUIRE(GetMethodName(get->estimated_selectivity) == "filter");
	REQUIRE(get->estimated_cardinality == 50000);
}

TEST_CASE("An estimation extension's scan filter estimate replaces DuckDB's default", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	// DuckDB cannot estimate a filter on the row id, and assumes the scan keeps 20% of its rows.
	Register(con, make_shared_ptr<UnestimatedFilters>(LogicalOperatorType::LOGICAL_GET, 1));

	auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.k = small.k WHERE big.rowid >= 0");
	auto get = FindLogical(*logical, [](LogicalOperator &op) { return IsTable(op, "big"); });
	REQUIRE(get);
	REQUIRE(get->estimated_cardinality == 100000);
	REQUIRE(get->estimated_selectivity->source == SelectivitySource::EXTENSION);
}

TEST_CASE("An estimation extension's predicate selectivity sets a filter's rows", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	Register(con, make_shared_ptr<UnestimatedFilters>(LogicalOperatorType::LOGICAL_FILTER, 0.5));
	// Join ordering estimates the filter on the aggregate's `c`.
	auto logical = con.ExtractPlan("SELECT * FROM (SELECT k, count(*) AS c FROM big GROUP BY k) a JOIN big b "
	                               "ON a.k = b.k WHERE a.c > 5");
	auto filter = FindLogical(*logical, LogicalOperatorType::LOGICAL_FILTER);
	REQUIRE(filter);
	REQUIRE(filter->estimated_selectivity);
	REQUIRE(filter->estimated_selectivity->selectivity == 0.5);
	REQUIRE(filter->estimated_cardinality == filter->children[0]->estimated_cardinality / 2);
}

TEST_CASE("An estimation extension's predicate selectivity sets the selectivity of a filter added after join "
          "ordering, not its rows",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	Register(con, make_shared_ptr<UnestimatedFilters>(LogicalOperatorType::LOGICAL_FILTER, 0.5));
	con.BeginTransaction();
	auto logical = con.ExtractPlan("SELECT * FROM (SELECT k, count(*) AS c FROM big GROUP BY k) a "
	                               "JOIN small ON a.k = small.k WHERE a.c > 5");
	// As if a pass after join ordering had added the filter.
	auto &added = *FindLogical(*logical, LogicalOperatorType::LOGICAL_FILTER);
	added.expressions[0]->estimated_selectivity = nullopt;
	added.estimated_selectivity = nullopt;
	auto rows = added.estimated_cardinality;
	auto physical = PhysicalPlanGenerator(*con.context).Plan(std::move(logical));
	auto filter =
	    FindPhysical(physical->Root(), [](PhysicalOperator &op) { return op.type == PhysicalOperatorType::FILTER; });
	REQUIRE(filter);
	REQUIRE(filter->estimated_selectivity->selectivity == 0.5);
	REQUIRE(filter->estimated_cardinality == rows);
	con.Rollback();
}

TEST_CASE("A filter keeps DuckDB's default once for the predicates no estimation extension estimates",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	Register(con, make_shared_ptr<GreaterThanFilters>());

	con.BeginTransaction();
	auto logical = con.ExtractPlan("SELECT big.v FROM big JOIN small ON big.k = small.k "
	                               "WHERE big.v + big.k > 5 AND (big.v * big.k) % 7 = 1");
	REQUIRE(FindLogical(*logical, LogicalOperatorType::LOGICAL_FILTER)->expressions.size() == 2);
	auto physical = PhysicalPlanGenerator(*con.context).Plan(std::move(logical));
	auto filter =
	    FindPhysical(physical->Root(), [](PhysicalOperator &op) { return op.type == PhysicalOperatorType::FILTER; });
	REQUIRE(filter);
	REQUIRE(filter->estimated_selectivity);
	REQUIRE(filter->estimated_selectivity->selectivity == Approx(0.5 * RelationStatisticsHelper::DEFAULT_SELECTIVITY));
	REQUIRE(filter->estimated_selectivity->source == SelectivitySource::DEFAULT);
	con.Rollback();
}

TEST_CASE("An estimation extension estimates an operator added after join ordering", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto physical = PlanTopN(con);
	auto top_n =
	    FindPhysical(physical->Root(), [](PhysicalOperator &op) { return op.type == PhysicalOperatorType::TOP_N; });
	REQUIRE(top_n);
	REQUIRE(top_n->estimated_cardinality == 4);
	con.Rollback();
}

TEST_CASE("An estimation extension estimates an optional filter added after join ordering", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto physical = PlanTopN(con);
	auto scan = FindPhysical(physical->Root(), [](PhysicalOperator &op) {
		return op.type == PhysicalOperatorType::TABLE_SCAN && op.Cast<PhysicalTableScan>().table_filters;
	});
	REQUIRE(scan);
	optional<EstimatedSelectivity> optional_filter;
	for (auto &entry : *scan->Cast<PhysicalTableScan>().table_filters) {
		if (ExpressionFilter::IsOptionalFilter(entry.Filter())) {
			optional_filter = entry.Filter().Cast<ExpressionFilter>().expr->estimated_selectivity;
		}
	}
	REQUIRE(optional_filter);
	REQUIRE(optional_filter->selectivity == 0.3);
	con.Rollback();
}

TEST_CASE("An estimation extension estimates a join from the rows of its inputs", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE tiny AS SELECT i AS k FROM range(5) t(i)"));
	auto extension = make_shared_ptr<ProductOfInputs>();
	Register(con, extension);

	// An inner join of tables is estimated from the tables' rows.
	auto logical = con.ExtractPlan("SELECT count(*) FROM big JOIN small ON big.k = small.k");
	auto join = FindLogical(*logical, LogicalOperatorType::LOGICAL_COMPARISON_JOIN);
	REQUIRE(join);
	REQUIRE(join->estimated_cardinality == 100000 * 10 / 100);

	// A join whose side is a join is estimated from that side's estimate.
	con.ExtractPlan("SELECT count(*) FROM (big JOIN small ON big.k = small.k) LEFT JOIN tiny ON small.k = tiny.k");
	REQUIRE(extension->join_sides > 0);
	REQUIRE(extension->matched == extension->join_sides);
}

TEST_CASE("An estimation extension makes every estimate of a plan", "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	auto extension = make_shared_ptr<EveryEstimate>();
	Register(con, extension);

	// Each plan shape DuckDB estimates: joins of each type, subqueries, CTEs, windows, unions and filters added after
	// join ordering.
	const vector<string> queries = {
	    "SELECT small.k, count(*) FROM big JOIN small ON big.k = small.k WHERE big.v > 5 AND big.v % 7 = 1 "
	    "GROUP BY small.k",
	    "SELECT * FROM big LEFT JOIN small ON big.k = small.k WHERE big.v + small.k > 5",
	    "SELECT v FROM big WHERE k IN (SELECT k FROM small) ORDER BY v LIMIT 5",
	    "SELECT v FROM big WHERE NOT EXISTS (SELECT 1 FROM small WHERE small.k = big.k)",
	    "SELECT v FROM big WHERE v > (SELECT avg(k) FROM small WHERE small.k = big.k)",
	    "WITH c AS MATERIALIZED (SELECT k, sum(v) AS s FROM big GROUP BY k) SELECT * FROM c JOIN small USING (k) "
	    "WHERE s > 10",
	    "SELECT k, row_number() OVER (PARTITION BY k ORDER BY v) FROM big WHERE v < 100",
	    "SELECT k FROM big WHERE v < 10 UNION SELECT k FROM small",
	    "SELECT * FROM big LEFT JOIN small ON big.k = small.k WHERE big.v > 5 OR small.k < 3",
	};
	for (auto &query : queries) {
		con.BeginTransaction();
		auto logical = con.ExtractPlan(query);
		// The physical root has the logical root's rows, as the operators planned with it.
		auto root_rows = GetEveryEstimateRows(logical->type);
		auto physical = PhysicalPlanGenerator(*con.context).Plan(std::move(logical));
		CheckPhysicalEstimates(physical->Root(), root_rows, SelectivitySource::EXTENSION, extension->missed);
		con.Rollback();
		INFO(query);
		REQUIRE(extension->missed == vector<string>());
	}
	REQUIRE(extension->joins > 0);
}

TEST_CASE("An estimation extension's relation rows set the share of the table's rows the relation keeps",
          "[optimizer][estimation]") {
	DuckDB db(nullptr);
	Connection con(db);
	MakeTables(con);
	Register(con, make_shared_ptr<RelationRows>(50000));
	auto logical = con.ExtractPlan("SELECT * FROM big");
	auto scan = FindLogical(*logical, [](LogicalOperator &op) { return IsTable(op, "big"); });
	REQUIRE(scan);
	RelationStats stats;
	stats.cardinality = 20000;
	stats.row_retention = 0.2;
	floe_estimation::ReplaceRelationEstimateWithExtensions(*con.context, *scan, stats);
	REQUIRE(stats.cardinality == 50000);
	REQUIRE(stats.row_retention == 0.5);
}
