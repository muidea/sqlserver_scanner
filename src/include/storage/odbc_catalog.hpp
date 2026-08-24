//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/odbc_catalog.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "storage/odbc_schema_set.hpp"
#include "storage/odbc_connection_pool.hpp"

namespace duckdb {
class ODBCSchemaEntry;

class ODBCCatalog : public Catalog {
public:
	explicit ODBCCatalog(AttachedDatabase &db_p, string connection_string, string attach_path, string default_schema,
	                     AccessMode access_mode);
	~ODBCCatalog();

	string connection_string;
	string attach_path;
	AccessMode access_mode;

public:
	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return "sqlserver";
	}

	static string GetConnectionString(ClientContext &context, const string &attach_path, string secret_name);

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;

	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;

	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;

	//! Whether or not this is an in-memory ODBC database
	bool InMemory() override;
	string GetDBPath() override;

	void ClearCache();

	static void MaterializeODBCScans(PhysicalOperator &op);
	static bool IsODBCScan(const string &name);
	static bool IsODBCQuery(const string &name);

public:
	ODBCConnectionPool &GetConnectionPool() {
		return connection_pool;
	}
	string GetDefaultSchema() const override {
		return default_schema;
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	ODBCSchemaSet schemas;
	string default_schema;
	ODBCConnectionPool connection_pool;
};

} // namespace duckdb
