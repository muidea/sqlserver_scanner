//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlserver_guarded_crud.cpp
//
// Restricted SQL Server mutations on an isolated scanner-owned session.
//===----------------------------------------------------------------------===//

#include "duckdb.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "storage/odbc_catalog.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <codecvt>
#include <locale>
#include <mutex>
#include <thread>

namespace duckdb {
namespace {
static void Reject() {
	throw InvalidInputException("guarded SQL Server operation rejected");
}
static string JSON(const string &s) {
	string out = "\"";
	for (auto c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 32)
				Reject();
			out += c;
		}
	}
	return out + "\"";
}
static bool SimpleName(const string &s) {
	if (s.empty() || s.size() > 128)
		return false;
	for (auto c : s)
		if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '_')
			return false;
	return true;
}
static string Identifier(const string &s) {
	string out = "[";
	for (auto c : s)
		out += c == ']' ? string("]]") : string(1, c);
	return out + "]";
}
static string Literal(const string &s) {
	string out = "N'";
	for (auto c : s)
		out += c == '\'' ? string("''") : string(1, c);
	return out + "'";
}

// One stable statement handle lives until the cancellation watcher is joined.
// Never free/reassign a handle while another thread can call SQLCancelHandle.
class Session {
public:
	Session(ClientContext &ctx_p, SQLHDBC dbc) : ctx(ctx_p), stmt(nullptr, ODBCStatementDelete) {
		SQLHSTMT h = nullptr;
		if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &h) != SQL_SUCCESS)
			Reject();
		stmt = ODBCStatementPtr(h, ODBCStatementDelete);
		if (SQLSetStmtAttr(h, SQL_ATTR_QUERY_TIMEOUT, reinterpret_cast<SQLPOINTER>(5), 0) != SQL_SUCCESS)
			Reject();
		watcher = std::thread([this] {
			std::unique_lock<std::mutex> lock(mu);
			auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!stopped) {
				if (ctx.IsInterrupted() || std::chrono::steady_clock::now() >= end) {
					cancelled.store(true);
					SQLCancelHandle(SQL_HANDLE_STMT, stmt.get());
					break;
				}
				cv.wait_for(lock, std::chrono::milliseconds(25), [this] { return stopped; });
			}
		});
	}
	~Session() {
		{
			std::lock_guard<std::mutex> lock(mu);
			stopped = true;
		}
		cv.notify_all();
		if (watcher.joinable())
			watcher.join();
	}
	string error;
	void Check(SQLRETURN rc) {
		if (rc == SQL_SUCCESS)
			return;
		SQLCHAR state[6] = {0};
		SQLINTEGER native = 0;
		SQLSMALLINT len = 0;
		SQLGetDiagRec(SQL_HANDLE_STMT, stmt.get(), 1, state, &native, nullptr, 0, &len);
		switch (native) {
		case 2601:
		case 2627:
			error = "23505";
			break;
		case 515:
			error = "23502";
			break;
		case 8152:
		case 2628:
			error = "22001";
			break;
		case 8115:
			error = "22003";
			break;
		case 1205:
			error = "40P01";
			break;
		case 1222:
			error = "55P03";
			break;
		case 229:
			error = "42501";
			break;
		default:
			if (cancelled.load() || ctx.IsInterrupted())
				error = "57014";
			else if (rc == SQL_SUCCESS_WITH_INFO)
				error = "01000";
			else
				error = state[0] ? string(reinterpret_cast<char *>(state)) : "08006";
		}
		Reject();
	}
	void Start(bool cleanup = false) {
		if (!cleanup && (cancelled.load() || ctx.IsInterrupted())) {
			error = "57014";
			Reject();
		}
		Check(SQLFreeStmt(stmt.get(), SQL_CLOSE));
		Check(SQLFreeStmt(stmt.get(), SQL_UNBIND));
		Check(SQLFreeStmt(stmt.get(), SQL_RESET_PARAMS));
	}
	void Exec(const string &sql, bool cleanup = false) {
		Start(cleanup);
		auto rc = SQLExecDirect(stmt.get(), reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS);
		if (rc == SQL_NO_DATA)
			return;
		Check(rc);
		while ((rc = SQLMoreResults(stmt.get())) != SQL_NO_DATA)
			Check(rc);
	}
	vector<vector<string>> Rows(const string &sql, bool cleanup = false) {
		Start(cleanup);
		Check(SQLExecDirect(stmt.get(), reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS));
		SQLSMALLINT cols = 0;
		Check(SQLNumResultCols(stmt.get(), &cols));
		if (cols < 1 || cols > 64)
			Reject();
		vector<vector<string>> rows;
		idx_t bytes = 0;
		SQLRETURN rc;
		while ((rc = SQLFetch(stmt.get())) != SQL_NO_DATA) {
			Check(rc);
			if (rows.size() >= 8192)
				Reject();
			vector<string> row;
			for (SQLUSMALLINT col = 1; col <= cols; col++) {
				char value[4096] = {0};
				SQLLEN len = 0;
				Check(SQLGetData(stmt.get(), col, SQL_C_CHAR, value, sizeof(value), &len));
				if (len == SQL_NULL_DATA || len < 0 || len >= sizeof(value))
					Reject();
				row.emplace_back(value, static_cast<size_t>(len));
				bytes += row.back().size();
				if (bytes > 1024 * 1024)
					Reject();
			}
			rows.push_back(std::move(row));
		}
		// Metadata SELECTs must have exactly one result, fully consumed.
		rc = SQLMoreResults(stmt.get());
		if (rc != SQL_NO_DATA) {
			Check(rc);
			Reject();
		}
		return rows;
	}
	string Scalar(const string &sql, bool cleanup = false) {
		auto r = Rows(sql, cleanup);
		if (r.size() != 1 || r[0].size() != 1)
			Reject();
		return r[0][0];
	}
	int64_t Execute(const string &sql, const vector<Value> &params, bool &sent) {
		Start();
		Check(SQLPrepare(stmt.get(), reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS));
		SQLSMALLINT count = 0;
		Check(SQLNumParams(stmt.get(), &count));
		if (count != params.size() || count > 2100)
			Reject();
		vector<int64_t> values(params.size());
		vector<vector<SQLWCHAR>> strings(params.size());
		vector<SQLLEN> lengths(params.size(), 0);
		for (idx_t i = 0; i < params.size(); i++) {
			auto &v = params[i];
			// Keep Unicode/DECIMAL/finite ISO payloads exact. Generated SQL
			// supplies the source type CAST; neither float nor ANSI code-page
			// conversion may change a value before the guarded statement runs.
			if (v.type().id() == LogicalTypeId::VARCHAR) {
				idx_t units = 0;
				if (v.IsNull()) {
					lengths[i] = SQL_NULL_DATA;
				} else {
					auto text = v.GetValue<string>();
					if (text.size() > 16000 || text.find('\0') != string::npos)
						Reject();
					std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
					auto utf16 = converter.from_bytes(text);
					units = utf16.size();
					if (units > 4000)
						Reject();
					strings[i].assign(utf16.begin(), utf16.end());
					lengths[i] = units * sizeof(SQLWCHAR);
				}
				static_assert(sizeof(SQLWCHAR) == 2, "guarded SQL Server Unicode requires 16-bit ODBC units");
				strings[i].push_back(0);
				Check(SQLBindParameter(stmt.get(), i + 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
				                       MaxValue<idx_t>(1, units), 0, strings[i].data(),
				                       strings[i].size() * sizeof(SQLWCHAR), &lengths[i]));
				continue;
			}
			if (v.IsNull())
				lengths[i] = SQL_NULL_DATA;
			else {
				switch (v.type().id()) {
				case LogicalTypeId::SMALLINT:
					values[i] = v.GetValue<int16_t>();
					break;
				case LogicalTypeId::INTEGER:
					values[i] = v.GetValue<int32_t>();
					break;
				case LogicalTypeId::BIGINT:
					values[i] = v.GetValue<int64_t>();
					break;
				default:
					Reject();
				}
			}
			Check(SQLBindParameter(stmt.get(), i + 1, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 19, 0, &values[i],
			                       sizeof(int64_t), &lengths[i]));
		}
		if (cancelled.load() || ctx.IsInterrupted()) {
			error = "57014";
			Reject();
		}
		sent = true;
		auto rc = SQLExecute(stmt.get());
		if (rc != SQL_NO_DATA)
			Check(rc);
		SQLSMALLINT cols = 0;
		Check(SQLNumResultCols(stmt.get(), &cols));
		if (cols != 0)
			Reject();
		SQLLEN affected = -1;
		Check(SQLRowCount(stmt.get(), &affected));
		if (affected < 0)
			Reject();
		rc = SQLMoreResults(stmt.get());
		if (rc != SQL_NO_DATA) {
			Check(rc);
			Reject();
		}
		return affected;
	}

private:
	ClientContext &ctx;
	ODBCStatementPtr stmt;
	std::mutex mu;
	std::condition_variable cv;
	bool stopped = false;
	std::atomic<bool> cancelled {false};
	std::thread watcher;
};
struct Data : TableFunctionData {
	ODBCCatalog &catalog;
	string schema, table, expected, version, sql;
	vector<Value> params;
	bool execute;
	Data(ODBCCatalog &c, bool exec) : catalog(c), execute(exec) {
	}
};
struct State : GlobalTableFunctionState {
	bool done = false;
};
static unique_ptr<GlobalTableFunctionState> Init(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<State>();
}
static unique_ptr<FunctionData> Bind(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                     vector<string> &names) {
	for (auto &v : in.inputs)
		if (v.IsNull())
			Reject();
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, in.inputs[0].GetValue<string>());
	if (!db || db->GetCatalog().GetCatalogType() != "sqlserver")
		Reject();
	auto d = make_uniq<Data>(db->GetCatalog().Cast<ODBCCatalog>(), in.inputs.size() == 6);
	d->schema = in.inputs[1].GetValue<string>();
	d->table = in.inputs[2].GetValue<string>();
	if (!SimpleName(d->schema) || !SimpleName(d->table))
		Reject();
	if (d->execute) {
		d->expected = in.inputs[3].GetValue<string>();
		d->version = in.inputs[4].GetValue<string>();
		d->sql = in.inputs[5].GetValue<string>();
		if (d->expected.empty() || d->expected.size() > 1024 * 1024 || d->sql.empty() ||
		    d->sql.size() > 4 * 1024 * 1024)
			Reject();
		auto it = in.named_parameters.find("params");
		if (it != in.named_parameters.end()) {
			if (it->second.IsNull() || it->second.type().id() != LogicalTypeId::STRUCT)
				Reject();
			d->params = StructValue::GetChildren(it->second);
		}
		if (d->params.size() > 2100)
			Reject();
	}
	names = {"status", "affected_rows", "sqlstate", "stage", "version", "descriptor"};
	types = {LogicalType::VARCHAR, LogicalType::BIGINT,  LogicalType::VARCHAR,
	         LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(d);
}
static string JSONRows(const vector<vector<string>> &rows) {
	string out = "[";
	bool first = true;
	for (auto &row : rows) {
		if (!first)
			out += ",";
		first = false;
		out += "[";
		bool cell = true;
		for (auto &s : row) {
			if (!cell)
				out += ",";
			cell = false;
			out += JSON(s);
		}
		out += "]";
		if (out.size() > 1024 * 1024)
			Reject();
	}
	return out + "]";
}
static string Metadata(Session &s, const Data &d) {
	const auto target = Identifier(d.schema) + "." + Identifier(d.table);
	const auto obj = "OBJECT_ID(" + Literal(target) + ",N'U')";
	if (s.Scalar("SELECT CONVERT(varchar(1),HAS_PERMS_BY_NAME(DB_NAME(),'DATABASE','VIEW DEFINITION'))") != "1")
		Reject();
	auto table = s.Rows(
	    "SELECT "
	    "CONVERT(varchar(20),t.object_id),CONVERT(varchar(33),t.create_date,126),CONVERT(varchar(33),t.modify_date,126)"
	    ",CONVERT(varchar(20),DB_ID()),CONVERT(varchar(20),t.lock_escalation) FROM sys.tables t WHERE t.object_id=" +
	    obj +
	    " AND t.is_ms_shipped=0 AND t.is_memory_optimized=0 AND t.temporal_type=0 AND t.is_filetable=0 AND "
	    "t.is_remote_data_archive_enabled=0");
	if (table.size() != 1)
		Reject();
	// TABLOCKX + HOLDLOCK is retained through COMMIT. Verify the actual granted
	// lock, including for an empty table, instead of inferring it from a SELECT.
	if (s.Scalar("SELECT CONVERT(varchar(20),COUNT_BIG(*)) FROM sys.dm_tran_locks WHERE resource_type='OBJECT' AND "
	             "resource_database_id=DB_ID() AND resource_associated_entity_id=" +
	             obj + " AND request_session_id=@@SPID AND request_status='GRANT' AND request_mode='X'") == "0")
		Reject();
	auto features = s.Scalar(
	    "SELECT CONVERT(varchar(20),(SELECT COUNT_BIG(*) FROM sys.triggers WHERE parent_id=" + obj +
	    ")+(SELECT COUNT_BIG(*) FROM sys.foreign_keys WHERE parent_object_id=" + obj + " OR referenced_object_id=" +
	    obj + ")+(SELECT COUNT_BIG(*) FROM sys.check_constraints WHERE parent_object_id=" + obj +
	    ")+(SELECT COUNT_BIG(*) FROM sys.security_predicates WHERE target_object_id=" + obj +
	    ")+(SELECT COUNT_BIG(*) FROM sys.external_tables WHERE object_id=" + obj +
	    ")+(SELECT COUNT_BIG(*) FROM sys.partitions WHERE object_id=" + obj +
	    " AND partition_number>1)+(SELECT COUNT_BIG(*) FROM sys.indexes WHERE object_id=" + obj +
	    " AND (type NOT IN(0,1,2) OR has_filter=1 OR ignore_dup_key=1 OR is_disabled=1 OR is_hypothetical=1)))");
	if (features != "0")
		Reject();
	auto cols =
	    s.Rows("SELECT "
	           "c.name,TYPE_NAME(c.system_type_id),CONVERT(varchar(1),c.is_nullable),CONVERT(varchar(20),c.column_id),"
	           "CONVERT(varchar(20),c.max_length),CONVERT(varchar(20),c.[precision]),CONVERT(varchar(20),c.[scale]),"
	           "COALESCE(c.collation_name,'') FROM "
	           "sys.columns c WHERE c.object_id=" +
	           obj +
	           " AND c.system_type_id=c.user_type_id AND ((c.system_type_id IN(52,56,127,106,108,40,231)) OR "
	           "(c.system_type_id=42 "
	           "AND c.scale<=6)) AND c.is_identity=0 AND "
	           "c.is_computed=0 AND c.is_rowguidcol=0 AND c.is_filestream=0 AND c.is_sparse=0 AND c.is_column_set=0 "
	           "AND c.is_hidden=0 AND c.generated_always_type=0 AND c.encryption_type IS NULL AND "
	           "c.default_object_id=0 AND c.rule_object_id=0 AND NOT EXISTS(SELECT 1 FROM sys.masked_columns m WHERE "
	           "m.object_id=c.object_id AND m.column_id=c.column_id AND m.is_masked=1) ORDER BY c.column_id");
	auto count = s.Scalar("SELECT CONVERT(varchar(20),COUNT_BIG(*)) FROM sys.columns WHERE object_id=" + obj);
	if (cols.empty() || cols.size() > 1024 || count != std::to_string(cols.size()))
		Reject();
	auto indexes = s.Rows("SELECT "
	                      "CONVERT(varchar(20),index_id),COALESCE(name,''),CONVERT(varchar(20),type),CONVERT(varchar(1)"
	                      ",is_unique),CONVERT(varchar(1),is_primary_key),CONVERT(varchar(1),is_unique_constraint),"
	                      "CONVERT(varchar(20),data_space_id) FROM sys.indexes WHERE object_id=" +
	                      obj + " ORDER BY index_id");
	auto keys = s.Rows("SELECT "
	                   "CONVERT(varchar(20),index_id),CONVERT(varchar(20),index_column_id),CONVERT(varchar(20),column_"
	                   "id),CONVERT(varchar(20),key_ordinal),CONVERT(varchar(1),is_descending_key),CONVERT(varchar(1),"
	                   "is_included_column) FROM sys.index_columns WHERE object_id=" +
	                   obj + " ORDER BY index_id,index_column_id");
	auto constraints = s.Rows("SELECT "
	                          "CONVERT(varchar(20),object_id),name,type,CONVERT(varchar(33),create_date,126),CONVERT("
	                          "varchar(33),modify_date,126) FROM sys.objects WHERE parent_object_id=" +
	                          obj + " ORDER BY object_id");
	auto revision = JSONRows(table) + JSONRows(cols) + JSONRows(indexes) + JSONRows(keys) + JSONRows(constraints);
	string out = "{\"database\":" + JSON(s.Scalar("SELECT DB_NAME()")) + ",\"schema\":" + JSON(d.schema) +
	             ",\"table\":" + JSON(d.table) + ",\"object_id\":" + JSON(table[0][0]) +
	             ",\"revision\":" + JSON(revision) + ",\"columns\":[";
	bool first = true;
	for (auto &c : cols) {
		if (!SimpleName(c[0]))
			Reject();
		if (!first)
			out += ",";
		first = false;
		out += "{\"name\":" + JSON(c[0]) + ",\"data_type\":" + JSON(c[1]) +
		       ",\"nullable\":" + (c[2] == "1" ? "true" : "false");
		if (c[1] == "decimal" || c[1] == "numeric" || c[1] == "datetime2") {
			if (c[5].empty() || c[6].empty() || c[5].find_first_not_of("0123456789") != string::npos ||
			    c[6].find_first_not_of("0123456789") != string::npos)
				Reject();
			out += ",\"precision\":" + c[5] + ",\"scale\":" + c[6];
		}
		if (c[1] == "nvarchar") {
			if (c[4].empty() || c[4].find_first_not_of("-0123456789") != string::npos || c[7].empty())
				Reject();
			out += ",\"max_length\":" + c[4] + ",\"collation\":" + JSON(c[7]);
		}
		out += "}";
	}
	out += "]}";
	if (out.size() > 1024 * 1024)
		Reject();
	return out;
}
// Version is an opaque source identity. Capability is proved by the live
// session/catalog checks below, never by a fixed release interval.
static bool CompatibleVersion(const string &v) {
	if (v.empty() || v.size() > 1024 || StringUtil::CharacterIsSpace(v.front()) ||
	    StringUtil::CharacterIsSpace(v.back()))
		return false;
	for (auto c : v)
		if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) == 127)
			return false;
	return true;
}

static void Scan(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
	auto &state = in.global_state->Cast<State>();
	if (state.done)
		return;
	state.done = true;
	auto &d = in.bind_data->Cast<Data>();
	string status = "CONFIRMED_NOT_EXECUTED", error, stage = "source_metadata", version, descriptor;
	int64_t affected = -1;
	bool sent = false, commit_started = false;
	ODBCPoolConnection pooled;
	shared_ptr<ODBCConnection> connection;
	unique_ptr<Session> session;
	try {
		if (ctx.IsInterrupted())
			throw InterruptException();
		if (!d.catalog.GetConnectionPool().TryGetConnection(pooled))
			Reject();
		connection = pooled.GetConnection();
		session = make_uniq<Session>(ctx, connection->GetConn());
		auto &s = *session;
		if (s.Scalar("SELECT CONVERT(varchar(20),@@TRANCOUNT)") != "0")
			Reject();
		if (s.Scalar("SELECT CASE WHEN @@VERSION LIKE 'Microsoft SQL Server%' AND "
		             "CONVERT(int,SERVERPROPERTY('EngineEdition')) IN(2,3,4) THEN '1' ELSE '0' END") != "1")
			Reject();
		version = s.Scalar("SELECT CONVERT(varchar(128),SERVERPROPERTY('ProductVersion'))");
		if (!CompatibleVersion(version) || (d.execute && version != d.version))
			Reject();
		s.Exec("SET XACT_ABORT ON;SET NOCOUNT OFF;SET ANSI_WARNINGS ON;SET ANSI_NULLS ON;SET QUOTED_IDENTIFIER ON;SET "
		       "ARITHABORT ON;SET NUMERIC_ROUNDABORT OFF;SET LOCK_TIMEOUT 5000;SET TRANSACTION ISOLATION LEVEL "
		       "SERIALIZABLE;BEGIN TRANSACTION");
		s.Rows("SELECT TOP(1) CONVERT(varchar(1),1) FROM " + Identifier(d.schema) + "." + Identifier(d.table) +
		       " WITH(TABLOCKX,HOLDLOCK)");
		descriptor = Metadata(s, d);
		if (d.execute) {
			stage = "source_descriptor";
			if (descriptor != d.expected) {
				error = "0A000";
				Reject();
			}
			stage = "source_execute";
			affected = s.Execute(d.sql, d.params, sent);
			if (s.Scalar("SELECT CASE WHEN @@TRANCOUNT=1 AND XACT_STATE()=1 THEN '1' ELSE '0' END") != "1")
				Reject();
			stage = "source_commit";
			commit_started = true;
			status = "COMMIT_STATUS_UNKNOWN";
			s.Exec("COMMIT TRANSACTION");
			if (s.Scalar("SELECT CASE WHEN @@TRANCOUNT=0 AND XACT_STATE()=0 THEN '1' ELSE '0' END") != "1")
				Reject();
			status = "COMMITTED";
			stage.clear();
		} else {
			s.Exec("ROLLBACK TRANSACTION");
			if (s.Scalar("SELECT CASE WHEN @@TRANCOUNT=0 AND XACT_STATE()=0 THEN '1' ELSE '0' END") != "1")
				Reject();
			stage.clear();
		}
	} catch (...) {
		if (error.empty())
			error = session && !session->error.empty() ? session->error : (ctx.IsInterrupted() ? "57014" : "08006");
		if (session) {
			try {
				session->Exec("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION", true);
				if (session->Scalar("SELECT CASE WHEN @@TRANCOUNT=0 AND XACT_STATE()=0 THEN '1' ELSE '0' END", true) !=
				    "1")
					Reject();
				if (sent && !commit_started)
					status = "CONFIRMED_ROLLED_BACK";
			} catch (...) {
				if (sent)
					status = "COMMIT_STATUS_UNKNOWN";
			}
		}
	}
	session.reset(); // join cancellation thread before disconnecting this session
	if (connection)
		connection->Close(); // isolated state is never returned for reuse
	out.SetCardinality(1);
	out.SetValue(0, 0, Value(status));
	out.SetValue(1, 0, Value::BIGINT(affected));
	out.SetValue(2, 0, Value(error));
	out.SetValue(3, 0, Value(stage));
	out.SetValue(4, 0, Value(version));
	out.SetValue(5, 0, Value(descriptor));
}

// Administrative DuckDB callers use an explicitly writable attachment.
// fedQuery attaches SQL Server read-only and denies this function publicly.
struct AdminData : TableFunctionData {
	ODBCCatalog &catalog;
	string sql;
	AdminData(ODBCCatalog &c, string q) : catalog(c), sql(std::move(q)) {
	}
};
static unique_ptr<FunctionData> AdminBind(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                          vector<string> &names) {
	if (in.inputs[0].IsNull() || in.inputs[1].IsNull())
		Reject();
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, in.inputs[0].GetValue<string>());
	if (!db || db->GetCatalog().GetCatalogType() != "sqlserver")
		Reject();
	auto &catalog = db->GetCatalog().Cast<ODBCCatalog>();
	auto sql = in.inputs[1].GetValue<string>();
	if (catalog.access_mode != AccessMode::READ_WRITE || sql.empty() || sql.size() > 4 * 1024 * 1024)
		Reject();
	types = {LogicalType::BOOLEAN};
	names = {"success"};
	return make_uniq<AdminData>(catalog, std::move(sql));
}
static void AdminScan(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
	auto &state = in.global_state->Cast<State>();
	if (state.done)
		return;
	state.done = true;
	auto &d = in.bind_data->Cast<AdminData>();
	ODBCPoolConnection pooled;
	if (!d.catalog.GetConnectionPool().TryGetConnection(pooled))
		Reject();
	auto connection = pooled.GetConnection();
	try {
		Session session(ctx, connection->GetConn());
		session.Exec(d.sql);
	} catch (...) {
		connection->Close();
		Reject();
	}
	connection->Close();
	out.SetCardinality(1);
	out.SetValue(0, 0, Value::BOOLEAN(true));
}
} // namespace
void RegisterSQLServerGuardedCRUD(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    TableFunction("sqlserver_execute", {LogicalType::VARCHAR, LogicalType::VARCHAR}, AdminScan, AdminBind, Init));
	loader.RegisterFunction(
	    TableFunction("sqlserver_crud_metadata_v1", vector<LogicalType>(3, LogicalType::VARCHAR), Scan, Bind, Init));
	TableFunction exec("sqlserver_guarded_mutation_v1", vector<LogicalType>(6, LogicalType::VARCHAR), Scan, Bind, Init);
	exec.named_parameters["params"] = LogicalType::ANY;
	loader.RegisterFunction(exec);
}
} // namespace duckdb
