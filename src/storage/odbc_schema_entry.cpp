#include "storage/odbc_schema_entry.hpp"

namespace duckdb {

ODBCSchemaEntry::ODBCSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info), tables(*this) {
}

optional_ptr<CatalogEntry> ODBCSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

optional_ptr<CatalogEntry> ODBCSchemaEntry::CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

optional_ptr<CatalogEntry> ODBCSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                        TableCatalogEntry &table) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

optional_ptr<CatalogEntry> ODBCSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

optional_ptr<CatalogEntry> ODBCSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

optional_ptr<CatalogEntry> ODBCSchemaEntry::CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

optional_ptr<CatalogEntry> ODBCSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                CreateTableFunctionInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

optional_ptr<CatalogEntry> ODBCSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                               CreateCopyFunctionInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

optional_ptr<CatalogEntry> ODBCSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                 CreatePragmaFunctionInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

optional_ptr<CatalogEntry> ODBCSchemaEntry::CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

void ODBCSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

bool CatalogTypeIsSupported(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return true;
	default:
		return false;
	}
}

void ODBCSchemaEntry::Scan(ClientContext &context, CatalogType type,
                           const std::function<void(CatalogEntry &)> &callback) {
	if (!CatalogTypeIsSupported(type)) {
		return;
	}
	GetCatalogSet(type).Scan(context, callback);
}
void ODBCSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	throw NotImplementedException("Scan without context not supported");
}

void ODBCSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

optional_ptr<CatalogEntry> ODBCSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                        const EntryLookupInfo &lookup_info) {
	auto lookup_type = lookup_info.GetCatalogType();
	if (!CatalogTypeIsSupported(lookup_type)) {
		return nullptr;
	}
	auto &context = transaction.GetContext();
	auto entry_name = lookup_info.GetEntryName();

	// A point lookup must not trigger a full schema scan. Large SQL Server owners can
	// contain thousands of objects, so probe only the requested object and leave
	// bulk loading to catalog scans such as sqlserver_metadata.
	auto &catalog_set = GetCatalogSet(lookup_type);
	auto entry = catalog_set.GetEntryIfLoaded(entry_name);
	if (entry) {
		return entry;
	}

	if (lookup_type == CatalogType::TABLE_ENTRY || lookup_type == CatalogType::VIEW_ENTRY) {
		return tables.RefreshTable(context, entry_name);
	}

	return nullptr;
}

ODBCCatalogSet &ODBCSchemaEntry::GetCatalogSet(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return tables;
	default:
		throw InternalException("Type not supported for GetCatalogSet");
	}
}

} // namespace duckdb
