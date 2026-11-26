//===----------------------------------------------------------------------===//
//                         DuckDB
//
// odbc_filter_pushdown.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/table_filter_set.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"

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
	static bool TryTransformFilter(const string &column_name, const string &remote_type_name, const TableFilter &filter,
	                               string &out);
	static bool TryTransformComparison(ExpressionType type, string &out);
};

} // namespace duckdb
