#define DUCKDB_BUILD_LOADABLE_EXTENSION
#include "duckdb.hpp"

#include "odbc_scanner.hpp"
#include "odbc_storage.hpp"
#include "sqlserver_scanner_extension.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "storage/odbc_catalog.hpp"
#include "storage/odbc_optimizer.hpp"

using namespace duckdb;
#ifdef SQLSERVER_GUARDED_CRUD
namespace duckdb {
void RegisterSQLServerGuardedCRUD(ExtensionLoader &loader);
void RegisterSQLServerGuardedDDL(ExtensionLoader &loader);
}
#endif

static void SetODBCDebugQueryPrint(ClientContext &context, SetScope scope, Value &parameter) {
	ODBCConnection::DebugSetPrintQueries(BooleanValue::Get(parameter));
}

template <typename Fn>
static void ForEachODBCCatalog(ClientContext &context, Fn &&fn) {
	auto databases = DatabaseManager::Get(context).GetDatabases(context);
	for (auto &db_ref : databases) {
		auto &db = *db_ref;
		auto &catalog = db.GetCatalog();
		if (catalog.GetCatalogType() != "sqlserver") {
			continue;
		}
		fn(catalog.Cast<ODBCCatalog>());
	}
}

static void SetODBCConnectionLimit(ClientContext &context, SetScope scope, Value &parameter) {
	if (scope == SetScope::LOCAL) {
		throw InvalidInputException("sqlserver_connection_limit can only be set globally");
	}
	auto new_max = parameter.GetValue<uint64_t>();
	ForEachODBCCatalog(context,
	                   [&](ODBCCatalog &catalog) { catalog.GetConnectionPool().SetMaximumConnections(new_max); });
	auto &config = DBConfig::GetConfig(context);
	config.SetOption("sqlserver_connection_limit", parameter);
}

static void SetODBCConnectionCache(ClientContext &context, SetScope scope, Value &parameter) {
	if (scope == SetScope::LOCAL) {
		throw InvalidInputException("sqlserver_connection_cache can only be set globally");
	}
	ODBCConnectionPool::ODBCSetConnectionCache(context, scope, parameter);
	auto &config = DBConfig::GetConfig(context);
	config.SetOption("sqlserver_connection_cache", parameter);
}

unique_ptr<BaseSecret> CreateODBCSecretFunction(ClientContext &, CreateSecretInput &input) {
	// apply any overridden settings
	vector<string> prefix_paths;
	auto result = make_uniq<KeyValueSecret>(prefix_paths, "sqlserver", "config", input.name);
	for (const auto &named_param : input.options) {
		auto lower_name = StringUtil::Lower(named_param.first);

		if (lower_name == "driver") {
			result->secret_map["driver"] = named_param.second.ToString();
		} else if (lower_name == "server") {
			result->secret_map["server"] = named_param.second.ToString();
		} else if (lower_name == "database") {
			result->secret_map["database"] = named_param.second.ToString();
		} else if (lower_name == "uid") {
			result->secret_map["uid"] = named_param.second.ToString();
		} else if (lower_name == "pwd") {
			result->secret_map["pwd"] = named_param.second.ToString();
		} else if (lower_name == "port") {
			result->secret_map["port"] = named_param.second.ToString();
		} else {
			throw InternalException("Unknown named parameter passed to CreateODBCSecretFunction: " + lower_name);
		}
	}

	//! Set redact keys
	result->redact_keys = {"pwd", "password"};
	return std::move(result);
}

void SetODBCSecretParameters(CreateSecretFunction &function) {
	function.named_parameters["driver"] = LogicalType::VARCHAR;
	function.named_parameters["server"] = LogicalType::VARCHAR;
	function.named_parameters["port"] = LogicalType::VARCHAR;
	function.named_parameters["uid"] = LogicalType::VARCHAR;
	function.named_parameters["pwd"] = LogicalType::VARCHAR;
	function.named_parameters["database"] = LogicalType::VARCHAR;
}

static void LoadInternal(ExtensionLoader &loader) {
#ifdef SQLSERVER_GUARDED_CRUD
	RegisterSQLServerGuardedCRUD(loader);
	RegisterSQLServerGuardedDDL(loader);
#endif
	// ODBC doesn't require explicit library initialization like MySQL
	// The ODBC driver manager handles initialization automatically
	ODBCClearCacheFunction clear_cache_func;
	loader.RegisterFunction(clear_cache_func);

	ODBCQueryFunction query_function;
	loader.RegisterFunction(query_function);

	ODBCMetadataFunction metadata_function;
	loader.RegisterFunction(metadata_function);

	// Declare the supported forms explicitly. Instance initialization can
	// inspect the signatures without executing a source-dependent binder.
	ODBCRelationalMetadataFunction relational_metadata_function;
	TableFunctionSet relational_metadata("sqlserver_relational_metadata");
	relational_metadata.AddFunction(relational_metadata_function);
	relational_metadata_function.GetSignature().AddParameter(LogicalType::VARCHAR);
	relational_metadata.AddFunction(relational_metadata_function);
	relational_metadata_function.GetSignature().AddParameter(LogicalType::LIST(LogicalType::VARCHAR));
	relational_metadata.AddFunction(relational_metadata_function);
	loader.RegisterFunction(relational_metadata);

	SecretType secret_type;
	secret_type.name = "sqlserver";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";

	loader.RegisterSecretType(secret_type);

	CreateSecretFunction odbc_secret_function = {"sqlserver", "config", CreateODBCSecretFunction};
	SetODBCSecretParameters(odbc_secret_function);
	loader.RegisterFunction(odbc_secret_function);

	auto &db = loader.GetDatabaseInstance();

	auto &config = DBConfig::GetConfig(db);
	StorageExtension::Register(config, "sqlserver_scanner", make_shared_ptr<ODBCStorageExtension>());

	config.AddExtensionOption("sqlserver_experimental_filter_pushdown",
	                          "Whether or not to use filter pushdown (currently experimental)", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(true));
	config.AddExtensionOption("sqlserver_debug_show_queries",
	                          "DEBUG SETTING: print all queries sent to SQL Server to stdout", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(false), SetODBCDebugQueryPrint);
	config.AddExtensionOption("sqlserver_tinyint1_as_boolean",
	                          "Whether or not to convert TINYINT(1) columns to BOOLEAN", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(true), ODBCClearCacheFunction::ClearCacheOnSetting);
	config.AddExtensionOption("sqlserver_bit1_as_boolean", "Whether or not to convert BIT(1) columns to BOOLEAN",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true), ODBCClearCacheFunction::ClearCacheOnSetting);
	config.AddExtensionOption("sqlserver_time_as_time",
	                          "Whether or not to convert SQL Server TIME columns to DuckDB TIME", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(false), ODBCClearCacheFunction::ClearCacheOnSetting);
	config.AddExtensionOption("sqlserver_connection_limit", "Maximum number of connections per SQL Server catalog",
	                          LogicalType::UBIGINT, Value::UBIGINT(ODBCConnectionPool::DEFAULT_MAX_CONNECTIONS),
	                          SetODBCConnectionLimit);
	config.AddExtensionOption("sqlserver_connection_cache", "Whether or not to cache idle SQL Server connections",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true), SetODBCConnectionCache);

	OptimizerExtension odbc_optimizer;
	odbc_optimizer.optimize_function = ODBCOptimizer::Optimize;
	OptimizerExtension::Register(config, std::move(odbc_optimizer));
}

void SQLServerScannerExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(sqlserver_scanner, loader) {
	LoadInternal(loader);
}
}
