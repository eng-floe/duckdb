#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/binder.hpp"

using namespace duckdb;

//! The columns extracting names finds `query` reads of each table it names, as `table: columns`, "*" for every
//! column.
static string ExtractedColumnsOf(Connection &con, const string &query) {
	map<string, set<string>> result;
	Parser parser(*con.context);
	parser.ParseQuery(query);
	con.context->RunFunctionInTransaction([&]() {
		auto binder = Binder::CreateBinder(*con.context);
		binder->SetBindingMode(BindingMode::EXTRACT_NAMES);
		binder->Bind(*parser.statements[0]);
		for (auto &table : binder->GetExtractedColumns()) {
			auto &columns = result[table.table_name];
			columns.insert(table.columns.begin(), table.columns.end());
			if (table.all) {
				columns.insert("*");
			}
		}
	});
	string text;
	for (auto &table : result) {
		text += (text.empty() ? "" : "; ") + table.first + ":";
		// a table read whole reads no column by name
		if (table.second.count("*")) {
			text += " *";
			continue;
		}
		for (auto &column : table.second) {
			text += " " + column;
		}
	}
	return text;
}

TEST_CASE("Extracting names finds the columns a query may read of each table", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE a(id INTEGER, x VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE b(id INTEGER, y VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE s(st STRUCT(f INTEGER))"));

	const vector<pair<string, string>> queries {
	    // an unqualified name, of every table in scope; a qualified one, of its table, its first name of every table
	    {"SELECT x FROM a, b", "a: x; b: x"},
	    {"SELECT a.x FROM a, b", "a: a x; b: a"},
	    {"SELECT st.f FROM s", "s: st"},
	    {"SELECT id, st.* FROM s", "s: id st"},
	    // an enclosing query's tables are in scope; a table's alias may also name another table's struct column
	    {"SELECT (SELECT y FROM b WHERE b.id = a.id) FROM a", "a: a b id y; b: a b id y"},
	    {"SELECT (SELECT st.f FROM b AS st) FROM s", "b: f st; s: st"},
	    {"SELECT (SELECT 1 FROM b HAVING a.x > '') FROM a", "a: a x; b: a"},
	    // the columns of a join condition, a group, HAVING and ORDER BY; a USING column, of both sides
	    {"SELECT 1 FROM a JOIN b ON a.id = b.id GROUP BY a.x HAVING min(b.y) > '' ORDER BY a.x",
		 "a: a b id x; b: a b id y"},
	    {"SELECT 1 FROM a JOIN b USING (id)", "a: id; b: id"},
	    // every column: a star, a whole row, a position, a NATURAL join, a column alias list, a pivot
	    {"SELECT * FROM a", "a: *"},
	    {"SELECT a.* FROM a, b", "a: *; b: a"},
	    {"SELECT a.* REPLACE (b.y AS x) FROM a JOIN b ON a.id = b.id", "a: *; b: a b id y"},
	    {"SELECT * REPLACE ((SELECT max(y) FROM b) AS x) FROM a", "a: *; b: y"},
	    {"SELECT a FROM a", "a: *"},
	    {"SELECT #2 FROM a, b", "a: *; b: *"},
	    {"SELECT 1 FROM a NATURAL JOIN b", "a: *; b: *"},
	    {"SELECT p FROM a AS u(p)", "a: *"},
	    {"SELECT * FROM a PIVOT (count(id) FOR x IN ('k'))", "a: *"},
	    {"SELECT n FROM (UNPIVOT a ON x INTO NAME n VALUE w)", "a: *"},
	    // a set operation's ORDER BY reads its outputs alone
	    {"SELECT x FROM a UNION SELECT y FROM b ORDER BY 1", "a: x; b: y"},
	    // a name that is a column or an alias of the select list, decided before binding
	    {"SELECT 1 AS x, count(*) FROM a GROUP BY x", "a: x"},
	    {"WITH RECURSIVE t AS (SELECT (SELECT min(x) FROM a) AS x UNION SELECT x + 1 FROM t WHERE x < 3) SELECT x FROM "
		 "t",
		 "a: x"},
	};
	for (auto &query : queries) {
		INFO(query.first);
		CHECK(ExtractedColumnsOf(con, query.first) == query.second);
	}
}
