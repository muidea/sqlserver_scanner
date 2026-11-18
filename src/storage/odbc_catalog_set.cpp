#include "storage/odbc_catalog_set.hpp"
#include "storage/odbc_transaction.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "storage/odbc_schema_entry.hpp"

namespace duckdb {

ODBCCatalogSet::ODBCCatalogSet(Catalog &catalog) : catalog(catalog), is_loaded(false) {
}

optional_ptr<CatalogEntry> ODBCCatalogSet::GetEntry(ClientContext &context, const string &name) {
	if (!is_loaded) {
		is_loaded = true;
		LoadEntries(context);
	}
	return GetEntryIfLoaded(name);
}

optional_ptr<CatalogEntry> ODBCCatalogSet::GetEntryIfLoaded(const string &name) {
	lock_guard<mutex> l(entry_lock);
	auto entry = entries.find(name);
	if (entry == entries.end()) {
		return nullptr;
	}
	return entry->second.get();
}

void ODBCCatalogSet::DropEntry(ClientContext &context, DropInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

void ODBCCatalogSet::EraseEntryInternal(const string &name) {
	lock_guard<mutex> l(entry_lock);
	entries.erase(name);
}

void ODBCCatalogSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	if (!is_loaded) {
		is_loaded = true;
		LoadEntries(context);
	}
	lock_guard<mutex> l(entry_lock);
	for (auto &entry : entries) {
		callback(*entry.second);
	}
}

optional_ptr<CatalogEntry> ODBCCatalogSet::CreateEntry(unique_ptr<CatalogEntry> entry) {
	lock_guard<mutex> l(entry_lock);
	auto result = entry.get();
	if (result->name.empty()) {
		throw InternalException("ODBCCatalogSet::CreateEntry called with empty name");
	}
	entries.insert(make_pair(result->name.GetIdentifierName(), std::move(entry)));
	return result;
}

void ODBCCatalogSet::ClearEntries() {
	entries.clear();
	is_loaded = false;
}

ODBCInSchemaSet::ODBCInSchemaSet(ODBCSchemaEntry &schema) : ODBCCatalogSet(schema.ParentCatalog()), schema(schema) {
}

optional_ptr<CatalogEntry> ODBCInSchemaSet::CreateEntry(unique_ptr<CatalogEntry> entry) {
	if (!entry->internal) {
		entry->internal = schema.internal;
	}
	return ODBCCatalogSet::CreateEntry(std::move(entry));
}

} // namespace duckdb
