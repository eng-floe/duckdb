//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/floe_prefix_range_export.hpp
//
// Floe: how a scan outside DuckDB reads a PrefixRangeFilter. Not part of upstream DuckDB.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//! The build keys' bounds of a PrefixRangeFilter, in the key's own type (the whole string for VARCHAR).
//! Whoever initializes a filter sets them to the bounds it passes to Initialize; NULL until then.
struct FloePrefixRangeBounds {
	Value min;
	Value max;
};

//! A PrefixRangeFilter as a scan outside DuckDB reads it: its bounds and its bitmap. The words are the filter's own,
//! not a copy, and valid while the filter lives. The words and dense are final only after the last MergeBuildState,
//! which has already happened for a filter obtained through a pushed PrefixRangeFunctionData.
struct FloePrefixRangeExport {
	//! The filter's FloePrefixRangeBounds
	Value min;
	Value max;
	//! word_count words; bit i of the bitmap is bit (i & 63) of words[i >> 6]. Bits past (span >> shift) are zero.
	const uint64_t *words = nullptr;
	idx_t word_count = 0;
	//! The largest key less the smallest, in the key's unsigned domain (a string filter covers the big-endian
	//! integer of each key's first 4 bytes)
	uint64_t span = 0;
	//! One bit covers 2^shift keys: key k maps to bit ((k - min) >> shift), the subtraction being unsigned and
	//! wrapping at the key's width (min = INT64_MIN with span = UINT64_MAX is a valid export)
	idx_t shift = 0;
	//! Every bit up to and including (span >> shift) is set
	bool dense = false;
};

} // namespace duckdb
