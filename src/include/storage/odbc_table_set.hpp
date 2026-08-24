//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/odbc_table_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/odbc_catalog_set.hpp"
#include "storage/odbc_table_entry.hpp"

namespace duckdb {
class ODBCSchemaEntry;

class ODBCTableSet : public ODBCInSchemaSet {
public:
	explicit ODBCTableSet(ODBCSchemaEntry &schema);

public:
	static unique_ptr<ODBCTableInfo> GetTableInfo(ClientContext &context, ODBCSchemaEntry &schema,
	                                              const string &table_name, bool is_view = false);
	optional_ptr<CatalogEntry> RefreshTable(ClientContext &context, const string &table_name);

protected:
	void LoadEntries(ClientContext &context) override;
};

} // namespace duckdb
