#include "storage/odbc_table_set.hpp"

#include "duckdb/common/string_util.hpp"
#include "storage/odbc_schema_entry.hpp"
#include "storage/odbc_transaction.hpp"

#include "odbc_types.hpp"

#include <algorithm>

namespace duckdb {

ODBCTableSet::ODBCTableSet(ODBCSchemaEntry &schema) : ODBCInSchemaSet(schema) {
}

static string NormalizeObjectName(string name) {
	name.erase(std::remove(name.begin(), name.end(), '\0'), name.end());
	StringUtil::Trim(name);
	return name;
}

static bool ODBCSchemaIsInternal(const string &name) {
	auto lower = StringUtil::Lower(name);
	return lower == "information_schema" || lower == "sys";
}

void ODBCTableSet::LoadEntries(ClientContext &context) {
	if (ODBCSchemaIsInternal(schema.name)) {
		return;
	}

	auto &transaction = ODBCTransaction::Get(context, catalog);
	// Bracket precision/scale: both names are reserved-word-sensitive on
	// newer parsers, while SQL Server 2000 does not expose sys.objects/sys.columns
	// at all. Select the legacy catalog only from the proven server version;
	// an arbitrary modern-query error must remain visible instead of silently
	// changing metadata semantics.
	auto version_result = transaction.Query("SELECT CAST(SERVERPROPERTY('ProductVersion') AS varchar(128))");
	if (!version_result->Next()) {
		throw InvalidInputException("SQL Server product version query returned no rows");
	}
	auto version = version_result->GetString(0);
	bool legacy;
	try {
		legacy = std::stoi(version.substr(0, version.find('.'))) < 9;
	} catch (...) {
		throw InvalidInputException("Unable to parse SQL Server product version \"%s\"", version);
	}
	auto modern_query = StringUtil::Replace(R"(
SELECT
    CAST(t.name AS varchar(128)) AS TABLE_NAME,
    CAST(CASE WHEN t.type = 'V' THEN 'VIEW' ELSE 'BASE TABLE' END AS varchar(32)) AS TABLE_TYPE,
    CAST(c.name AS varchar(128)) AS COLUMN_NAME,
    CAST(ty.name AS varchar(128)) AS DATA_TYPE,
    CAST(c.[precision] AS bigint) AS NUMERIC_PRECISION,
    CAST(c.[scale] AS bigint) AS NUMERIC_SCALE,
    CAST(c.column_id AS bigint) AS ORDINAL_POSITION
FROM sys.objects t
JOIN sys.schemas s ON s.schema_id = t.schema_id
JOIN sys.columns c ON c.object_id = t.object_id
JOIN sys.types ty ON ty.user_type_id = c.user_type_id
WHERE s.name = ${SCHEMA_NAME}
  AND t.is_ms_shipped = 0
  AND t.type IN ('U', 'V')
ORDER BY t.name, c.column_id
)",
	                                        "${SCHEMA_NAME}", ODBCUtils::WriteLiteral(schema.name));
	auto legacy_query = StringUtil::Replace(R"(
SELECT
    CAST(t.name AS varchar(128)) AS TABLE_NAME,
    CAST(CASE WHEN t.xtype = 'V' THEN 'VIEW' ELSE 'BASE TABLE' END AS varchar(32)) AS TABLE_TYPE,
    CAST(c.name AS varchar(128)) AS COLUMN_NAME,
    CAST(ty.name AS varchar(128)) AS DATA_TYPE,
    CAST(c.prec AS bigint) AS NUMERIC_PRECISION,
    CAST(c.scale AS bigint) AS NUMERIC_SCALE,
    CAST(c.colid AS bigint) AS ORDINAL_POSITION
FROM sysobjects t
JOIN syscolumns c ON c.id = t.id
JOIN systypes ty ON ty.xusertype = c.xusertype
WHERE t.uid = USER_ID(${SCHEMA_NAME})
  AND t.xtype IN ('U', 'V')
ORDER BY t.name, c.colid
)",
	                                        "${SCHEMA_NAME}", ODBCUtils::WriteLiteral(schema.name));
	auto result = transaction.Query(legacy ? legacy_query : modern_query);

	unordered_map<string, unique_ptr<ODBCTableInfo>> table_infos;
	ODBCTypeConfig type_config(context);
	while (result->Next()) {
		auto table_name = NormalizeObjectName(result->GetString(0));
		if (table_name.empty()) {
			continue;
		}
		auto &table_info = table_infos[table_name];
		if (!table_info) {
			auto table_type = StringUtil::Upper(result->GetString(1));
			table_info = make_uniq<ODBCTableInfo>(schema, table_name, table_type == "VIEW");
		}
		if (result->IsNull(2)) {
			continue;
		}

		auto remote_column_name = result->GetString(2);
		auto column_name = NormalizeObjectName(remote_column_name);
		if (column_name.empty()) {
			continue;
		}
		ODBCTypeData type_info;
		type_info.type_name = StringUtil::Lower(result->GetString(3));
		type_info.column_type = type_info.type_name;
		type_info.precision = result->IsNull(4) ? -1 : result->GetInt64(4);
		type_info.scale = result->IsNull(5) ? -1 : result->GetInt64(5);

		ColumnDefinition column(std::move(column_name), ODBCTypes::TypeToLogicalType(type_config, type_info));
		table_info->create_info->columns.AddColumn(std::move(column));
		table_info->remote_column_names.push_back(std::move(remote_column_name));
		table_info->remote_type_names.push_back(type_info.type_name);
	}

	for (auto &entry : table_infos) {
		if (entry.second->remote_column_names.empty() || GetEntryIfLoaded(entry.first)) {
			continue;
		}
		CreateEntry(make_uniq<ODBCTableEntry>(catalog, schema, *entry.second));
	}
}

static string GetTableInfoQuery(const string &schema_name, const string &table_name) {
	return "SELECT TOP 0 * FROM " + ODBCUtils::WriteIdentifier(schema_name) + "." +
	       ODBCUtils::WriteIdentifier(table_name);
}

unique_ptr<ODBCTableInfo> ODBCTableSet::GetTableInfo(ClientContext &context, ODBCSchemaEntry &schema,
                                                     const string &table_name, bool is_view) {
	auto &transaction = ODBCTransaction::Get(context, schema.ParentCatalog());
	auto result = transaction.Query(GetTableInfoQuery(schema.name, table_name));
	auto table_info = make_uniq<ODBCTableInfo>(schema, table_name, is_view);
	for (auto &field : result->Fields()) {
		auto remote_column_name = field.name;
		auto column_name = NormalizeObjectName(field.name);
		if (column_name.empty()) {
			continue;
		}
		ColumnDefinition column(std::move(column_name), field.duckdb_type);
		table_info->create_info->columns.AddColumn(std::move(column));
		table_info->remote_column_names.push_back(std::move(remote_column_name));
		table_info->remote_type_names.push_back(StringUtil::Lower(field.type_name));
	}
	return table_info;
}

optional_ptr<CatalogEntry> ODBCTableSet::RefreshTable(ClientContext &context, const string &table_name) {
	auto table_info = GetTableInfo(context, schema, table_name);
	auto table_entry = make_uniq<ODBCTableEntry>(catalog, schema, *table_info);
	auto table_ptr = table_entry.get();
	CreateEntry(std::move(table_entry));
	return table_ptr;
}

} // namespace duckdb
