#pragma once

#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

struct CreateOpenPromptSecretFunctions {
public:
    static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
