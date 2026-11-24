#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aigiskr::core {

enum class CoverageBucket {
    Exact,
    Context,
    Template,
    Regex,
    Unmatched,
    Ignored,
    TagRejected,
};

enum class IgnoredSubbucket : uint8_t {
    Numeric = 0,
    AsciiCounter,
    AlreadyKorean,
    InternalPlaceholder,
    NonVisible,
};

enum class UnmatchedSubbucket : uint8_t {
    StaticVisible = 0,
    ServerDynamic,
    AssetText,
};

struct CoverageEntryStats {
    uint64_t calls = 0;
    std::string ruleOrReason;
    std::string shapeKey;
    uint32_t firstCallerRva = 0;
    uint8_t subbucket = 0;
    std::vector<uint32_t> callerRvas;  // capped small list
    std::vector<std::string> screens;  // distinct observed screens
};

struct CoverageSummary {
    uint64_t exactCalls = 0;
    uint64_t contextCalls = 0;
    uint64_t templateCalls = 0;
    uint64_t regexCalls = 0;
    uint64_t unmatchedCalls = 0;
    uint64_t ignoredCalls = 0;
    uint64_t tagRejectedCalls = 0;

    size_t exactUnique = 0;
    size_t contextUnique = 0;
    size_t templateUnique = 0;
    size_t regexUnique = 0;
    size_t unmatchedUnique = 0;
    size_t ignoredUnique = 0;
    size_t tagRejectedUnique = 0;

    // R2 subbucket telemetry
    uint64_t ignoredNumericCalls = 0;
    uint64_t ignoredAsciiCounterCalls = 0;
    uint64_t ignoredAlreadyKoreanCalls = 0;
    uint64_t ignoredInternalPlaceholderCalls = 0;
    uint64_t ignoredNonVisibleCalls = 0;
    size_t ignoredNumericUnique = 0;
    size_t ignoredAsciiCounterUnique = 0;
    size_t ignoredAlreadyKoreanUnique = 0;
    size_t ignoredInternalPlaceholderUnique = 0;
    size_t ignoredNonVisibleUnique = 0;

    uint64_t unmatchedStaticVisibleCalls = 0;
    uint64_t unmatchedServerDynamicCalls = 0;
    uint64_t unmatchedAssetTextCalls = 0;
    size_t unmatchedStaticVisibleUnique = 0;
    size_t unmatchedServerDynamicUnique = 0;
    size_t unmatchedAssetTextUnique = 0;

    uint64_t noticeTranslatedCalls = 0;
    size_t noticeTranslatedUnique = 0;
    size_t coreStaticTranslatedUnique = 0;

    uint64_t translatedCalls() const noexcept {
        return exactCalls + contextCalls + templateCalls + regexCalls;
    }
    uint64_t translatableCalls() const noexcept {
        return translatedCalls() + unmatchedCalls + tagRejectedCalls;
    }
    size_t translatedUnique() const noexcept {
        return exactUnique + contextUnique + templateUnique + regexUnique;
    }
    size_t translatableUnique() const noexcept {
        return translatedUnique() + unmatchedUnique + tagRejectedUnique;
    }

    uint64_t coreStaticTranslatedCalls() const noexcept {
        const uint64_t total = translatedCalls();
        return (total >= noticeTranslatedCalls) ? (total - noticeTranslatedCalls) : 0;
    }
    uint64_t coreStaticTranslatableCalls() const noexcept {
        return coreStaticTranslatedCalls() + unmatchedStaticVisibleCalls + unmatchedAssetTextCalls + tagRejectedCalls;
    }
    uint64_t coreStaticUiOnlyTranslatableCalls() const noexcept {
        return coreStaticTranslatedCalls() + unmatchedStaticVisibleCalls + tagRejectedCalls;
    }
    size_t coreStaticTranslatableUnique() const noexcept {
        return coreStaticTranslatedUnique + unmatchedStaticVisibleUnique + unmatchedAssetTextUnique + tagRejectedUnique;
    }
    uint64_t noticeTranslatableCalls() const noexcept {
        return noticeTranslatedCalls + unmatchedServerDynamicCalls;
    }
    size_t noticeTranslatableUnique() const noexcept {
        return noticeTranslatedUnique + unmatchedServerDynamicUnique;
    }
};

// Returns true only when UTF-8 input has zero Japanese characters (e.g., ASCII numbers/labels/empty/already-Korean).
bool ShouldIgnoreForCoverage(std::string_view utf8);

// Classifies built-in (non-placeholder-list) ignored strings into concrete sub-reasons.
IgnoredSubbucket ClassifyBuiltinIgnoredReason(std::string_view utf8) noexcept;

// Redacts high-entropy hex/session/user tokens while keeping UI structure legible.
std::string RedactSensitiveTokens(std::string_view utf8);

// Replaces ASCII digit runs [0-9]+ with "{n}" for dynamic pattern grouping.
std::string ComputeShapeKey(std::string_view utf8);

const char* CoverageBucketName(CoverageBucket bucket) noexcept;
const char* IgnoredSubbucketName(IgnoredSubbucket sub) noexcept;
const char* UnmatchedSubbucketName(UnmatchedSubbucket sub) noexcept;

class CoverageTracker {
public:
    void Configure(bool enabled, std::string outputDir);
    bool LoadClassificationRulesFromJsonFile(const std::wstring& path, std::string* errorOut = nullptr);
    bool LoadClassificationRulesFromJsonBytes(std::string_view jsonUtf8, std::string* errorOut = nullptr);
    size_t PlaceholderRuleCount() const;

    bool IsIgnoredText(std::string_view normalizedSource) const;
    IgnoredSubbucket ClassifyIgnored(std::string_view normalizedSource) const;
    UnmatchedSubbucket ClassifyUnmatched(uint32_t callerRva) const;
    bool IsServerDynamicRva(uint32_t callerRva) const;

    void SetCurrentScreen(std::string_view screenName);
    std::string GetCurrentScreen() const;
    void Reset();

    // Thread-safe hot-path recording: no disk I/O inside Record().
    void Record(std::string_view normalizedSource,
                CoverageBucket bucket,
                std::string_view ruleOrReason = {},
                uint32_t callerRva = 0,
                std::string_view explicitScreen = {});

    CoverageSummary SnapshotSummary() const;

    // Flushes aggregated session JSON + unmatched TSV to disk. Safe to call on shutdown or interval.
    bool FlushToDisk(std::string* outSessionJsonPath = nullptr,
                     std::string* outUnmatchedTsvPath = nullptr);

private:
    static constexpr size_t kMaxUniquePerMap = 16384;

    mutable std::mutex mutex_;
    bool enabled_ = true;
    std::string outputDir_ = "coverage";
    std::string currentScreen_ = "Startup";
    std::string sessionStamp_;

    uint64_t exactCalls_ = 0;
    uint64_t contextCalls_ = 0;
    uint64_t templateCalls_ = 0;
    uint64_t regexCalls_ = 0;
    uint64_t unmatchedCalls_ = 0;
    uint64_t ignoredCalls_ = 0;
    uint64_t tagRejectedCalls_ = 0;
    uint64_t mapCapDropped_ = 0;

    uint64_t ignoredNumericCalls_ = 0;
    uint64_t ignoredAsciiCounterCalls_ = 0;
    uint64_t ignoredAlreadyKoreanCalls_ = 0;
    uint64_t ignoredInternalPlaceholderCalls_ = 0;
    uint64_t ignoredNonVisibleCalls_ = 0;

    uint64_t unmatchedStaticVisibleCalls_ = 0;
    uint64_t unmatchedServerDynamicCalls_ = 0;
    uint64_t unmatchedAssetTextCalls_ = 0;
    uint64_t noticeTranslatedCalls_ = 0;

    std::unordered_map<std::string, CoverageEntryStats> exactMap_;
    std::unordered_map<std::string, CoverageEntryStats> contextMap_;
    std::unordered_map<std::string, CoverageEntryStats> templateMap_;
    std::unordered_map<std::string, CoverageEntryStats> regexMap_;
    std::unordered_map<std::string, CoverageEntryStats> unmatchedMap_;
    std::unordered_map<std::string, CoverageEntryStats> ignoredMap_;
    std::unordered_map<std::string, CoverageEntryStats> tagRejectedMap_;

    // R2 classification rules (preserved across Reset()).
    std::unordered_map<std::string, std::string> placeholderMap_;
    std::unordered_map<uint32_t, bool> serverDynamicRvas_{{0x283b73u, true}};
    std::unordered_map<uint32_t, bool> assetTextRvas_{{0x6e399du, true}, {0x63d61du, true}};
    std::unordered_map<std::string, uint8_t> noticeTranslatedKeys_;
    std::unordered_map<std::string, uint8_t> coreStaticTranslatedKeys_;

    struct ScreenCounters {
        uint64_t translated = 0;
        uint64_t unmatched = 0;
        uint64_t ignored = 0;
        uint64_t tagRejected = 0;
    };
    std::unordered_map<std::string, ScreenCounters> perScreen_;

    IgnoredSubbucket ClassifyIgnoredUnlocked(std::string_view normalizedSource) const;
    UnmatchedSubbucket ClassifyUnmatchedUnlocked(uint32_t callerRva) const;
    CoverageSummary BuildSummaryUnlocked() const;
    std::unordered_map<std::string, CoverageEntryStats>& SelectMap(CoverageBucket b);
    static void AddEntryStats(std::unordered_map<std::string, CoverageEntryStats>& mp,
                              const std::string& key,
                              std::string_view ruleOrReason,
                              uint32_t callerRva,
                              const std::string& screen,
                              uint8_t subbucket,
                              uint64_t& dropCounter);
};

}  // namespace aigiskr::core
