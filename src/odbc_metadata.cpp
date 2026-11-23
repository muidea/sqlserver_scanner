#include "odbc_scanner.hpp"

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "storage/odbc_catalog.hpp"
#include "storage/odbc_table_entry.hpp"

namespace duckdb {

struct ODBCMetadataRow {
	string table_schema;
	string table_name;
	string table_type;
	string table_comment;
	string column_name;
	string data_type;
	int64_t ordinal_position;
	string column_comment;
};

struct ODBCMetadataBindData : public FunctionData {
	vector<ODBCMetadataRow> rows;

public:
	unique_ptr<FunctionData> Copy() const override {
		throw NotImplementedException("ODBCMetadataBindData copy not supported");
	}
	bool Equals(const FunctionData &other_p) const override {
		return false;
	}
};

struct ODBCMetadataGlobalState : public GlobalTableFunctionState {
	explicit ODBCMetadataGlobalState(vector<ODBCMetadataRow> rows_p) : rows(std::move(rows_p)) {
	}

	vector<ODBCMetadataRow> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static bool ODBCMetadataSchemaIsInternal(const string &name) {
	auto lower = StringUtil::Lower(name);
	return lower == "information_schema" || lower == "performance_schema" || lower == "sys";
}

static void ODBCMetadataAppendSchema(ClientContext &context, ODBCMetadataBindData &bind_data,
                                     SchemaCatalogEntry &schema) {
	if (ODBCMetadataSchemaIsInternal(schema.name.GetIdentifierName())) {
		return;
	}
	schema.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
		auto &table = entry.Cast<TableCatalogEntry>();
		auto &sqlserver_table = entry.Cast<ODBCTableEntry>();
		int64_t ordinal = 1;
		for (auto &column : table.GetColumns().Logical()) {
			ODBCMetadataRow row;
			row.table_schema = schema.name.GetIdentifierName();
			row.table_name = table.name.GetIdentifierName();
			row.table_type = sqlserver_table.IsView() ? "VIEW" : "BASE TABLE";
			row.table_comment = "";
			row.column_name = column.GetName().GetIdentifierName();
			row.data_type = column.GetType().ToString();
			row.ordinal_position = ordinal++;
			row.column_comment = "";
			bind_data.rows.push_back(std::move(row));
		}
	});
}

static unique_ptr<FunctionData> ODBCMetadataBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("Parameters to sqlserver_metadata cannot be NULL");
	}

	names = {Identifier("table_schema"), Identifier("table_name"),  Identifier("table_type"),
	         Identifier("table_comment"), Identifier("column_name"), Identifier("data_type"),
	         Identifier("ordinal_position"), Identifier("column_comment")};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER, LogicalType::VARCHAR};

	auto db_name = input.inputs[0].GetValue<string>();
	string schema_filter;
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		schema_filter = input.inputs[1].GetValue<string>();
		StringUtil::Trim(schema_filter);
	}
	auto &db_manager = DatabaseManager::Get(context);
	auto db = db_manager.GetDatabase(context, Identifier(db_name));
	if (!db) {
		throw BinderException("Failed to find attached database \"%s\" referenced in sqlserver_metadata", db_name);
	}
	auto &catalog = db->GetCatalog();
	if (catalog.GetCatalogType() != "sqlserver") {
		throw BinderException("Attached database \"%s\" does not refer to a SQL Server database", db_name);
	}

	auto bind_data = make_uniq<ODBCMetadataBindData>();
	auto &odbc_catalog = catalog.Cast<ODBCCatalog>();
	if (!schema_filter.empty()) {
		auto &schema = odbc_catalog.GetSchema(context, Identifier(schema_filter));
		ODBCMetadataAppendSchema(context, *bind_data, schema);
	} else {
		odbc_catalog.ScanSchemas(
		    context, [&](SchemaCatalogEntry &schema) { ODBCMetadataAppendSchema(context, *bind_data, schema); });
	}
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> ODBCMetadataInitGlobalState(ClientContext &context,
                                                                        TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->CastNoConst<ODBCMetadataBindData>();
	return make_uniq<ODBCMetadataGlobalState>(std::move(bind_data.rows));
}

static void ODBCMetadataScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<ODBCMetadataGlobalState>();
	idx_t count = 0;
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.offset++];
		output.SetValue(0, count, Value(row.table_schema));
		output.SetValue(1, count, Value(row.table_name));
		output.SetValue(2, count, Value(row.table_type));
		output.SetValue(3, count, Value(row.table_comment));
		output.SetValue(4, count, Value(row.column_name));
		output.SetValue(5, count, Value(row.data_type));
		output.SetValue(6, count, Value::INTEGER(static_cast<int32_t>(row.ordinal_position)));
		output.SetValue(7, count, Value(row.column_comment));
		count++;
	}
	output.SetChildCardinality(count);
}

ODBCMetadataFunction::ODBCMetadataFunction()
    : TableFunction("sqlserver_metadata", {LogicalType::VARCHAR}, ODBCMetadataScan, ODBCMetadataBind,
                    ODBCMetadataInitGlobalState) {
	GetSignature().AddArgs("schemas", LogicalType::VARCHAR);
}

} // namespace duckdb
