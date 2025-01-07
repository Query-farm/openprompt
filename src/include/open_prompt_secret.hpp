#pragma once

#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/extension_util.hpp"

namespace duckdb {

struct CreateOpenPromptSecretFunctions {
public:
    static void Register(DatabaseInstance &instance);
};

} // namespace duckdb
