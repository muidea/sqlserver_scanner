#include "odbc_types.hpp"

namespace duckdb {

static bool GetBoolOption(ClientContext &context, const string &name, bool default_val = false) {
	Value value;
	if (context.TryGetCurrentSetting(Identifier(name), value) && !value.IsNull()) {
		return BooleanValue::Get(value);
	}
	return default_val;
}

static bool GetBoolOption(DatabaseInstance &db, const string &name, bool default_val = false) {
	Value value;
	if (db.TryGetCurrentSetting(Identifier(name), value) && !value.IsNull()) {
		return BooleanValue::Get(value);
	}
	return default_val;
}

ODBCTypeConfig::ODBCTypeConfig() {
}

ODBCTypeConfig::ODBCTypeConfig(ClientContext &context)
    : bit1_as_boolean(GetBoolOption(context, "sqlserver_bit1_as_boolean")),
      tinyint1_as_boolean(GetBoolOption(context, "sqlserver_tinyint1_as_boolean")),
      time_as_time(GetBoolOption(context, "sqlserver_time_as_time", true)) {
}

ODBCTypeConfig::ODBCTypeConfig(DatabaseInstance &db)
    : bit1_as_boolean(GetBoolOption(db, "sqlserver_bit1_as_boolean")),
      tinyint1_as_boolean(GetBoolOption(db, "sqlserver_tinyint1_as_boolean")),
      time_as_time(GetBoolOption(db, "sqlserver_time_as_time", true)) {
}

LogicalType ODBCTypes::TypeToLogicalType(const ODBCTypeConfig &type_config, const ODBCTypeData &type_info) {
	auto type_name = StringUtil::Lower(type_info.type_name);
	StringUtil::Trim(type_name);

	if (type_name == "bit" || type_name == "boolean") {
		return LogicalType::BOOLEAN;
	}
	if (type_name == "tinyint") {
		return LogicalType::UTINYINT;
	}
	if (type_name == "smallint") {
		return LogicalType::SMALLINT;
	}
	if (type_name == "int" || type_name == "integer") {
		return LogicalType::INTEGER;
	}
	if (type_name == "bigint") {
		return LogicalType::BIGINT;
	}
	if (type_name == "decimal" || type_name == "numeric" || type_name == "money" || type_name == "smallmoney") {
		auto precision = type_info.precision;
		auto scale = type_info.scale < 0 ? 0 : type_info.scale;
		if (type_name == "money") {
			precision = 19;
			scale = 4;
		} else if (type_name == "smallmoney") {
			precision = 10;
			scale = 4;
		}
		if (precision > 0 && precision <= 38 && scale <= precision) {
			return LogicalType::DECIMAL(NumericCast<uint8_t>(precision), NumericCast<uint8_t>(scale));
		}
		return LogicalType::DOUBLE;
	}
	if (type_name == "real") {
		return LogicalType::FLOAT;
	}
	if (type_name == "float" || type_name == "double" || type_name == "double precision") {
		return LogicalType::DOUBLE;
	}
	if (type_name == "date") {
		return LogicalType::DATE;
	}
	if (type_name == "time") {
		return type_config.time_as_time ? LogicalType::TIME : LogicalType::VARCHAR;
	}
	if (type_name == "datetimeoffset") {
		return LogicalType::TIMESTAMP_TZ;
	}
	if (type_name == "datetime" || type_name == "datetime2" || type_name == "smalldatetime") {
		return LogicalType::TIMESTAMP;
	}
	if (type_name == "uniqueidentifier") {
		return LogicalType::UUID;
	}
	if (type_name == "timestamp" || type_name == "rowversion" || type_name == "binary" || type_name == "varbinary" ||
	    type_name == "image" || type_name == "longvarbinary") {
		return LogicalType::BLOB;
	}
	if (type_name == "char" || type_name == "varchar" || type_name == "text" || type_name == "nchar" ||
	    type_name == "nvarchar" || type_name == "ntext" || type_name == "xml" || type_name == "sql_variant") {
		return LogicalType::VARCHAR;
	}
	return LogicalType::VARCHAR;
}

LogicalType ODBCTypes::FieldToLogicalType(const ODBCTypeConfig &type_config, SQLSMALLINT data_type, SQLULEN column_size,
                                          SQLSMALLINT decimal_digits, const string &reported_type_name) {
	ODBCTypeData type_info;
	type_info.type_name = StringUtil::Lower(reported_type_name);
	type_info.column_type = type_info.type_name;
	type_info.precision = NumericCast<int64_t>(column_size);
	type_info.scale = decimal_digits;
	if (!type_info.type_name.empty()) {
		return TypeToLogicalType(type_config, type_info);
	}

	switch (data_type) {
	case SQL_BIT:
		type_info.type_name = "bit";
		break;
	case SQL_TINYINT:
		type_info.type_name = "tinyint";
		break;
	case SQL_SMALLINT:
		type_info.type_name = "smallint";
		break;
	case SQL_INTEGER:
		type_info.type_name = "int";
		break;
	case SQL_BIGINT:
		type_info.type_name = "bigint";
		break;
	case SQL_DECIMAL:
	case SQL_NUMERIC:
		type_info.type_name = "decimal";
		break;
	case SQL_REAL:
		type_info.type_name = "real";
		break;
	case SQL_FLOAT:
	case SQL_DOUBLE:
		type_info.type_name = "float";
		break;
	case SQL_DATE:
	case SQL_TYPE_DATE:
		type_info.type_name = "date";
		break;
	case SQL_TIME:
	case SQL_TYPE_TIME:
		type_info.type_name = "time";
		break;
	case SQL_TIMESTAMP:
	case SQL_TYPE_TIMESTAMP:
		type_info.type_name = "datetime2";
		break;
	case SQL_BINARY:
	case SQL_VARBINARY:
	case SQL_LONGVARBINARY:
		type_info.type_name = "varbinary";
		break;
	case SQL_GUID:
		type_info.type_name = "uniqueidentifier";
		break;
	case -154: // SQL_SS_TIME2
		type_info.type_name = "time";
		break;
	case -155: // SQL_SS_TIMESTAMPOFFSET
		type_info.type_name = "datetimeoffset";
		break;
	default:
		type_info.type_name = "varchar";
		break;
	}
	type_info.column_type = type_info.type_name;
	return TypeToLogicalType(type_config, type_info);
}

SQLSMALLINT ODBCTypes::GetCType(SQLSMALLINT sql_type) {
	switch (sql_type) {
	case SQL_CHAR:
	case SQL_VARCHAR:
	case SQL_LONGVARCHAR:
		return SQL_C_CHAR;
	case SQL_WCHAR:
	case SQL_WVARCHAR:
	case SQL_WLONGVARCHAR:
		return SQL_C_WCHAR;
	case SQL_TINYINT:
		return SQL_C_UTINYINT;
	case SQL_SMALLINT:
		return SQL_C_SSHORT;
	case SQL_INTEGER:
		return SQL_C_SLONG;
	case SQL_BIGINT:
		return SQL_C_SBIGINT;
	case SQL_REAL:
		return SQL_C_FLOAT;
	case SQL_FLOAT:
	case SQL_DOUBLE:
		return SQL_C_DOUBLE;
	case SQL_BIT:
		return SQL_C_BIT;
	case SQL_BINARY:
	case SQL_VARBINARY:
	case SQL_LONGVARBINARY:
		return SQL_C_BINARY;
	case SQL_DATE:
	case SQL_TYPE_DATE:
		return SQL_C_TYPE_DATE;
	case SQL_TIME:
	case SQL_TYPE_TIME:
		return SQL_C_TYPE_TIME;
	case SQL_TIMESTAMP:
	case SQL_TYPE_TIMESTAMP:
		return SQL_C_TYPE_TIMESTAMP;
	case SQL_DECIMAL:
	case SQL_NUMERIC:
	case SQL_GUID:
	default:
		return SQL_C_CHAR;
	}
}

LogicalType ODBCTypes::ODBCToLogicalType(const ODBCTypeConfig &type_config, SQLSMALLINT data_type, SQLULEN column_size,
                                         SQLSMALLINT decimal_digits, const string &type_name) {
	return FieldToLogicalType(type_config, data_type, column_size, decimal_digits, type_name);
}

} // namespace duckdb
