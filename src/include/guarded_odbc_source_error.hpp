//===----------------------------------------------------------------------===//
//                         DuckDB
//
// guarded_odbc_source_error.hpp
//
//===----------------------------------------------------------------------===//

#pragma once
#include "duckdb.hpp"
#include <sql.h>
#include <sqlext.h>
#include <codecvt>
#include <locale>
#include <algorithm>

namespace duckdb {
// Copied across independent scanner modules to keep the optional native error
// contract identical. The original source state stays separate from the
// normalized protocol SQLSTATE. No execution outcome is inferred here.
struct GuardedODBCSourceError {
	string product, state, code, message, detail;
	static string Quote(const string &value) {
		const char *hex = "0123456789abcdef";
		string out = "\"";
		for (unsigned char ch : value) {
			if (ch == '"' || ch == '\\') {
				out += '\\';
				out += ch;
			} else if (ch < 32) {
				out += "\\u00";
				out += hex[ch >> 4];
				out += hex[ch & 15];
			} else
				out += ch;
		}
		return out + "\"";
	}
	string JSON() const {
		if (message.empty())
			return "";
		return "{\"version\":1,\"product\":" + Quote(product) + ",\"state\":" + Quote(state) +
		       ",\"code\":" + Quote(code) + ",\"message\":" + Quote(message) + ",\"detail\":" + Quote(detail) + "}";
	}
};
// A descriptor/shape refusal is local evidence, never a source connection error.
class GuardedODBCLocalRejection : public InvalidInputException {
public:
	explicit GuardedODBCLocalRejection(const string &message) : InvalidInputException("%s", message) {
	}
};

// Some drivers report a wide message length in bytes instead of SQLWCHARs.
// Length is only an allocation hint; the first bounded terminator ends text.
// Buffers are zeroed before each call so a shorter re-read cannot retain bytes.
template <class CHAR>
inline idx_t GuardedODBCTextLength(const vector<CHAR> &buffer) {
	idx_t count = 0;
	while (count < buffer.size() && buffer[count])
		count++;
	return count;
}
inline GuardedODBCSourceError ReadGuardedODBCSourceError(SQLSMALLINT kind, SQLHANDLE handle, const string &product) {
	GuardedODBCSourceError result;
	result.product = product;
	// SQLSMALLINT is the ODBC per-record length limit. Re-read a long message
	// before another ODBC operation overwrites the diagnostic area.
	for (SQLSMALLINT record = 1; record <= 32; record++) {
		SQLWCHAR state[6] = {};
		SQLINTEGER code = 0;
		SQLSMALLINT length = 0;
		vector<SQLWCHAR> buffer(512);
		auto rc = SQLGetDiagRecW(kind, handle, record, state, &code, buffer.data(), buffer.size(), &length);
		if (rc == SQL_NO_DATA)
			break;
		string native_state, message;
		if (SQL_SUCCEEDED(rc) && length >= 0) {
			if (length >= static_cast<SQLSMALLINT>(buffer.size())) {
				buffer.assign(MinValue<idx_t>(static_cast<idx_t>(length) + 1, 32767), 0);
				rc = SQLGetDiagRecW(kind, handle, record, state, &code, buffer.data(), buffer.size(), &length);
			}
			if (!SQL_SUCCEEDED(rc) || length < 0)
				break;
			for (idx_t i = 0; i < 5 && state[i]; i++)
				native_state += static_cast<char>(state[i]);
			const auto count = GuardedODBCTextLength(buffer);
			const bool truncated = count == buffer.size() ||
			                       (count == buffer.size() - 1 &&
			                        (rc == SQL_SUCCESS_WITH_INFO || static_cast<idx_t>(length) >= buffer.size()));
			std::u16string units;
			for (idx_t i = 0; i < count; i++)
				units += static_cast<char16_t>(buffer[i]);
			// A truncated record may end between a UTF-16 surrogate pair.
			if (truncated && !units.empty() && units.back() >= 0xD800 && units.back() <= 0xDBFF)
				units.pop_back();
			try {
				std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
				message = converter.to_bytes(units);
			} catch (const std::range_error &) {
				// Keep the normalized status if a driver returns invalid Unicode.
				break;
			}
			if (truncated)
				message += " [ODBC record truncated]";
		} else {
			// Some older drivers expose only ANSI diagnostics.
			SQLCHAR ansi_state[6] = {};
			vector<SQLCHAR> bytes(32767);
			rc = SQLGetDiagRec(kind, handle, record, ansi_state, &code, bytes.data(), bytes.size(), &length);
			if (!SQL_SUCCEEDED(rc) || length < 0)
				break;
			for (idx_t i = 0; i < 5 && ansi_state[i]; i++)
				native_state += static_cast<char>(ansi_state[i]);
			const auto count = GuardedODBCTextLength(bytes);
			message.assign(reinterpret_cast<const char *>(bytes.data()), count);
			if (count == bytes.size() || (count == bytes.size() - 1 &&
			                              (rc == SQL_SUCCESS_WITH_INFO || static_cast<idx_t>(length) >= bytes.size())))
				message += " [ODBC record truncated]";
		}
		if (native_state.size() != 5 || native_state == "00000" || message.empty())
			continue;
		string entry = "SQLSTATE=" + native_state + "; native_code=" + std::to_string(code) + "; " + message;
		if (!result.detail.empty())
			result.detail += "\n";
		result.detail += entry;
		if (result.message.empty() || (result.state.substr(0, 2) == "01" && native_state.substr(0, 2) != "01")) {
			result.state = native_state;
			result.code = std::to_string(code);
			result.message = message;
		}
		if (result.detail.size() > 100000) {
			result.detail += "\n[ODBC diagnostic chain truncated]";
			break;
		}
	}
	return result;
}
// A connection failure must survive handle cleanup and the scanner pool.
class GuardedODBCConnectionFailure : public IOException {
public:
	explicit GuardedODBCConnectionFailure(GuardedODBCSourceError diagnostic_p)
	    : IOException("%s", diagnostic_p.message), diagnostic(std::move(diagnostic_p)) {
	}
	GuardedODBCSourceError diagnostic;
};
} // namespace duckdb
