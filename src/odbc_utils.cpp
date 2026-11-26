#include "guarded_odbc_source_error.hpp"
#include "odbc_utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <tuple>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <sql.h>
#include <sqlext.h>

#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "sqlserver_temporal_utils.hpp"

#include "storage/odbc_schema_entry.hpp"
#include "storage/odbc_transaction.hpp"

namespace duckdb {

#ifndef _WIN32
static std::mutex odbc_ini_mutex;
#endif

static bool ParseValue(const string &dsn, idx_t &pos, string &result) {
	// skip leading spaces
	while (pos < dsn.size() && StringUtil::CharacterIsSpace(dsn[pos])) {
		pos++;
	}
	if (pos >= dsn.size()) {
		return false;
	}
	// check if we are parsing a quoted value or not
	if (dsn[pos] == '"') {
		pos++;
		// scan until we find another quote
		bool found_quote = false;
		for (; pos < dsn.size(); pos++) {
			if (dsn[pos] == '"') {
				found_quote = true;
				pos++;
				break;
			}
			if (dsn[pos] == '\\') {
				// backslash escapes the backslash or double-quote
				if (pos + 1 >= dsn.size()) {
					throw InvalidInputException("Invalid dsn \"%s\" - backslash at end of dsn", dsn);
				}
				if (dsn[pos + 1] != '\\' && dsn[pos + 1] != '"') {
					throw InvalidInputException("Invalid dsn \"%s\" - backslash can only escape \\ or \"", dsn);
				}
				result += dsn[pos + 1];
				pos++;
			} else {
				result += dsn[pos];
			}
		}
		if (!found_quote) {
			throw InvalidInputException("Invalid dsn \"%s\" - unterminated quote", dsn);
		}
	} else {
		// unquoted value, continue until space, equality sign or end of string
		for (; pos < dsn.size(); pos++) {
			if (dsn[pos] == '=') {
				break;
			}
			if (StringUtil::CharacterIsSpace(dsn[pos])) {
				break;
			}
			result += dsn[pos];
		}
	}
	return true;
}

static bool ParseBoolValue(const string &value) {
	return value != "0" && !StringUtil::CIEquals(value, "false");
}

bool ReadOptionFromEnv(const char *env, string &result) {
	auto res = std::getenv(env);
	if (!res) {
		return false;
	}
	result = res;
	return true;
}

uint32_t ParsePort(const string &value) {
	constexpr const static int PORT_MIN = 0;
	constexpr const static int PORT_MAX = 65535;
	int port_val = std::stoi(value);
	if (port_val < PORT_MIN || port_val > PORT_MAX) {
		throw InvalidInputException("Invalid port %d - port must be between %d and %d", port_val, PORT_MIN, PORT_MAX);
	}
	return uint32_t(port_val);
}

string EscapeODBCConnectionValue(const string &value) {
	if (value.empty()) {
		return value;
	}
	if (value.find_first_of(";{}") == string::npos) {
		return value;
	}
	string result = "{";
	for (auto c : value) {
		if (c == '}') {
			result += "}}";
		} else {
			result += c;
		}
	}
	result += "}";
	return result;
}

#ifndef _WIN32
bool HasExtraParam(const vector<string> &extra_params, const string &name) {
	auto lower_name = StringUtil::Lower(name);
	for (auto &extra : extra_params) {
		auto separator = extra.find('=');
		auto key = separator == string::npos ? extra : extra.substr(0, separator);
		if (StringUtil::Lower(key) == lower_name) {
			return true;
		}
	}
	return false;
}

void WriteODBCIniValue(std::ofstream &file, const string &key, const string &value) {
	if (value.find_first_of("\r\n") != string::npos) {
		throw InvalidInputException("Invalid ODBC connection value for \"%s\" - newlines are not supported", key);
	}
	file << key << "=" << value << "\n";
}

string CreateTemporaryODBCIni(const ODBCConnectionParameters &config, string &dsn_name) {
	char path_template[] = "/tmp/duckdb_odbc_XXXXXX";
	auto fd = mkstemp(path_template);
	if (fd < 0) {
		throw IOException("Failed to create temporary ODBC ini file");
	}
	close(fd);

	string path = path_template;
	auto slash = path.find_last_of('/');
	dsn_name = slash == string::npos ? path : path.substr(slash + 1);

	std::ofstream file(path);
	if (!file.is_open()) {
		std::remove(path.c_str());
		throw IOException("Failed to open temporary ODBC ini file \"%s\"", path);
	}

	file << "[" << dsn_name << "]\n";
	WriteODBCIniValue(file, "Driver", config.driver);
	WriteODBCIniValue(file, "Server", config.server);
	if (config.port != 0) {
		WriteODBCIniValue(file, "Port", std::to_string(config.port));
	}
	if (!config.database.empty()) {
		WriteODBCIniValue(file, "Database", config.database);
	}
	for (auto &extra : config.extra_params) {
		auto separator = extra.find('=');
		if (separator == string::npos) {
			continue;
		}
		WriteODBCIniValue(file, extra.substr(0, separator), extra.substr(separator + 1));
	}
	if (!HasExtraParam(config.extra_params, "ClientCharset")) {
		WriteODBCIniValue(file, "ClientCharset", "UTF-8");
	}
	if (!HasExtraParam(config.extra_params, "TDS_Version")) {
		WriteODBCIniValue(file, "TDS_Version", "auto");
	}

	file.close();
	if (!file.good()) {
		std::remove(path.c_str());
		throw IOException("Failed to write temporary ODBC ini file \"%s\"", path);
	}
	return path;
}

bool ShouldUseSQLConnectPasswordFallback(const ODBCConnectionParameters &config) {
	return !config.driver.empty() && !config.server.empty() && !config.uid.empty() && !config.pwd.empty() &&
	       config.pwd.find('}') != string::npos;
}

SQLRETURN SQLConnectWithTemporaryDSN(SQLHDBC dbc, const ODBCConnectionParameters &config) {
	string dsn_name;
	auto odbc_ini_path = CreateTemporaryODBCIni(config, dsn_name);

	lock_guard<std::mutex> lock(odbc_ini_mutex);
	auto old_odbc_ini = std::getenv("ODBCINI");
	string old_odbc_ini_value;
	if (old_odbc_ini) {
		old_odbc_ini_value = old_odbc_ini;
	}
	setenv("ODBCINI", odbc_ini_path.c_str(), 1);

	auto ret = SQLConnect(dbc, (SQLCHAR *)dsn_name.c_str(), SQL_NTS, (SQLCHAR *)config.uid.c_str(), SQL_NTS,
	                      (SQLCHAR *)config.pwd.c_str(), SQL_NTS);

	if (old_odbc_ini) {
		setenv("ODBCINI", old_odbc_ini_value.c_str(), 1);
	} else {
		unsetenv("ODBCINI");
	}
	std::remove(odbc_ini_path.c_str());
	return ret;
}
#endif

std::tuple<ODBCConnectionParameters, unordered_set<string>> ODBCUtils::ParseConnectionParameters(const string &dsn) {
	ODBCConnectionParameters result;

	unordered_set<string> set_options;
	// parse options from ODBC connection string format: "key1=value1;key2=value2;"
	idx_t pos = 0;
	while (pos < dsn.size()) {
		string key;
		string value;

		// Parse key
		while (pos < dsn.size() && dsn[pos] != '=' && dsn[pos] != ';') {
			key += dsn[pos];
			pos++;
		}

		if (pos >= dsn.size() || dsn[pos] != '=') {
			throw InvalidInputException("Invalid SQL Server connection string - expected key=value pairs separated by "
			                            "semicolons");
		}
		pos++; // skip '='

		// Parse value
		if (pos < dsn.size() && dsn[pos] == '{') {
			pos++; // skip '{'
			bool found_closing_brace = false;
			while (pos < dsn.size()) {
				if (dsn[pos] == '}') {
					if (pos + 1 < dsn.size() && dsn[pos + 1] == '}') {
						value += '}';
						pos += 2;
						continue;
					}
					pos++; // skip closing '}'
					found_closing_brace = true;
					break;
				}
				value += dsn[pos];
				pos++;
			}
			if (!found_closing_brace) {
				throw InvalidInputException("Invalid SQL Server connection string - unterminated braced value");
			}
			if (pos < dsn.size() && dsn[pos] != ';') {
				throw InvalidInputException(
				    "Invalid SQL Server connection string - expected semicolon after braced value");
			}
		} else {
			while (pos < dsn.size() && dsn[pos] != ';') {
				value += dsn[pos];
				pos++;
			}
		}

		if (pos < dsn.size() && dsn[pos] == ';') {
			pos++; // skip ';'
		}

		auto original_key = key;
		key = StringUtil::Lower(key);

		if (set_options.find(key) != set_options.end()) {
			throw InvalidInputException("Duplicate '%s' parameter in connection string. Each parameter "
			                            "should only be specified once.",
			                            key);
		}

		// Handle standard ODBC connection parameters
		if (key == "driver") {
			set_options.insert("driver");
			result.driver = value;
		} else if (key == "server") {
			set_options.insert("server");
			result.server = value;
		} else if (key == "database") {
			set_options.insert("database");
			result.database = value;
		} else if (key == "uid") {
			set_options.insert("uid");
			result.uid = value;
		} else if (key == "pwd") {
			set_options.insert("pwd");
			result.pwd = value;
		} else if (key == "port") {
			set_options.insert("port");
			result.port = ParsePort(value);
		} else {
			// Preserve driver-specific parameters (like TDS_Version) verbatim
			result.extra_params.push_back(original_key + "=" + value);
			continue;
		}
	}

	// For ODBC, connection parameters are specified in the connection string
	return std::make_tuple(result, set_options);
}

string ODBCUtils::BuildConnectionString(const string &dsn) {
	ODBCConnectionParameters config;
	unordered_set<string> unused_params;
	std::tie(config, unused_params) = ParseConnectionParameters(dsn);

	string connection_string;
	if (!config.driver.empty()) {
		connection_string += "DRIVER=" + EscapeODBCConnectionValue(config.driver) + ";";
	}
	if (!config.server.empty()) {
		connection_string += "SERVER=" + EscapeODBCConnectionValue(config.server) + ";";
	}
	if (!config.uid.empty()) {
		connection_string += "UID=" + EscapeODBCConnectionValue(config.uid) + ";";
	}
	if (!config.pwd.empty()) {
		connection_string += "PWD=" + EscapeODBCConnectionValue(config.pwd) + ";";
	}
	if (!config.database.empty()) {
		connection_string += "DATABASE=" + EscapeODBCConnectionValue(config.database) + ";";
	}
	if (config.port != 0) {
		connection_string += "PORT=" + std::to_string(config.port) + ";";
	}

	// Append driver-specific / unrecognized parameters verbatim
	for (auto &extra : config.extra_params) {
		auto separator = extra.find('=');
		if (separator == string::npos) {
			connection_string += extra + ";";
			continue;
		}
		connection_string +=
		    extra.substr(0, separator + 1) + EscapeODBCConnectionValue(extra.substr(separator + 1)) + ";";
	}

#ifndef _WIN32
	auto lower_driver = StringUtil::Lower(config.driver);
	if (StringUtil::Contains(lower_driver, "freetds")) {
		if (StringUtil::Lower(connection_string).find("clientcharset") == string::npos) {
			connection_string += "ClientCharset=UTF-8;";
		}
		if (StringUtil::Lower(connection_string).find("tds_version") == string::npos) {
			connection_string += "TDS_Version=auto;";
		}
	}
#endif

	return connection_string;
}

SQLHDBC ODBCUtils::Connect(const string &dsn, SQLHENV &out_env) {
	SQLHENV env = SQL_NULL_HENV;
	SQLHDBC dbc = SQL_NULL_HDBC;
	SQLRETURN ret;

	ret = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
	if (!SQL_SUCCEEDED(ret)) {
		throw IOException("Failed to allocate ODBC environment handle");
	}

	ret = SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
	if (!SQL_SUCCEEDED(ret)) {
		SQLFreeHandle(SQL_HANDLE_ENV, env);
		throw IOException("Failed to set ODBC version");
	}

	ret = SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
	if (!SQL_SUCCEEDED(ret)) {
		SQLFreeHandle(SQL_HANDLE_ENV, env);
		throw IOException("Failed to allocate ODBC connection handle");
	}

	// Bound login before a guarded operation can install its statement watcher.
	ret = SQLSetConnectAttr(dbc, SQL_ATTR_LOGIN_TIMEOUT, reinterpret_cast<SQLPOINTER>(10), 0);
	if (!SQL_SUCCEEDED(ret)) {
		SQLFreeHandle(SQL_HANDLE_DBC, dbc);
		SQLFreeHandle(SQL_HANDLE_ENV, env);
		throw IOException("Unable to configure bounded SQL Server login");
	}

	ODBCConnectionParameters config;
	unordered_set<string> unused_params;
	std::tie(config, unused_params) = ParseConnectionParameters(dsn);

#ifndef _WIN32
	if (ShouldUseSQLConnectPasswordFallback(config)) {
		ret = SQLConnectWithTemporaryDSN(dbc, config);
	} else
#endif
	{
		string connection_string = BuildConnectionString(dsn);

		SQLCHAR outstr[1024];
		SQLSMALLINT outstrlen;
		ret = SQLDriverConnect(dbc, NULL, (SQLCHAR *)connection_string.c_str(), SQL_NTS, outstr, sizeof(outstr),
		                       &outstrlen, SQL_DRIVER_NOPROMPT);
	}

	if (!SQL_SUCCEEDED(ret)) {
		auto source = ReadGuardedODBCSourceError(SQL_HANDLE_DBC, dbc, "");
		SQLFreeHandle(SQL_HANDLE_DBC, dbc);
		SQLFreeHandle(SQL_HANDLE_ENV, env);
		if (!source.message.empty())
			throw GuardedODBCConnectionFailure(std::move(source));
		throw IOException("ODBC connection failed without a source diagnostic");
	}

	out_env = env;
	return dbc;
}

string ODBCUtils::EscapeQuotes(const string &text, char quote) {
	string result;
	for (auto c : text) {
		if (c == quote) {
			result += string(2, quote);
		} else {
			result += c;
		}
	}
	return result;
}

string ODBCUtils::WriteQuoted(const string &text, char quote) {
	return string(1, quote) + EscapeQuotes(text, quote) + string(1, quote);
}

string ODBCUtils::WriteIdentifier(const string &identifier) {
	// For SQL Server ODBC, use square brackets for identifiers
	// We need to handle square brackets specially since they are different from quotes
	string result;
	result += '[';
	result += EscapeQuotes(identifier, ']'); // Escape any closing brackets in the identifier
	result += ']';
	return result;
}

string ODBCUtils::WriteLiteral(const string &identifier) {
	return ODBCUtils::WriteQuoted(identifier, '\'');
}

static string TransformBlobToODBC(const string &val) {
	char const HEX_DIGITS[] = "0123456789ABCDEF";

	string result = "0x";
	for (idx_t i = 0; i < val.size(); i++) {
		uint8_t byte_val = static_cast<uint8_t>(val[i]);
		result += HEX_DIGITS[(byte_val >> 4) & 0xf];
		result += HEX_DIGITS[byte_val & 0xf];
	}
	return result;
}

bool ODBCUtils::TryTransformConstant(const Value &val, const string &remote_type_name, string &out) {
	if (val.IsNull()) {
		out = "NULL";
		return true;
	}
	if (val.type().id() == LogicalTypeId::BOOLEAN) {
		out = BooleanValue::Get(val) ? "1" : "0";
		return true;
	}
	if (val.type().IsNumeric()) {
		out = val.ToSQLString();
		return true;
	}
	if (val.type().id() == LogicalTypeId::BLOB) {
		out = TransformBlobToODBC(StringValue::Get(val));
		return true;
	}
	if (val.type().id() == LogicalTypeId::DATE) {
		date_t d = DateValue::Get(val);
		if (!Date::IsFinite(d)) {
			return false;
		}
		int32_t year, month, day;
		Date::Convert(d, year, month, day);
		if (!IsSQLServerDateYearInRange(year)) {
			return false;
		}
		out = "CAST(" + WriteLiteral(FormatSQLServerDateISO(year, month, day)) + " AS DATE)";
		return true;
	}
	if (val.type().id() == LogicalTypeId::TIME) {
		dtime_t tm = TimeValue::Get(val);
		int32_t hour, minute, second, micros;
		Time::Convert(tm, hour, minute, second, micros);
		if (!IsSQLServerTimeInRange(hour, minute, second, micros)) {
			return false;
		}
		out = "CAST(" + WriteLiteral(FormatSQLServerTimeISO(hour, minute, second, micros)) + " AS TIME(6))";
		return true;
	}
	if (val.type().id() == LogicalTypeId::TIMESTAMP) {
		timestamp_t ts = TimestampValue::Get(val);
		if (!Timestamp::IsFinite(ts)) {
			return false;
		}
		date_t d;
		dtime_t tm;
		Timestamp::Convert(ts, d, tm);
		int32_t year, month, day, hour, minute, second, micros;
		Date::Convert(d, year, month, day);
		Time::Convert(tm, hour, minute, second, micros);
		auto lower_remote = StringUtil::Lower(remote_type_name);
		StringUtil::Trim(lower_remote);
		if (lower_remote == "datetime") {
			if (!IsSQLServerDateTimeInRange(year) || !IsSQLServerDateTimeExactTick(micros)) {
				return false;
			}
			out = "CONVERT(datetime, " + WriteLiteral(FormatSQLServerDateTime126(year, month, day, hour, minute, second, micros)) +
			      ", 126)";
			return true;
		}
		if (!IsSQLServerDateYearInRange(year)) {
			return false;
		}
		out = "CAST(" +
		      WriteLiteral(FormatSQLServerTimestampISO(year, month, day, hour, minute, second, micros, false)) +
		      " AS DATETIME2(6))";
		return true;
	}
	if (val.type().id() == LogicalTypeId::TIMESTAMP_TZ) {
		timestamp_t ts(val.GetValueUnsafe<int64_t>());
		if (!Timestamp::IsFinite(ts)) {
			return false;
		}
		date_t d;
		dtime_t tm;
		Timestamp::Convert(ts, d, tm);
		int32_t year, month, day, hour, minute, second, micros;
		Date::Convert(d, year, month, day);
		Time::Convert(tm, hour, minute, second, micros);
		if (!IsSQLServerDateYearInRange(year)) {
			return false;
		}
		out = "CAST(" + WriteLiteral(FormatSQLServerTimestampISO(year, month, day, hour, minute, second, micros, true)) +
		      " AS DATETIMEOFFSET(6))";
		return true;
	}
	out = val.DefaultCastAs(LogicalType::VARCHAR).ToSQLString();
	return true;
}

string ODBCUtils::TransformConstant(const Value &val) {
	string result;
	if (!TryTransformConstant(val, "", result)) {
		throw ConversionException("Value cannot be pushed to SQL Server: %s", val.ToString());
	}
	return result;
}

} // namespace duckdb
