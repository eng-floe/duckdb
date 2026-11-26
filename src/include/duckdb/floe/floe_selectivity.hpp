//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/floe/floe_selectivity.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/floe/floe_estimated_selectivity.hpp"

namespace duckdb {

class ClientContext;
class Expression;
class LogicalFilter;
class LogicalGet;
class LogicalOperator;
class PhysicalOperator;

//! Floe's additions to DuckDB's estimates.
namespace floe_estimation {

//! The rows `get`'s filters keep of `rows`: those the filter keeping the fewest keeps, or DuckDB's default when none
//! cuts a row. `selectivity` gets the share of rows they keep together.
idx_t EstimateFilteredRows(ClientContext &context, LogicalGet &get, idx_t rows,
                           optional<EstimatedSelectivity> &selectivity);
//! Estimates the selectivity of each filter of `get` that has none, from its column's statistics, and of `get`: what
//! its filters keep together.
void EstimateScanFilters(LogicalGet &get, ClientContext &context);
//! The rows `filter` keeps of its child's `child_cardinality` by its estimated selectivity, DuckDB's default when it
//! has none.
idx_t EstimateFilterCardinality(LogicalFilter &filter, idx_t child_cardinality);
//! The rows a filter keeping `selectivity` of its child's `child_cardinality` keeps: at least one when the child has
//! any.
idx_t EstimateFilterRows(idx_t child_cardinality, double selectivity);
//! Gives DuckDB's default to `filter`, a predicate join ordering applies across relations, when it has no selectivity.
void EstimateResidualFilter(Expression &filter);
//! Estimates the selectivity of the filters added after join ordering, and of the scans and filters holding them.
//! Their rows stay: DuckDB planned them without these filters.
void EstimateFiltersAddedLate(ClientContext &context, LogicalOperator &op);
//! Gives `selectivity` to the topmost scan or filter planned for an operator, `op` the root of its plan; to `op` when
//! there is none. `planned_operators` holds the physical operators already planned. A scan DuckDB splits into a filter
//! over a scan has it once, on the filter; an operator planned as its child's operator gives none.
void GiveSelectivityToPlannedOperators(PhysicalOperator &op, const optional<EstimatedSelectivity> &selectivity,
                                       reference_set_t<const PhysicalOperator> &planned_operators);
//! Gives the predicates of `copy` the estimated selectivities of the original's, which serialization drops. Its
//! operators get none: a copy may read other rows, as partitioned execution's slices do.
void CopyPredicateSelectivities(const LogicalOperator &original, LogicalOperator &copy);

} // namespace floe_estimation
} // namespace duckdb
