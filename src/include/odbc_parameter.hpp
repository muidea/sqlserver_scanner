//===----------------------------------------------------------------------===//
//                         DuckDB
//
// odbc_parameter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "odbc_types.hpp"

namespace duckdb {

struct ODBCParameter {
	Value value;
	SQLSMALLINT buffer_type = SQL_UNKNOWN_TYPE;
	bool is_unsigned = false;
	bool use_text_buffer = false;
	string text_buffer;
	SQLULEN column_size = 0;
	SQLSMALLINT decimal_digits = 0;

	vector<char> bind_buffer;
	SQLLEN bind_length = 0;

	ODBCParameter(const string &query, Value value_p);

	SQLRETURN BindParameter(SQLHSTMT stmt, SQLUSMALLINT param_idx);
};

} // namespace duckdb
