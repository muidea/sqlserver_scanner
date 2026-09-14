//===----------------------------------------------------------------------===//
//                         DuckDB
//
// odbc_connection.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/mutex.hpp"

#include "odbc_result.hpp"
#include "odbc_types.hpp"
#include "odbc_utils.hpp"
#include <atomic>

namespace duckdb {
class ODBCBinaryWriter;
class ODBCTextWriter;
struct ODBCBinaryReader;
class ODBCSchemaEntry;
class ODBCTableEntry;
class ODBCStatement;
class ODBCResult;

struct OwnedODBCConnection {
	explicit OwnedODBCConnection(SQLHDBC conn = nullptr, SQLHENV env = nullptr) : connection(conn), environment(env) {
	}
	~OwnedODBCConnection() {
		if (connection) {
			SQLDisconnect(connection);
			SQLFreeHandle(SQL_HANDLE_DBC, connection);
			connection = nullptr;
		}
		if (environment) {
			SQLFreeHandle(SQL_HANDLE_ENV, environment);
			environment = nullptr;
		}
	}

	SQLHDBC connection;
	SQLHENV environment;
};

class ODBCConnection : public enable_shared_from_this<ODBCConnection> {
public:
	explicit ODBCConnection(shared_ptr<OwnedODBCConnection> connection, const std::string &dsn_p,
	                        ODBCTypeConfig type_config_p);
	~ODBCConnection();
	// disable copy constructors
	ODBCConnection(const ODBCConnection &other) = delete;
	ODBCConnection &operator=(const ODBCConnection &) = delete;
	//! enable move constructors
	ODBCConnection(ODBCConnection &&other) noexcept;
	ODBCConnection &operator=(ODBCConnection &&) noexcept;

public:
	static shared_ptr<ODBCConnection> Open(ODBCTypeConfig type_config, const string &connection_string);
	void Execute(const string &query);
	void Execute(const string &query, vector<Value> params);
	unique_ptr<ODBCResult> Query(const string &query, ODBCResultStreaming streaming);
	unique_ptr<ODBCResult> Query(const string &query, vector<Value> params, ODBCResultStreaming streaming);
	unique_ptr<ODBCResult> QueryWithTimeout(const string &query, ODBCResultStreaming streaming,
	                                        uint32_t timeout_seconds);

	bool IsOpen();
	void Close();

	shared_ptr<OwnedODBCConnection> GetConnection() {
		return connection;
	}
	string GetDSN() {
		return dsn;
	}

	SQLHDBC GetConn() {
		if (!connection || !connection->connection) {
			throw InternalException("ODBCConnection::GetConn - no connection available");
		}
		return connection->connection;
	}

	static void DebugSetPrintQueries(bool print);
	static bool DebugPrintQueries();

	bool CheckConnectionHealth();
	void ReleaseConnection();

private:
	unique_ptr<ODBCResult> QueryInternal(const string &query, vector<Value> params, ODBCResultStreaming streaming,
	                                     ODBCConnectorInterface con_interface, uint32_t timeout_seconds = 0);
	idx_t ODBCExecute(SQLHSTMT stmt, const string &query, vector<Value> params, bool streaming,
	                  uint32_t timeout_seconds = 0);

	std::recursive_mutex query_lock;
	std::atomic<bool> connection_busy {false};
	shared_ptr<OwnedODBCConnection> connection;
	string dsn;
	ODBCTypeConfig type_config;
};

} // namespace duckdb
