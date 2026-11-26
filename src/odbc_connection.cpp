#include "query_odbc_source_error.hpp"
#include "odbc_connection.hpp"

#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/parser/column_list.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/storage/table_storage_info.hpp"

#include "odbc_parameter.hpp"
#include "odbc_types.hpp"
#include "sqlserver_temporal_utils.hpp"
#include "encoding_utils.hpp"
namespace duckdb {

static bool debug_odbc_print_queries = false;

static SQLRETURN SQLExecDirectWithFallback(SQLHSTMT stmt, const string &query) {
#ifdef _WIN32
	SQLRETURN retcode = SQLExecDirect(stmt, (SQLCHAR *)query.c_str(), SQL_NTS);
	if (SQL_SUCCEEDED(retcode)) {
		return retcode;
	}
	string query_gbk = UTF8ToGBK(query);
	if (query_gbk != query) {
		SQLFreeStmt(stmt, SQL_CLOSE);
		retcode = SQLExecDirect(stmt, (SQLCHAR *)query_gbk.c_str(), SQL_NTS);
	}
	return retcode;
#else
	return SQLExecDirect(stmt, (SQLCHAR *)query.c_str(), SQL_NTS);
#endif
}

static SQLRETURN SQLPrepareWithFallback(SQLHSTMT stmt, const string &query) {
#ifdef _WIN32
	SQLRETURN retcode = SQLPrepare(stmt, (SQLCHAR *)query.c_str(), SQL_NTS);
	if (SQL_SUCCEEDED(retcode)) {
		return retcode;
	}
	string query_gbk = UTF8ToGBK(query);
	if (query_gbk != query) {
		SQLFreeStmt(stmt, SQL_CLOSE);
		retcode = SQLPrepare(stmt, (SQLCHAR *)query_gbk.c_str(), SQL_NTS);
	}
	return retcode;
#else
	return SQLPrepare(stmt, (SQLCHAR *)query.c_str(), SQL_NTS);
#endif
}

ODBCConnection::ODBCConnection(shared_ptr<OwnedODBCConnection> connection_p, const std::string &dsn_p,
                               ODBCTypeConfig type_config_p)
    : connection(std::move(connection_p)), dsn(std::move(dsn_p)), type_config(std::move(type_config_p)) {
}

ODBCConnection::~ODBCConnection() {
	Close();
}

ODBCConnection::ODBCConnection(ODBCConnection &&other) noexcept {
	std::swap(connection, other.connection);
	std::swap(dsn, other.dsn);
	std::swap(type_config, other.type_config);
}

ODBCConnection &ODBCConnection::operator=(ODBCConnection &&other) noexcept {
	std::swap(connection, other.connection);
	std::swap(dsn, other.dsn);
	std::swap(type_config, other.type_config);
	return *this;
}

shared_ptr<ODBCConnection> ODBCConnection::Open(ODBCTypeConfig type_config, const string &connection_string) {
	SQLHENV env = SQL_NULL_HENV;
	SQLHDBC dbc = ODBCUtils::Connect(connection_string, env);
	auto owned_connection = make_shared_ptr<OwnedODBCConnection>(dbc, env);
	auto conn = make_shared_ptr<ODBCConnection>(std::move(owned_connection), connection_string, std::move(type_config));
	conn->ResetSessionTemporalSettings();
	return conn;
}

void ODBCConnection::ResetSessionTemporalSettings() {
	Execute(SQLSERVER_SESSION_TEMPORAL_SQL);
}

idx_t ODBCConnection::ODBCExecute(SQLHSTMT stmt, const string &query, vector<Value> params, bool streaming,
                                  uint32_t timeout_seconds) {
	if (ODBCConnection::DebugPrintQueries()) {
		Printer::Print(query + "\n");
	}

	lock_guard<std::recursive_mutex> l(query_lock);

	SQLRETURN retcode;

	if (!stmt) { // basic interface
		auto hdbc = GetConn();
		SQLHSTMT hstmt = SQL_NULL_HSTMT;
		retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
		if (!SQL_SUCCEEDED(retcode)) {
			ThrowQueryODBCError(SQL_HANDLE_DBC, hdbc, GetConn(), "source_prepare", query);
		}

		try {
			retcode = SQLExecDirectWithFallback(hstmt, query);
			// FreeTDS returns SQL_NO_DATA for SET statements and 0-row DML without diagnostics.
			if (!SQL_SUCCEEDED(retcode) && retcode != SQL_NO_DATA) {
				ThrowQueryODBCError(SQL_HANDLE_STMT, hstmt, GetConn(), "source_execute", query);
			}
		} catch (const std::exception &) {
			// 确保语句句柄被清理
			if (hstmt != SQL_NULL_HSTMT) {
				SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
			}
			throw;
		}

		SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
		return 0;
	}

	// prepared statement interface
	// 对于预编译语句接口，调用者负责清理语句句柄
	// 我们只需要确保在失败时抛出适当的异常

	if (timeout_seconds > 0) {
		retcode = SQLSetStmtAttr(stmt, SQL_ATTR_QUERY_TIMEOUT,
		                         reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(timeout_seconds)), 0);
		if (!SQL_SUCCEEDED(retcode)) {
			ThrowQueryODBCError(SQL_HANDLE_STMT, stmt, GetConn(), "source_prepare", query);
		}
	}

	// FreeTDS has a cursor-state bug for prepared statements that return
	// several variable-width columns (the next execution can expose values
	// from a different column). Queries without parameters do not need the
	// prepare path, so execute them directly on the same statement handle.
	if (params.empty()) {
		retcode = SQLExecDirectWithFallback(stmt, query);
		if (retcode == SQL_NO_DATA) {
			return 0;
		}
	} else {
		retcode = SQLPrepareWithFallback(stmt, query);
	}
	if (!SQL_SUCCEEDED(retcode)) {
		ThrowQueryODBCError(SQL_HANDLE_STMT, stmt, GetConn(), params.empty() ? "source_execute" : "source_prepare",
		                    query);
	}

	vector<ODBCParameter> odbc_params;
	if (params.empty()) {
		SQLLEN affected_rows = 0;
		retcode = SQLRowCount(stmt, &affected_rows);
		if (!SQL_SUCCEEDED(retcode)) {
			affected_rows = -1;
		}
		SQLSMALLINT num_cols = 0;
		SQLRETURN col_ret = SQLNumResultCols(stmt, &num_cols);
		if (SQL_SUCCEEDED(col_ret) && num_cols > 0) {
			affected_rows = -1;
		}
		return affected_rows >= 0 ? static_cast<idx_t>(affected_rows) : static_cast<idx_t>(-1);
	}
	if (params.size() > 0) {
		SQLSMALLINT param_count;
		retcode = SQLNumParams(stmt, &param_count);
		if (!SQL_SUCCEEDED(retcode)) {
			ThrowQueryODBCError(SQL_HANDLE_STMT, stmt, GetConn(), "source_describe", query);
		}

		if (param_count != params.size()) {
			throw IOException(
			    "Incorrect query parameters count specified, expected: %d, actual: %zu, ODBC query \"%s\"\n",
			    param_count, params.size(), query.c_str());
		}

		odbc_params.reserve(params.size());
		for (Value &dp : params) {
			ODBCParameter mp(query, std::move(dp));
			odbc_params.emplace_back(std::move(mp));
			ODBCParameter &mp_ref = odbc_params.back();
			SQLRETURN bind_ret = mp_ref.BindParameter(stmt, odbc_params.size());
			if (!SQL_SUCCEEDED(bind_ret)) {
				ThrowQueryODBCError(SQL_HANDLE_STMT, stmt, GetConn(), "source_bind", query);
			}
		}
	}

	retcode = SQLExecute(stmt);
	if (retcode == SQL_NO_DATA) {
		return 0;
	}
	if (!SQL_SUCCEEDED(retcode)) {
		ThrowQueryODBCError(SQL_HANDLE_STMT, stmt, GetConn(), "source_execute", query);
	}

	SQLLEN affected_rows = 0;
	retcode = SQLRowCount(stmt, &affected_rows);
	if (!SQL_SUCCEEDED(retcode)) {
		// For SELECT queries, SQLRowCount should return -1
		affected_rows = -1;
	}

	// For ODBC, we need to be more careful:
	// - SELECT queries should return -1 (no affected rows)
	// - DML queries (INSERT, UPDATE, DELETE) return >= 0
	// But some ODBC drivers might return 0 for SELECT queries, so we need to check column count
	SQLSMALLINT num_cols = 0;
	SQLRETURN col_ret = SQLNumResultCols(stmt, &num_cols);
	if (SQL_SUCCEEDED(col_ret) && num_cols > 0) {
		// This is a SELECT query, force affected_rows to -1
		affected_rows = -1;
	}

	// Return -1 for SELECT queries (no affected rows), otherwise return the actual count
	return affected_rows >= 0 ? static_cast<idx_t>(affected_rows) : static_cast<idx_t>(-1);
}

unique_ptr<ODBCResult> ODBCConnection::QueryInternal(const string &query, vector<Value> params,
                                                     ODBCResultStreaming streaming,
                                                     ODBCConnectorInterface con_interface, uint32_t timeout_seconds) {

	// 检查连接是否繁忙
	bool expected = false;
	if (!connection_busy.compare_exchange_strong(expected, true)) {
		throw IOException("ODBC connection is busy executing another query \"%s\"\n", query.c_str());
	}

	// 检查连接健康状态
	if (!CheckConnectionHealth()) {
		ReleaseConnection();
		throw IOException("ODBC connection is not healthy for query \"%s\"\n", query.c_str());
	}

	// 注意：我们不使用ConnectionBusyGuard，因为连接需要保持繁忙直到ODBCResult被销毁
	// 连接繁忙状态由ODBCResult在析构时释放

	// 使用递归互斥锁保护连接操作，防止多个查询同时使用同一个连接
	// 使用 recursive_mutex 允许同一线程重入，避免死锁
	lock_guard<std::recursive_mutex> l(query_lock);

	auto hdbc = GetConn();

	// 检查连接是否有效
	if (!hdbc) {
		ReleaseConnection();
		throw IOException("ODBC connection is not open for query \"%s\"\n", query.c_str());
	}

	// 检查连接状态
	SQLINTEGER conn_attr = SQL_CD_FALSE;
	SQLRETURN status_ret = SQLGetConnectAttr(hdbc, SQL_ATTR_CONNECTION_DEAD, &conn_attr, 0, nullptr);
	if (SQL_SUCCEEDED(status_ret) && conn_attr == SQL_CD_TRUE) {
		ReleaseConnection();
		throw IOException("ODBC connection is dead for query \"%s\"\n", query.c_str());
	}

	bool result_streaming = streaming == ODBCResultStreaming::ALLOW_STREAMING;
	bool basic_interface = con_interface == ODBCConnectorInterface::BASIC;

	if (basic_interface) {
		try {
			ODBCExecute(nullptr, query, params, result_streaming, timeout_seconds);
		} catch (...) {
			ReleaseConnection();
			throw;
		}
		// BASIC接口不创建ODBCResult，需要立即释放连接忙状态
		ReleaseConnection();
		return unique_ptr<ODBCResult>(nullptr);
	}

	SQLHSTMT hstmt = SQL_NULL_HSTMT;
	SQLRETURN retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
	if (!SQL_SUCCEEDED(retcode)) {
		// 释放连接忙状态
		ReleaseConnection();
		ThrowQueryODBCError(SQL_HANDLE_DBC, hdbc, GetConn(), "source_prepare", query);
	}

	idx_t affected_rows = 0;
	try {
		affected_rows = ODBCExecute(hstmt, query, params, result_streaming, timeout_seconds);
	} catch (const std::exception &) {
		// 执行失败，清理语句句柄
		if (hstmt != SQL_NULL_HSTMT) {
			SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
		}
		// 释放连接忙状态
		ReleaseConnection();
		throw;
	}

	// Get number of result columns
	SQLSMALLINT num_cols = 0;
	retcode = SQLNumResultCols(hstmt, &num_cols);
	if (!SQL_SUCCEEDED(retcode)) {
		num_cols = 0;
	}

	// For SELECT queries, affected_rows will be -1, but we still need to process the result set
	if (affected_rows == static_cast<idx_t>(-1) && num_cols == 0) {
		// Try to get column count again - some drivers might not set it immediately
		retcode = SQLNumResultCols(hstmt, &num_cols);
	}

	try {
		auto result = make_uniq<ODBCResult>(query, ODBCStatementPtr(hstmt, ODBCStatementDelete), type_config,
		                                    affected_rows, num_cols, shared_from_this(), streaming);
		return result;
	} catch (const std::exception &e) {
		// ODBCStatementPtr already owns and releases hstmt if construction
		// fails. Freeing the raw handle here would be a double free.
		// 释放连接忙状态
		ReleaseConnection();
		throw;
	}
}

unique_ptr<ODBCResult> ODBCConnection::Query(const string &query, ODBCResultStreaming streaming) {
	return Query(query, vector<Value>(), streaming);
}

unique_ptr<ODBCResult> ODBCConnection::Query(const string &query, vector<Value> params, ODBCResultStreaming streaming) {
	return QueryInternal(query, params, streaming, ODBCConnectorInterface::PREPARED_STATEMENT);
}

unique_ptr<ODBCResult> ODBCConnection::QueryWithTimeout(const string &query, ODBCResultStreaming streaming,
                                                        uint32_t timeout_seconds) {
	if (timeout_seconds == 0) {
		throw InvalidInputException("ODBC query timeout must be greater than zero");
	}
	return QueryInternal(query, vector<Value>(), streaming, ODBCConnectorInterface::PREPARED_STATEMENT,
	                     timeout_seconds);
}

void ODBCConnection::Execute(const string &query) {
	Execute(query, vector<Value>());
}

void ODBCConnection::Execute(const string &query, vector<Value> params) {
	ODBCConnectorInterface con_interface =
	    params.size() > 0 ? ODBCConnectorInterface::PREPARED_STATEMENT : ODBCConnectorInterface::BASIC;
	QueryInternal(query, std::move(params), ODBCResultStreaming::FORCE_MATERIALIZATION, con_interface);
}

bool ODBCConnection::IsOpen() {
	return connection.get();
}

void ODBCConnection::Close() {
	if (!IsOpen()) {
		return;
	}
	connection = nullptr;
}

bool ODBCConnection::CheckConnectionHealth() {
	if (!IsOpen()) {
		return false;
	}

	auto hdbc = GetConn();
	SQLINTEGER conn_attr = SQL_CD_FALSE;
	SQLRETURN retcode = SQLGetConnectAttr(hdbc, SQL_ATTR_CONNECTION_DEAD, &conn_attr, 0, nullptr);

	if (!SQL_SUCCEEDED(retcode)) {
		// SQL_ATTR_CONNECTION_DEAD is optional in some ODBC drivers.
		return true;
	}

	return (conn_attr == SQL_CD_FALSE);
}

void ODBCConnection::DebugSetPrintQueries(bool print) {
	debug_odbc_print_queries = print;
}

bool ODBCConnection::DebugPrintQueries() {
	return debug_odbc_print_queries;
}

void ODBCConnection::ReleaseConnection() {
	// 使用原子操作确保幂等性和线程安全
	bool expected = true;
	connection_busy.compare_exchange_strong(expected, false);
}

} // namespace duckdb
