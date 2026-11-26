//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlserver_guarded_ddl.cpp
//
// Guarded SQL Server capability observation, catalog description, and
// transactional core-subset DDL execution on an isolated session.
//===----------------------------------------------------------------------===//

#include "duckdb.hpp"
#include "guarded_odbc_source_error.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "storage/odbc_catalog.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <codecvt>
#include <condition_variable>
#include <locale>
#include <mutex>
#include <thread>

namespace duckdb {
namespace {
class DDLRejectException : public std::runtime_error {
public:
	string sqlstate;
	string stage;
	DDLRejectException(string state_p, string stage_p, const string &msg)
	    : std::runtime_error(msg), sqlstate(std::move(state_p)), stage(std::move(stage_p)) {
	}
};

[[noreturn]] static void RejectDDL(const string &sqlstate = "0A000", const string &stage = "source_identity",
                                   const string &msg = "guarded SQL Server DDL rejected") {
	throw DDLRejectException(sqlstate, stage, msg);
}

[[noreturn]] static void RejectLocal() {
	throw GuardedODBCLocalRejection("guarded SQL Server DDL input invalid");
}

static string JSON(const string &s) {
	const char *hex = "0123456789abcdef";
	string out = "\"";
	for (unsigned char c : s) {
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
			if (c < 32) {
				out += "\\u00";
				out += hex[c >> 4];
				out += hex[c & 15];
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	return out + "\"";
}

static bool ValidName(const string &s) {
	return !s.empty() && s.size() <= 63 && s.find('\0') == string::npos;
}

static bool ValidUUID(const string &s) {
	if (s.size() != 36) {
		return false;
	}
	for (idx_t i = 0; i < s.size(); i++) {
		char c = s[i];
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-') {
				return false;
			}
		} else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

static string Identifier(const string &s) {
	string out = "[";
	for (char c : s) {
		if (c == '\0') {
			RejectLocal();
		}
		out += c == ']' ? string("]]") : string(1, c);
	}
	return out + "]";
}

static string SafeSQLServerCollation(const string &s) {
	if (s.empty() || s.size() > 128) {
		RejectDDL("0A000", "source_definition", "invalid SQL Server collation name");
	}
	for (char c : s) {
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) {
			RejectDDL("0A000", "source_definition", "invalid SQL Server collation name");
		}
	}
	return s;
}

static string Literal(const string &s) {
	string out = "N'";
	for (char c : s) {
		if (c == '\0') {
			RejectLocal();
		}
		out += c == '\'' ? string("''") : string(1, c);
	}
	return out + "'";
}

static string TextVal(const Value &v) {
	if (v.IsNull() || v.type().id() != LogicalTypeId::VARCHAR) {
		RejectLocal();
	}
	auto s = v.GetValue<string>();
	if (s.find('\0') != string::npos) {
		RejectLocal();
	}
	return s;
}

static int32_t IntVal(const Value &v) {
	if (v.IsNull() || v.type().id() != LogicalTypeId::INTEGER) {
		RejectLocal();
	}
	return v.GetValue<int32_t>();
}

static bool BoolVal(const Value &v) {
	if (v.IsNull() || v.type().id() != LogicalTypeId::BOOLEAN) {
		RejectLocal();
	}
	return v.GetValue<bool>();
}

static bool IsSignedInt64Literal(const string &s) {
	if (s.empty() || s == "-") {
		return false;
	}
	for (idx_t i = 0; i < s.size(); i++) {
		char c = s[i];
		if (!((c >= '0' && c <= '9') || (i == 0 && c == '-'))) {
			return false;
		}
	}
	try {
		size_t pos = 0;
		long long val = std::stoll(s, &pos, 10);
		return pos == s.size() && std::to_string(val) == s;
	} catch (...) {
		return false;
	}
}

static string ValidSQLState(const string &candidate, const string &fallback) {
	if (candidate.size() == 5) {
		bool ok = true;
		for (char c : candidate) {
			if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
				ok = false;
				break;
			}
		}
		if (ok) {
			return candidate;
		}
	}
	return fallback;
}

static string MapSQLServerSQLState(SQLINTEGER native, const char *odbc_state) {
	switch (native) {
	case 1222:
		return "55P03";
	case 1205:
		return "40P01";
	case 515:
	case 4901:
		return "23502";
	case 547:
		return "23514";
	case 2601:
	case 2627:
	case 1505:
		return "23505";
	case 3726:
	case 4712:
	case 5074:
	case 4902:
	case 4922:
	case 5072:
	case 3723:
		return "2BP01";
	case 4923:
		return "42P16";
	case 2714:
	case 1913:
	case 1779:
	case 1781:
	case 2705:
		return "42P07";
	case 3701:
	case 3728:
	case 15248:
	case 15335:
		return "42704";
	case 208:
	case 1088:
		return "42P01";
	case 207:
	case 4924:
		return "42703";
	case 229:
	case 262:
	case 15247:
		return "42501";
	default:
		break;
	}
	if (odbc_state) {
		return ValidSQLState(odbc_state, "HY000");
	}
	return "HY000";
}

class DDLSession {
public:
	DDLSession(ClientContext &ctx_p, SQLHDBC dbc, uint32_t query_timeout_sec = 15)
	    : ctx(ctx_p), stmt(nullptr, ODBCStatementDelete) {
		SQLHSTMT h = nullptr;
		if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &h) != SQL_SUCCESS) {
			RejectDDL("08006", "source_identity", "failed to allocate SQL Server statement");
		}
		stmt = ODBCStatementPtr(h, ODBCStatementDelete);
		if (SQLSetStmtAttr(h, SQL_ATTR_QUERY_TIMEOUT,
		                   reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(query_timeout_sec)), 0) !=
		    SQL_SUCCESS) {
			RejectDDL("08006", "source_identity", "failed to configure statement timeout");
		}
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
	~DDLSession() {
		{
			std::lock_guard<std::mutex> lock(mu);
			stopped = true;
		}
		cv.notify_all();
		if (watcher.joinable()) {
			watcher.join();
		}
	}

	string error;
	SQLINTEGER last_native = 0;
	int64_t last_warning_count = 0;
	GuardedODBCSourceError source;

	void Check(SQLRETURN rc, bool ignore_sp_rename_info = false) {
		if (rc == SQL_SUCCESS) {
			return;
		}
		if (rc == SQL_SUCCESS_WITH_INFO && ignore_sp_rename_info) {
			bool only_benign_15477 = true;
			SQLINTEGER non_benign_count = 0;
			for (SQLSMALLINT rec = 1; rec <= 32; rec++) {
				SQLCHAR state[6] = {0};
				SQLINTEGER native = 0;
				SQLSMALLINT len = 0;
				auto diag_rc = SQLGetDiagRec(SQL_HANDLE_STMT, stmt.get(), rec, state, &native, nullptr, 0, &len);
				if (diag_rc == SQL_NO_DATA) {
					break;
				}
				if (!SQL_SUCCEEDED(diag_rc)) {
					only_benign_15477 = false;
					break;
				}
				if (native != 15477) {
					only_benign_15477 = false;
					non_benign_count++;
				}
			}
			if (only_benign_15477) {
				return;
			}
			last_warning_count += non_benign_count;
			return;
		}
		if (rc == SQL_SUCCESS_WITH_INFO) {
			for (SQLSMALLINT rec = 1; rec <= 32; rec++) {
				SQLCHAR state[6] = {0};
				SQLINTEGER native = 0;
				SQLSMALLINT len = 0;
				if (SQLGetDiagRec(SQL_HANDLE_STMT, stmt.get(), rec, state, &native, nullptr, 0, &len) == SQL_NO_DATA) {
					break;
				}
				last_warning_count++;
			}
			return;
		}
		source = ReadGuardedODBCSourceError(SQL_HANDLE_STMT, stmt.get(), "sqlserver");
		SQLCHAR state[6] = {0};
		SQLINTEGER native = 0;
		SQLSMALLINT len = 0;
		SQLGetDiagRec(SQL_HANDLE_STMT, stmt.get(), 1, state, &native, nullptr, 0, &len);
		last_native = native;
		if (cancelled.load() || ctx.IsInterrupted()) {
			error = "57014";
		} else {
			error = MapSQLServerSQLState(native, reinterpret_cast<const char *>(state));
		}
		RejectDDL(error, current_stage, source.message.empty() ? "SQL Server DDL error" : source.message);
	}

	string current_stage = "source_identity";

	void Start(bool cleanup = false) {
		if (!cleanup && (cancelled.load() || ctx.IsInterrupted())) {
			RejectDDL("57014", current_stage, "operation cancelled");
		}
		auto rc1 = SQLFreeStmt(stmt.get(), SQL_CLOSE);
		if (!cleanup) Check(rc1);
		auto rc2 = SQLFreeStmt(stmt.get(), SQL_UNBIND);
		if (!cleanup) Check(rc2);
		auto rc3 = SQLFreeStmt(stmt.get(), SQL_RESET_PARAMS);
		if (!cleanup) Check(rc3);
	}

	void Exec(const string &sql, bool cleanup = false, bool ignore_sp_rename_info = false) {
		Start(cleanup);
		auto rc = SQLExecDirect(stmt.get(), reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS);
		if (cleanup) {
			return;
		}
		if (rc == SQL_NO_DATA) {
			return;
		}
		Check(rc, ignore_sp_rename_info);
		while ((rc = SQLMoreResults(stmt.get())) != SQL_NO_DATA) {
			if (!SQL_SUCCEEDED(rc)) {
				Check(rc, ignore_sp_rename_info);
			}
			if (rc == SQL_SUCCESS_WITH_INFO) {
				Check(rc, ignore_sp_rename_info);
			}
		}
	}

	vector<vector<string>> Rows(const string &sql, bool allow_null = true) {
		Start(false);
		auto rc = SQLExecDirect(stmt.get(), reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS);
		Check(rc);
		SQLSMALLINT cols = 0;
		Check(SQLNumResultCols(stmt.get(), &cols));
		if (cols < 1 || cols > 64) {
			RejectDDL("XX000", current_stage, "unexpected column count in SQL Server query");
		}
		vector<vector<string>> rows;
		idx_t total_bytes = 0;
		while ((rc = SQLFetch(stmt.get())) != SQL_NO_DATA) {
			Check(rc);
			if (rows.size() >= 8192) {
				RejectDDL("54000", current_stage, "SQL Server metadata row bound exceeded");
			}
			vector<string> row;
			for (SQLUSMALLINT col = 1; col <= cols; col++) {
				char buf[8192] = {0};
				SQLLEN ind = 0;
				auto get_rc = SQLGetData(stmt.get(), col, SQL_C_CHAR, buf, sizeof(buf), &ind);
				if (get_rc != SQL_SUCCESS) {
					Check(get_rc);
				}
				if (ind == SQL_NULL_DATA) {
					if (!allow_null) {
						RejectDDL("XX000", current_stage, "unexpected NULL metadata value");
					}
					row.emplace_back("");
				} else if (ind < 0 || static_cast<idx_t>(ind) >= sizeof(buf)) {
					RejectDDL("54000", current_stage, "SQL Server metadata cell bound exceeded");
				} else {
					row.emplace_back(buf, static_cast<size_t>(ind));
				}
				total_bytes += row.back().size();
				if (total_bytes > 1024 * 1024) {
					RejectDDL("54000", current_stage, "SQL Server metadata document bound exceeded");
				}
			}
			rows.push_back(std::move(row));
		}
		while ((rc = SQLMoreResults(stmt.get())) != SQL_NO_DATA) {
			Check(rc);
		}
		return rows;
	}

	string Scalar(const string &sql, bool cleanup = false) {
		if (cleanup) {
			Start(true);
			if (!SQL_SUCCEEDED(
			        SQLExecDirect(stmt.get(), reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS))) {
				return "";
			}
			if (!SQL_SUCCEEDED(SQLFetch(stmt.get()))) {
				return "";
			}
			char buf[256] = {0};
			SQLLEN ind = 0;
			if (!SQL_SUCCEEDED(SQLGetData(stmt.get(), 1, SQL_C_CHAR, buf, sizeof(buf), &ind)) || ind == SQL_NULL_DATA ||
			    ind < 0) {
				return "";
			}
			return string(buf, static_cast<size_t>(ind));
		}
		auto r = Rows(sql, true);
		if (r.size() != 1 || r[0].size() != 1) {
			RejectDDL("XX000", current_stage, "expected scalar SQL Server result");
		}
		return r[0][0];
	}

private:
	ClientContext &ctx;
	ODBCStatementPtr stmt;
	std::mutex mu;
	std::condition_variable cv;
	std::thread watcher;
	bool stopped = false;
	std::atomic<bool> cancelled {false};
};

static bool CompatibleSQLServerInstance(DDLSession &s) {
	if (s.Scalar("SELECT CASE WHEN @@VERSION LIKE 'Microsoft SQL Server%' AND "
	             "CONVERT(int,SERVERPROPERTY('EngineEdition')) IN(2,3,4) THEN '1' ELSE '0' END") != "1") {
		return false;
	}
	return s.Scalar("SELECT CONVERT(varchar(20),@@TRANCOUNT)") == "0";
}

struct SQLServerServerFacts {
	string version;
	string machine;
	string instance;
	string db_name;
	string db_id;
	string updateability;
	string login;
	string db_user;
	string schema_id;
};

static SQLServerServerFacts QueryServerFacts(DDLSession &s, const string &schema) {
	auto rows = s.Rows(
	    "SELECT "
	    "CAST(SERVERPROPERTY('ProductVersion') AS varchar(128)),"
	    "COALESCE(CAST(SERVERPROPERTY('MachineName') AS varchar(128)),''),"
	    "COALESCE(CAST(SERVERPROPERTY('InstanceName') AS varchar(128)),'MSSQLSERVER'),"
	    "COALESCE(CAST(DB_NAME() AS varchar(128)),''),"
	    "COALESCE(CONVERT(varchar(20),DB_ID()),''),"
	    "COALESCE(CAST(DATABASEPROPERTYEX(DB_NAME(),'Updateability') AS varchar(64)),''),"
	    "COALESCE(CAST(SUSER_SNAME() AS varchar(128)),''),"
	    "COALESCE(CAST(USER_NAME() AS varchar(128)),''),"
	    "COALESCE(CONVERT(varchar(20),SCHEMA_ID(" +
	    Literal(schema) + ")),'')");
	if (rows.size() != 1 || rows[0].size() != 9) {
		RejectDDL("0A000", "source_identity", "SQL Server identity query failed");
	}
	SQLServerServerFacts facts;
	facts.version = rows[0][0];
	facts.machine = rows[0][1];
	facts.instance = rows[0][2];
	facts.db_name = rows[0][3];
	facts.db_id = rows[0][4];
	facts.updateability = rows[0][5];
	facts.login = rows[0][6];
	facts.db_user = rows[0][7];
	facts.schema_id = rows[0][8];
	if (facts.version.empty() || facts.db_name.empty() || facts.db_id.empty()) {
		RejectDDL("0A000", "source_identity", "SQL Server identity fields incomplete");
	}
	return facts;
}

// ---------- Observation table functions ----------

struct DDLMetaData : TableFunctionData {
	ODBCCatalog &catalog;
	string schema;
	string kind;
	string name;
	bool describe = false;
	DDLMetaData(ODBCCatalog &c, bool d) : catalog(c), describe(d) {
	}
};

struct DDLState : GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<GlobalTableFunctionState> DDLInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<DDLState>();
}

static unique_ptr<FunctionData> DDLMetadataBind(ClientContext &ctx, TableFunctionBindInput &in,
                                                vector<LogicalType> &types, vector<string> &names) {
	for (auto &v : in.inputs) {
		if (v.IsNull()) {
			RejectLocal();
		}
	}
	auto db_name = TextVal(in.inputs[0]);
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, db_name);
	if (!db || db->GetCatalog().GetCatalogType() != "sqlserver") {
		throw BinderException("DDL catalog is unavailable");
	}
	bool describe = in.inputs.size() == 4;
	auto d = make_uniq<DDLMetaData>(db->GetCatalog().Cast<ODBCCatalog>(), describe);
	d->schema = TextVal(in.inputs[1]);
	if (!ValidName(d->schema)) {
		RejectLocal();
	}
	if (describe) {
		d->kind = TextVal(in.inputs[2]);
		d->name = TextVal(in.inputs[3]);
		if ((d->kind != "table" && d->kind != "index") || !ValidName(d->name)) {
			RejectLocal();
		}
	}
	names = {"sqlstate", "stage", "document"};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(d);
}

static void DDLMetadataScan(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
	auto &state = in.global_state->Cast<DDLState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &d = in.bind_data->Cast<DDLMetaData>();
	string sqlstate, stage = "source_identity", document;

	ODBCPoolConnection pooled;
	shared_ptr<ODBCConnection> connection;
	unique_ptr<DDLSession> session;
	try {
		if (ctx.IsInterrupted()) {
			RejectDDL("57014", "source_identity", "interrupted");
		}
		if (!d.catalog.GetConnectionPool().TryGetConnection(pooled)) {
			RejectDDL("08006", "source_identity", "connection unavailable");
		}
		connection = pooled.GetConnection();
		session = make_uniq<DDLSession>(ctx, connection->GetConn(), 5);
		auto &s = *session;
		if (!CompatibleSQLServerInstance(s)) {
			RejectDDL("0A000", "source_identity", "unsupported SQL Server instance state");
		}
		auto facts = QueryServerFacts(s, d.schema);
		string server_id = facts.machine + ":" + facts.instance;
		bool ns_present = !facts.schema_id.empty();
		bool read_only = StringUtil::Upper(facts.updateability) != "READ_WRITE";

		if (!d.describe) {
			document = string("{") + "\"contract_version\":1,\"product\":\"sqlserver\",\"source_version\":" +
			           JSON(facts.version) + ",\"database\":" + JSON(facts.db_name) +
			           ",\"database_identity\":" + JSON(facts.db_id) + ",\"server_identity\":" + JSON(server_id) +
			           ",\"schema\":" + JSON(d.schema) + ",\"namespace_identity\":" + JSON(facts.schema_id) +
			           ",\"namespace_present\":" + (ns_present ? "true" : "false") +
			           ",\"source_read_only\":" + (read_only ? "true" : "false") +
			           ",\"session_identity\":{\"login\":" + JSON(facts.login) + ",\"user\":" + JSON(facts.db_user) +
			           "},\"visibility\":\"CATALOG_READ\",\"executor_available\":false" +
			           ",\"identity_protection\":\"UNVERIFIED\"}";
		} else {
			string presence = "ABSENT";
			string object_identity, observed_kind, owning_table, table_identity;
			string cols_json = "[]", indexes_json = "[]", constraints_json = "[]";
			if (ns_present && d.kind == "table") {
				auto objs = s.Rows(
				    "SELECT CONVERT(varchar(20),o.object_id),o.type FROM sys.objects o "
				    "JOIN sys.schemas s ON s.schema_id=o.schema_id "
				    "WHERE s.name=" +
				    Literal(d.schema) + " AND o.name=" + Literal(d.name) +
				    " AND o.is_ms_shipped=0 AND o.type IN ('U','V')");
				if (objs.size() == 1) {
					presence = "PRESENT";
					object_identity = objs[0][0];
					observed_kind = objs[0][1] == "V" ? "v" : "r";
					auto col_rows = s.Rows(
					    "SELECT CONVERT(varchar(20),c.column_id),c.name,ty.name,"
					    "CONVERT(varchar(20),c.max_length),CONVERT(varchar(20),c.precision),"
					    "CONVERT(varchar(20),c.scale),CONVERT(varchar(5),c.is_nullable),"
					    "COALESCE(dc.name,''),COALESCE(dc.definition,'') "
					    "FROM sys.columns c JOIN sys.types ty ON ty.user_type_id=c.user_type_id "
					    "LEFT JOIN sys.default_constraints dc ON dc.object_id=c.default_object_id "
					    "WHERE c.object_id=" +
					    object_identity + " ORDER BY c.column_id");
					cols_json = "[";
					for (idx_t idx = 0; idx < col_rows.size() && idx < 256; idx++) {
						if (idx > 0) cols_json += ",";
						auto &r = col_rows[idx];
						cols_json += "{\"ordinal\":" + r[0] + ",\"name\":" + JSON(r[1]) + ",\"type\":" + JSON(r[2]) +
						             ",\"max_length\":" + r[3] + ",\"precision\":" + r[4] + ",\"scale\":" + r[5] +
						             ",\"nullable\":" + (r[6] == "1" ? "true" : "false") +
						             ",\"default_name\":" + JSON(r[7]) + ",\"default\":" + JSON(r[8]) + "}";
					}
					cols_json += "]";

					auto idx_rows = s.Rows(
					    "SELECT CONVERT(varchar(20),i.index_id),i.name,i.type_desc,"
					    "CONVERT(varchar(5),i.is_unique),CONVERT(varchar(5),i.is_primary_key) "
					    "FROM sys.indexes i WHERE i.object_id=" +
					    object_identity + " AND i.index_id>0 ORDER BY i.index_id");
					indexes_json = "[";
					for (idx_t idx = 0; idx < idx_rows.size() && idx < 256; idx++) {
						if (idx > 0) indexes_json += ",";
						auto &r = idx_rows[idx];
						indexes_json += "{\"table\":" + JSON(d.name) + ",\"name\":" + JSON(r[1]) +
						                ",\"type\":" + JSON(r[2]) +
						                ",\"unique\":" + (r[3] == "1" ? "true" : "false") +
						                ",\"primary\":" + (r[4] == "1" ? "true" : "false") + "}";
					}
					indexes_json += "]";

					auto con_rows = s.Rows(
					    "SELECT kc.name,kc.type FROM sys.key_constraints kc WHERE kc.parent_object_id=" +
					    object_identity +
					    " UNION ALL SELECT fk.name,'F' FROM sys.foreign_keys fk WHERE fk.parent_object_id=" +
					    object_identity +
					    " UNION ALL SELECT ch.name,'C' FROM sys.check_constraints ch WHERE ch.parent_object_id=" +
					    object_identity + " ORDER BY 1");
					constraints_json = "[";
					for (idx_t idx = 0; idx < con_rows.size() && idx < 256; idx++) {
						if (idx > 0) constraints_json += ",";
						constraints_json +=
						    "{\"name\":" + JSON(con_rows[idx][0]) + ",\"type\":" + JSON(con_rows[idx][1]) + "}";
					}
					constraints_json += "]";
				}
			} else if (ns_present && d.kind == "index") {
				auto idx_rows = s.Rows(
				    "SELECT CONVERT(varchar(20),t.object_id),t.name,CONVERT(varchar(20),i.index_id),"
				    "i.name,CONVERT(varchar(5),i.is_unique),CONVERT(varchar(5),i.is_primary_key) "
				    "FROM sys.indexes i JOIN sys.tables t ON t.object_id=i.object_id "
				    "JOIN sys.schemas s ON s.schema_id=t.schema_id "
				    "WHERE s.name=" +
				    Literal(d.schema) + " AND i.name=" + Literal(d.name) +
				    " AND t.is_ms_shipped=0 AND i.index_id>0 ORDER BY t.name,i.index_id");
				indexes_json = "[";
				for (idx_t idx = 0; idx < idx_rows.size() && idx < 256; idx++) {
					if (idx > 0) indexes_json += ",";
					auto &r = idx_rows[idx];
					indexes_json += "{\"table\":" + JSON(r[1]) + ",\"name\":" + JSON(r[3]) +
					                ",\"unique\":" + (r[4] == "1" ? "true" : "false") +
					                ",\"primary\":" + (r[5] == "1" ? "true" : "false") + "}";
				}
				indexes_json += "]";
				if (idx_rows.size() == 1) {
					presence = "PRESENT";
					table_identity = idx_rows[0][0];
					owning_table = idx_rows[0][1];
					object_identity = idx_rows[0][0] + ":" + idx_rows[0][2];
					observed_kind = "i";
				} else if (idx_rows.size() > 1) {
					presence = "UNKNOWN";
				}
			}
			document = string("{") + "\"contract_version\":1,\"product\":\"sqlserver\",\"source_version\":" +
			           JSON(facts.version) + ",\"database\":" + JSON(facts.db_name) +
			           ",\"database_identity\":" + JSON(facts.db_id) + ",\"server_identity\":" + JSON(server_id) +
			           ",\"schema\":" + JSON(d.schema) + ",\"name\":" + JSON(d.name) + ",\"kind\":" + JSON(d.kind) +
			           ",\"namespace_identity\":" + JSON(facts.schema_id) +
			           ",\"namespace_present\":" + (ns_present ? "true" : "false") +
			           ",\"presence\":" + JSON(presence) + ",\"object_identity\":" + JSON(object_identity);
			if (!observed_kind.empty()) {
				document += ",\"observed_kind\":" + JSON(observed_kind);
			}
			if (!owning_table.empty()) {
				document += ",\"owning_table\":" + JSON(owning_table);
			}
			if (!table_identity.empty()) {
				document += ",\"table_identity\":" + JSON(table_identity);
			}
			document += ",\"metadata_complete\":false,\"identity_protection\":\"UNVERIFIED\",\"columns\":" + cols_json +
			            ",\"indexes\":" + indexes_json + ",\"constraints\":" + constraints_json + "}";
		}
		stage.clear();
	} catch (const DDLRejectException &rej) {
		sqlstate = ValidSQLState(rej.sqlstate, "HY000");
		stage = rej.stage;
		document.clear();
	} catch (...) {
		sqlstate = ctx.IsInterrupted() ? "57014" : "08006";
		document.clear();
	}
	session.reset();
	if (connection) {
		connection->Close();
	}
	out.SetCardinality(1);
	out.SetValue(0, 0, Value(sqlstate));
	out.SetValue(1, 0, Value(stage));
	out.SetValue(2, 0, Value(document));
}

// ---------- Guarded execution table function ----------

struct DDLColumnDef {
	string name;
	string type;
	int32_t length = 0;
	int32_t precision = 0;
	int32_t scale = 0;
	bool not_null = false;
	string default_kind;
	string default_value;
	bool primary_key = false;
};

struct SQLServerDDLIntent {
	string database;
	string schema;
	string operation;
	string action;
	string name;
	string index_name;
	string owner;
	string new_name;
	string column_name;
	string default_kind;
	string default_value;
	string constraint_name;
	string constraint_kind;
	string check_column;
	string check_operator;
	string check_value;
	string reference_schema;
	string reference_table;
	string identity_mode;
	vector<string> constraint_keys;
	vector<string> reference_keys;
	vector<DDLColumnDef> columns;
	vector<string> keys;
	bool conditional = false;
	string primary_key_name;
	vector<string> primary_key_keys;
};

static string FormatSQLServerDefault(const string &kind, const string &value) {
	if (kind == "null") {
		if (!value.empty()) {
			RejectDDL("0A000", "source_definition", "invalid null default");
		}
		return "NULL";
	}
	if (kind == "integer") {
		if (!IsSignedInt64Literal(value)) {
			RejectDDL("0A000", "source_definition", "invalid integer default");
		}
		return "(" + value + ")";
	}
	if (kind == "string") {
		return Literal(value);
	}
	if (kind == "boolean") {
		if (value == "true") {
			return "(1)";
		}
		if (value == "false") {
			return "(0)";
		}
		RejectDDL("0A000", "source_definition", "invalid boolean default");
	}
	RejectDDL("0A000", "source_definition", "unsupported default kind");
}

static string MapColumnTypeSQLServer(const DDLColumnDef &col) {
	if (col.type == "numeric") {
		if (col.length != 0 || col.precision < 1 || col.precision > 38 || col.scale < 0 || col.scale > col.precision) {
			RejectLocal();
		}
		return "DECIMAL(" + std::to_string(col.precision) + "," + std::to_string(col.scale) + ")";
	}
	if (col.type == "time" || col.type == "timestamp" || col.type == "timestamptz") {
		if (col.length != 0 || col.precision < 0 || col.precision > 6 || col.scale != 0) {
			RejectLocal();
		}
		string base = col.type == "time" ? "TIME(" : (col.type == "timestamp" ? "DATETIME2(" : "DATETIMEOFFSET(");
		return base + std::to_string(col.precision) + ")";
	}
	if (col.precision != 0 || col.scale != 0) {
		RejectLocal();
	}
	if (col.type == "varchar") {
		if (col.length < 1 || col.length > 4000) {
			RejectLocal();
		}
		return "NVARCHAR(" + std::to_string(col.length) + ")";
	}
	if (col.length != 0) {
		RejectLocal();
	}
	if (col.type == "boolean") return "BIT";
	if (col.type == "smallint") return "SMALLINT";
	if (col.type == "integer") return "INT";
	if (col.type == "bigint") return "BIGINT";
	if (col.type == "real") return "REAL";
	if (col.type == "double") return "FLOAT(53)";
	if (col.type == "text") return "NVARCHAR(MAX)";
	if (col.type == "bytea") return "VARBINARY(MAX)";
	if (col.type == "date") return "DATE";
	if (col.type == "uuid") return "UNIQUEIDENTIFIER";
	RejectLocal();
}

static vector<string> ReadStringList(const Value &v, idx_t max_len) {
	if (v.IsNull() || v.type().id() != LogicalTypeId::LIST) {
		RejectLocal();
	}
	vector<string> out;
	const auto &items = ListValue::GetChildren(v);
	if (items.size() > max_len) {
		RejectLocal();
	}
	for (const auto &item : items) {
		auto s = TextVal(item);
		if (!ValidName(s) || std::find(out.begin(), out.end(), s) != out.end()) {
			RejectLocal();
		}
		out.push_back(std::move(s));
	}
	return out;
}

static SQLServerDDLIntent ParseUnifiedIntent(const vector<Value> &inputs) {
	if (inputs.size() != 5) {
		RejectLocal();
	}
	SQLServerDDLIntent d;
	d.database = TextVal(inputs[1]);
	d.schema = TextVal(inputs[2]);
	d.operation = TextVal(inputs[3]);
	if (!ValidName(d.database) || !ValidName(d.schema) || !ValidUUID(d.operation)) {
		RejectLocal();
	}
	const auto &sval = inputs[4];
	if (sval.IsNull() || sval.type().id() != LogicalTypeId::STRUCT) {
		RejectLocal();
	}
	const auto &types = StructType::GetChildTypes(sval.type());
	const auto &fields = StructValue::GetChildren(sval);
	const vector<string> expected = {
	    "contract_version", "target_contract",  "action",          "name",             "index_name",
	    "owner",            "new_name",         "column_name",     "default_kind",     "default_value",
	    "constraint_name",  "constraint_kind",  "check_column",    "check_operator",   "check_value",
	    "reference_schema", "reference_table",  "identity_mode",   "constraint_keys",  "reference_keys",
	    "columns",          "keys",             "conditional",     "primary_key_name", "primary_key_keys",
	};
	if (types.size() != expected.size()) {
		RejectLocal();
	}
	for (idx_t i = 0; i < expected.size(); i++) {
		if (types[i].first != expected[i] || fields[i].IsNull()) {
			RejectLocal();
		}
	}
	if (IntVal(fields[0]) != 1 || TextVal(fields[1]) != "EXECUTION_NAME") {
		RejectLocal();
	}
	d.action = TextVal(fields[2]);
	d.name = TextVal(fields[3]);
	d.index_name = TextVal(fields[4]);
	d.owner = TextVal(fields[5]);
	d.new_name = TextVal(fields[6]);
	d.column_name = TextVal(fields[7]);
	d.default_kind = TextVal(fields[8]);
	d.default_value = TextVal(fields[9]);
	d.constraint_name = TextVal(fields[10]);
	d.constraint_kind = TextVal(fields[11]);
	d.check_column = TextVal(fields[12]);
	d.check_operator = TextVal(fields[13]);
	d.check_value = TextVal(fields[14]);
	d.reference_schema = TextVal(fields[15]);
	d.reference_table = TextVal(fields[16]);
	d.identity_mode = TextVal(fields[17]);

	if (!ValidName(d.name)) RejectLocal();
	for (const auto *opt : {&d.index_name, &d.owner, &d.new_name, &d.column_name, &d.constraint_name,
	                        &d.check_column, &d.reference_schema, &d.reference_table}) {
		if (!opt->empty() && !ValidName(*opt)) {
			RejectLocal();
		}
	}
	if (d.default_value.size() > 4096) {
		RejectLocal();
	}
	d.constraint_keys = ReadStringList(fields[18], 32);
	d.reference_keys = ReadStringList(fields[19], 32);
	d.keys = ReadStringList(fields[21], 32);
	d.conditional = BoolVal(fields[22]);
	d.primary_key_name = TextVal(fields[23]);
	if (!d.primary_key_name.empty() && !ValidName(d.primary_key_name)) {
		RejectLocal();
	}
	d.primary_key_keys = ReadStringList(fields[24], 32);

	if (d.conditional && d.action != "create_table" && d.action != "drop_table" && d.action != "add_column" &&
	    d.action != "drop_column" && d.action != "create_index" && d.action != "create_unique_index" &&
	    d.action != "drop_index") {
		RejectLocal();
	}
	if ((!d.primary_key_keys.empty() || !d.primary_key_name.empty()) && d.action != "create_table") {
		RejectLocal();
	}

	if (fields[20].type().id() != LogicalTypeId::LIST) {
		RejectLocal();
	}
	const auto &cols = ListValue::GetChildren(fields[20]);
	if (cols.size() > 256) {
		RejectLocal();
	}
	vector<string> seen_cols;
	idx_t inline_pk_count = 0;
	for (const auto &col : cols) {
		if (col.IsNull() || col.type().id() != LogicalTypeId::STRUCT) {
			RejectLocal();
		}
		const auto &ct = StructType::GetChildTypes(col.type());
		const auto &cv = StructValue::GetChildren(col);
		if (ct.size() != 9 || ct[0].first != "name" || ct[1].first != "type" || ct[2].first != "length" ||
		    ct[3].first != "precision" || ct[4].first != "scale" || ct[5].first != "not_null" ||
		    ct[6].first != "default_kind" || ct[7].first != "default_value" || ct[8].first != "primary_key") {
			RejectLocal();
		}
		DDLColumnDef def;
		def.name = TextVal(cv[0]);
		def.type = TextVal(cv[1]);
		def.length = IntVal(cv[2]);
		def.precision = IntVal(cv[3]);
		def.scale = IntVal(cv[4]);
		def.not_null = BoolVal(cv[5]);
		def.default_kind = TextVal(cv[6]);
		def.default_value = TextVal(cv[7]);
		def.primary_key = BoolVal(cv[8]);
		if (!ValidName(def.name) || std::find(seen_cols.begin(), seen_cols.end(), def.name) != seen_cols.end()) {
			RejectLocal();
		}
		if (def.default_value.size() > 4096) {
			RejectLocal();
		}
		if (def.default_kind.empty() || def.default_kind == "null") {
			if (!def.default_value.empty()) {
				RejectLocal();
			}
		} else if (def.default_kind == "integer") {
			if (!IsSignedInt64Literal(def.default_value)) {
				RejectLocal();
			}
		} else if (def.default_kind == "boolean") {
			if (def.default_value != "true" && def.default_value != "false") {
				RejectLocal();
			}
		} else if (def.default_kind != "string") {
			RejectLocal();
		}
		if (d.action == "alter_column_type" && (def.not_null || !def.default_kind.empty() || def.primary_key)) {
			RejectLocal();
		}
		if (d.action == "add_column" && def.primary_key) {
			RejectLocal();
		}
		if ((def.not_null || def.primary_key) && def.default_kind == "null") {
			RejectLocal();
		}
		if (def.primary_key) {
			inline_pk_count++;
		}
		seen_cols.push_back(def.name);
		d.columns.push_back(std::move(def));
	}
	if (inline_pk_count > 1 || (inline_pk_count > 0 && !d.primary_key_keys.empty())) {
		RejectLocal();
	}
	for (const auto &k : d.primary_key_keys) {
		auto it = std::find_if(d.columns.begin(), d.columns.end(), [&](const DDLColumnDef &c) { return c.name == k; });
		if (it == d.columns.end() || it->default_kind == "null") {
			RejectLocal();
		}
	}
	if (!d.primary_key_name.empty() && inline_pk_count == 0 && d.primary_key_keys.empty()) {
		RejectLocal();
	}
	return d;
}

static string BuildSQLServerAlterColumnType(const vector<string> &row) {
	// row: {ty.name, max_length, precision, scale, collation_name, is_computed, is_identity, is_nullable, is_user_defined}
	const string ty = StringUtil::Lower(row[0]);
	int max_len = std::stoi(row[1]);
	int prec = std::stoi(row[2]);
	int scale = std::stoi(row[3]);
	const string collation = row[4];
	if (row[5] == "1" || row[6] == "1" || row[8] == "1") {
		RejectDDL("0A000", "source_definition", "cannot alter computed, identity, or user-defined column");
	}
	if (ty == "bit" || ty == "tinyint" || ty == "smallint" || ty == "int" || ty == "bigint" || ty == "real" ||
	    ty == "date" || ty == "uniqueidentifier" || ty == "datetime" || ty == "smalldatetime" || ty == "money" ||
	    ty == "smallmoney") {
		return ty;
	}
	if (ty == "float") {
		return "float(" + std::to_string(prec > 0 ? prec : 53) + ")";
	}
	if (ty == "decimal" || ty == "numeric") {
		return ty + "(" + std::to_string(prec) + "," + std::to_string(scale) + ")";
	}
	if (ty == "time" || ty == "datetime2" || ty == "datetimeoffset") {
		return ty + "(" + std::to_string(scale) + ")";
	}
	if (ty == "varbinary" || ty == "binary") {
		if (max_len == -1) {
			return ty + "(MAX)";
		}
		return ty + "(" + std::to_string(max_len) + ")";
	}
	if (ty == "varchar" || ty == "char" || ty == "nvarchar" || ty == "nchar") {
		int char_len = max_len;
		if (max_len != -1 && (ty == "nvarchar" || ty == "nchar")) {
			char_len = max_len / 2;
		}
		string res = ty + "(" + (max_len == -1 ? string("MAX") : std::to_string(char_len)) + ")";
		if (!collation.empty()) {
			res += " COLLATE " + SafeSQLServerCollation(collation);
		}
		return res;
	}
	RejectDDL("0A000", "source_definition", "unsupported column type for NULL/NOT NULL change");
}

struct DDLExecData : TableFunctionData {
	ODBCCatalog &catalog;
	SQLServerDDLIntent intent;
	bool diagnostics = false;
	DDLExecData(ODBCCatalog &c, SQLServerDDLIntent d) : catalog(c), intent(std::move(d)) {
	}
};

static unique_ptr<FunctionData> DDLExecBind(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                            vector<string> &names) {
	auto intent = ParseUnifiedIntent(in.inputs);
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, TextVal(in.inputs[0]));
	if (!db || db->GetCatalog().GetCatalogType() != "sqlserver") {
		throw BinderException("DDL catalog is unavailable");
	}
	auto data = make_uniq<DDLExecData>(db->GetCatalog().Cast<ODBCCatalog>(), std::move(intent));
	auto diag = in.named_parameters.find("source_diagnostics");
	if (diag != in.named_parameters.end() && !diag->second.IsNull()) {
		data->diagnostics = diag->second.GetValue<bool>();
	}
	names = {"operation_id", "status", "sqlstate", "stage", "warning_count"};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::BIGINT};
	if (data->diagnostics) {
		names.push_back("source_error");
		types.push_back(LogicalType::VARCHAR);
	}
	return std::move(data);
}

static void DDLExecScan(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
	auto &state = in.global_state->Cast<DDLState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &d = in.bind_data->Cast<DDLExecData>();
	const auto &i = d.intent;

	string status = "CONFIRMED_NOT_EXECUTED";
	string error = "0A000";
	string stage = "source_identity";
	int64_t warnings = 0;
	string source_error;
	bool sent = false;
	bool commit_started = false;
	bool in_tx = false;

	ODBCPoolConnection pooled;
	shared_ptr<ODBCConnection> connection;
	unique_ptr<DDLSession> session;
	try {
		if (ctx.IsInterrupted()) {
			RejectDDL("57014", "source_identity", "operation cancelled");
		}
		if (!d.catalog.GetConnectionPool().TryGetConnection(pooled)) {
			RejectDDL("08006", "source_identity", "connection unavailable");
		}
		connection = pooled.GetConnection();
		session = make_uniq<DDLSession>(ctx, connection->GetConn(), 15);
		auto &s = *session;

		if (!CompatibleSQLServerInstance(s)) {
			RejectDDL("0A000", "source_identity", "unsupported SQL Server instance state");
		}
		auto facts = QueryServerFacts(s, i.schema);
		if (facts.db_name != i.database) {
			RejectDDL("0A000", "source_identity", "database identity mismatch");
		}
		if (StringUtil::Upper(facts.updateability) != "READ_WRITE") {
			RejectDDL("25006", "source_identity", "SQL Server database is read-only");
		}
		if (facts.schema_id.empty()) {
			RejectDDL("3F000", "source_definition", "target schema does not exist");
		}

		// Session safety settings + short lock timeout (2000ms) so DDL fails fast when online traffic holds locks.
		s.Exec("SET DATEFORMAT ymd;SET XACT_ABORT ON;SET NOCOUNT OFF;SET ANSI_NULLS ON;SET QUOTED_IDENTIFIER ON;"
		       "SET ANSI_WARNINGS ON;SET ARITHABORT ON;SET NUMERIC_ROUNDABORT OFF;SET LOCK_TIMEOUT 2000;"
		       "BEGIN TRANSACTION");
		in_tx = true;

		stage = "source_definition";
		s.current_stage = "source_definition";
		const string target = Identifier(i.schema) + "." + Identifier(i.name);
		auto obj_id_of = [&](const string &obj_name) {
			return s.Scalar("SELECT COALESCE(CONVERT(varchar(20),MAX(o.object_id)),'') FROM sys.objects o "
			                "JOIN sys.schemas s ON s.schema_id=o.schema_id WHERE s.name=" +
			                Literal(i.schema) + " AND o.name=" + Literal(obj_name) +
			                " AND o.is_ms_shipped=0 AND o.type='U'");
		};
		auto any_obj_exists = [&](const string &obj_name) {
			return s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.objects o "
			                "JOIN sys.schemas s ON s.schema_id=o.schema_id WHERE s.name=" +
			                Literal(i.schema) + " AND o.name=" + Literal(obj_name)) != "0";
		};
		auto check_col_dependencies = [&](const string &obj_id, const string &col_id) {
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.index_columns ic "
			             "JOIN sys.indexes ix ON ix.object_id=ic.object_id AND ix.index_id=ic.index_id "
			             "WHERE ic.object_id=" +
			             obj_id + " AND ic.column_id=" + col_id + " AND ix.index_id>0") != "0") {
				RejectDDL("2BP01", "source_definition", "cannot modify or drop column referenced by an index");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.foreign_key_columns "
			             "WHERE (parent_object_id=" +
			             obj_id + " AND parent_column_id=" + col_id + ") OR (referenced_object_id=" + obj_id +
			             " AND referenced_column_id=" + col_id + ")") != "0") {
				RejectDDL("2BP01", "source_definition",
				          "cannot modify or drop column referenced by a foreign key constraint");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.check_constraints ch "
			             "WHERE ch.parent_object_id=" +
			             obj_id + " AND (ch.parent_column_id=" + col_id +
			             " OR EXISTS (SELECT 1 FROM sys.sql_expression_dependencies ed "
			             "WHERE ed.referencing_id=ch.object_id AND ed.referenced_id=" +
			             obj_id + " AND ed.referenced_minor_id=" + col_id + "))") != "0") {
				RejectDDL("2BP01", "source_definition",
				          "cannot modify or drop column referenced by a check constraint");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.sql_expression_dependencies ed "
			             "JOIN sys.objects o ON o.object_id=ed.referencing_id "
			             "WHERE ed.referenced_id=" +
			             obj_id + " AND (ed.referenced_minor_id=" + col_id +
			             " OR ed.referenced_minor_id=0) AND o.type='V'") != "0") {
				RejectDDL("2BP01", "source_definition", "cannot modify or drop column referenced by a view");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.sql_expression_dependencies ed "
			             "WHERE ed.referenced_id=" +
			             obj_id + " AND ed.referenced_minor_id=" + col_id +
			             " AND ed.referencing_id <> COALESCE((SELECT default_object_id FROM sys.columns "
			             "WHERE object_id=" +
			             obj_id + " AND column_id=" + col_id + "),0)") != "0") {
				RejectDDL("2BP01", "source_definition",
				          "cannot modify or drop column with dependent objects or computed columns");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.stats_columns sc "
			             "JOIN sys.stats st ON st.object_id=sc.object_id AND st.stats_id=sc.stats_id "
			             "WHERE sc.object_id=" +
			             obj_id + " AND sc.column_id=" + col_id + " AND st.user_created=1") != "0") {
				RejectDDL("2BP01", "source_definition",
				          "cannot modify or drop column referenced by user-created statistics");
			}
		};

		string native_sql;
		bool sp_rename_op = false;
		bool no_op_commit = false;
		bool source_noop = false;

		if (i.action == "create_table") {
			if (i.columns.empty() || !i.keys.empty() || !i.index_name.empty() || !i.owner.empty() ||
			    !i.new_name.empty() || !i.column_name.empty()) {
				RejectDDL("0A000", "source_definition", "invalid create_table fields");
			}
			if (!obj_id_of(i.name).empty()) {
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42P07", "source_definition", "target object already exists");
				}
			} else if (any_obj_exists(i.name)) {
				RejectDDL("42P07", "source_definition", "target object already exists");
			}
			if (!source_noop) {
				vector<string> eff_pk = i.primary_key_keys;
				if (eff_pk.empty()) {
					for (const auto &c : i.columns) {
						if (c.primary_key) {
							eff_pk.push_back(c.name);
							break;
						}
					}
				}
				vector<string> defs;
				for (const auto &c : i.columns) {
					bool eff_nn = c.not_null || c.primary_key ||
					              std::find(i.primary_key_keys.begin(), i.primary_key_keys.end(), c.name) !=
					                  i.primary_key_keys.end();
					string col_def = Identifier(c.name) + " " + MapColumnTypeSQLServer(c);
					if (!c.default_kind.empty()) {
						col_def += " DEFAULT " + FormatSQLServerDefault(c.default_kind, c.default_value);
					}
					col_def += eff_nn ? " NOT NULL" : " NULL";
					defs.push_back(std::move(col_def));
				}
				if (!eff_pk.empty()) {
					vector<string> pk_quoted;
					pk_quoted.reserve(eff_pk.size());
					for (const auto &k : eff_pk) {
						pk_quoted.push_back(Identifier(k));
					}
					string pk_clause;
					if (!i.primary_key_name.empty()) {
						if (any_obj_exists(i.primary_key_name)) {
							RejectDDL("42710", "source_definition", "constraint name already exists");
						}
						pk_clause = "CONSTRAINT " + Identifier(i.primary_key_name) + " ";
					}
					pk_clause += "PRIMARY KEY (" + StringUtil::Join(pk_quoted, ",") + ")";
					defs.push_back(std::move(pk_clause));
				}
				native_sql = "CREATE TABLE " + target + " (" + StringUtil::Join(defs, ",") + ")";
			}
		} else if (i.action == "drop_table") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				if (any_obj_exists(i.name)) {
					RejectDDL("42809", "source_definition", "target object is not a base table");
				}
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42P01", "source_definition", "target table does not exist");
				}
			} else {
				if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.foreign_keys WHERE referenced_object_id=" +
				             obj_id + " AND parent_object_id<>" + obj_id) != "0") {
					RejectDDL("2BP01", "source_definition", "cannot drop table with referencing foreign keys");
				}
				native_sql = "DROP TABLE " + target;
			}
		} else if (i.action == "rename_table") {
			if (i.new_name.empty() || i.new_name == i.name) {
				RejectDDL("0A000", "source_definition", "invalid rename_table name");
			}
			if (obj_id_of(i.name).empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (any_obj_exists(i.new_name)) {
				RejectDDL("42P07", "source_definition", "destination table already exists");
			}
			native_sql = "EXEC sys.sp_rename @objname=" + Literal(target) + ",@newname=" + Literal(i.new_name) +
			             ",@objtype=N'OBJECT'";
			sp_rename_op = true;
		} else if (i.action == "add_column") {
			if (i.columns.size() != 1 || i.columns[0].primary_key) {
				RejectDDL("0A000", "source_definition", "invalid add_column payload");
			}
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			const auto &c = i.columns[0];
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.columns WHERE object_id=" + obj_id +
			             " AND name=" + Literal(c.name)) != "0") {
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42701", "source_definition", "column already exists");
				}
			} else {
				if (c.not_null && (c.default_kind.empty() || c.default_kind == "null")) {
					if (s.Scalar("SELECT CASE WHEN EXISTS(SELECT 1 FROM " + target + ") THEN '1' ELSE '0' END") ==
					    "1") {
						RejectDDL("23502", "source_definition",
						          "cannot add NOT NULL column without default to non-empty table");
					}
				}
				native_sql = "ALTER TABLE " + target + " ADD " + Identifier(c.name) + " " +
				             MapColumnTypeSQLServer(c) + (c.not_null ? " NOT NULL" : " NULL");
				if (!c.default_kind.empty()) {
					native_sql += " DEFAULT " + FormatSQLServerDefault(c.default_kind, c.default_value);
				}
			}
		} else if (i.action == "drop_column") {
			if (i.column_name.empty()) {
				RejectDDL("0A000", "source_definition", "column name required");
			}
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			auto col_rows = s.Rows(
			    "SELECT CONVERT(varchar(20),c.column_id),COALESCE(dc.name,'') FROM sys.columns c "
			    "LEFT JOIN sys.default_constraints dc ON dc.object_id=c.default_object_id "
			    "WHERE c.object_id=" +
			    obj_id + " AND c.name=" + Literal(i.column_name));
			if (col_rows.empty()) {
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42703", "source_definition", "column does not exist");
				}
			} else {
				if (std::stoi(s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.columns WHERE object_id=" +
				                       obj_id)) <= 1) {
					RejectDDL("42P16", "source_definition", "cannot drop the only column of a table");
				}
				check_col_dependencies(obj_id, col_rows[0][0]);
				if (!col_rows[0][1].empty()) {
					native_sql = "ALTER TABLE " + target + " DROP CONSTRAINT " + Identifier(col_rows[0][1]) + ";";
				}
				native_sql += "ALTER TABLE " + target + " DROP COLUMN " + Identifier(i.column_name);
			}
		} else if (i.action == "alter_column_type") {
			if (i.column_name.empty() || i.columns.size() != 1 || i.columns[0].name != i.column_name) {
				RejectDDL("0A000", "source_definition", "invalid alter_column_type payload");
			}
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			auto col_rows = s.Rows(
			    "SELECT ty.name,CONVERT(varchar(20),c.max_length),CONVERT(varchar(20),c.precision),"
			    "CONVERT(varchar(20),c.scale),COALESCE(c.collation_name,''),CONVERT(varchar(5),c.is_computed),"
			    "CONVERT(varchar(5),c.is_identity),CONVERT(varchar(5),c.is_nullable),"
			    "CONVERT(varchar(5),ty.is_user_defined),CONVERT(varchar(20),c.column_id),"
			    "COALESCE(dc.name,''),COALESCE(dc.definition,''),CONVERT(varchar(5),COALESCE(dc.is_system_named,1)) "
			    "FROM sys.columns c JOIN sys.types ty ON ty.user_type_id=c.user_type_id "
			    "LEFT JOIN sys.default_constraints dc ON dc.object_id=c.default_object_id "
			    "WHERE c.object_id=" +
			    obj_id + " AND c.name=" + Literal(i.column_name));
			if (col_rows.size() != 1) {
				RejectDDL("42703", "source_definition", "column does not exist");
			}
			const auto &r = col_rows[0];
			if (r[5] == "1" || r[6] == "1" || r[8] == "1") {
				RejectDDL("0A000", "source_definition", "cannot alter computed, identity, or user-defined column");
			}
			check_col_dependencies(obj_id, r[9]);
			const string ty = StringUtil::Lower(r[0]);
			int max_len = std::stoi(r[1]);
			int prec = std::stoi(r[2]);
			int scale = std::stoi(r[3]);
			const string collation = r[4];
			const bool nullable = r[7] != "0";
			const auto &target_col = i.columns[0];
			bool widened = false;
			string collate_clause;
			if (ty == "smallint" && (target_col.type == "integer" || target_col.type == "bigint")) {
				widened = true;
			} else if (ty == "int" && target_col.type == "bigint") {
				widened = true;
			} else if ((ty == "real" || (ty == "float" && prec <= 24)) && target_col.type == "double") {
				widened = true;
			} else if ((ty == "decimal" || ty == "numeric") && target_col.type == "numeric") {
				widened = target_col.scale == scale && target_col.precision > prec && target_col.precision <= 38;
			} else if (ty == "nvarchar" && target_col.type == "varchar") {
				if (max_len > 0 && (max_len % 2) == 0) {
					int cur_len = max_len / 2;
					if (target_col.length > cur_len && target_col.length <= 4000) {
						widened = true;
						if (!collation.empty()) {
							collate_clause = " COLLATE " + SafeSQLServerCollation(collation);
						}
					}
				}
			} else if (ty == "time" && target_col.type == "time") {
				widened = target_col.precision > scale && target_col.precision <= 6;
			} else if (ty == "datetime2" && target_col.type == "timestamp") {
				widened = target_col.precision > scale && target_col.precision <= 6;
			} else if (ty == "datetimeoffset" && target_col.type == "timestamptz") {
				widened = target_col.precision > scale && target_col.precision <= 6;
			}
			if (!widened) {
				RejectDDL("0A000", "source_definition", "only lossless type widening is supported");
			}
			const string df_name = r[10];
			const string df_def = r[11];
			const bool df_sys_named = r[12] != "0";
			const string alter_col_sql = "ALTER TABLE " + target + " ALTER COLUMN " + Identifier(i.column_name) +
			                             " " + MapColumnTypeSQLServer(target_col) + collate_clause +
			                             (nullable ? " NULL" : " NOT NULL");
			if (!df_name.empty()) {
				if (df_def.empty()) {
					RejectDDL("0A000", "source_definition",
					          "cannot preserve default constraint definition during alter_column_type");
				}
				const string add_df_sql =
				    "ALTER TABLE " + target +
				    (df_sys_named ? " ADD DEFAULT " : (" ADD CONSTRAINT " + Identifier(df_name) + " DEFAULT ")) +
				    df_def + " FOR " + Identifier(i.column_name);
				native_sql = "ALTER TABLE " + target + " DROP CONSTRAINT " + Identifier(df_name) + ";" +
				             alter_col_sql + ";" + add_df_sql;
			} else {
				native_sql = alter_col_sql;
			}
		} else if (i.action == "rename_column") {
			if (i.column_name.empty() || i.new_name.empty() || i.new_name == i.column_name) {
				RejectDDL("0A000", "source_definition", "invalid rename_column payload");
			}
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.columns WHERE object_id=" + obj_id +
			             " AND name=" + Literal(i.column_name)) == "0") {
				RejectDDL("42703", "source_definition", "column does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.columns WHERE object_id=" + obj_id +
			             " AND name=" + Literal(i.new_name)) != "0") {
				RejectDDL("42701", "source_definition", "destination column already exists");
			}
			native_sql = "EXEC sys.sp_rename @objname=" + Literal(target + "." + Identifier(i.column_name)) +
			             ",@newname=" + Literal(i.new_name) + ",@objtype=N'COLUMN'";
			sp_rename_op = true;
		} else if (i.action == "set_default" || i.action == "drop_default") {
			if (i.column_name.empty()) {
				RejectDDL("0A000", "source_definition", "column name required");
			}
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			auto col_rows = s.Rows(
			    "SELECT COALESCE(dc.name,'') FROM sys.columns c "
			    "LEFT JOIN sys.default_constraints dc ON dc.object_id=c.default_object_id "
			    "WHERE c.object_id=" +
			    obj_id + " AND c.name=" + Literal(i.column_name));
			if (col_rows.size() != 1) {
				RejectDDL("42703", "source_definition", "column does not exist");
			}
			string existing_df = col_rows[0][0];
			if (i.action == "drop_default") {
				if (existing_df.empty()) {
					no_op_commit = true;
				} else {
					native_sql = "ALTER TABLE " + target + " DROP CONSTRAINT " + Identifier(existing_df);
				}
			} else {
				string lit = FormatSQLServerDefault(i.default_kind, i.default_value);
				if (!existing_df.empty()) {
					native_sql = "ALTER TABLE " + target + " DROP CONSTRAINT " + Identifier(existing_df) + ";";
				}
				native_sql += "ALTER TABLE " + target + " ADD DEFAULT " + lit + " FOR " + Identifier(i.column_name);
			}
		} else if (i.action == "set_not_null" || i.action == "drop_not_null") {
			if (i.column_name.empty()) {
				RejectDDL("0A000", "source_definition", "column name required");
			}
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			auto col_rows = s.Rows(
			    "SELECT ty.name,CONVERT(varchar(20),c.max_length),CONVERT(varchar(20),c.precision),"
			    "CONVERT(varchar(20),c.scale),COALESCE(c.collation_name,''),CONVERT(varchar(5),c.is_computed),"
			    "CONVERT(varchar(5),c.is_identity),CONVERT(varchar(5),c.is_nullable),"
			    "CONVERT(varchar(5),ty.is_user_defined) "
			    "FROM sys.columns c JOIN sys.types ty ON ty.user_type_id=c.user_type_id "
			    "WHERE c.object_id=" +
			    obj_id + " AND c.name=" + Literal(i.column_name));
			if (col_rows.size() != 1) {
				RejectDDL("42703", "source_definition", "column does not exist");
			}
			string col_type = BuildSQLServerAlterColumnType(col_rows[0]);
			native_sql = "ALTER TABLE " + target + " ALTER COLUMN " + Identifier(i.column_name) + " " + col_type +
			             (i.action == "set_not_null" ? " NOT NULL" : " NULL");
		} else if (i.action == "add_constraint") {
			if (i.constraint_name.empty() || i.constraint_kind.empty()) {
				RejectDDL("0A000", "source_definition", "invalid add_constraint payload");
			}
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.objects o JOIN sys.schemas s ON "
			             "s.schema_id=o.schema_id WHERE s.name=" +
			             Literal(i.schema) + " AND o.name=" + Literal(i.constraint_name)) != "0") {
				RejectDDL("42710", "source_definition", "constraint name already exists");
			}
			auto quote_list = [](const vector<string> &items) {
				vector<string> quoted;
				quoted.reserve(items.size());
				for (const auto &it : items) quoted.push_back(Identifier(it));
				return StringUtil::Join(quoted, ",");
			};
			if (i.constraint_kind == "primary_key" || i.constraint_kind == "unique") {
				if (i.constraint_keys.empty()) RejectDDL("0A000", "source_definition", "constraint keys required");
				native_sql = "ALTER TABLE " + target + " ADD CONSTRAINT " + Identifier(i.constraint_name) +
				             (i.constraint_kind == "primary_key" ? " PRIMARY KEY (" : " UNIQUE (") +
				             quote_list(i.constraint_keys) + ")";
			} else if (i.constraint_kind == "foreign_key") {
				if (i.constraint_keys.empty() || i.reference_table.empty() ||
				    i.reference_keys.size() != i.constraint_keys.size() || i.reference_schema != i.schema) {
					RejectDDL("0A000", "source_definition", "invalid foreign key definition");
				}
				if (obj_id_of(i.reference_table).empty()) {
					RejectDDL("42P01", "source_definition", "referenced table does not exist");
				}
				native_sql = "ALTER TABLE " + target + " ADD CONSTRAINT " + Identifier(i.constraint_name) +
				             " FOREIGN KEY (" + quote_list(i.constraint_keys) + ") REFERENCES " +
				             Identifier(i.reference_schema) + "." + Identifier(i.reference_table) + " (" +
				             quote_list(i.reference_keys) + ") ON DELETE NO ACTION ON UPDATE NO ACTION";
			} else if (i.constraint_kind == "check") {
				if (i.check_column.empty() || i.check_value.empty() || i.check_value == "-") {
					RejectDDL("0A000", "source_definition", "invalid check constraint");
				}
				if (i.check_operator != "=" && i.check_operator != "<>" && i.check_operator != "<" &&
				    i.check_operator != ">" && i.check_operator != "<=" && i.check_operator != ">=") {
					RejectDDL("0A000", "source_definition", "unsupported check operator");
				}
				for (idx_t idx = 0; idx < i.check_value.size(); idx++) {
					char c = i.check_value[idx];
					if (!((c >= '0' && c <= '9') || (idx == 0 && c == '-'))) {
						RejectDDL("0A000", "source_definition", "invalid integer check literal");
					}
				}
				native_sql = "ALTER TABLE " + target + " ADD CONSTRAINT " + Identifier(i.constraint_name) + " CHECK (" +
				             Identifier(i.check_column) + " " + i.check_operator + " " + i.check_value + ")";
			} else {
				RejectDDL("0A000", "source_definition", "unsupported constraint kind");
			}
		} else if (i.action == "drop_constraint") {
			if (i.constraint_name.empty()) {
				RejectDDL("0A000", "source_definition", "constraint name required");
			}
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM ("
			             "SELECT name FROM sys.key_constraints WHERE parent_object_id=" +
			             obj_id + " AND name=" + Literal(i.constraint_name) +
			             " UNION ALL SELECT name FROM sys.foreign_keys WHERE parent_object_id=" + obj_id +
			             " AND name=" + Literal(i.constraint_name) +
			             " UNION ALL SELECT name FROM sys.check_constraints WHERE parent_object_id=" + obj_id +
			             " AND name=" + Literal(i.constraint_name) + ") c") == "0") {
				RejectDDL("42704", "source_definition", "constraint does not exist");
			}
			native_sql = "ALTER TABLE " + target + " DROP CONSTRAINT " + Identifier(i.constraint_name);
		} else if (i.action == "create_index" || i.action == "create_unique_index") {
			if (i.index_name.empty() || i.keys.empty()) {
				RejectDDL("0A000", "source_definition", "index name and keys required");
			}
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.indexes WHERE object_id=" + obj_id +
			             " AND name=" + Literal(i.index_name)) != "0") {
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42P07", "source_definition", "index already exists on table");
				}
			} else {
				vector<string> cols;
				for (const auto &k : i.keys) cols.push_back(Identifier(k));
				native_sql = string(i.action == "create_unique_index" ? "CREATE UNIQUE INDEX " : "CREATE INDEX ") +
				             Identifier(i.index_name) + " ON " + target + " (" + StringUtil::Join(cols, ",") + ")";
			}
		} else if (i.action == "drop_index") {
			if (i.owner.empty()) {
				if (!i.conditional) {
					RejectDDL("0A000", "source_definition", "owning table required for SQL Server DROP INDEX");
				}
				if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.indexes ix "
				             "JOIN sys.tables t ON t.object_id=ix.object_id "
				             "JOIN sys.schemas s ON s.schema_id=t.schema_id "
				             "WHERE s.name=" +
				             Literal(i.schema) + " AND ix.name=" + Literal(i.name) +
				             " AND t.is_ms_shipped=0 AND ix.index_id>0") == "0") {
					source_noop = true;
				} else {
					RejectDDL("0A000", "source_definition", "owning table required for SQL Server DROP INDEX");
				}
			} else {
				auto obj_id = obj_id_of(i.owner);
				if (obj_id.empty()) {
					if (i.conditional) {
						source_noop = true;
					} else {
						RejectDDL("42P01", "source_definition", "owning table does not exist");
					}
				} else {
					auto ix_rows = s.Rows("SELECT CONVERT(varchar(5),is_primary_key),CONVERT(varchar(5),is_unique_constraint) "
					                      "FROM sys.indexes WHERE object_id=" +
					                      obj_id + " AND name=" + Literal(i.name) + " AND index_id>0");
					if (ix_rows.empty()) {
						if (i.conditional) {
							source_noop = true;
						} else {
							RejectDDL("42704", "source_definition", "index does not exist");
						}
					} else {
						if (ix_rows[0][0] == "1" || ix_rows[0][1] == "1") {
							RejectDDL("2BP01", "source_definition", "cannot drop constraint backing index directly");
						}
						native_sql = "DROP INDEX " + Identifier(i.name) + " ON " + Identifier(i.schema) + "." +
						             Identifier(i.owner);
					}
				}
			}
		} else if (i.action == "rename_index") {
			if (i.owner.empty() || i.new_name.empty() || i.new_name == i.name) {
				RejectDDL("0A000", "source_definition", "invalid rename_index payload");
			}
			auto obj_id = obj_id_of(i.owner);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "owning table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.indexes WHERE object_id=" + obj_id +
			             " AND name=" + Literal(i.name) + " AND index_id>0") == "0") {
				RejectDDL("42704", "source_definition", "index does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.indexes WHERE object_id=" + obj_id +
			             " AND name=" + Literal(i.new_name)) != "0") {
				RejectDDL("42P07", "source_definition", "destination index already exists");
			}
			const string owner_target = Identifier(i.schema) + "." + Identifier(i.owner);
			native_sql = "EXEC sys.sp_rename @objname=" + Literal(owner_target + "." + Identifier(i.name)) +
			             ",@newname=" + Literal(i.new_name) + ",@objtype=N'INDEX'";
			sp_rename_op = true;
		} else if (i.action == "truncate") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.foreign_keys WHERE referenced_object_id=" +
			             obj_id) != "0") {
				RejectDDL("2BP01", "source_definition", "cannot truncate table with referencing foreign keys");
			}
			if (i.identity_mode != "restart" &&
			    s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.columns WHERE object_id=" + obj_id +
			             " AND is_identity=1") != "0") {
				RejectDDL("0A000", "source_definition",
				          "TRUNCATE CONTINUE IDENTITY is not supported on SQL Server identity columns");
			}
			native_sql = "TRUNCATE TABLE " + target;
		} else {
			RejectDDL("0A000", "source_definition", "unsupported SQL Server DDL action");
		}

		stage = "source_execute";
		s.current_stage = "source_execute";
		if (!no_op_commit && !source_noop) {
			if (ctx.IsInterrupted()) {
				RejectDDL("57014", "source_execute", "operation cancelled");
			}
			s.last_warning_count = 0;
			sent = true;
			s.Exec(native_sql, false, sp_rename_op);
			if (s.Scalar("SELECT CASE WHEN @@TRANCOUNT=1 AND XACT_STATE()=1 THEN '1' ELSE '0' END") != "1") {
				RejectDDL("40001", "source_execute", "SQL Server DDL transaction invalidated before commit");
			}
		}

		stage = "source_commit";
		s.current_stage = "source_commit";
		commit_started = true;
		status = "UNKNOWN";
		s.Exec("COMMIT TRANSACTION");
		if (s.Scalar("SELECT CASE WHEN @@TRANCOUNT=0 AND XACT_STATE()=0 THEN '1' ELSE '0' END") != "1") {
			RejectDDL("40001", "source_commit", "SQL Server DDL commit verification failed");
		}
		status = "COMMITTED";
		if (source_noop) {
			warnings = 0;
			error.clear();
			stage = "source_noop";
		} else {
			warnings = s.last_warning_count;
			error.clear();
			stage = warnings > 0 ? "source_warning" : "";
		}
	} catch (const GuardedODBCLocalRejection &) {
		error = status == "COMMITTED" ? "XX000" : "0A000";
		stage = "source_identity";
	} catch (const DDLRejectException &rej) {
		if (source_error.empty() && session) {
			source_error = session->source.JSON();
		}
		error = ValidSQLState(rej.sqlstate, "HY000");
		stage = rej.stage;
	} catch (...) {
		if (source_error.empty() && session) {
			source_error = session->source.JSON();
		}
		error = session && !session->error.empty() ? session->error : (ctx.IsInterrupted() ? "57014" : "08006");
	}

	if (status != "COMMITTED" && in_tx && session) {
		try {
			session->Exec("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION", true);
			auto state_after =
			    session->Scalar("SELECT CASE WHEN @@TRANCOUNT=0 AND XACT_STATE()=0 THEN '1' ELSE '0' END", true);
			if (state_after == "1" && !commit_started) {
				status = sent ? "CONFIRMED_ROLLED_BACK" : "CONFIRMED_NOT_EXECUTED";
				if (stage == "source_lock" || stage == "source_definition") {
					if (status != "CONFIRMED_NOT_EXECUTED") {
						stage = "source_execute";
					}
				}
			} else if (sent || commit_started) {
				status = "UNKNOWN";
			}
		} catch (...) {
			if (sent || commit_started) {
				status = "UNKNOWN";
			}
		}
	}
	if (status == "CONFIRMED_ROLLED_BACK" && (stage == "source_lock" || stage == "source_definition")) {
		stage = "source_execute";
	}
	if (status != "COMMITTED") {
		warnings = 0;
		if (stage == "source_warning" || stage == "source_verify") {
			stage = "source_execute";
		}
		error = ValidSQLState(error, "HY000");
	}

	session.reset();
	if (connection) {
		connection->Close();
	}

	out.SetCardinality(1);
	out.SetValue(0, 0, Value(i.operation));
	out.SetValue(1, 0, Value(status));
	out.SetValue(2, 0, Value(error));
	out.SetValue(3, 0, Value(stage));
	out.SetValue(4, 0, Value::BIGINT(warnings));
	if (d.diagnostics) {
		out.SetValue(5, 0, Value(source_error));
	}
}
} // namespace

void RegisterSQLServerGuardedDDL(ExtensionLoader &loader) {
	loader.RegisterFunction(TableFunction("sqlserver_ddl_capabilities_v1",
	                                      {LogicalType::VARCHAR, LogicalType::VARCHAR}, DDLMetadataScan,
	                                      DDLMetadataBind, DDLInit));
	loader.RegisterFunction(TableFunction(
	    "sqlserver_ddl_describe_v1",
	    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, DDLMetadataScan,
	    DDLMetadataBind, DDLInit));
	TableFunction exec("sqlserver_guarded_ddl_v1",
	                   {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                    LogicalType::ANY},
	                   DDLExecScan, DDLExecBind, DDLInit);
	exec.named_parameters["source_diagnostics"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(exec);
}
} // namespace duckdb
