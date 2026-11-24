#pragma once

#include "core/config.hpp"
#include "core/coverage.hpp"
#include "core/hdtrans_adapter.hpp"
#include "core/knowledge_layer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace aigiskr::core {

enum class AutoLookupOutcome {
    Disabled,
    CacheHit,
    Queued,
    AlreadyPending,
    Filtered,
    PreviouslyFailed,
    QueueFull,
    BackendUnavailable,
};

struct CandidateFilterResult {
    bool accepted = false;
    const char* reason = "unknown";
};

struct AutoTranslateStats {
    bool enabled = false;
    bool backendAvailable = false;
    bool backendInitialized = false;
    std::string engineVersion;
    std::string cachePathUtf8;

    size_t overridesLoaded = 0;
    size_t seedCacheLoaded = 0;
    size_t runtimeCacheLoaded = 0;
    size_t malformedCacheLines = 0;
    size_t rejectedCacheLines = 0;
    size_t currentCacheEntries = 0;

    uint64_t overrideHits = 0;
    uint64_t cacheHits = 0;
    uint64_t cacheMisses = 0;
    uint64_t filteredOut = 0;
    uint64_t jobsQueued = 0;
    uint64_t jobsCompleted = 0;
    uint64_t jobsFailed = 0;
    uint64_t jobsDeduplicated = 0;
    uint64_t jobsDroppedQueueFull = 0;
    uint64_t tagRejected = 0;
    uint64_t renderThreadBackendCalls = 0;

    double initMs = 0.0;
    double avgTranslateMs = 0.0;
    double maxTranslateMs = 0.0;

    bool knowledgeLoaded = false;
    std::string knowledgeVersion;
    size_t knowledgeExactOverrides = 0;
    size_t knowledgeProperNouns = 0;
    size_t knowledgePreRules = 0;
    size_t knowledgePostRules = 0;
    size_t staleCacheSkipped = 0;
    uint64_t klExactHits = 0;
    uint64_t klPreRewrites = 0;
    uint64_t klPostRewrites = 0;
    uint64_t klSentinelsRestored = 0;
    uint64_t klParticleFixes = 0;

    std::string hostMode = "Auto";
    bool hostSpawned = false;
    uint64_t hostSpawnCount = 0;
    uint64_t hostRestartCount = 0;
    uint16_t hostProtocolVer = 0;
    std::string hostVersion;
    std::string hostEngineVer;
    std::string hostEngineRootType = "none";
    uint64_t ipcRequests = 0;
    uint64_t ipcCompleted = 0;
    uint64_t ipcFailed = 0;
    uint64_t ipcTimeout = 0;
    int64_t hostExitCode = -1;
    uint64_t renderThreadHostIpcCalls = 0;
};

/// Evaluate whether a normalized UTF-8 string (that already missed overrides/curated DB)
/// is safe and eligible for local runtime auto-translation.
CandidateFilterResult EvaluateAutoTranslateCandidate(std::string_view normalizedSource,
                                                     uint32_t callerRva,
                                                     std::string_view screenTag,
                                                     const AutoTranslateConfig& cfg,
                                                     const CoverageTracker* coverageTracker = nullptr);

class AutoTranslateManager {
public:
    AutoTranslateManager() = default;
    ~AutoTranslateManager();

    AutoTranslateManager(const AutoTranslateManager&) = delete;
    AutoTranslateManager& operator=(const AutoTranslateManager&) = delete;

    /// Inject a custom backend (for deterministic unit tests); defaults to HdTransJ2KAdapter.
    void SetBackendForTesting(std::unique_ptr<IAutoTranslateBackend> backend);

    /// Load optional overrides, seed cache, and persistent cache, and start single background worker.
    /// Never blocks the caller on dictionary initialization or translation.
    bool Initialize(const AutoTranslateConfig& cfg, const CoverageTracker* coverageTracker = nullptr);

    /// Gracefully stop worker thread and release backend cleanly.
    /// Wait/free semantics match DLL_PROCESS_DETACH safety: pass allowModuleUnload=false on exit detach.
    void Shutdown(bool allowModuleUnload = true);

    /// Tier 1 lookup (before Curated Exact/Template/Regex): check optional manual overrides.
    bool LookupOverride(std::string_view normalizedSource,
                        std::string_view contextName,
                        std::string& outTranslated) const;

    /// Tier 3 lookup (after Curated miss): O(1) memory cache lookup, or enqueue async job.
    /// Never runs backend translation or disk I/O on the calling thread.
    AutoLookupOutcome LookupCacheOrQueue(std::string_view normalizedSource,
                                         uint32_t callerRva,
                                         std::string_view screenTag,
                                         std::string& outTranslated);

    /// Wait until background queue drains (unit test helper only; never call on render thread).
    bool WaitForIdleForTesting(uint32_t timeoutMs = 5000);

    AutoTranslateStats GetStats() const;

    // Completion notification for UI hooks. No backend, enqueue, stats, or disk access.
    uint64_t CompletionRevision() const noexcept { return completionRevision_.load(std::memory_order_acquire); }
    bool LookupCompleted(std::string_view source, std::string& out) const;

private:
    struct PendingJob {
        std::string normalizedSource;
        uint32_t callerRva = 0;
        std::string screenTag;
    };

    void StopInternal(bool allowModuleUnload, bool destroyBackend);
    void LoadOverridesUnlocked(const std::wstring& path);
    void LoadJsonlCacheUnlocked(const std::wstring& path, bool isSeed);
    bool AppendJsonlRecord(const PendingJob& job,
                           std::string_view translated,
                           std::string_view engineVer,
                           std::string_view klVer) const;
    void WorkerLoop();

    AutoTranslateConfig cfg_;
    const CoverageTracker* coverageTracker_ = nullptr;

    std::unique_ptr<IAutoTranslateBackend> backend_;
    mutable std::mutex mutex_;
    mutable std::mutex diskMutex_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;

    std::unordered_map<std::string, std::string> exactOverrides_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> contextOverrides_;
    std::unordered_map<std::string, std::string> cache_;
    std::unordered_map<std::string, std::string> cacheKlVer_;
    KnowledgeLayer knowledge_;
    std::unordered_set<std::string> pendingKeys_;
    std::unordered_set<std::string> failedKeys_;
    std::deque<PendingJob> queue_;

    std::thread worker_;
    std::thread::id workerThreadId_;
    bool activeJob_ = false;
    bool initialized_ = false;
    bool workerRunning_ = false;
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint64_t> completionRevision_{0};
    bool backendAvailable_ = false;
    bool backendInitDone_ = false;
    bool backendInitOk_ = false;
    bool customBackendInjected_ = false;
    std::wstring resolvedEngineRoot_;
    std::string hostEngineRootType_ = "none";
    HostTelemetrySnapshot cachedHostTelemetry_;
    uint64_t hostRestartCount_ = 0;
    uint64_t renderThreadHostIpcCalls_ = 0;
    std::string engineVersion_ = "hdTrans";

    size_t overridesLoaded_ = 0;
    size_t seedCacheLoaded_ = 0;
    size_t runtimeCacheLoaded_ = 0;
    size_t malformedCacheLines_ = 0;
    size_t rejectedCacheLines_ = 0;

    mutable uint64_t overrideHits_ = 0;
    uint64_t cacheHits_ = 0;
    uint64_t cacheMisses_ = 0;
    uint64_t filteredOut_ = 0;
    uint64_t jobsQueued_ = 0;
    uint64_t jobsCompleted_ = 0;
    uint64_t jobsFailed_ = 0;
    uint64_t jobsDeduplicated_ = 0;
    uint64_t jobsDroppedQueueFull_ = 0;
    uint64_t tagRejected_ = 0;
    uint64_t renderThreadBackendCalls_ = 0;

    double initMs_ = 0.0;
    double totalTranslateMs_ = 0.0;
    double maxTranslateMs_ = 0.0;
    size_t staleCacheSkipped_ = 0;
    uint64_t klExactHits_ = 0;
    uint64_t klPreRewrites_ = 0;
    uint64_t klPostRewrites_ = 0;
    uint64_t klSentinelsRestored_ = 0;
    uint64_t klParticleFixes_ = 0;
    uint32_t tagLogCount_ = 0;
    uint32_t sampleLogCount_ = 0;
};

}  // namespace aigiskr::core
