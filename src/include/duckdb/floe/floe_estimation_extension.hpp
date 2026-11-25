//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/floe/floe_estimation_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/floe/floe_estimated_selectivity.hpp"

namespace duckdb {

class CardinalityEstimator;
class Expression;
class LogicalFilter;
class LogicalOperator;
class QueryGraphManager;
struct JoinOrderOperator;
struct JoinRelationSet;
struct RelationStats;
enum class JoinOrderOperatorType : uint8_t;

//! A relation entering join ordering.
struct RelationEstimate {
	//! Its rows, after its filters
	idx_t cardinality;
	//! The distinct values of each of its columns in the whole table it comes from, in the order of its operator's
	//! GetColumnBindings(): DuckDB's guess when it has no statistics, invalid when it has no column there. DuckDB
	//! takes a count an extension changes as measured, as a HyperLogLog sketch's.
	vector<optional_idx> column_distinct_counts;
};

//! The type of a join being estimated.
enum class EstimatedJoinType : uint8_t { INNER, LEFT, SEMI, ANTI };

//! A join being estimated.
struct JoinEstimateInput {
	EstimatedJoinType type;
	//! The plans joined.
	vector<reference<LogicalOperator>> relations;
	//! The estimated rows of each of `relations`, after its filters.
	vector<double> relation_cardinalities;
	//! The plans on each side; both empty when an inner join of `relations` is estimated as a whole.
	vector<reference<LogicalOperator>> left;
	vector<reference<LogicalOperator>> right;
	//! The estimated rows of each side, as joined; 0 without sides.
	double left_cardinality;
	double right_cardinality;
	//! The predicates evaluated at this join: its own conditions, and the filters across its sides, or among
	//! `relations` when it has none. Valid during the call only.
	vector<reference<const Expression>> predicates;
};

//! Replaces DuckDB's estimates with its own. Each method gets DuckDB's estimate, or an earlier extension's, and may
//! change it in place; leaving it keeps it. A method may be asked again about the same operator, getting its own
//! earlier answer: an answer relative to the one it gets is applied again. Estimates choose a plan, never its result:
//! an extension reads the operators it gets, and never changes them.
//! A method that throws is logged and its change dropped, unless the error is an interrupt or fatal; an invalid answer
//! is dropped too. Registered as an optimizer extension's info, after those already registered.
class EstimationExtension : public OptimizerExtensionInfo {
public:
	//! The selectivity of `predicate`, filtering `op`, a scan or a filter: none when DuckDB makes no estimate. One the
	//! extension changes becomes the predicate's, with its extension info, and is not asked again. An answer of none,
	//! or not from 0 to 1, is invalid.
	virtual void EstimatePredicate(ClientContext &context, LogicalOperator &op, const Expression &predicate,
	                               optional<EstimatedSelectivity> &selectivity) {
	}
	//! A relation entering join ordering, `op` its root. A distinct count changed to 0 is invalid; one above the
	//! table's rows counts as its rows.
	virtual void EstimateRelation(ClientContext &context, LogicalOperator &op, RelationEstimate &estimate) {
	}
	//! The rows of a join of two or more relations. An answer that is not a finite non-negative number is invalid.
	virtual void EstimateJoin(ClientContext &context, const JoinEstimateInput &input, double &cardinality) {
	}
	//! The rows of `op`, after its children's. DuckDB's estimate of them is from its own estimates of the children,
	//! which an extension may have changed since. An operator's selectivity is what its predicates keep, whatever rows
	//! an extension gives it.
	virtual void EstimateOperator(ClientContext &context, LogicalOperator &op, idx_t &cardinality) {
	}

public:
	//! Registers `extension` on `config`, after those already registered.
	static void Register(DBConfig &config, shared_ptr<EstimationExtension> extension);
};

namespace floe_estimation {

//! How the cardinality estimator joins a set: an inner join of its relations, with no sides; an inner join of two
//! sides; or the LEFT, SEMI or ANTI join `op` of its sides.
struct JoinEstimateSides {
	JoinOrderOperatorType type;
	optional_ptr<JoinRelationSet> left;
	optional_ptr<JoinRelationSet> right;
	//! The non-inner join; null for an inner join.
	optional_ptr<const JoinOrderOperator> op;
};

//! Whether an estimation extension is registered.
bool HasEstimationExtensions(ClientContext &context);
//! Replaces `selectivity`, DuckDB's estimate of `predicate` filtering `op` or none, with the estimation extensions'
//! valid estimates.
void ReplacePredicateSelectivityWithExtensions(ClientContext &context, LogicalOperator &op, const Expression &predicate,
                                               optional<EstimatedSelectivity> &selectivity);
//! The selectivity of `filter` from the extensions' estimates of its predicates, those they do not estimate keeping
//! `unestimated_selectivity` together, with DuckDB's default as source then; none when they estimate none. Asks them to
//! estimate each predicate without a selectivity.
optional<EstimatedSelectivity> EstimateFilterWithExtensions(ClientContext &context, LogicalFilter &filter,
                                                            double unestimated_selectivity);
//! Replaces `stats`, DuckDB's estimate of a relation entering join ordering, `op` its root, with the estimation
//! extensions' estimate.
void ReplaceRelationEstimateWithExtensions(ClientContext &context, LogicalOperator &op, RelationStats &stats);
//! Replaces `cardinality`, DuckDB's estimate of the join of `set` as `sides` describes it, with the estimation
//! extensions' valid estimates.
void ReplaceJoinEstimateWithExtensions(ClientContext &context, QueryGraphManager &query_graph,
                                       CardinalityEstimator &estimator, JoinRelationSet &set,
                                       const JoinEstimateSides &sides, double &cardinality);
//! Replaces the rows of every operator of `plan`, children first, with the estimation extensions' estimates.
void ReplaceOperatorCardinalitiesWithExtensions(ClientContext &context, LogicalOperator &plan);

} // namespace floe_estimation
} // namespace duckdb
