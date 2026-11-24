#include "core/mini_json.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace aigiskr::core {

namespace {

void AppendUtf8Codepoint(uint32_t cp, std::string& out) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view src) noexcept : src_(src) {
        if (src_.size() >= 3 &&
            static_cast<unsigned char>(src_[0]) == 0xEF &&
            static_cast<unsigned char>(src_[1]) == 0xBB &&
            static_cast<unsigned char>(src_[2]) == 0xBF) {
            pos_ = 3;
        }
    }

    bool Parse(JsonValue& out, std::string& err) {
        SkipWs();
        if (!ParseValue(out, err, 0)) {
            return false;
        }
        SkipWs();
        if (pos_ != src_.size()) {
            err = "trailing characters after JSON root";
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;
    std::string_view src_;
    size_t pos_ = 0;

    void SkipWs() noexcept {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool ParseHex4(uint32_t& val) noexcept {
        if (pos_ + 4 > src_.size()) {
            return false;
        }
        val = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = src_[pos_++];
            val <<= 4;
            if (c >= '0' && c <= '9') {
                val |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                val |= static_cast<uint32_t>(10 + (c - 'a'));
            } else if (c >= 'A' && c <= 'F') {
                val |= static_cast<uint32_t>(10 + (c - 'A'));
            } else {
                return false;
            }
        }
        return true;
    }

    bool ParseString(std::string& out, std::string& err) {
        if (pos_ >= src_.size() || src_[pos_] != '"') {
            err = "expected '\"'";
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (pos_ >= src_.size()) {
                    err = "unterminated escape";
                    return false;
                }
                const char e = src_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!ParseHex4(cp)) {
                            err = "invalid \\uXXXX escape";
                            return false;
                        }
                        if (cp >= 0xD800u && cp <= 0xDBFFu &&
                            pos_ + 2 <= src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t low = 0;
                            if (!ParseHex4(low) || low < 0xDC00u || low > 0xDFFFu) {
                                err = "invalid UTF-16 surrogate pair";
                                return false;
                            }
                            cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                        }
                        AppendUtf8Codepoint(cp, out);
                        break;
                    }
                    default:
                        err = "unknown JSON escape";
                        return false;
                }
            } else {
                out.push_back(c);
            }
        }
        err = "unterminated string";
        return false;
    }

    bool ParseValue(JsonValue& out, std::string& err, int depth) {
        if (depth > kMaxDepth) {
            err = "JSON nesting exceeded limit";
            return false;
        }
        SkipWs();
        if (pos_ >= src_.size()) {
            err = "unexpected end of JSON";
            return false;
        }
        const char c = src_[pos_];
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return ParseString(out.strVal, err);
        }
        if (c == '{') {
            out.type = JsonValue::Type::Object;
            ++pos_;
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            while (true) {
                SkipWs();
                std::string key;
                if (!ParseString(key, err)) {
                    return false;
                }
                SkipWs();
                if (pos_ >= src_.size() || src_[pos_] != ':') {
                    err = "expected ':' in object";
                    return false;
                }
                ++pos_;
                JsonValue val;
                if (!ParseValue(val, err, depth + 1)) {
                    return false;
                }
                out.objVal.insert_or_assign(std::move(key), std::move(val));
                SkipWs();
                if (pos_ >= src_.size()) {
                    err = "unterminated object";
                    return false;
                }
                if (src_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                if (src_[pos_] != ',') {
                    err = "expected ',' or '}' in object";
                    return false;
                }
                ++pos_;
            }
        }
        if (c == '[') {
            out.type = JsonValue::Type::Array;
            ++pos_;
            SkipWs();
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            while (true) {
                JsonValue elem;
                if (!ParseValue(elem, err, depth + 1)) {
                    return false;
                }
                out.arrVal.push_back(std::move(elem));
                SkipWs();
                if (pos_ >= src_.size()) {
                    err = "unterminated array";
                    return false;
                }
                if (src_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                if (src_[pos_] != ',') {
                    err = "expected ',' or ']' in array";
                    return false;
                }
                ++pos_;
            }
        }
        if (src_.substr(pos_, 4) == "true") {
            out.type = JsonValue::Type::Bool;
            out.boolVal = true;
            pos_ += 4;
            return true;
        }
        if (src_.substr(pos_, 5) == "false") {
            out.type = JsonValue::Type::Bool;
            out.boolVal = false;
            pos_ += 5;
            return true;
        }
        if (src_.substr(pos_, 4) == "null") {
            out.type = JsonValue::Type::Null;
            pos_ += 4;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t start = pos_;
            while (pos_ < src_.size()) {
                const char nc = src_[pos_];
                if ((nc >= '0' && nc <= '9') || nc == '-' || nc == '+' || nc == '.' || nc == 'e' || nc == 'E') {
                    ++pos_;
                } else {
                    break;
                }
            }
            const std::string numStr(src_.substr(start, pos_ - start));
            char* endPtr = nullptr;
            out.numVal = std::strtod(numStr.c_str(), &endPtr);
            if (endPtr == numStr.c_str()) {
                err = "invalid number token";
                return false;
            }
            out.type = JsonValue::Type::Number;
            return true;
        }
        err = std::string("unexpected character: ") + c;
        return false;
    }
};

}  // namespace

bool ParseJson(std::string_view input, JsonValue& out, std::string& errorMsg) {
    out = JsonValue{};
    JsonParser parser(input);
    return parser.Parse(out, errorMsg);
}

bool ParseJsonFile(const std::string& path, JsonValue& out, std::string& errorMsg) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        errorMsg = "cannot open file: " + path;
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return ParseJson(oss.str(), out, errorMsg);
}

std::string QuoteJsonString(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    out.push_back('"');
    for (unsigned char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string EscapeTsvField(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 4);
    for (char c : input) {
        if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace aigiskr::core
