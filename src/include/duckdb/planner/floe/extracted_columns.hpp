//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/floe/extracted_columns.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/query_location.hpp"
#include "duckdb/common/set.hpp"

namespace duckdb {

//! The columns a statement reads of a table it names, as BindingMode::EXTRACT_NAMES finds them
struct ExtractedColumns {
	//! Where the table is named, and its name and qualifiers as written
	QueryLocation location;
	string table_name;
	vector<string> qualifiers;
	//! Whether it reads every column, as a star or a NATURAL join does
	bool all = false;
	//! The names it may read the table's columns by, as written; some may match no column
	set<string> columns;
};

class ErrorData;
class LogicalOperator;
class ParsedExpression;
class StarExpression;

//! Throws an InternalException for a column `plan` reads that no reference naming its table reads
void VerifyExtractedColumns(const vector<ExtractedColumns> &extracted, LogicalOperator &plan);
//! While extracting names no column is known, so none is replaced: binds the replacements of `expr` within its first
//! expansion, at `first` of `select_list`
void KeepReplacements(StarExpression &expr, vector<unique_ptr<ParsedExpression>> &select_list, idx_t first);
//! Whether names-only binding failed for one of its known limitations
bool IsKnownNamesOnlyFailure(const ErrorData &error);

} // namespace duckdb
