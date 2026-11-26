#include "odbc_parameter.hpp"
#include "odbc_types.hpp"
#include "sqlserver_temporal_utils.hpp"

#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

template <typename NUM_TYPE>
static void FillNumberBuffer(Value &value, vector<char> &bind_buffer) {
	bind_buffer.resize(sizeof(NUM_TYPE));
	NUM_TYPE val = value.GetValueUnsafe<NUM_TYPE>();
	std::memcpy(bind_buffer.data(), &val, sizeof(NUM_TYPE));
}

static void FillDateBuffer(Value &value, vector<char> &bind_buffer) {
	SQL_DATE_STRUCT date;
	std::memset(&date, '\0', sizeof(SQL_DATE_STRUCT));
	date_t dd = DateValue::Get(value);
	if (!dd.IsFinite()) {
		throw ConversionException("SQL Server DATE parameter must be finite: %s", value.ToString());
	}
	int32_t year, month, day;
	Date::Convert(dd, year, month, day);
	if (!IsSQLServerDateYearInRange(year)) {
		throw ConversionException("SQL Server DATE parameter out of supported range (0001-01-01..9999-12-31): %s",
		                          value.ToString());
	}

	date.year = static_cast<SQLSMALLINT>(year);
	date.month = static_cast<SQLUSMALLINT>(month);
	date.day = static_cast<SQLUSMALLINT>(day);

	bind_buffer.resize(sizeof(SQL_DATE_STRUCT));
	std::memcpy(bind_buffer.data(), &date, sizeof(SQL_DATE_STRUCT));
}

static void FillTimestampBuffer(timestamp_t ts, const Value &orig_value, vector<char> &bind_buffer) {
	SQL_TIMESTAMP_STRUCT timestamp;
	std::memset(&timestamp, '\0', sizeof(SQL_TIMESTAMP_STRUCT));
	if (!ts.IsFinite()) {
		throw ConversionException("SQL Server TIMESTAMP parameter must be finite: %s", orig_value.ToString());
	}
	date_t dd;
	dtime_t dt;
	Timestamp::Convert(ts, dd, dt);
	int32_t year, month, day;
	Date::Convert(dd, year, month, day);
	if (!IsSQLServerDateYearInRange(year)) {
		throw ConversionException(
		    "SQL Server TIMESTAMP parameter out of supported range (0001-01-01..9999-12-31): %s", orig_value.ToString());
	}
	int32_t hour, minute, second, micros;
	Time::Convert(dt, hour, minute, second, micros);

	timestamp.year = static_cast<SQLSMALLINT>(year);
	timestamp.month = static_cast<SQLUSMALLINT>(month);
	timestamp.day = static_cast<SQLUSMALLINT>(day);
	timestamp.hour = static_cast<SQLUSMALLINT>(hour);
	timestamp.minute = static_cast<SQLUSMALLINT>(minute);
	timestamp.second = static_cast<SQLUSMALLINT>(second);
	timestamp.fraction = static_cast<SQLUINTEGER>(micros) * 1000; // Convert to nanoseconds

	bind_buffer.resize(sizeof(SQL_TIMESTAMP_STRUCT));
	std::memcpy(bind_buffer.data(), &timestamp, sizeof(SQL_TIMESTAMP_STRUCT));
}

ODBCParameter::ODBCParameter(const string &query, Value value_p) : value(std::move(value_p)) {
	if (value.IsNull()) {
		return;
	}

	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		this->buffer_type = SQL_TINYINT;
		FillNumberBuffer<int8_t>(value, bind_buffer);
		break;
	case LogicalTypeId::TINYINT:
		this->buffer_type = SQL_TINYINT;
		FillNumberBuffer<int8_t>(value, bind_buffer);
		break;
	case LogicalTypeId::UTINYINT:
		this->buffer_type = SQL_TINYINT;
		this->is_unsigned = true;
		FillNumberBuffer<uint8_t>(value, bind_buffer);
		break;
	case LogicalTypeId::SMALLINT:
		this->buffer_type = SQL_SMALLINT;
		FillNumberBuffer<int16_t>(value, bind_buffer);
		break;
	case LogicalTypeId::USMALLINT:
		this->buffer_type = SQL_SMALLINT;
		this->is_unsigned = true;
		FillNumberBuffer<uint16_t>(value, bind_buffer);
		break;
	case LogicalTypeId::INTEGER:
		this->buffer_type = SQL_INTEGER;
		FillNumberBuffer<int32_t>(value, bind_buffer);
		break;
	case LogicalTypeId::UINTEGER:
		this->buffer_type = SQL_INTEGER;
		this->is_unsigned = true;
		FillNumberBuffer<uint32_t>(value, bind_buffer);
		break;
	case LogicalTypeId::BIGINT:
		this->buffer_type = SQL_BIGINT;
		FillNumberBuffer<int64_t>(value, bind_buffer);
		break;
	case LogicalTypeId::UBIGINT:
		this->buffer_type = SQL_BIGINT;
		this->is_unsigned = true;
		FillNumberBuffer<uint64_t>(value, bind_buffer);
		break;
	case LogicalTypeId::FLOAT:
		this->buffer_type = SQL_REAL;
		FillNumberBuffer<float>(value, bind_buffer);
		break;
	case LogicalTypeId::DOUBLE:
		this->buffer_type = SQL_DOUBLE;
		FillNumberBuffer<double>(value, bind_buffer);
		break;
	case LogicalTypeId::DATE:
		this->buffer_type = SQL_TYPE_DATE;
		this->column_size = 10;
		this->decimal_digits = 0;
		FillDateBuffer(value, bind_buffer);
		break;
	case LogicalTypeId::TIME: {
		dtime_t dt = TimeValue::Get(value);
		int32_t hour, minute, second, micros;
		Time::Convert(dt, hour, minute, second, micros);
		if (!IsSQLServerTimeInRange(hour, minute, second, micros)) {
			throw ConversionException("SQL Server TIME parameter out of range (00:00:00..23:59:59.999999): %s",
			                          value.ToString());
		}
		this->buffer_type = SQL_VARCHAR;
		this->use_text_buffer = true;
		this->text_buffer = FormatSQLServerTimeISO(hour, minute, second, micros);
		break;
	}
	case LogicalTypeId::TIMESTAMP:
		this->buffer_type = SQL_TYPE_TIMESTAMP;
		this->column_size = 26;
		this->decimal_digits = 6;
		FillTimestampBuffer(TimestampValue::Get(value), value, bind_buffer);
		break;
	case LogicalTypeId::TIMESTAMP_TZ: {
		timestamp_t ts(value.GetValueUnsafe<int64_t>());
		if (!ts.IsFinite()) {
			throw ConversionException("SQL Server TIMESTAMP_TZ parameter must be finite: %s", value.ToString());
		}
		date_t dd;
		dtime_t dt;
		Timestamp::Convert(ts, dd, dt);
		int32_t year, month, day, hour, minute, second, micros;
		Date::Convert(dd, year, month, day);
		if (!IsSQLServerDateYearInRange(year)) {
			throw ConversionException(
			    "SQL Server TIMESTAMP_TZ parameter out of supported range (0001-01-01..9999-12-31): %s",
			    value.ToString());
		}
		Time::Convert(dt, hour, minute, second, micros);
		this->buffer_type = SQL_VARCHAR;
		this->use_text_buffer = true;
		this->text_buffer = FormatSQLServerTimestampISO(year, month, day, hour, minute, second, micros, true);
		break;
	}
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UUID:
	case LogicalTypeId::VARCHAR:
		this->buffer_type = SQL_VARCHAR;
		break;
	case LogicalTypeId::BLOB:
		this->buffer_type = SQL_VARBINARY;
		break;
	default:
		throw IOException("Unsupported parameters type: \"%s\", ODBC query \"%s\"", value.type(), query.c_str());
	}
}

SQLRETURN ODBCParameter::BindParameter(SQLHSTMT stmt, SQLUSMALLINT param_idx) {
	if (value.IsNull()) {
		bind_length = SQL_NULL_DATA;
		return SQLBindParameter(stmt, param_idx, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 1, 0, nullptr, 0,
		                        &bind_length);
	}

	switch (buffer_type) {
	case SQL_TINYINT:
	case SQL_SMALLINT:
	case SQL_INTEGER:
	case SQL_BIGINT:
	case SQL_REAL:
	case SQL_DOUBLE:
		bind_length = NumericCast<SQLLEN>(bind_buffer.size());
		return SQLBindParameter(stmt, param_idx, SQL_PARAM_INPUT, ODBCTypes::GetCType(buffer_type), buffer_type,
		                        bind_buffer.size(), 0, bind_buffer.data(), bind_buffer.size(), &bind_length);
	case SQL_TYPE_DATE:
	case SQL_TYPE_TIME:
	case SQL_TYPE_TIMESTAMP:
		bind_length = NumericCast<SQLLEN>(bind_buffer.size());
		return SQLBindParameter(stmt, param_idx, SQL_PARAM_INPUT, ODBCTypes::GetCType(buffer_type), buffer_type,
		                        column_size > 0 ? column_size : bind_buffer.size(), decimal_digits, bind_buffer.data(),
		                        bind_buffer.size(), &bind_length);
	case SQL_VARCHAR: {
		if (!use_text_buffer) {
			text_buffer = value.ToString();
		}
		bind_buffer.resize(text_buffer.size() + 1);
		std::memcpy(bind_buffer.data(), text_buffer.c_str(), text_buffer.size() + 1);
		bind_length = text_buffer.size();
		return SQLBindParameter(stmt, param_idx, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, text_buffer.size(), 0,
		                        bind_buffer.data(), bind_buffer.size(), &bind_length);
	}
	case SQL_VARBINARY: {
		auto blob = StringValue::Get(value);
		bind_buffer.assign(blob.begin(), blob.end());
		bind_length = NumericCast<SQLLEN>(bind_buffer.size());
		return SQLBindParameter(stmt, param_idx, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_VARBINARY, bind_buffer.size(), 0,
		                        bind_buffer.data(), bind_buffer.size(), &bind_length);
	}
	default:
		return SQL_ERROR;
	}
}

} // namespace duckdb
