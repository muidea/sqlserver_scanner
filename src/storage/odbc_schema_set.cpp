#include "storage/odbc_schema_set.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "storage/odbc_catalog.hpp"
#include "storage/odbc_transaction.hpp"

#include <algorithm>

namespace duckdb {

static bool ODBCSchemaIsInternal(const string &name) {
	auto lower = StringUtil::Lower(name);
	return lower == "information_schema" || lower == "sys" || lower == "guest";
}

static string NormalizeSchemaName(string name) {
	name.erase(std::remove(name.begin(), name.end(), '\0'), name.end());
	StringUtil::Trim(name);
	return name;
}

ODBCSchemaSet::ODBCSchemaSet(Catalog &catalog) : ODBCCatalogSet(catalog) {
}

void ODBCSchemaSet::LoadEntries(ClientContext &context) {
	auto &transaction = ODBCTransaction::Get(context, catalog);
	vector<string> schema_names;
	try {
		auto result = transaction.Query(R"(
SELECT name
FROM sys.schemas
ORDER BY name
)");
		while (result->Next()) {
			auto schema_name = NormalizeSchemaName(result->GetString(0));
			if (!schema_name.empty() && !ODBCSchemaIsInternal(schema_name)) {
				schema_names.push_back(std::move(schema_name));
			}
		}
	} catch (...) {
		try {
			auto result = transaction.Query(R"(
SELECT SCHEMA_NAME
FROM INFORMATION_SCHEMA.SCHEMATA
ORDER BY SCHEMA_NAME
)");
			while (result->Next()) {
				auto schema_name = NormalizeSchemaName(result->GetString(0));
				if (!schema_name.empty() && !ODBCSchemaIsInternal(schema_name)) {
					schema_names.push_back(std::move(schema_name));
				}
			}
		} catch (...) {
		}
	}

	auto default_schema_opt = catalog.Cast<ODBCCatalog>().GetDefaultSchema();
	if (default_schema_opt.has_value() && !default_schema_opt->GetIdentifierName().empty()) {
		schema_names.push_back(default_schema_opt->GetIdentifierName());
	}
	if (schema_names.empty()) {
		schema_names.emplace_back("dbo");
	}

	unordered_set<string> created;
	for (auto &schema_name : schema_names) {
		auto normalized = NormalizeSchemaName(std::move(schema_name));
		if (normalized.empty() || ODBCSchemaIsInternal(normalized) || !created.insert(normalized).second) {
			continue;
		}
		CreateSchemaInfo info;
		info.SetSchema(Identifier(normalized));
		info.internal = false;
		CreateEntry(make_uniq<ODBCSchemaEntry>(catalog, info));
	}
}

optional_ptr<CatalogEntry> ODBCSchemaSet::CreateSchema(ClientContext &context, CreateSchemaInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

} // namespace duckdb
