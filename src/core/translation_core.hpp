#pragma once

#include "core/tag_parser.hpp"
#include "core/template_matcher.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aigiskr::core {

enum class TranslateOutcome {
    Miss,
    Hit,
    TagMismatchFallback,
};

enum class MatchTier {
    None,
    Exact,
    ExactContext,
    Template,
    WhitelistedRegex,
};

struct TranslationStats {
    size_t loadedEntries = 0;
    size_t loadedContextEntries = 0;
    size_t loadedTemplates = 0;
    size_t loadedRegexRules = 0;
    uint64_t lookups = 0;
    uint64_t hits = 0;
    uint64_t exactHits = 0;
    uint64_t contextHits = 0;
    uint64_t templateHits = 0;
    uint64_t regexHits = 0;
    uint64_t misses = 0;
    uint64_t tagFallbacks = 0;
};

struct TranslateResult {
    TranslateOutcome outcome = TranslateOutcome::Miss;
    MatchTier tier = MatchTier::None;
    std::string text;
    std::string matchedRule;
};

struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

using ExactMap = std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>>;
using ContextMap = std::unordered_map<std::string, ExactMap, TransparentStringHash, std::equal_to<>>;

// Thread-safe 5-tier UTF-8 localization store with placeholder/tag safety fallback.
class TranslationCore {
public:
    void Clear();
    bool LoadFromJsonFile(const std::wstring& path, std::string* errorOut = nullptr);
    bool LoadFromJsonBytes(std::string_view jsonUtf8, std::string* errorOut = nullptr);
    bool LoadOverlayFromJsonFile(const std::wstring& path, std::string* errorOut = nullptr);
    bool LoadOverlayFromJsonBytes(std::string_view jsonUtf8, std::string* errorOut = nullptr);

    bool LoadTemplatesFromJsonFile(const std::wstring& path, std::string* errorOut = nullptr);
    bool LoadTemplatesFromJsonBytes(std::string_view jsonUtf8, std::string* errorOut = nullptr);

    // Hot-path lookup: ExactContext (if context non-empty) -> Exact -> Template -> WhitelistedRegex -> Fallback
    TranslateResult Translate(std::string_view inputUtf8, std::string_view contextUtf8 = {});

    TranslationStats GetStats() const;

    static std::string NormalizeLineEndings(std::string_view input);
    static std::vector<std::string> ExtractProtectedTokens(std::string_view normalizedUtf8);
    static bool TokensMatch(std::string_view srcNormalized, std::string_view dstNormalized);

private:
    mutable std::mutex mutex_;
    ExactMap exact_;
    ContextMap exactContext_;
    TemplateMatcher templateMatcher_;
    TranslationStats stats_;

    bool LoadFromJsonBytesUnlocked(std::string_view jsonUtf8, bool mergeOverlay, std::string* errorOut);
};

}  // namespace aigiskr::core
