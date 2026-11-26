#include "duckdb.hpp"

#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "odbc_scanner.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "storage/odbc_catalog.hpp"

namespace duckdb {

struct ClearCacheFunctionData : public TableFunctionData {
	string catalog;
};

struct ClearCacheGlobalState : public GlobalTableFunctionState {
	bool finished = false;
};

static unique_ptr<GlobalTableFunctionState> ClearCacheInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ClearCacheGlobalState>();
}

static ODBCCatalog &ResolveAttachedODBCCatalog(ClientContext &context, const string &catalog_name) {
	if (catalog_name.empty()) {
		throw BinderException("scoped cache retirement requires a catalog");
	}
	auto db = DatabaseManager::Get(context).GetDatabase(context, Identifier(catalog_name));
	if (!db) {
		throw BinderException("Failed to find attached database \"%s\" for scoped cache retirement", catalog_name);
	}
	auto &catalog = db->GetCatalog();
	if (catalog.GetCatalogType() != "sqlserver") {
		throw BinderException("Attached database \"%s\" is not a SQL Server catalog", catalog_name);
	}
	return catalog.Cast<ODBCCatalog>();
}

static unique_ptr<FunctionData> ClearCacheBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<ClearCacheFunctionData>();
	if (!input.inputs.empty()) {
		if (input.inputs[0].IsNull()) {
			throw BinderException("scoped cache retirement requires a catalog");
		}
		result->catalog = input.inputs[0].GetValue<string>();
		ResolveAttachedODBCCatalog(context, result->catalog);
	}
	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

static void ClearODBCCaches(ClientContext &context) {
	auto databases = DatabaseManager::Get(context).GetDatabases(context);
	for (auto &db_ref : databases) {
		auto &db = *db_ref;
		auto &catalog = db.GetCatalog();
		if (catalog.GetCatalogType() != "sqlserver") {
			continue;
		}
		catalog.Cast<ODBCCatalog>().ClearCache();
	}
}

static void ClearCacheFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<ClearCacheFunctionData>();
	auto &state = data_p.global_state->Cast<ClearCacheGlobalState>();
	if (state.finished) {
		return;
	}
	if (data.catalog.empty()) {
		ClearODBCCaches(context);
	} else {
		ResolveAttachedODBCCatalog(context, data.catalog).ClearCache();
	}
	state.finished = true;
}

void ODBCClearCacheFunction::ClearCacheOnSetting(ClientContext &context, SetScope scope, Value &parameter) {
	ClearODBCCaches(context);
}

ODBCClearCacheFunction::ODBCClearCacheFunction()
    : TableFunction("sqlserver_clear_cache", {}, ClearCacheFunction, ClearCacheBind, ClearCacheInit) {
}

ODBCClearCacheScopedFunction::ODBCClearCacheScopedFunction()
    : TableFunction("sqlserver_clear_cache_scoped", {LogicalType::VARCHAR}, ClearCacheFunction, ClearCacheBind,
                    ClearCacheInit) {
}
} // namespace duckdb
