#include "duckdb.hpp"
#include "odbc_scanner.hpp"
#include "odbc_utils.hpp"
#include "odbc_filter_pushdown.hpp"
#include "storage/odbc_catalog.hpp"
#include "storage/odbc_transaction.hpp"
#include "storage/odbc_connection_pool.hpp"
#include "storage/odbc_table_set.hpp"
#include "odbc_filter_pushdown.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include <thread>

namespace duckdb {

struct ODBCGlobalState;

struct ODBCLocalState : public LocalTableFunctionState {
	ODBCPoolConnection pool_connection;
	shared_ptr<ODBCConnection> connection;
	unique_ptr<ODBCResult> result;
};

struct ODBCGlobalState : public GlobalTableFunctionState {
	explicit ODBCGlobalState(unique_ptr<ODBCResult> result_p) : result(std::move(result_p)) {
	}

	unique_ptr<ODBCResult> result;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static string NormalizeSQLServerDateTimeOffset(string input) {
	StringUtil::Trim(input);
	auto sign_pos = input.find_last_of("+-");
	if (sign_pos != string::npos && sign_pos > 0 && input[sign_pos - 1] == ' ') {
		input.erase(sign_pos - 1, 1);
	}
	return input;
}

static bool TryCastDateTimeOffsetVector(Vector &res_vec, Vector &output_vec, idx_t count, string &error) {
	if (res_vec.GetType().id() != LogicalTypeId::VARCHAR || output_vec.GetType().id() != LogicalTypeId::TIMESTAMP_TZ) {
		return false;
	}
	auto src = FlatVector::GetData<string_t>(res_vec);
	auto dst = FlatVector::GetData<timestamp_tz_t>(output_vec);
	for (idx_t r = 0; r < count; r++) {
		if (FlatVector::IsNull(res_vec, r)) {
			FlatVector::SetNull(output_vec, r, true);
			continue;
		}
		auto s = src[r];
		string raw(s.GetData(), s.GetSize());
		string normalized = NormalizeSQLServerDateTimeOffset(raw);
		timestamp_t parsed_ts;
		if (Timestamp::TryConvertTimestamp(normalized.c_str(), normalized.size(), parsed_ts, true) !=
		    TimestampCastResult::SUCCESS) {
			error = StringUtil::Format("invalid timestamp field format: \"%s\"", raw);
			return false;
		}
		dst[r] = timestamp_tz_t(parsed_ts.value);
	}
	error.clear();
	return true;
}

static unique_ptr<FunctionData> ODBCBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	throw InternalException("ODBCBind");
}

static unique_ptr<GlobalTableFunctionState> ODBCInitGlobalState(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ODBCBindData>();
	// generate the SELECT statement
	string select;
	select += "SELECT ";

	// For SQL Server, TOP clause must come immediately after SELECT
	if (!bind_data.limit.empty()) {
		select += "TOP " + bind_data.limit + " ";
	}

	for (idx_t c = 0; c < input.column_ids.size(); c++) {
		if (c > 0) {
			select += ", ";
		}
		if (input.column_ids[c] == COLUMN_IDENTIFIER_ROW_ID) {
			select += "NULL";
		} else {
			auto column_id = input.column_ids[c];
			string col_name;
			if (column_id < bind_data.remote_names.size()) {
				col_name = bind_data.remote_names[column_id];
			} else {
				auto &col = bind_data.table.GetColumn(LogicalIndex(column_id));
				col_name = col.GetName();
			}
			select += ODBCUtils::WriteIdentifier(col_name);
		}
	}
	select += " FROM ";
	select += ODBCUtils::WriteIdentifier(bind_data.table.schema.name);
	select += ".";
	select += ODBCUtils::WriteIdentifier(bind_data.table.name);
	auto &filter_names = bind_data.remote_names.empty() ? bind_data.names : bind_data.remote_names;
	string filter_string = ODBCFilterPushdown::TransformFilters(input.column_ids, input.filters, filter_names);
	if (!filter_string.empty()) {
		select += " WHERE " + filter_string;
	}

	// run the query
	auto &transaction = ODBCTransaction::Get(context, bind_data.table.catalog);
	auto &con = transaction.GetConnection();

	auto query_result = con.Query(select, bind_data.streaming);

	// 检查查询结果是否为空
	if (!query_result) {
		throw InternalException("ODBCInitGlobalState: Query returned null result for table %s.%s",
		                        bind_data.table.schema.name.c_str(), bind_data.table.name.c_str());
	}

	auto result = make_uniq<ODBCGlobalState>(std::move(query_result));

	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> ODBCInitLocalState(ExecutionContext &context, TableFunctionInitInput &input,
                                                              GlobalTableFunctionState *global_state) {
	return make_uniq<ODBCLocalState>();
}

static void ODBCScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<ODBCGlobalState>();

	if (!gstate.result) {
		throw InternalException("ODBCScan: result pointer is null");
	}

	DataChunk &res_chunk = gstate.result->NextChunk();

	// 空结果集，设置输出为空
	if (res_chunk.size() == 0) {
		output.SetCardinality(0);
		return;
	}

	// 没有列的结果集，设置输出为空
	if (res_chunk.ColumnCount() == 0) {
		output.SetCardinality(0);
		return;
	}

	// 检查列数匹配
	if (output.ColumnCount() != res_chunk.ColumnCount()) {
		throw InternalException("ODBCScan: column count mismatch. Output has %lu columns, result has %lu columns",
		                        output.ColumnCount(), res_chunk.ColumnCount());
	}

	string error;
	for (idx_t c = 0; c < output.ColumnCount(); c++) {
		Vector &output_vec = output.data[c];
		Vector &res_vec = res_chunk.data[c];

		// 检查向量有效性
		if (output_vec.GetType().id() == LogicalTypeId::INVALID) {
			throw InternalException("ODBCScan: output vector %lu has invalid type", c);
		}
		if (res_vec.GetType().id() == LogicalTypeId::INVALID) {
			throw InternalException("ODBCScan: result vector %lu has invalid type", c);
		}

		// 总是使用 TryCast 进行安全的数据复制，避免引用生命周期问题
		VectorOperations::TryCast(context, res_vec, output_vec, res_chunk.size(), &error);

		if (!error.empty()) {
			if (TryCastDateTimeOffsetVector(res_vec, output_vec, res_chunk.size(), error)) {
				continue;
			}
		}

		// throw an error in case of TryCast fails
		if (!error.empty()) {
			throw BinderException(error);
		}
	}
	output.SetCardinality(res_chunk.size());
}

static InsertionOrderPreservingMap<string> ODBCScanToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	auto &bind_data = input.bind_data->Cast<ODBCBindData>();
	result["Table"] = bind_data.table.name;
	return result;
}

static void ODBCScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                              const TableFunction &function) {
	throw NotImplementedException("ODBCScanSerialize");
}

static unique_ptr<FunctionData> ODBCScanDeserialize(Deserializer &deserializer, TableFunction &function) {
	throw NotImplementedException("ODBCScanDeserialize");
}

static BindInfo ODBCGetBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<ODBCBindData>();
	BindInfo info(ScanType::EXTERNAL);
	info.table = bind_data.table;
	return info;
}

ODBCScanFunction::ODBCScanFunction()
    : TableFunction("sqlserver_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, ODBCScan,
                    ODBCBind, ODBCInitGlobalState, ODBCInitLocalState) {
	to_string = ODBCScanToString;
	serialize = ODBCScanSerialize;
	deserialize = ODBCScanDeserialize;
	get_bind_info = ODBCGetBindInfo;
	projection_pushdown = true;
}

//===--------------------------------------------------------------------===//
// ODBC Query
//===--------------------------------------------------------------------===//
static void ValidateSQLServerReadOnlyQuery(const string &sql) {
	vector<string> tokens;
	string token;
	bool single_quote = false;
	bool double_quote = false;
	bool bracket_identifier = false;
	bool line_comment = false;
	bool block_comment = false;
	idx_t statement_terminators = 0;

	auto flush_token = [&]() {
		if (!token.empty()) {
			tokens.push_back(StringUtil::Lower(token));
			token.clear();
		}
	};
	for (idx_t i = 0; i < sql.size(); i++) {
		auto ch = sql[i];
		auto next = i + 1 < sql.size() ? sql[i + 1] : '\0';
		if (line_comment) {
			if (ch == '\n') {
				line_comment = false;
			}
			continue;
		}
		if (block_comment) {
			if (ch == '*' && next == '/') {
				block_comment = false;
				i++;
			}
			continue;
		}
		if (single_quote) {
			if (ch == '\'' && next == '\'') {
				i++;
			} else if (ch == '\'') {
				single_quote = false;
			}
			continue;
		}
		if (double_quote) {
			if (ch == '"' && next == '"') {
				i++;
			} else if (ch == '"') {
				double_quote = false;
			}
			continue;
		}
		if (bracket_identifier) {
			if (ch == ']' && next == ']') {
				i++;
			} else if (ch == ']') {
				bracket_identifier = false;
			}
			continue;
		}
		if (ch == '-' && next == '-') {
			flush_token();
			line_comment = true;
			i++;
		} else if (ch == '/' && next == '*') {
			flush_token();
			block_comment = true;
			i++;
		} else if (ch == '\'') {
			flush_token();
			single_quote = true;
		} else if (ch == '"') {
			flush_token();
			double_quote = true;
		} else if (ch == '[') {
			flush_token();
			bracket_identifier = true;
		} else if (ch == ';') {
			flush_token();
			statement_terminators++;
		} else if (StringUtil::CharacterIsAlphaNumeric(ch) || ch == '_') {
			token += ch;
		} else {
			flush_token();
		}
	}
	flush_token();

	if (single_quote || double_quote || bracket_identifier || block_comment) {
		throw PermissionException("sqlserver_query contains an unterminated literal, identifier, or comment");
	}
	if (tokens.empty() || (tokens[0] != "select" && tokens[0] != "with")) {
		throw PermissionException("sqlserver_query only accepts SELECT or WITH queries");
	}
	auto trimmed_sql = sql;
	StringUtil::Trim(trimmed_sql);
	if (statement_terminators > 1 || (statement_terminators == 1 && !StringUtil::EndsWith(trimmed_sql, ";"))) {
		throw PermissionException("sqlserver_query accepts exactly one read-only statement");
	}
	unordered_set<string> forbidden = {"insert", "update", "delete",   "merge", "exec",   "execute", "create",
	                                   "alter",  "drop",   "truncate", "grant", "revoke", "deny",    "use"};
	for (auto &entry : tokens) {
		if (forbidden.find(entry) != forbidden.end()) {
			throw PermissionException("sqlserver_query only accepts read-only queries");
		}
	}
}

static unique_ptr<FunctionData> ODBCQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("Parameters to sqlserver_query cannot be NULL");
	}

	// look up the database to query
	auto db_name = input.inputs[0].GetValue<string>();
	auto &db_manager = DatabaseManager::Get(context);
	auto db = db_manager.GetDatabase(context, db_name);
	if (!db) {
		throw BinderException("Failed to find attached database \"%s\" referenced in sqlserver_query", db_name);
	}
	auto &catalog = db->GetCatalog();
	if (catalog.GetCatalogType() != "sqlserver") {
		throw BinderException("Attached database \"%s\" does not refer to a SQL Server database", db_name);
	}
	auto &transaction = ODBCTransaction::Get(context, catalog);
	auto sql = input.inputs[1].GetValue<string>();
	ValidateSQLServerReadOnlyQuery(sql);

	vector<Value> params;
	auto params_it = input.named_parameters.find("params");
	if (params_it != input.named_parameters.end()) {
		Value &struct_val = params_it->second;
		if (struct_val.IsNull()) {
			throw BinderException("Parameters to sqlserver_query cannot be NULL");
		}
		if (struct_val.type().id() != LogicalTypeId::STRUCT) {
			throw BinderException("Query parameters must be specified in a STRUCT");
		}
		params = StructValue::GetChildren(struct_val);
	}

	auto result = transaction.GetConnection().Query(sql, std::move(params), ODBCResultStreaming::FORCE_MATERIALIZATION);
	for (auto &field : result->Fields()) {
		names.push_back(field.name);
		return_types.push_back(field.duckdb_type);
	}
	if (return_types.empty()) {
		throw InvalidInputException("Failed to fetch return types for query '%s'", sql);
	}
	return make_uniq<ODBCQueryBindData>(catalog, std::move(result), std::move(sql));
}

static unique_ptr<GlobalTableFunctionState> ODBCQueryInitGlobalState(ClientContext &context,
                                                                     TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->CastNoConst<ODBCQueryBindData>();
	unique_ptr<ODBCResult> odbc_result;
	if (bind_data.result) {
		odbc_result = std::move(bind_data.result);
	} else {
		auto &transaction = ODBCTransaction::Get(context, bind_data.catalog);
		odbc_result = transaction.GetConnection().Query(bind_data.query, ODBCResultStreaming::FORCE_MATERIALIZATION);
	}

	return make_uniq<ODBCGlobalState>(std::move(odbc_result));
}

ODBCQueryFunction::ODBCQueryFunction()
    : TableFunction("sqlserver_query", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ODBCScan, ODBCQueryBind,
                    ODBCQueryInitGlobalState, ODBCInitLocalState) {
	serialize = ODBCScanSerialize;
	deserialize = ODBCScanDeserialize;
	named_parameters["params"] = LogicalType::ANY;
}

} // namespace duckdb
