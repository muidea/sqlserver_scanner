#include "storage/odbc_optimizer.hpp"
#include "storage/odbc_catalog.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "odbc_scanner.hpp"

namespace duckdb {

struct ODBCOperators {
	reference_map_t<ODBCCatalog, vector<reference<LogicalGet>>> scans;
};

void GatherODBCScans(LogicalOperator &op, ODBCOperators &result) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		auto &table_scan = get.function;
		if (ODBCCatalog::IsODBCScan(table_scan.GetName().GetIdentifierName())) {
			// odbc table scan - add to the catalog
			auto &bind_data = get.bind_data->Cast<ODBCBindData>();
			auto &catalog = bind_data.table.ParentCatalog().Cast<ODBCCatalog>();
			result.scans[catalog].push_back(get);
		}
		if (ODBCCatalog::IsODBCQuery(table_scan.GetName().GetIdentifierName())) {
			auto &bind_data = get.bind_data->Cast<ODBCQueryBindData>();
			auto &catalog = bind_data.catalog.Cast<ODBCCatalog>();
			result.scans[catalog].push_back(get);
		}
	}
	// recurse into children
	for (auto &child : op.children) {
		GatherODBCScans(*child, result);
	}
}

void OptimizeODBCScan(unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_LIMIT) {
		auto &limit = op->Cast<LogicalLimit>();
		reference<LogicalOperator> child = *op->children[0];
		while (child.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
			child = *child.get().children[0];
		}
		if (child.get().type != LogicalOperatorType::LOGICAL_GET) {
			return;
		}
		auto &get = child.get().Cast<LogicalGet>();
		if (!ODBCCatalog::IsODBCScan(get.function.GetName().GetIdentifierName())) {
			return;
		}
		switch (limit.limit_val.Type()) {
		case LimitNodeType::CONSTANT_VALUE:
		case LimitNodeType::UNSET:
			break;
		default:
			// not a constant or unset limit
			return;
		}
		switch (limit.offset_val.Type()) {
		case LimitNodeType::CONSTANT_VALUE:
		case LimitNodeType::UNSET:
			break;
		default:
			// not a constant or unset offset
			return;
		}
		if (limit.offset_val.Type() != LimitNodeType::UNSET) {
			// Keep OFFSET local until the supported SQL Server version is known.
			return;
		}
		if (limit.limit_val.Type() != LimitNodeType::UNSET) {
			auto &bind_data = get.bind_data->Cast<ODBCBindData>();
			// SQL Server TOP does not require ORDER BY and is supported across the
			// target server versions.
			bind_data.limit = to_string(limit.limit_val.GetConstantValue());
		}
		// remove the limit
		op = std::move(op->children[0]);
		return;
	}
	// recurse into children
	for (auto &child : op->children) {
		OptimizeODBCScan(child);
	}
}

void ODBCOptimizer::Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	ODBCOperators operators;
	GatherODBCScans(*plan, operators);
	for (auto &entry : operators.scans) {
		ODBCResultStreaming result_streaming = ODBCResultStreaming::FORCE_MATERIALIZATION;
		if (entry.second.size() == 1) {
			// if we have exactly one scan for a given catalog we can stream it
			result_streaming = ODBCResultStreaming::ALLOW_STREAMING;
		}
		for (auto &logical_get : entry.second) {
			auto &get = logical_get.get();
			if (ODBCCatalog::IsODBCScan(get.function.GetName().GetIdentifierName())) {
				// odbc table scan - add to the catalog
				auto &bind_data = get.bind_data->Cast<ODBCBindData>();
				if (bind_data.streaming == ODBCResultStreaming::UNINITIALIZED ||
				    result_streaming == ODBCResultStreaming::FORCE_MATERIALIZATION) {
					bind_data.streaming = result_streaming;
				}
			}
			if (ODBCCatalog::IsODBCQuery(get.function.GetName().GetIdentifierName())) {
				auto &bind_data = get.bind_data->Cast<ODBCQueryBindData>();
				if (bind_data.streaming == ODBCResultStreaming::UNINITIALIZED ||
				    result_streaming == ODBCResultStreaming::FORCE_MATERIALIZATION) {
					bind_data.streaming = result_streaming;
				}
			}
		}
	}

	OptimizeODBCScan(plan);
}

} // namespace duckdb
