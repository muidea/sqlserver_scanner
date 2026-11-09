#pragma once
#include "guarded_odbc_source_error.hpp"
#include "query_source_error.hpp"
namespace duckdb {
inline string QueryODBCProduct(SQLHDBC connection) {
	SQLCHAR name[256] = {};
	SQLSMALLINT length = 0;
	if (!connection || !SQL_SUCCEEDED(SQLGetInfo(connection, SQL_DBMS_NAME, name, sizeof(name), &length)))
		return "odbc";
	auto product = StringUtil::Lower(string(reinterpret_cast<const char *>(name)));
	if (product.find("oracle") != string::npos)
		return "oracle";
	if (product.find("sql server") != string::npos)
		return "sqlserver";
	if (product.find("hana") != string::npos || product == "hdb")
		return "hana";
	if (product.find("db2") != string::npos)
		return "db2";
	if (product.find("dameng") != string::npos || product == "dm" || product.find("dm dbms") != string::npos)
		return "dameng";
	return "odbc";
}
[[noreturn]] inline void ThrowQueryODBCError(SQLSMALLINT kind, SQLHANDLE handle, SQLHDBC connection,
                                             const string &stage, const string &query) {
	// Capture the entire area before SQLGetInfo or cleanup changes diagnostics.
	auto source = ReadGuardedODBCSourceError(kind, handle, "odbc");
	source.product = QueryODBCProduct(connection);
	throw IOException("%s", QuerySourceMessage(source.JSON(), source.message, stage, query));
}
} // namespace duckdb
