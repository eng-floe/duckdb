#include "duckdb/planner/floe/extracted_columns.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

void Binder::AddExtractedTable(TableIndex index, const BaseTableRef &ref) {
	auto &extracted = global_binder_state->extracted_columns[index];
	extracted.location = ref.query_location;
	extracted.table_name = ref.Table().GetIdentifierName();
	auto &path = ref.GetQualifiedName().Path();
	for (idx_t i = 0; i + 1 < path.size(); i++) {
		extracted.qualifiers.push_back(path[i].GetIdentifierName());
	}
	// a column alias list renames the table's columns by position
	extracted.all = !ref.column_name_alias.empty();
}

void Binder::ExtractColumn(const ColumnRefExpression &column) {
	auto &names = column.ColumnNames();
	// the tables in this scope and the enclosing ones
	vector<reference<BindContext>> scopes {bind_context};
	auto &enclosing = GetEnclosingScopes();
	for (idx_t i = enclosing.size(); i > 0; i--) {
		scopes.push_back(enclosing[i - 1].get().GetBinder().bind_context);
	}
	for (auto &scope : scopes) {
		for (auto &binding : scope.get().GetBindingsList()) {
			auto extracted = global_binder_state->extracted_columns.find(binding->GetIndex());
			if (extracted == global_binder_state->extracted_columns.end()) {
				continue;
			}
			auto &table = extracted->second;
			// the dummy column stands for every column
			auto &placeholders = binding->GetColumnNames();
			auto read = [&](const Identifier &name) {
				if (std::find(placeholders.begin(), placeholders.end(), name) != placeholders.end()) {
					table.all = true;
				} else {
					table.columns.insert(name.GetIdentifierName());
				}
			};
			// the first name may be a struct column of any table
			read(names[0]);
			// after a table's alias, its column or its whole row
			for (idx_t i = 0; i < names.size(); i++) {
				if (names[i] != binding->GetAlias()) {
					continue;
				}
				if (i + 1 < names.size()) {
					read(names[i + 1]);
				} else {
					table.all = true;
				}
			}
		}
	}
}

void Binder::ExtractAllColumns(BindContext &context) {
	for (auto &binding : context.GetBindingsList()) {
		auto extracted = global_binder_state->extracted_columns.find(binding->GetIndex());
		if (extracted != global_binder_state->extracted_columns.end()) {
			extracted->second.all = true;
		}
	}
}

void KeepReplacements(StarExpression &expr, vector<unique_ptr<ParsedExpression>> &select_list, idx_t first) {
	if (expr.ReplaceList().empty() || first >= select_list.size()) {
		return;
	}
	auto name = select_list[first]->GetName();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(select_list[first]));
	for (auto &entry : expr.ReplaceListMutable()) {
		children.push_back(std::move(entry.second));
	}
	expr.ReplaceListMutable().clear();
	select_list[first] = make_uniq<FunctionExpression>(Identifier("row"), std::move(children));
	select_list[first]->SetAlias(name);
}

vector<ExtractedColumns> Binder::GetExtractedColumns() {
	vector<ExtractedColumns> result;
	for (auto &entry : global_binder_state->extracted_columns) {
		result.push_back(entry.second);
	}
	return result;
}

//! Whether `reference` may name `table`: by its name, and each qualifier naming its schema or catalog
static bool MayName(const ExtractedColumns &reference, TableCatalogEntry &table) {
	if (!StringUtil::CIEquals(reference.table_name, table.name.GetIdentifierName())) {
		return false;
	}
	for (auto &qualifier : reference.qualifiers) {
		if (!StringUtil::CIEquals(qualifier, table.schema.name.GetIdentifierName()) &&
		    !StringUtil::CIEquals(qualifier, table.ParentCatalog().GetName().GetIdentifierName())) {
			return false;
		}
	}
	return true;
}

void VerifyExtractedColumns(const vector<ExtractedColumns> &extracted, LogicalOperator &plan) {
	for (auto &child : plan.children) {
		VerifyExtractedColumns(extracted, *child);
	}
	if (plan.type != LogicalOperatorType::LOGICAL_GET) {
		return;
	}
	auto &get = plan.Cast<LogicalGet>();
	auto table = get.GetTable();
	// a generated column reads columns the statement need not name
	if (!table || table->HasGeneratedColumns()) {
		return;
	}
	// the columns read by the references that may name the table; none skips the check: a table names-only binding
	// does not name is left whole for binding to resolve
	set<string> read;
	bool named = false;
	for (auto &reference : extracted) {
		if (!MayName(reference, *table)) {
			continue;
		}
		if (reference.all) {
			return;
		}
		named = true;
		for (auto &column : reference.columns) {
			read.insert(StringUtil::Lower(column));
		}
	}
	if (!named) {
		return;
	}
	for (auto &column : get.GetColumnIds()) {
		if (column.IsVirtualColumn()) {
			continue;
		}
		auto &name = get.names[column.GetPrimaryIndex()].GetIdentifierName();
		if (read.find(StringUtil::Lower(name)) == read.end()) {
			throw InternalException("Names-only binding missed column \"%s\" of table \"%s\"", name,
			                        table->name.GetIdentifierName());
		}
	}
}

//! Messages of names-only binding failures with a known cause
static const char *const KNOWN_NAMES_ONLY_FAILURES[] = {
    // a function call binds to NULL, which an argument wanting a constant refuses
    "cannot take NULL list as parameter",
    "requires a struct as input",
    "requires a struct or list as input",
    "requires a list as input",
    "expects a value of type MAP, not NULL",
    "expects a non-NULL STRUCT",
    "Cannot use NULL as",
    "cannot be NULL",
    "must not be NULL",
    "is not supported as a valid option for COPY option",
    "cannot execute NULL plan",
    "syntax error at or near \"NULL\"",
    "\"NULL\" was specified multiple times in the IN clause",
    "FILL argument must support subtraction",
    "collations are only supported for type varchar",
    "requires a non-NULL file name",
    "requires a string or a list as input",
    "Invalid value for 'force_not_null' parameter",
    "(\"NULL\", INTERVAL)",
    "Can't have NULL parameters",
    // a parameter's type is not resolved
    "Parameter types could not be resolved",
    // positions, USING and NATURAL columns and groups need the real columns
    "Positional reference",
    "Ambiguous reference to column name",
    "does not have a column named \"rowid\"",
    "must appear in the GROUP BY clause",
    "must be a grouping column",
    "WHERE clause cannot contain aggregates",
    "GROUP BY clause cannot contain aggregates",
    "GROUPING statement cannot be used without groups",
    // a column a table does not name binds in an outer query, which makes a subquery LATERAL
    "LATERAL join cannot contain aggregates",
    // a COPY returning its column types needs the real types
    "Failed to cast expression to type - expression type mismatch",
    // a pivot's aggregates bind to NULL
    "PIVOT: identical aggregates must have the same name",
    "Pivot - expected an aggregate",
    // a DELETE, UPDATE or MERGE target binds as no base table
    "Can only delete from base table",
    "Can only update base table",
    "Can only merge into base tables",
};

bool IsKnownNamesOnlyFailure(const ErrorData &error) {
	for (auto message : KNOWN_NAMES_ONLY_FAILURES) {
		if (StringUtil::Contains(error.RawMessage(), message)) {
			return true;
		}
	}
	return false;
}

} // namespace duckdb
