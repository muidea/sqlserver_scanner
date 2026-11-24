//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlserver_guarded_ddl.cpp
//
// Restricted transactional SQL Server DDL and capability introspection.
//===----------------------------------------------------------------------===//

#include "duckdb.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "guarded_odbc_source_error.hpp"
#include "storage/odbc_catalog.hpp"

#include <atomic>
#include <chrono>
#include <codecvt>
#include <condition_variable>
#include <locale>
#include <mutex>
#include <thread>

namespace duckdb {
namespace {

static bool SimpleIntLiteral(const string &s) {
	if (s.empty() || s.size() > 38)
		return false;
	idx_t i = s[0] == '-' ? 1 : 0;
	if (i == s.size())
		return false;
	for (; i < s.size(); i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
	}
	return true;
}

static string Quote(const string &s) {
	string out = "[";
	for (auto c : s) {
		if (c == '\0')
			throw BinderException("SQL Server DDL identifier contains null byte");
		out += c == ']' ? string("]]") : string(1, c);
	}
	return out + "]";
}

static string NLiteral(const string &s) {
	string out = "N'";
	for (auto c : s) {
		if (c == '\0')
			throw BinderException("SQL Server DDL literal contains null byte");
		out += c == '\'' ? string("''") : string(1, c);
	}
	return out + "'";
}

static string JSON(const string &s) {
	const char *hex = "0123456789abcdef";
	string out = "\"";
	for (unsigned char c : s) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += static_cast<char>(c);
		} else if (c < 32) {
			out += "\\u00";
			out += hex[c >> 4];
			out += hex[c & 15];
		} else {
			out += static_cast<char>(c);
		}
	}
	return out + "\"";
}

static std::vector<SQLWCHAR> ToWide(const string &utf8) {
	if (utf8.find('\0') != string::npos)
		throw BinderException("SQL Server DDL input contains null byte");
	std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> conv;
	auto u16 = conv.from_bytes(utf8);
	std::vector<SQLWCHAR> out(u16.begin(), u16.end());
	out.push_back(0);
	return out;
}

static int MssqlMajorVersion(const string &version) {
	idx_t i = 0;
	while (i < version.size() && version[i] >= '0' && version[i] <= '9')
		i++;
	if (i == 0 || i > 6)
		return -1;
	try {
		return std::stoi(version.substr(0, i));
	} catch (...) {
		return -1;
	}
}

static bool ValidQuerySQL(const string &sql) {
	if (sql.empty() || sql.size() > 65536 || sql.find('\0') != string::npos)
		return false;
	idx_t i = 0;
	while (i < sql.size() && StringUtil::CharacterIsSpace(sql[i]))
		i++;
	auto rest = StringUtil::Upper(sql.substr(i));
	bool starts_select = StringUtil::StartsWith(rest, "SELECT") &&
	                     (rest.size() == 6 || StringUtil::CharacterIsSpace(rest[6]) || rest[6] == '(');
	bool starts_with = StringUtil::StartsWith(rest, "WITH") &&
	                   (rest.size() == 4 || StringUtil::CharacterIsSpace(rest[4]) || rest[4] == '(');
	if (!starts_select && !starts_with)
		return false;
	char quote = 0;
	for (; i < sql.size(); i++) {
		char c = sql[i];
		if (quote) {
			if (quote == '[') {
				if (c == ']') {
					if (i + 1 < sql.size() && sql[i + 1] == ']')
						i++;
					else
						quote = 0;
				}
			} else if (c == quote) {
				if (i + 1 < sql.size() && sql[i + 1] == quote)
					i++;
				else
					quote = 0;
			}
		} else if (c == '\'' || c == '"' || c == '[') {
			quote = c;
		} else if (c == ';') {
			return false;
		}
	}
	return quote == 0;
}

static string DefaultLiteralSQL(const string &kind, const string &value) {
	if (kind == "null" && value.empty())
		return "NULL";
	if (kind == "integer" && SimpleIntLiteral(value))
		return value;
	if (kind == "boolean" && (value == "true" || value == "false"))
		return value == "true" ? "1" : "0";
	if (kind == "string" && value.size() <= 4096 && value.find('\0') == string::npos)
		return NLiteral(value);
	throw BinderException("DDL default literal is unsupported");
}

struct DDLIntent {
	int32_t contract_version = 2;
	bool conditional = false;
	string database, schema, operation, action, name, index, owner, new_name, column_name, query_sql, sql;
	vector<pair<string, string>> column_specs;
	vector<string> columns, constraints, keys;
	string default_kind, default_value, constraint_name, constraint_kind, check_column, check_operator, check_value,
	    reference_schema, reference_table, identity_mode;
	vector<string> constraint_keys, reference_keys;

	static bool Name(const string &s) {
		return !s.empty() && s.size() <= 128 && s.find('\0') == string::npos;
	}
	static string Text(const Value &v) {
		if (v.IsNull() || v.type().id() != LogicalTypeId::VARCHAR)
			throw BinderException("DDL structured input is invalid");
		return v.GetValue<string>();
	}
	static string ColumnType(const string &type, int32_t length, int32_t precision, int32_t scale, bool v5) {
		if (v5) {
			if (type == "numeric") {
				if (length || precision < 1 || precision > 38 || scale < 0 || scale > precision)
					throw BinderException("SQL Server DDL numeric precision/scale is unsupported");
				return "DECIMAL(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
			}
			if (type == "timestamp" || type == "time" || type == "timestamptz") {
				if (length || precision < 0 || precision > 6 || scale)
					throw BinderException("SQL Server DDL temporal precision is unsupported");
				if (type == "timestamptz")
					return "DATETIMEOFFSET(" + std::to_string(precision) + ")";
				return string(type == "time" ? "TIME(" : "DATETIME2(") + std::to_string(precision) + ")";
			}
			if (precision || scale)
				throw BinderException("SQL Server DDL unexpected type modifiers");
			if (type == "varchar" && length >= 1 && length <= 4000)
				return "NVARCHAR(" + std::to_string(length) + ")";
			if (length)
				throw BinderException("SQL Server DDL unexpected column length");
			if (type == "smallint")
				return "SMALLINT";
			if (type == "integer")
				return "INT";
			if (type == "bigint")
				return "BIGINT";
			if (type == "real")
				return "REAL";
			if (type == "double")
				return "FLOAT";
			if (type == "boolean")
				return "BIT";
			if (type == "text")
				return "NVARCHAR(MAX)";
			if (type == "date")
				return "DATE";
			if (type == "uuid")
				return "UNIQUEIDENTIFIER";
			if (type == "bytea")
				return "VARBINARY(MAX)";
			throw BinderException("SQL Server DDL column type is unsupported");
		}
		if (type == "smallint")
			return length ? throw BinderException("SQL Server DDL column length is invalid") : string("SMALLINT");
		if (type == "integer")
			return length ? throw BinderException("SQL Server DDL column length is invalid") : string("INT");
		if (type == "bigint")
			return length ? throw BinderException("SQL Server DDL column length is invalid") : string("BIGINT");
		if (type == "varchar" && length >= 1 && length <= 4000)
			return "NVARCHAR(" + std::to_string(length) + ")";
		if (type == "text" && !length)
			return "NVARCHAR(MAX)";
		throw BinderException("SQL Server DDL column type is unsupported");
	}
	static DDLIntent Bind(const vector<Value> &input, bool conditional = false) {
		if (input.size() != 5 || input[4].IsNull() || input[4].type().id() != LogicalTypeId::STRUCT)
			throw BinderException("DDL structured input is invalid");
		const auto &types = StructType::GetChildTypes(input[4].type());
		const auto &fields = StructValue::GetChildren(input[4]);
		if (types.empty() || types[0].first != "contract_version" || fields[0].IsNull() ||
		    fields[0].type().id() != LogicalTypeId::INTEGER)
			throw BinderException("DDL contract is unavailable");
		const int32_t contract = fields[0].GetValue<int32_t>();
		if (contract < 2 || contract > 6)
			throw BinderException("DDL contract is unavailable");
		DDLIntent d;
		d.contract_version = contract;
		d.conditional = conditional;
		d.database = Text(input[1]);
		d.schema = Text(input[2]);
		d.operation = Text(input[3]);
		if (!Name(d.database) || !Name(d.schema) || d.operation.size() != 36)
			throw BinderException("DDL source scope is invalid");

		vector<string> names = {"contract_version", "target_contract", "action", "name", "index_name", "owner"};
		if (contract >= 3) {
			names.push_back("new_name");
			names.push_back("column_name");
		}
		if (contract == 4) {
			for (auto *f : {"default_kind", "default_value", "constraint_name", "constraint_kind", "check_column",
			                "check_operator", "check_value", "reference_schema", "reference_table", "identity_mode",
			                "constraint_keys", "reference_keys"})
				names.push_back(f);
		}
		if (contract == 6) {
			names.push_back("query_sql");
			names.push_back("constraints");
		}
		names.push_back("columns");
		names.push_back("keys");
		if (types.size() != names.size())
			throw BinderException("DDL fields are invalid");
		for (idx_t i = 0; i < names.size(); i++)
			if (types[i].first != names[i] || fields[i].IsNull())
				throw BinderException("DDL fields are invalid");
		if (Text(fields[1]) != "EXECUTION_NAME")
			throw BinderException("DDL contract is unavailable");
		d.action = Text(fields[2]);
		if (contract == 6 && d.action != "create_table" && d.action != "add_column" && d.action != "create_table_as")
			throw BinderException("DDL action requires a different contract");
		if (contract != 6 && d.action == "create_table_as")
			throw BinderException("DDL action requires contract v6");
		d.name = Text(fields[3]);
		d.index = Text(fields[4]);
		d.owner = Text(fields[5]);
		if (!Name(d.name) || (!d.index.empty() && !Name(d.index)) || (!d.owner.empty() && !Name(d.owner)))
			throw BinderException("DDL target name is invalid");
		idx_t idx = 6;
		if (contract >= 3) {
			d.new_name = Text(fields[idx++]);
			d.column_name = Text(fields[idx++]);
			if ((!d.new_name.empty() && !Name(d.new_name)) || (!d.column_name.empty() && !Name(d.column_name)))
				throw BinderException("DDL altered name is invalid");
		}
		if (contract == 4) {
			d.default_kind = Text(fields[idx++]);
			d.default_value = Text(fields[idx++]);
			d.constraint_name = Text(fields[idx++]);
			d.constraint_kind = Text(fields[idx++]);
			d.check_column = Text(fields[idx++]);
			d.check_operator = Text(fields[idx++]);
			d.check_value = Text(fields[idx++]);
			d.reference_schema = Text(fields[idx++]);
			d.reference_table = Text(fields[idx++]);
			d.identity_mode = Text(fields[idx++]);
			if (fields[idx].type().id() != LogicalTypeId::LIST || fields[idx + 1].type().id() != LogicalTypeId::LIST)
				throw BinderException("DDL constraint keys are invalid");
			for (auto &k : ListValue::GetChildren(fields[idx++])) {
				auto s = Text(k);
				if (!Name(s))
					throw BinderException("DDL constraint key is invalid");
				d.constraint_keys.push_back(s);
			}
			for (auto &k : ListValue::GetChildren(fields[idx++])) {
				auto s = Text(k);
				if (!Name(s))
					throw BinderException("DDL reference key is invalid");
				d.reference_keys.push_back(s);
			}
		}
		auto constraint_clause = [&](const string &kind, const vector<string> &ck, const string &rs, const string &rt,
		                             const vector<string> &rk, const string &col, const string &op,
		                             const string &val) {
			if (kind == "primary_key" || kind == "unique") {
				if (ck.empty() || ck.size() > 32)
					throw BinderException("DDL constraint keys are invalid");
				vector<string> ks;
				for (auto &k : ck)
					ks.push_back(Quote(k));
				return string(kind == "primary_key" ? "PRIMARY KEY (" : "UNIQUE (") + StringUtil::Join(ks, ",") + ")";
			}
			if (kind == "foreign_key") {
				if (ck.empty() || ck.size() != rk.size() || !Name(rs) || !Name(rt))
					throw BinderException("DDL foreign key options are invalid");
				vector<string> cqs, rqs;
				for (auto &k : ck)
					cqs.push_back(Quote(k));
				for (auto &k : rk)
					rqs.push_back(Quote(k));
				return "FOREIGN KEY (" + StringUtil::Join(cqs, ",") + ") REFERENCES " + Quote(rs) + "." + Quote(rt) +
				       " (" + StringUtil::Join(rqs, ",") + ")";
			}
			if (kind == "check") {
				if (!Name(col) || !SimpleIntLiteral(val) ||
				    (op != "=" && op != "<>" && op != "<" && op != ">" && op != "<=" && op != ">="))
					throw BinderException("DDL check constraint is unsupported");
				return "CHECK (" + Quote(col) + " " + op + " " + val + ")";
			}
			throw BinderException("DDL constraint kind is unsupported");
		};
		if (contract == 6) {
			d.query_sql = Text(fields[idx++]);
			if (!d.query_sql.empty() && !ValidQuerySQL(d.query_sql))
				throw BinderException("DDL query payload is invalid");
			if (fields[idx].type().id() != LogicalTypeId::LIST)
				throw BinderException("DDL constraint list is invalid");
			const auto &cons = ListValue::GetChildren(fields[idx++]);
			if (cons.size() > 64)
				throw BinderException("DDL constraint bound exceeded");
			for (auto &c : cons) {
				if (c.IsNull() || c.type().id() != LogicalTypeId::STRUCT)
					throw BinderException("DDL constraint definition is invalid");
				const auto &ct = StructType::GetChildTypes(c.type());
				const auto &cv = StructValue::GetChildren(c);
				const vector<string> cnames = {"name",             "kind",            "columns",
				                               "reference_schema", "reference_table", "reference_columns",
				                               "check_column",     "check_operator",  "check_value"};
				if (ct.size() != cnames.size())
					throw BinderException("DDL constraint fields are invalid");
				for (idx_t j = 0; j < cnames.size(); j++)
					if (ct[j].first != cnames[j] || cv[j].IsNull())
						throw BinderException("DDL constraint fields are invalid");
				string cname = Text(cv[0]), kind = Text(cv[1]);
				if (!cname.empty() && !Name(cname))
					throw BinderException("DDL constraint name is invalid");
				auto read_keys = [&](const Value &lv) {
					if (lv.type().id() != LogicalTypeId::LIST)
						throw BinderException("DDL constraint keys are invalid");
					vector<string> out;
					for (auto &item : ListValue::GetChildren(lv)) {
						auto kn = Text(item);
						if (!Name(kn))
							throw BinderException("DDL constraint key name is invalid");
						out.push_back(kn);
					}
					if (out.size() > 32)
						throw BinderException("DDL constraint keys bound exceeded");
					return out;
				};
				auto ck = (kind == "check") ? vector<string>() : read_keys(cv[2]);
				auto rk = (kind == "foreign_key") ? read_keys(cv[5]) : vector<string>();
				string rs = Text(cv[3]), rt = Text(cv[4]);
				if (kind == "foreign_key" && rs != d.schema)
					throw BinderException("DDL foreign key schema must match source schema");
				auto clause = constraint_clause(kind, ck, rs, rt, rk, Text(cv[6]), Text(cv[7]), Text(cv[8]));
				d.constraints.push_back(cname.empty() ? clause : "CONSTRAINT " + Quote(cname) + " " + clause);
			}
		}
		if (fields[idx].type().id() != LogicalTypeId::LIST || fields[idx + 1].type().id() != LogicalTypeId::LIST)
			throw BinderException("DDL payload is invalid");
		const auto &cols = ListValue::GetChildren(fields[idx]);
		const auto &keys = ListValue::GetChildren(fields[idx + 1]);
		if (cols.size() > 256 || keys.size() > 32)
			throw BinderException("DDL payload bound exceeded");
		vector<string> seen;
		for (auto &col : cols) {
			if (col.IsNull() || col.type().id() != LogicalTypeId::STRUCT)
				throw BinderException("DDL column definition is invalid");
			const auto &ct = StructType::GetChildTypes(col.type());
			const auto &cv = StructValue::GetChildren(col);
			idx_t expected_fields = contract == 6 ? 10 : (contract == 5 ? 5 : 3);
			if (ct.size() != expected_fields || ct[0].first != "name" || ct[1].first != "type" ||
			    ct[2].first != "length" || cv[2].IsNull() || cv[2].type().id() != LogicalTypeId::INTEGER)
				throw BinderException("DDL column fields are invalid");
			string n = Text(cv[0]), type = Text(cv[1]);
			if (!Name(n) || std::find(seen.begin(), seen.end(), n) != seen.end())
				throw BinderException("DDL duplicate column is invalid");
			seen.push_back(n);
			int32_t prec = 0, scale = 0;
			if (contract >= 5) {
				if (ct[3].first != "precision" || ct[4].first != "scale" || cv[3].IsNull() || cv[4].IsNull() ||
				    cv[3].type().id() != LogicalTypeId::INTEGER || cv[4].type().id() != LogicalTypeId::INTEGER)
					throw BinderException("DDL numeric/time modifiers are invalid");
				prec = cv[3].GetValue<int32_t>();
				scale = cv[4].GetValue<int32_t>();
			}
			auto native = ColumnType(type, cv[2].GetValue<int32_t>(), prec, scale, contract >= 5);
			string extra;
			if (contract == 6) {
				if (ct[5].first != "not_null" || ct[6].first != "default_kind" || ct[7].first != "default_value" ||
				    ct[8].first != "primary_key" || ct[9].first != "unique" || cv[5].IsNull() || cv[8].IsNull() ||
				    cv[9].IsNull() || cv[5].type().id() != LogicalTypeId::BOOLEAN ||
				    cv[8].type().id() != LogicalTypeId::BOOLEAN || cv[9].type().id() != LogicalTypeId::BOOLEAN)
					throw BinderException("DDL v6 column modifiers are invalid");
				if (cv[5].GetValue<bool>())
					extra += " NOT NULL";
				string dk = Text(cv[6]), dv = Text(cv[7]);
				if (!dk.empty())
					extra += " DEFAULT (" + DefaultLiteralSQL(dk, dv) + ")";
				else if (!dv.empty())
					throw BinderException("DDL column default kind is missing");
				if (cv[8].GetValue<bool>())
					extra += " PRIMARY KEY";
				if (cv[9].GetValue<bool>())
					extra += " UNIQUE";
			}
			d.columns.push_back(Quote(n) + " " + native + extra);
			d.column_specs.emplace_back(n, native);
		}
		for (auto &k : keys) {
			auto n = Text(k);
			if (!Name(n) || std::find(d.keys.begin(), d.keys.end(), n) != d.keys.end())
				throw BinderException("DDL index key is invalid");
			d.keys.push_back(n);
		}
		const auto target = Quote(d.schema) + "." + Quote(d.name);
		if (d.action == "create_table") {
			if (d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty())
				throw BinderException("DDL create table options are invalid");
			vector<string> defs = d.columns;
			defs.insert(defs.end(), d.constraints.begin(), d.constraints.end());
			d.sql = "CREATE TABLE " + target + " (" + StringUtil::Join(defs, ",") + ")";
		} else if (d.action == "create_table_as") {
			if (!d.columns.empty() || !d.constraints.empty() || !d.keys.empty() || !d.index.empty() ||
			    !d.owner.empty() || !d.new_name.empty() || !d.column_name.empty() || d.query_sql.empty())
				throw BinderException("DDL create table as options are invalid");
			d.sql = "SELECT * INTO " + target + " FROM (" + d.query_sql + ") AS _fqe_ctas";
		} else if (d.action == "drop_table") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty())
				throw BinderException("DDL drop table options are invalid");
			d.sql = "DROP TABLE " + target;
		} else if (d.action == "create_index" || d.action == "create_unique_index") {
			if (!d.columns.empty() || d.keys.empty() || d.index.empty() || !d.owner.empty())
				throw BinderException("DDL create index options are invalid");
			vector<string> ks;
			for (auto &k : d.keys)
				ks.push_back(Quote(k));
			d.sql = string(d.action == "create_unique_index" ? "CREATE UNIQUE NONCLUSTERED INDEX "
			                                                 : "CREATE NONCLUSTERED INDEX ") +
			        Quote(d.index) + " ON " + target + " (" + StringUtil::Join(ks, ",") + ")";
		} else if (d.action == "drop_index") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || (!conditional && d.owner.empty()))
				throw BinderException("DDL drop index options are invalid");
			if (!d.owner.empty())
				d.sql = "DROP INDEX " + Quote(d.name) + " ON " + Quote(d.schema) + "." + Quote(d.owner);
		} else if (d.action == "rename_table") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
			    !d.column_name.empty() || d.new_name.empty() || d.new_name == d.name)
				throw BinderException("DDL rename table options are invalid");
			d.sql = "EXEC sys.sp_rename @objname=" + NLiteral(target) + ",@newname=" + NLiteral(d.new_name) +
			        ",@objtype=N'OBJECT'";
		} else if (d.action == "add_column") {
			if (d.columns.size() != 1 || !d.keys.empty() || !d.index.empty() || !d.owner.empty())
				throw BinderException("DDL add column options are invalid");
			d.sql = "ALTER TABLE " + target + " ADD " + d.columns[0];
		} else if (d.action == "drop_column") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
			    !d.new_name.empty() || d.column_name.empty())
				throw BinderException("DDL drop column options are invalid");
			d.sql = "ALTER TABLE " + target + " DROP COLUMN " + Quote(d.column_name);
		} else if (d.action == "rename_column") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
			    d.column_name.empty() || d.new_name.empty() || d.new_name == d.column_name)
				throw BinderException("DDL rename column options are invalid");
			d.sql = "EXEC sys.sp_rename @objname=" + NLiteral(target + "." + Quote(d.column_name)) +
			        ",@newname=" + NLiteral(d.new_name) + ",@objtype=N'COLUMN'";
		} else if (d.action == "alter_column_type") {
			if (d.column_specs.size() != 1 || d.column_name.empty() || d.column_specs[0].first != d.column_name)
				throw BinderException("DDL alter column type options are invalid");
			// Nullable suffix is resolved from locked sys.columns in ScanDDL.
			d.sql = "ALTER TABLE " + target + " ALTER COLUMN " + Quote(d.column_name) + " " + d.column_specs[0].second;
		} else if (d.action == "rename_index") {
			if (!d.columns.empty() || !d.keys.empty() || d.owner.empty() || d.new_name.empty() || d.new_name == d.name)
				throw BinderException("DDL rename index options are invalid");
			d.sql = "EXEC sys.sp_rename @objname=" +
			        NLiteral(Quote(d.schema) + "." + Quote(d.owner) + "." + Quote(d.name)) +
			        ",@newname=" + NLiteral(d.new_name) + ",@objtype=N'INDEX'";
		} else if (d.action == "create_schema") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
			    !d.new_name.empty() || !d.column_name.empty() || d.name != d.schema)
				throw BinderException("DDL create schema options are invalid");
			d.sql = "CREATE SCHEMA " + Quote(d.name);
		} else if (d.action == "drop_schema") {
			if (!d.columns.empty() || !d.keys.empty() || !d.index.empty() || !d.owner.empty() ||
			    !d.new_name.empty() || !d.column_name.empty() || d.name != d.schema)
				throw BinderException("DDL drop schema options are invalid");
			d.sql = "DROP SCHEMA " + Quote(d.name);
		} else if (d.action == "set_default") {
			if (d.column_name.empty())
				throw BinderException("DDL default column is missing");
			d.sql = "ALTER TABLE " + target + " ADD DEFAULT (" +
			        DefaultLiteralSQL(d.default_kind, d.default_value) + ") FOR " + Quote(d.column_name);
		} else if (d.action == "drop_default" || d.action == "set_not_null" || d.action == "drop_not_null") {
			if (d.column_name.empty())
				throw BinderException("DDL column is missing");
			// Generated dynamically from sys.default_constraints / sys.columns inside tx.
		} else if (d.action == "add_constraint") {
			if (!Name(d.constraint_name))
				throw BinderException("DDL constraint name is invalid");
			string clause;
			if (d.constraint_kind == "primary_key" || d.constraint_kind == "unique") {
				if (d.constraint_keys.empty() || d.constraint_keys.size() > 32)
					throw BinderException("DDL constraint keys are invalid");
				vector<string> ks;
				for (auto &k : d.constraint_keys)
					ks.push_back(Quote(k));
				clause = string(d.constraint_kind == "primary_key" ? "PRIMARY KEY (" : "UNIQUE (") +
				         StringUtil::Join(ks, ",") + ")";
			} else if (d.constraint_kind == "foreign_key") {
				if (d.constraint_keys.empty() || d.constraint_keys.size() != d.reference_keys.size() ||
				    !Name(d.reference_schema) || !Name(d.reference_table))
					throw BinderException("DDL foreign key options are invalid");
				vector<string> ck, rk;
				for (auto &k : d.constraint_keys)
					ck.push_back(Quote(k));
				for (auto &k : d.reference_keys)
					rk.push_back(Quote(k));
				clause = "FOREIGN KEY (" + StringUtil::Join(ck, ",") + ") REFERENCES " + Quote(d.reference_schema) +
				         "." + Quote(d.reference_table) + " (" + StringUtil::Join(rk, ",") + ")";
			} else if (d.constraint_kind == "check") {
				if (!Name(d.check_column) || !SimpleIntLiteral(d.check_value) ||
				    (d.check_operator != "=" && d.check_operator != "<>" && d.check_operator != "<" &&
				     d.check_operator != ">" && d.check_operator != "<=" && d.check_operator != ">="))
					throw BinderException("DDL check constraint is unsupported");
				clause = "CHECK (" + Quote(d.check_column) + " " + d.check_operator + " " + d.check_value + ")";
			} else {
				throw BinderException("DDL constraint kind is unsupported");
			}
			d.sql = "ALTER TABLE " + target + " ADD CONSTRAINT " + Quote(d.constraint_name) + " " + clause;
		} else if (d.action == "drop_constraint") {
			if (!Name(d.constraint_name))
				throw BinderException("DDL constraint name is invalid");
			d.sql = "ALTER TABLE " + target + " DROP CONSTRAINT " + Quote(d.constraint_name);
		} else if (d.action == "truncate") {
			if (!d.identity_mode.empty() && d.identity_mode != "restart" && d.identity_mode != "continue")
				throw BinderException("DDL truncate identity mode is unsupported");
			d.sql = "TRUNCATE TABLE " + target;
		} else if (d.action == "reindex") {
			if (d.owner.empty())
				throw BinderException("SQL Server REINDEX requires owning table");
			d.sql = "ALTER INDEX " + Quote(d.name) + " ON " + Quote(d.schema) + "." + Quote(d.owner) + " REBUILD";
		} else {
			throw BinderException("SQL Server DDL action is unsupported");
		}
		if (d.sql.size() > 65536)
			throw BinderException("DDL statement length exceeded");
		return d;
	}
};

class DDLSession {
public:
	DDLSession(ClientContext &ctx_p, SQLHDBC dbc, int seconds) : ctx(ctx_p), stmt(nullptr, ODBCStatementDelete) {
		SQLHSTMT h = nullptr;
		if (SQLAllocHandle(SQL_HANDLE_STMT, dbc, &h) != SQL_SUCCESS)
			throw IOException("SQL Server DDL statement allocation failed");
		stmt = ODBCStatementPtr(h, ODBCStatementDelete);
		SQLSetStmtAttr(h, SQL_ATTR_QUERY_TIMEOUT, reinterpret_cast<SQLPOINTER>(static_cast<intptr_t>(seconds)), 0);
		watcher = std::thread([this, seconds] {
			std::unique_lock<std::mutex> lock(mu);
			auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
			while (!stopped) {
				if (ctx.IsInterrupted() || std::chrono::steady_clock::now() >= deadline) {
					cancelled.store(true);
					SQLCancelHandle(SQL_HANDLE_STMT, stmt.get());
					break;
				}
				cv.wait_for(lock, std::chrono::milliseconds(25), [this] { return stopped; });
			}
		});
	}
	~DDLSession() {
		{
			std::lock_guard<std::mutex> lock(mu);
			stopped = true;
		}
		cv.notify_all();
		if (watcher.joinable())
			watcher.join();
	}
	string sqlstate;
	SQLINTEGER native_code = 0;
	GuardedODBCSourceError source;
	std::atomic<bool> cancelled {false};

	bool CheckRC(SQLRETURN rc) {
		if (SQL_SUCCEEDED(rc))
			return true;
		source = ReadGuardedODBCSourceError(SQL_HANDLE_STMT, stmt.get(), "sqlserver");
		SQLCHAR st[6] = {0};
		SQLSMALLINT len = 0;
		SQLGetDiagRec(SQL_HANDLE_STMT, stmt.get(), 1, st, &native_code, nullptr, 0, &len);
		if (cancelled.load() || ctx.IsInterrupted()) {
			sqlstate = "57014";
		} else {
			sqlstate = MapMssqlState(native_code, st[0] ? string(reinterpret_cast<char *>(st)) : "HY000");
		}
		return false;
	}
	static string MapMssqlState(SQLINTEGER code, const string &odbc_state) {
		switch (code) {
		case 2714:
		case 1913:
			return "42P07";
		case 208:
		case 3701:
		case 3728:
			return "42P01";
		case 207:
		case 4924:
			return "42703";
		case 2705:
			return "42701";
		case 229:
		case 262:
			return "42501";
		case 2760:
		case 3729:
			return "3F000";
		case 2601:
		case 2627:
		case 1505:
			return "23505";
		case 547:
			return "23503";
		case 515:
			return "23502";
		case 1205:
			return "40P01";
		case 1222:
			return "55P03";
		default:
			return odbc_state.size() == 5 ? odbc_state : "HY000";
		}
	}
	vector<vector<string>> Query(const string &sql, const vector<string> &params = {}) {
		SQLFreeStmt(stmt.get(), SQL_CLOSE);
		SQLFreeStmt(stmt.get(), SQL_RESET_PARAMS);
		auto wsql = ToWide(sql);
		if (!CheckRC(SQLPrepareW(stmt.get(), wsql.data(), static_cast<SQLINTEGER>(wsql.size() - 1))))
			throw IOException("SQL Server DDL query prepare failed");
		vector<std::vector<SQLWCHAR>> wparams(params.size());
		vector<SQLLEN> lens(params.size(), 0);
		for (idx_t i = 0; i < params.size(); i++) {
			wparams[i] = ToWide(params[i]);
			lens[i] = (wparams[i].size() - 1) * sizeof(SQLWCHAR);
			if (!CheckRC(SQLBindParameter(stmt.get(), i + 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
			                              std::max<size_t>(1, wparams[i].size() - 1), 0, wparams[i].data(),
			                              wparams[i].size() * sizeof(SQLWCHAR), &lens[i])))
				throw IOException("SQL Server DDL query bind failed");
		}
		if (!CheckRC(SQLExecute(stmt.get())))
			throw IOException("SQL Server DDL query failed");
		SQLSMALLINT ncols = 0;
		if (!CheckRC(SQLNumResultCols(stmt.get(), &ncols)))
			throw IOException("SQL Server DDL column count failed");
		vector<vector<string>> rows;
		while (true) {
			auto rc = SQLFetch(stmt.get());
			if (rc == SQL_NO_DATA)
				break;
			if (!CheckRC(rc))
				throw IOException("SQL Server DDL fetch failed");
			vector<string> row;
			row.reserve(ncols);
			for (SQLSMALLINT c = 1; c <= ncols; c++) {
				SQLCHAR buf[4096] = {};
				SQLLEN ind = 0;
				auto grc = SQLGetData(stmt.get(), c, SQL_C_CHAR, buf, sizeof(buf), &ind);
				if (!CheckRC(grc))
					throw IOException("SQL Server DDL cell read failed");
				row.emplace_back(ind == SQL_NULL_DATA ? string() : string(reinterpret_cast<char *>(buf)));
			}
			rows.push_back(std::move(row));
		}
		return rows;
	}
	string Scalar(const string &sql, const vector<string> &params = {}) {
		auto r = Query(sql, params);
		if (r.size() != 1 || r[0].size() != 1)
			throw IOException("SQL Server DDL scalar unavailable");
		return r[0][0];
	}
	void Exec(const string &sql) {
		SQLFreeStmt(stmt.get(), SQL_CLOSE);
		SQLFreeStmt(stmt.get(), SQL_RESET_PARAMS);
		auto wsql = ToWide(sql);
		if (!CheckRC(SQLExecDirectW(stmt.get(), wsql.data(), static_cast<SQLINTEGER>(wsql.size() - 1))))
			throw IOException("SQL Server DDL execution failed");
	}
	bool TryExec(const string &sql) {
		SQLFreeStmt(stmt.get(), SQL_CLOSE);
		SQLFreeStmt(stmt.get(), SQL_RESET_PARAMS);
		auto wsql = ToWide(sql);
		return SQL_SUCCEEDED(SQLExecDirectW(stmt.get(), wsql.data(), static_cast<SQLINTEGER>(wsql.size() - 1)));
	}

private:
	ClientContext &ctx;
	ODBCStatementPtr stmt;
	std::mutex mu;
	std::condition_variable cv;
	bool stopped = false;
	std::thread watcher;
};

static bool VerifyMssqlIdentity(DDLSession &s, string &version, string &db_name) {
	if (s.Scalar("SELECT CASE WHEN @@VERSION LIKE 'Microsoft SQL Server%' THEN '1' ELSE '0' END") != "1")
		return false;
	version = s.Scalar("SELECT CONVERT(varchar(128),SERVERPROPERTY('ProductVersion'))");
	db_name = s.Scalar("SELECT DB_NAME()");
	return !version.empty() && !db_name.empty();
}

static string FormatMssqlColumnType(const string &type_name, int max_len, int prec, int scale) {
	auto t = StringUtil::Lower(type_name);
	if (t == "varchar" || t == "char" || t == "varbinary" || t == "binary") {
		return StringUtil::Upper(t) + "(" + (max_len == -1 ? "MAX" : std::to_string(max_len)) + ")";
	}
	if (t == "nvarchar" || t == "nchar") {
		return StringUtil::Upper(t) + "(" + (max_len == -1 ? "MAX" : std::to_string(max_len / 2)) + ")";
	}
	if (t == "decimal" || t == "numeric") {
		return StringUtil::Upper(t) + "(" + std::to_string(prec) + "," + std::to_string(scale) + ")";
	}
	if (t == "datetime2" || t == "datetimeoffset" || t == "time") {
		return StringUtil::Upper(t) + "(" + std::to_string(scale) + ")";
	}
	return StringUtil::Upper(t);
}

struct CapData : TableFunctionData {
	ODBCCatalog &catalog;
	string schema;
	explicit CapData(ODBCCatalog &c) : catalog(c) {
	}
};

struct DDLData : TableFunctionData {
	ODBCCatalog &catalog;
	DDLIntent intent;
	bool diagnostics = false;
	DDLData(ODBCCatalog &c, DDLIntent i) : catalog(c), intent(std::move(i)) {
	}
};

struct OneRowState : GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<GlobalTableFunctionState> InitOne(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<OneRowState>();
}

static unique_ptr<FunctionData> BindCapabilities(ClientContext &ctx, TableFunctionBindInput &in,
                                                 vector<LogicalType> &types, vector<Identifier> &names) {
	if (in.inputs.size() != 2 || in.inputs[0].IsNull() || in.inputs[1].IsNull())
		throw BinderException("SQL Server DDL capability input is incomplete");
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, Identifier(in.inputs[0].GetValue<string>()));
	if (!db || db->GetCatalog().GetCatalogType() != "sqlserver")
		throw BinderException("SQL Server DDL catalog is unavailable");
	auto d = make_uniq<CapData>(db->GetCatalog().Cast<ODBCCatalog>());
	d->schema = in.inputs[1].GetValue<string>();
	if (!DDLIntent::Name(d->schema))
		throw BinderException("SQL Server DDL schema name is invalid");
	names = {"sqlstate", "stage", "document"};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(d);
}

static void ScanCapabilities(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
	auto &state = in.global_state->Cast<OneRowState>();
	if (state.done)
		return;
	state.done = true;
	auto &d = in.bind_data->Cast<CapData>();
	string sqlstate, stage = "source_identity", document;
	ODBCPoolConnection pooled;
	shared_ptr<ODBCConnection> connection;
	unique_ptr<DDLSession> session;
	try {
		if (ctx.IsInterrupted())
			throw InterruptException();
		if (!d.catalog.GetConnectionPool().TryGetConnection(pooled))
			throw IOException("SQL Server connection unavailable");
		connection = pooled.GetConnection();
		auto dbc = connection->GetConn();
		session = make_uniq<DDLSession>(ctx, dbc, 10);
		string version, db_name;
		if (!VerifyMssqlIdentity(*session, version, db_name)) {
			sqlstate = "0A000";
			throw IOException("SQL Server DDL identity not certified");
		}
		stage = "source_metadata";
		const int major = MssqlMajorVersion(version);
		const bool executor_available = major >= 9;
		bool read_only = false;
		bool ns_present = false;
		if (executor_available) {
			read_only =
			    session->Scalar("SELECT CASE WHEN DATABASEPROPERTYEX(DB_NAME(),'Updateability')='READ_ONLY' OR "
			                    "is_read_only=1 THEN '1' ELSE '0' END FROM sys.databases WHERE name=DB_NAME()") == "1";
			ns_present =
			    session->Scalar("SELECT CASE WHEN SCHEMA_ID(?) IS NOT NULL THEN '1' ELSE '0' END", {d.schema}) == "1";
		} else {
			read_only =
			    session->Scalar("SELECT CASE WHEN DATABASEPROPERTYEX(DB_NAME(),'Updateability')='READ_ONLY' THEN '1' "
			                    "ELSE '0' END") == "1";
			ns_present =
			    session->Scalar("SELECT CASE WHEN EXISTS (SELECT 1 FROM dbo.sysusers WHERE name=?) THEN '1' ELSE '0' "
			                    "END",
			                    {d.schema}) == "1";
		}
		document = "{\"contract_version\":1,\"product\":\"sqlserver\",\"source_version\":" + JSON(version) +
		           ",\"database\":" + JSON(db_name) + ",\"schema\":" + JSON(d.schema) +
		           ",\"namespace_present\":" + (ns_present ? "true" : "false") +
		           ",\"source_read_only\":" + (read_only ? "true" : "false") +
		           ",\"visibility\":\"CATALOG_READ\",\"executor_available\":" +
		           (executor_available ? "true" : "false") + "}";
		stage.clear();
	} catch (...) {
		if (sqlstate.empty()) {
			sqlstate = session && !session->sqlstate.empty()
			               ? session->sqlstate
			               : (ctx.IsInterrupted() ? "57014" : "08006");
		}
		document.clear();
		if (connection)
			connection->Close();
	}
	out.SetValue(0, 0, Value(sqlstate));
	out.SetValue(1, 0, Value(stage));
	out.SetValue(2, 0, Value(document));
	out.SetChildCardinality(1);
}

static unique_ptr<FunctionData> BindDDL(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                        vector<Identifier> &names) {
	auto cond_it = in.named_parameters.find("conditional");
	const bool conditional = cond_it != in.named_parameters.end() && !cond_it->second.IsNull() &&
	                         cond_it->second.GetValue<bool>();
	auto intent = DDLIntent::Bind(in.inputs, conditional);
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, Identifier(DDLIntent::Text(in.inputs[0])));
	if (!db || db->GetCatalog().GetCatalogType() != "sqlserver")
		throw BinderException("SQL Server DDL catalog is unavailable");
	auto d = make_uniq<DDLData>(db->GetCatalog().Cast<ODBCCatalog>(), std::move(intent));
	auto diag = in.named_parameters.find("source_diagnostics");
	d->diagnostics = diag != in.named_parameters.end() && !diag->second.IsNull() && diag->second.GetValue<bool>();
	names = {"operation_id", "status", "sqlstate", "stage", "warning_count"};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	         LogicalType::BIGINT};
	if (d->diagnostics) {
		names.push_back("source_error");
		types.push_back(LogicalType::VARCHAR);
	}
	return std::move(d);
}

static void ScanDDL(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
	auto &state = in.global_state->Cast<OneRowState>();
	if (state.done)
		return;
	state.done = true;
	auto &d = in.bind_data->Cast<DDLData>();
	string status = "CONFIRMED_NOT_EXECUTED", error, stage = "source_identity";
	GuardedODBCSourceError source;
	ODBCPoolConnection pooled;
	shared_ptr<ODBCConnection> connection;
	unique_ptr<DDLSession> session;
	bool in_tx = false, sent = false, commit_sent = false;
	try {
		if (ctx.IsInterrupted()) {
			error = "57014";
			throw InterruptException();
		}
		if (!d.catalog.GetConnectionPool().TryGetConnection(pooled)) {
			error = "08006";
			throw IOException("SQL Server connection unavailable");
		}
		connection = pooled.GetConnection();
		auto dbc = connection->GetConn();
		session = make_uniq<DDLSession>(ctx, dbc, d.intent.action == "reindex" ? 290 : 60);
		if (session->Scalar("SELECT CONVERT(varchar(20),@@TRANCOUNT)") != "0") {
			error = "HY000";
			throw IOException("SQL Server DDL connection is not idle");
		}
		string version, db_name;
		if (!VerifyMssqlIdentity(*session, version, db_name) || db_name != d.intent.database) {
			error = "0A000";
			throw IOException("SQL Server DDL identity not certified");
		}
		if (MssqlMajorVersion(version) < 9) {
			error = "0A000";
			throw IOException("SQL Server < 2005 guarded DDL is unsupported");
		}
		if (session->Scalar("SELECT CASE WHEN DATABASEPROPERTYEX(DB_NAME(),'Updateability')='READ_ONLY' OR "
		                    "is_read_only=1 THEN '1' ELSE '0' END FROM sys.databases WHERE name=DB_NAME()") == "1") {
			error = "25006";
			throw IOException("SQL Server DDL source is read-only");
		}
		session->Exec("SET XACT_ABORT ON;SET NOCOUNT ON;SET QUOTED_IDENTIFIER ON;SET ANSI_NULLS ON;SET LOCK_TIMEOUT "
		              "5000;BEGIN TRANSACTION");
		in_tx = true;

		const auto target = Quote(d.intent.schema) + "." + Quote(d.intent.name);
		bool idempotent_skip = false;
		if (d.intent.conditional) {
			auto count_query = [&](const string &sql, const vector<string> &params) -> bool {
				auto r = session->Query(sql, params);
				return r.size() == 1 && !r[0].empty() && r[0][0] != "0";
			};
			if (d.intent.action == "create_table" || d.intent.action == "create_table_as") {
				idempotent_skip = count_query(
				    "SELECT COUNT(*) FROM sys.tables t JOIN sys.schemas s ON s.schema_id=t.schema_id WHERE "
				    "s.name=? AND t.name=?",
				    {d.intent.schema, d.intent.name});
			} else if (d.intent.action == "drop_table") {
				idempotent_skip = !count_query(
				    "SELECT COUNT(*) FROM sys.tables t JOIN sys.schemas s ON s.schema_id=t.schema_id WHERE "
				    "s.name=? AND t.name=?",
				    {d.intent.schema, d.intent.name});
			} else if (d.intent.action == "create_schema") {
				idempotent_skip = count_query("SELECT COUNT(*) FROM sys.schemas WHERE name=?", {d.intent.name});
			} else if (d.intent.action == "drop_schema") {
				idempotent_skip = !count_query("SELECT COUNT(*) FROM sys.schemas WHERE name=?", {d.intent.name});
			} else if (d.intent.action == "add_column" || d.intent.action == "drop_column") {
				if (count_query(
				        "SELECT COUNT(*) FROM sys.tables t JOIN sys.schemas s ON s.schema_id=t.schema_id WHERE "
				        "s.name=? AND t.name=?",
				        {d.intent.schema, d.intent.name})) {
					const auto &cname = d.intent.action == "add_column" ? d.intent.column_specs[0].first
					                                                    : d.intent.column_name;
					bool col_present = count_query(
					    "SELECT COUNT(*) FROM sys.columns WHERE object_id=OBJECT_ID(?,N'U') AND name=?",
					    {target, cname});
					idempotent_skip = d.intent.action == "add_column" ? col_present : !col_present;
				}
			} else if (d.intent.action == "create_index" || d.intent.action == "create_unique_index") {
				if (count_query(
				        "SELECT COUNT(*) FROM sys.tables t JOIN sys.schemas s ON s.schema_id=t.schema_id WHERE "
				        "s.name=? AND t.name=?",
				        {d.intent.schema, d.intent.name})) {
					idempotent_skip = count_query(
					    "SELECT COUNT(*) FROM sys.indexes WHERE object_id=OBJECT_ID(?,N'U') AND name=?",
					    {target, d.intent.index});
				}
			} else if (d.intent.action == "drop_index") {
				if (d.intent.owner.empty()) {
					if (!count_query(
					        "SELECT COUNT(*) FROM sys.indexes i JOIN sys.tables t ON t.object_id=i.object_id JOIN "
					        "sys.schemas s ON s.schema_id=t.schema_id WHERE s.name=? AND i.name=?",
					        {d.intent.schema, d.intent.name})) {
						idempotent_skip = true;
					} else {
						error = "0A000";
						throw IOException("SQL Server DROP INDEX requires owning table");
					}
				} else {
					const auto owner_target = Quote(d.intent.schema) + "." + Quote(d.intent.owner);
					idempotent_skip = !count_query(
					    "SELECT COUNT(*) FROM sys.indexes WHERE object_id=OBJECT_ID(?,N'U') AND name=?",
					    {owner_target, d.intent.name});
				}
			} else if (d.intent.action == "drop_constraint") {
				if (count_query(
				        "SELECT COUNT(*) FROM sys.tables t JOIN sys.schemas s ON s.schema_id=t.schema_id WHERE "
				        "s.name=? AND t.name=?",
				        {d.intent.schema, d.intent.name})) {
					idempotent_skip = !count_query(
					    "SELECT COUNT(*) FROM sys.objects WHERE parent_object_id=OBJECT_ID(?,N'U') AND name=? AND "
					    "type IN('PK','UQ','F','C','D')",
					    {target, d.intent.constraint_name});
				}
			}
		}

		string native_sql = d.intent.sql;
		if (!idempotent_skip && d.intent.action == "alter_column_type") {
			auto rows = session->Query("SELECT CONVERT(varchar(1),c.is_nullable) FROM sys.columns c WHERE "
			                           "c.object_id=OBJECT_ID(?,N'U') AND c.name=?",
			                           {target, d.intent.column_name});
			if (rows.size() == 1)
				native_sql += (rows[0][0] == "0" ? " NOT NULL" : " NULL");
		} else if (!idempotent_skip && (d.intent.action == "set_not_null" || d.intent.action == "drop_not_null")) {
			auto rows = session->Query("SELECT TYPE_NAME(c.user_type_id),c.max_length,c.[precision],c.[scale] "
			                           "FROM sys.columns c WHERE c.object_id=OBJECT_ID(?,N'U') AND c.name=?",
			                           {target, d.intent.column_name});
			if (rows.size() != 1) {
				error = "42703";
				throw IOException("SQL Server DDL column unavailable");
			}
			auto col_type =
			    FormatMssqlColumnType(rows[0][0], std::stoi(rows[0][1]), std::stoi(rows[0][2]), std::stoi(rows[0][3]));
			native_sql = "ALTER TABLE " + target + " ALTER COLUMN " + Quote(d.intent.column_name) + " " + col_type +
			             (d.intent.action == "set_not_null" ? " NOT NULL" : " NULL");
		} else if (!idempotent_skip && d.intent.action == "drop_default") {
			auto rows = session->Query("SELECT dc.name FROM sys.default_constraints dc WHERE "
			                           "dc.parent_object_id=OBJECT_ID(?,N'U') AND "
			                           "dc.parent_column_id=COLUMNPROPERTY(OBJECT_ID(?,N'U'),?,N'ColumnId')",
			                           {target, target, d.intent.column_name});
			if (rows.size() != 1) {
				error = "42704";
				throw IOException("SQL Server column default constraint not found");
			}
			native_sql = "ALTER TABLE " + target + " DROP CONSTRAINT " + Quote(rows[0][0]);
		}

		if (!idempotent_skip) {
			stage = "source_execute";
			if (ctx.IsInterrupted()) {
				error = "57014";
				throw InterruptException();
			}
			sent = true;
			session->Exec(native_sql);
		}

		stage = "source_commit";
		commit_sent = true;
		status = "UNKNOWN";
		session->Exec("COMMIT TRANSACTION");
		in_tx = false;
		status = "COMMITTED";
		stage.clear();
	} catch (...) {
		if (session) {
			source = session->source;
			if (error.empty())
				error = session->sqlstate;
		}
		if (error.empty())
			error = ctx.IsInterrupted() ? "57014" : "08006";
		if (sent && status != "COMMITTED") {
			status = "UNKNOWN";
			if (in_tx && !commit_sent && session && session->TryExec("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION")) {
				status = "CONFIRMED_ROLLED_BACK";
				in_tx = false;
			}
		} else if (in_tx && session) {
			session->TryExec("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION");
			in_tx = false;
		}
	}
	if (connection)
		connection->Close();
	out.SetValue(0, 0, Value(d.intent.operation));
	out.SetValue(1, 0, Value(status));
	out.SetValue(2, 0, Value(error));
	out.SetValue(3, 0, Value(stage));
	out.SetValue(4, 0, Value::BIGINT(0));
	if (d.diagnostics)
		out.SetValue(5, 0, Value(source.JSON()));
	out.SetChildCardinality(1);
}

} // namespace

void RegisterSQLServerGuardedDDL(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    TableFunction("sqlserver_ddl_capabilities", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                  ScanCapabilities, BindCapabilities, InitOne));
	TableFunction ddl("sqlserver_guarded_ddl",
	                  {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                   LogicalType::ANY},
	                  ScanDDL, BindDDL, InitOne);
	ddl.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("source_diagnostics", LogicalType::BOOLEAN);
		options.Add("conditional", LogicalType::BOOLEAN);
	});
	loader.RegisterFunction(ddl);
}

} // namespace duckdb
