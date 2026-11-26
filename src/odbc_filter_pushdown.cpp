#include "odbc_filter_pushdown.hpp"
#include "odbc_utils.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

namespace duckdb {

static bool TryTransformFilterExpr(const string &column_name, const string &remote_type_name, const Expression &expr,
                                   const std::function<bool(ExpressionType, string &)> &try_transform_cmp, string &out);

static bool TryCreateConjunctionExpr(const string &column_name, const string &remote_type_name,
                                     const vector<unique_ptr<Expression>> &children, const string &op,
                                     const std::function<bool(ExpressionType, string &)> &try_transform_cmp,
                                     string &out) {
	vector<string> parts;
	for (auto &child : children) {
		string part;
		if (!TryTransformFilterExpr(column_name, remote_type_name, *child, try_transform_cmp, part)) {
			out.clear();
			return false;
		}
		if (part.empty()) {
			if (op == "OR") {
				out.clear();
				return false;
			}
			continue;
		}
		parts.push_back(std::move(part));
	}
	if (parts.empty()) {
		out.clear();
		return true;
	}
	out = "(" + StringUtil::Join(parts, " " + op + " ") + ")";
	return true;
}

static bool IsDirectColumnRef(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_REF ||
	       expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF;
}

static bool TryTransformFilterExpr(const string &column_name, const string &remote_type_name, const Expression &expr,
                                   const std::function<bool(ExpressionType, string &)> &try_transform_cmp,
                                   string &out) {
	if (BoundComparisonExpression::IsComparison(expr)) {
		auto &cmp = expr.Cast<BoundFunctionExpression>();
		auto cmp_type = cmp.GetExpressionType();
		auto &left = BoundComparisonExpression::Left(cmp);
		auto &right = BoundComparisonExpression::Right(cmp);
		const Value *constant = nullptr;
		if (IsDirectColumnRef(left) && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			constant = &right.Cast<BoundConstantExpression>().GetValue();
		} else if (left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT && IsDirectColumnRef(right)) {
			constant = &left.Cast<BoundConstantExpression>().GetValue();
			cmp_type = FlipComparisonExpression(cmp_type);
		} else {
			out.clear();
			return false;
		}
		string operator_string;
		if (!try_transform_cmp(cmp_type, operator_string)) {
			out.clear();
			return false;
		}
		string constant_string;
		if (!ODBCUtils::TryTransformConstant(*constant, remote_type_name, constant_string)) {
			out.clear();
			return false;
		}
		out = StringUtil::Format("%s %s %s", column_name, operator_string, constant_string);
		return true;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto op = conj.GetExpressionType() == ExpressionType::CONJUNCTION_AND ? "AND" : "OR";
		return TryCreateConjunctionExpr(column_name, remote_type_name, conj.GetChildren(), op, try_transform_cmp, out);
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &op = expr.Cast<BoundOperatorExpression>();
		auto &children = op.GetChildren();
		if (children.empty() || !IsDirectColumnRef(*children[0])) {
			out.clear();
			return false;
		}
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
			out = column_name + " IS NULL";
			return true;
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			out = column_name + " IS NOT NULL";
			return true;
		case ExpressionType::COMPARE_IN: {
			if (children.size() <= 1) {
				out.clear();
				return false;
			}
			string in_list;
			for (idx_t i = 1; i < children.size(); i++) {
				if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
					out.clear();
					return false;
				}
				string constant_string;
				if (!ODBCUtils::TryTransformConstant(children[i]->Cast<BoundConstantExpression>().GetValue(),
				                                     remote_type_name, constant_string)) {
					out.clear();
					return false;
				}
				if (!in_list.empty()) {
					in_list += ", ";
				}
				in_list += constant_string;
			}
			out = column_name + " IN (" + in_list + ")";
			return true;
		}
		default:
			out.clear();
			return false;
		}
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (func.Function().GetName() == OptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<OptionalFilterFunctionData>();
			if (!data.child_filter_expr) {
				out.clear();
				return true;
			}
			if (!TryTransformFilterExpr(column_name, remote_type_name, *data.child_filter_expr, try_transform_cmp,
			                            out)) {
				// Optional pushdown hints may be safely omitted locally without changing query semantics.
				out.clear();
			}
			return true;
		}
		if (func.Function().GetName() == SelectivityOptionalFilterScalarFun::NAME && func.BindInfo()) {
			auto &data = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>();
			if (!data.child_filter_expr) {
				out.clear();
				return true;
			}
			if (!TryTransformFilterExpr(column_name, remote_type_name, *data.child_filter_expr, try_transform_cmp,
			                            out)) {
				// Optional pushdown hints may be safely omitted locally without changing query semantics.
				out.clear();
			}
			return true;
		}
		if (func.Function().GetName() == DynamicFilterScalarFun::NAME) {
			out.clear();
			return true;
		}
		out.clear();
		return false;
	}
	default:
		out.clear();
		return false;
	}
}

bool ODBCFilterPushdown::TryTransformComparison(ExpressionType type, string &out) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		out = "=";
		return true;
	case ExpressionType::COMPARE_NOTEQUAL:
		out = "!=";
		return true;
	case ExpressionType::COMPARE_LESSTHAN:
		out = "<";
		return true;
	case ExpressionType::COMPARE_GREATERTHAN:
		out = ">";
		return true;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		out = "<=";
		return true;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		out = ">=";
		return true;
	default:
		out.clear();
		return false;
	}
}

bool ODBCFilterPushdown::TryTransformFilter(const string &column_name, const string &remote_type_name,
                                            const TableFilter &filter, string &out) {
	if (filter.filter_type == TableFilterType::EXPRESSION_FILTER) {
		auto &expr_filter = filter.Cast<ExpressionFilter>();
		if (!expr_filter.expr) {
			out.clear();
			return true;
		}
		return TryTransformFilterExpr(column_name, remote_type_name, *expr_filter.expr, TryTransformComparison, out);
	}
	out.clear();
	return false;
}

ODBCFilterPushdownResult ODBCFilterPushdown::TransformFiltersWithFallback(const vector<column_t> &column_ids,
                                                                          optional_ptr<TableFilterSet> filters,
                                                                          const vector<string> &names,
                                                                          const vector<string> &remote_type_names) {
	ODBCFilterPushdownResult res;
	if (!filters || !filters->HasFilters()) {
		return res;
	}
	for (auto &entry : *filters) {
		auto filter_col_idx = entry.GetIndex().GetIndexUnsafe();
		if (filter_col_idx >= column_ids.size()) {
			res.unpushed_filter_columns.push_back(filter_col_idx);
			continue;
		}
		auto col_id = column_ids[filter_col_idx];
		if (col_id >= names.size()) {
			res.unpushed_filter_columns.push_back(filter_col_idx);
			continue;
		}
		auto column_name = ODBCUtils::WriteIdentifier(names[col_id]);
		string remote_type_name = col_id < remote_type_names.size() ? remote_type_names[col_id] : string();
		auto &filter = entry.Filter();
		string new_filter;
		if (!TryTransformFilter(column_name, remote_type_name, filter, new_filter)) {
			res.unpushed_filter_columns.push_back(filter_col_idx);
			continue;
		}
		if (new_filter.empty()) {
			continue;
		}
		if (!res.where_clause.empty()) {
			res.where_clause += " AND ";
		}
		res.where_clause += new_filter;
	}
	return res;
}

string ODBCFilterPushdown::TransformFilters(const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters,
                                            const vector<string> &names) {
	return TransformFiltersWithFallback(column_ids, filters, names, vector<string>()).where_clause;
}

} // namespace duckdb
