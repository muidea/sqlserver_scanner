#include "storage/odbc_catalog.hpp"
#include "storage/odbc_schema_entry.hpp"
#include "storage/odbc_table_entry.hpp"
#include "storage/odbc_transaction.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "odbc_scanner.hpp"

namespace duckdb {

static bool TableIsInternal(const SchemaCatalogEntry &schema, const string &name) {
	return false;
}

ODBCTableEntry::ODBCTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
    : TableCatalogEntry(catalog, schema, info), columns(std::move(info.columns)) {
	this->internal = TableIsInternal(schema, name.GetIdentifierName());
}

ODBCTableEntry::ODBCTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, ODBCTableInfo &info)
    : TableCatalogEntry(catalog, schema, *info.create_info), remote_column_names(info.remote_column_names),
      columns(std::move(info.create_info->columns)), is_view(info.is_view) {
	this->internal = TableIsInternal(schema, name.GetIdentifierName());
}

const ColumnList &ODBCTableEntry::GetColumns() const {
	return columns;
}

unique_ptr<BaseStatistics> ODBCTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

void ODBCTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &, LogicalProjection &, LogicalUpdate &update,
                                           ClientContext &) {
	update.update_is_del_and_insert = false;
}

TableFunction ODBCTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<ODBCBindData>(*this);
	idx_t column_index = 0;
	for (auto &col : columns.Logical()) {
		result->types.push_back(col.GetType());
		result->names.push_back(col.GetName().GetIdentifierName());
		if (column_index < remote_column_names.size()) {
			result->remote_names.push_back(remote_column_names[column_index]);
		} else {
			result->remote_names.push_back(col.GetName().GetIdentifierName());
		}
		column_index++;
	}

	bind_data = std::move(result);

	auto function = ODBCScanFunction();
	Value filter_pushdown;
	if (context.TryGetCurrentSetting(Identifier("sqlserver_experimental_filter_pushdown"), filter_pushdown)) {
		function.filter_pushdown = BooleanValue::Get(filter_pushdown);
	}
	return function;
}

TableStorageInfo ODBCTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	result.cardinality = 0;
	return result;
}

} // namespace duckdb
