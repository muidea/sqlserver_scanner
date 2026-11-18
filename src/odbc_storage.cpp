#include "duckdb.hpp"

#include "odbc_storage.hpp"
#include "storage/odbc_catalog.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "storage/odbc_transaction_manager.hpp"

namespace duckdb {

static unique_ptr<Catalog> ODBCAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                      AttachedDatabase &db, const string &name, AttachInfo &info,
                                      AttachOptions &attach_options) {
	auto &config = DBConfig::GetConfig(context);
	if (!Settings::Get<EnableExternalAccessSetting>(config)) {
		throw PermissionException("Attaching SQL Server databases is disabled through configuration");
	}
	// check if we have a secret provided
	string secret_name;
	string schema_name;
	bool guarded_crud_admin = false;
	for (auto &entry : attach_options.options) {
		auto lower_name = StringUtil::Lower(entry.first);
		if (lower_name == "secret") {
			secret_name = entry.second.ToString();
		} else if (lower_name == "schema") {
			schema_name = entry.second.ToString();
#ifdef SQLSERVER_GUARDED_CRUD
		} else if (lower_name == "guarded_crud_admin") {
			guarded_crud_admin = BooleanValue::Get(entry.second);
#endif
		} else {
			throw BinderException("Unrecognized option for SQL Server attach: %s", entry.first);
		}
	}
	if (attach_options.access_mode != AccessMode::READ_ONLY && !guarded_crud_admin) {
		throw BinderException("sqlserver_scanner is read-only; add READ_ONLY to the ATTACH options");
	}

	string attach_path = info.path;
	auto connection_string = ODBCCatalog::GetConnectionString(context, attach_path, secret_name);
	return make_uniq<ODBCCatalog>(db, std::move(connection_string), std::move(attach_path), std::move(schema_name),
	                              attach_options.access_mode);
}

static unique_ptr<TransactionManager> ODBCCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                   AttachedDatabase &db, Catalog &catalog) {
	auto &odbc_catalog = catalog.Cast<ODBCCatalog>();
	return make_uniq<ODBCTransactionManager>(db, odbc_catalog);
}

ODBCStorageExtension::ODBCStorageExtension() {
	attach = ODBCAttach;
	create_transaction_manager = ODBCCreateTransactionManager;
}

} // namespace duckdb
