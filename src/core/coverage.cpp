#include "core/coverage.hpp"

#include "core/mini_json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aigiskr::core {

namespace {

bool DecodeUtf8Next(std::string_view s, size_t& pos, uint32_t& cp) noexcept {
    if (pos >= s.size()) {
        return false;
    }
    const unsigned char b0 = static_cast<unsigned char>(s[pos]);
    if (b0 < 0x80u) {
        cp = b0;
        ++pos;
        return true;
    }
    if ((b0 & 0xE0u) == 0xC0u && pos + 1 < s.size()) {
        cp = ((b0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[pos + 1]) & 0x3Fu);
        pos += 2;
        return true;
    }
    if ((b0 & 0xF0u) == 0xE0u && pos + 2 < s.size()) {
        cp = ((b0 & 0x0Fu) << 12) |
             ((static_cast<unsigned char>(s[pos + 1]) & 0x3Fu) << 6) |
             (static_cast<unsigned char>(s[pos + 2]) & 0x3Fu);
        pos += 3;
        return true;
    }
    if ((b0 & 0xF8u) == 0xF0u && pos + 3 < s.size()) {
        cp = ((b0 & 0x07u) << 18) |
             ((static_cast<unsigned char>(s[pos + 1]) & 0x3Fu) << 12) |
             ((static_cast<unsigned char>(s[pos + 2]) & 0x3Fu) << 6) |
             (static_cast<unsigned char>(s[pos + 3]) & 0x3Fu);
        pos += 4;
        return true;
    }
    cp = b0;
    ++pos;
    return true;
}

bool IsJapaneseCodepoint(uint32_t cp) noexcept {
    if (cp >= 0x3040u && cp <= 0x309Fu) return true;  // Hiragana
    if (cp >= 0x30A0u && cp <= 0x30FFu) return true;  // Katakana
    if (cp >= 0x31F0u && cp <= 0x31FFu) return true;  // Katakana Phonetic Extensions
    if (cp >= 0x4E00u && cp <= 0x9FFFu) return true;  // CJK Unified Ideographs
    if (cp >= 0x3400u && cp <= 0x4DBFu) return true;  // CJK Extension A
    if (cp >= 0xFF65u && cp <= 0xFF9Fu) return true;  // Halfwidth Katakana
    // Note: common Japanese corner/lenticular brackets often carry untranslated UI terms
    if (cp == 0x300Cu || cp == 0x300Du || cp == 0x300Eu || cp == 0x300Fu ||
        cp == 0x3010u || cp == 0x3011u || cp == 0x3001u || cp == 0x3002u) {
        return true;
    }
    return false;
}

bool IsHangulCodepoint(uint32_t cp) noexcept {
    return (cp >= 0xAC00u && cp <= 0xD7A3u) ||
           (cp >= 0x1100u && cp <= 0x11FFu) ||
           (cp >= 0x3130u && cp <= 0x318Fu);
}

bool IsHexChar(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string MakeTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
    localtime_s(&tmBuf, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                  tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
    return std::string(buf);
}

double SafePct(uint64_t num, uint64_t den) noexcept {
    return (den == 0) ? 0.0 : (100.0 * static_cast<double>(num) / static_cast<double>(den));
}

std::string NormalizeCrLf(std::string_view in) {
    if (in.find('\r') == std::string_view::npos) {
        return std::string(in);
    }
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < in.size() && in[i + 1] == '\n') {
                ++i;
            }
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

bool ParseHexRva(std::string_view s, uint32_t& out) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    if (s.empty()) return false;
    std::string buf(s);
    char* endp = nullptr;
    const unsigned long val = std::strtoul(buf.c_str(), &endp, 0);
    if (!endp || *endp != '\0' || val == 0) {
        return false;
    }
    out = static_cast<uint32_t>(val);
    return true;
}

}  // namespace

IgnoredSubbucket ClassifyBuiltinIgnoredReason(std::string_view utf8) noexcept {
    bool hasNonWs = false;
    bool hasHangul = false;
    bool hasKana = false;
    bool hasDigit = false;
    bool hasAsciiAlpha = false;
    bool hasNonNumericGraphic = false;

    for (size_t i = 0; i < utf8.size();) {
        // Skip AquaLead inline color tags (%c[rrggbb]) when distinguishing numeric vs ASCII labels.
        if (i + 3 < utf8.size() && utf8[i] == '%' && utf8[i + 1] == 'c' && utf8[i + 2] == '[') {
            const size_t close = utf8.find(']', i + 3);
            if (close != std::string_view::npos) {
                i = close + 1;
                hasNonWs = true;
                continue;
            }
        }
        uint32_t cp = 0;
        if (!DecodeUtf8Next(utf8, i, cp)) break;
        if (cp != ' ' && cp != '\t' && cp != '\r' && cp != '\n') {
            hasNonWs = true;
        }
        if ((cp >= 0x3040u && cp <= 0x30FFu) || (cp >= 0xFF65u && cp <= 0xFF9Fu)) {
            hasKana = true;
        }
        if (IsHangulCodepoint(cp)) {
            hasHangul = true;
        }
        if (cp >= '0' && cp <= '9') {
            hasDigit = true;
        } else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
            hasAsciiAlpha = true;
        } else if (cp != ' ' && cp != '\t' && cp != '\r' && cp != '\n' &&
                   cp != '/' && cp != ':' && cp != '.' && cp != ',' && cp != '%' &&
                   cp != '+' && cp != '-' && cp != '~' && cp != '(' && cp != ')') {
            hasNonNumericGraphic = true;
        }
    }

    if (!hasNonWs) {
        return IgnoredSubbucket::NonVisible;
    }
    if (hasHangul && !hasKana) {
        return IgnoredSubbucket::AlreadyKorean;
    }
    if (hasDigit && !hasAsciiAlpha && !hasNonNumericGraphic) {
        return IgnoredSubbucket::Numeric;
    }
    if (hasAsciiAlpha) {
        return IgnoredSubbucket::AsciiCounter;
    }
    return IgnoredSubbucket::NonVisible;
}

bool ShouldIgnoreForCoverage(std::string_view utf8) {
    bool hasNonWs = false;
    bool hasJapanese = false;
    bool hasHangul = false;
    bool hasKana = false;

    size_t pos = 0;
    uint32_t cp = 0;
    while (DecodeUtf8Next(utf8, pos, cp)) {
        if (cp != ' ' && cp != '\t' && cp != '\r' && cp != '\n') {
            hasNonWs = true;
        }
        if ((cp >= 0x3040u && cp <= 0x30FFu) || (cp >= 0xFF65u && cp <= 0xFF9Fu)) {
            hasKana = true;
        }
        if (IsJapaneseCodepoint(cp)) {
            hasJapanese = true;
        } else if (IsHangulCodepoint(cp)) {
            hasHangul = true;
        }
    }

    if (!hasNonWs) {
        return true;
    }
    if (hasHangul && !hasKana) {
        return true;  // Already localized Hangul string (or mixed Latin+Hangul)
    }
    return !hasJapanese;
}

std::string RedactSensitiveTokens(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        // Check sensitive query-like keys (token=, sid=, session=, uid=)
        bool matchedKey = false;
        for (std::string_view key : {"token=", "sid=", "session=", "uid=", "auth="}) {
            if (utf8.size() - i >= key.size()) {
                bool eq = true;
                for (size_t k = 0; k < key.size(); ++k) {
                    if (std::tolower(static_cast<unsigned char>(utf8[i + k])) != key[k]) {
                        eq = false;
                        break;
                    }
                }
                if (eq) {
                    out.append(key);
                    out.append("<REDACTED>");
                    i += key.size();
                    while (i < utf8.size() && utf8[i] != '&' && utf8[i] != ' ' &&
                           utf8[i] != '\n' && utf8[i] != '\r') {
                        ++i;
                    }
                    matchedKey = true;
                    break;
                }
            }
        }
        if (matchedKey) {
            continue;
        }

        // Collapse long >=16-char hexadecimal identifiers
        if (IsHexChar(utf8[i])) {
            size_t j = i;
            while (j < utf8.size() && IsHexChar(utf8[j])) {
                ++j;
            }
            if (j - i >= 16) {
                out.append("<HEX>");
                i = j;
                continue;
            }
        }
        out.push_back(utf8[i++]);
    }
    return out;
}

std::string ComputeShapeKey(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        if (utf8[i] >= '0' && utf8[i] <= '9') {
            while (i < utf8.size() && utf8[i] >= '0' && utf8[i] <= '9') {
                ++i;
            }
            out += "{n}";
        } else {
            out.push_back(utf8[i++]);
        }
    }
    return out;
}

const char* CoverageBucketName(CoverageBucket bucket) noexcept {
    switch (bucket) {
        case CoverageBucket::Exact: return "EXACT";
        case CoverageBucket::Context: return "CONTEXT";
        case CoverageBucket::Template: return "TEMPLATE";
        case CoverageBucket::Regex: return "REGEX";
        case CoverageBucket::Unmatched: return "UNMATCHED";
        case CoverageBucket::Ignored: return "IGNORED";
        case CoverageBucket::TagRejected: return "TAG_REJECTED";
    }
    return "UNKNOWN";
}

const char* IgnoredSubbucketName(IgnoredSubbucket sub) noexcept {
    switch (sub) {
        case IgnoredSubbucket::Numeric: return "IGNORED_NUMERIC";
        case IgnoredSubbucket::AsciiCounter: return "IGNORED_ASCII_COUNTER";
        case IgnoredSubbucket::AlreadyKorean: return "IGNORED_ALREADY_KOREAN";
        case IgnoredSubbucket::InternalPlaceholder: return "IGNORED_INTERNAL_PLACEHOLDER";
        case IgnoredSubbucket::NonVisible: return "IGNORED_NON_VISIBLE";
    }
    return "IGNORED_UNKNOWN";
}

const char* UnmatchedSubbucketName(UnmatchedSubbucket sub) noexcept {
    switch (sub) {
        case UnmatchedSubbucket::StaticVisible: return "UNMATCHED_STATIC_VISIBLE";
        case UnmatchedSubbucket::ServerDynamic: return "UNMATCHED_SERVER_DYNAMIC";
        case UnmatchedSubbucket::AssetText: return "UNMATCHED_ASSET_TEXT";
    }
    return "UNMATCHED_UNKNOWN";
}

void CoverageTracker::Configure(bool enabled, std::string outputDir) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    if (!outputDir.empty()) {
        outputDir_ = std::move(outputDir);
    }
    if (sessionStamp_.empty()) {
        sessionStamp_ = MakeTimestamp();
    }
}

bool CoverageTracker::LoadClassificationRulesFromJsonFile(const std::wstring& path, std::string* errorOut) {
    if (path.empty()) {
        if (errorOut) *errorOut = "empty classification rules path";
        return false;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        if (errorOut) *errorOut = "cannot open classification rules file";
        return false;
    }
    std::string raw;
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        raw.append(buf, n);
    }
    std::fclose(f);
    return LoadClassificationRulesFromJsonBytes(raw, errorOut);
}

bool CoverageTracker::LoadClassificationRulesFromJsonBytes(std::string_view jsonUtf8, std::string* errorOut) {
    JsonValue root;
    std::string parseErr;
    if (!ParseJson(jsonUtf8, root, parseErr)) {
        if (errorOut) *errorOut = parseErr;
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        if (errorOut) *errorOut = "classification rules root must be object";
        return false;
    }

    std::unordered_map<std::string, std::string> loadedPlaceholders;
    if (const JsonValue* arr = root.Find("placeholders")) {
        if (arr->type != JsonValue::Type::Array) {
            if (errorOut) *errorOut = "'placeholders' must be an array";
            return false;
        }
        for (const JsonValue& item : arr->arrVal) {
            if (item.type == JsonValue::Type::String) {
                std::string k = NormalizeCrLf(item.strVal);
                if (!k.empty()) {
                    loadedPlaceholders.insert_or_assign(std::move(k), "internal_placeholder");
                }
            } else if (item.type == JsonValue::Type::Object) {
                const JsonValue* tv = item.Find("text");
                if (tv && tv->type == JsonValue::Type::String && !tv->strVal.empty()) {
                    const JsonValue* rv = item.Find("reason");
                    std::string reason = (rv && rv->type == JsonValue::Type::String && !rv->strVal.empty())
                                             ? rv->strVal
                                             : std::string("internal_placeholder");
                    loadedPlaceholders.insert_or_assign(NormalizeCrLf(tv->strVal), std::move(reason));
                }
            }
        }
    }

    std::unordered_map<uint32_t, bool> loadedServerRvas{{0x283b73u, true}};
    std::unordered_map<uint32_t, bool> loadedAssetRvas{{0x6e399du, true}, {0x63d61du, true}};
    if (const JsonValue* roles = root.Find("caller_rva_roles")) {
        if (roles->type == JsonValue::Type::Object) {
            if (const JsonValue* sd = roles->Find("server_dynamic")) {
                if (sd->type == JsonValue::Type::Array) {
                    loadedServerRvas.clear();
                    for (const auto& v : sd->arrVal) {
                        uint32_t rva = 0;
                        if (v.type == JsonValue::Type::String && ParseHexRva(v.strVal, rva)) {
                            loadedServerRvas.emplace(rva, true);
                        }
                    }
                }
            }
            if (const JsonValue* at = roles->Find("asset_text")) {
                if (at->type == JsonValue::Type::Array) {
                    loadedAssetRvas.clear();
                    for (const auto& v : at->arrVal) {
                        uint32_t rva = 0;
                        if (v.type == JsonValue::Type::String && ParseHexRva(v.strVal, rva)) {
                            loadedAssetRvas.emplace(rva, true);
                        }
                    }
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    placeholderMap_ = std::move(loadedPlaceholders);
    serverDynamicRvas_ = std::move(loadedServerRvas);
    assetTextRvas_ = std::move(loadedAssetRvas);
    return true;
}

size_t CoverageTracker::PlaceholderRuleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return placeholderMap_.size();
}

IgnoredSubbucket CoverageTracker::ClassifyIgnoredUnlocked(std::string_view normalizedSource) const {
    std::string tmp;
    std::string_view key = normalizedSource;
    if (normalizedSource.find('\r') != std::string_view::npos) {
        tmp = NormalizeCrLf(normalizedSource);
        key = tmp;
    }
    if (!placeholderMap_.empty() && placeholderMap_.find(std::string(key)) != placeholderMap_.end()) {
        return IgnoredSubbucket::InternalPlaceholder;
    }
    return ClassifyBuiltinIgnoredReason(key);
}

UnmatchedSubbucket CoverageTracker::ClassifyUnmatchedUnlocked(uint32_t callerRva) const {
    if (callerRva != 0) {
        if (serverDynamicRvas_.find(callerRva) != serverDynamicRvas_.end()) {
            return UnmatchedSubbucket::ServerDynamic;
        }
        if (assetTextRvas_.find(callerRva) != assetTextRvas_.end()) {
            return UnmatchedSubbucket::AssetText;
        }
    }
    return UnmatchedSubbucket::StaticVisible;
}

bool CoverageTracker::IsIgnoredText(std::string_view normalizedSource) const {
    if (ShouldIgnoreForCoverage(normalizedSource)) {
        return true;
    }
    std::string tmp;
    std::string_view key = normalizedSource;
    if (normalizedSource.find('\r') != std::string_view::npos) {
        tmp = NormalizeCrLf(normalizedSource);
        key = tmp;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return placeholderMap_.find(std::string(key)) != placeholderMap_.end();
}

IgnoredSubbucket CoverageTracker::ClassifyIgnored(std::string_view normalizedSource) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ClassifyIgnoredUnlocked(normalizedSource);
}

UnmatchedSubbucket CoverageTracker::ClassifyUnmatched(uint32_t callerRva) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ClassifyUnmatchedUnlocked(callerRva);
}

bool CoverageTracker::IsServerDynamicRva(uint32_t callerRva) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return callerRva != 0 && serverDynamicRvas_.find(callerRva) != serverDynamicRvas_.end();
}

void CoverageTracker::SetCurrentScreen(std::string_view screenName) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentScreen_ = screenName.empty() ? std::string("Unknown") : std::string(screenName);
}

std::string CoverageTracker::GetCurrentScreen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentScreen_;
}

void CoverageTracker::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    sessionStamp_ = MakeTimestamp();
    exactCalls_ = contextCalls_ = templateCalls_ = regexCalls_ = 0;
    unmatchedCalls_ = ignoredCalls_ = tagRejectedCalls_ = mapCapDropped_ = 0;
    ignoredNumericCalls_ = ignoredAsciiCounterCalls_ = ignoredAlreadyKoreanCalls_ = 0;
    ignoredInternalPlaceholderCalls_ = ignoredNonVisibleCalls_ = 0;
    unmatchedStaticVisibleCalls_ = unmatchedServerDynamicCalls_ = unmatchedAssetTextCalls_ = 0;
    noticeTranslatedCalls_ = 0;
    exactMap_.clear();
    contextMap_.clear();
    templateMap_.clear();
    regexMap_.clear();
    unmatchedMap_.clear();
    ignoredMap_.clear();
    tagRejectedMap_.clear();
    noticeTranslatedKeys_.clear();
    coreStaticTranslatedKeys_.clear();
    perScreen_.clear();
}

std::unordered_map<std::string, CoverageEntryStats>& CoverageTracker::SelectMap(CoverageBucket b) {
    switch (b) {
        case CoverageBucket::Exact: return exactMap_;
        case CoverageBucket::Context: return contextMap_;
        case CoverageBucket::Template: return templateMap_;
        case CoverageBucket::Regex: return regexMap_;
        case CoverageBucket::Unmatched: return unmatchedMap_;
        case CoverageBucket::Ignored: return ignoredMap_;
        case CoverageBucket::TagRejected: return tagRejectedMap_;
    }
    return unmatchedMap_;
}

void CoverageTracker::AddEntryStats(std::unordered_map<std::string, CoverageEntryStats>& mp,
                                    const std::string& key,
                                    std::string_view ruleOrReason,
                                    uint32_t callerRva,
                                    const std::string& screen,
                                    uint8_t subbucket,
                                    uint64_t& dropCounter) {
    auto it = mp.find(key);
    if (it == mp.end()) {
        if (mp.size() >= kMaxUniquePerMap) {
            ++dropCounter;
            return;
        }
        CoverageEntryStats st;
        st.calls = 1;
        st.ruleOrReason = std::string(ruleOrReason);
        st.shapeKey = ComputeShapeKey(key);
        st.firstCallerRva = callerRva;
        st.subbucket = subbucket;
        if (callerRva != 0) {
            st.callerRvas.push_back(callerRva);
        }
        if (!screen.empty()) {
            st.screens.push_back(screen);
        }
        mp.emplace(key, std::move(st));
        return;
    }

    CoverageEntryStats& st = it->second;
    ++st.calls;
    if (st.ruleOrReason.empty() && !ruleOrReason.empty()) {
        st.ruleOrReason = std::string(ruleOrReason);
    }
    if (subbucket != st.subbucket) {
        // Keep most specific subbucket when one key appears at multiple call sites.
        if (st.subbucket == 0 && subbucket != 0) {
            st.subbucket = subbucket;
        } else if (subbucket == static_cast<uint8_t>(UnmatchedSubbucket::ServerDynamic)) {
            st.subbucket = subbucket;
        }
    }
    if (callerRva != 0 && st.callerRvas.size() < 4 &&
        std::find(st.callerRvas.begin(), st.callerRvas.end(), callerRva) == st.callerRvas.end()) {
        st.callerRvas.push_back(callerRva);
    }
    if (!screen.empty() && st.screens.size() < 8 &&
        std::find(st.screens.begin(), st.screens.end(), screen) == st.screens.end()) {
        st.screens.push_back(screen);
    }
}

void CoverageTracker::Record(std::string_view normalizedSource,
                             CoverageBucket bucket,
                             std::string_view ruleOrReason,
                             uint32_t callerRva,
                             std::string_view explicitScreen) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) {
        return;
    }
    if (sessionStamp_.empty()) {
        sessionStamp_ = MakeTimestamp();
    }

    const std::string screen = explicitScreen.empty() ? currentScreen_ : std::string(explicitScreen);
    ScreenCounters& sc = perScreen_[screen];
    const std::string key = RedactSensitiveTokens(normalizedSource);
    uint8_t subVal = 0;
    std::string_view effRuleOrReason = ruleOrReason;

    switch (bucket) {
        case CoverageBucket::Exact: ++exactCalls_; ++sc.translated; break;
        case CoverageBucket::Context: ++contextCalls_; ++sc.translated; break;
        case CoverageBucket::Template: ++templateCalls_; ++sc.translated; break;
        case CoverageBucket::Regex: ++regexCalls_; ++sc.translated; break;
        case CoverageBucket::Unmatched: {
            ++unmatchedCalls_;
            ++sc.unmatched;
            const auto uSub = ClassifyUnmatchedUnlocked(callerRva);
            subVal = static_cast<uint8_t>(uSub);
            if (effRuleOrReason.empty()) {
                effRuleOrReason = UnmatchedSubbucketName(uSub);
            }
            switch (uSub) {
                case UnmatchedSubbucket::StaticVisible: ++unmatchedStaticVisibleCalls_; break;
                case UnmatchedSubbucket::ServerDynamic: ++unmatchedServerDynamicCalls_; break;
                case UnmatchedSubbucket::AssetText: ++unmatchedAssetTextCalls_; break;
            }
            break;
        }
        case CoverageBucket::Ignored: {
            ++ignoredCalls_;
            ++sc.ignored;
            const auto iSub = ClassifyIgnoredUnlocked(normalizedSource);
            subVal = static_cast<uint8_t>(iSub);
            if (effRuleOrReason.empty()) {
                if (iSub == IgnoredSubbucket::InternalPlaceholder) {
                    const auto pit = placeholderMap_.find(std::string(normalizedSource));
                    effRuleOrReason = (pit != placeholderMap_.end())
                                          ? std::string_view(pit->second)
                                          : std::string_view(IgnoredSubbucketName(iSub));
                } else {
                    effRuleOrReason = IgnoredSubbucketName(iSub);
                }
            }
            switch (iSub) {
                case IgnoredSubbucket::Numeric: ++ignoredNumericCalls_; break;
                case IgnoredSubbucket::AsciiCounter: ++ignoredAsciiCounterCalls_; break;
                case IgnoredSubbucket::AlreadyKorean: ++ignoredAlreadyKoreanCalls_; break;
                case IgnoredSubbucket::InternalPlaceholder: ++ignoredInternalPlaceholderCalls_; break;
                case IgnoredSubbucket::NonVisible: ++ignoredNonVisibleCalls_; break;
            }
            break;
        }
        case CoverageBucket::TagRejected: ++tagRejectedCalls_; ++sc.tagRejected; break;
    }

    if (bucket == CoverageBucket::Exact || bucket == CoverageBucket::Context ||
        bucket == CoverageBucket::Template || bucket == CoverageBucket::Regex) {
        if (callerRva != 0 && serverDynamicRvas_.find(callerRva) != serverDynamicRvas_.end()) {
            ++noticeTranslatedCalls_;
            if (noticeTranslatedKeys_.size() < kMaxUniquePerMap) {
                noticeTranslatedKeys_.emplace(key, 1);
            }
        } else {
            if (coreStaticTranslatedKeys_.size() < kMaxUniquePerMap) {
                coreStaticTranslatedKeys_.emplace(key, 1);
            }
        }
    }

    AddEntryStats(SelectMap(bucket), key, effRuleOrReason, callerRva, screen, subVal, mapCapDropped_);
}

CoverageSummary CoverageTracker::BuildSummaryUnlocked() const {
    CoverageSummary s;
    s.exactCalls = exactCalls_;
    s.contextCalls = contextCalls_;
    s.templateCalls = templateCalls_;
    s.regexCalls = regexCalls_;
    s.unmatchedCalls = unmatchedCalls_;
    s.ignoredCalls = ignoredCalls_;
    s.tagRejectedCalls = tagRejectedCalls_;
    s.exactUnique = exactMap_.size();
    s.contextUnique = contextMap_.size();
    s.templateUnique = templateMap_.size();
    s.regexUnique = regexMap_.size();
    s.unmatchedUnique = unmatchedMap_.size();
    s.ignoredUnique = ignoredMap_.size();
    s.tagRejectedUnique = tagRejectedMap_.size();

    s.ignoredNumericCalls = ignoredNumericCalls_;
    s.ignoredAsciiCounterCalls = ignoredAsciiCounterCalls_;
    s.ignoredAlreadyKoreanCalls = ignoredAlreadyKoreanCalls_;
    s.ignoredInternalPlaceholderCalls = ignoredInternalPlaceholderCalls_;
    s.ignoredNonVisibleCalls = ignoredNonVisibleCalls_;
    for (const auto& [_, st] : ignoredMap_) {
        switch (static_cast<IgnoredSubbucket>(st.subbucket)) {
            case IgnoredSubbucket::Numeric: ++s.ignoredNumericUnique; break;
            case IgnoredSubbucket::AsciiCounter: ++s.ignoredAsciiCounterUnique; break;
            case IgnoredSubbucket::AlreadyKorean: ++s.ignoredAlreadyKoreanUnique; break;
            case IgnoredSubbucket::InternalPlaceholder: ++s.ignoredInternalPlaceholderUnique; break;
            case IgnoredSubbucket::NonVisible: ++s.ignoredNonVisibleUnique; break;
        }
    }

    s.unmatchedStaticVisibleCalls = unmatchedStaticVisibleCalls_;
    s.unmatchedServerDynamicCalls = unmatchedServerDynamicCalls_;
    s.unmatchedAssetTextCalls = unmatchedAssetTextCalls_;
    for (const auto& [_, st] : unmatchedMap_) {
        switch (static_cast<UnmatchedSubbucket>(st.subbucket)) {
            case UnmatchedSubbucket::StaticVisible: ++s.unmatchedStaticVisibleUnique; break;
            case UnmatchedSubbucket::ServerDynamic: ++s.unmatchedServerDynamicUnique; break;
            case UnmatchedSubbucket::AssetText: ++s.unmatchedAssetTextUnique; break;
        }
    }

    s.noticeTranslatedCalls = noticeTranslatedCalls_;
    s.noticeTranslatedUnique = noticeTranslatedKeys_.size();
    s.coreStaticTranslatedUnique = coreStaticTranslatedKeys_.size();
    return s;
}

CoverageSummary CoverageTracker::SnapshotSummary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return BuildSummaryUnlocked();
}

bool CoverageTracker::FlushToDisk(std::string* outSessionJsonPath,
                                  std::string* outUnmatchedTsvPath) {
    // Snapshot under lock so file I/O does not block render threads unnecessarily
    std::string outDir;
    std::string stamp;
    CoverageSummary s;
    uint64_t capDropped = 0;
    std::vector<std::pair<std::string, CoverageEntryStats>> unmatchedRows;
    std::vector<std::pair<std::string, CoverageEntryStats>> tagRejectRows;
    std::vector<std::pair<std::string, CoverageEntryStats>> templateHitRows;
    std::vector<std::pair<std::string, CoverageEntryStats>> placeholderRows;
    std::unordered_map<std::string, ScreenCounters> screens;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) {
            return false;
        }
        outDir = outputDir_;
        stamp = sessionStamp_.empty() ? MakeTimestamp() : sessionStamp_;
        s = BuildSummaryUnlocked();
        capDropped = mapCapDropped_;
        screens = perScreen_;

        unmatchedRows.assign(unmatchedMap_.begin(), unmatchedMap_.end());
        tagRejectRows.assign(tagRejectedMap_.begin(), tagRejectedMap_.end());
        templateHitRows.assign(templateMap_.begin(), templateMap_.end());
        for (const auto& kv : regexMap_) {
            templateHitRows.push_back(kv);
        }
        for (const auto& kv : ignoredMap_) {
            if (static_cast<IgnoredSubbucket>(kv.second.subbucket) == IgnoredSubbucket::InternalPlaceholder) {
                placeholderRows.push_back(kv);
            }
        }
    }

    auto byCallsDesc = [](const auto& a, const auto& b) {
        if (a.second.calls != b.second.calls) return a.second.calls > b.second.calls;
        return a.first < b.first;
    };
    std::sort(unmatchedRows.begin(), unmatchedRows.end(), byCallsDesc);
    std::sort(tagRejectRows.begin(), tagRejectRows.end(), byCallsDesc);
    std::sort(templateHitRows.begin(), templateHitRows.end(), byCallsDesc);
    std::sort(placeholderRows.begin(), placeholderRows.end(), byCallsDesc);

    std::error_code ec;
    const std::filesystem::path dirPath(outDir);
    std::filesystem::create_directories(dirPath, ec);
    if (ec) {
        return false;
    }

    const std::filesystem::path jsonPath = dirPath / ("session_" + stamp + ".json");
    const std::filesystem::path latestJsonPath = dirPath / "latest_session.json";
    const std::filesystem::path tsvPath = dirPath / ("unmatched_" + stamp + ".tsv");
    const std::filesystem::path latestTsvPath = dirPath / "latest_unmatched.tsv";

    const uint64_t trCalls = s.translatedCalls();
    const uint64_t tblCalls = s.translatableCalls();
    const size_t trUniq = s.translatedUnique();
    const size_t tblUniq = s.translatableUnique();

    std::ostringstream jss;
    jss << "{\n";
    jss << "  \"session_stamp\": " << QuoteJsonString(stamp) << ",\n";
    jss << "  \"calls\": {\n";
    jss << "    \"EXACT\": " << s.exactCalls << ",\n";
    jss << "    \"CONTEXT\": " << s.contextCalls << ",\n";
    jss << "    \"TEMPLATE\": " << s.templateCalls << ",\n";
    jss << "    \"REGEX\": " << s.regexCalls << ",\n";
    jss << "    \"UNMATCHED\": " << s.unmatchedCalls << ",\n";
    jss << "    \"IGNORED\": " << s.ignoredCalls << ",\n";
    jss << "    \"TAG_REJECTED\": " << s.tagRejectedCalls << "\n";
    jss << "  },\n";
    jss << "  \"unique\": {\n";
    jss << "    \"EXACT\": " << s.exactUnique << ",\n";
    jss << "    \"CONTEXT\": " << s.contextUnique << ",\n";
    jss << "    \"TEMPLATE\": " << s.templateUnique << ",\n";
    jss << "    \"REGEX\": " << s.regexUnique << ",\n";
    jss << "    \"UNMATCHED\": " << s.unmatchedUnique << ",\n";
    jss << "    \"IGNORED\": " << s.ignoredUnique << ",\n";
    jss << "    \"TAG_REJECTED\": " << s.tagRejectedUnique << "\n";
    jss << "  },\n";
    jss << "  \"summary\": {\n";
    jss << "    \"translated_calls\": " << trCalls << ",\n";
    jss << "    \"translatable_calls\": " << tblCalls << ",\n";
    jss << "    \"call_weighted_coverage_pct\": " << SafePct(trCalls, tblCalls) << ",\n";
    jss << "    \"translated_unique\": " << trUniq << ",\n";
    jss << "    \"translatable_unique\": " << tblUniq << ",\n";
    jss << "    \"unique_coverage_pct\": " << SafePct(trUniq, tblUniq) << ",\n";
    jss << "    \"map_cap_dropped\": " << capDropped << "\n";
    jss << "  },\n";

    const uint64_t coreTrCalls = s.coreStaticTranslatedCalls();
    const uint64_t coreTblCalls = s.coreStaticTranslatableCalls();
    const uint64_t coreUiTblCalls = s.coreStaticUiOnlyTranslatableCalls();
    const size_t coreTblUniq = s.coreStaticTranslatableUnique();
    const uint64_t noticeTblCalls = s.noticeTranslatableCalls();
    const size_t noticeTblUniq = s.noticeTranslatableUnique();

    jss << "  \"ignored_subbuckets\": {\n";
    jss << "    \"calls\": {\n";
    jss << "      \"IGNORED_NUMERIC\": " << s.ignoredNumericCalls << ",\n";
    jss << "      \"IGNORED_ASCII_COUNTER\": " << s.ignoredAsciiCounterCalls << ",\n";
    jss << "      \"IGNORED_ALREADY_KOREAN\": " << s.ignoredAlreadyKoreanCalls << ",\n";
    jss << "      \"IGNORED_INTERNAL_PLACEHOLDER\": " << s.ignoredInternalPlaceholderCalls << ",\n";
    jss << "      \"IGNORED_NON_VISIBLE\": " << s.ignoredNonVisibleCalls << "\n";
    jss << "    },\n";
    jss << "    \"unique\": {\n";
    jss << "      \"IGNORED_NUMERIC\": " << s.ignoredNumericUnique << ",\n";
    jss << "      \"IGNORED_ASCII_COUNTER\": " << s.ignoredAsciiCounterUnique << ",\n";
    jss << "      \"IGNORED_ALREADY_KOREAN\": " << s.ignoredAlreadyKoreanUnique << ",\n";
    jss << "      \"IGNORED_INTERNAL_PLACEHOLDER\": " << s.ignoredInternalPlaceholderUnique << ",\n";
    jss << "      \"IGNORED_NON_VISIBLE\": " << s.ignoredNonVisibleUnique << "\n";
    jss << "    }\n";
    jss << "  },\n";

    jss << "  \"unmatched_subbuckets\": {\n";
    jss << "    \"calls\": {\n";
    jss << "      \"UNMATCHED_STATIC_VISIBLE\": " << s.unmatchedStaticVisibleCalls << ",\n";
    jss << "      \"UNMATCHED_SERVER_DYNAMIC\": " << s.unmatchedServerDynamicCalls << ",\n";
    jss << "      \"UNMATCHED_ASSET_TEXT\": " << s.unmatchedAssetTextCalls << "\n";
    jss << "    },\n";
    jss << "    \"unique\": {\n";
    jss << "      \"UNMATCHED_STATIC_VISIBLE\": " << s.unmatchedStaticVisibleUnique << ",\n";
    jss << "      \"UNMATCHED_SERVER_DYNAMIC\": " << s.unmatchedServerDynamicUnique << ",\n";
    jss << "      \"UNMATCHED_ASSET_TEXT\": " << s.unmatchedAssetTextUnique << "\n";
    jss << "    }\n";
    jss << "  },\n";

    jss << "  \"r2_metrics\": {\n";
    jss << "    \"core_static_translated_calls\": " << coreTrCalls << ",\n";
    jss << "    \"core_static_translatable_calls\": " << coreTblCalls << ",\n";
    jss << "    \"core_static_call_coverage_pct\": " << SafePct(coreTrCalls, coreTblCalls) << ",\n";
    jss << "    \"core_static_ui_translatable_calls\": " << coreUiTblCalls << ",\n";
    jss << "    \"core_static_ui_call_coverage_pct\": " << SafePct(coreTrCalls, coreUiTblCalls) << ",\n";
    jss << "    \"core_static_translated_unique\": " << s.coreStaticTranslatedUnique << ",\n";
    jss << "    \"core_static_translatable_unique\": " << coreTblUniq << ",\n";
    jss << "    \"core_static_unique_coverage_pct\": " << SafePct(s.coreStaticTranslatedUnique, coreTblUniq) << ",\n";
    jss << "    \"notice_translated_calls\": " << s.noticeTranslatedCalls << ",\n";
    jss << "    \"notice_translatable_calls\": " << noticeTblCalls << ",\n";
    jss << "    \"notice_call_coverage_pct\": " << SafePct(s.noticeTranslatedCalls, noticeTblCalls) << ",\n";
    jss << "    \"notice_translated_unique\": " << s.noticeTranslatedUnique << ",\n";
    jss << "    \"notice_translatable_unique\": " << noticeTblUniq << ",\n";
    jss << "    \"notice_unique_coverage_pct\": " << SafePct(s.noticeTranslatedUnique, noticeTblUniq) << "\n";
    jss << "  },\n";

    jss << "  \"screens\": {\n";
    size_t scrIdx = 0;
    for (const auto& [name, sc] : screens) {
        const uint64_t sTbl = sc.translated + sc.unmatched + sc.tagRejected;
        jss << "    " << QuoteJsonString(name) << ": {\"translated\": " << sc.translated
            << ", \"unmatched\": " << sc.unmatched << ", \"ignored\": " << sc.ignored
            << ", \"tag_rejected\": " << sc.tagRejected
            << ", \"coverage_pct\": " << SafePct(sc.translated, sTbl) << "}";
        if (++scrIdx < screens.size()) jss << ",";
        jss << "\n";
    }
    jss << "  },\n";

    auto writeEntryArray = [&](const char* fieldName,
                               const std::vector<std::pair<std::string, CoverageEntryStats>>& rows,
                               size_t limit,
                               int subKind,  // 0=none, 1=unmatched, 2=ignored
                               bool trailingComma) {
        jss << "  \"" << fieldName << "\": [\n";
        const size_t cnt = std::min(rows.size(), limit);
        for (size_t i = 0; i < cnt; ++i) {
            const auto& [text, st] = rows[i];
            jss << "    {\"text\": " << QuoteJsonString(text)
                << ", \"shape\": " << QuoteJsonString(st.shapeKey)
                << ", \"calls\": " << st.calls
                << ", \"rule\": " << QuoteJsonString(st.ruleOrReason);
            if (subKind == 1) {
                jss << ", \"subbucket\": " << QuoteJsonString(UnmatchedSubbucketName(static_cast<UnmatchedSubbucket>(st.subbucket)));
            } else if (subKind == 2) {
                jss << ", \"subbucket\": " << QuoteJsonString(IgnoredSubbucketName(static_cast<IgnoredSubbucket>(st.subbucket)));
            }
            jss << ", \"rvas\": [";
            for (size_t r = 0; r < st.callerRvas.size(); ++r) {
                char hex[16];
                std::snprintf(hex, sizeof(hex), "0x%x", st.callerRvas[r]);
                jss << QuoteJsonString(hex) << (r + 1 < st.callerRvas.size() ? ", " : "");
            }
            jss << "], \"screens\": [";
            for (size_t si = 0; si < st.screens.size(); ++si) {
                jss << QuoteJsonString(st.screens[si]) << (si + 1 < st.screens.size() ? ", " : "");
            }
            jss << "]}" << (i + 1 < cnt ? "," : "") << "\n";
        }
        jss << "  ]" << (trailingComma ? "," : "") << "\n";
    };

    writeEntryArray("top_unmatched", unmatchedRows, 250, 1, true);
    writeEntryArray("top_dynamic_matches", templateHitRows, 100, 0, true);
    writeEntryArray("top_internal_placeholders", placeholderRows, 100, 2, true);
    writeEntryArray("tag_rejected", tagRejectRows, 100, 0, false);
    jss << "}\n";

    const std::string jsonBody = jss.str();
    {
        std::ofstream ofs(jsonPath, std::ios::binary | std::ios::trunc);
        if (ofs) ofs.write(jsonBody.data(), static_cast<std::streamsize>(jsonBody.size()));
    }
    {
        std::ofstream ofs(latestJsonPath, std::ios::binary | std::ios::trunc);
        if (ofs) ofs.write(jsonBody.data(), static_cast<std::streamsize>(jsonBody.size()));
    }

    std::ostringstream tss;
    tss << "calls\tsource_escaped\tshape_key\tcaller_rvas\tscreens\tsubbucket\n";
    for (const auto& [text, st] : unmatchedRows) {
        tss << st.calls << '\t' << EscapeTsvField(text) << '\t' << EscapeTsvField(st.shapeKey) << '\t';
        for (size_t r = 0; r < st.callerRvas.size(); ++r) {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%x", st.callerRvas[r]);
            tss << hex << (r + 1 < st.callerRvas.size() ? "," : "");
        }
        tss << '\t';
        for (size_t si = 0; si < st.screens.size(); ++si) {
            tss << EscapeTsvField(st.screens[si]) << (si + 1 < st.screens.size() ? "," : "");
        }
        tss << '\t' << UnmatchedSubbucketName(static_cast<UnmatchedSubbucket>(st.subbucket)) << '\n';
    }
    const std::string tsvBody = tss.str();
    {
        std::ofstream ofs(tsvPath, std::ios::binary | std::ios::trunc);
        if (ofs) ofs.write(tsvBody.data(), static_cast<std::streamsize>(tsvBody.size()));
    }
    {
        std::ofstream ofs(latestTsvPath, std::ios::binary | std::ios::trunc);
        if (ofs) ofs.write(tsvBody.data(), static_cast<std::streamsize>(tsvBody.size()));
    }

    if (outSessionJsonPath) *outSessionJsonPath = jsonPath.string();
    if (outUnmatchedTsvPath) *outUnmatchedTsvPath = tsvPath.string();
    return true;
}

}  // namespace aigiskr::core
