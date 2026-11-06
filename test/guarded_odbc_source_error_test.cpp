#include "guarded_odbc_source_error.hpp"
#include <iostream>
#include <cstring>
using namespace duckdb;
struct Record {
	string state;
	SQLINTEGER code;
	std::u16string message;
};
static vector<Record> records;
static bool wide = true;
static bool byte_length = false;
static string ansi_message = "ansi failure";
static void Require(bool ok) {
	if (!ok)
		throw std::runtime_error("ODBC source diagnostic contract failed");
}
extern "C" SQLRETURN SQL_API SQLGetDiagRecW(SQLSMALLINT, SQLHANDLE, SQLSMALLINT record, SQLWCHAR *state,
                                            SQLINTEGER *code, SQLWCHAR *message, SQLSMALLINT capacity,
                                            SQLSMALLINT *length) {
	if (!wide)
		return SQL_ERROR;
	if (record < 1 || static_cast<idx_t>(record) > records.size())
		return SQL_NO_DATA;
	auto &r = records[record - 1];
	for (int i = 0; i < 5; i++)
		state[i] = r.state[i];
	state[5] = 0;
	*code = r.code;
	*length = MinValue<idx_t>(r.message.size() * (byte_length ? sizeof(SQLWCHAR) : 1), 32767);
	auto count = MinValue<idx_t>(r.message.size(), capacity - 1);
	for (idx_t i = 0; i < count; i++)
		message[i] = r.message[i];
	message[count] = 0;
	return count == r.message.size() ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO;
}
extern "C" SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT, SQLHANDLE, SQLSMALLINT record, SQLCHAR *state, SQLINTEGER *code,
                                           SQLCHAR *message, SQLSMALLINT capacity, SQLSMALLINT *length) {
	if (record != 1)
		return SQL_NO_DATA;
	std::memcpy(state, "HY000", 6);
	*code = -999;
	*length = MinValue<idx_t>(ansi_message.size() * (byte_length ? 2 : 1), 32767);
	auto count = MinValue<idx_t>(ansi_message.size(), capacity - 1);
	std::memcpy(message, ansi_message.data(), count);
	message[count] = 0;
	return SQL_SUCCESS;
}
int main() {
	records = {{"01000", 0, u"warning"}, {"23000", 2627, std::u16string(1500, u'中') + u"\n\"订单\""}};
	auto captured = ReadGuardedODBCSourceError(SQL_HANDLE_STMT, nullptr, "sqlserver");
	Require(captured.state == "23000" && captured.code == "2627" && captured.message.size() > 4500);
	Require(captured.message.find("订单") != string::npos && captured.detail.find("warning") != string::npos);
	Require(captured.JSON().find("\\u000a\\\"") != string::npos && captured.JSON().find("sqlserver") != string::npos);
	records = {{"08006", -1, u"rollback failed"}};
	auto cleanup = ReadGuardedODBCSourceError(SQL_HANDLE_DBC, nullptr, "sqlserver");
	Require(cleanup.state == "08006" && captured.state == "23000" && captured.code == "2627");
	records.clear();
	Require(ReadGuardedODBCSourceError(SQL_HANDLE_STMT, nullptr, "oracle").JSON().empty());
	// Oracle's driver reports bytes for a wide diagnostic on some platforms.
	// A valid terminator, not that count, ends the diagnostic text.
	byte_length = true;
	records = {{"23000", 1, u"ORA-00001: 唯一约束冲突"}};
	auto oracle = ReadGuardedODBCSourceError(SQL_HANDLE_STMT, nullptr, "oracle");
	Require(oracle.message == "ORA-00001: 唯一约束冲突" && oracle.message.find('\0') == string::npos);
	records = {{"23000", 1, std::u16string(800, u'中')}};
	oracle = ReadGuardedODBCSourceError(SQL_HANDLE_STMT, nullptr, "oracle");
	Require(oracle.message ==
	        std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>().to_bytes(records[0].message));
	Require(oracle.detail.find('\0') == string::npos && oracle.message.find("truncated") == string::npos);
	byte_length = false;
	records = {{"HY000", -1, std::u16string(40000, u'x')}};
	auto truncated = ReadGuardedODBCSourceError(SQL_HANDLE_STMT, nullptr, "db2");
	Require(truncated.message.size() > 32000 && truncated.message.find("[ODBC record truncated]") != string::npos);
	wide = false;
	auto fallback = ReadGuardedODBCSourceError(SQL_HANDLE_DBC, nullptr, "db2");
	Require(fallback.state == "HY000" && fallback.code == "-999" && fallback.message == "ansi failure");
	byte_length = true;
	auto ansi_padded = ReadGuardedODBCSourceError(SQL_HANDLE_DBC, nullptr, "db2");
	Require(ansi_padded.message == "ansi failure" && ansi_padded.detail.find('\0') == string::npos);
	ansi_message = string(40000, 'x');
	auto ansi_truncated = ReadGuardedODBCSourceError(SQL_HANDLE_DBC, nullptr, "db2");
	Require(ansi_truncated.message.find("[ODBC record truncated]") != string::npos);
	std::cout << "PASS: long Unicode, diagnostic chain, original state/code, cleanup snapshot, ANSI fallback\n";
}
