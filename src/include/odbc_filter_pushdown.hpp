//===----------------------------------------------------------------------===//
//                         DuckDB
//
// odbc_filter_pushdown.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"

namespace duckdb {

struct ODBCFilterPushdownResult {
	string where_clause;
	vector<idx_t> unpushed_filter_columns;
};

class ODBCFilterPushdown {
public:
	static string TransformFilters(const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters,
	                               const vector<string> &names);
	static ODBCFilterPushdownResult TransformFiltersWithFallback(const vector<column_t> &column_ids,
	                                                             optional_ptr<TableFilterSet> filters,
	                                                             const vector<string> &names,
	                                                             const vector<string> &remote_type_names);

private:
	static bool TryTransformFilter(const string &column_name, const string &remote_type_name, TableFilter &filter,
	                               string &out);
	static string TransformComparison(ExpressionType type);
	static bool TryCreateExpression(const string &column_name, const string &remote_type_name,
	                                vector<unique_ptr<TableFilter>> &filters, const string &op, string &out);
};

} // namespace duckdb
