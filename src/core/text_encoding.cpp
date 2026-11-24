#include "core/text_encoding.hpp"
#include "core/logger.hpp"

#include <windows.h>

#include <algorithm>
#include <vector>

namespace aigiskr::core {

namespace {

bool IsValidStrictUtf8(const uint8_t* p, size_t len, bool& hasNonAscii) {
    hasNonAscii = false;
    size_t i = 0;
    while (i < len) {
        const uint8_t c = p[i];
        if (c < 0x80) {
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') return false;
            ++i;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= len) return false;
            const uint8_t c1 = p[i + 1];
            if ((c1 & 0xC0) != 0x80) return false;
            uint32_t cp = ((c & 0x1F) << 6) | (c1 & 0x3F);
            if (cp < 0x80) return false;
            hasNonAscii = true;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len) return false;
            const uint8_t c1 = p[i + 1], c2 = p[i + 2];
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
            uint32_t cp = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
            hasNonAscii = true;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= len) return false;
            const uint8_t c1 = p[i + 1], c2 = p[i + 2], c3 = p[i + 3];
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
            uint32_t cp = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return false;
            hasNonAscii = true;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

bool IsValidCp932(const uint8_t* p, size_t len) {
    size_t i = 0;
    while (i < len) {
        const uint8_t c = p[i];
        if (c < 0x80) {
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') return false;
            ++i;
        } else if (c >= 0xA1 && c <= 0xDF) {
            ++i;  // half-width katakana
        } else if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
            if (i + 1 >= len) return false;
            const uint8_t c2 = p[i + 1];
            if (!((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0x80 && c2 <= 0xFC))) return false;
            i += 2;
        } else {
            return false;
        }
    }
    return true;
}

std::string MakeHexPreview(const uint8_t* p, size_t len) {
    static const char kHex[] = "0123456789abcdef";
    const size_t n = std::min<size_t>(len, 24);
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kHex[p[i] >> 4]);
        out.push_back(kHex[p[i] & 0x0F]);
    }
    if (len > n) out += "...";
    return out;
}

void AppendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

const char* EncodingName(DetectedEncoding enc) {
    switch (enc) {
        case DetectedEncoding::Empty: return "EMPTY";
        case DetectedEncoding::Ascii: return "ASCII";
        case DetectedEncoding::Utf8: return "UTF-8";
        case DetectedEncoding::ShiftJis: return "CP932";
        case DetectedEncoding::Utf16Le: return "UTF-16LE";
        case DetectedEncoding::Utf32Le: return "UTF-32LE";
        case DetectedEncoding::BinaryOrInvalid: return "BIN";
    }
    return "?";
}

DecodedText DecodeMultibyteBytes(const uint8_t* data, size_t len) {
    DecodedText res;
    res.rawByteLen = len;
    if (!data || len == 0) {
        res.encoding = DetectedEncoding::Empty;
        return res;
    }
    res.hexPreview = MakeHexPreview(data, len);

    bool hasNonAscii = false;
    if (IsValidStrictUtf8(data, len, hasNonAscii)) {
        res.encoding = hasNonAscii ? DetectedEncoding::Utf8 : DetectedEncoding::Ascii;
        res.utf8.assign(reinterpret_cast<const char*>(data), len);
        return res;
    }

    if (IsValidCp932(data, len)) {
        int wlen = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(data), static_cast<int>(len), nullptr, 0);
        if (wlen > 0) {
            std::wstring w(static_cast<size_t>(wlen), L'\0');
            MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(data), static_cast<int>(len), w.data(), wlen);
            res.encoding = DetectedEncoding::ShiftJis;
            res.utf8 = WideToUtf8(w);
            return res;
        }
    }

    res.encoding = DetectedEncoding::BinaryOrInvalid;
    res.utf8 = "<BIN:" + res.hexPreview + ">";
    return res;
}

DecodedText DecodeUtf16String(const wchar_t* data, size_t charLen) {
    DecodedText res;
    res.rawByteLen = charLen * sizeof(wchar_t);
    res.hexPreview = MakeHexPreview(reinterpret_cast<const uint8_t*>(data), res.rawByteLen);
    if (!data || charLen == 0) {
        res.encoding = DetectedEncoding::Empty;
        return res;
    }
    res.encoding = DetectedEncoding::Utf16Le;
    res.utf8 = WideToUtf8(std::wstring_view(data, charLen));
    return res;
}

DecodedText DecodeUtf32String(const uint32_t* data, size_t charLen) {
    DecodedText res;
    res.rawByteLen = charLen * sizeof(uint32_t);
    res.hexPreview = MakeHexPreview(reinterpret_cast<const uint8_t*>(data), res.rawByteLen);
    if (!data || charLen == 0) {
        res.encoding = DetectedEncoding::Empty;
        return res;
    }
    res.encoding = DetectedEncoding::Utf32Le;
    for (size_t i = 0; i < charLen; ++i) {
        if (data[i] <= 0x10FFFF) {
            AppendUtf8Codepoint(res.utf8, data[i]);
        } else {
            res.utf8.push_back('?');
        }
    }
    return res;
}

std::string EscapeForTsv(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size() + 8);
    for (unsigned char c : utf8) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\x%02X", c);
            out += buf;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

uint64_t Fnv1a64(std::string_view s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string Utf8ToShiftJis(std::string_view utf8) {
    std::wstring w = Utf8ToWide(utf8);
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(932, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(932, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

}  // namespace aigiskr::core
