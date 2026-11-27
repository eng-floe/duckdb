#include "duckdb/floe/floe_estimation_extension.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/join_order/cardinality_estimator.hpp"
#include "duckdb/optimizer/join_order/join_order_operator.hpp"
#include "duckdb/optimizer/join_order/query_graph_manager.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/joinside.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

#include <cmath>

namespace duckdb {

void EstimationExtension::Register(DBConfig &config, shared_ptr<EstimationExtension> extension) {
	OptimizerExtension optimizer_extension;
	optimizer_extension.optimizer_info = std::move(extension);
	OptimizerExtension::Register(config, std::move(optimizer_extension));
}

namespace floe_estimation {

//! Runs `call`, which may change `value`; if it throws, restores `value` and logs, unless the error is an interrupt or
//! fatal. Then restores `value` if `is_valid` refuses it.
template <class T, class CALL, class IS_VALID>
static void RunGuarded(ClientContext &context, T &value, CALL &&call, IS_VALID &&is_valid) {
	auto prior = value;
	try {
		call();
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (error.Type() == ExceptionType::INTERRUPT || error.Type() == ExceptionType::FATAL) {
			throw;
		}
		value = std::move(prior);
		DUCKDB_LOG_WARNING(context, "Estimation extension failed, its estimate is dropped: %s", error.RawMessage());
		return;
	} catch (...) {
		value = std::move(prior);
		DUCKDB_LOG_WARNING(context, "Estimation extension failed, its estimate is dropped");
		return;
	}
	if (!is_valid(value)) {
		value = std::move(prior);
	}
}

//! Runs `call` as above, with any value it leaves valid.
template <class T, class CALL>
static void RunGuarded(ClientContext &context, T &value, CALL &&call) {
	RunGuarded(context, value, std::forward<CALL>(call), [](const T &) { return true; });
}

//! The registered estimation extensions, in registration order.
static vector<reference<EstimationExtension>> GetEstimationExtensions(ClientContext &context) {
	vector<reference<EstimationExtension>> result;
	for (auto &extension : OptimizerExtension::Iterate(context)) {
		auto estimation = dynamic_cast<EstimationExtension *>(extension.optimizer_info.get());
		if (estimation) {
			result.push_back(*estimation);
		}
	}
	return result;
}

bool HasEstimationExtensions(ClientContext &context) {
	return !GetEstimationExtensions(context).empty();
}

//! Whether `a` and `b` are both none, or the same selectivity from the same source and extension info.
static bool IsSameSelectivity(const optional<EstimatedSelectivity> &a, const optional<EstimatedSelectivity> &b) {
	if (!a || !b) {
		return !a && !b;
	}
	return a->selectivity == b->selectivity && a->source == b->source && a->extension_info == b->extension_info;
}

void ReplacePredicateSelectivityWithExtensions(ClientContext &context, LogicalOperator &op, const Expression &predicate,
                                               optional<EstimatedSelectivity> &selectivity) {
	for (auto &extension : GetEstimationExtensions(context)) {
		auto prior = selectivity;
		RunGuarded(
		    context, selectivity, [&]() { extension.get().EstimatePredicate(context, op, predicate, selectivity); },
		    [&](const optional<EstimatedSelectivity> &answer) {
			    return IsSameSelectivity(answer, prior) ||
			           (answer && answer->selectivity >= 0 && answer->selectivity <= 1);
		    });
		if (selectivity && !IsSameSelectivity(selectivity, prior)) {
			selectivity->source = SelectivitySource::EXTENSION;
		}
	}
}

optional<EstimatedSelectivity> EstimateFilterWithExtensions(ClientContext &context, LogicalFilter &filter,
                                                            double unestimated_selectivity) {
	if (!HasEstimationExtensions(context)) {
		return nullopt;
	}
	double extension_selectivity = 1;
	auto has_estimated = false;
	auto has_unestimated = false;
	for (auto &expression : filter.expressions) {
		auto &selectivity = expression->estimated_selectivity;
		// A predicate join ordering estimated across relations has DuckDB's default.
		if (!selectivity || selectivity->source == SelectivitySource::DEFAULT) {
			ReplacePredicateSelectivityWithExtensions(context, filter, *expression, selectivity);
		}
		if (selectivity && selectivity->source == SelectivitySource::EXTENSION) {
			extension_selectivity *= selectivity->selectivity;
			has_estimated = true;
		} else {
			has_unestimated = true;
		}
	}
	if (!has_estimated) {
		return nullopt;
	}
	if (has_unestimated) {
		// The predicates no extension estimates share DuckDB's one default.
		return EstimatedSelectivity {extension_selectivity * unestimated_selectivity, SelectivitySource::DEFAULT};
	}
	return EstimatedSelectivity {extension_selectivity, SelectivitySource::EXTENSION};
}

void ReplaceRelationEstimateWithExtensions(ClientContext &context, LogicalOperator &op, RelationStats &stats) {
	auto extensions = GetEstimationExtensions(context);
	if (extensions.empty()) {
		return;
	}
	auto bindings = op.GetColumnBindings();
	RelationEstimate estimate {stats.cardinality, {}};
	for (auto &binding : bindings) {
		auto column_stats = stats.GetColumnStats(binding);
		estimate.column_distinct_counts.push_back(column_stats ? optional_idx(column_stats->total_domain.distinct_count)
		                                                       : optional_idx());
	}
	auto prior = estimate;
	for (auto &extension : extensions) {
		auto before = estimate;
		RunGuarded(
		    context, estimate, [&]() { extension.get().EstimateRelation(context, op, estimate); },
		    [&](const RelationEstimate &answer) {
			    if (answer.column_distinct_counts.size() != before.column_distinct_counts.size()) {
				    return false;
			    }
			    for (idx_t i = 0; i < answer.column_distinct_counts.size(); i++) {
				    auto &count = answer.column_distinct_counts[i];
				    if (count != before.column_distinct_counts[i] && count.IsValid() && count.GetIndex() == 0) {
					    return false;
				    }
			    }
			    return true;
		    });
	}

	// The rows before the relation's filters, which its distinct values in the table are spread over.
	auto table_rows = stats.row_retention > 0
	                      ? LossyNumericCast<idx_t>(static_cast<double>(stats.cardinality) / stats.row_retention)
	                      : stats.cardinality;
	for (idx_t i = 0; i < bindings.size() && i < estimate.column_distinct_counts.size(); i++) {
		auto index = stats.FindColumn(bindings[i]);
		auto &distinct_count = estimate.column_distinct_counts[i];
		if (index.IsValid() && distinct_count.IsValid() && distinct_count != prior.column_distinct_counts[i]) {
			auto &column = stats.columns[index.GetIndex()];
			// DuckDB takes an extension's distinct count as measured, as a HyperLogLog sketch's.
			auto count = table_rows > 0 ? MinValue(distinct_count.GetIndex(), table_rows) : distinct_count.GetIndex();
			column.total_domain = DistinctCount(count, DistinctCountSource::HLL);
			column.current_domain =
			    RelationStatisticsHelper::EstimateCurrentDomain(column.total_domain, table_rows, stats.cardinality);
		}
	}
	if (estimate.cardinality != prior.cardinality) {
		RelationStatisticsHelper::ApplyCardinality(stats, estimate.cardinality);
		if (table_rows > 0) {
			stats.row_retention =
			    MinValue(1.0, static_cast<double>(estimate.cardinality) / static_cast<double>(table_rows));
		}
	}
	stats.CapCurrentDomainsToCardinality();
}

//! The join type an extension sees for how the cardinality estimator joins.
static EstimatedJoinType GetEstimatedJoinType(JoinOrderOperatorType type) {
	switch (type) {
	case JoinOrderOperatorType::LEFT:
		return EstimatedJoinType::LEFT;
	case JoinOrderOperatorType::SEMI:
		return EstimatedJoinType::SEMI;
	case JoinOrderOperatorType::ANTI:
		return EstimatedJoinType::ANTI;
	default:
		return EstimatedJoinType::INNER;
	}
}

//! Whether `a` and `b` share a relation.
static bool HaveCommonRelation(const JoinRelationSet &a, const JoinRelationSet &b) {
	for (idx_t i = 0; i < a.count; i++) {
		for (idx_t j = 0; j < b.count; j++) {
			if (a.relations[i] == b.relations[j]) {
				return true;
			}
		}
	}
	return false;
}

void ReplaceJoinEstimateWithExtensions(ClientContext &context, QueryGraphManager &query_graph,
                                       CardinalityEstimator &estimator, JoinRelationSet &set,
                                       const JoinEstimateSides &sides, double &cardinality) {
	auto extensions = GetEstimationExtensions(context);
	if (extensions.empty() || set.count < 2) {
		return;
	}
	auto &set_manager = query_graph.set_manager;
	auto get_operators = [&](const JoinRelationSet &relation_set) {
		vector<reference<LogicalOperator>> result;
		for (idx_t i = 0; i < relation_set.count; i++) {
			result.push_back(query_graph.relation_manager.GetRelationOperator(relation_set.relations[i].index));
		}
		return result;
	};
	// The estimator keeps each set's estimate, and has made those of the relations and of both sides already.
	JoinEstimateInput input {GetEstimatedJoinType(sides.type), get_operators(set), {}, {}, {}, 0, 0, {}};
	for (idx_t i = 0; i < set.count; i++) {
		auto &relation = set_manager.GetJoinRelation(set.relations[i]);
		input.relation_cardinalities.push_back(estimator.EstimateCardinality(relation).GetDouble());
	}
	optional_ptr<JoinRelationSet> left = sides.left;
	optional_ptr<JoinRelationSet> right = sides.right;
	if (left && right) {
		input.left = get_operators(*left);
		input.right = get_operators(*right);
		input.left_cardinality = estimator.EstimateCardinality(*left).GetDouble();
		input.right_cardinality = estimator.EstimateCardinality(*right).GetDouble();
	}
	vector<unique_ptr<Expression>> conditions;
	if (sides.op) {
		for (auto &condition : sides.op->conditions) {
			conditions.push_back(JoinCondition::CreateExpression(condition.Copy()));
			input.predicates.push_back(*conditions.back());
		}
	}
	for (auto predicate_ref : query_graph.GetPredicateModel().GetPredicates()) {
		auto &predicate = predicate_ref.get();
		auto predicate_class = predicate.GetPredicateClass();
		if (predicate_class == JoinPredicateClass::LEFT_JOIN || predicate_class == JoinPredicateClass::SEMI_ANTI_JOIN) {
			continue;
		}
		auto &filter = predicate.GetFilter();
		if (!filter.filter || !JoinRelationSet::IsSubset(set, filter.set.get())) {
			continue;
		}
		if (left && right &&
		    (!HaveCommonRelation(filter.set.get(), *left) || !HaveCommonRelation(filter.set.get(), *right))) {
			continue;
		}
		input.predicates.push_back(*filter.filter);
	}
	for (auto &extension : extensions) {
		RunGuarded(
		    context, cardinality, [&]() { extension.get().EstimateJoin(context, input, cardinality); },
		    [](double answer) { return std::isfinite(answer) && answer >= 0; });
	}
}

//! Replaces the rows of `op` and of every operator under it, children first, with `extensions`' estimates.
static void ReplaceOperatorCardinalityWithExtensions(ClientContext &context,
                                                     const vector<reference<EstimationExtension>> &extensions,
                                                     LogicalOperator &op) {
	for (auto &child : op.children) {
		ReplaceOperatorCardinalityWithExtensions(context, extensions, *child);
	}
	auto rows = op.EstimateCardinality(context);
	auto cardinality = rows;
	for (auto &extension : extensions) {
		RunGuarded(context, cardinality, [&]() { extension.get().EstimateOperator(context, op, cardinality); });
	}
	if (cardinality != rows) {
		op.SetEstimatedCardinality(cardinality);
	}
}

void ReplaceOperatorCardinalitiesWithExtensions(ClientContext &context, LogicalOperator &plan) {
	auto extensions = GetEstimationExtensions(context);
	if (!extensions.empty()) {
		ReplaceOperatorCardinalityWithExtensions(context, extensions, plan);
	}
}

} // namespace floe_estimation
} // namespace duckdb
