#include "duckdb/floe/floe_selectivity.hpp"

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/floe/floe_estimation_extension.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

#include <cmath>

namespace duckdb {
namespace floe_estimation {

//! The rows of `rows` that `selectivity` keeps: at least one when it keeps any, as DuckDB estimates a filter.
static idx_t CountRowsKept(idx_t rows, double selectivity) {
	auto kept = LossyNumericCast<idx_t>(std::round(static_cast<double>(rows) * selectivity));
	return selectivity > 0 && rows > 0 ? MaxValue<idx_t>(kept, 1) : kept;
}

//! The rows `filter` of `get` keeps of `cardinality`: those its estimated selectivity keeps. When it has none, it is
//! estimated from `base_stats`, if any, then by the estimation extensions.
static idx_t EstimateScanFilterRows(ClientContext &context, LogicalGet &get, idx_t cardinality, TableFilter &filter,
                                    optional_ptr<BaseStatistics> base_stats) {
	auto &predicate = *ExpressionFilter::GetExpressionFilter(filter, "EstimateScanFilterRows").expr;
	auto &selectivity = predicate.estimated_selectivity;
	if (!selectivity && cardinality > 0) {
		if (base_stats) {
			auto source = SelectivitySource::DEFAULT;
			auto rows = RelationStatisticsHelper::InspectTableFilter(cardinality, filter, *base_stats, source);
			selectivity = EstimatedSelectivity {static_cast<double>(rows) / static_cast<double>(cardinality), source};
		}
		ReplacePredicateSelectivityWithExtensions(context, get, predicate, selectivity);
	}
	return selectivity ? CountRowsKept(cardinality, selectivity->selectivity) : cardinality;
}

//! Whether an estimation extension estimated `filter`.
static bool IsEstimatedByExtension(const TableFilter &filter) {
	auto &selectivity =
	    ExpressionFilter::GetExpressionFilter(filter, "IsEstimatedByExtension").expr->estimated_selectivity;
	return selectivity && selectivity->source == SelectivitySource::EXTENSION;
}

//! Whether `filter`'s column statistics prove it keeps every row.
static bool IsProvenAlwaysTrue(ClientContext &context, const ExpressionFilter &filter,
                               optional_ptr<BaseStatistics> column_statistics) {
	return column_statistics && filter.column_indexes.size() <= 1 &&
	       filter.CheckStatistics(context, *column_statistics) == FilterPropagateResult::FILTER_ALWAYS_TRUE;
}

idx_t EstimateFilteredRows(ClientContext &context, LogicalGet &get, idx_t rows,
                           optional<EstimatedSelectivity> &selectivity) {
	auto rows_kept = rows;
	optional<EstimatedSelectivity> fewest;
	auto has_non_optional_filters = false;
	auto has_statistics = false;
	for (auto &entry : get.table_filters) {
		auto column_statistics =
		    RelationStatisticsHelper::GetColumnStatistics(get, context, get.GetColumnIndex(entry.GetIndex()));
		auto &filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "EstimateFilteredRows");
		if (IsProvenAlwaysTrue(context, filter, column_statistics.get())) {
			has_statistics = true;
			continue;
		}
		// DuckDB does not estimate a filter on a column without statistics; an extension may.
		rows_kept =
		    MinValue(rows_kept, EstimateScanFilterRows(context, get, rows, entry.Filter(), column_statistics.get()));
		auto &filter_selectivity = filter.expr->estimated_selectivity;
		if (filter_selectivity) {
			has_statistics = has_statistics || filter_selectivity->source == SelectivitySource::STATISTICS;
			// A filter keeping every row by DuckDB's estimate does not decide the scan's; by an extension's it may.
			auto decides =
			    filter_selectivity->selectivity < 1 || filter_selectivity->source == SelectivitySource::EXTENSION;
			if (decides && (!fewest || filter_selectivity->selectivity < fewest->selectivity)) {
				fewest = filter_selectivity;
			}
		}
		// DuckDB's default for filters that cut no row is not for those an extension estimated.
		if (!ExpressionFilter::IsOptionalFilter(entry.Filter()) && !IsEstimatedByExtension(entry.Filter())) {
			has_non_optional_filters = true;
		}
	}
	if (rows_kept == rows && has_non_optional_filters) {
		selectivity = EstimatedSelectivity {RelationStatisticsHelper::DEFAULT_SELECTIVITY, SelectivitySource::DEFAULT};
		return MaxValue<idx_t>(LossyNumericCast<idx_t>(double(rows) * RelationStatisticsHelper::DEFAULT_SELECTIVITY),
		                       1);
	}
	if (fewest) {
		selectivity = std::move(fewest);
	} else {
		selectivity =
		    EstimatedSelectivity {1, has_statistics ? SelectivitySource::STATISTICS : SelectivitySource::DEFAULT};
	}
	return rows_kept;
}

void EstimateScanFilters(LogicalGet &get, ClientContext &context) {
	auto rows = get.EstimateSourceCardinality(context);
	optional<EstimatedSelectivity> selectivity;
	EstimateFilteredRows(context, get, rows, selectivity);
	if (rows > 0) {
		get.estimated_selectivity = std::move(selectivity);
	}
}

idx_t EstimateFilterCardinality(ClientContext &context, LogicalFilter &filter, idx_t child_cardinality) {
	if (!filter.estimated_selectivity) {
		// One default for the filter, whatever its predicates, unless the extensions estimate them.
		auto selectivity = EstimateFilterWithExtensions(context, filter, RelationStatisticsHelper::DEFAULT_SELECTIVITY);
		filter.estimated_selectivity =
		    selectivity
		        ? *selectivity
		        : EstimatedSelectivity {RelationStatisticsHelper::DEFAULT_SELECTIVITY, SelectivitySource::DEFAULT};
	}
	return EstimateFilterRows(child_cardinality, filter.estimated_selectivity->selectivity);
}

idx_t EstimateFilterRows(idx_t child_cardinality, double selectivity) {
	if (child_cardinality == 0) {
		return 0;
	}
	auto rows = static_cast<double>(child_cardinality) * selectivity;
	return MaxValue<idx_t>(LossyNumericCast<idx_t>(rows), 1);
}

void EstimateResidualFilter(Expression &filter) {
	if (!filter.estimated_selectivity) {
		filter.estimated_selectivity =
		    EstimatedSelectivity {RelationStatisticsHelper::DEFAULT_SELECTIVITY, SelectivitySource::DEFAULT};
	}
}

//! Without an estimation extension, an optional filter alone does not trigger the estimate.
static void EstimateFiltersAddedLate(ClientContext &context, LogicalOperator &op, bool has_estimation_extensions) {
	for (auto &child : op.children) {
		EstimateFiltersAddedLate(context, *child, has_estimation_extensions);
	}
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		auto has_late_filter = !get.estimated_selectivity;
		for (auto &entry : get.table_filters) {
			auto &filter = entry.Filter();
			auto &predicate = *ExpressionFilter::GetExpressionFilter(filter, "EstimateFiltersAddedLate").expr;
			auto needs_estimate = has_estimation_extensions || !ExpressionFilter::IsOptionalFilter(filter);
			has_late_filter = has_late_filter || (needs_estimate && !predicate.estimated_selectivity);
		}
		if (!get.table_filters.HasFilters() || !has_late_filter) {
			return;
		}
		EstimateScanFilters(get, context);
	} else if (op.type == LogicalOperatorType::LOGICAL_FILTER && !op.estimated_selectivity) {
		op.estimated_selectivity =
		    EstimatedSelectivity {RelationStatisticsHelper::DEFAULT_SELECTIVITY, SelectivitySource::DEFAULT};
		auto selectivity = has_estimation_extensions
		                       ? EstimateFilterWithExtensions(context, op.Cast<LogicalFilter>(),
		                                                      RelationStatisticsHelper::DEFAULT_SELECTIVITY)
		                       : nullopt;
		if (selectivity) {
			op.estimated_selectivity = std::move(selectivity);
		}
	}
}

void EstimateFiltersAddedLate(ClientContext &context, LogicalOperator &op) {
	EstimateFiltersAddedLate(context, op, HasEstimationExtensions(context));
}

//! Adds `op` and the operators planned with it to `planned`, stopping at those in `planned_operators`.
static void CollectPlannedOperators(PhysicalOperator &op, reference_set_t<const PhysicalOperator> &planned_operators,
                                    vector<reference<PhysicalOperator>> &planned) {
	if (!planned_operators.insert(op).second) {
		return;
	}
	planned.push_back(op);
	for (auto &child : op.children) {
		CollectPlannedOperators(child.get(), planned_operators, planned);
	}
}

void GiveSelectivityToPlannedOperators(PhysicalOperator &op, const optional<EstimatedSelectivity> &selectivity,
                                       reference_set_t<const PhysicalOperator> &planned_operators) {
	vector<reference<PhysicalOperator>> planned;
	CollectPlannedOperators(op, planned_operators, planned);
	if (planned.empty()) {
		return;
	}
	for (auto &entry : planned) {
		auto type = entry.get().type;
		if (type == PhysicalOperatorType::FILTER || type == PhysicalOperatorType::TABLE_SCAN) {
			entry.get().estimated_selectivity = selectivity;
			return;
		}
	}
	op.estimated_selectivity = selectivity;
}

//! Adds `expression` and every expression under it to `result`.
template <class EXPRESSION>
static void AddWithChildren(EXPRESSION &expression, vector<reference<EXPRESSION>> &result) {
	result.push_back(expression);
	ExpressionIterator::EnumerateChildren(expression, [&](EXPRESSION &child) { AddWithChildren(child, result); });
}

//! Every expression of `op`, its scan filters' included, in an order its copy shares.
template <class OPERATOR, class EXPRESSION>
static void CollectOperatorExpressions(OPERATOR &op, vector<reference<EXPRESSION>> &result) {
	LogicalOperatorVisitor::EnumerateExpressions(op,
	                                             [&](auto *child) { AddWithChildren<EXPRESSION>(**child, result); });
	if (op.type != LogicalOperatorType::LOGICAL_GET) {
		return;
	}
	auto &filters = op.template Cast<LogicalGet>().table_filters;
	auto add = [&](const TableFilter &filter) {
		if (filter.filter_type == TableFilterType::EXPRESSION_FILTER) {
			AddWithChildren<EXPRESSION>(*filter.Cast<ExpressionFilter>().expr, result);
		}
	};
	for (auto &entry : filters) {
		add(entry.Filter());
	}
	for (auto &filter : filters.GetMultiColumnFilters()) {
		add(*filter);
	}
}

void CopyPredicateSelectivities(const LogicalOperator &original, LogicalOperator &copy) {
	if (original.type != copy.type || original.children.size() != copy.children.size()) {
		return;
	}
	vector<reference<const Expression>> from;
	vector<reference<Expression>> to;
	CollectOperatorExpressions(original, from);
	CollectOperatorExpressions(copy, to);
	if (from.size() == to.size()) {
		for (idx_t i = 0; i < from.size(); i++) {
			to[i].get().estimated_selectivity = from[i].get().estimated_selectivity;
		}
	}
	for (idx_t i = 0; i < original.children.size(); i++) {
		CopyPredicateSelectivities(*original.children[i], *copy.children[i]);
	}
}

} // namespace floe_estimation
} // namespace duckdb
