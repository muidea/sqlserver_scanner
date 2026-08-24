//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/odbc_connection_pool.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "odbc_connection.hpp"

namespace duckdb {
class ODBCCatalog;
class ODBCConnectionPool;

class ODBCPoolConnection {
public:
	ODBCPoolConnection();
	ODBCPoolConnection(optional_ptr<ODBCConnectionPool> pool, shared_ptr<ODBCConnection> connection);
	~ODBCPoolConnection();
	// disable copy constructors
	ODBCPoolConnection(const ODBCPoolConnection &other) = delete;
	ODBCPoolConnection &operator=(const ODBCPoolConnection &) = delete;
	//! enable move constructors
	ODBCPoolConnection(ODBCPoolConnection &&other) noexcept;
	ODBCPoolConnection &operator=(ODBCPoolConnection &&) noexcept;

	bool HasConnection();
	shared_ptr<ODBCConnection> GetConnection();

private:
	optional_ptr<ODBCConnectionPool> pool;
	shared_ptr<ODBCConnection> connection;
};

class ODBCConnectionPool {
public:
	static constexpr const idx_t DEFAULT_MAX_CONNECTIONS = 64;

	ODBCConnectionPool(ODBCCatalog &odbc_catalog, idx_t maximum_connections = DEFAULT_MAX_CONNECTIONS);

public:
	bool TryGetConnection(ODBCPoolConnection &connection);
	ODBCPoolConnection GetConnection();
	//! Always returns a connection - even if the connection slots are exhausted
	ODBCPoolConnection ForceGetConnection();
	void ReturnConnection(shared_ptr<ODBCConnection> connection);
	void SetMaximumConnections(idx_t new_max);

	static void ODBCSetConnectionCache(ClientContext &context, SetScope scope, Value &parameter);

private:
	ODBCCatalog &odbc_catalog;
	mutex connection_lock;
	idx_t active_connections;
	idx_t maximum_connections;
	vector<shared_ptr<ODBCConnection>> connection_cache;

private:
	ODBCPoolConnection GetConnectionInternal();
};

} // namespace duckdb
