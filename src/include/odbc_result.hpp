//===----------------------------------------------------------------------===//
//                         DuckDB
//
// odbc_result.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "odbc_types.hpp"
#include "odbc_utils.hpp"
#include <memory>

namespace duckdb {
class ODBCConnection;
struct OwnedODBCConnection;

struct ODBCField {
	string name;
	string type_name;
	SQLSMALLINT odbc_type = SQL_UNKNOWN_TYPE;
	SQLSMALLINT c_type = SQL_C_CHAR;
	LogicalType duckdb_type;

	vector<char> bind_buffer;
	SQLLEN bind_length = 0;
	bool bind_is_null = false;
	bool bind_error = false;

	ODBCField(void *field_info, LogicalType duckdb_type_p, const ODBCTypeConfig &type_config);

	void ResetBind();
};

struct ODBCBind {

	ODBCBind(const LogicalType &ltype);
};

class ODBCResult {
public:
	ODBCResult(const std::string &query_p, ODBCStatementPtr stmt_p, ODBCTypeConfig type_config_p, idx_t affected_rows_p,
	           SQLSMALLINT num_cols_p, shared_ptr<ODBCConnection> connection_p,
	           ODBCResultStreaming streaming_p = ODBCResultStreaming::FORCE_MATERIALIZATION);
	~ODBCResult();

	string GetString(idx_t col);
	int32_t GetInt32(idx_t col);
	int64_t GetInt64(idx_t col);
	bool IsNull(idx_t col);

	DataChunk &NextChunk();
	bool Next();
	idx_t AffectedRows();
	const vector<ODBCField> &Fields();

private:
	string query;
	ODBCStatementPtr stmt;
	ODBCTypeConfig type_config;
	idx_t affected_rows = static_cast<idx_t>(-1);
	shared_ptr<ODBCConnection> connection;

	vector<ODBCField> fields;
	vector<SQLPOINTER> bind_buffers;
	vector<SQLLEN> bind_lengths;
	vector<SQLSMALLINT> bind_types;

	DataChunk data_chunk;
	idx_t row_idx = static_cast<idx_t>(-1);
	bool connection_released = false;

	bool is_streaming = false;
	bool stream_exhausted = false;

	// Materialized mode: cached chunks for full result set
	vector<unique_ptr<DataChunk>> cached_chunks;
	idx_t current_chunk_idx = 0;

	bool FetchNext();
	void HandleTruncatedData();
	void WriteToChunk(idx_t row);
	void CheckColumnIdx(idx_t col);
	void CheckNotNull(idx_t col);
	void CheckType(idx_t col, LogicalTypeId type_id);

	// Materialized mode: read all data into cached_chunks
	void ReadAllData();
};

} // namespace duckdb
