//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlserver_temporal_utils.hpp
//
// Helpers for SQL Server temporal range validation, formatting, and offset parsing
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <cctype>
#include <cstdio>

namespace duckdb {

static constexpr const char *SQLSERVER_SESSION_TEMPORAL_SQL = "SET DATEFORMAT ymd";

inline bool IsSQLServerDateYearInRange(int32_t year) {
	return year >= 1 && year <= 9999;
}

inline bool IsSQLServerDateTimeInRange(int32_t year) {
	return year >= 1753 && year <= 9999;
}

inline bool IsSQLServerDateTimeExactTick(int32_t micros) {
	if (micros < 0 || micros > 999999 || (micros % 1000) != 0) {
		return false;
	}
	int32_t last_ms_digit = (micros / 1000) % 10;
	return last_ms_digit == 0 || last_ms_digit == 3 || last_ms_digit == 7;
}

inline bool IsSQLServerTimeInRange(int32_t hour, int32_t minute, int32_t second, int32_t micros) {
	return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59 && micros >= 0 &&
	       micros <= 999999;
}

inline string FormatSQLServerDateISO(int32_t year, int32_t month, int32_t day) {
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
	return string(buf);
}

inline string FormatSQLServerTimeISO(int32_t hour, int32_t minute, int32_t second, int32_t micros) {
	char buf[32];
	if (micros == 0) {
		std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hour, minute, second);
	} else {
		std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06d", hour, minute, second, micros);
	}
	return string(buf);
}

inline string FormatSQLServerTimestampISO(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute,
                                          int32_t second, int32_t micros, bool include_utc_offset) {
	char buf[48];
	if (micros == 0) {
		std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%s", year, month, day, hour, minute, second,
		              include_utc_offset ? "+00:00" : "");
	} else {
		std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06d%s", year, month, day, hour, minute, second,
		              micros, include_utc_offset ? "+00:00" : "");
	}
	return string(buf);
}

inline string FormatSQLServerDateTime126(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t minute,
                                         int32_t second, int32_t micros) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03d", year, month, day, hour, minute, second,
	              micros / 1000);
	return string(buf);
}

inline string NormalizeSQLServerTimestampTZText(string text) {
	StringUtil::Trim(text);
	auto date_time_sep = text.find_first_of(" T");
	if (date_time_sep == string::npos) {
		return text;
	}
	auto offset_pos = text.find_first_of("+-Zz", date_time_sep + 1);
	if (offset_pos == string::npos) {
		return text;
	}
	auto prefix = text.substr(0, offset_pos);
	StringUtil::Trim(prefix);
	auto suffix = text.substr(offset_pos);
	StringUtil::Trim(suffix);
	if (suffix == "z" || suffix == "Z") {
		return prefix + "Z";
	}
	if ((suffix[0] == '+' || suffix[0] == '-') && suffix.size() >= 2 &&
	    std::isdigit(static_cast<unsigned char>(suffix[1])) &&
	    (suffix.size() == 2 || !std::isdigit(static_cast<unsigned char>(suffix[2])))) {
		suffix.insert(1, 1, '0');
	}
	return prefix + suffix;
}

inline bool ParseSQLServerTimestampTZText(const string &raw, timestamp_tz_t &out) {
	string normalized = NormalizeSQLServerTimestampTZText(raw);
	timestamp_t ts_val;
	bool has_offset = false;
	string_t tz(nullptr, 0);
	if (Timestamp::TryConvertTimestampTZ(normalized.c_str(), normalized.size(), ts_val, true, has_offset, tz) !=
	        TimestampCastResult::SUCCESS ||
	    !has_offset || tz.GetSize() != 0) {
		return false;
	}
	out = timestamp_tz_t(ts_val.value);
	return true;
}

} // namespace duckdb
