//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlserver_guarded_ddl.cpp
//
// Guarded SQL Server capability observation, catalog description, and
// transactional DDL execution (v2-v6 ABI, CTAS, conditional source_noop,
// dependency/widening safety guards, and pre-2005 capability gate).
//===----------------------------------------------------------------------===//

#include "duckdb.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "guarded_odbc_source_error.hpp"
#include "storage/odbc_catalog.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <codecvt>
#include <condition_variable>
#include <locale>
#include <mutex>
#include <thread>

namespace duckdb {
namespace {

class DDLRejectException : public std::runtime_error {
public:
	string sqlstate;
	string stage;
	DDLRejectException(string state_p, string stage_p, const string &msg)
	    : std::runtime_error(msg), sqlstate(std::move(state_p)), stage(std::move(stage_p)) {
	}
};

[[noreturn]] static void RejectDDL(const string &sqlstate = "0A000", const string &stage = "source_identity",
                                   const string &msg = "guarded SQL Server DDL rejected") {
	throw DDLRejectException(sqlstate, stage, msg);
}

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

static string SafeSQLServerCollation(const string &s) {
	if (s.empty() || s.size() > 128) {
		RejectDDL("0A000", "source_definition", "invalid SQL Server collation name");
	}
	for (char c : s) {
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) {
			RejectDDL("0A000", "source_definition", "invalid SQL Server collation name");
		}
	}
	return s;
}

static string JSON(const string &s) {
	const char *hex = "0123456789abcdef";
	string out = "\"";
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 32) {
				out += "\\u00";
				out += hex[c >> 4];
				out += hex[c & 15];
			} else {
				out += static_cast<char>(c);
			}
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

static string ValidSQLState(const string &candidate, const string &fallback) {
	if (candidate.size() == 5) {
		bool ok = true;
		for (char c : candidate) {
			if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
				ok = false;
				break;
			}
		}
		if (ok) {
			return candidate;
		}
	}
	return fallback;
}

static string MapMssqlState(SQLINTEGER native, const string &odbc_state) {
	switch (native) {
	case 1222:
		return "55P03";
	case 1205:
		return "40P01";
	case 515:
	case 4901:
		return "23502";
	case 547:
		return "23514";
	case 2601:
	case 2627:
	case 1505:
		return "23505";
	case 3726:
	case 4712:
	case 5074:
	case 4902:
	case 4922:
	case 5072:
	case 3723:
		return "2BP01";
	case 4923:
		return "42P16";
	case 2714:
	case 1913:
	case 1779:
	case 1781:
	case 2705:
		return "42P07";
	case 3701:
	case 3728:
	case 15248:
	case 15335:
		return "42704";
	case 208:
	case 1088:
		return "42P01";
	case 207:
	case 4924:
		return "42703";
	case 229:
	case 262:
	case 15247:
		return "42501";
	case 2760:
	case 3729:
		return "3F000";
	default:
		return ValidSQLState(odbc_state, "HY000");
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

struct DDLColumnDef {
	string name;
	string type;
	int32_t length = 0;
	int32_t precision = 0;
	int32_t scale = 0;
	bool not_null = false;
	string default_kind;
	string default_value;
	bool primary_key = false;
	bool unique = false;
	string native_type;
};

struct DDLIntent {
	int32_t contract_version = 2;
	bool conditional = false;
	string database, schema, operation, action, name, index, owner, new_name, column_name, query_sql, sql;
	vector<DDLColumnDef> column_defs;
	vector<string> columns, constraints, keys;
	vector<string> table_pk_columns;
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
				return "FLOAT(53)";
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
				if (kind == "primary_key") {
					d.table_pk_columns.insert(d.table_pk_columns.end(), ck.begin(), ck.end());
				}
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
			DDLColumnDef def;
			def.name = Text(cv[0]);
			def.type = Text(cv[1]);
			def.length = cv[2].GetValue<int32_t>();
			if (!Name(def.name) || std::find(seen.begin(), seen.end(), def.name) != seen.end())
				throw BinderException("DDL duplicate column is invalid");
			seen.push_back(def.name);
			if (contract >= 5) {
				if (ct[3].first != "precision" || ct[4].first != "scale" || cv[3].IsNull() || cv[4].IsNull() ||
				    cv[3].type().id() != LogicalTypeId::INTEGER || cv[4].type().id() != LogicalTypeId::INTEGER)
					throw BinderException("DDL numeric/time modifiers are invalid");
				def.precision = cv[3].GetValue<int32_t>();
				def.scale = cv[4].GetValue<int32_t>();
			}
			def.native_type = ColumnType(def.type, def.length, def.precision, def.scale, contract >= 5);
			string extra;
			if (contract == 6) {
				if (ct[5].first != "not_null" || ct[6].first != "default_kind" || ct[7].first != "default_value" ||
				    ct[8].first != "primary_key" || ct[9].first != "unique" || cv[5].IsNull() || cv[8].IsNull() ||
				    cv[9].IsNull() || cv[5].type().id() != LogicalTypeId::BOOLEAN ||
				    cv[8].type().id() != LogicalTypeId::BOOLEAN || cv[9].type().id() != LogicalTypeId::BOOLEAN)
					throw BinderException("DDL v6 column modifiers are invalid");
				def.not_null = cv[5].GetValue<bool>();
				def.default_kind = Text(cv[6]);
				def.default_value = Text(cv[7]);
				def.primary_key = cv[8].GetValue<bool>();
				def.unique = cv[9].GetValue<bool>();
				if (def.default_kind.empty() && !def.default_value.empty())
					throw BinderException("DDL column default kind is missing");
				bool in_table_pk = std::find(d.table_pk_columns.begin(), d.table_pk_columns.end(), def.name) !=
				                   d.table_pk_columns.end();
				bool eff_nn = def.not_null || def.primary_key || in_table_pk;
				if (eff_nn && def.default_kind == "null")
					throw BinderException("DDL NOT NULL column cannot have NULL default");
				extra += eff_nn ? " NOT NULL" : " NULL";
				if (!def.default_kind.empty())
					extra += " DEFAULT (" + DefaultLiteralSQL(def.default_kind, def.default_value) + ")";
				if (def.primary_key)
					extra += " PRIMARY KEY";
				if (def.unique)
					extra += " UNIQUE";
			} else {
				bool in_table_pk = std::find(d.table_pk_columns.begin(), d.table_pk_columns.end(), def.name) !=
				                   d.table_pk_columns.end();
				extra += in_table_pk ? " NOT NULL" : " NULL";
			}
			d.columns.push_back(Quote(def.name) + " " + def.native_type + extra);
			d.column_defs.push_back(std::move(def));
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
			if (d.column_defs.size() != 1 || d.column_name.empty() || d.column_defs[0].name != d.column_name)
				throw BinderException("DDL alter column type options are invalid");
			// Lossless widening, collation, default preservation, and nullability resolved in ScanDDL.
			d.sql = "ALTER TABLE " + target + " ALTER COLUMN " + Quote(d.column_name) + " " +
			        d.column_defs[0].native_type;
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
			RejectDDL("08006", "source_identity", "SQL Server DDL statement allocation failed");
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
	string current_stage = "source_identity";
	SQLINTEGER native_code = 0;
	int64_t last_warning_count = 0;
	GuardedODBCSourceError source;
	std::atomic<bool> cancelled {false};

	void Check(SQLRETURN rc, bool ignore_sp_rename_info = false) {
		if (rc == SQL_SUCCESS) {
			return;
		}
		if (rc == SQL_SUCCESS_WITH_INFO && ignore_sp_rename_info) {
			bool only_benign_15477 = true;
			SQLINTEGER non_benign_count = 0;
			for (SQLSMALLINT rec = 1; rec <= 32; rec++) {
				SQLCHAR state[6] = {0};
				SQLINTEGER native = 0;
				SQLSMALLINT len = 0;
				auto diag_rc = SQLGetDiagRec(SQL_HANDLE_STMT, stmt.get(), rec, state, &native, nullptr, 0, &len);
				if (diag_rc == SQL_NO_DATA) {
					break;
				}
				if (!SQL_SUCCEEDED(diag_rc)) {
					only_benign_15477 = false;
					break;
				}
				if (native != 15477) {
					only_benign_15477 = false;
					non_benign_count++;
				}
			}
			if (only_benign_15477) {
				return;
			}
			last_warning_count += non_benign_count;
			return;
		}
		if (rc == SQL_SUCCESS_WITH_INFO) {
			for (SQLSMALLINT rec = 1; rec <= 32; rec++) {
				SQLCHAR state[6] = {0};
				SQLINTEGER native = 0;
				SQLSMALLINT len = 0;
				if (SQLGetDiagRec(SQL_HANDLE_STMT, stmt.get(), rec, state, &native, nullptr, 0, &len) == SQL_NO_DATA) {
					break;
				}
				last_warning_count++;
			}
			return;
		}
		source = ReadGuardedODBCSourceError(SQL_HANDLE_STMT, stmt.get(), "sqlserver");
		SQLCHAR st[6] = {0};
		SQLSMALLINT len = 0;
		SQLGetDiagRec(SQL_HANDLE_STMT, stmt.get(), 1, st, &native_code, nullptr, 0, &len);
		if (cancelled.load() || ctx.IsInterrupted()) {
			sqlstate = "57014";
		} else {
			sqlstate = MapMssqlState(native_code, st[0] ? string(reinterpret_cast<char *>(st)) : "HY000");
		}
		RejectDDL(sqlstate, current_stage, source.message.empty() ? "SQL Server DDL error" : source.message);
	}

	void Start(bool cleanup = false) {
		if (!cleanup && (cancelled.load() || ctx.IsInterrupted())) {
			RejectDDL("57014", current_stage, "operation cancelled");
		}
		auto rc1 = SQLFreeStmt(stmt.get(), SQL_CLOSE);
		if (!cleanup)
			Check(rc1);
		auto rc2 = SQLFreeStmt(stmt.get(), SQL_UNBIND);
		if (!cleanup)
			Check(rc2);
		auto rc3 = SQLFreeStmt(stmt.get(), SQL_RESET_PARAMS);
		if (!cleanup)
			Check(rc3);
	}

	vector<vector<string>> Query(const string &sql, const vector<string> &params = {}) {
		Start(false);
		auto wsql = ToWide(sql);
		if (params.empty()) {
			Check(SQLExecDirectW(stmt.get(), wsql.data(), static_cast<SQLINTEGER>(wsql.size() - 1)));
		} else {
			Check(SQLPrepareW(stmt.get(), wsql.data(), static_cast<SQLINTEGER>(wsql.size() - 1)));
			vector<std::vector<SQLWCHAR>> wparams(params.size());
			vector<SQLLEN> lens(params.size(), 0);
			for (idx_t i = 0; i < params.size(); i++) {
				wparams[i] = ToWide(params[i]);
				lens[i] = (wparams[i].size() - 1) * sizeof(SQLWCHAR);
				Check(SQLBindParameter(stmt.get(), i + 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
				                       std::max<size_t>(1, wparams[i].size() - 1), 0, wparams[i].data(),
				                       wparams[i].size() * sizeof(SQLWCHAR), &lens[i]));
			}
			Check(SQLExecute(stmt.get()));
		}
		SQLSMALLINT ncols = 0;
		Check(SQLNumResultCols(stmt.get(), &ncols));
		if (ncols < 1 || ncols > 64) {
			RejectDDL("XX000", current_stage, "unexpected column count in SQL Server query");
		}
		vector<vector<string>> rows;
		idx_t total_bytes = 0;
		while (true) {
			auto rc = SQLFetch(stmt.get());
			if (rc == SQL_NO_DATA)
				break;
			Check(rc);
			if (rows.size() >= 8192) {
				RejectDDL("54000", current_stage, "SQL Server metadata row bound exceeded");
			}
			vector<string> row;
			row.reserve(ncols);
			for (SQLSMALLINT c = 1; c <= ncols; c++) {
				char buf[8192] = {};
				SQLLEN ind = 0;
				auto grc = SQLGetData(stmt.get(), c, SQL_C_CHAR, buf, sizeof(buf), &ind);
				if (grc != SQL_SUCCESS) {
					Check(grc);
				}
				if (ind == SQL_NULL_DATA) {
					row.emplace_back("");
				} else if (ind < 0 || static_cast<idx_t>(ind) >= sizeof(buf)) {
					RejectDDL("54000", current_stage, "SQL Server metadata cell bound exceeded");
				} else {
					row.emplace_back(buf, static_cast<size_t>(ind));
				}
				total_bytes += row.back().size();
				if (total_bytes > 1024 * 1024) {
					RejectDDL("54000", current_stage, "SQL Server metadata document bound exceeded");
				}
			}
			rows.push_back(std::move(row));
		}
		SQLRETURN more_rc;
		while ((more_rc = SQLMoreResults(stmt.get())) != SQL_NO_DATA) {
			Check(more_rc);
		}
		return rows;
	}

	string Scalar(const string &sql, const vector<string> &params = {}, bool cleanup = false) {
		if (cleanup) {
			Start(true);
			auto wsql = ToWide(sql);
			if (!SQL_SUCCEEDED(SQLExecDirectW(stmt.get(), wsql.data(), static_cast<SQLINTEGER>(wsql.size() - 1)))) {
				return "";
			}
			if (!SQL_SUCCEEDED(SQLFetch(stmt.get()))) {
				return "";
			}
			char buf[256] = {0};
			SQLLEN ind = 0;
			if (!SQL_SUCCEEDED(SQLGetData(stmt.get(), 1, SQL_C_CHAR, buf, sizeof(buf), &ind)) || ind == SQL_NULL_DATA ||
			    ind < 0) {
				return "";
			}
			return string(buf, static_cast<size_t>(ind));
		}
		auto r = Query(sql, params);
		if (r.size() != 1 || r[0].size() != 1)
			RejectDDL("XX000", current_stage, "SQL Server DDL scalar unavailable");
		return r[0][0];
	}

	void Exec(const string &sql, bool cleanup = false, bool ignore_sp_rename_info = false) {
		Start(cleanup);
		auto wsql = ToWide(sql);
		auto rc = SQLExecDirectW(stmt.get(), wsql.data(), static_cast<SQLINTEGER>(wsql.size() - 1));
		if (cleanup) {
			return;
		}
		if (rc == SQL_NO_DATA) {
			return;
		}
		Check(rc, ignore_sp_rename_info);
		while ((rc = SQLMoreResults(stmt.get())) != SQL_NO_DATA) {
			if (!SQL_SUCCEEDED(rc)) {
				Check(rc, ignore_sp_rename_info);
			}
			if (rc == SQL_SUCCESS_WITH_INFO) {
				Check(rc, ignore_sp_rename_info);
			}
		}
	}

private:
	ClientContext &ctx;
	ODBCStatementPtr stmt;
	std::mutex mu;
	std::condition_variable cv;
	bool stopped = false;
	std::thread watcher;
};

struct SQLServerServerFacts {
	string version;
	string machine;
	string instance;
	string db_name;
	string db_id;
	string updateability;
	string login;
	string db_user;
	string schema_id;
	bool read_only = false;
	bool ns_present = false;
	bool executor_available = false;
};

static bool VerifyMssqlIdentity(DDLSession &s, string &version, string &db_name) {
	if (s.Scalar("SELECT CASE WHEN @@VERSION LIKE 'Microsoft SQL Server%' THEN '1' ELSE '0' END") != "1")
		return false;
	version = s.Scalar("SELECT CONVERT(varchar(128),SERVERPROPERTY('ProductVersion'))");
	db_name = s.Scalar("SELECT DB_NAME()");
	return !version.empty() && !db_name.empty();
}

static SQLServerServerFacts QueryServerFacts(DDLSession &s, const string &schema) {
	string version, db_name;
	if (!VerifyMssqlIdentity(s, version, db_name)) {
		RejectDDL("0A000", "source_identity", "SQL Server DDL identity not certified");
	}
	SQLServerServerFacts facts;
	facts.version = version;
	facts.db_name = db_name;
	const int major = MssqlMajorVersion(version);
	facts.executor_available = major >= 9;

	if (facts.executor_available) {
		auto rows = s.Query(
		    "SELECT "
		    "COALESCE(CAST(SERVERPROPERTY('MachineName') AS varchar(128)),''),"
		    "COALESCE(CAST(SERVERPROPERTY('InstanceName') AS varchar(128)),'MSSQLSERVER'),"
		    "COALESCE(CONVERT(varchar(20),DB_ID()),''),"
		    "COALESCE(CAST(DATABASEPROPERTYEX(DB_NAME(),'Updateability') AS varchar(64)),''),"
		    "COALESCE(CAST(SUSER_SNAME() AS varchar(128)),''),"
		    "COALESCE(CAST(USER_NAME() AS varchar(128)),''),"
		    "COALESCE(CONVERT(varchar(20),SCHEMA_ID(?)),''),"
		    "COALESCE((SELECT CONVERT(varchar(1),is_read_only) FROM sys.databases WHERE name=DB_NAME()),'0')",
		    {schema});
		if (rows.size() != 1 || rows[0].size() != 8) {
			RejectDDL("0A000", "source_identity", "SQL Server identity query failed");
		}
		facts.machine = rows[0][0];
		facts.instance = rows[0][1];
		facts.db_id = rows[0][2];
		facts.updateability = rows[0][3];
		facts.login = rows[0][4];
		facts.db_user = rows[0][5];
		facts.schema_id = rows[0][6];
		facts.ns_present = !facts.schema_id.empty();
		facts.read_only = (StringUtil::Upper(facts.updateability) != "READ_WRITE" || rows[0][7] == "1");
	} else {
		facts.updateability =
		    s.Scalar("SELECT COALESCE(CAST(DATABASEPROPERTYEX(DB_NAME(),'Updateability') AS varchar(64)),'')");
		facts.read_only = StringUtil::Upper(facts.updateability) == "READ_ONLY";
		facts.ns_present =
		    s.Scalar("SELECT CASE WHEN EXISTS (SELECT 1 FROM dbo.sysusers WHERE name=?) THEN '1' ELSE '0' END",
		             {schema}) == "1";
	}
	return facts;
}

static string BuildSQLServerAlterColumnType(const vector<string> &row) {
	// row: {ty.name, max_length, precision, scale, collation_name, is_computed, is_identity, is_nullable,
	//       is_user_defined}
	const string ty = StringUtil::Lower(row[0]);
	int max_len = std::stoi(row[1]);
	int prec = std::stoi(row[2]);
	int scale = std::stoi(row[3]);
	const string collation = row[4];
	if (row[5] == "1" || row[6] == "1" || row[8] == "1") {
		RejectDDL("0A000", "source_definition", "cannot alter computed, identity, or user-defined column");
	}
	if (ty == "bit" || ty == "tinyint" || ty == "smallint" || ty == "int" || ty == "bigint" || ty == "real" ||
	    ty == "date" || ty == "uniqueidentifier" || ty == "datetime" || ty == "smalldatetime" || ty == "money" ||
	    ty == "smallmoney") {
		return ty;
	}
	if (ty == "float") {
		return "float(" + std::to_string(prec > 0 ? prec : 53) + ")";
	}
	if (ty == "decimal" || ty == "numeric") {
		return ty + "(" + std::to_string(prec) + "," + std::to_string(scale) + ")";
	}
	if (ty == "time" || ty == "datetime2" || ty == "datetimeoffset") {
		return ty + "(" + std::to_string(scale) + ")";
	}
	if (ty == "varbinary" || ty == "binary") {
		if (max_len == -1) {
			return ty + "(MAX)";
		}
		return ty + "(" + std::to_string(max_len) + ")";
	}
	if (ty == "varchar" || ty == "char" || ty == "nvarchar" || ty == "nchar") {
		int char_len = max_len;
		if (max_len != -1 && (ty == "nvarchar" || ty == "nchar")) {
			char_len = max_len / 2;
		}
		string res = ty + "(" + (max_len == -1 ? string("MAX") : std::to_string(char_len)) + ")";
		if (!collation.empty()) {
			res += " COLLATE " + SafeSQLServerCollation(collation);
		}
		return res;
	}
	RejectDDL("0A000", "source_definition", "unsupported column type for NULL/NOT NULL change");
}

struct DDLMetaData : TableFunctionData {
	ODBCCatalog &catalog;
	string schema;
	string kind;
	string name;
	bool describe = false;
	DDLMetaData(ODBCCatalog &c, bool d) : catalog(c), describe(d) {
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

static unique_ptr<FunctionData> BindMetadata(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                             vector<duckdb::Identifier> &names) {
	if ((in.inputs.size() != 2 && in.inputs.size() != 4) || in.inputs[0].IsNull() || in.inputs[1].IsNull())
		throw BinderException("SQL Server DDL metadata input is incomplete");
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, duckdb::Identifier(in.inputs[0].GetValue<string>()));
	if (!db || db->GetCatalog().GetCatalogType() != "sqlserver")
		throw BinderException("SQL Server DDL catalog is unavailable");
	bool describe = in.inputs.size() == 4;
	auto d = make_uniq<DDLMetaData>(db->GetCatalog().Cast<ODBCCatalog>(), describe);
	d->schema = in.inputs[1].GetValue<string>();
	if (!DDLIntent::Name(d->schema))
		throw BinderException("SQL Server DDL schema name is invalid");
	if (describe) {
		if (in.inputs[2].IsNull() || in.inputs[3].IsNull())
			throw BinderException("SQL Server DDL describe input is incomplete");
		d->kind = in.inputs[2].GetValue<string>();
		d->name = in.inputs[3].GetValue<string>();
		if ((d->kind != "table" && d->kind != "index") || !DDLIntent::Name(d->name))
			throw BinderException("SQL Server DDL describe target is invalid");
	}
	names = {"sqlstate", "stage", "document"};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(d);
}

static void ScanMetadata(ClientContext &ctx, TableFunctionInput &in, DataChunk &out) {
	auto &state = in.global_state->Cast<OneRowState>();
	if (state.done)
		return;
	state.done = true;
	auto &d = in.bind_data->Cast<DDLMetaData>();
	string sqlstate, stage = "source_identity", document;
	ODBCPoolConnection pooled;
	shared_ptr<ODBCConnection> connection;
	unique_ptr<DDLSession> session;
	try {
		if (ctx.IsInterrupted())
			RejectDDL("57014", "source_identity", "interrupted");
		if (!d.catalog.GetConnectionPool().TryGetConnection(pooled))
			RejectDDL("08006", "source_identity", "SQL Server connection unavailable");
		connection = pooled.GetConnection();
		auto dbc = connection->GetConn();
		session = make_uniq<DDLSession>(ctx, dbc, 10);
		auto &s = *session;
		auto facts = QueryServerFacts(s, d.schema);
		string server_id = facts.machine.empty() ? string() : (facts.machine + ":" + facts.instance);

		if (!d.describe) {
			document = string("{") + "\"contract_version\":1,\"product\":\"sqlserver\",\"source_version\":" +
			           JSON(facts.version) + ",\"database\":" + JSON(facts.db_name) +
			           ",\"database_identity\":" + JSON(facts.db_id) + ",\"server_identity\":" + JSON(server_id) +
			           ",\"schema\":" + JSON(d.schema) + ",\"namespace_identity\":" + JSON(facts.schema_id) +
			           ",\"namespace_present\":" + (facts.ns_present ? "true" : "false") +
			           ",\"source_read_only\":" + (facts.read_only ? "true" : "false") +
			           ",\"session_identity\":{\"login\":" + JSON(facts.login) + ",\"user\":" + JSON(facts.db_user) +
			           "},\"visibility\":\"CATALOG_READ\",\"executor_available\":" +
			           (facts.executor_available ? "true" : "false") + ",\"identity_protection\":\"UNVERIFIED\"}";
		} else {
			if (!facts.executor_available) {
				RejectDDL("0A000", "source_identity", "SQL Server < 2005 guarded DDL describe is unsupported");
			}
			stage = "source_metadata";
			s.current_stage = "source_metadata";
			string presence = "ABSENT";
			string object_identity, observed_kind, owning_table, table_identity;
			string cols_json = "[]", indexes_json = "[]", constraints_json = "[]";
			if (facts.ns_present && d.kind == "table") {
				auto objs = s.Query(
				    "SELECT CONVERT(varchar(20),o.object_id),o.type FROM sys.objects o "
				    "JOIN sys.schemas s ON s.schema_id=o.schema_id "
				    "WHERE s.name=? AND o.name=? AND o.is_ms_shipped=0 AND o.type IN ('U','V')",
				    {d.schema, d.name});
				if (objs.size() == 1) {
					presence = "PRESENT";
					object_identity = objs[0][0];
					observed_kind = objs[0][1] == "V" ? "v" : "r";
					auto col_rows = s.Query(
					    "SELECT CONVERT(varchar(20),c.column_id),c.name,ty.name,"
					    "CONVERT(varchar(20),c.max_length),CONVERT(varchar(20),c.[precision]),"
					    "CONVERT(varchar(20),c.[scale]),CONVERT(varchar(5),c.is_nullable),"
					    "COALESCE(dc.name,''),COALESCE(dc.definition,'') "
					    "FROM sys.columns c JOIN sys.types ty ON ty.user_type_id=c.user_type_id "
					    "LEFT JOIN sys.default_constraints dc ON dc.object_id=c.default_object_id "
					    "WHERE c.object_id=" +
					    object_identity + " ORDER BY c.column_id");
					cols_json = "[";
					for (idx_t idx = 0; idx < col_rows.size() && idx < 256; idx++) {
						if (idx > 0)
							cols_json += ",";
						auto &r = col_rows[idx];
						cols_json += "{\"ordinal\":" + r[0] + ",\"name\":" + JSON(r[1]) + ",\"type\":" + JSON(r[2]) +
						             ",\"max_length\":" + r[3] + ",\"precision\":" + r[4] + ",\"scale\":" + r[5] +
						             ",\"nullable\":" + (r[6] == "1" ? "true" : "false") +
						             ",\"default_name\":" + JSON(r[7]) + ",\"default\":" + JSON(r[8]) + "}";
					}
					cols_json += "]";

					auto idx_rows = s.Query(
					    "SELECT CONVERT(varchar(20),i.index_id),i.name,i.type_desc,"
					    "CONVERT(varchar(5),i.is_unique),CONVERT(varchar(5),i.is_primary_key) "
					    "FROM sys.indexes i WHERE i.object_id=" +
					    object_identity + " AND i.index_id>0 ORDER BY i.index_id");
					indexes_json = "[";
					for (idx_t idx = 0; idx < idx_rows.size() && idx < 256; idx++) {
						if (idx > 0)
							indexes_json += ",";
						auto &r = idx_rows[idx];
						indexes_json += "{\"table\":" + JSON(d.name) + ",\"name\":" + JSON(r[1]) +
						                ",\"type\":" + JSON(r[2]) +
						                ",\"unique\":" + (r[3] == "1" ? "true" : "false") +
						                ",\"primary\":" + (r[4] == "1" ? "true" : "false") + "}";
					}
					indexes_json += "]";

					auto con_rows = s.Query(
					    "SELECT kc.name,kc.type FROM sys.key_constraints kc WHERE kc.parent_object_id=" +
					    object_identity +
					    " UNION ALL SELECT fk.name,'F' FROM sys.foreign_keys fk WHERE fk.parent_object_id=" +
					    object_identity +
					    " UNION ALL SELECT ch.name,'C' FROM sys.check_constraints ch WHERE ch.parent_object_id=" +
					    object_identity + " ORDER BY 1");
					constraints_json = "[";
					for (idx_t idx = 0; idx < con_rows.size() && idx < 256; idx++) {
						if (idx > 0)
							constraints_json += ",";
						constraints_json +=
						    "{\"name\":" + JSON(con_rows[idx][0]) + ",\"type\":" + JSON(con_rows[idx][1]) + "}";
					}
					constraints_json += "]";
				}
			} else if (facts.ns_present && d.kind == "index") {
				auto idx_rows = s.Query(
				    "SELECT CONVERT(varchar(20),t.object_id),t.name,CONVERT(varchar(20),i.index_id),"
				    "i.name,CONVERT(varchar(5),i.is_unique),CONVERT(varchar(5),i.is_primary_key) "
				    "FROM sys.indexes i JOIN sys.tables t ON t.object_id=i.object_id "
				    "JOIN sys.schemas s ON s.schema_id=t.schema_id "
				    "WHERE s.name=? AND i.name=? AND t.is_ms_shipped=0 AND i.index_id>0 ORDER BY t.name,i.index_id",
				    {d.schema, d.name});
				indexes_json = "[";
				for (idx_t idx = 0; idx < idx_rows.size() && idx < 256; idx++) {
					if (idx > 0)
						indexes_json += ",";
					auto &r = idx_rows[idx];
					indexes_json += "{\"table\":" + JSON(r[1]) + ",\"name\":" + JSON(r[3]) +
					                ",\"unique\":" + (r[4] == "1" ? "true" : "false") +
					                ",\"primary\":" + (r[5] == "1" ? "true" : "false") + "}";
				}
				indexes_json += "]";
				if (idx_rows.size() == 1) {
					presence = "PRESENT";
					table_identity = idx_rows[0][0];
					owning_table = idx_rows[0][1];
					object_identity = idx_rows[0][0] + ":" + idx_rows[0][2];
					observed_kind = "i";
				} else if (idx_rows.size() > 1) {
					presence = "UNKNOWN";
				}
			}
			document = string("{") + "\"contract_version\":1,\"product\":\"sqlserver\",\"source_version\":" +
			           JSON(facts.version) + ",\"database\":" + JSON(facts.db_name) +
			           ",\"database_identity\":" + JSON(facts.db_id) + ",\"server_identity\":" + JSON(server_id) +
			           ",\"schema\":" + JSON(d.schema) + ",\"name\":" + JSON(d.name) + ",\"kind\":" + JSON(d.kind) +
			           ",\"namespace_identity\":" + JSON(facts.schema_id) +
			           ",\"namespace_present\":" + (facts.ns_present ? "true" : "false") +
			           ",\"presence\":" + JSON(presence) + ",\"object_identity\":" + JSON(object_identity);
			if (!observed_kind.empty()) {
				document += ",\"observed_kind\":" + JSON(observed_kind);
			}
			if (!owning_table.empty()) {
				document += ",\"owning_table\":" + JSON(owning_table);
			}
			if (!table_identity.empty()) {
				document += ",\"table_identity\":" + JSON(table_identity);
			}
			document += ",\"metadata_complete\":false,\"identity_protection\":\"UNVERIFIED\",\"columns\":" + cols_json +
			            ",\"indexes\":" + indexes_json + ",\"constraints\":" + constraints_json + "}";
		}
		stage.clear();
	} catch (const DDLRejectException &rej) {
		sqlstate = ValidSQLState(rej.sqlstate, "HY000");
		stage = rej.stage;
		document.clear();
	} catch (...) {
		sqlstate = session && !session->sqlstate.empty()
		               ? session->sqlstate
		               : (ctx.IsInterrupted() ? "57014" : "08006");
		document.clear();
	}
	session.reset();
	if (connection)
		connection->Close();
	out.SetValue(0, 0, Value(sqlstate));
	out.SetValue(1, 0, Value(stage));
	out.SetValue(2, 0, Value(document));
	out.SetChildCardinality(1);
}

static unique_ptr<FunctionData> BindDDL(ClientContext &ctx, TableFunctionBindInput &in, vector<LogicalType> &types,
                                        vector<duckdb::Identifier> &names) {
	auto cond_it = in.named_parameters.find("conditional");
	const bool conditional = cond_it != in.named_parameters.end() && !cond_it->second.IsNull() &&
	                         cond_it->second.GetValue<bool>();
	auto intent = DDLIntent::Bind(in.inputs, conditional);
	auto db = DatabaseManager::Get(ctx).GetDatabase(ctx, duckdb::Identifier(DDLIntent::Text(in.inputs[0])));
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
	const auto &i = d.intent;
	string status = "CONFIRMED_NOT_EXECUTED", error = "0A000", stage = "source_identity";
	int64_t warnings = 0;
	string source_error;
	ODBCPoolConnection pooled;
	shared_ptr<ODBCConnection> connection;
	unique_ptr<DDLSession> session;
	bool in_tx = false, sent = false, commit_sent = false;
	try {
		if (ctx.IsInterrupted()) {
			RejectDDL("57014", "source_identity", "operation cancelled");
		}
		if (!d.catalog.GetConnectionPool().TryGetConnection(pooled)) {
			RejectDDL("08006", "source_identity", "SQL Server connection unavailable");
		}
		connection = pooled.GetConnection();
		auto dbc = connection->GetConn();
		session = make_uniq<DDLSession>(ctx, dbc, i.action == "reindex" ? 290 : 30);
		auto &s = *session;
		if (s.Scalar("SELECT CONVERT(varchar(20),@@TRANCOUNT)") != "0") {
			RejectDDL("HY000", "source_identity", "SQL Server DDL connection is not idle");
		}
		auto facts = QueryServerFacts(s, i.schema);
		if (facts.db_name != i.database) {
			RejectDDL("0A000", "source_identity", "SQL Server DDL database identity mismatch");
		}
		if (!facts.executor_available) {
			RejectDDL("0A000", "source_identity", "SQL Server < 2005 guarded DDL is unsupported");
		}
		if (facts.read_only) {
			RejectDDL("25006", "source_identity", "SQL Server DDL source is read-only");
		}
		if (!facts.ns_present && i.action != "create_schema" && i.action != "drop_schema") {
			RejectDDL("3F000", "source_definition", "target schema does not exist");
		}

		// Session safety settings + short lock timeout (2000ms) so DDL fails fast when online traffic holds locks.
		s.Exec("SET DATEFORMAT ymd;SET XACT_ABORT ON;SET NOCOUNT OFF;SET ANSI_NULLS ON;SET QUOTED_IDENTIFIER ON;"
		       "SET ANSI_WARNINGS ON;SET ARITHABORT ON;SET NUMERIC_ROUNDABORT OFF;SET LOCK_TIMEOUT 2000;"
		       "BEGIN TRANSACTION");
		in_tx = true;

		stage = "source_definition";
		s.current_stage = "source_definition";
		const string target = Quote(i.schema) + "." + Quote(i.name);
		auto obj_id_of = [&](const string &obj_name) {
			return s.Scalar("SELECT COALESCE(CONVERT(varchar(20),MAX(o.object_id)),'') FROM sys.objects o "
			                "JOIN sys.schemas s ON s.schema_id=o.schema_id WHERE s.name=? AND o.name=? "
			                "AND o.is_ms_shipped=0 AND o.type='U'",
			                {i.schema, obj_name});
		};
		auto any_obj_exists = [&](const string &obj_name) {
			return s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.objects o "
			                "JOIN sys.schemas s ON s.schema_id=o.schema_id WHERE s.name=? AND o.name=?",
			                {i.schema, obj_name}) != "0";
		};
		auto check_col_dependencies = [&](const string &obj_id, const string &col_id) {
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.index_columns ic "
			             "JOIN sys.indexes ix ON ix.object_id=ic.object_id AND ix.index_id=ic.index_id "
			             "WHERE ic.object_id=" +
			             obj_id + " AND ic.column_id=" + col_id + " AND ix.index_id>0") != "0") {
				RejectDDL("2BP01", "source_definition", "cannot modify or drop column referenced by an index");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.foreign_key_columns "
			             "WHERE (parent_object_id=" +
			             obj_id + " AND parent_column_id=" + col_id + ") OR (referenced_object_id=" + obj_id +
			             " AND referenced_column_id=" + col_id + ")") != "0") {
				RejectDDL("2BP01", "source_definition",
				          "cannot modify or drop column referenced by a foreign key constraint");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.check_constraints ch "
			             "WHERE ch.parent_object_id=" +
			             obj_id + " AND (ch.parent_column_id=" + col_id +
			             " OR EXISTS (SELECT 1 FROM sys.sql_expression_dependencies ed "
			             "WHERE ed.referencing_id=ch.object_id AND ed.referenced_id=" +
			             obj_id + " AND ed.referenced_minor_id=" + col_id + "))") != "0") {
				RejectDDL("2BP01", "source_definition",
				          "cannot modify or drop column referenced by a check constraint");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.sql_expression_dependencies ed "
			             "JOIN sys.objects o ON o.object_id=ed.referencing_id "
			             "WHERE ed.referenced_id=" +
			             obj_id + " AND (ed.referenced_minor_id=" + col_id +
			             " OR ed.referenced_minor_id=0) AND o.type='V'") != "0") {
				RejectDDL("2BP01", "source_definition", "cannot modify or drop column referenced by a view");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.sql_expression_dependencies ed "
			             "WHERE ed.referenced_id=" +
			             obj_id + " AND ed.referenced_minor_id=" + col_id +
			             " AND ed.referencing_id <> COALESCE((SELECT default_object_id FROM sys.columns "
			             "WHERE object_id=" +
			             obj_id + " AND column_id=" + col_id + "),0)") != "0") {
				RejectDDL("2BP01", "source_definition",
				          "cannot modify or drop column with dependent objects or computed columns");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.stats_columns sc "
			             "JOIN sys.stats st ON st.object_id=sc.object_id AND st.stats_id=sc.stats_id "
			             "WHERE sc.object_id=" +
			             obj_id + " AND sc.column_id=" + col_id + " AND st.user_created=1") != "0") {
				RejectDDL("2BP01", "source_definition",
				          "cannot modify or drop column referenced by user-created statistics");
			}
		};

		string native_sql = i.sql;
		bool sp_rename_op = false;
		bool no_op_commit = false;
		bool source_noop = false;

		if (i.action == "create_table" || i.action == "create_table_as") {
			if (!obj_id_of(i.name).empty()) {
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42P07", "source_definition", "target object already exists");
				}
			} else if (any_obj_exists(i.name)) {
				RejectDDL("42P07", "source_definition", "target object already exists");
			}
		} else if (i.action == "drop_table") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				if (any_obj_exists(i.name)) {
					RejectDDL("42809", "source_definition", "target object is not a base table");
				}
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42P01", "source_definition", "target table does not exist");
				}
			} else {
				if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.foreign_keys WHERE referenced_object_id=" +
				             obj_id + " AND parent_object_id<>" + obj_id) != "0") {
					RejectDDL("2BP01", "source_definition", "cannot drop table with referencing foreign keys");
				}
			}
		} else if (i.action == "create_schema") {
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.schemas WHERE name=?", {i.name}) != "0") {
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42P06", "source_definition", "target schema already exists");
				}
			}
		} else if (i.action == "drop_schema") {
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.schemas WHERE name=?", {i.name}) == "0") {
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("3F000", "source_definition", "target schema does not exist");
				}
			}
		} else if (i.action == "rename_table") {
			if (obj_id_of(i.name).empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (any_obj_exists(i.new_name)) {
				RejectDDL("42P07", "source_definition", "destination table already exists");
			}
			sp_rename_op = true;
		} else if (i.action == "add_column") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			const auto &c = i.column_defs[0];
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.columns WHERE object_id=" + obj_id +
			             " AND name=?",
			             {c.name}) != "0") {
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42701", "source_definition", "column already exists");
				}
			} else if (c.not_null && (c.default_kind.empty() || c.default_kind == "null")) {
				if (s.Scalar("SELECT CASE WHEN EXISTS(SELECT 1 FROM " + target + ") THEN '1' ELSE '0' END") == "1") {
					RejectDDL("23502", "source_definition",
					          "cannot add NOT NULL column without default to non-empty table");
				}
			}
		} else if (i.action == "drop_column") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			auto col_rows = s.Query("SELECT CONVERT(varchar(20),c.column_id),COALESCE(dc.name,'') FROM sys.columns c "
			                        "LEFT JOIN sys.default_constraints dc ON dc.object_id=c.default_object_id "
			                        "WHERE c.object_id=" +
			                            obj_id + " AND c.name=?",
			                        {i.column_name});
			if (col_rows.empty()) {
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42703", "source_definition", "column does not exist");
				}
			} else {
				if (std::stoi(s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.columns WHERE object_id=" +
				                       obj_id)) <= 1) {
					RejectDDL("42P16", "source_definition", "cannot drop the only column of a table");
				}
				check_col_dependencies(obj_id, col_rows[0][0]);
				native_sql.clear();
				if (!col_rows[0][1].empty()) {
					native_sql = "ALTER TABLE " + target + " DROP CONSTRAINT " + Quote(col_rows[0][1]) + ";";
				}
				native_sql += "ALTER TABLE " + target + " DROP COLUMN " + Quote(i.column_name);
			}
		} else if (i.action == "alter_column_type") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			auto col_rows = s.Query(
			    "SELECT ty.name,CONVERT(varchar(20),c.max_length),CONVERT(varchar(20),c.[precision]),"
			    "CONVERT(varchar(20),c.[scale]),COALESCE(c.collation_name,''),CONVERT(varchar(5),c.is_computed),"
			    "CONVERT(varchar(5),c.is_identity),CONVERT(varchar(5),c.is_nullable),"
			    "CONVERT(varchar(5),ty.is_user_defined),CONVERT(varchar(20),c.column_id),"
			    "COALESCE(dc.name,''),COALESCE(dc.definition,''),CONVERT(varchar(5),COALESCE(dc.is_system_named,1)) "
			    "FROM sys.columns c JOIN sys.types ty ON ty.user_type_id=c.user_type_id "
			    "LEFT JOIN sys.default_constraints dc ON dc.object_id=c.default_object_id "
			    "WHERE c.object_id=" +
			        obj_id + " AND c.name=?",
			    {i.column_name});
			if (col_rows.size() != 1) {
				RejectDDL("42703", "source_definition", "column does not exist");
			}
			const auto &r = col_rows[0];
			if (r[5] == "1" || r[6] == "1" || r[8] == "1") {
				RejectDDL("0A000", "source_definition", "cannot alter computed, identity, or user-defined column");
			}
			check_col_dependencies(obj_id, r[9]);
			const string ty = StringUtil::Lower(r[0]);
			int max_len = std::stoi(r[1]);
			int prec = std::stoi(r[2]);
			int scale = std::stoi(r[3]);
			const string collation = r[4];
			const bool nullable = r[7] != "0";
			const auto &target_col = i.column_defs[0];
			bool widened = false;
			string collate_clause;
			if (ty == "smallint" && (target_col.type == "integer" || target_col.type == "bigint")) {
				widened = true;
			} else if (ty == "int" && target_col.type == "bigint") {
				widened = true;
			} else if ((ty == "real" || (ty == "float" && prec <= 24)) && target_col.type == "double") {
				widened = true;
			} else if ((ty == "decimal" || ty == "numeric") && target_col.type == "numeric") {
				widened = target_col.scale == scale && target_col.precision > prec && target_col.precision <= 38;
			} else if (ty == "nvarchar" && target_col.type == "varchar") {
				if (max_len > 0 && (max_len % 2) == 0) {
					int cur_len = max_len / 2;
					if (target_col.length > cur_len && target_col.length <= 4000) {
						widened = true;
						if (!collation.empty()) {
							collate_clause = " COLLATE " + SafeSQLServerCollation(collation);
						}
					}
				}
			} else if (ty == "time" && target_col.type == "time") {
				widened = target_col.precision > scale && target_col.precision <= 6;
			} else if (ty == "datetime2" && target_col.type == "timestamp") {
				widened = target_col.precision > scale && target_col.precision <= 6;
			} else if (ty == "datetimeoffset" && target_col.type == "timestamptz") {
				widened = target_col.precision > scale && target_col.precision <= 6;
			}
			if (!widened) {
				RejectDDL("0A000", "source_definition", "only lossless type widening is supported");
			}
			const string df_name = r[10];
			const string df_def = r[11];
			const bool df_sys_named = r[12] != "0";
			const string alter_col_sql = "ALTER TABLE " + target + " ALTER COLUMN " + Quote(i.column_name) + " " +
			                             target_col.native_type + collate_clause + (nullable ? " NULL" : " NOT NULL");
			if (!df_name.empty()) {
				if (df_def.empty()) {
					RejectDDL("0A000", "source_definition",
					          "cannot preserve default constraint definition during alter_column_type");
				}
				const string add_df_sql =
				    "ALTER TABLE " + target +
				    (df_sys_named ? " ADD DEFAULT " : (" ADD CONSTRAINT " + Quote(df_name) + " DEFAULT ")) + df_def +
				    " FOR " + Quote(i.column_name);
				native_sql = "ALTER TABLE " + target + " DROP CONSTRAINT " + Quote(df_name) + ";" + alter_col_sql +
				             ";" + add_df_sql;
			} else {
				native_sql = alter_col_sql;
			}
		} else if (i.action == "rename_column") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.columns WHERE object_id=" + obj_id +
			             " AND name=?",
			             {i.column_name}) == "0") {
				RejectDDL("42703", "source_definition", "column does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.columns WHERE object_id=" + obj_id +
			             " AND name=?",
			             {i.new_name}) != "0") {
				RejectDDL("42701", "source_definition", "destination column already exists");
			}
			sp_rename_op = true;
		} else if (i.action == "set_default" || i.action == "drop_default") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			auto col_rows = s.Query("SELECT COALESCE(dc.name,'') FROM sys.columns c "
			                        "LEFT JOIN sys.default_constraints dc ON dc.object_id=c.default_object_id "
			                        "WHERE c.object_id=" +
			                            obj_id + " AND c.name=?",
			                        {i.column_name});
			if (col_rows.size() != 1) {
				RejectDDL("42703", "source_definition", "column does not exist");
			}
			string existing_df = col_rows[0][0];
			if (i.action == "drop_default") {
				if (existing_df.empty()) {
					no_op_commit = true;
				} else {
					native_sql = "ALTER TABLE " + target + " DROP CONSTRAINT " + Quote(existing_df);
				}
			} else {
				native_sql.clear();
				if (!existing_df.empty()) {
					native_sql = "ALTER TABLE " + target + " DROP CONSTRAINT " + Quote(existing_df) + ";";
				}
				native_sql += i.sql;
			}
		} else if (i.action == "set_not_null" || i.action == "drop_not_null") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			auto col_rows = s.Query(
			    "SELECT ty.name,CONVERT(varchar(20),c.max_length),CONVERT(varchar(20),c.[precision]),"
			    "CONVERT(varchar(20),c.[scale]),COALESCE(c.collation_name,''),CONVERT(varchar(5),c.is_computed),"
			    "CONVERT(varchar(5),c.is_identity),CONVERT(varchar(5),c.is_nullable),"
			    "CONVERT(varchar(5),ty.is_user_defined) "
			    "FROM sys.columns c JOIN sys.types ty ON ty.user_type_id=c.user_type_id "
			    "WHERE c.object_id=" +
			        obj_id + " AND c.name=?",
			    {i.column_name});
			if (col_rows.size() != 1) {
				RejectDDL("42703", "source_definition", "column does not exist");
			}
			string col_type = BuildSQLServerAlterColumnType(col_rows[0]);
			native_sql = "ALTER TABLE " + target + " ALTER COLUMN " + Quote(i.column_name) + " " + col_type +
			             (i.action == "set_not_null" ? " NOT NULL" : " NULL");
		} else if (i.action == "add_constraint") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (any_obj_exists(i.constraint_name)) {
				RejectDDL("42710", "source_definition", "constraint name already exists");
			}
			if (i.constraint_kind == "foreign_key") {
				if (i.reference_schema != i.schema) {
					RejectDDL("0A000", "source_definition", "foreign key schema must match source schema");
				}
				if (obj_id_of(i.reference_table).empty()) {
					RejectDDL("42P01", "source_definition", "referenced table does not exist");
				}
			}
		} else if (i.action == "drop_constraint") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.objects WHERE parent_object_id=" + obj_id +
			             " AND name=? AND type IN('PK','UQ','F','C','D')",
			             {i.constraint_name}) == "0") {
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42704", "source_definition", "constraint does not exist");
				}
			}
		} else if (i.action == "create_index" || i.action == "create_unique_index") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.indexes WHERE object_id=" + obj_id +
			             " AND name=?",
			             {i.index}) != "0") {
				if (i.conditional) {
					source_noop = true;
				} else {
					RejectDDL("42P07", "source_definition", "index already exists on table");
				}
			}
		} else if (i.action == "drop_index") {
			if (i.owner.empty()) {
				if (!i.conditional) {
					RejectDDL("0A000", "source_definition", "owning table required for SQL Server DROP INDEX");
				}
				if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.indexes ix "
				             "JOIN sys.tables t ON t.object_id=ix.object_id "
				             "JOIN sys.schemas s ON s.schema_id=t.schema_id "
				             "WHERE s.name=? AND ix.name=? AND t.is_ms_shipped=0 AND ix.index_id>0",
				             {i.schema, i.name}) == "0") {
					source_noop = true;
				} else {
					RejectDDL("0A000", "source_definition", "owning table required for SQL Server DROP INDEX");
				}
			} else {
				auto obj_id = obj_id_of(i.owner);
				if (obj_id.empty()) {
					if (i.conditional) {
						source_noop = true;
					} else {
						RejectDDL("42P01", "source_definition", "owning table does not exist");
					}
				} else {
					auto ix_rows = s.Query("SELECT CONVERT(varchar(5),is_primary_key),"
					                       "CONVERT(varchar(5),is_unique_constraint) "
					                       "FROM sys.indexes WHERE object_id=" +
					                           obj_id + " AND name=? AND index_id>0",
					                       {i.name});
					if (ix_rows.empty()) {
						if (i.conditional) {
							source_noop = true;
						} else {
							RejectDDL("42704", "source_definition", "index does not exist");
						}
					} else if (ix_rows[0][0] == "1" || ix_rows[0][1] == "1") {
						RejectDDL("2BP01", "source_definition", "cannot drop constraint backing index directly");
					}
				}
			}
		} else if (i.action == "rename_index") {
			auto obj_id = obj_id_of(i.owner);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "owning table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.indexes WHERE object_id=" + obj_id +
			             " AND name=? AND index_id>0",
			             {i.name}) == "0") {
				RejectDDL("42704", "source_definition", "index does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.indexes WHERE object_id=" + obj_id +
			             " AND name=?",
			             {i.new_name}) != "0") {
				RejectDDL("42P07", "source_definition", "destination index already exists");
			}
			sp_rename_op = true;
		} else if (i.action == "truncate") {
			auto obj_id = obj_id_of(i.name);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "target table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.foreign_keys WHERE referenced_object_id=" +
			             obj_id) != "0") {
				RejectDDL("2BP01", "source_definition", "cannot truncate table with referencing foreign keys");
			}
			if (i.identity_mode == "continue" &&
			    s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.columns WHERE object_id=" + obj_id +
			             " AND is_identity=1") != "0") {
				RejectDDL("0A000", "source_definition",
				          "TRUNCATE CONTINUE IDENTITY is not supported on SQL Server identity columns");
			}
		} else if (i.action == "reindex") {
			auto obj_id = obj_id_of(i.owner);
			if (obj_id.empty()) {
				RejectDDL("42P01", "source_definition", "owning table does not exist");
			}
			if (s.Scalar("SELECT CONVERT(varchar(20),COUNT(*)) FROM sys.indexes WHERE object_id=" + obj_id +
			             " AND name=? AND index_id>0",
			             {i.name}) == "0") {
				RejectDDL("42704", "source_definition", "index does not exist");
			}
		} else {
			RejectDDL("0A000", "source_definition", "unsupported SQL Server DDL action");
		}

		stage = "source_execute";
		s.current_stage = "source_execute";
		if (!no_op_commit && !source_noop) {
			if (ctx.IsInterrupted()) {
				RejectDDL("57014", "source_execute", "operation cancelled");
			}
			s.last_warning_count = 0;
			sent = true;
			s.Exec(native_sql, false, sp_rename_op);
			if (s.Scalar("SELECT CASE WHEN @@TRANCOUNT=1 AND XACT_STATE()=1 THEN '1' ELSE '0' END") != "1") {
				RejectDDL("40001", "source_execute", "SQL Server DDL transaction invalidated before commit");
			}
		}

		stage = "source_commit";
		s.current_stage = "source_commit";
		commit_sent = true;
		status = "UNKNOWN";
		s.Exec("COMMIT TRANSACTION");
		if (s.Scalar("SELECT CASE WHEN @@TRANCOUNT=0 AND XACT_STATE()=0 THEN '1' ELSE '0' END") != "1") {
			RejectDDL("40001", "source_commit", "SQL Server DDL commit verification failed");
		}
		in_tx = false;
		status = "COMMITTED";
		if (source_noop) {
			warnings = 0;
			error.clear();
			stage = "source_noop";
		} else {
			warnings = s.last_warning_count;
			error.clear();
			stage = warnings > 0 ? "source_warning" : "";
		}
	} catch (const GuardedODBCLocalRejection &) {
		error = status == "COMMITTED" ? "XX000" : "0A000";
		stage = "source_identity";
	} catch (const DDLRejectException &rej) {
		if (source_error.empty() && session) {
			source_error = session->source.JSON();
		}
		error = ValidSQLState(rej.sqlstate, "HY000");
		stage = rej.stage;
	} catch (...) {
		if (source_error.empty() && session) {
			source_error = session->source.JSON();
		}
		error = session && !session->sqlstate.empty()
		            ? session->sqlstate
		            : (ctx.IsInterrupted() ? "57014" : "08006");
	}

	if (status != "COMMITTED" && in_tx && session) {
		try {
			session->Exec("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION", true);
			auto state_after =
			    session->Scalar("SELECT CASE WHEN @@TRANCOUNT=0 AND XACT_STATE()=0 THEN '1' ELSE '0' END", {}, true);
			if (state_after == "1" && !commit_sent) {
				status = sent ? "CONFIRMED_ROLLED_BACK" : "CONFIRMED_NOT_EXECUTED";
				if (stage == "source_lock" || stage == "source_definition") {
					if (status != "CONFIRMED_NOT_EXECUTED") {
						stage = "source_execute";
					}
				}
			} else if (sent || commit_sent) {
				status = "UNKNOWN";
			}
		} catch (...) {
			if (sent || commit_sent) {
				status = "UNKNOWN";
			}
		}
	}
	if (status == "CONFIRMED_ROLLED_BACK" && (stage == "source_lock" || stage == "source_definition")) {
		stage = "source_execute";
	}
	if (status != "COMMITTED") {
		warnings = 0;
		if (stage == "source_warning" || stage == "source_verify") {
			stage = "source_execute";
		}
		error = ValidSQLState(error, "HY000");
	}

	session.reset();
	if (connection)
		connection->Close();
	out.SetValue(0, 0, Value(i.operation));
	out.SetValue(1, 0, Value(status));
	out.SetValue(2, 0, Value(error));
	out.SetValue(3, 0, Value(stage));
	out.SetValue(4, 0, Value::BIGINT(warnings));
	if (d.diagnostics)
		out.SetValue(5, 0, Value(source_error));
	out.SetChildCardinality(1);
}

} // namespace

void RegisterSQLServerGuardedDDL(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    TableFunction("sqlserver_ddl_capabilities", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                  ScanMetadata, BindMetadata, InitOne));
	loader.RegisterFunction(TableFunction(
	    "sqlserver_ddl_describe",
	    {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, ScanMetadata,
	    BindMetadata, InitOne));
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
