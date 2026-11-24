#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aigiskr::core {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<JsonValue> arrVal;
    std::unordered_map<std::string, JsonValue> objVal;

    const JsonValue* Find(std::string_view key) const noexcept {
        if (type != Type::Object) {
            return nullptr;
        }
        // Lookup with std::string until C++20 heterogeneous unordered_map in MSVC
        const auto it = objVal.find(std::string(key));
        return (it != objVal.end()) ? &it->second : nullptr;
    }

    std::string_view AsString(std::string_view fallback = {}) const noexcept {
        return (type == Type::String) ? std::string_view(strVal) : fallback;
    }
};

// Parse strict UTF-8 JSON document (with optional UTF-8 BOM skipped).
bool ParseJson(std::string_view input, JsonValue& out, std::string& errorMsg);

// Read file bytes from disk (UTF-8) and parse JSON.
bool ParseJsonFile(const std::string& path, JsonValue& out, std::string& errorMsg);

// Escape UTF-8 text as a valid JSON string literal (with surrounding quotes).
std::string QuoteJsonString(std::string_view input);

// Escape control chars (\n, \r, \t) for compact single-line TSV output.
std::string EscapeTsvField(std::string_view input);

}  // namespace aigiskr::core
