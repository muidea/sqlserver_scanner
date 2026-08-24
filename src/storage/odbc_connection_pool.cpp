#include "storage/odbc_connection_pool.hpp"
#include "storage/odbc_catalog.hpp"

#include <atomic>

namespace duckdb {
static std::atomic<bool> odbc_use_connection_cache {true};

ODBCPoolConnection::ODBCPoolConnection() : pool(nullptr) {
}

ODBCPoolConnection::ODBCPoolConnection(optional_ptr<ODBCConnectionPool> pool, shared_ptr<ODBCConnection> connection_p)
    : pool(pool), connection(std::move(connection_p)) {
}

ODBCPoolConnection::~ODBCPoolConnection() {
	if (pool) {
		pool->ReturnConnection(std::move(connection));
	}
}

ODBCPoolConnection::ODBCPoolConnection(ODBCPoolConnection &&other) noexcept {
	std::swap(pool, other.pool);
	std::swap(connection, other.connection);
}

ODBCPoolConnection &ODBCPoolConnection::operator=(ODBCPoolConnection &&other) noexcept {
	std::swap(pool, other.pool);
	std::swap(connection, other.connection);
	return *this;
}

bool ODBCPoolConnection::HasConnection() {
	return pool;
}

shared_ptr<ODBCConnection> ODBCPoolConnection::GetConnection() {
	if (!HasConnection()) {
		throw InternalException("ODBCPoolConnection::GetConnection called without a connection pool");
	}
	return connection;
}

ODBCConnectionPool::ODBCConnectionPool(ODBCCatalog &odbc_catalog, idx_t maximum_connections_p)
    : odbc_catalog(odbc_catalog), active_connections(0), maximum_connections(maximum_connections_p) {
}

ODBCPoolConnection ODBCConnectionPool::GetConnectionInternal() {
	active_connections++;
	// check if we have any cached connections left
	if (!connection_cache.empty()) {
		auto connection = ODBCPoolConnection(this, std::move(connection_cache.back()));
		connection_cache.pop_back();
		return connection;
	}

	// no cached connections left but there is space to open a new one - open it
	ODBCTypeConfig type_config;
	return ODBCPoolConnection(this, ODBCConnection::Open(type_config, odbc_catalog.connection_string));
}

ODBCPoolConnection ODBCConnectionPool::ForceGetConnection() {
	lock_guard<mutex> l(connection_lock);
	return GetConnectionInternal();
}

bool ODBCConnectionPool::TryGetConnection(ODBCPoolConnection &connection) {
	lock_guard<mutex> l(connection_lock);
	if (active_connections >= maximum_connections) {
		return false;
	}
	connection = GetConnectionInternal();
	return true;
}

void ODBCConnectionPool::ODBCSetConnectionCache(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull()) {
		throw BinderException("Cannot be set to NULL");
	}
	odbc_use_connection_cache.store(BooleanValue::Get(parameter), std::memory_order_relaxed);
}

ODBCPoolConnection ODBCConnectionPool::GetConnection() {
	ODBCPoolConnection result;
	if (!TryGetConnection(result)) {
		throw IOException(
		    "Failed to get connection from ODBCConnectionPool - maximum connection count exceeded (%llu/%llu max)",
		    active_connections, maximum_connections);
	}
	return result;
}

void ODBCConnectionPool::ReturnConnection(shared_ptr<ODBCConnection> connection) {
	lock_guard<mutex> l(connection_lock);
	if (active_connections <= 0) {
		throw InternalException("ODBCConnectionPool::ReturnConnection called but active_connections is 0");
	}
	active_connections--;
	if (active_connections >= maximum_connections) {
		// if the maximum number of connections has been decreased by the user we might need to reclaim the connection
		// immediately
		return;
	}
	if (!odbc_use_connection_cache.load(std::memory_order_relaxed)) {
		return;
	}
	// check if the underlying connection is still usable
	// For ODBC, we need to check if the connection is still valid
	// This is a simplified check - in production we might need more robust connection health checking
	connection_cache.push_back(std::move(connection));
}

void ODBCConnectionPool::SetMaximumConnections(idx_t new_max) {
	lock_guard<mutex> l(connection_lock);
	if (new_max < maximum_connections) {
		// potentially close connections
		// note that we can only close connections in the connection cache
		// we will have to wait for connections to be returned
		auto total_open_connections = active_connections + connection_cache.size();
		while (!connection_cache.empty() && total_open_connections > new_max) {
			total_open_connections--;
			connection_cache.pop_back();
		}
	}
	maximum_connections = new_max;
}

} // namespace duckdb
