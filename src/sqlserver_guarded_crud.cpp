//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlserver_guarded_crud.cpp
//
// Restricted SQL Server mutations on an isolated scanner-owned session.
//===----------------------------------------------------------------------===//

#include "duckdb.hpp"
#include "guarded_odbc_source_error.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "storage/odbc_catalog.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <codecvt>
#include <locale>
#include <mutex>
#include <thread>

namespace duckdb {
namespace {
static void Reject() {
	throw GuardedODBCLocalRejection("guarded SQL Server operation rejected");
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
	GuardedODBCSourceError source;
	void Check(SQLRETURN rc) {
		if (rc == SQL_SUCCESS)
			return;
		source = ReadGuardedODBCSourceError(SQL_HANDLE_STMT, stmt.get(), "sqlserver");
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
				error = state[0] ? string(reinterpret_cast<char *>(state)) : "XX000";
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
	void BindInputParams(const vector<Value> &params, vector<int64_t> &values, vector<uint8_t> &bools,
	                     vector<double> &doubles, vector<string> &blobs, vector<vector<SQLWCHAR>> &strings,
	                     vector<SQLLEN> &lengths) {
		static char empty_blob_byte = 0;
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
					if (text.size() > 65536 || text.find('\0') != string::npos)
						Reject();
					std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
					auto utf16 = converter.from_bytes(text);
					units = utf16.size();
					if (units > 32768)
						Reject();
					strings[i].assign(utf16.begin(), utf16.end());
					lengths[i] = units * sizeof(SQLWCHAR);
				}
				static_assert(sizeof(SQLWCHAR) == 2, "guarded SQL Server Unicode requires 16-bit ODBC units");
				strings[i].push_back(0);
				SQLSMALLINT sql_type = units > 4000 ? SQL_WLONGVARCHAR : SQL_WVARCHAR;
				Check(SQLBindParameter(stmt.get(), i + 1, SQL_PARAM_INPUT, SQL_C_WCHAR, sql_type,
				                       MaxValue<idx_t>(1, units), 0, strings[i].data(),
				                       strings[i].size() * sizeof(SQLWCHAR), &lengths[i]));
				continue;
			}
			if (v.type().id() == LogicalTypeId::BOOLEAN) {
				if (v.IsNull()) {
					lengths[i] = SQL_NULL_DATA;
				} else {
					bools[i] = v.GetValue<bool>() ? 1 : 0;
					lengths[i] = sizeof(uint8_t);
				}
				Check(SQLBindParameter(stmt.get(), i + 1, SQL_PARAM_INPUT, SQL_C_BIT, SQL_BIT, 1, 0, &bools[i],
				                       sizeof(uint8_t), &lengths[i]));
				continue;
			}
			if (v.type().id() == LogicalTypeId::FLOAT || v.type().id() == LogicalTypeId::DOUBLE) {
				if (v.IsNull()) {
					lengths[i] = SQL_NULL_DATA;
				} else {
					double dval = v.type().id() == LogicalTypeId::FLOAT ? static_cast<double>(v.GetValue<float>())
					                                                    : v.GetValue<double>();
					if (!std::isfinite(dval))
						Reject();
					doubles[i] = dval;
					lengths[i] = sizeof(double);
				}
				Check(SQLBindParameter(stmt.get(), i + 1, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE, 53, 0,
				                       &doubles[i], sizeof(double), &lengths[i]));
				continue;
			}
			if (v.type().id() == LogicalTypeId::BLOB) {
				if (v.IsNull()) {
					lengths[i] = SQL_NULL_DATA;
				} else {
					blobs[i] = StringValue::Get(v);
					if (blobs[i].size() > 65536)
						Reject();
					lengths[i] = blobs[i].size();
				}
				SQLSMALLINT sql_type = blobs[i].size() > 8000 ? SQL_LONGVARBINARY : SQL_VARBINARY;
				Check(SQLBindParameter(stmt.get(), i + 1, SQL_PARAM_INPUT, SQL_C_BINARY, sql_type,
				                       MaxValue<idx_t>(1, blobs[i].size()), 0,
				                       blobs[i].empty() ? &empty_blob_byte : const_cast<char *>(blobs[i].data()),
				                       blobs[i].size(), &lengths[i]));
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
	}
	int64_t Execute(const string &sql, const vector<Value> &params, bool &sent) {
		Start();
		Check(SQLPrepare(stmt.get(), reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS));
		SQLSMALLINT count = 0;
		Check(SQLNumParams(stmt.get(), &count));
		if (count != params.size() || count > 2100)
			Reject();
		vector<int64_t> values(params.size());
		vector<uint8_t> bools(params.size());
		vector<double> doubles(params.size());
		vector<string> blobs(params.size());
		vector<vector<SQLWCHAR>> strings(params.size());
		vector<SQLLEN> lengths(params.size(), 0);
		BindInputParams(params, values, bools, doubles, blobs, strings, lengths);
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
	int64_t ExecuteReturning(const string &sql, const vector<Value> &params, int64_t returning_oid, bool &sent,
	                         string &returned_rows) {
		Start();
		Check(SQLPrepare(stmt.get(), reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS));
		SQLSMALLINT pcount = 0;
		Check(SQLNumParams(stmt.get(), &pcount));
		if (pcount != params.size() || pcount > 2100)
			Reject();
		vector<int64_t> values(params.size());
		vector<uint8_t> bools(params.size());
		vector<double> doubles(params.size());
		vector<string> blobs(params.size());
		vector<vector<SQLWCHAR>> strings(params.size());
		vector<SQLLEN> lengths(params.size(), 0);
		BindInputParams(params, values, bools, doubles, blobs, strings, lengths);
		if (cancelled.load() || ctx.IsInterrupted()) {
			error = "57014";
			Reject();
		}
		sent = true;
		auto rc = SQLExecute(stmt.get());
		if (rc == SQL_NO_DATA) {
			returned_rows = "[]";
			return 0;
		}
		Check(rc);
		SQLSMALLINT cols = 0;
		Check(SQLNumResultCols(stmt.get(), &cols));
		if (cols != 1)
			Reject();
		returned_rows = "[";
		int64_t row_count = 0;
		while ((rc = SQLFetch(stmt.get())) != SQL_NO_DATA) {
			Check(rc);
			if (++row_count > 4096)
				Reject();
			char buf[256] = {0};
			SQLLEN len = 0;
			Check(SQLGetData(stmt.get(), 1, SQL_C_CHAR, buf, sizeof(buf), &len));
			if (len == SQL_NULL_DATA || len <= 0 || len >= static_cast<SQLLEN>(sizeof(buf)))
				Reject();
			string val(buf, static_cast<size_t>(len));
			if (returning_oid == 2950) {
				if (val.size() != 36)
					Reject();
				for (idx_t k = 0; k < 36; k++) {
					if (k == 8 || k == 13 || k == 18 || k == 23) {
						if (val[k] != '-')
							Reject();
					} else {
						char ch = val[k];
						if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
							Reject();
					}
				}
				val = StringUtil::Lower(val);
			} else if (val.size() > 20 || val.find_first_not_of("0123456789-") != string::npos) {
				Reject();
			}
			if (row_count > 1)
				returned_rows += ",";
			returned_rows += "[" + JSON(val) + "]";
		}
		returned_rows += "]";
		rc = SQLMoreResults(stmt.get());
		if (rc != SQL_NO_DATA) {
			Check(rc);
			Reject();
		}
		return row_count;
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
	bool diagnostics = false;
	bool returning = false;
	string returning_name;
	int64_t returning_oid = 0;
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
                                     vector<duckdb::Identifier> &names) {
	for (auto &v : in.inputs)
		if (v.IsNull())
			Reject();
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, duckdb::Identifier(in.inputs[0].GetValue<string>()));
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
	d->returning = in.table_function.GetName().GetIdentifierName() == "sqlserver_guarded_insert_returning";
	if (d->returning) {
		auto rname = in.named_parameters.find("returning_name"), roid = in.named_parameters.find("returning_oid");
		if (rname == in.named_parameters.end() || roid == in.named_parameters.end() || rname->second.IsNull() ||
		    roid->second.IsNull())
			Reject();
		d->returning_name = rname->second.GetValue<string>();
		d->returning_oid = roid->second.GetValue<int64_t>();
		if (!SimpleName(d->returning_name) ||
		    (d->returning_oid != 20 && d->returning_oid != 21 && d->returning_oid != 23 && d->returning_oid != 2950))
			Reject();
	}
	auto diagnostic = in.named_parameters.find("source_diagnostics");
	if (diagnostic != in.named_parameters.end()) {
		if (diagnostic->second.IsNull())
			Reject();
		d->diagnostics = diagnostic->second.GetValue<bool>();
	}
	names = {"status", "affected_rows", "sqlstate", "stage", "version", "descriptor"};
	types = {LogicalType::VARCHAR, LogicalType::BIGINT,  LogicalType::VARCHAR,
	         LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	if (d->returning) {
		names.push_back("returned_rows");
		types.push_back(LogicalType::VARCHAR);
	}
	if (d->diagnostics) {
		names.push_back("source_error");
		types.push_back(LogicalType::VARCHAR);
	}
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
static string Metadata(Session &s, const Data &d, int64_t *returning_col_oid = nullptr) {
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
	           "COALESCE(c.collation_name,''),CONVERT(varchar(1),c.is_identity),"
	           "CASE WHEN c.default_object_id<>0 THEN '1' ELSE '0' END FROM "
	           "sys.columns c WHERE c.object_id=" +
	           obj +
	           " AND c.system_type_id=c.user_type_id AND "
	           "c.system_type_id IN(35,36,40,41,42,43,48,52,56,58,59,60,61,62,99,104,106,108,122,127,165,167,173,175,231,239) "
	           "AND (c.system_type_id NOT IN(41,42,43) OR c.scale<=7) AND "
	           "c.is_computed=0 AND c.is_rowguidcol=0 AND c.is_filestream=0 AND c.is_sparse=0 AND c.is_column_set=0 "
	           "AND c.is_hidden=0 AND c.generated_always_type=0 AND c.encryption_type IS NULL AND "
	           "c.rule_object_id=0 AND NOT EXISTS(SELECT 1 FROM sys.masked_columns m WHERE "
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
		       ",\"nullable\":" + (c[2] == "1" ? "true" : "false") +
		       ",\"has_default\":" + (c[9] == "1" ? "true" : "false") +
		       ",\"identity\":" + (c[8] == "1" ? "true" : "false");
		int64_t type_oid = 0;
		if (c[1] == "smallint")
			type_oid = 21;
		else if (c[1] == "int")
			type_oid = 23;
		else if (c[1] == "bigint")
			type_oid = 20;
		else if (c[1] == "uniqueidentifier")
			type_oid = 2950;
		if (returning_col_oid && c[0] == d.returning_name)
			*returning_col_oid = type_oid;
		if (type_oid != 0)
			out += ",\"type_oid\":" + std::to_string(type_oid);
		if (c[1] == "decimal" || c[1] == "numeric" || c[1] == "datetime2" || c[1] == "time" ||
		    c[1] == "datetimeoffset") {
			if (c[5].empty() || c[6].empty() || c[5].find_first_not_of("0123456789") != string::npos ||
			    c[6].find_first_not_of("0123456789") != string::npos)
				Reject();
			out += ",\"precision\":" + c[5] + ",\"scale\":" + c[6];
		}
		if (c[1] == "nvarchar" || c[1] == "varchar" || c[1] == "char" || c[1] == "nchar") {
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
	string source_error, returned_rows;
	int64_t affected = -1;
	int64_t ret_oid = 0;
	bool sent = false, commit_started = false, local_rejection = false;
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
		descriptor = Metadata(s, d, d.returning ? &ret_oid : nullptr);
		if (d.execute) {
			stage = "source_descriptor";
			if (descriptor != d.expected) {
				error = "0A000";
				Reject();
			}
			if (d.returning && (ret_oid == 0 || ret_oid != d.returning_oid)) {
				error = "0A000";
				Reject();
			}
			stage = "source_execute";
			affected = d.returning ? s.ExecuteReturning(d.sql, d.params, d.returning_oid, sent, returned_rows)
			                       : s.Execute(d.sql, d.params, sent);
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
		try {
			throw;
		} catch (const GuardedODBCLocalRejection &) {
			local_rejection = true;
		} catch (const GuardedODBCConnectionFailure &failure) {
			auto source = failure.diagnostic;
			source.product = "sqlserver";
			source_error = source.JSON();
			error = source.state;
			stage = "source_connection";
		} catch (...) {
		}
		// Snapshot before rollback/cancellation cleanup replaces handle diagnostics.
		if (session && source_error.empty())
			source_error = session->source.JSON();
		if (error.empty())
			error = session && !session->error.empty()
			            ? session->error
			            : (ctx.IsInterrupted() ? "57014" : (local_rejection ? "0A000" : "XX000"));
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
	out.SetValue(0, 0, Value(status));
	out.SetValue(1, 0, Value::BIGINT(affected));
	out.SetValue(2, 0, Value(error));
	out.SetValue(3, 0, Value(stage));
	out.SetValue(4, 0, Value(version));
	out.SetValue(5, 0, Value(descriptor));
	if (d.returning)
		out.SetValue(6, 0, Value(status == "COMMITTED" ? returned_rows : ""));
	if (d.diagnostics)
		out.SetValue(6 + (d.returning ? 1 : 0), 0, Value(source_error));
	out.SetChildCardinality(1);
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
                                          vector<duckdb::Identifier> &names) {
	if (in.inputs[0].IsNull() || in.inputs[1].IsNull())
		Reject();
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, duckdb::Identifier(in.inputs[0].GetValue<string>()));
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
	out.SetValue(0, 0, Value::BOOLEAN(true));
	out.SetChildCardinality(1);
}
} // namespace
void RegisterSQLServerGuardedCRUD(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    TableFunction("sqlserver_execute", {LogicalType::VARCHAR, LogicalType::VARCHAR}, AdminScan, AdminBind, Init));
	TableFunction metadata("sqlserver_crud_metadata", vector<LogicalType>(3, LogicalType::VARCHAR), Scan, Bind,
	                       Init);
	metadata.GetSignature().WithTypedKwargs(
	    "options", [](TypedKwargs &options) { options.Add("source_diagnostics", LogicalType::BOOLEAN); });
	loader.RegisterFunction(metadata);
	TableFunction exec("sqlserver_guarded_mutation", vector<LogicalType>(6, LogicalType::VARCHAR), Scan, Bind, Init);
	exec.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("params", LogicalType::ANY);
		options.Add("source_diagnostics", LogicalType::BOOLEAN);
	});
	loader.RegisterFunction(exec);
	TableFunction ret("sqlserver_guarded_insert_returning", vector<LogicalType>(6, LogicalType::VARCHAR), Scan, Bind,
	                  Init);
	ret.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("params", LogicalType::ANY);
		options.Add("returning_name", LogicalType::VARCHAR);
		options.Add("returning_oid", LogicalType::BIGINT);
		options.Add("source_diagnostics", LogicalType::BOOLEAN);
	});
	loader.RegisterFunction(ret);
}
} // namespace duckdb
