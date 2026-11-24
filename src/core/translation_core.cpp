#include "core/translation_core.hpp"

#include "core/mini_json.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace aigiskr::core {

namespace {

bool ReadWholeFileUtf8(const std::wstring& path, std::string& outBytes, std::string* errOut) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        if (errOut) *errOut = "cannot open file";
        return false;
    }
    outBytes.clear();
    char buf[16384];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        outBytes.append(buf, n);
    }
    std::fclose(f);
    return true;
}

std::string RestoreLineEndingsIfCrlf(std::string_view origInput, std::string normalizedOut) {
    if (origInput.find("\r\n") == std::string_view::npos) {
        return normalizedOut;
    }
    std::string crlf;
    crlf.reserve(normalizedOut.size() + 8);
    for (char c : normalizedOut) {
        if (c == '\n') {
            crlf.push_back('\r');
        }
        crlf.push_back(c);
    }
    return crlf;
}

}  // namespace

std::string TranslationCore::NormalizeLineEndings(std::string_view input) {
    return aigiskr::core::NormalizeLineEndings(input);
}

std::vector<std::string> TranslationCore::ExtractProtectedTokens(std::string_view normalizedUtf8) {
    auto tags = ExtractProtectedTags(normalizedUtf8);
    std::vector<std::string> out;
    out.reserve(tags.size());
    for (auto& t : tags) {
        out.push_back(std::move(t.token));
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool TranslationCore::TokensMatch(std::string_view srcNormalized, std::string_view dstNormalized) {
    return ValidateTagPreservation(srcNormalized, dstNormalized).valid;
}

void TranslationCore::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    exact_.clear();
    exactContext_.clear();
    templateMatcher_.Clear();
    stats_ = TranslationStats{};
}

bool TranslationCore::LoadFromJsonFile(const std::wstring& path, std::string* errorOut) {
    std::string bytes;
    if (!ReadWholeFileUtf8(path, bytes, errorOut)) {
        return false;
    }
    return LoadFromJsonBytes(bytes, errorOut);
}

bool TranslationCore::LoadFromJsonBytes(std::string_view jsonUtf8, std::string* errorOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    return LoadFromJsonBytesUnlocked(jsonUtf8, false, errorOut);
}

bool TranslationCore::LoadOverlayFromJsonFile(const std::wstring& path, std::string* errorOut) {
    std::string bytes;
    if (!ReadWholeFileUtf8(path, bytes, errorOut)) {
        return false;
    }
    return LoadOverlayFromJsonBytes(bytes, errorOut);
}

bool TranslationCore::LoadOverlayFromJsonBytes(std::string_view jsonUtf8, std::string* errorOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    return LoadFromJsonBytesUnlocked(jsonUtf8, true, errorOut);
}

bool TranslationCore::LoadFromJsonBytesUnlocked(std::string_view jsonUtf8, bool mergeOverlay, std::string* errorOut) {
    JsonValue root;
    std::string parseErr;
    if (!ParseJson(jsonUtf8, root, parseErr)) {
        if (errorOut) *errorOut = parseErr;
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        if (errorOut) *errorOut = "translation JSON root must be object";
        return false;
    }

    // Accept either R1 ("exact") or R0 ("entries") dictionary object (overlay may omit primary if templates-only).
    const JsonValue* primaryObj = root.Find("exact");
    if (!primaryObj) {
        primaryObj = root.Find("entries");
    }
    if (!mergeOverlay && (!primaryObj || primaryObj->type != JsonValue::Type::Object)) {
        if (errorOut) *errorOut = "missing 'exact' or 'entries' JSON object";
        return false;
    }
    if (primaryObj && primaryObj->type != JsonValue::Type::Object) {
        if (errorOut) *errorOut = "'exact' must be a JSON object";
        return false;
    }

    ExactMap loadedExact = mergeOverlay ? std::move(exact_) : ExactMap{};
    if (primaryObj) {
        if (!mergeOverlay) {
            loadedExact.reserve(primaryObj->objVal.size());
        }
        for (const auto& [k, v] : primaryObj->objVal) {
            if (v.type == JsonValue::Type::String) {
                loadedExact.insert_or_assign(NormalizeLineEndings(k), NormalizeLineEndings(v.strVal));
            }
        }
    }

    ContextMap loadedCtx = mergeOverlay ? std::move(exactContext_) : ContextMap{};
    if (const JsonValue* ctxObj = root.Find("exact_context")) {
        if (ctxObj->type != JsonValue::Type::Object) {
            if (errorOut) *errorOut = "'exact_context' must be a JSON object";
            if (mergeOverlay) {
                exact_ = std::move(loadedExact);
                exactContext_ = std::move(loadedCtx);
            }
            return false;
        }
        for (const auto& [ctxName, inner] : ctxObj->objVal) {
            if (inner.type != JsonValue::Type::Object) continue;
            ExactMap& bucket = loadedCtx[ctxName];
            for (const auto& [k, v] : inner.objVal) {
                if (v.type == JsonValue::Type::String) {
                    bucket.insert_or_assign(NormalizeLineEndings(k), NormalizeLineEndings(v.strVal));
                }
            }
        }
    }

    if (mergeOverlay && (root.Find("templates") || root.Find("regex_rules"))) {
        std::string tplErr;
        if (!templateMatcher_.LoadFromJsonValue(root, tplErr, true)) {
            if (errorOut) *errorOut = tplErr;
            exact_ = std::move(loadedExact);
            exactContext_ = std::move(loadedCtx);
            return false;
        }
    }

    exact_ = std::move(loadedExact);
    exactContext_ = std::move(loadedCtx);
    size_t totalCtx = 0;
    for (const auto& [_, mp] : exactContext_) {
        totalCtx += mp.size();
    }
    stats_.loadedEntries = exact_.size();
    stats_.loadedContextEntries = totalCtx;
    stats_.loadedTemplates = templateMatcher_.TemplateCount();
    stats_.loadedRegexRules = templateMatcher_.RegexCount();
    return true;
}

bool TranslationCore::LoadTemplatesFromJsonFile(const std::wstring& path, std::string* errorOut) {
    std::string bytes;
    if (!ReadWholeFileUtf8(path, bytes, errorOut)) {
        return false;
    }
    return LoadTemplatesFromJsonBytes(bytes, errorOut);
}

bool TranslationCore::LoadTemplatesFromJsonBytes(std::string_view jsonUtf8, std::string* errorOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string err;
    if (!templateMatcher_.LoadFromJsonBytes(jsonUtf8, err)) {
        if (errorOut) *errorOut = err;
        return false;
    }
    stats_.loadedTemplates = templateMatcher_.TemplateCount();
    stats_.loadedRegexRules = templateMatcher_.RegexCount();
    return true;
}

TranslateResult TranslationCore::Translate(std::string_view inputUtf8, std::string_view contextUtf8) {
    TranslateResult res;
    if (inputUtf8.empty()) {
        return res;
    }

    // Avoid heap allocation on 99.9% of inputs containing no '\r'.
    std::string crlfStorage;
    std::string_view keyView = inputUtf8;
    if (inputUtf8.find('\r') != std::string_view::npos) {
        crlfStorage = NormalizeLineEndings(inputUtf8);
        keyView = crlfStorage;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.lookups;

    std::string candidate;
    MatchTier tier = MatchTier::None;
    std::string ruleId;

    auto tryMatchTiers = [&](std::string_view view, std::string& outCandidate,
                             MatchTier& outTier, std::string& outRuleId) -> bool {
        if (!contextUtf8.empty()) {
            const auto ctxIt = exactContext_.find(contextUtf8);
            if (ctxIt != exactContext_.end()) {
                const auto it = ctxIt->second.find(view);
                if (it != ctxIt->second.end()) {
                    outCandidate = it->second;
                    outTier = MatchTier::ExactContext;
                    outRuleId = std::string(contextUtf8);
                    return true;
                }
            }
        }
        const auto exactIt = exact_.find(view);
        if (exactIt != exact_.end()) {
            outCandidate = exactIt->second;
            outTier = MatchTier::Exact;
            outRuleId.clear();
            return true;
        }
        if (templateMatcher_.MatchTemplate(view, outCandidate, outRuleId)) {
            outTier = MatchTier::Template;
            return true;
        }
        if (templateMatcher_.MatchRegex(view, outCandidate, outRuleId)) {
            outTier = MatchTier::WhitelistedRegex;
            return true;
        }
        return false;
    };

    if (!tryMatchTiers(keyView, candidate, tier, ruleId)) {
        // Fallback: strip leading/trailing '\n' and ASCII ' ' padding; preserve outer '\n'.
        size_t head = 0;
        while (head < keyView.size() && (keyView[head] == '\n' || keyView[head] == ' ')) {
            ++head;
        }
        size_t tail = keyView.size();
        while (tail > head && (keyView[tail - 1] == '\n' || keyView[tail - 1] == ' ')) {
            --tail;
        }
        if ((head > 0 || tail < keyView.size()) && head < tail) {
            std::string trimmedCandidate;
            MatchTier trimmedTier = MatchTier::None;
            std::string trimmedRuleId;
            if (tryMatchTiers(keyView.substr(head, tail - head), trimmedCandidate, trimmedTier, trimmedRuleId)) {
                std::string prefixNl;
                for (size_t i = 0; i < head; ++i) {
                    if (keyView[i] == '\n') prefixNl.push_back('\n');
                }
                std::string suffixNl;
                for (size_t i = tail; i < keyView.size(); ++i) {
                    if (keyView[i] == '\n') suffixNl.push_back('\n');
                }
                candidate = prefixNl + std::move(trimmedCandidate) + suffixNl;
                tier = trimmedTier;
                ruleId = std::move(trimmedRuleId);
            }
        }
    }

    // 5. Fallback on miss
    if (tier == MatchTier::None) {
        ++stats_.misses;
        res.outcome = TranslateOutcome::Miss;
        res.tier = MatchTier::None;
        return res;
    }

    // Strict protected tag preservation check before accepting any tier
    const auto tagCheck = ValidateTagPreservation(keyView, candidate);
    if (!tagCheck.valid) {
        ++stats_.tagFallbacks;
        res.outcome = TranslateOutcome::TagMismatchFallback;
        res.tier = tier;
        res.matchedRule = tagCheck.reason;
        res.text.assign(inputUtf8);
        return res;
    }

    ++stats_.hits;
    switch (tier) {
        case MatchTier::Exact: ++stats_.exactHits; break;
        case MatchTier::ExactContext: ++stats_.contextHits; break;
        case MatchTier::Template: ++stats_.templateHits; break;
        case MatchTier::WhitelistedRegex: ++stats_.regexHits; break;
        default: break;
    }

    res.outcome = TranslateOutcome::Hit;
    res.tier = tier;
    res.matchedRule = std::move(ruleId);
    res.text = RestoreLineEndingsIfCrlf(inputUtf8, std::move(candidate));
    return res;
}

TranslationStats TranslationCore::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

}  // namespace aigiskr::core
