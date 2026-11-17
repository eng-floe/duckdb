//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/scan_hint.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class BoundColumnRefExpression;

//! __internal_scan_hint(condition) is true, and marks condition as a hint the scan may skip data by. The other
//! conjuncts of the AND it is in must imply it: they keep no row it rules out. Filter pushdown gives a hint that is one
//! of a filter's conjuncts to the scan as an optional filter, and drops the call where no scan takes it. Elsewhere, as
//! in a join condition, the call stays and is true, but its condition is still evaluated, so it must not throw.
struct ScanHint {
	//! Whether this expression node is a hint
	static bool Is(const Expression &expr);
	//! The condition a hint marks
	static const Expression &Condition(const Expression &hint);
	//! The one column a scan can take the hint's condition on: none when it reads no column or several, or is
	//! volatile or can throw
	static optional_ptr<const BoundColumnRefExpression> PushableColumn(const Expression &hint);
	//! Mark the given condition as a hint
	static unique_ptr<Expression> Wrap(unique_ptr<Expression> condition);
};

} // namespace duckdb
