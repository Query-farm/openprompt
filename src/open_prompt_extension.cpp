#define DUCKDB_EXTENSION_MAIN

#include "open_prompt_extension.hpp"
#include "open_prompt_secret.hpp"
#include "query_farm_telemetry.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/unified_vector_format.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include "yyjson.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

// One constant for both Version() and telemetry
#define OPEN_PROMPT_VERSION "2026110701"

static constexpr const char *DEFAULT_API_URL = "http://localhost:11434/v1/chat/completions";
static constexpr const char *DEFAULT_MODEL_NAME = "qwen2.5:0.5b";
static constexpr const char *SECRET_TYPE = "open_prompt";
static constexpr const char *SESSION_STATE_KEY = "open_prompt_session";
// TRY() refuses volatile functions (it would re-run the requests), so this setting gives NULL-on-error instead
static constexpr const char *NULL_ON_ERROR_SETTING = "open_prompt_null_on_error";

// Session variables written by set_api_url / set_model_name / set_api_timeout (and readable with getvariable()).
static constexpr const char *VAR_API_URL = "openprompt_api_url";
static constexpr const char *VAR_MODEL_NAME = "openprompt_model_name";
static constexpr const char *VAR_API_TIMEOUT = "openprompt_api_timeout";
// Only read for compatibility with a SET VARIABLE done by hand; set_api_token no longer writes it.
static constexpr const char *VAR_API_TOKEN = "openprompt_api_token";

// Longest wait between retries of a rate-limited (429/503) request
static constexpr int64_t MAX_RETRY_WAIT_MS = 60000;
// How much of an unparseable error body is quoted in an error message
static constexpr idx_t MAX_ERROR_BODY = 500;

// The arguments of open_prompt(), in the order of its signature. DuckDB 2.0 lays out the arguments in signature order
// and fills in the NULL defaults of the ones a call leaves out, so a NULL means "not given".
static constexpr idx_t ARG_PROMPT = 0;
static constexpr idx_t ARG_MODEL = 1;
static constexpr idx_t ARG_JSON_SCHEMA = 2;
static constexpr idx_t ARG_SYSTEM_PROMPT = 3;
static constexpr idx_t ARG_TEMPERATURE = 4;
static constexpr idx_t ARG_MAX_TOKENS = 5;

//===--------------------------------------------------------------------===//
// Per-connection state: the token given to set_api_token()
//===--------------------------------------------------------------------===//
// Kept out of the user variables so getvariable() / duckdb_variables() cannot read it back.
class OpenPromptSessionState : public ClientContextState {
public:
	void SetApiToken(string token) {
		lock_guard<mutex> guard(lock);
		api_token = std::move(token);
	}
	string GetApiToken() {
		lock_guard<mutex> guard(lock);
		return api_token;
	}

private:
	mutex lock;
	string api_token;
};

static shared_ptr<OpenPromptSessionState> GetSessionState(ClientContext &context) {
	return context.registered_state->GetOrCreate<OpenPromptSessionState>(SESSION_STATE_KEY);
}

//===--------------------------------------------------------------------===//
// Configuration
//===--------------------------------------------------------------------===//
struct OpenPromptConfig {
	string api_url;
	string api_token;
	string model_name;
	//! 0 = use DuckDB's http_timeout
	int64_t timeout_seconds = 0;
};

static bool GetSessionVariable(ClientContext &context, const char *name, string &result) {
	Value value;
	if (ClientConfig::GetConfig(context).GetUserVariable(string(name), value) && !value.IsNull()) {
		result = value.ToString();
		return !result.empty();
	}
	return false;
}

static bool GetEnvironmentVariable(const char *name, string &result) {
	const char *value = std::getenv(name);
	if (value && value[0] != '\0') {
		result = value;
		return true;
	}
	return false;
}

//! Parses a timeout in whole seconds; `source` names where it came from for the error message.
static int64_t ParseTimeout(const string &text, const string &source) {
	auto trimmed = text;
	StringUtil::Trim(trimmed);
	int64_t seconds = 0;
	bool valid = !trimmed.empty() && trimmed.size() <= 9;
	for (auto c : trimmed) {
		if (c < '0' || c > '9') {
			valid = false;
			break;
		}
		seconds = seconds * 10 + (c - '0');
	}
	if (!valid || seconds <= 0) {
		throw InvalidInputException("open_prompt: %s must be a positive whole number of seconds, got \"%s\"", source,
		                            text);
	}
	return seconds;
}

static void CheckApiUrl(const string &url) {
	auto lower = StringUtil::Lower(url);
	if (!StringUtil::StartsWith(lower, "http://") && !StringUtil::StartsWith(lower, "https://")) {
		throw InvalidInputException("open_prompt: the API URL must start with http:// or https://, got \"%s\"", url);
	}
	for (auto c : url) {
		if (static_cast<unsigned char>(c) <= ' ') {
			throw InvalidInputException("open_prompt: the API URL must not contain whitespace or control characters");
		}
	}
}

//! Reads the open_prompt secret that best matches `path` (a secret with a SCOPE that prefixes the API URL beats an
//! unscoped one). Lookup errors are not swallowed.
static void ReadSecret(ClientContext &context, const string &path, case_insensitive_map_t<string> &values) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto match = secret_manager.LookupSecret(transaction, path, SECRET_TYPE);
	if (!match.HasMatch()) {
		return;
	}
	const auto *kv_secret = dynamic_cast<const KeyValueSecret *>(&match.GetSecret());
	if (!kv_secret) {
		throw InvalidConfigurationException("open_prompt: the open_prompt secret is not a key/value secret");
	}
	for (auto key : {"api_url", "api_token", "model_name", "api_timeout"}) {
		Value value;
		if (kv_secret->TryGetValue(Identifier(key), value) && !value.IsNull()) {
			auto text = value.ToString();
			if (!text.empty()) {
				values[key] = std::move(text);
			}
		}
	}
}

// Precedence, highest first: an open_prompt secret (scoped to the API URL, else unscoped), the set_*() session value,
// the OPEN_PROMPT_* environment variable, the built-in default. The model can also be given per call.
static OpenPromptConfig ResolveConfig(ClientContext &context) {
	OpenPromptConfig config;

	// The URL that scoped secrets are matched against
	string lookup_url;
	if (!GetSessionVariable(context, VAR_API_URL, lookup_url) && !GetEnvironmentVariable("OPEN_PROMPT_API_URL", lookup_url)) {
		lookup_url = DEFAULT_API_URL;
	}

	case_insensitive_map_t<string> secret;
	ReadSecret(context, lookup_url, secret);

	auto entry = secret.find("api_url");
	config.api_url = entry != secret.end() ? entry->second : lookup_url;

	entry = secret.find("api_token");
	if (entry != secret.end()) {
		config.api_token = entry->second;
	} else {
		config.api_token = GetSessionState(context)->GetApiToken();
		if (config.api_token.empty() && !GetSessionVariable(context, VAR_API_TOKEN, config.api_token)) {
			GetEnvironmentVariable("OPEN_PROMPT_API_TOKEN", config.api_token);
		}
	}

	entry = secret.find("model_name");
	if (entry != secret.end()) {
		config.model_name = entry->second;
	} else if (!GetSessionVariable(context, VAR_MODEL_NAME, config.model_name) &&
	           !GetEnvironmentVariable("OPEN_PROMPT_MODEL_NAME", config.model_name)) {
		config.model_name = DEFAULT_MODEL_NAME;
	}

	string timeout;
	entry = secret.find("api_timeout");
	if (entry != secret.end()) {
		config.timeout_seconds = ParseTimeout(entry->second, "the secret's api_timeout");
	} else if (GetSessionVariable(context, VAR_API_TIMEOUT, timeout)) {
		config.timeout_seconds = ParseTimeout(timeout, "the API timeout");
	} else if (GetEnvironmentVariable("OPEN_PROMPT_API_TIMEOUT", timeout)) {
		config.timeout_seconds = ParseTimeout(timeout, "OPEN_PROMPT_API_TIMEOUT");
	}

	CheckApiUrl(config.api_url);
	return config;
}

//===--------------------------------------------------------------------===//
// JSON helpers
//===--------------------------------------------------------------------===//
struct YYDoc {
	explicit YYDoc(yyjson_doc *doc_p) : doc(doc_p) {
	}
	~YYDoc() {
		if (doc) {
			yyjson_doc_free(doc);
		}
	}
	YYDoc(const YYDoc &) = delete;
	YYDoc &operator=(const YYDoc &) = delete;
	yyjson_doc *doc;
};

struct YYMutDoc {
	YYMutDoc() : doc(yyjson_mut_doc_new(nullptr)) {
		if (!doc) {
			throw std::bad_alloc();
		}
	}
	~YYMutDoc() {
		yyjson_mut_doc_free(doc);
	}
	YYMutDoc(const YYMutDoc &) = delete;
	YYMutDoc &operator=(const YYMutDoc &) = delete;
	yyjson_mut_doc *doc;
};

static yyjson_mut_val *MutString(yyjson_mut_doc *doc, const string &value) {
	return yyjson_mut_strncpy(doc, value.data(), value.size());
}

static bool IsObjectWithType(yyjson_val *root, const char *type) {
	auto type_val = yyjson_obj_get(root, "type");
	return type_val && yyjson_is_str(type_val) && strcmp(yyjson_get_str(type_val), type) == 0;
}

//! Builds the OpenAI response_format for a json_schema argument, which may be
//!  - a JSON Schema: sent as {"type":"json_schema","json_schema":{"name":"response","schema":<schema>}}
//!  - a complete response_format ({"type":"json_schema","json_schema":{...}} or {"type":"json_object"}): sent as is
static yyjson_mut_val *BuildResponseFormat(yyjson_mut_doc *doc, const string &json_schema) {
	yyjson_read_err err;
	YYDoc schema_doc(yyjson_read_opts(const_cast<char *>(json_schema.data()), json_schema.size(), 0, nullptr, &err));
	if (!schema_doc.doc) {
		throw InvalidInputException("open_prompt: json_schema is not valid JSON: %s at byte %llu", err.msg,
		                            static_cast<uint64_t>(err.pos));
	}
	auto root = yyjson_doc_get_root(schema_doc.doc);
	if (!yyjson_is_obj(root)) {
		throw InvalidInputException("open_prompt: json_schema must be a JSON object");
	}
	// yyjson_val_mut_copy copies strings into `doc`, so the parsed schema can be freed afterwards
	if (IsObjectWithType(root, "json_schema") && yyjson_is_obj(yyjson_obj_get(root, "json_schema"))) {
		return yyjson_val_mut_copy(doc, root);
	}
	if (IsObjectWithType(root, "json_object") && yyjson_obj_size(root) == 1) {
		return yyjson_val_mut_copy(doc, root);
	}
	auto response_format = yyjson_mut_obj(doc);
	yyjson_mut_obj_add_str(doc, response_format, "type", "json_schema");
	auto wrapper = yyjson_mut_obj_add_obj(doc, response_format, "json_schema");
	yyjson_mut_obj_add_str(doc, wrapper, "name", "response");
	yyjson_mut_obj_add_val(doc, wrapper, "schema", yyjson_val_mut_copy(doc, root));
	return response_format;
}

struct PromptRequest {
	string prompt;
	string model;
	string json_schema;
	string system_prompt;
	bool has_temperature = false;
	double temperature = 0;
	bool has_max_tokens = false;
	int32_t max_tokens = 0;
};

static string BuildRequestBody(const PromptRequest &request) {
	YYMutDoc doc;
	auto root = yyjson_mut_obj(doc.doc);
	yyjson_mut_doc_set_root(doc.doc, root);
	yyjson_mut_obj_add_val(doc.doc, root, "model", MutString(doc.doc, request.model));

	auto messages = yyjson_mut_obj_add_arr(doc.doc, root, "messages");
	if (!request.system_prompt.empty()) {
		auto message = yyjson_mut_arr_add_obj(doc.doc, messages);
		yyjson_mut_obj_add_str(doc.doc, message, "role", "system");
		yyjson_mut_obj_add_val(doc.doc, message, "content", MutString(doc.doc, request.system_prompt));
	}
	auto message = yyjson_mut_arr_add_obj(doc.doc, messages);
	yyjson_mut_obj_add_str(doc.doc, message, "role", "user");
	yyjson_mut_obj_add_val(doc.doc, message, "content", MutString(doc.doc, request.prompt));

	if (!request.json_schema.empty()) {
		yyjson_mut_obj_add_val(doc.doc, root, "response_format", BuildResponseFormat(doc.doc, request.json_schema));
	}
	if (request.has_temperature) {
		yyjson_mut_obj_add_real(doc.doc, root, "temperature", request.temperature);
	}
	if (request.has_max_tokens) {
		yyjson_mut_obj_add_int(doc.doc, root, "max_tokens", request.max_tokens);
	}

	yyjson_write_err err;
	size_t length = 0;
	auto json = yyjson_mut_write_opts(doc.doc, 0, nullptr, &length, &err);
	if (!json) {
		throw InvalidInputException("open_prompt: could not build the request: %s", err.msg);
	}
	string body(json, length);
	free(json);
	return body;
}

//! The most useful message an error response carries: OpenAI's {"error":{"message":..}}, Ollama's {"error":".."},
//! else the start of the body.
static string ExtractApiError(const string &body) {
	YYDoc doc(yyjson_read(body.data(), body.size(), 0));
	if (doc.doc) {
		auto root = yyjson_doc_get_root(doc.doc);
		if (yyjson_is_obj(root)) {
			auto error = yyjson_obj_get(root, "error");
			if (yyjson_is_str(error)) {
				return yyjson_get_str(error);
			}
			if (yyjson_is_obj(error)) {
				auto message = yyjson_obj_get(error, "message");
				if (yyjson_is_str(message)) {
					return yyjson_get_str(message);
				}
			}
			auto message = yyjson_obj_get(root, "message");
			if (yyjson_is_str(message)) {
				return yyjson_get_str(message);
			}
		}
	}
	if (body.empty()) {
		return "(empty response body)";
	}
	if (body.size() > MAX_ERROR_BODY) {
		return body.substr(0, MAX_ERROR_BODY) + "...";
	}
	return body;
}

static string ParseCompletion(const string &body, const string &url) {
	YYDoc doc(yyjson_read(body.data(), body.size(), 0));
	if (!doc.doc) {
		throw IOException("open_prompt: the response from \"%s\" is not valid JSON", url);
	}
	auto root = yyjson_doc_get_root(doc.doc);
	auto choices = yyjson_is_obj(root) ? yyjson_obj_get(root, "choices") : nullptr;
	if (!yyjson_is_arr(choices)) {
		auto error = yyjson_is_obj(root) ? yyjson_obj_get(root, "error") : nullptr;
		if (error) {
			throw IOException("open_prompt: the API at \"%s\" returned an error: %s", url, ExtractApiError(body));
		}
		throw IOException("open_prompt: the response from \"%s\" has no \"choices\" array", url);
	}
	auto first_choice = yyjson_arr_get_first(choices);
	auto message = yyjson_is_obj(first_choice) ? yyjson_obj_get(first_choice, "message") : nullptr;
	if (!yyjson_is_obj(message)) {
		throw IOException("open_prompt: the response from \"%s\" has no message in its first choice", url);
	}
	auto content = yyjson_obj_get(message, "content");
	if (yyjson_is_str(content)) {
		return string(yyjson_get_str(content), yyjson_get_len(content));
	}
	auto refusal = yyjson_obj_get(message, "refusal");
	if (yyjson_is_str(refusal)) {
		throw IOException("open_prompt: the model refused: %s", yyjson_get_str(refusal));
	}
	throw IOException("open_prompt: the response from \"%s\" has no text content", url);
}

//===--------------------------------------------------------------------===//
// HTTP
//===--------------------------------------------------------------------===//
static void CheckExternalAccess(ClientContext &context) {
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("open_prompt is disabled because enable_external_access is false");
	}
}

// DuckDB's built-in HTTP client cannot POST (and has no TLS); httpfs provides the full client and is autoloaded.
static HTTPUtil &GetHttpUtil(ClientContext &context) {
	auto &db = DatabaseInstance::GetDatabase(context);
	if (HTTPUtil::Get(db).GetName() != "Built-In") {
		return HTTPUtil::Get(db);
	}
	ExtensionHelper::TryAutoLoadExtension(context, "httpfs");
	auto &http_util = HTTPUtil::Get(db);
	if (http_util.GetName() == "Built-In") {
		throw MissingExtensionException("open_prompt needs the httpfs extension to send HTTP requests. Run INSTALL "
		                                "httpfs; LOAD httpfs; and try again");
	}
	return http_util;
}

//! Sends chat completion requests for one chunk, reusing the HTTP parameters and connection.
class CompletionClient {
public:
	CompletionClient(ClientContext &context_p, OpenPromptConfig config_p)
	    : context(context_p), config(std::move(config_p)), http_util(GetHttpUtil(context)) {
		params = http_util.InitializeParameters(context, config.api_url);
		if (config.timeout_seconds > 0) {
			params->timeout = static_cast<uint64_t>(config.timeout_seconds);
			params->timeout_usec = 0;
		}
		headers.Insert("Content-Type", "application/json");
		headers.Insert("Accept", "application/json");
		if (!config.api_token.empty()) {
			headers.Insert("Authorization", "Bearer " + config.api_token);
		}
	}

	string Complete(const string &body) {
		auto &url = config.api_url;
		for (idx_t attempt = 0;; attempt++) {
			PostRequestInfo post(url, headers, *params, const_data_ptr_cast(body.data()), body.size());
			// Get every response back, error statuses included, instead of a generic exception
			post.try_request = true;
			auto response = http_util.Request(post, client);
			if (context.IsInterrupted()) {
				throw InterruptException();
			}
			if (response->HasRequestError()) {
				throw IOException("open_prompt: HTTP POST to \"%s\" failed: %s", url, response->GetRequestError());
			}
			auto &response_body = post.buffer_out.empty() ? response->body : post.buffer_out;
			auto status = static_cast<int>(response->status);
			if (status >= 200 && status < 300) {
				return ParseCompletion(response_body, url);
			}
			bool throttled = response->status == HTTPStatusCode::TooManyRequests_429 ||
			                 response->status == HTTPStatusCode::ServiceUnavailable_503;
			if (throttled && attempt < params->retries) {
				WaitBeforeRetry(*response, attempt);
				client.reset();
				continue;
			}
			throw HTTPException(StringUtil::Format("open_prompt: HTTP %d (%s) from \"%s\": %s", status,
			                                       HTTPUtil::GetStatusMessage(response->status), url,
			                                       ExtractApiError(response_body)));
		}
	}

private:
	//! Honors Retry-After (seconds), else waits http_retry_wait_ms * http_retry_backoff^attempt; capped at 60s.
	void WaitBeforeRetry(const HTTPResponse &response, idx_t attempt) {
		int64_t wait_ms = -1;
		if (response.HasHeader("Retry-After")) {
			auto value = response.GetHeaderValue("Retry-After");
			char *end = nullptr;
			auto seconds = std::strtod(value.c_str(), &end);
			if (end != value.c_str() && std::isfinite(seconds) && seconds >= 0) {
				wait_ms = static_cast<int64_t>(MinValue<double>(seconds * 1000.0, double(MAX_RETRY_WAIT_MS)));
			}
		}
		if (wait_ms < 0) {
			auto backoff = std::pow(static_cast<double>(params->retry_backoff), static_cast<double>(attempt));
			auto wait = static_cast<double>(params->retry_wait_ms) * (std::isfinite(backoff) ? backoff : 1.0);
			wait_ms = static_cast<int64_t>(MinValue<double>(wait, double(MAX_RETRY_WAIT_MS)));
		}
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
		while (std::chrono::steady_clock::now() < deadline) {
			if (context.IsInterrupted()) {
				throw InterruptException();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}

	ClientContext &context;
	OpenPromptConfig config;
	HTTPUtil &http_util;
	unique_ptr<HTTPParams> params;
	unique_ptr<HTTPClient> client;
	HTTPHeaders headers;
};

//===--------------------------------------------------------------------===//
// open_prompt()
//===--------------------------------------------------------------------===//
static bool NullOnError(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting(NULL_ON_ERROR_SETTING, value) && !value.IsNull()) {
		return BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
	}
	return false;
}

static bool GetString(const UnifiedVectorFormat &format, idx_t row, string &result) {
	auto idx = format.sel->get_index(row);
	if (!format.validity.RowIsValid(idx)) {
		return false;
	}
	auto &value = format.GetData<string_t>()[idx];
	result = value.GetString();
	return true;
}

static void OpenPromptFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	const auto count = args.size();
	const auto arg_count = args.ColumnCount();

	vector<UnifiedVectorFormat> formats(arg_count);
	for (idx_t i = 0; i < arg_count; i++) {
		args.data[i].ToUnifiedFormat(formats[i]);
	}
	auto has_arg = [&](idx_t i) {
		return i < arg_count;
	};

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetDataMutable<string_t>(result);

	// Resolved on the first non-NULL prompt, so a chunk of NULLs sends nothing and needs no configuration
	unique_ptr<OpenPromptConfig> config;
	unique_ptr<CompletionClient> client;
	const bool null_on_error = NullOnError(context);

	for (idx_t row = 0; row < count; row++) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		PromptRequest request;
		if (!GetString(formats[ARG_PROMPT], row, request.prompt)) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		if (!config) {
			CheckExternalAccess(context);
			config = make_uniq<OpenPromptConfig>(ResolveConfig(context));
		}

		request.model = config->model_name;
		if (has_arg(ARG_MODEL) && GetString(formats[ARG_MODEL], row, request.model) && request.model.empty()) {
			throw InvalidInputException("open_prompt: model must not be empty");
		}
		if (has_arg(ARG_JSON_SCHEMA)) {
			GetString(formats[ARG_JSON_SCHEMA], row, request.json_schema);
		}
		if (has_arg(ARG_SYSTEM_PROMPT)) {
			GetString(formats[ARG_SYSTEM_PROMPT], row, request.system_prompt);
		}
		if (has_arg(ARG_TEMPERATURE)) {
			auto &format = formats[ARG_TEMPERATURE];
			auto idx = format.sel->get_index(row);
			if (format.validity.RowIsValid(idx)) {
				request.temperature = format.GetData<double>()[idx];
				if (!std::isfinite(request.temperature) || request.temperature < 0) {
					throw InvalidInputException("open_prompt: temperature must be a non-negative number, got %s",
					                            std::to_string(request.temperature));
				}
				request.has_temperature = true;
			}
		}
		if (has_arg(ARG_MAX_TOKENS)) {
			auto &format = formats[ARG_MAX_TOKENS];
			auto idx = format.sel->get_index(row);
			if (format.validity.RowIsValid(idx)) {
				request.max_tokens = format.GetData<int32_t>()[idx];
				if (request.max_tokens <= 0) {
					throw InvalidInputException("open_prompt: max_tokens must be positive, got %d", request.max_tokens);
				}
				request.has_max_tokens = true;
			}
		}

		// The request is validated (e.g. the JSON schema) before anything touches the network
		auto body = BuildRequestBody(request);
		if (!client) {
			client = make_uniq<CompletionClient>(context, *config);
		}
		string reply;
		try {
			reply = client->Complete(body);
		} catch (IOException &) {
			if (!null_on_error) {
				throw;
			}
			FlatVector::SetNull(result, row, true);
			continue;
		} catch (HTTPException &) {
			if (!null_on_error) {
				throw;
			}
			FlatVector::SetNull(result, row, true);
			continue;
		}
		result_data[row] = StringVector::AddString(result, reply);
	}
}

//===--------------------------------------------------------------------===//
// set_api_token() / set_api_url() / set_model_name() / set_api_timeout()
//===--------------------------------------------------------------------===//
static string RequireNonEmpty(const string_t &value, const char *what) {
	auto text = value.GetString();
	if (text.empty()) {
		throw InvalidInputException("%s cannot be empty", what);
	}
	return text;
}

static void SetApiToken(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, [&](string_t value) {
		GetSessionState(context)->SetApiToken(RequireNonEmpty(value, "API token"));
		// Never echo the token back: query results end up in logs, history and screenshots
		return StringVector::AddString(result, "API token set");
	});
}

static void SetSessionValue(DataChunk &args, ExpressionState &state, Vector &result, const char *var_name,
                            const char *what) {
	auto &context = state.GetContext();
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, [&](string_t value) {
		auto text = RequireNonEmpty(value, what);
		if (strcmp(var_name, VAR_API_URL) == 0) {
			CheckApiUrl(text);
		} else if (strcmp(var_name, VAR_API_TIMEOUT) == 0) {
			ParseTimeout(text, "the API timeout");
		}
		ClientConfig::GetConfig(context).SetUserVariable(var_name, Value(text));
		return StringVector::AddString(result, string(what) + " set to: " + text);
	});
}

static void SetApiUrl(DataChunk &args, ExpressionState &state, Vector &result) {
	SetSessionValue(args, state, result, VAR_API_URL, "API URL");
}

static void SetApiTimeout(DataChunk &args, ExpressionState &state, Vector &result) {
	SetSessionValue(args, state, result, VAR_API_TIMEOUT, "API timeout");
}

static void SetModelName(DataChunk &args, ExpressionState &state, Vector &result) {
	SetSessionValue(args, state, result, VAR_MODEL_NAME, "Model name");
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
static void RegisterSetter(ExtensionLoader &loader, const char *name, const char *param_name, scalar_function_t fun,
                           const string &description, const string &example) {
	ScalarFunction function(name, {FunctionParameter(param_name, LogicalType::VARCHAR, {},
	                                                 FunctionParameterKind::POSITIONAL_ONLY)},
	                        LogicalType::VARCHAR, fun);
	// Changes session state: never constant-fold or run it at plan time
	function.SetVolatile();
	function.SetFallible();
	CreateScalarFunctionInfo info(function);
	FunctionDescription desc;
	desc.description = description;
	desc.examples = {example};
	desc.categories = {"ai", "configuration"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(NULL_ON_ERROR_SETTING,
	                          "Make open_prompt return NULL for a row whose request fails (connection error, HTTP error "
	                          "status, unusable response) instead of raising an error. Invalid arguments and "
	                          "enable_external_access = false still raise",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));

	// open_prompt(prompt, model, json_schema, system_prompt, temperature, max_tokens): every argument but the prompt is
	// optional and can be passed by position or by name (e.g. json_schema := '...'), a NULL default meaning "not given".
	ScalarFunction open_prompt_fun("open_prompt", {}, LogicalType::VARCHAR, OpenPromptFunction);
	open_prompt_fun.GetSignature()
	    .AddParameter("prompt", LogicalType::VARCHAR)
	    .AddParameter("model", LogicalType::VARCHAR, Value(LogicalType::VARCHAR))
	    .AddParameter("json_schema", LogicalType::VARCHAR, Value(LogicalType::VARCHAR))
	    .AddParameter("system_prompt", LogicalType::VARCHAR, Value(LogicalType::VARCHAR))
	    .AddParameter("temperature", LogicalType::DOUBLE, Value(LogicalType::DOUBLE))
	    .AddParameter("max_tokens", LogicalType::INTEGER, Value(LogicalType::INTEGER));
	// The NULL defaults must reach the function; a NULL prompt gives NULL
	open_prompt_fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	open_prompt_fun.SetFallible();
	// Every call is a network request with a non-deterministic answer: never constant-fold it (EXPLAIN must not
	// call the API) or share results between rows
	open_prompt_fun.SetVolatile();

	CreateScalarFunctionInfo open_prompt_info(open_prompt_fun);
	FunctionDescription desc;
	desc.description =
	    "Sends a prompt to an OpenAI-compatible chat completions API and returns the reply text. The model, a JSON "
	    "Schema for structured output, a system prompt, the sampling temperature and max_tokens are optional and can "
	    "be passed by position or by name. Configuration comes from an open_prompt secret (CREATE SECRET (TYPE "
	    "open_prompt, api_token ..., api_url ..., model_name ...)), else set_api_url()/set_model_name()/"
	    "set_api_timeout(), else OPEN_PROMPT_* environment variables. HTTP goes through httpfs, so http_proxy, "
	    "http_timeout and CA settings apply; 429/503 responses are retried with backoff. Failed requests raise "
	    "errors (SET open_prompt_null_on_error = true to get NULL instead). Refuses when enable_external_access is "
	    "false.";
	desc.examples = {"open_prompt('What is DuckDB?')", "open_prompt('Explain SQL', 'gpt-4o-mini')",
	                 "open_prompt('Extract the name: Alice is 30', json_schema := '{\"type\":\"object\","
	                 "\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}')",
	                 "open_prompt('Hello', system_prompt := 'You are terse', temperature := 0.2, max_tokens := 50)"};
	desc.categories = {"ai"};
	open_prompt_info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(open_prompt_info));

	CreateOpenPromptSecretFunctions::Register(loader);

	RegisterSetter(loader, "set_api_token", "token", SetApiToken,
	               "Deprecated: prefer CREATE SECRET (TYPE open_prompt, api_token '...'), which takes precedence. Sets "
	               "the bearer token for this connection only. The token is kept out of getvariable() and is never "
	               "echoed back; returns 'API token set'",
	               "set_api_token('sk-...')");
	RegisterSetter(loader, "set_api_url", "url", SetApiUrl,
	               "Sets the chat completions URL for this connection (an open_prompt secret's api_url takes "
	               "precedence). Must start with http:// or https://",
	               "set_api_url('https://api.openai.com/v1/chat/completions')");
	RegisterSetter(loader, "set_model_name", "model", SetModelName,
	               "Sets the default model for this connection (an open_prompt secret's model_name takes precedence; "
	               "the model argument of open_prompt() overrides both)",
	               "set_model_name('gpt-4o-mini')");
	RegisterSetter(loader, "set_api_timeout", "timeout_seconds", SetApiTimeout,
	               "Sets the request timeout in whole seconds for this connection (an open_prompt secret's api_timeout "
	               "takes precedence; without either, DuckDB's http_timeout applies)",
	               "set_api_timeout('30')");

	QueryFarmSendTelemetry(loader, "open_prompt", OPEN_PROMPT_VERSION);
}

void OpenPromptExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string OpenPromptExtension::Name() {
	return "open_prompt";
}

std::string OpenPromptExtension::Version() const {
	return OPEN_PROMPT_VERSION;
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(open_prompt, loader) {
	duckdb::LoadInternal(loader);
}
}
