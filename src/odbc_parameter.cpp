#include "odbc_parameter.hpp"
#include "odbc_types.hpp"

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
	int32_t year, month, day;
	Date::Convert(dd, year, month, day);

	date.year = static_cast<SQLSMALLINT>(std::abs(year));
	date.month = static_cast<SQLUSMALLINT>(std::abs(month));
	date.day = static_cast<SQLUSMALLINT>(std::abs(day));

	bind_buffer.resize(sizeof(SQL_DATE_STRUCT));
	std::memcpy(bind_buffer.data(), &date, sizeof(SQL_DATE_STRUCT));
}

static void FillTimeBuffer(Value &value, vector<char> &bind_buffer) {
	SQL_TIME_STRUCT time;
	std::memset(&time, '\0', sizeof(SQL_TIME_STRUCT));
	dtime_t dt = TimeValue::Get(value);
	int32_t hour, minute, second, micros;
	Time::Convert(dt, hour, minute, second, micros);

	time.hour = static_cast<SQLUSMALLINT>(std::abs(hour));
	time.minute = static_cast<SQLUSMALLINT>(std::abs(minute));
	time.second = static_cast<SQLUSMALLINT>(std::abs(second));

	bind_buffer.resize(sizeof(SQL_TIME_STRUCT));
	std::memcpy(bind_buffer.data(), &time, sizeof(SQL_TIME_STRUCT));
}

static void FillTimestampBuffer(Value &value, vector<char> &bind_buffer) {
	SQL_TIMESTAMP_STRUCT timestamp;
	std::memset(&timestamp, '\0', sizeof(SQL_TIMESTAMP_STRUCT));
	timestamp_t ts = TimestampValue::Get(value);
	date_t dd;
	dtime_t dt;
	Timestamp::Convert(ts, dd, dt);
	int32_t year, month, day;
	Date::Convert(dd, year, month, day);
	int32_t hour, minute, second, micros;
	Time::Convert(dt, hour, minute, second, micros);

	timestamp.year = static_cast<SQLSMALLINT>(std::abs(year));
	timestamp.month = static_cast<SQLUSMALLINT>(std::abs(month));
	timestamp.day = static_cast<SQLUSMALLINT>(std::abs(day));
	timestamp.hour = static_cast<SQLUSMALLINT>(std::abs(hour));
	timestamp.minute = static_cast<SQLUSMALLINT>(std::abs(minute));
	timestamp.second = static_cast<SQLUSMALLINT>(std::abs(second));
	timestamp.fraction = static_cast<SQLUINTEGER>(std::abs(micros)) * 1000; // Convert to nanoseconds

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
		FillDateBuffer(value, bind_buffer);
		break;
	case LogicalTypeId::TIME:
		this->buffer_type = SQL_TYPE_TIME;
		FillTimeBuffer(value, bind_buffer);
		break;
	case LogicalTypeId::TIMESTAMP:
		this->buffer_type = SQL_TYPE_TIMESTAMP;
		FillTimestampBuffer(value, bind_buffer);
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
		this->buffer_type = SQL_TYPE_TIMESTAMP;
		value = value.DefaultCastAs(LogicalType::TIMESTAMP);
		FillTimestampBuffer(value, bind_buffer);
		break;
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::HUGEINT:
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
	case SQL_TYPE_DATE:
	case SQL_TYPE_TIME:
	case SQL_TYPE_TIMESTAMP:
		return SQLBindParameter(stmt, param_idx, SQL_PARAM_INPUT, ODBCTypes::GetCType(buffer_type), buffer_type,
		                        bind_buffer.size(), 0, bind_buffer.data(), bind_buffer.size(), &bind_length);
	case SQL_VARCHAR: {
		string str = value.ToString();
		bind_buffer.resize(str.size() + 1);
		std::memcpy(bind_buffer.data(), str.c_str(), str.size() + 1);
		bind_length = str.size();
		return SQLBindParameter(stmt, param_idx, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, str.size(), 0,
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
