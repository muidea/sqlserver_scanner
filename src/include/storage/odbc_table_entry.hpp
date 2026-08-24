//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/odbc_table_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "odbc_utils.hpp"

namespace duckdb {

struct ODBCTableInfo {
	ODBCTableInfo() {
		create_info = make_uniq<CreateTableInfo>();
	}
	ODBCTableInfo(const string &schema, const string &table, bool is_view_p = false) : is_view(is_view_p) {
		create_info = make_uniq<CreateTableInfo>(string(), schema, table);
	}
	ODBCTableInfo(const SchemaCatalogEntry &schema, const string &table, bool is_view_p = false) : is_view(is_view_p) {
		create_info = make_uniq<CreateTableInfo>((SchemaCatalogEntry &)schema, table);
	}

	const string &GetTableName() const {
		return create_info->table;
	}

	unique_ptr<CreateTableInfo> create_info;
	vector<string> remote_column_names;
	bool is_view = false;
};

class ODBCTableEntry : public TableCatalogEntry {
public:
	ODBCTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info);
	ODBCTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, ODBCTableInfo &info);

public:
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;

	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;

	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;

	vector<string> remote_column_names;
	bool IsView() const {
		return is_view;
	}

private:
	bool is_view = false;
};

} // namespace duckdb
