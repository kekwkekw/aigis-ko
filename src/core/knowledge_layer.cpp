#include "core/knowledge_layer.hpp"

#include "core/logger.hpp"
#include "core/mini_json.hpp"
#include "core/tag_parser.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace aigiskr::core {

namespace {

struct ProtectedSpan {
    size_t start = 0;
    size_t end = 0;
};

bool ContainsCaseInsensitiveAscii(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < needle.size(); ++k) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + k])) !=
                std::tolower(static_cast<unsigned char>(needle[k]))) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

std::vector<std::wstring> ParseWideArray(const JsonValue* arrVal) {
    std::vector<std::wstring> out;
    if (!arrVal || arrVal->type != JsonValue::Type::Array) {
        return out;
    }
    out.reserve(arrVal->arrVal.size());
    for (const auto& item : arrVal->arrVal) {
        if (item.type == JsonValue::Type::String && !item.strVal.empty()) {
            out.push_back(Utf8ToWide(NormalizeLineEndings(item.strVal)));
        }
    }
    return out;
}

std::vector<std::string> ParseAsciiArray(const JsonValue* arrVal) {
    std::vector<std::string> out;
    if (!arrVal || arrVal->type != JsonValue::Type::Array) {
        return out;
    }
    out.reserve(arrVal->arrVal.size());
    for (const auto& item : arrVal->arrVal) {
        if (item.type == JsonValue::Type::String && !item.strVal.empty()) {
            out.push_back(item.strVal);
        }
    }
    return out;
}

bool GetBoolField(const JsonValue& obj, std::string_view key, bool defaultVal) {
    if (const auto* v = obj.Find(key)) {
        if (v->type == JsonValue::Type::Bool) {
            return v->boolVal;
        }
        if (v->type == JsonValue::Type::Number) {
            return v->numVal != 0.0;
        }
    }
    return defaultVal;
}

int GetIntField(const JsonValue& obj, std::string_view key, int defaultVal) {
    if (const auto* v = obj.Find(key)) {
        if (v->type == JsonValue::Type::Number) {
            return static_cast<int>(v->numVal);
        }
    }
    return defaultVal;
}

std::string GetStringField(const JsonValue& obj, std::string_view key) {
    if (const auto* v = obj.Find(key)) {
        if (v->type == JsonValue::Type::String) {
            return v->strVal;
        }
    }
    return {};
}

template <typename GuardT>
bool PassesGlobalGuards(const std::wstring& fullText, std::string_view screenTag, const GuardT& guards) {
    if (!guards.domainScopes.empty()) {
        bool scopeMatch = false;
        for (const auto& sc : guards.domainScopes) {
            if (ContainsCaseInsensitiveAscii(screenTag, sc)) {
                scopeMatch = true;
                break;
            }
        }
        if (!scopeMatch) {
            return false;
        }
    }
    if (!guards.requireContextContains.empty()) {
        bool anyCtx = false;
        for (const auto& needle : guards.requireContextContains) {
            if (!needle.empty() && fullText.find(needle) != std::wstring::npos) {
                anyCtx = true;
                break;
            }
        }
        if (!anyCtx) {
            return false;
        }
    }
    for (const auto& ex : guards.excludeIfContains) {
        if (!ex.empty() && fullText.find(ex) != std::wstring::npos) {
            return false;
        }
    }
    return true;
}

template <typename GuardT>
bool PassesBoundaryGuards(const std::wstring& text,
                          size_t matchStart,
                          size_t matchEnd,
                          const GuardT& guards) {
    for (const auto& pre : guards.notPrecededBy) {
        if (!pre.empty() && matchStart >= pre.size() &&
            text.compare(matchStart - pre.size(), pre.size(), pre) == 0) {
            return false;
        }
    }
    for (const auto& suf : guards.notFollowedBy) {
        if (!suf.empty() && matchEnd + suf.size() <= text.size() &&
            text.compare(matchEnd, suf.size(), suf) == 0) {
            return false;
        }
    }
    return true;
}

void FindTokenPrefixSpans(const std::wstring& text,
                          const wchar_t* prefix,
                          size_t prefixLen,
                          const wchar_t* suffix,
                          size_t suffixLen,
                          std::vector<ProtectedSpan>& spans) {
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t hit = text.find(prefix, pos);
        if (hit == std::wstring::npos) {
            break;
        }
        size_t d = hit + prefixLen;
        const size_t dStart = d;
        while (d < text.size() && text[d] >= L'0' && text[d] <= L'9') {
            ++d;
        }
        if (d > dStart && d + suffixLen <= text.size() &&
            text.compare(d, suffixLen, suffix) == 0) {
            spans.push_back({hit, d + suffixLen});
            pos = d + suffixLen;
        } else {
            pos = hit + prefixLen;
        }
    }
}

std::vector<ProtectedSpan> CollectSentinelSpans(const std::wstring& text) {
    std::vector<ProtectedSpan> spans;
    static constexpr wchar_t kTagPrefix[] = L"__AIGIS_TAG_";
    static constexpr wchar_t kTagSuffix[] = L"__";
    static constexpr wchar_t kAkn[] = L"AKN";
    FindTokenPrefixSpans(text, kTagPrefix, 12, kTagSuffix, 2, spans);
    FindTokenPrefixSpans(text, kAkn, 3, kAkn, 3, spans);
    std::sort(spans.begin(), spans.end(), [](const ProtectedSpan& a, const ProtectedSpan& b) {
        return a.start < b.start;
    });
    return spans;
}

bool OverlapsProtectedSpan(const std::vector<ProtectedSpan>& spans, size_t start, size_t end) noexcept {
    for (const auto& sp : spans) {
        if (sp.end <= start) {
            continue;
        }
        if (sp.start >= end) {
            break;
        }
        return true;
    }
    return false;
}

std::wstring ExpandRegexReplacement(std::wstring_view tmpl, const std::wsmatch& match) {
    std::wstring out;
    out.reserve(tmpl.size() + 16);
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == L'$' && i + 1 < tmpl.size()) {
            const wchar_t next = tmpl[i + 1];
            if (next >= L'0' && next <= L'9') {
                const size_t groupIdx = static_cast<size_t>(next - L'0');
                if (groupIdx < match.size()) {
                    out.append(match[groupIdx].str());
                }
                ++i;
                continue;
            }
            if (next == L'$') {
                out.push_back(L'$');
                ++i;
                continue;
            }
        }
        out.push_back(tmpl[i]);
    }
    return out;
}

template <typename GuardParser>
void LoadGuardsFromObj(const JsonValue& obj, GuardParser& guards) {
    guards.notPrecededBy = ParseWideArray(obj.Find("not_preceded_by"));
    guards.notFollowedBy = ParseWideArray(obj.Find("not_followed_by"));
    guards.requireContextContains = ParseWideArray(obj.Find("require_context_contains"));
    guards.excludeIfContains = ParseWideArray(obj.Find("exclude_if_contains"));
    guards.domainScopes = ParseAsciiArray(obj.Find("domain_scopes"));
}

bool IsHangulSyllable(wchar_t c) noexcept {
    return c >= 0xAC00 && c <= 0xD7A3;
}

bool HasHangulBatchim(wchar_t syllable) noexcept {
    return IsHangulSyllable(syllable) && ((syllable - 0xAC00) % 28) != 0;
}

bool IsHangulRieulBatchim(wchar_t syllable) noexcept {
    return IsHangulSyllable(syllable) && ((syllable - 0xAC00) % 28) == 8;
}

bool IsParticleBoundary(const std::wstring& s, size_t idx) noexcept {
    if (idx >= s.size()) {
        return true;
    }
    const wchar_t ch = s[idx];
    // Treat any non-Hangul character (including …, ★, ♪, tags, brackets, spaces, digits)
    // as a valid particle boundary so trailing Korean particles never miss punctuation.
    const bool isHangulJamo = (ch >= 0x3131 && ch <= 0x318E) || (ch >= 0x1100 && ch <= 0x11FF);
    return !IsHangulSyllable(ch) && !isHangulJamo;
}

bool IsKatakanaWordLead(wchar_t ch) noexcept {
    return (ch >= 0x30A1 && ch <= 0x30FA) || ch == 0x30FC;
}

bool IsWordLeadSpaceCandidate(wchar_t ch) noexcept {
    if (IsHangulSyllable(ch)) {
        return true;
    }
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9');
}

bool StartsWithSingleKoreanParticle(const std::wstring& s, size_t pos) noexcept {
    if (pos >= s.size()) {
        return false;
    }
    const wchar_t c = s[pos];
    // Standalone 1-char particles: 의(\uC758), 에(\uC5D0), 와(\uC640), 과(\uACFC), 도(\uB3C4), 만(\uB9CC), 이(\uC774), 가(\uAC00), 은(\uC740), 는(\uB294), 을(\uC744), 를(\uB97C), 로(\uB85C)
    if (c == 0xC758 || c == 0xC5D0 || c == 0xC640 || c == 0xACFC || c == 0xB3C4 ||
        c == 0xB9CC || c == 0xC774 || c == 0xAC00 || c == 0xC740 || c == 0xB294 ||
        c == 0xC744 || c == 0xB97C || c == 0xB85C) {
        if (IsParticleBoundary(s, pos + 1)) {
            return true;
        }
    }
    // 2-char particles: 에게(\uC5D0\uAC8C), 에서(\uC5D0\uC11C), 으로(\uC73C\uB85C), 까지(\uAE4C\uC9C0), 부터(\uBD80\uD130), 이랑(\uC774\uB791)
    if (pos + 1 < s.size()) {
        const wchar_t c2 = s[pos + 1];
        if ((c == 0xC5D0 && (c2 == 0xAC8C || c2 == 0xC11C)) ||
            (c == 0xC73C && c2 == 0xB85C) ||
            (c == 0xAE4C && c2 == 0xC9C0) ||
            (c == 0xBD80 && c2 == 0xD130) ||
            (c == 0xC774 && c2 == 0xB791)) {
            if (IsParticleBoundary(s, pos + 2)) {
                return true;
            }
        }
        if ((c == 0xB77C || c == 0xB791) && (c2 == 0xACE0 || c2 == 0xBA74 || c2 == 0xB3C4)) {
            if (IsParticleBoundary(s, pos + 2)) {
                return true;
            }
        }
    }
    if (pos + 2 < s.size() && c == 0xC774 && s[pos + 1] == 0xB77C &&
        (s[pos + 2] == 0xACE0 || s[pos + 2] == 0xBA74 || s[pos + 2] == 0xB3C4) &&
        IsParticleBoundary(s, pos + 3)) {
        return true;
    }
    return false;
}

bool TryAdjustFollowingParticle(std::wstring& text, size_t particlePos, wchar_t lastHangul) {
    if (particlePos >= text.size() || !IsHangulSyllable(lastHangul)) {
        return false;
    }
    const bool hasBatchim = HasHangulBatchim(lastHangul);
    const bool isRieul = IsHangulRieulBatchim(lastHangul);
    const wchar_t p0 = text[particlePos];

    // 이라고/이라고/이라면/이라도 vs 라고/라면/라도
    if (p0 == 0xC774 && particlePos + 2 < text.size() && text[particlePos + 1] == 0xB77C &&
        (text[particlePos + 2] == 0xACE0 || text[particlePos + 2] == 0xBA74 || text[particlePos + 2] == 0xB3C4) &&
        IsParticleBoundary(text, particlePos + 3)) {
        if (!hasBatchim) {
            text.erase(particlePos, 1);
            return true;
        }
        return false;
    }
    if (p0 == 0xB77C && particlePos + 1 < text.size() &&
        (text[particlePos + 1] == 0xACE0 || text[particlePos + 1] == 0xBA74 || text[particlePos + 1] == 0xB3C4) &&
        IsParticleBoundary(text, particlePos + 2)) {
        if (hasBatchim) {
            text.insert(particlePos, 1, 0xC774);
            return true;
        }
        return false;
    }

    // 으로(\uC73C\uB85C) vs 로(\uB85C)
    if (p0 == 0xC73C && particlePos + 1 < text.size() && text[particlePos + 1] == 0xB85C &&
        IsParticleBoundary(text, particlePos + 2)) {
        if (!hasBatchim || isRieul) {
            text.replace(particlePos, 2, 1, 0xB85C);
            return true;
        }
        return false;
    }
    if (p0 == 0xB85C && IsParticleBoundary(text, particlePos + 1)) {
        if (hasBatchim && !isRieul) {
            const wchar_t euro[2] = {0xC73C, 0xB85C};
            text.replace(particlePos, 1, euro, 2);
            return true;
        }
        return false;
    }

    // Single-syllable batchim-paired particles at word boundary
    if (!IsParticleBoundary(text, particlePos + 1)) {
        return false;
    }
    wchar_t desired = 0;
    if (p0 == 0xC774 || p0 == 0xAC00) {          // 이 / 가
        desired = hasBatchim ? 0xC774 : 0xAC00;
    } else if (p0 == 0xC740 || p0 == 0xB294) {   // 은 / 는
        desired = hasBatchim ? 0xC740 : 0xB294;
    } else if (p0 == 0xC744 || p0 == 0xB97C) {   // 을 / 를
        desired = hasBatchim ? 0xC744 : 0xB97C;
    } else if (p0 == 0xACFC || p0 == 0xC640) {   // 과 / 와
        desired = hasBatchim ? 0xACFC : 0xC640;
    }
    if (desired != 0 && desired != p0) {
        text[particlePos] = desired;
        return true;
    }
    return false;
}

template <typename Rule>
uint32_t ApplyRuleList(std::wstring& wtext,
                       std::string_view screenTag,
                       const std::vector<Rule>& rules,
                       bool protectSentinels) {
    uint32_t rulesApplied = 0;
    for (const auto& rule : rules) {
        if (!PassesGlobalGuards(wtext, screenTag, rule.guards)) {
            continue;
        }
        const auto spans = protectSentinels ? CollectSentinelSpans(wtext) : std::vector<ProtectedSpan>{};
        bool ruleHit = false;
        std::wstring next;
        size_t cursor = 0;
        bool rebuilt = false;

        if (!rule.isRegex) {
            if (rule.patternWide.empty()) {
                continue;
            }
            while (cursor < wtext.size()) {
                const size_t hit = wtext.find(rule.patternWide, cursor);
                if (hit == std::wstring::npos) {
                    break;
                }
                const size_t hitEnd = hit + rule.patternWide.size();
                if ((protectSentinels && OverlapsProtectedSpan(spans, hit, hitEnd)) ||
                    !PassesBoundaryGuards(wtext, hit, hitEnd, rule.guards)) {
                    if (!rebuilt) {
                        next.reserve(wtext.size());
                        rebuilt = true;
                    }
                    next.append(wtext, cursor, hit + 1 - cursor);
                    cursor = hit + 1;
                    continue;
                }
                if (!rebuilt) {
                    next.reserve(wtext.size());
                    rebuilt = true;
                }
                next.append(wtext, cursor, hit - cursor);
                next.append(rule.replaceWide);
                cursor = hitEnd;
                ruleHit = true;
            }
        } else if (rule.regexValid) {
            while (cursor <= wtext.size()) {
                std::wsmatch match;
                const auto flags = (cursor == 0)
                                       ? std::regex_constants::match_default
                                       : (std::regex_constants::match_not_bol |
                                          std::regex_constants::match_prev_avail);
                if (!std::regex_search(wtext.cbegin() + static_cast<std::ptrdiff_t>(cursor),
                                       wtext.cend(),
                                       match,
                                       rule.compiledRegex,
                                       flags)) {
                    break;
                }
                const size_t hit = cursor + static_cast<size_t>(match.position(0));
                const size_t mlen = static_cast<size_t>(match.length(0));
                const size_t hitEnd = hit + mlen;
                if (mlen == 0) {
                    break;
                }
                if ((protectSentinels && OverlapsProtectedSpan(spans, hit, hitEnd)) ||
                    !PassesBoundaryGuards(wtext, hit, hitEnd, rule.guards)) {
                    if (!rebuilt) {
                        next.reserve(wtext.size());
                        rebuilt = true;
                    }
                    next.append(wtext, cursor, hit + 1 - cursor);
                    cursor = hit + 1;
                    continue;
                }
                if (!rebuilt) {
                    next.reserve(wtext.size());
                    rebuilt = true;
                }
                next.append(wtext, cursor, hit - cursor);
                next.append(ExpandRegexReplacement(rule.replaceWide, match));
                cursor = hitEnd;
                ruleHit = true;
            }
        }

        if (rebuilt) {
            next.append(wtext, cursor, wtext.size() - cursor);
            wtext = std::move(next);
            if (ruleHit) {
                ++rulesApplied;
            }
        }
    }
    return rulesApplied;
}

}  // namespace

void KnowledgeLayer::Clear() {
    loaded_ = false;
    version_.clear();
    exactOverrides_.clear();
    properNouns_.clear();
    preRules_.clear();
    postRules_.clear();
}

KnowledgeStats KnowledgeLayer::GetStats() const {
    KnowledgeStats st;
    st.loaded = loaded_;
    st.version = version_;
    st.exactOverrides = exactOverrides_.size();
    st.properNouns = properNouns_.size();
    st.preRules = preRules_.size();
    st.postRules = postRules_.size();
    return st;
}

bool KnowledgeLayer::LoadExactOverridesFile(const std::wstring& path, std::string* errOut) {
    if (std::filesystem::status(std::filesystem::path(path)).type() != std::filesystem::file_type::regular) {
        return false;
    }
    JsonValue root;
    std::string parseErr;
    if (!ParseJsonFile(WideToUtf8(path), root, parseErr) || root.type != JsonValue::Type::Object) {
        if (errOut) *errOut = "ExactOverrides parse failed: " + parseErr;
        return false;
    }
    if (version_.empty()) {
        version_ = GetStringField(root, "knowledge_version");
    }
    if (const auto* entriesArr = root.Find("entries")) {
        if (entriesArr->type == JsonValue::Type::Array) {
            for (const auto& item : entriesArr->arrVal) {
                if (item.type != JsonValue::Type::Object) continue;
                if (!GetBoolField(item, "enabled", true)) continue;
                const std::string jp = NormalizeLineEndings(GetStringField(item, "jp"));
                const std::string ko = NormalizeLineEndings(GetStringField(item, "ko"));
                if (!jp.empty() && !ko.empty()) {
                    exactOverrides_[jp] = ko;
                }
            }
            return true;
        }
        if (entriesArr->type == JsonValue::Type::Object) {
            for (const auto& kv : entriesArr->objVal) {
                if (kv.second.type == JsonValue::Type::String) {
                    const std::string jp = NormalizeLineEndings(kv.first);
                    const std::string ko = NormalizeLineEndings(kv.second.strVal);
                    if (!jp.empty() && !ko.empty()) {
                        exactOverrides_[jp] = ko;
                    }
                }
            }
            return true;
        }
    }
    return false;
}

bool KnowledgeLayer::LoadProperNounsFile(const std::wstring& path, std::string* errOut) {
    if (std::filesystem::status(std::filesystem::path(path)).type() != std::filesystem::file_type::regular) {
        return false;
    }
    JsonValue root;
    std::string parseErr;
    if (!ParseJsonFile(WideToUtf8(path), root, parseErr) || root.type != JsonValue::Type::Object) {
        if (errOut) *errOut = "ProperNouns parse failed: " + parseErr;
        return false;
    }
    if (version_.empty()) {
        version_ = GetStringField(root, "knowledge_version");
    }
    const auto* entriesArr = root.Find("entries");
    if (!entriesArr || entriesArr->type != JsonValue::Type::Array) {
        return false;
    }
    for (const auto& item : entriesArr->arrVal) {
        if (item.type != JsonValue::Type::Object) continue;
        if (!GetBoolField(item, "enabled", true)) continue;
        const std::string jp = NormalizeLineEndings(GetStringField(item, "jp"));
        const std::string ko = NormalizeLineEndings(GetStringField(item, "ko"));
        if (jp.empty() || ko.empty()) continue;

        ProperNounRule rule;
        rule.id = GetStringField(item, "id");
        rule.jpWide = Utf8ToWide(jp);
        rule.koWide = Utf8ToWide(ko);
        rule.priority = GetIntField(item, "priority", 100);
        rule.useSentinel = GetBoolField(item, "use_sentinel", true);
        rule.fixParticle = GetBoolField(item, "fix_particle", true);
        rule.spaceBeforeNonParticle = GetBoolField(item, "space_before_non_particle", false);
        rule.spaceInPreBeforeKatakana = GetBoolField(item, "space_in_pre_before_katakana", false);
        const std::string deferMatch = NormalizeLineEndings(GetStringField(item, "defer_to_post_on_katakana"));
        if (!deferMatch.empty()) {
            rule.deferToPostOnKatakana = Utf8ToWide(deferMatch);
        }
        LoadGuardsFromObj(item, rule.guards);
        if (!rule.jpWide.empty() && !rule.koWide.empty()) {
            properNouns_.push_back(std::move(rule));
        }
    }
    std::stable_sort(properNouns_.begin(), properNouns_.end(),
                     [](const ProperNounRule& a, const ProperNounRule& b) {
                         if (a.priority != b.priority) return a.priority > b.priority;
                         return a.jpWide.size() > b.jpWide.size();
                     });
    return true;
}

bool KnowledgeLayer::LoadRewriteRulesFile(const std::wstring& path, bool isPost, std::string* errOut) {
    if (std::filesystem::status(std::filesystem::path(path)).type() != std::filesystem::file_type::regular) {
        return false;
    }
    JsonValue root;
    std::string parseErr;
    if (!ParseJsonFile(WideToUtf8(path), root, parseErr) || root.type != JsonValue::Type::Object) {
        if (errOut) *errOut = std::string(isPost ? "PostRules" : "PreRules") + " parse failed: " + parseErr;
        return false;
    }
    if (version_.empty()) {
        version_ = GetStringField(root, "knowledge_version");
    }
    const auto* rulesArr = root.Find("rules");
    if (!rulesArr || rulesArr->type != JsonValue::Type::Array) {
        return false;
    }
    auto& target = isPost ? postRules_ : preRules_;
    for (const auto& item : rulesArr->arrVal) {
        if (item.type != JsonValue::Type::Object) continue;
        if (!GetBoolField(item, "enabled", true)) continue;
        const std::string pat = NormalizeLineEndings(GetStringField(item, "pattern"));
        const std::string rep = NormalizeLineEndings(GetStringField(item, "replace"));
        if (pat.empty()) continue;

        RewriteRule rule;
        rule.id = GetStringField(item, "id");
        rule.isRegex = (GetStringField(item, "kind") == "regex");
        rule.priority = GetIntField(item, "priority", 100);
        rule.patternWide = Utf8ToWide(pat);
        rule.replaceWide = Utf8ToWide(rep);
        LoadGuardsFromObj(item, rule.guards);
        if (rule.isRegex) {
            try {
                rule.compiledRegex = std::wregex(rule.patternWide, std::regex_constants::ECMAScript);
                rule.regexValid = true;
            } catch (const std::regex_error& e) {
                Logger::Instance().Logf(
                    "[KNOWLEDGE] Skipping invalid regex rule '%s': %s",
                    rule.id.c_str(),
                    e.what());
                continue;
            }
        }
        target.push_back(std::move(rule));
    }
    std::stable_sort(target.begin(), target.end(),
                     [](const RewriteRule& a, const RewriteRule& b) {
                         if (a.priority != b.priority) return a.priority > b.priority;
                         return a.patternWide.size() > b.patternWide.size();
                     });
    return true;
}

bool KnowledgeLayer::LoadFromDirectory(const std::wstring& dirPath, std::string* errorOut) {
    Clear();
    if (dirPath.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path base(dirPath);
    if (!std::filesystem::exists(base, ec) || !std::filesystem::is_directory(base, ec)) {
        if (errorOut) *errorOut = "Knowledge directory does not exist";
        return false;
    }

    std::wstring exactFile = (base / L"exact_overrides_v0.json").wstring();
    std::wstring properFile = (base / L"proper_nouns_v0.json").wstring();
    std::wstring preFile = (base / L"pre_rules_v0.json").wstring();
    std::wstring postFile = (base / L"post_rules_v0.json").wstring();

    std::filesystem::path manifestPath = base / L"manifest_v0_1.json";
    if (!std::filesystem::is_regular_file(manifestPath, ec)) {
        manifestPath = base / L"manifest.json";
    }
    if (!std::filesystem::is_regular_file(manifestPath, ec)) {
        manifestPath = base / L"manifest_v0.json";
    }
    if (std::filesystem::is_regular_file(manifestPath, ec)) {
        JsonValue manifest;
        std::string mErr;
        if (ParseJsonFile(WideToUtf8(manifestPath.wstring()), manifest, mErr) &&
            manifest.type == JsonValue::Type::Object) {
            const std::string ver = GetStringField(manifest, "knowledge_version");
            if (!ver.empty()) {
                version_ = ver;
            }
            const auto resolveManifestChild = [&](std::string_view key, std::wstring& target) {
                const std::string rel = GetStringField(manifest, key);
                if (!rel.empty()) {
                    target = (base / Utf8ToWide(rel)).wstring();
                }
            };
            resolveManifestChild("exact_overrides_file", exactFile);
            resolveManifestChild("proper_nouns_file", properFile);
            resolveManifestChild("pre_rules_file", preFile);
            resolveManifestChild("post_rules_file", postFile);
        }
    }

    std::string subErr;
    const bool okExact = LoadExactOverridesFile(exactFile, &subErr);
    if (!okExact && !subErr.empty()) {
        Logger::Instance().Logf("[KNOWLEDGE] %s", subErr.c_str());
        subErr.clear();
    }
    const bool okProper = LoadProperNounsFile(properFile, &subErr);
    if (!okProper && !subErr.empty()) {
        Logger::Instance().Logf("[KNOWLEDGE] %s", subErr.c_str());
        subErr.clear();
    }
    const bool okPre = LoadRewriteRulesFile(preFile, false, &subErr);
    if (!okPre && !subErr.empty()) {
        Logger::Instance().Logf("[KNOWLEDGE] %s", subErr.c_str());
        subErr.clear();
    }
    const bool okPost = LoadRewriteRulesFile(postFile, true, &subErr);
    if (!okPost && !subErr.empty()) {
        Logger::Instance().Logf("[KNOWLEDGE] %s", subErr.c_str());
    }

    loaded_ = okExact || okProper || okPre || okPost;
    if (loaded_ && version_.empty()) {
        version_ = "aigis-v0";
    }
    if (!loaded_ && errorOut) {
        *errorOut = "No valid knowledge rule files loaded";
    }
    return loaded_;
}

bool KnowledgeLayer::LookupExactOverride(std::string_view normalizedSource, std::string& outTranslated) const {
    if (!loaded_ || exactOverrides_.empty() || normalizedSource.empty()) {
        return false;
    }
    const auto it = exactOverrides_.find(std::string(normalizedSource));
    if (it == exactOverrides_.end()) {
        return false;
    }
    outTranslated = it->second;
    return true;
}

PreProcessResult KnowledgeLayer::PreProcess(std::string_view tagMaskedSource, std::string_view screenTag) const {
    PreProcessResult res;
    if (!loaded_ || tagMaskedSource.empty()) {
        res.text.assign(tagMaskedSource);
        return res;
    }

    std::wstring wtext = Utf8ToWide(tagMaskedSource);
    res.preRulesApplied = ApplyRuleList(wtext, screenTag, preRules_, true);

    if (!properNouns_.empty()) {
        // Evaluate context guards against text before sentinel substitution so earlier
        // proper-noun masks (e.g. NPC殿 -> AKN0AKN) never hide context needed by later rules.
        const std::wstring wtextContext = wtext;
        for (const auto& pn : properNouns_) {
            if (!PassesGlobalGuards(wtextContext, screenTag, pn.guards)) {
                continue;
            }
            const auto spans = CollectSentinelSpans(wtext);
            size_t cursor = 0;
            bool rebuilt = false;
            std::wstring next;
            std::wstring assignedSentinel;

            while (cursor < wtext.size()) {
                const size_t hit = wtext.find(pn.jpWide, cursor);
                if (hit == std::wstring::npos) {
                    break;
                }
                const size_t hitEnd = hit + pn.jpWide.size();
                if (OverlapsProtectedSpan(spans, hit, hitEnd) ||
                    !PassesBoundaryGuards(wtext, hit, hitEnd, pn.guards)) {
                    if (!rebuilt) {
                        next.reserve(wtext.size());
                        rebuilt = true;
                    }
                    next.append(wtext, cursor, hit + 1 - cursor);
                    cursor = hit + 1;
                    continue;
                }
                const bool followedByKatakana = (hitEnd < wtext.size()) && IsKatakanaWordLead(wtext[hitEnd]);
                if (!pn.deferToPostOnKatakana.empty() && followedByKatakana) {
                    res.deferredRewrites.push_back({pn.deferToPostOnKatakana, pn.koWide});
                    if (!rebuilt) {
                        next.reserve(wtext.size());
                        rebuilt = true;
                    }
                    next.append(wtext, cursor, hitEnd - cursor);
                    cursor = hitEnd;
                    continue;
                }
                if (!rebuilt) {
                    next.reserve(wtext.size());
                    rebuilt = true;
                }
                if (pn.useSentinel) {
                    if (assignedSentinel.empty()) {
                        const size_t idx = res.sentinels.size();
                        assignedSentinel = L"AKN" + std::to_wstring(idx) + L"AKN";
                        res.sentinels.push_back({assignedSentinel, pn.koWide, pn.fixParticle, pn.spaceBeforeNonParticle});
                    }
                    next.append(wtext, cursor, hit - cursor);
                    next.append(assignedSentinel);
                    if (pn.spaceInPreBeforeKatakana && followedByKatakana) {
                        next.push_back(L' ');
                    }
                } else {
                    next.append(wtext, cursor, hit - cursor);
                    next.append(pn.koWide);
                }
                cursor = hitEnd;
                ++res.properNounsMasked;
            }

            if (rebuilt) {
                next.append(wtext, cursor, wtext.size() - cursor);
                wtext = std::move(next);
            }
        }
    }

    res.text = WideToUtf8(wtext);
    return res;
}

PostProcessResult KnowledgeLayer::PostProcess(std::string_view translatedWithTags,
                                              const PreProcessResult& preInfo,
                                              std::string_view screenTag) const {
    PostProcessResult res;
    if (translatedWithTags.empty()) {
        return res;
    }

    std::wstring wtext = Utf8ToWide(translatedWithTags);

    // 1. Restore ProperNoun sentinels cleanly with Korean batchim particle agreement
    for (const auto& binding : preInfo.sentinels) {
        if (binding.sentinelWide.empty()) {
            continue;
        }
        wchar_t lastHangul = 0;
        for (size_t k = binding.koWide.size(); k > 0; --k) {
            const wchar_t c = binding.koWide[k - 1];
            if (c != L' ' && c != L'\t') {
                lastHangul = c;
                break;
            }
        }

        size_t pos = 0;
        while ((pos = wtext.find(binding.sentinelWide, pos)) != std::wstring::npos) {
            wtext.replace(pos, binding.sentinelWide.size(), binding.koWide);
            const size_t afterPos = pos + binding.koWide.size();
            ++res.sentinelsRestored;

            if (binding.fixParticle && IsHangulSyllable(lastHangul)) {
                if (TryAdjustFollowingParticle(wtext, afterPos, lastHangul)) {
                    ++res.particleFixes;
                }
            }
            if (binding.spaceBeforeNonParticle && afterPos < wtext.size() &&
                IsWordLeadSpaceCandidate(wtext[afterPos]) &&
                !StartsWithSingleKoreanParticle(wtext, afterPos)) {
                wtext.insert(afterPos, 1, L' ');
            }
            pos = afterPos;
        }
    }

    // 1b. Apply deferred compound rewrites (e.g. ちび<katakana> -> hdTrans 꼬맹이 -> 치비)
    for (const auto& def : preInfo.deferredRewrites) {
        if (def.matchWide.empty() || def.replaceWide.empty()) continue;
        size_t pos = 0;
        while ((pos = wtext.find(def.matchWide, pos)) != std::wstring::npos) {
            wtext.replace(pos, def.matchWide.size(), def.replaceWide);
            pos += def.replaceWide.size();
            ++res.postRulesApplied;
        }
    }

    // 2. Apply ordered PostRules while protecting any restored/original engine tags?
    // Engine tags (<TIME>, <NUM3>, %c[...]) SHOULD be readable by PostRules like <NUM3>몸 -> <NUM3>체,
    // while never accidentally breaking syntax because PostRules match surrounding text & specific tag shapes.
    if (loaded_ && !postRules_.empty()) {
        res.postRulesApplied = ApplyRuleList(wtext, screenTag, postRules_, false);
    }

    res.text = WideToUtf8(wtext);
    return res;
}

}  // namespace aigiskr::core
