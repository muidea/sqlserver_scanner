#include "storage/odbc_catalog.hpp"

#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/storage/database_size.hpp"

#include "odbc_connection.hpp"
#include "odbc_scanner.hpp"
#include "odbc_types.hpp"
#include "storage/odbc_schema_entry.hpp"
#include "storage/odbc_transaction.hpp"

namespace duckdb {

ODBCCatalog::ODBCCatalog(AttachedDatabase &db_p, string connection_string_p, string attach_path_p,
                         string default_schema_p, AccessMode access_mode)
    : Catalog(db_p), connection_string(std::move(connection_string_p)), attach_path(std::move(attach_path_p)),
      access_mode(access_mode), schemas(*this), default_schema(std::move(default_schema_p)), connection_pool(*this) {
	ODBCConnectionParameters connection_params;
	unordered_set<string> unused_params;
	std::tie(connection_params, unused_params) = ODBCUtils::ParseConnectionParameters(connection_string);
	StringUtil::Trim(default_schema);
	Value connection_limit;
	auto &db_instance = db_p.GetDatabase();
	if (db_instance.TryGetCurrentSetting("sqlserver_connection_limit", connection_limit)) {
		connection_pool.SetMaximumConnections(UBigIntValue::Get(connection_limit));
	}
	// try to connect
	ODBCTypeConfig type_config(db_instance);
	auto connection = ODBCConnection::Open(type_config, connection_string);
	if (default_schema.empty()) {
		try {
			auto result = connection->Query("SELECT SCHEMA_NAME()", ODBCResultStreaming::FORCE_MATERIALIZATION);
			if (result->Next() && !result->IsNull(0)) {
				default_schema = result->GetString(0);
				StringUtil::Trim(default_schema);
			}
		} catch (...) {
		}
	}
	if (default_schema.empty()) {
		default_schema = "dbo";
	}
}

ODBCCatalog::~ODBCCatalog() = default;

string AddConnectionOption(const KeyValueSecret &kv_secret, const string &name,
                           const unordered_set<string> &existing_params) {
	if (existing_params.find(name) != existing_params.end()) {
		// option already provided in connection string
		return string();
	}
	Value input_val = kv_secret.TryGetValue(name);
	if (input_val.IsNull()) {
		// not provided
		return string();
	}
	string result = name + "={";
	for (auto ch : input_val.ToString()) {
		result += ch == '}' ? "}}" : string(1, ch);
	}
	result += "};";
	return result;
}

unique_ptr<SecretEntry> GetSecret(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	// FIXME: this should be adjusted once the `GetSecretByName` API supports this
	// use case
	auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "memory");
	if (secret_entry) {
		return secret_entry;
	}
	secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "local_file");
	if (secret_entry) {
		return secret_entry;
	}
	return nullptr;
}

struct URIToken {
	string value;
	char delimiter;
};

string UnescapePercentage(const string &input, idx_t start, idx_t end) {
	// url escapes encoded as [ESC][RESULT]
	auto url_escapes = "20 3C<3E>23#25%2B+7B{7D}7C|5C\\5E^7E~5B[5D]60`3B;2F/3F?3A:40@3D=26&24$21!2A*27'22\"28(29)2C,";

	string result;
	for (idx_t i = start; i < end; i++) {
		if (i + 2 < end && input[i] == '%') {
			// find the escape code
			char first_char = StringUtil::CharacterToUpper(input[i + 1]);
			char second_char = StringUtil::CharacterToUpper(input[i + 2]);
			char escape_result = '\0';
			for (idx_t esc_pos = 0; url_escapes[esc_pos]; esc_pos += 3) {
				if (first_char == url_escapes[esc_pos] && second_char == url_escapes[esc_pos + 1]) {
					// found the correct escape
					escape_result = url_escapes[esc_pos + 2];
					break;
				}
			}
			if (escape_result != '\0') {
				// found the escape - skip forward
				result += escape_result;
				i += 2;
				continue;
			}
			// escape not found - just put the % in as normal
		}
		result += input[i];
	}
	return result;
}

vector<URIToken> ParseURITokens(const string &dsn, idx_t start) {
	vector<URIToken> result;
	for (idx_t pos = start; pos < dsn.size(); pos++) {
		switch (dsn[pos]) {
		case ':':
		case '@':
		case '/':
		case '?':
		case '=':
		case '&': {
			// found a delimiter
			URIToken token;
			token.value = UnescapePercentage(dsn, start, pos);
			token.delimiter = dsn[pos];
			start = pos + 1;
			result.push_back(std::move(token));
			break;
		}
		default:
			// include in token
			break;
		}
	}
	URIToken token;
	token.value = UnescapePercentage(dsn, start, dsn.size());
	token.delimiter = '\0';
	result.push_back(std::move(token));
	return result;
}

struct URIValue {
	URIValue(string name_p, string value_p) : name(std::move(name_p)), value(std::move(value_p)) {
	}

	string name;
	string value;
};

vector<string> GetAttributeNames(const vector<URIToken> &tokens, idx_t token_count, ErrorData &error) {
	// [scheme://][user[:[password]]@]host[:port][/schema][?attribute1=value1&attribute2=value2...
	vector<string> result;
	if (token_count == 1) {
		// only one token - always the host
		result.emplace_back("server");
		return result;
	}
	idx_t current_pos = 0;
	if (tokens[0].delimiter == '@') {
		// user@...
		result.emplace_back("uid");
		result.emplace_back("server");
		current_pos = 1;
	} else if (tokens[1].delimiter == '@') {
		// user:password@
		if (tokens[0].delimiter != ':') {
			error = ParserException("Invalid URI string - expected user:password");
			return result;
		}
		D_ASSERT(token_count > 2);
		result.emplace_back("uid");
		result.emplace_back("pwd");
		result.emplace_back("server");
		current_pos = 2;
	} else {
		// neither user nor password - this MUST be the host
		result.emplace_back("server");
		current_pos = 0;
	}
	if (current_pos + 1 == token_count) {
		// we have parsed the entire string (until the attributes)
		return result;
	}
	// we are at host_pos
	if (tokens[current_pos].delimiter == ':') {
		// host:port
		result.emplace_back("port");
		current_pos++;
		if (current_pos + 1 == token_count) {
			return result;
		}
		// we still have a "/schema"
		if (tokens[current_pos].delimiter != '/') {
			error = ParserException("Invalid URI string - expected host:port/schema");
		}
		result.emplace_back("database");
		current_pos++;
	} else if (tokens[current_pos].delimiter == '/') {
		// host/schema
		result.emplace_back("database");
		current_pos++;
	} else {
		error = ParserException("Invalid URI string - expected host:port or host/schema");
	}
	if (current_pos + 1 != token_count) {
		error = ParserException("Invalid URI string - expected ? after "
		                        "[user[:[password]]@]host[:port][/schema]");
	}
	return result;
}

void ParseMainAttributes(const vector<URIToken> &tokens, idx_t token_count, vector<URIValue> &result,
                         ErrorData &error) {
	auto attribute_names = GetAttributeNames(tokens, token_count, error);
	if (error.HasError()) {
		return;
	}
	D_ASSERT(attribute_names.size() == token_count);
	for (idx_t i = 0; i < token_count; i++) {
		result.emplace_back(attribute_names[i], tokens[i].value);
	}
}

void ParseAttributes(const vector<URIToken> &tokens, idx_t attribute_start, vector<URIValue> &result) {
	unordered_map<string, string> uri_attribute_map;
	uri_attribute_map["socket"] = "socket";
	uri_attribute_map["compression"] = "compression";
	uri_attribute_map["ssl-mode"] = "ssl_mode";
	uri_attribute_map["ssl-ca"] = "ssl_ca";
	uri_attribute_map["ssl-capath"] = "ssl_capath";
	uri_attribute_map["ssl-cert"] = "ssl_cert";
	uri_attribute_map["ssl-cipher"] = "ssl_cipher";
	uri_attribute_map["ssl-crl"] = "ssl_crl";
	uri_attribute_map["ssl-crlpath"] = "ssl_crlpath";
	uri_attribute_map["ssl-key"] = "ssl_key";

	// parse key=value attributes
	for (idx_t i = attribute_start; i < tokens.size(); i += 2) {
		// check if the format is correct
		if (i + 1 >= tokens.size() || tokens[i].delimiter != '=') {
			throw ParserException("Invalid URI string - expected attribute=value pairs after ?");
		}
		if (tokens[i + 1].delimiter != '\0' && tokens[i + 1].delimiter != '&') {
			throw ParserException("Invalid URI string - attribute=value pairs must be separated by &");
		}
		auto entry = uri_attribute_map.find(tokens[i].value);
		if (entry == uri_attribute_map.end()) {
			string supported_options;
			for (auto &entry : uri_attribute_map) {
				if (!supported_options.empty()) {
					supported_options += ", ";
				}
				supported_options += entry.first;
			}
			throw ParserException("Invalid URI string - unsupported attribute "
			                      "\"%s\"\nSupported options: %s",
			                      tokens[i].value, supported_options);
		}
		result.emplace_back(entry->second, tokens[i + 1].value);
	}
}

vector<URIValue> ExtractURIValues(const vector<URIToken> &tokens, ErrorData &error) {
	// [scheme://][user[:[password]]@]host[:port][/schema][?attribute1=value1&attribute2=value2...
	vector<URIValue> result;

	if (tokens.empty()) {
		return result;
	}

	// If we only have one empty token with no delimiter, don't treat it as a host
	if (tokens.size() == 1 && tokens[0].value.empty() && tokens[0].delimiter == '\0') {
		return result;
	}

	// figure out how many "non-attribute" tokens we have
	idx_t attribute_start = tokens.size();
	for (idx_t i = 0; i < tokens.size(); i++) {
		if (tokens[i].delimiter == '?') {
			// found a question-mark - this is a token
			attribute_start = i + 1;
			break;
		}
	}

	// parse the main attributes in the string
	ParseMainAttributes(tokens, attribute_start, result, error);
	// parse key-value attributes
	ParseAttributes(tokens, attribute_start, result);

	return result;
}

bool TryConvertURIInternal(const string &dsn, idx_t start_pos, string &connection_string, ErrorData &error) {
	// parse tokens from the string
	auto tokens = ParseURITokens(dsn, start_pos);

	auto values = ExtractURIValues(tokens, error);
	if (error.HasError()) {
		return false;
	}

	unordered_set<string> added_params;

	for (auto &val : values) {
		// Skip duplicate parameters
		if (added_params.find(val.name) != added_params.end()) {
			continue;
		}

		added_params.insert(val.name);

		connection_string += val.name;
		connection_string += "={";
		for (auto ch : val.value) {
			connection_string += ch == '}' ? "}}" : string(1, ch);
		}
		connection_string += "};";
	}

	return true;
}

void TryConvertURI(string &dsn) {
	// Skip empty strings
	if (dsn.empty()) {
		return;
	}

	// [scheme://][user[:[password]]@]host[:port][/schema][?attribute1=value1&attribute2=value2...
	idx_t start_pos = 0;
	// Skip past the sqlserver:// scheme.
	if (StringUtil::StartsWith(dsn, "sqlserver://")) {
		start_pos = 12;
	} else {
		return;
	}

	// try to convert this as a URI
	string connection_string;
	ErrorData error;
	if (TryConvertURIInternal(dsn, start_pos, connection_string, error)) {
		// success! this is a URI
		dsn = std::move(connection_string);
		return;
	}

	// not a URI
	if (start_pos > 0) {
		// It used the SQL Server URI scheme, so surface the parse error.
		error.Throw();
	}
}

string ODBCCatalog::GetConnectionString(ClientContext &context, const string &attach_path, string secret_name) {
	// if no secret is specified we default to the unnamed odbc secret, if it
	// exists
	bool explicit_secret = !secret_name.empty();
	if (!explicit_secret) {
		// look up settings from the default unnamed odbc secret if none is
		// provided
		secret_name = "__default_sqlserver";
	}

	auto secret_entry = GetSecret(context, secret_name);
	string connection_string = attach_path;
	StringUtil::Trim(connection_string);

	// if the connection string is a URI, try and convert it
	TryConvertURI(connection_string);

	if (secret_entry) {
		// secret found - read data
		const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_entry->secret);

		// Parse the original connection string to find which parameters are already
		// Parse the original connection string to find which parameters are already set
		ODBCConnectionParameters unused_config;
		unordered_set<string> existing_params;
		std::tie(unused_config, existing_params) = ODBCUtils::ParseConnectionParameters(connection_string);

		// Build a new connection string with parameters from the secret that don't
		// already exist in the original connection string
		string new_connection_info;

		new_connection_info += AddConnectionOption(kv_secret, "uid", existing_params);
		new_connection_info += AddConnectionOption(kv_secret, "pwd", existing_params);
		new_connection_info += AddConnectionOption(kv_secret, "server", existing_params);
		new_connection_info += AddConnectionOption(kv_secret, "port", existing_params);
		new_connection_info += AddConnectionOption(kv_secret, "database", existing_params);
		new_connection_info += AddConnectionOption(kv_secret, "driver", existing_params);

		// Combine the parameters, putting secret parameters first
		if (!new_connection_info.empty()) {
			if (!connection_string.empty()) {
				// Only add a space if both parts are non-empty
				connection_string = new_connection_info + connection_string;
			} else {
				connection_string = new_connection_info;
			}
		}
	} else if (explicit_secret) {
		// secret not found and one was explicitly provided - throw an error
		throw BinderException("Secret with name \"%s\" not found", secret_name);
	}
	return connection_string;
}

void ODBCCatalog::Initialize(bool load_builtin) {
}

optional_ptr<CatalogEntry> ODBCCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

void ODBCCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

void ODBCCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	schemas.Scan(context, [&](CatalogEntry &schema) { callback(schema.Cast<ODBCSchemaEntry>()); });
}

optional_ptr<SchemaCatalogEntry> ODBCCatalog::LookupSchema(CatalogTransaction transaction,
                                                           const EntryLookupInfo &schema_lookup,
                                                           OnEntryNotFound if_not_found) {
	auto schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA) {
		if (default_schema.empty()) {
			throw InvalidInputException("Attempting to fetch the default schema - but no database was "
			                            "provided in the connection string");
		}
		schema_name = default_schema;
	}
	auto entry = schemas.GetEntry(transaction.GetContext(), schema_name);
	if (!entry && if_not_found != OnEntryNotFound::RETURN_NULL) {
		throw BinderException("Schema with name \"%s\" not found", schema_name);
	}
	return reinterpret_cast<SchemaCatalogEntry *>(entry.get());
}

bool ODBCCatalog::InMemory() {
	return false;
}

string ODBCCatalog::GetDBPath() {
	return attach_path;
}

bool ODBCCatalog::IsODBCScan(const string &name) {
	return name == "sqlserver_scan";
}

bool ODBCCatalog::IsODBCQuery(const string &name) {
	return name == "sqlserver_query";
}

void ODBCCatalog::MaterializeODBCScans(PhysicalOperator &op) {
	if (op.type == PhysicalOperatorType::TABLE_SCAN) {
		auto &table_scan = op.Cast<PhysicalTableScan>();
		if (ODBCCatalog::IsODBCScan(table_scan.function.name)) {
			auto &bind_data = table_scan.bind_data->Cast<ODBCBindData>();
			bind_data.streaming = ODBCResultStreaming::FORCE_MATERIALIZATION;
		}
		if (ODBCCatalog::IsODBCQuery(table_scan.function.name)) {
			auto &bind_data = table_scan.bind_data->Cast<ODBCQueryBindData>();
			bind_data.streaming = ODBCResultStreaming::FORCE_MATERIALIZATION;
		}
	}
	for (auto &child : op.children) {
		MaterializeODBCScans(child);
	}
}

DatabaseSize ODBCCatalog::GetDatabaseSize(ClientContext &context) {
	DatabaseSize size;
	size.free_blocks = 0;
	size.total_blocks = 0;
	size.used_blocks = 0;
	size.wal_size = 0;
	size.block_size = 0;
	size.bytes = 0;
	return size;
}

void ODBCCatalog::ClearCache() {
	schemas.ClearEntries();
}

PhysicalOperator &ODBCCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                 LogicalCreateTable &op, PhysicalOperator &plan) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

PhysicalOperator &ODBCCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                          optional_ptr<PhysicalOperator> plan) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

PhysicalOperator &ODBCCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                          PhysicalOperator &plan) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

PhysicalOperator &ODBCCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                          PhysicalOperator &plan) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

unique_ptr<LogicalOperator> ODBCCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                         TableCatalogEntry &table, unique_ptr<LogicalOperator> plan) {
	throw PermissionException("sqlserver_scanner catalogs are read-only");
}

} // namespace duckdb
