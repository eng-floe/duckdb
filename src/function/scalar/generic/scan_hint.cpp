#include "duckdb/function/scalar/generic_functions.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/scan_hint.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

namespace {

void ScanHintFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value::BOOLEAN(true), count_t(args.size()));
}

} // namespace

ScalarFunction ScanHintFun::GetFunction() {
	ScalarFunction fun(ScanHintFun::Name, {}, LogicalType::BOOLEAN, ScanHintFunction);
	fun.GetSignature().AddParameter("condition", LogicalType::BOOLEAN);
	// true whatever the condition is, NULL included
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

bool ScanHint::Is(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       expr.Cast<BoundFunctionExpression>().Function().GetName() == ScanHintFun::Name;
}

const Expression &ScanHint::Condition(const Expression &hint) {
	D_ASSERT(Is(hint));
	return *hint.Cast<BoundFunctionExpression>().GetChildren()[0];
}

optional_ptr<const BoundColumnRefExpression> ScanHint::PushableColumn(const Expression &hint) {
	auto &condition = Condition(hint);
	// zone map checks evaluate the condition on column bounds, where it must not throw nor change
	if (condition.IsVolatile() || condition.CanThrow()) {
		return nullptr;
	}
	optional_ptr<const BoundColumnRefExpression> column;
	bool one_column = true;
	ExpressionIterator::VisitExpressionClass(condition, ExpressionClass::BOUND_COLUMN_REF, [&](const Expression &ref) {
		auto &column_ref = ref.Cast<BoundColumnRefExpression>();
		one_column = one_column && (!column || column->Binding() == column_ref.Binding());
		column = column_ref;
	});
	return one_column ? column : nullptr;
}

unique_ptr<Expression> ScanHint::Wrap(unique_ptr<Expression> condition) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(condition));
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(ScanHintFun::GetFunction()), std::move(arguments),
	                                          nullptr);
}

} // namespace duckdb
