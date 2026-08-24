//===----------------------------------------------------------------------===//
//                         DuckDB
//
// odbc_types.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

// On Windows, we need to include Windows headers before DuckDB headers
// to avoid type definition conflicts
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "duckdb.hpp"

// Include ODBC headers
#include <sql.h>
#include <sqlext.h>

namespace duckdb {

struct ODBCTypeData {
	string type_name;
	string column_type;
	int64_t precision;
	int64_t scale;
};

enum class ODBCTypeAnnotation { STANDARD, CAST_TO_VARCHAR, NUMERIC_AS_DOUBLE, CTID, JSONB, FIXED_LENGTH_CHAR };

struct ODBCType {
	idx_t oid = 0;
	ODBCTypeAnnotation info = ODBCTypeAnnotation::STANDARD;
	vector<ODBCType> children;
};

struct ODBCTypeConfig {
	bool bit1_as_boolean = false;
	bool tinyint1_as_boolean = false;
	bool time_as_time = false;

	ODBCTypeConfig();
	ODBCTypeConfig(ClientContext &context);
};

class ODBCTypes {
public:
	static LogicalType TypeToLogicalType(const ODBCTypeConfig &type_config, const ODBCTypeData &input);
	static LogicalType FieldToLogicalType(const ODBCTypeConfig &type_config, SQLSMALLINT data_type, SQLULEN col_size,
	                                      SQLSMALLINT decimal_digits, const string &type_name = string());

	// ODBC type conversion utilities
	static SQLSMALLINT GetCType(SQLSMALLINT sql_type);
	static LogicalType ODBCToLogicalType(const ODBCTypeConfig &type_config, SQLSMALLINT data_type, SQLULEN column_size,
	                                     SQLSMALLINT decimal_digits, const string &type_name = string());
};

} // namespace duckdb
