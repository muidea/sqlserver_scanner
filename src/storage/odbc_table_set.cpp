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
	try {
		auto query = StringUtil::Replace(R"(
SELECT
    t.TABLE_NAME,
    t.TABLE_TYPE,
    c.COLUMN_NAME,
    c.DATA_TYPE,
    c.NUMERIC_PRECISION,
    c.NUMERIC_SCALE,
    c.ORDINAL_POSITION
FROM INFORMATION_SCHEMA.TABLES t
LEFT JOIN INFORMATION_SCHEMA.COLUMNS c
  ON c.TABLE_SCHEMA = t.TABLE_SCHEMA
 AND c.TABLE_NAME = t.TABLE_NAME
WHERE t.TABLE_SCHEMA = ${SCHEMA_NAME}
  AND t.TABLE_TYPE IN ('BASE TABLE', 'VIEW')
ORDER BY t.TABLE_NAME, c.ORDINAL_POSITION
)",
		                                 "${SCHEMA_NAME}", ODBCUtils::WriteLiteral(schema.name));

		unordered_map<string, unique_ptr<ODBCTableInfo>> table_infos;
		ODBCTypeConfig type_config(context);
		auto result = transaction.Query(query);
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
		}

		idx_t loaded_count = 0;
		for (auto &entry : table_infos) {
			if (entry.second->remote_column_names.empty() || GetEntryIfLoaded(entry.first)) {
				continue;
			}
			CreateEntry(make_uniq<ODBCTableEntry>(catalog, schema, *entry.second));
			loaded_count++;
		}
		if (loaded_count > 0) {
			return;
		}
	} catch (...) {
	}

	// Fallback for drivers with incomplete INFORMATION_SCHEMA support.
	try {
		auto query = StringUtil::Replace(R"(
SELECT o.name, CASE WHEN o.type = 'V' THEN 'VIEW' ELSE 'BASE TABLE' END
FROM sys.objects o
JOIN sys.schemas s ON s.schema_id = o.schema_id
WHERE s.name = ${SCHEMA_NAME} AND o.type IN ('U', 'V')
ORDER BY o.name
)",
		                                 "${SCHEMA_NAME}", ODBCUtils::WriteLiteral(schema.name));
		auto result = transaction.Query(query);
		while (result->Next()) {
			auto table_name = NormalizeObjectName(result->GetString(0));
			if (table_name.empty()) {
				continue;
			}
			try {
				auto table_info =
				    GetTableInfo(context, schema, table_name, StringUtil::CIEquals(result->GetString(1), "VIEW"));
				CreateEntry(make_uniq<ODBCTableEntry>(catalog, schema, *table_info));
			} catch (...) {
			}
		}
	} catch (...) {
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
