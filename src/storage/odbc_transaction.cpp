#include "storage/odbc_transaction.hpp"

#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"

#include "odbc_result.hpp"
#include "odbc_types.hpp"
#include "storage/odbc_catalog.hpp"

namespace duckdb {

ODBCTransaction::ODBCTransaction(ODBCCatalog &odbc_catalog, TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), odbc_catalog(odbc_catalog), access_mode(odbc_catalog.access_mode) {
	// 从连接池获取连接
	if (!odbc_catalog.GetConnectionPool().TryGetConnection(pool_connection)) {
		throw IOException("Failed to get connection from ODBC connection pool");
	}
}

ODBCTransaction::~ODBCTransaction() = default;

void ODBCTransaction::Start() {
	transaction_state = ODBCTransactionState::TRANSACTION_NOT_YET_STARTED;
}
void ODBCTransaction::Commit() {
	if (transaction_state == ODBCTransactionState::TRANSACTION_STARTED) {
		transaction_state = ODBCTransactionState::TRANSACTION_FINISHED;
		// Don't explicitly commit - let the ODBC driver handle transaction management
		// connection.Execute("COMMIT");
	}
}
void ODBCTransaction::Rollback() {
	if (transaction_state == ODBCTransactionState::TRANSACTION_STARTED) {
		transaction_state = ODBCTransactionState::TRANSACTION_FINISHED;
		// Don't explicitly rollback - let the ODBC driver handle transaction management
		// connection.Execute("ROLLBACK");
	}
}

ODBCConnection &ODBCTransaction::GetConnection() {
	// For ODBC connections, we rely on the driver's default transaction management
	// SQL Server reads use the driver's default transaction handling.
	if (transaction_state == ODBCTransactionState::TRANSACTION_NOT_YET_STARTED) {
		transaction_state = ODBCTransactionState::TRANSACTION_STARTED;
		// Don't explicitly start a transaction - let the ODBC driver handle it
	}

	if (!pool_connection.HasConnection()) {
		throw InternalException("ODBCTransaction::GetConnection called but pool connection is not valid");
	}

	return *pool_connection.GetConnection();
}

shared_ptr<ODBCConnection> ODBCTransaction::GetConnectionShared() {
	// For ODBC connections, we rely on the driver's default transaction management
	// SQL Server reads use the driver's default transaction handling.
	if (transaction_state == ODBCTransactionState::TRANSACTION_NOT_YET_STARTED) {
		transaction_state = ODBCTransactionState::TRANSACTION_STARTED;
		// Don't explicitly start a transaction - let the ODBC driver handle it
	}

	if (!pool_connection.HasConnection()) {
		throw InternalException("ODBCTransaction::GetConnectionShared called but pool connection is not valid");
	}

	return pool_connection.GetConnection();
}

unique_ptr<ODBCResult> ODBCTransaction::Query(const string &query) {
	// For ODBC connections, we rely on the driver's default transaction management
	if (transaction_state == ODBCTransactionState::TRANSACTION_NOT_YET_STARTED) {
		transaction_state = ODBCTransactionState::TRANSACTION_STARTED;
		// Don't explicitly start a transaction - let the ODBC driver handle it
	}

	if (!pool_connection.HasConnection()) {
		throw InternalException("ODBCTransaction::Query called but pool connection is not valid");
	}

	return pool_connection.GetConnection()->Query(query, ODBCResultStreaming::FORCE_MATERIALIZATION);
}

ODBCTransaction &ODBCTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<ODBCTransaction>();
}

} // namespace duckdb
