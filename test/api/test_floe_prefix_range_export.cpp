#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "test_helpers.hpp"

using namespace duckdb; // NOLINT

//! The bit budget the hash join passes when the key span fits a bitmap exactly
static constexpr idx_t EXACT_MAX_BITS = 1ULL << 26;

//! Fills a build state with keys through the serial insert when parallel is false, the atomic insert otherwise
static unique_ptr<PrefixRangeFilter::BuildState> InsertBuildKeys(ClientContext &context, PrefixRangeFilter &filter,
                                                                 const LogicalType &type, const vector<Value> &keys,
                                                                 bool parallel) {
	Vector key_vector(type, MaxValue<idx_t>(keys.size(), 1));
	for (auto &key : keys) {
		key_vector.Append(key);
	}
	auto state = filter.InitializeBuildState(context);
	if (parallel) {
		filter.InsertKeysParallel(key_vector, *state);
	} else {
		filter.InsertKeys(key_vector, *state);
	}
	return state;
}

//! Builds a filter the way the hash join does: bounds first, then one build state per lane, merged in turn
static unique_ptr<PrefixRangeFilter> BuildFilter(ClientContext &context, const LogicalType &type, const Value &min,
                                                 const Value &max, idx_t max_bits, const vector<Value> &serial_keys,
                                                 const vector<Value> &parallel_keys = vector<Value>()) {
	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(type);
	filter->floe_bounds = {min, max};
	filter->Initialize(context, serial_keys.size() + parallel_keys.size(), min, max, max_bits);
	auto serial_state = InsertBuildKeys(context, *filter, type, serial_keys, false);
	auto parallel_state = InsertBuildKeys(context, *filter, type, parallel_keys, true);
	filter->MergeBuildState(*serial_state);
	filter->MergeBuildState(*parallel_state);
	return filter;
}

//! Every step-th INTEGER key from first to last, without skipped
static vector<Value> IntegerKeys(int32_t first, int32_t last, int32_t step, int32_t skipped) {
	vector<Value> keys;
	for (int32_t key = first; key <= last; key += step) {
		if (key != skipped) {
			keys.push_back(Value::INTEGER(key));
		}
	}
	return keys;
}

TEST_CASE("Prefix range filter export of negative INTEGER keys", "[api][prefix_range]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto filter = BuildFilter(*con.context, LogicalType::INTEGER, Value::INTEGER(-5), Value::INTEGER(10),
	                          EXACT_MAX_BITS,
	                          {Value::INTEGER(-5), Value::INTEGER(-3), Value::INTEGER(0), Value::INTEGER(10)});
	REQUIRE(filter->IsInitialized());
	FloePrefixRangeExport view;
	REQUIRE(filter->FloeTryExport(view));

	REQUIRE(view.span == 15);
	REQUIRE(view.shift == 0);
	REQUIRE(view.word_count == 1);
	// keys -5, -3, 0 and 10 sit 0, 2, 5 and 15 above the minimum
	REQUIRE(view.words[0] == 0x8025ULL);
	REQUIRE_FALSE(view.dense);
	REQUIRE(view.min == Value::INTEGER(-5));
	REQUIRE(view.max == Value::INTEGER(10));
	REQUIRE(view.min.type() == LogicalType::INTEGER);
	REQUIRE(view.max.type() == LogicalType::INTEGER);
}

TEST_CASE("Prefix range filter export of BIGINT keys at the type limits", "[api][prefix_range]") {
	const auto minimum = NumericLimits<int64_t>::Minimum();
	const auto maximum = NumericLimits<int64_t>::Maximum();

	DuckDB db(nullptr);
	Connection con(db);
	auto filter = BuildFilter(*con.context, LogicalType::BIGINT, Value::BIGINT(minimum), Value::BIGINT(maximum), 64,
	                          {Value::BIGINT(minimum), Value::BIGINT(0), Value::BIGINT(maximum)});
	REQUIRE(filter->IsInitialized());
	FloePrefixRangeExport view;
	REQUIRE(filter->FloeTryExport(view));

	REQUIRE(view.span == NumericLimits<uint64_t>::Maximum());
	// the whole unsigned domain folds into 64 buckets of 2^58 keys
	REQUIRE(view.shift == 58);
	REQUIRE(view.word_count == 1);
	// the minimum, zero and the maximum land in buckets 0, 32 and 63
	REQUIRE(view.words[0] == ((1ULL << 63) | (1ULL << 32) | 1ULL));
	REQUIRE_FALSE(view.dense);
	REQUIRE(view.min == Value::BIGINT(minimum));
	REQUIRE(view.max == Value::BIGINT(maximum));
	REQUIRE(view.min.type() == LogicalType::BIGINT);
	REQUIRE(view.max.type() == LogicalType::BIGINT);
}

TEST_CASE("Prefix range filter export of a lossy span", "[api][prefix_range]") {
	// a span equal to the bit budget is the one lossy shape the hash join builds
	DuckDB db(nullptr);
	Connection con(db);
	auto filter = BuildFilter(
	    *con.context, LogicalType::INTEGER, Value::INTEGER(0), Value::INTEGER(512), 512,
	    {Value::INTEGER(0), Value::INTEGER(1), Value::INTEGER(2), Value::INTEGER(511), Value::INTEGER(512)});
	REQUIRE(filter->IsInitialized());
	FloePrefixRangeExport view;
	REQUIRE(filter->FloeTryExport(view));

	REQUIRE(view.span == 512);
	REQUIRE(view.shift == 1);
	// 257 buckets
	REQUIRE(view.word_count == 5);
	// keys 0 and 1 share bucket 0, key 2 is bucket 1
	REQUIRE(view.words[0] == 3ULL);
	REQUIRE(view.words[1] == 0ULL);
	REQUIRE(view.words[2] == 0ULL);
	// key 511 is bucket 255
	REQUIRE(view.words[3] == (1ULL << 63));
	// key 512 is bucket 256, the last one; every bit past it is zero
	REQUIRE(view.words[4] == 1ULL);
	REQUIRE_FALSE(view.dense);
	REQUIRE(view.min == Value::INTEGER(0));
	REQUIRE(view.max == Value::INTEGER(512));
	REQUIRE(view.min.type() == LogicalType::INTEGER);
	REQUIRE(view.max.type() == LogicalType::INTEGER);
}

TEST_CASE("Prefix range filter export of a lossy span with an odd negative minimum", "[api][prefix_range]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto filter = BuildFilter(
	    *con.context, LogicalType::INTEGER, Value::INTEGER(-5), Value::INTEGER(507), 512,
	    {Value::INTEGER(-5), Value::INTEGER(-4), Value::INTEGER(0), Value::INTEGER(506), Value::INTEGER(507)});
	REQUIRE(filter->IsInitialized());
	FloePrefixRangeExport view;
	REQUIRE(filter->FloeTryExport(view));

	REQUIRE(view.span == 512);
	REQUIRE(view.shift == 1);
	REQUIRE(view.word_count == 5);
	// the minimum is subtracted before the shift: keys -5 and -4 sit 0 and 1 above it and share bucket 0, key 0
	// sits 5 above it in bucket 2. Shifting the key and the minimum separately would give buckets 0, 1 and 3.
	REQUIRE(view.words[0] == 5ULL);
	REQUIRE(view.words[1] == 0ULL);
	REQUIRE(view.words[2] == 0ULL);
	// key 506 sits 511 above the minimum, in bucket 255
	REQUIRE(view.words[3] == (1ULL << 63));
	// key 507 is bucket 256, the last one
	REQUIRE(view.words[4] == 1ULL);
	REQUIRE_FALSE(view.dense);
	REQUIRE(view.min == Value::INTEGER(-5));
	REQUIRE(view.max == Value::INTEGER(507));
	REQUIRE(view.min.type() == LogicalType::INTEGER);
	REQUIRE(view.max.type() == LogicalType::INTEGER);
}

TEST_CASE("Prefix range filter export of DATE keys", "[api][prefix_range]") {
	// days since 1970-01-01; the negative ones are dates before it
	DuckDB db(nullptr);
	Connection con(db);
	auto filter = BuildFilter(
	    *con.context, LogicalType::DATE, Value::DATE(date_t(-3)), Value::DATE(date_t(10)), EXACT_MAX_BITS,
	    {Value::DATE(date_t(-3)), Value::DATE(date_t(-1)), Value::DATE(date_t(0)), Value::DATE(date_t(10))});
	REQUIRE(filter->IsInitialized());
	FloePrefixRangeExport view;
	REQUIRE(filter->FloeTryExport(view));

	REQUIRE(view.span == 13);
	REQUIRE(view.shift == 0);
	REQUIRE(view.word_count == 1);
	// days -3, -1, 0 and 10 sit 0, 2, 3 and 13 above the minimum
	REQUIRE(view.words[0] == 0x200DULL);
	REQUIRE_FALSE(view.dense);
	REQUIRE(view.min == Value::DATE(date_t(-3)));
	REQUIRE(view.max == Value::DATE(date_t(10)));
	REQUIRE(view.min.type() == LogicalType::DATE);
	REQUIRE(view.max.type() == LogicalType::DATE);
}

TEST_CASE("Prefix range filter export of negative BIGINT keys", "[api][prefix_range]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto filter = BuildFilter(
	    *con.context, LogicalType::BIGINT, Value::BIGINT(-70), Value::BIGINT(5), EXACT_MAX_BITS,
	    {Value::BIGINT(-70), Value::BIGINT(-69), Value::BIGINT(-6), Value::BIGINT(0), Value::BIGINT(5)});
	REQUIRE(filter->IsInitialized());
	FloePrefixRangeExport view;
	REQUIRE(filter->FloeTryExport(view));

	REQUIRE(view.span == 75);
	REQUIRE(view.shift == 0);
	REQUIRE(view.word_count == 2);
	// keys -70 and -69 are bits 0 and 1
	REQUIRE(view.words[0] == 3ULL);
	// keys -6, 0 and 5 are bits 64, 70 and 75
	REQUIRE(view.words[1] == 0x841ULL);
	REQUIRE_FALSE(view.dense);
	REQUIRE(view.min == Value::BIGINT(-70));
	REQUIRE(view.max == Value::BIGINT(5));
	REQUIRE(view.min.type() == LogicalType::BIGINT);
	REQUIRE(view.max.type() == LogicalType::BIGINT);
}

#ifndef DUCKDB_DEBUG_NO_INLINE
TEST_CASE("Prefix range filter export of VARCHAR keys longer than the prefix", "[api][prefix_range]") {
	const Value minimum("apple pie and custard");
	const Value maximum("apricot jam on toast");

	DuckDB db(nullptr);
	Connection con(db);
	auto filter = BuildFilter(*con.context, LogicalType::VARCHAR, minimum, maximum, EXACT_MAX_BITS,
	                          {minimum, Value("approve"), maximum});
	REQUIRE(filter->IsInitialized());
	FloePrefixRangeExport view;
	REQUIRE(filter->FloeTryExport(view));

	// the bitmap covers the 4-byte prefixes: "appl" is 0x6170706C and "apri" is 0x61707269
	REQUIRE(view.span == 0x1FD);
	REQUIRE(view.shift == 0);
	REQUIRE(view.word_count == 8);
	// "appl" is bit 0 and "appr" is bit 6
	REQUIRE(view.words[0] == 0x41ULL);
	for (idx_t word_idx = 1; word_idx < 7; word_idx++) {
		REQUIRE(view.words[word_idx] == 0ULL);
	}
	// "apri" is bit 509, the last one
	REQUIRE(view.words[7] == (1ULL << 61));
	REQUIRE_FALSE(view.dense);
	// the bounds are the whole strings, not their prefixes
	REQUIRE(view.min == minimum);
	REQUIRE(view.max == maximum);
	REQUIRE(view.min.type() == LogicalType::VARCHAR);
	REQUIRE(view.max.type() == LogicalType::VARCHAR);
}
#endif

TEST_CASE("Prefix range filter export of a dense build", "[api][prefix_range]") {
	// every key from 3 to 70, split over a serial and a parallel build state
	vector<Value> serial_keys;
	vector<Value> parallel_keys;
	for (int32_t key = 3; key <= 70; key++) {
		auto &keys = key % 2 == 0 ? serial_keys : parallel_keys;
		keys.push_back(Value::INTEGER(key));
	}

	DuckDB db(nullptr);
	Connection con(db);
	auto filter = BuildFilter(*con.context, LogicalType::INTEGER, Value::INTEGER(3), Value::INTEGER(70),
	                          EXACT_MAX_BITS, serial_keys, parallel_keys);
	REQUIRE(filter->IsInitialized());
	FloePrefixRangeExport view;
	REQUIRE(filter->FloeTryExport(view));

	REQUIRE(view.span == 67);
	REQUIRE(view.shift == 0);
	REQUIRE(view.word_count == 2);
	REQUIRE(view.words[0] == NumericLimits<uint64_t>::Maximum());
	// bits 64 to 67 are set; the padding past the span stays zero
	REQUIRE(view.words[1] == 0xFULL);
	REQUIRE(view.dense);
	REQUIRE(view.min == Value::INTEGER(3));
	REQUIRE(view.max == Value::INTEGER(70));
	REQUIRE(view.min.type() == LogicalType::INTEGER);
	REQUIRE(view.max.type() == LogicalType::INTEGER);
}

TEST_CASE("Prefix range filter export is dense when one word is full", "[api][prefix_range]") {
	const auto full_word = NumericLimits<uint64_t>::Maximum();
	DuckDB db(nullptr);
	Connection con(db);

	// 64 buckets: the last bucket is the last bit of the only word
	auto full = BuildFilter(*con.context, LogicalType::INTEGER, Value::INTEGER(0), Value::INTEGER(63),
	                        EXACT_MAX_BITS, IntegerKeys(0, 63, 1, -1));
	FloePrefixRangeExport full_view;
	REQUIRE(full->FloeTryExport(full_view));
	REQUIRE(full_view.span == 63);
	REQUIRE(full_view.shift == 0);
	REQUIRE(full_view.word_count == 1);
	REQUIRE(full_view.words[0] == full_word);
	REQUIRE(full_view.dense);

	// without key 63 the last bit is clear
	auto holed = BuildFilter(*con.context, LogicalType::INTEGER, Value::INTEGER(0), Value::INTEGER(63),
	                         EXACT_MAX_BITS, IntegerKeys(0, 63, 1, 63));
	FloePrefixRangeExport holed_view;
	REQUIRE(holed->FloeTryExport(holed_view));
	REQUIRE(holed_view.word_count == 1);
	REQUIRE(holed_view.words[0] == (full_word >> 1));
	REQUIRE_FALSE(holed_view.dense);
}

TEST_CASE("Prefix range filter export is dense when two words are full", "[api][prefix_range]") {
	const auto full_word = NumericLimits<uint64_t>::Maximum();
	DuckDB db(nullptr);
	Connection con(db);

	// 128 buckets: both words are full, with no padding
	auto full = BuildFilter(*con.context, LogicalType::INTEGER, Value::INTEGER(0), Value::INTEGER(127),
	                        EXACT_MAX_BITS, IntegerKeys(0, 127, 1, -1));
	FloePrefixRangeExport full_view;
	REQUIRE(full->FloeTryExport(full_view));
	REQUIRE(full_view.span == 127);
	REQUIRE(full_view.shift == 0);
	REQUIRE(full_view.word_count == 2);
	REQUIRE(full_view.words[0] == full_word);
	REQUIRE(full_view.words[1] == full_word);
	REQUIRE(full_view.dense);

	// without key 63 the hole is in the first word and the last word is full
	auto first_holed = BuildFilter(*con.context, LogicalType::INTEGER, Value::INTEGER(0), Value::INTEGER(127),
	                               EXACT_MAX_BITS, IntegerKeys(0, 127, 1, 63));
	FloePrefixRangeExport first_holed_view;
	REQUIRE(first_holed->FloeTryExport(first_holed_view));
	REQUIRE(first_holed_view.words[0] == (full_word >> 1));
	REQUIRE(first_holed_view.words[1] == full_word);
	REQUIRE_FALSE(first_holed_view.dense);

	// without key 127 the hole is the last bucket
	auto last_holed = BuildFilter(*con.context, LogicalType::INTEGER, Value::INTEGER(0), Value::INTEGER(127),
	                              EXACT_MAX_BITS, IntegerKeys(0, 127, 1, 127));
	FloePrefixRangeExport last_holed_view;
	REQUIRE(last_holed->FloeTryExport(last_holed_view));
	REQUIRE(last_holed_view.words[0] == full_word);
	REQUIRE(last_holed_view.words[1] == (full_word >> 1));
	REQUIRE_FALSE(last_holed_view.dense);
}

TEST_CASE("Prefix range filter export is dense when every lossy bucket is set", "[api][prefix_range]") {
	const auto full_word = NumericLimits<uint64_t>::Maximum();
	DuckDB db(nullptr);
	Connection con(db);

	// 257 buckets of two keys; every even key sets every bucket
	auto full = BuildFilter(*con.context, LogicalType::INTEGER, Value::INTEGER(0), Value::INTEGER(512), 512,
	                        IntegerKeys(0, 512, 2, -1));
	FloePrefixRangeExport full_view;
	REQUIRE(full->FloeTryExport(full_view));
	REQUIRE(full_view.span == 512);
	REQUIRE(full_view.shift == 1);
	REQUIRE(full_view.word_count == 5);
	REQUIRE(full_view.words[0] == full_word);
	REQUIRE(full_view.words[1] == full_word);
	REQUIRE(full_view.words[2] == full_word);
	REQUIRE(full_view.words[3] == full_word);
	// bucket 256 is the last one; the padding past it stays zero
	REQUIRE(full_view.words[4] == 1ULL);
	REQUIRE(full_view.dense);

	// without key 512 the last bucket is empty
	auto holed = BuildFilter(*con.context, LogicalType::INTEGER, Value::INTEGER(0), Value::INTEGER(512), 512,
	                         IntegerKeys(0, 512, 2, 512));
	FloePrefixRangeExport holed_view;
	REQUIRE(holed->FloeTryExport(holed_view));
	REQUIRE(holed_view.words[3] == full_word);
	REQUIRE(holed_view.words[4] == 0ULL);
	REQUIRE_FALSE(holed_view.dense);
}

TEST_CASE("Prefix range filter outlives the reference of the join that built it", "[api][prefix_range]") {
	DuckDB db(nullptr);
	Connection con(db);
	// the join holds the mutable reference, a scan holds a read-only one
	shared_ptr<PrefixRangeFilter> owner =
	    BuildFilter(*con.context, LogicalType::INTEGER, Value::INTEGER(-5), Value::INTEGER(10), EXACT_MAX_BITS,
	                {Value::INTEGER(-5), Value::INTEGER(-3), Value::INTEGER(0), Value::INTEGER(10)});
	shared_ptr<const PrefixRangeFilter> held = owner;
	owner.reset();

	FloePrefixRangeExport view;
	REQUIRE(held->FloeTryExport(view));
	REQUIRE(view.words[0] == 0x8025ULL);
	REQUIRE(view.min == Value::INTEGER(-5));
	REQUIRE(held->LookupRange(Value::INTEGER(-3), Value::INTEGER(-1)) == FilterPropagateResult::NO_PRUNING_POSSIBLE);
	REQUIRE(held->LookupRange(Value::INTEGER(1), Value::INTEGER(9)) == FilterPropagateResult::FILTER_ALWAYS_FALSE);
}

TEST_CASE("Prefix range filter export of a filter not initialized", "[api][prefix_range]") {
	auto filter = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::INTEGER);
	FloePrefixRangeExport view;
	REQUIRE_FALSE(filter->FloeTryExport(view));
	REQUIRE(view.words == nullptr);
}
