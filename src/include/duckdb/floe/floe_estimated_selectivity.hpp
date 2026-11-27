//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/floe/floe_estimated_selectivity.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {

//! Where an estimated selectivity comes from.
enum class SelectivitySource : uint8_t {
	//! The column statistics
	STATISTICS,
	//! The fixed selectivity DuckDB assumes without statistics
	DEFAULT,
	//! An estimation extension
	EXTENSION,
};

//! An extension's own data about a selectivity it estimated; DuckDB keeps it with the selectivity.
class ExtensionEstimateInfo {
public:
	virtual ~ExtensionEstimateInfo() = default;
	//! The name of the extension's method, as EXPLAIN shows it: "mcv", "tdigest".
	virtual string GetMethodName() const = 0;
};

//! The share of its input rows a predicate or an operator is estimated to keep, and where that estimate comes from.
struct EstimatedSelectivity {
	//! From 0 to 1
	double selectivity;
	SelectivitySource source;
	//! None for DuckDB's own estimates
	shared_ptr<const ExtensionEstimateInfo> extension_info = nullptr;
};

} // namespace duckdb
