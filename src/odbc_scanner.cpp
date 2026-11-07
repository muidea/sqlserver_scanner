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
#include <mutex>

namespace duckdb {

// Some SQL Server ODBC drivers keep process-global cursor/diagnostic state.
// Serialize the dedicated relational metadata ABI across all DuckDB wrappers;
// ordinary table scans remain independently pooled.  The lock is held only
// through bind-time result materialization, after which the statement is
// released by ODBCResult before the connection is reusable.
static std::mutex relational_metadata_mutex;

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
		error.clear();

		// 检查向量有效性
		if (output_vec.GetType().id() == LogicalTypeId::INVALID) {
			throw InternalException("ODBCScan: output vector %lu has invalid type", c);
		}
		if (res_vec.GetType().id() == LogicalTypeId::INVALID) {
			throw InternalException("ODBCScan: result vector %lu has invalid type", c);
		}

		if (res_vec.GetType() == output_vec.GetType()) {
			// A same-type TryCast is DuckDB's NopCast and only references the
			// source vector. Materialized ODBC chunks are owned by the function
			// state, so the output must receive an independent copy.
			VectorOperations::Copy(res_vec, output_vec, res_chunk.size(), 0, 0);
		} else {
			VectorOperations::TryCast(context, res_vec, output_vec, res_chunk.size(), &error);
		}

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

//===--------------------------------------------------------------------===//
// SQL Server relational metadata
//===--------------------------------------------------------------------===//
static string SQLServerMetadataLiteral(const string &value) {
	return "'" + StringUtil::Replace(value, "'", "''") + "'";
}

static string SQLServerMetadataScope(const string &schema, const vector<string> &tables, bool legacy) {
	auto schema_column = legacy ? "USER_NAME(t.uid)" : "s.name";
	auto condition =
	    schema.empty() ? string() : " AND " + string(schema_column) + "=N" + SQLServerMetadataLiteral(schema);
	if (!tables.empty()) {
		condition += " AND t.name COLLATE Latin1_General_BIN IN (";
		for (idx_t i = 0; i < tables.size(); i++) {
			if (i)
				condition += ",";
			condition += "N" + SQLServerMetadataLiteral(tables[i]);
		}
		condition += ")";
	}
	return condition;
}

static string SQLServerLegacyRelationalMetadataSQL(const string &category, const string &schema,
                                                   const vector<string> &tables) {
	auto condition = SQLServerMetadataScope(schema, tables, true);
	if (category == "columnProperties") {
		return R"SQL(SELECT CAST(USER_NAME(t.uid) AS varchar(256)),CAST(t.name AS varchar(256)),CAST(c.name AS varchar(256)),CAST(c.colid AS varchar(20)),CAST(CASE WHEN c.isnullable=1 THEN 1 ELSE 0 END AS varchar(5)),CAST('' AS varchar(1)),CAST(0 AS varchar(5)),CAST('' AS varchar(1)),CAST('' AS varchar(1)),CAST(0 AS varchar(5)),CAST('' AS varchar(1)),CAST(ty.name AS varchar(256))
FROM sysobjects t JOIN syscolumns c ON c.id=t.id JOIN systypes ty ON ty.xusertype=c.xusertype
WHERE t.xtype IN ('U','V'))SQL" +
		       condition + " ORDER BY 1,2,c.colid";
	}
	if (category == "indexes") {
		return R"SQL(SELECT CAST(USER_NAME(t.uid) AS varchar(256)),CAST(t.name AS varchar(256)),CAST(i.id AS varchar(30))+':'+CAST(i.indid AS varchar(20)),CAST(i.name AS varchar(256)),CAST(CASE WHEN (i.status & 16)<>0 THEN 'CLUSTERED' ELSE 'NONCLUSTERED' END AS varchar(64)),CAST(CASE WHEN (i.status & 2)<>0 THEN 1 ELSE 0 END AS varchar(5)),CAST(CASE WHEN (i.status & 2048)<>0 THEN 1 ELSE 0 END AS varchar(5)),CAST(0 AS varchar(5)),CAST(0 AS varchar(5)),CAST('' AS varchar(1)),CAST(k.keyno AS varchar(20)),CAST(k.keyno AS varchar(20)),CAST(0 AS varchar(5)),CAST(c.name AS varchar(256)),CAST(0 AS varchar(5)),CAST(0 AS varchar(5)),CAST(0 AS varchar(5))
FROM sysindexes i JOIN sysobjects t ON t.id=i.id JOIN sysindexkeys k ON k.id=i.id AND k.indid=i.indid AND k.keyno>0 JOIN syscolumns c ON c.id=k.id AND c.colid=k.colid
WHERE t.xtype IN ('U','V') AND i.indid>0)SQL" +
		       condition + " ORDER BY 1,2,i.indid,k.keyno";
	}
	if (category == "constraints") {
		return R"SQL(SELECT CAST(USER_NAME(t.uid) AS varchar(256)),CAST(t.name AS varchar(256)),CAST(i.id AS varchar(30))+':'+CAST(i.indid AS varchar(20)),CAST(i.name AS varchar(256)),CAST('PK' AS varchar(5)),CAST(k.keyno AS varchar(20)),CAST(c.name AS varchar(256)),CAST('' AS varchar(256)),CAST('' AS varchar(256)),CAST('' AS varchar(256)),CAST('' AS varchar(64)),CAST('' AS varchar(64)),CAST('' AS varchar(8000)),CAST(0 AS varchar(5)),CAST(0 AS varchar(5)),CAST(i.id AS varchar(30))+':'+CAST(i.indid AS varchar(20))
FROM sysindexes i JOIN sysobjects t ON t.id=i.id JOIN sysindexkeys k ON k.id=i.id AND k.indid=i.indid AND k.keyno>0 JOIN syscolumns c ON c.id=k.id AND c.colid=k.colid
WHERE t.xtype IN ('U','V') AND i.indid>0 AND (i.status & 2048)<>0)SQL" +
		       condition + R"SQL(
UNION ALL
SELECT CAST(USER_NAME(t.uid) AS varchar(256)),CAST(t.name AS varchar(256)),CAST(fk.constid AS varchar(30)),CAST(o.name AS varchar(256)),CAST('F' AS varchar(5)),CAST(fk.keyno AS varchar(20)),CAST(pc.name AS varchar(256)),CAST(USER_NAME(rt.uid) AS varchar(256)),CAST(rt.name AS varchar(256)),CAST(rc.name AS varchar(256)),CAST('' AS varchar(64)),CAST('' AS varchar(64)),CAST('' AS varchar(8000)),CAST(0 AS varchar(5)),CAST(0 AS varchar(5)),CAST('' AS varchar(64))
FROM sysforeignkeys fk JOIN sysobjects t ON t.id=fk.fkeyid JOIN sysobjects rt ON rt.id=fk.rkeyid JOIN sysobjects o ON o.id=fk.constid JOIN syscolumns pc ON pc.id=fk.fkeyid AND pc.colid=fk.fkey JOIN syscolumns rc ON rc.id=fk.rkeyid AND rc.colid=fk.rkey
WHERE t.xtype IN ('U','V'))SQL" +
		       condition + " ORDER BY 1,2,3,6";
	}
	throw InvalidInputException("Unsupported sqlserver_relational_metadata category \"%s\"", category);
}

static string SQLServerRelationalMetadataSQL(const string &category, const string &schema, bool legacy,
                                             const vector<string> &tables) {
	if (legacy) {
		return SQLServerLegacyRelationalMetadataSQL(category, schema, tables);
	}
	auto condition = SQLServerMetadataScope(schema, tables, false);
	if (category == "capabilities") {
		return "SELECT CAST(SERVERPROPERTY('ProductVersion') AS varchar(128)) AS product_version";
	}
	if (category == "columnProperties") {
		return R"SQL(SELECT CAST(s.name AS varchar(256)) AS schema_name,CAST(t.name AS varchar(256)) AS table_name,CAST(c.name AS varchar(256)) AS column_name,CAST(c.column_id AS varchar(20)) AS column_id,CAST(c.is_nullable AS varchar(5)) AS is_nullable,CAST(COALESCE(dc.definition,'') AS varchar(max)) AS default_definition,CAST(c.is_identity AS varchar(5)) AS is_identity,COALESCE(CAST(ic.seed_value AS varchar(128)),'') AS identity_seed,COALESCE(CAST(ic.increment_value AS varchar(128)),'') AS identity_increment,CAST(c.is_computed AS varchar(5)) AS is_computed,CAST(COALESCE(cc.definition,'') AS varchar(max)) AS computed_definition,CAST(ty.name AS varchar(256)) AS type_name
FROM sys.objects t JOIN sys.schemas s ON s.schema_id=t.schema_id JOIN sys.columns c ON c.object_id=t.object_id JOIN sys.types ty ON ty.user_type_id=c.user_type_id LEFT JOIN sys.default_constraints dc ON dc.object_id=c.default_object_id LEFT JOIN sys.identity_columns ic ON ic.object_id=c.object_id AND ic.column_id=c.column_id LEFT JOIN sys.computed_columns cc ON cc.object_id=c.object_id AND cc.column_id=c.column_id
WHERE t.is_ms_shipped=0 AND t.type IN ('U','V'))SQL" +
		       condition + " ORDER BY s.name,t.name,c.column_id";
	}
	if (category == "constraints") {
		return R"SQL(SELECT CAST(s.name AS varchar(256)) AS schema_name,CAST(t.name AS varchar(256)) AS table_name,CAST(kc.object_id AS varchar(30)) AS constraint_id,CAST(kc.name AS varchar(256)) AS constraint_name,CAST(kc.type AS varchar(5)) AS constraint_type,CAST(ic.key_ordinal AS varchar(20)) AS column_ordinal,CAST(c.name AS varchar(256)) AS column_name,CAST('' AS varchar(256)) AS referenced_schema,CAST('' AS varchar(256)) AS referenced_table,CAST('' AS varchar(256)) AS referenced_column,CAST('' AS varchar(64)) AS update_action,CAST('' AS varchar(64)) AS delete_action,CAST('' AS varchar(max)) AS check_definition,CAST(0 AS varchar(5)) AS is_disabled,CAST(0 AS varchar(5)) AS is_not_trusted,CAST(t.object_id AS varchar(30))+':'+CAST(kc.unique_index_id AS varchar(20)) AS backing_index_id
FROM sys.key_constraints kc JOIN sys.tables t ON t.object_id=kc.parent_object_id JOIN sys.schemas s ON s.schema_id=t.schema_id JOIN sys.index_columns ic ON ic.object_id=t.object_id AND ic.index_id=kc.unique_index_id AND ic.key_ordinal>0 JOIN sys.columns c ON c.object_id=ic.object_id AND c.column_id=ic.column_id WHERE t.is_ms_shipped=0)SQL" +
		       condition + R"SQL(
UNION ALL
			SELECT CAST(s.name AS varchar(256)),CAST(t.name AS varchar(256)),CAST(fk.object_id AS varchar(30)),CAST(fk.name AS varchar(256)),CAST('F' AS varchar(5)),CAST(fkc.constraint_column_id AS varchar(20)),CAST(pc.name AS varchar(256)),CAST(rs.name AS varchar(256)),CAST(rt.name AS varchar(256)),CAST(rc.name AS varchar(256)),CAST(fk.update_referential_action_desc AS varchar(64)),CAST(fk.delete_referential_action_desc AS varchar(64)),CAST('' AS varchar(max)),CAST(fk.is_disabled AS varchar(5)),CAST(fk.is_not_trusted AS varchar(5)),CAST('' AS varchar(64))
FROM sys.foreign_keys fk JOIN sys.tables t ON t.object_id=fk.parent_object_id JOIN sys.schemas s ON s.schema_id=t.schema_id JOIN sys.foreign_key_columns fkc ON fkc.constraint_object_id=fk.object_id JOIN sys.columns pc ON pc.object_id=fkc.parent_object_id AND pc.column_id=fkc.parent_column_id JOIN sys.tables rt ON rt.object_id=fkc.referenced_object_id JOIN sys.schemas rs ON rs.schema_id=rt.schema_id JOIN sys.columns rc ON rc.object_id=fkc.referenced_object_id AND rc.column_id=fkc.referenced_column_id WHERE t.is_ms_shipped=0)SQL" +
		       condition + R"SQL(
UNION ALL
			SELECT CAST(s.name AS varchar(256)),CAST(t.name AS varchar(256)),CAST(ch.object_id AS varchar(30)),CAST(ch.name AS varchar(256)),CAST('C' AS varchar(5)),CASE WHEN ch.parent_column_id=0 THEN '' ELSE '1' END,CAST(COALESCE(c.name,'') AS varchar(256)),CAST('' AS varchar(256)),CAST('' AS varchar(256)),CAST('' AS varchar(256)),CAST('' AS varchar(64)),CAST('' AS varchar(64)),CAST(ch.definition AS varchar(max)),CAST(ch.is_disabled AS varchar(5)),CAST(ch.is_not_trusted AS varchar(5)),CAST('' AS varchar(64))
FROM sys.check_constraints ch JOIN sys.tables t ON t.object_id=ch.parent_object_id JOIN sys.schemas s ON s.schema_id=t.schema_id LEFT JOIN sys.columns c ON c.object_id=ch.parent_object_id AND c.column_id=ch.parent_column_id WHERE t.is_ms_shipped=0)SQL" +
		       condition + " ORDER BY 1,2,3,6";
	}
	if (category == "indexes") {
		return R"SQL(SELECT CAST(s.name AS varchar(256)) AS schema_name,CAST(t.name AS varchar(256)) AS table_name,CAST(i.object_id AS varchar(30))+':'+CAST(i.index_id AS varchar(20)) AS index_id,CAST(i.name AS varchar(256)) AS index_name,CAST(i.type_desc AS varchar(64)) AS index_type,CAST(i.is_unique AS varchar(5)) AS is_unique,CAST(i.is_primary_key AS varchar(5)) AS is_primary_key,CAST(i.is_disabled AS varchar(5)) AS is_disabled,CAST(i.is_hypothetical AS varchar(5)) AS is_hypothetical,CAST(COALESCE(i.filter_definition,'') AS varchar(max)) AS filter_definition,CAST(ic.key_ordinal AS varchar(20)) AS key_ordinal,CAST(ic.index_column_id AS varchar(20)) AS index_column_id,CAST(ic.is_included_column AS varchar(5)) AS is_included_column,CAST(c.name AS varchar(256)) AS column_name,CAST(ic.is_descending_key AS varchar(5)) AS is_descending_key,CAST(i.fill_factor AS varchar(5)) AS fill_factor,CAST(i.is_padded AS varchar(5)) AS is_padded
FROM sys.indexes i JOIN sys.objects t ON t.object_id=i.object_id JOIN sys.schemas s ON s.schema_id=t.schema_id JOIN sys.index_columns ic ON ic.object_id=i.object_id AND ic.index_id=i.index_id JOIN sys.columns c ON c.object_id=ic.object_id AND c.column_id=ic.column_id
WHERE t.is_ms_shipped=0 AND t.type IN ('U','V') AND i.index_id>0)SQL" +
		       condition +
		       " ORDER BY s.name,t.name,i.index_id,CASE WHEN ic.is_included_column=1 THEN 1 ELSE 0 END,CASE WHEN "
		       "ic.key_ordinal=0 THEN ic.index_column_id ELSE ic.key_ordinal END,ic.index_column_id";
	}
	throw InvalidInputException("Unsupported sqlserver_relational_metadata category \"%s\"", category);
}

static unique_ptr<FunctionData> ODBCRelationalMetadataBind(ClientContext &context, TableFunctionBindInput &input,
                                                           vector<LogicalType> &return_types, vector<string> &names) {
	lock_guard<std::mutex> relational_metadata_guard(relational_metadata_mutex);
	if (input.inputs.size() > 4) {
		throw BinderException(
		    "sqlserver_relational_metadata expects catalog, category, schema and optional table scope");
	}
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("Parameters to sqlserver_relational_metadata cannot be NULL");
	}
	auto db_name = input.inputs[0].GetValue<string>();
	auto category = input.inputs[1].GetValue<string>();
	string schema;
	if (input.inputs.size() > 2) {
		if (input.inputs[2].IsNull()) {
			throw BinderException("Parameters to sqlserver_relational_metadata cannot be NULL");
		}
		schema = input.inputs[2].GetValue<string>();
	}
	vector<string> tables;
	if (input.inputs.size() == 4) {
		if (input.inputs[3].IsNull() || schema.empty())
			throw BinderException("SQL Server metadata scope requires schema and table names");
		for (const auto &value : ListValue::GetChildren(input.inputs[3])) {
			if (value.IsNull())
				throw BinderException("SQL Server metadata table scope contains NULL");
			auto name = value.GetValue<string>();
			if (name.empty() || name.find('\0') != string::npos ||
			    std::find(tables.begin(), tables.end(), name) != tables.end())
				throw BinderException("SQL Server metadata table scope contains invalid or duplicate names");
			tables.push_back(std::move(name));
		}
		if (tables.empty() || tables.size() > 128)
			throw BinderException("SQL Server metadata table scope must contain 1 to 128 names");
	}
	auto &db_manager = DatabaseManager::Get(context);
	auto db = db_manager.GetDatabase(context, db_name);
	if (!db) {
		throw BinderException("Failed to find attached database \"%s\" referenced in sqlserver_relational_metadata",
		                      db_name);
	}
	auto &catalog = db->GetCatalog();
	if (catalog.GetCatalogType() != "sqlserver") {
		throw BinderException("Attached database \"%s\" does not refer to a SQL Server database", db_name);
	}
	auto &transaction = ODBCTransaction::Get(context, catalog);
	bool legacy = false;
	if (category != "capabilities") {
		auto version_result =
		    transaction.GetConnection().Query("SELECT CAST(SERVERPROPERTY('ProductVersion') AS varchar(128))",
		                                      ODBCResultStreaming::FORCE_MATERIALIZATION);
		if (!version_result->Next()) {
			throw InvalidInputException("SQL Server product version query returned no rows");
		}
		auto version = version_result->GetString(0);
		try {
			legacy = std::stoi(version.substr(0, version.find('.'))) < 10;
		} catch (...) {
			throw InvalidInputException("Unable to parse SQL Server product version \"%s\"", version);
		}
	}
	auto sql = SQLServerRelationalMetadataSQL(category, schema, legacy, tables);
	// The ODBC timeout expires before FQE's outer metadata deadline. This is
	// essential: asynchronously interrupting a driver call can leave DuckDB's
	// bind state alive after its native resources have been released.
	auto result = transaction.GetConnection().QueryWithTimeout(sql, ODBCResultStreaming::FORCE_MATERIALIZATION, 20);
	for (auto &field : result->Fields()) {
		names.push_back(field.name);
		return_types.push_back(field.duckdb_type);
	}
	if (return_types.empty()) {
		throw InvalidInputException("sqlserver_relational_metadata returned no columns for category \"%s\"", category);
	}
	return make_uniq<ODBCQueryBindData>(catalog, std::move(result), std::move(sql));
}

ODBCRelationalMetadataFunction::ODBCRelationalMetadataFunction()
    : TableFunction("sqlserver_relational_metadata", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ODBCScan,
                    ODBCRelationalMetadataBind, ODBCQueryInitGlobalState, ODBCInitLocalState) {
	serialize = ODBCScanSerialize;
	deserialize = ODBCScanDeserialize;
}

} // namespace duckdb
