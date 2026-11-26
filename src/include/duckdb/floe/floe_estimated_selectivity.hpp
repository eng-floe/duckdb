//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/floe/floe_estimated_selectivity.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

//! Where an estimated selectivity comes from.
enum class SelectivitySource : uint8_t {
	//! The column statistics
	STATISTICS,
	//! The fixed selectivity DuckDB assumes without statistics
	DEFAULT,
};

//! The share of its input rows a predicate or an operator is estimated to keep, and where that estimate comes from.
struct EstimatedSelectivity {
	//! From 0 to 1
	double selectivity;
	SelectivitySource source;
};

} // namespace duckdb
