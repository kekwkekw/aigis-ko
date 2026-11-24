#pragma once

#include <cstddef>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aigiskr::core {

struct KnowledgeStats {
    bool loaded = false;
    std::string version;
    size_t exactOverrides = 0;
    size_t properNouns = 0;
    size_t preRules = 0;
    size_t postRules = 0;
};

struct PreProcessResult {
    struct SentinelBinding {
        std::wstring sentinelWide;
        std::wstring koWide;
        bool fixParticle = true;
        bool spaceBeforeNonParticle = false;
    };

    struct DeferredPostRewrite {
        std::wstring matchWide;
        std::wstring replaceWide;
    };

    std::string text;
    std::vector<SentinelBinding> sentinels;
    std::vector<DeferredPostRewrite> deferredRewrites;
    uint32_t preRulesApplied = 0;
    uint32_t properNounsMasked = 0;
};

struct PostProcessResult {
    std::string text;
    uint32_t sentinelsRestored = 0;
    uint32_t particleFixes = 0;
    uint32_t postRulesApplied = 0;
};

/// Data-driven Aigis domain Knowledge Layer executed strictly in the worker thread
/// or offline seeding CLI (never on the D3D/text render thread).
class KnowledgeLayer {
public:
    KnowledgeLayer() = default;

    /// Load manifest and knowledge rule JSON files from dirPath.
    /// Gracefully stays unloaded (pass-through) if dirPath does not exist.
    bool LoadFromDirectory(const std::wstring& dirPath, std::string* errorOut = nullptr);

    void Clear();

    bool IsLoaded() const noexcept { return loaded_; }
    const std::string& Version() const noexcept { return version_; }
    KnowledgeStats GetStats() const;

    /// Lookup exact KL override (evaluated after manual/curated TranslationCore,
    /// before invoking J2KEngine).
    bool LookupExactOverride(std::string_view normalizedSource, std::string& outTranslated) const;

    /// Apply PreRules + ProperNoun sentinel masking on a tag-masked UTF-8 string.
    PreProcessResult PreProcess(std::string_view tagMaskedSource, std::string_view screenTag = {}) const;

    /// Restore ProperNoun sentinels (with Hangul batchim particle agreement) and
    /// apply PostRules on the tag-restored Korean string.
    PostProcessResult PostProcess(std::string_view translatedWithTags,
                                  const PreProcessResult& preInfo,
                                  std::string_view screenTag = {}) const;

private:
    struct GuardCondition {
        std::vector<std::wstring> notPrecededBy;
        std::vector<std::wstring> notFollowedBy;
        std::vector<std::wstring> requireContextContains;
        std::vector<std::wstring> excludeIfContains;
        std::vector<std::string> domainScopes;
    };

    struct ProperNounRule {
        std::string id;
        std::wstring jpWide;
        std::wstring koWide;
        int priority = 0;
        bool useSentinel = true;
        bool fixParticle = true;
        bool spaceBeforeNonParticle = false;
        bool spaceInPreBeforeKatakana = false;
        std::wstring deferToPostOnKatakana;
        GuardCondition guards;
    };

    struct RewriteRule {
        std::string id;
        bool isRegex = false;
        int priority = 0;
        std::wstring patternWide;
        std::wstring replaceWide;
        std::wregex compiledRegex;
        bool regexValid = false;
        GuardCondition guards;
    };

    bool LoadExactOverridesFile(const std::wstring& path, std::string* errOut);
    bool LoadProperNounsFile(const std::wstring& path, std::string* errOut);
    bool LoadRewriteRulesFile(const std::wstring& path, bool isPost, std::string* errOut);

    bool loaded_ = false;
    std::string version_;
    std::unordered_map<std::string, std::string> exactOverrides_;
    std::vector<ProperNounRule> properNouns_;
    std::vector<RewriteRule> preRules_;
    std::vector<RewriteRule> postRules_;
};

}  // namespace aigiskr::core
