#include "open_prompt_secret.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/secret/secret.hpp"

namespace duckdb {

static void CopySecret(const std::string &key, const CreateSecretInput &input, KeyValueSecret &result) {
    auto val = input.options.find(key);
    if (val != input.options.end()) {
        result.secret_map[key] = val->second;
    }
}

static void RegisterCommonSecretParameters(CreateSecretFunction &function) {
    // Register open_prompt common parameters
    function.named_parameters["api_token"] = LogicalType::VARCHAR;
    function.named_parameters["api_url"] = LogicalType::VARCHAR;
    function.named_parameters["model_name"] = LogicalType::VARCHAR;
    function.named_parameters["api_timeout"] = LogicalType::VARCHAR;
}

static void RedactCommonKeys(KeyValueSecret &result) {
    // Redact sensitive information
    result.redact_keys.insert("api_token");
}

static unique_ptr<BaseSecret> CreateOpenPromptSecretFromConfig(ClientContext &context, CreateSecretInput &input) {
    auto scope = input.scope;
    auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);

    // Copy all relevant secrets
    CopySecret("api_token", input, *result);
    CopySecret("api_url", input, *result);
    CopySecret("model_name", input, *result);
    CopySecret("api_timeout", input, *result);

    // Redact sensitive keys
    RedactCommonKeys(*result);

    return std::move(result);
}

void CreateOpenPromptSecretFunctions::Register(ExtensionLoader &loader) {
    string type = "open_prompt";

    // Register the new type
    SecretType secret_type;
    secret_type.name = type;
    secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
    secret_type.default_provider = "config";
    loader.RegisterSecretType(secret_type);

    // Register the config secret provider
    CreateSecretFunction config_function = {type, "config", CreateOpenPromptSecretFromConfig};
    RegisterCommonSecretParameters(config_function);
    loader.RegisterFunction(config_function);
}

} // namespace duckdb
