#include "core/auto_translate.hpp"

#include "core/hdtrans_host_client.hpp"
#include "core/logger.hpp"
#include "core/mem_safe.hpp"
#include "core/mini_json.hpp"
#include "core/tag_parser.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aigiskr::core {

namespace {

bool DecodeUtf8Step(std::string_view s, size_t& pos, uint32_t& cp) noexcept {
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

bool IsKanaCp(uint32_t cp) noexcept {
    return (cp >= 0x3040u && cp <= 0x309Fu) ||  // Hiragana
           (cp >= 0x30A0u && cp <= 0x30FFu) ||  // Katakana + prolonged sound mark U+30FC
           (cp >= 0x31F0u && cp <= 0x31FFu) ||  // Katakana Phonetic Extensions
           (cp >= 0xFF65u && cp <= 0xFF9Fu);    // Halfwidth Katakana
}

bool IsKanjiCp(uint32_t cp) noexcept {
    return (cp >= 0x4E00u && cp <= 0x9FFFu) ||  // CJK Unified Ideographs
           (cp >= 0x3400u && cp <= 0x4DBFu) ||  // CJK Extension A
           cp == 0x3005u || cp == 0x3006u;      // Iteration mark / Shime
}

bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t k = 0; k < needle.size(); ++k) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + k])) !=
                std::tolower(static_cast<unsigned char>(needle[k]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool IsStoryTextOrigin(uint32_t callerRva, std::string_view screenTag) noexcept {
    if (callerRva == 0xAD6C0u || callerRva == 0x6e399du || callerRva == 0x63d61du) {
        return true;
    }
    return ContainsCaseInsensitive(screenTag, "story") ||
           ContainsCaseInsensitive(screenTag, "adv") ||
           ContainsCaseInsensitive(screenTag, "dialogue") ||
           ContainsCaseInsensitive(screenTag, "novel");
}

bool LooksLikeInternalOrUrlToken(std::string_view s) noexcept {
    if (s.find("__AIGIS_TAG_") != std::string_view::npos) return true;
    if (ContainsCaseInsensitive(s, "http://") || ContainsCaseInsensitive(s, "https://")) return true;
    if (ContainsCaseInsensitive(s, "token=") || ContainsCaseInsensitive(s, "session=") ||
        ContainsCaseInsensitive(s, "sid=") || ContainsCaseInsensitive(s, "uid=")) {
        return true;
    }
    if (s.rfind("<NONPTR>", 0) == 0 || s.rfind("<OBJ:", 0) == 0 ||
        s.rfind("<SCALAR:", 0) == 0 || s.rfind("<PTR:", 0) == 0 ||
        s.rfind("<UNREADABLE", 0) == 0) {
        return true;
    }
    return false;
}

size_t CountUtf8Codepoints(std::string_view s) noexcept {
    size_t count = 0;
    size_t pos = 0;
    uint32_t cp = 0;
    while (DecodeUtf8Step(s, pos, cp)) {
        ++count;
    }
    return count;
}

std::string MakeSingleLinePreview(std::string_view s, size_t maxBytes = 80) {
    std::string out;
    out.reserve((std::min)(s.size(), maxBytes));
    for (size_t i = 0; i < s.size() && out.size() < maxBytes; ++i) {
        const char c = s[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    if (s.size() > maxBytes) {
        out.append("...");
    }
    return out;
}

}  // namespace

CandidateFilterResult EvaluateAutoTranslateCandidate(std::string_view normalizedSource,
                                                     uint32_t callerRva,
                                                     std::string_view screenTag,
                                                     const AutoTranslateConfig& cfg,
                                                     const CoverageTracker* coverageTracker) {
    if (normalizedSource.empty()) {
        return {false, "empty"};
    }
    if (LooksLikeInternalOrUrlToken(normalizedSource)) {
        return {false, "internal_or_url"};
    }
    // Coverage labels describe telemetry; they must not veto translation.
    (void)coverageTracker;

    const bool isStory = IsStoryTextOrigin(callerRva, screenTag);
    if (isStory && !cfg.translateStory) {
        return {false, "story_disabled"};
    }
    if (!isStory && !cfg.translateUI) {
        return {false, "ui_disabled"};
    }

    const size_t totalCp = CountUtf8Codepoints(normalizedSource);
    if (totalCp < cfg.minLength || normalizedSource.size() > kMaxNativeTextBytes ||
        (cfg.maxLength && normalizedSource.size() > cfg.maxLength)) {
        return {false, "length_out_of_bounds"};
    }

    // Inspect strictly non-tag text spans so pure tags like "<TIME>" or "%c[ff0000]100"
    // or numeric/ASCII strings are never sent to hdTrans.
    const auto spans = ExtractProtectedTagSpans(normalizedSource);
    bool hasNonWs = false;
    bool hasKana = false;
    bool hasKanji = false;

    auto scanSegment = [&](std::string_view seg) {
        size_t pos = 0;
        uint32_t cp = 0;
        while (DecodeUtf8Step(seg, pos, cp)) {
            if (cp != ' ' && cp != '\t' && cp != '\n' && cp != '\r' && cp != 0x3000u) {
                hasNonWs = true;
            }
            if (IsKanaCp(cp)) {
                hasKana = true;
            } else if (IsKanjiCp(cp)) {
                hasKanji = true;

            }
        }
    };

    size_t cursor = 0;
    for (const auto& sp : spans) {
        if (sp.offset > cursor) {
            scanSegment(normalizedSource.substr(cursor, sp.offset - cursor));
        }
        cursor = sp.offset + sp.length;
    }
    if (cursor < normalizedSource.size()) {
        scanSegment(normalizedSource.substr(cursor));
    }

    if (!hasNonWs) {
        return {false, "tags_or_whitespace_only"};
    }
    if (!hasKana && !hasKanji) {
        return {false, "no_japanese_glyphs"};
    }

    return {true, "ok"};
}

AutoTranslateManager::~AutoTranslateManager() {
    Shutdown(true);
}

void AutoTranslateManager::SetBackendForTesting(std::unique_ptr<IAutoTranslateBackend> backend) {
    std::lock_guard<std::mutex> lock(mutex_);
    customBackendInjected_ = (backend != nullptr);
    backend_ = std::move(backend);
}

bool AutoTranslateManager::Initialize(const AutoTranslateConfig& cfg,
                                      const CoverageTracker* coverageTracker) {
    StopInternal(true, false);

    std::lock_guard<std::mutex> lock(mutex_);
    cfg_ = cfg;
    coverageTracker_ = coverageTracker;
    exactOverrides_.clear();
    contextOverrides_.clear();
    cache_.clear();
    completionRevision_.store(0, std::memory_order_relaxed);
    cacheKlVer_.clear();
    knowledge_.Clear();
    pendingKeys_.clear();
    failedKeys_.clear();
    queue_.clear();

    overridesLoaded_ = 0;
    seedCacheLoaded_ = 0;
    staleCacheSkipped_ = 0;
    klExactHits_ = 0;
    klPreRewrites_ = 0;
    klPostRewrites_ = 0;
    klSentinelsRestored_ = 0;
    klParticleFixes_ = 0;
    runtimeCacheLoaded_ = 0;
    malformedCacheLines_ = 0;
    rejectedCacheLines_ = 0;
    overrideHits_ = 0;
    cacheHits_ = 0;
    cacheMisses_ = 0;
    filteredOut_ = 0;
    jobsQueued_ = 0;
    jobsCompleted_ = 0;
    jobsFailed_ = 0;
    jobsDeduplicated_ = 0;
    jobsDroppedQueueFull_ = 0;
    tagRejected_ = 0;
    renderThreadBackendCalls_ = 0;
    renderThreadHostIpcCalls_ = 0;
    hostRestartCount_ = 0;
    initMs_ = 0.0;
    totalTranslateMs_ = 0.0;
    maxTranslateMs_ = 0.0;
    tagLogCount_ = 0;
    sampleLogCount_ = 0;
    activeJob_ = false;
    stopRequested_ = false;
    workerRunning_ = false;
    backendAvailable_ = false;
    backendInitDone_ = false;
    backendInitOk_ = false;
    resolvedEngineRoot_.clear();
    hostEngineRootType_ = "none";
    cachedHostTelemetry_ = {};
    engineVersion_ = cfg_.provider.empty() ? "hdTrans" : cfg_.provider;

    if (!cfg_.enabled) {
        initialized_ = true;
        return true;
    }

    std::string probeDetail;
    if (customBackendInjected_ && backend_) {
        resolvedEngineRoot_ = !cfg_.hdTransPath.empty() ? cfg_.hdTransPath : cfg_.bundledHdTransPath;
        hostEngineRootType_ = (cfg_.hdTransMode == HdTransMode::Bundled ||
                               (cfg_.hdTransMode == HdTransMode::Auto && cfg_.hdTransPath.empty()))
                                  ? "bundled"
                                  : "external";
        backendAvailable_ = backend_->ProbeAvailability(resolvedEngineRoot_, &probeDetail);
    } else {
        auto client = std::make_unique<HdTransHostClient>();
        const HdTransSelectionResult sel = ResolveHdTransSelection(cfg_);
        if (!sel.warning.empty()) {
            Logger::Instance().Logf("[AUTO] Warning: %s", sel.warning.c_str());
        }
        hostEngineRootType_ = sel.rootType;
        resolvedEngineRoot_ = sel.resolvedEngineRoot;
        client->Configure(cfg_.hdTransHostPath, sel.rootType, cfg_.hostTimeoutMs);
        backend_ = std::move(client);
        backendAvailable_ = sel.available;
        probeDetail = sel.detail;
    }

    LoadOverridesUnlocked(cfg_.overridesFile);
    if (!cfg_.knowledgeDir.empty()) {
        std::string klErr;
        if (knowledge_.LoadFromDirectory(cfg_.knowledgeDir, &klErr)) {
            const auto kst = knowledge_.GetStats();
            Logger::Instance().Logf(
                "[KNOWLEDGE] Loaded Aigis Knowledge Layer ver=%s exact=%zu proper_nouns=%zu pre_rules=%zu post_rules=%zu",
                kst.version.c_str(),
                kst.exactOverrides,
                kst.properNouns,
                kst.preRules,
                kst.postRules);
        } else if (!klErr.empty()) {
            Logger::Instance().Logf("[KNOWLEDGE] Knowledge Layer not loaded (%s) -- raw hdTrans mode", klErr.c_str());
        }
    }
    if (cfg_.cacheEnabled) {
        LoadJsonlCacheUnlocked(cfg_.seedCacheFile, true);
        LoadJsonlCacheUnlocked(cfg_.cacheFile, false);
    }

    if (!backendAvailable_) {
        Logger::Instance().Logf(
            "[AUTO] hdTrans host unavailable mode=%s (%s) -- auto-queue disabled, curated + loaded cache remain active",
            HdTransModeName(cfg_.hdTransMode),
            probeDetail.c_str());
    } else {
        workerRunning_ = true;
        worker_ = std::thread(&AutoTranslateManager::WorkerLoop, this);
    }

    Logger::Instance().Logf(
        "[AUTO] Ready enabled=1 provider=%s host_mode=%s root_type=%s backend=%s overrides=%zu seed_cache=%zu runtime_cache=%zu total_cache=%zu malformed=%zu",
        cfg_.provider.c_str(),
        HdTransModeName(cfg_.hdTransMode),
        hostEngineRootType_.c_str(),
        backendAvailable_ ? "ready(lazy)" : "unavailable",
        overridesLoaded_,
        seedCacheLoaded_,
        runtimeCacheLoaded_,
        cache_.size(),
        malformedCacheLines_);

    initialized_ = true;
    return true;
}

void AutoTranslateManager::Shutdown(bool allowModuleUnload) {
    StopInternal(allowModuleUnload, allowModuleUnload);
}

void AutoTranslateManager::StopInternal(bool allowModuleUnload, bool destroyBackend) {
    bool wasRunning = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wasRunning = workerRunning_ && worker_.joinable();
        stopRequested_ = true;
        queue_.clear();
        pendingKeys_.clear();
    }
    cv_.notify_all();

    if (wasRunning) {
        if (allowModuleUnload) {
            worker_.join();
        } else {
            // Wait briefly for kernel exit cleanup without blocking indefinitely under loader lock.
            HANDLE th = reinterpret_cast<HANDLE>(worker_.native_handle());
            if (th && WaitForSingleObject(th, 50) == WAIT_OBJECT_0) {
                worker_.join();
            } else {
                worker_.detach();
            }
        }
    }

    if (backend_) {
        backend_->Shutdown(allowModuleUnload);
        const HostTelemetrySnapshot snap = backend_->GetHostTelemetry();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (snap.isHostBackend) {
                cachedHostTelemetry_ = snap;
            }
        }
        if (!allowModuleUnload) {
            // Leak intentionally on process exit detach after disabling pointers cleanly.
            backend_.release();
        } else if (destroyBackend) {
            backend_.reset();
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    workerRunning_ = false;
    activeJob_ = false;
}

void AutoTranslateManager::LoadOverridesUnlocked(const std::wstring& path) {
    if (path.empty()) {
        return;
    }
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return;
    }

    JsonValue root;
    std::string err;
    if (!ParseJsonFile(WideToUtf8(path), root, err) || root.type != JsonValue::Type::Object) {
        Logger::Instance().Logf("[AUTO] Warning: failed to parse overrides file '%s' (%s)",
                                WideToUtf8(path).c_str(), err.c_str());
        return;
    }

    auto ingestExactObj = [&](const JsonValue* obj) {
        if (!obj || obj->type != JsonValue::Type::Object) return;
        for (const auto& kv : obj->objVal) {
            if (kv.second.type != JsonValue::Type::String) continue;
            const std::string src = NormalizeLineEndings(kv.first);
            const std::string dst = NormalizeLineEndings(kv.second.strVal);
            if (src.empty() || dst.empty()) continue;
            exactOverrides_[src] = dst;
            ++overridesLoaded_;
        }
    };

    if (const auto* ctxObj = root.Find("context")) {
        if (ctxObj->type == JsonValue::Type::Object) {
            for (const auto& ctxKv : ctxObj->objVal) {
                if (ctxKv.second.type != JsonValue::Type::Object) continue;
                auto& bucket = contextOverrides_[ctxKv.first];
                for (const auto& kv : ctxKv.second.objVal) {
                    if (kv.second.type != JsonValue::Type::String) continue;
                    const std::string src = NormalizeLineEndings(kv.first);
                    const std::string dst = NormalizeLineEndings(kv.second.strVal);
                    if (src.empty() || dst.empty()) continue;
                    bucket[src] = dst;
                    ++overridesLoaded_;
                }
            }
        }
    }

    if (const auto* exactObj = root.Find("exact")) {
        ingestExactObj(exactObj);
    } else if (const auto* entriesObj = root.Find("entries")) {
        ingestExactObj(entriesObj);
    } else if (!root.Find("context")) {
        ingestExactObj(&root);
    }
}

void AutoTranslateManager::LoadJsonlCacheUnlocked(const std::wstring& path, bool isSeed) {
    if (path.empty()) {
        return;
    }
    std::ifstream ifs(std::filesystem::path(path), std::ios::binary);
    if (!ifs.is_open()) {
        return;
    }

    std::string line;
    size_t lineNo = 0;
    size_t warnCount = 0;
    while (std::getline(ifs, line)) {
        ++lineNo;
        if (lineNo == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        JsonValue obj;
        std::string parseErr;
        if (!ParseJson(line, obj, parseErr) || obj.type != JsonValue::Type::Object) {
            ++malformedCacheLines_;
            if (warnCount < 5) {
                ++warnCount;
                Logger::Instance().Logf(
                    "[AUTO_CACHE] Skipping malformed JSONL line %zu in '%s' (%s)",
                    lineNo, WideToUtf8(path).c_str(), parseErr.c_str());
            }
            continue;
        }

        const auto* srcVal = obj.Find("src");
        const auto* dstVal = obj.Find("dst");
        if (!srcVal || srcVal->type != JsonValue::Type::String ||
            !dstVal || dstVal->type != JsonValue::Type::String) {
            ++malformedCacheLines_;
            continue;
        }

        const std::string src = NormalizeLineEndings(srcVal->strVal);
        const std::string dst = NormalizeLineEndings(dstVal->strVal);
        if (src.empty() || dst.empty()) {
            ++malformedCacheLines_;
            continue;
        }

        const auto tagCheck = ValidateTagPreservation(src, dst);
        if (!tagCheck.valid) {
            ++rejectedCacheLines_;
            continue;
        }

        std::string rowKlVer;
        if (const auto* klVal = obj.Find("kl_ver"); klVal && klVal->type == JsonValue::Type::String) {
            rowKlVer = klVal->strVal;
        }

        const std::string expectedKlVer = knowledge_.IsLoaded() ? knowledge_.Version() : std::string();
        if (cfg_.retranslateStale && !expectedKlVer.empty() && rowKlVer != expectedKlVer) {
            ++staleCacheSkipped_;
            continue;
        }

        const auto existingIt = cache_.find(src);
        if (existingIt != cache_.end() && !expectedKlVer.empty()) {
            const auto verIt = cacheKlVer_.find(src);
            if (verIt != cacheKlVer_.end() && verIt->second == expectedKlVer && rowKlVer != expectedKlVer) {
                ++staleCacheSkipped_;
                continue;
            }
        }

        cache_[src] = dst;
        cacheKlVer_[src] = rowKlVer;
        if (isSeed) {
            ++seedCacheLoaded_;
        } else {
            ++runtimeCacheLoaded_;
        }
    }
}

bool AutoTranslateManager::AppendJsonlRecord(const PendingJob& job,
                                             std::string_view translated,
                                             std::string_view engineVer,
                                             std::string_view klVer) const {
    if (!cfg_.cacheEnabled || cfg_.cacheFile.empty()) {
        return true;
    }

    std::lock_guard<std::mutex> dlock(diskMutex_);
    std::error_code ec;
    const std::filesystem::path p(cfg_.cacheFile);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    // A process/disk interruption may leave an unterminated final record.
    // Separate the next valid record so a later load can skip just the bad tail.
    bool needsSeparator = false;
    {
        std::ifstream tail(p, std::ios::binary | std::ios::ate);
        if (tail.is_open() && tail.tellg() > std::streampos(0)) {
            tail.seekg(-1, std::ios::end);
            char last = '\0';
            if (tail.get(last)) needsSeparator = (last != '\n');
        }
    }
    std::ofstream ofs(std::filesystem::path(cfg_.cacheFile), std::ios::binary | std::ios::app);
    if (!ofs.is_open()) {
        return false;
    }

    const auto nowSec = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    char rvaBuf[32];
    std::snprintf(rvaBuf, sizeof(rvaBuf), "0x%x", job.callerRva);

    std::ostringstream oss;
    oss << "{\"src\":" << QuoteJsonString(job.normalizedSource)
        << ",\"dst\":" << QuoteJsonString(translated)
        << ",\"provider\":" << QuoteJsonString(cfg_.provider)
        << ",\"screen\":" << QuoteJsonString(job.screenTag)
        << ",\"caller_rva\":" << QuoteJsonString(rvaBuf)
        << ",\"ts\":" << nowSec
        << ",\"ver\":" << QuoteJsonString(engineVer);
    if (!klVer.empty()) {
        oss << ",\"kl_ver\":" << QuoteJsonString(klVer);
    }
    oss << "}\n";
    const std::string row = oss.str();
    if (needsSeparator) ofs.put('\n');
    ofs.write(row.data(), static_cast<std::streamsize>(row.size()));
    ofs.flush();
    return ofs.good();
}

bool AutoTranslateManager::LookupOverride(std::string_view normalizedSource,
                                          std::string_view contextName,
                                          std::string& outTranslated) const {
    if (!cfg_.enabled || normalizedSource.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key(normalizedSource);
    if (!contextName.empty() && !contextOverrides_.empty()) {
        const auto ctxIt = contextOverrides_.find(std::string(contextName));
        if (ctxIt != contextOverrides_.end()) {
            const auto entryIt = ctxIt->second.find(key);
            if (entryIt != ctxIt->second.end()) {
                outTranslated = entryIt->second;
                ++overrideHits_;
                return true;
            }
        }
    }
    const auto exIt = exactOverrides_.find(key);
    if (exIt != exactOverrides_.end()) {
        outTranslated = exIt->second;
        ++overrideHits_;
        return true;
    }
    return false;
}

AutoLookupOutcome AutoTranslateManager::LookupCacheOrQueue(std::string_view normalizedSource,
                                                             uint32_t callerRva,
                                                             std::string_view screenTag,
                                                             std::string& outTranslated) {
    if (!cfg_.enabled) {
        return AutoLookupOutcome::Disabled;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key(normalizedSource);
    const auto it = cache_.find(key);
    if (it != cache_.end()) {
        outTranslated = it->second;
        ++cacheHits_;
        return AutoLookupOutcome::CacheHit;
    }

    ++cacheMisses_;

    if (failedKeys_.find(key) != failedKeys_.end()) {
        return AutoLookupOutcome::PreviouslyFailed;
    }
    if (pendingKeys_.find(key) != pendingKeys_.end()) {
        ++jobsDeduplicated_;
        return AutoLookupOutcome::AlreadyPending;
    }

    const auto cand = EvaluateAutoTranslateCandidate(normalizedSource, callerRva, screenTag, cfg_, coverageTracker_);
    if (!cand.accepted) {
        ++filteredOut_;
        return AutoLookupOutcome::Filtered;
    }

    if (stopRequested_ || !workerRunning_ || !backendAvailable_) {
        return AutoLookupOutcome::BackendUnavailable;
    }
    if (queue_.size() >= cfg_.queueMax) {
        ++jobsDroppedQueueFull_;
        return AutoLookupOutcome::QueueFull;
    }

    pendingKeys_.insert(key);
    queue_.push_back({key, callerRva, std::string(screenTag)});
    ++jobsQueued_;
    cv_.notify_one();
    return AutoLookupOutcome::Queued;
}

bool AutoTranslateManager::LookupCompleted(std::string_view source, std::string& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(std::string(source));
    if (it == cache_.end()) return false;
    out = it->second;
    return true;
}

void AutoTranslateManager::WorkerLoop() {
    workerThreadId_ = std::this_thread::get_id();
    uint32_t consecutiveFailures = 0;
    auto retryAt = std::chrono::steady_clock::time_point::min();
    bool attemptedInit = false;
    auto retryTransport = [&](PendingJob job, std::string_view reason) {
        backend_->Shutdown(true);
        const auto delay = std::chrono::milliseconds(
            std::min<uint32_t>(30000, 1000u << std::min<uint32_t>(consecutiveFailures, 5)));
        ++consecutiveFailures;
        retryAt = std::chrono::steady_clock::now() + delay;
        std::lock_guard lock(mutex_);
        backendInitDone_ = backendInitOk_ = false;
        ++jobsFailed_; // attempts, not permanently abandoned sources
        if (!stopRequested_) queue_.push_back(std::move(job));
        Logger::Instance().Logf("[AUTO] Host recovery scheduled delay_ms=%lld reason=%.*s queued=%zu",
            static_cast<long long>(delay.count()), static_cast<int>(reason.size()), reason.data(), queue_.size());
    };

    while (true) {
        PendingJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            activeJob_ = false;
            idleCv_.notify_all();
            cv_.wait(lock, [this]() { return stopRequested_ || !queue_.empty(); });
            if (stopRequested_) {
                break;
            }
            if (cv_.wait_until(lock, retryAt, [this] { return stopRequested_.load(); })) break;
            job = std::move(queue_.front());
            queue_.pop_front();
            activeJob_ = true;
        }

        // Lazy initialize backend strictly on the worker thread on first actual miss job.
        if (!backendInitDone_) {
            const auto t0 = std::chrono::steady_clock::now();
            std::string initDetail;
            if (std::this_thread::get_id() != workerThreadId_) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++renderThreadBackendCalls_;
                if (backend_ && backend_->GetHostTelemetry().isHostBackend) {
                    ++renderThreadHostIpcCalls_;
                }
            }
            if (attemptedInit) { std::lock_guard lock(mutex_); ++hostRestartCount_; }
            attemptedInit = true;
            const bool ok = backend_ && backend_->Initialize(resolvedEngineRoot_, &initDetail);
            const auto t1 = std::chrono::steady_clock::now();
            const double elapsedInitMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

            Logger::Instance().Logf("[AUTO] Worker backend init ok=%d ms=%.1f detail=%s",
                                    ok ? 1 : 0, elapsedInitMs, initDetail.c_str());

            {
                std::lock_guard lock(mutex_);
                backendInitDone_ = true;
                backendInitOk_ = ok;
                initMs_ = elapsedInitMs;
                if (ok) {
                    const auto ver = backend_->EngineVersion();
                    if (!ver.empty()) engineVersion_ = ver;
                }
            }
            if (!ok) {
                retryTransport(std::move(job), initDetail);
                continue;
            }
        }

        std::string exactKlOut;
        if (knowledge_.LookupExactOverride(job.normalizedSource, exactKlOut)) {
            const auto tvExact = ValidateTagPreservation(job.normalizedSource, exactKlOut);
            if (tvExact.valid) {
                std::string engineVerCopy;
                std::string klVerCopy;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    engineVerCopy = engineVersion_;
                    klVerCopy = knowledge_.Version();
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cache_[job.normalizedSource] = exactKlOut;
                    cacheKlVer_[job.normalizedSource] = klVerCopy;
                    pendingKeys_.erase(job.normalizedSource);
                    ++jobsCompleted_;
                    completionRevision_.fetch_add(1, std::memory_order_release);
                    ++klExactHits_;
                }
                AppendJsonlRecord(job, exactKlOut, engineVerCopy, klVerCopy);
                continue;
            }
        }

        const MaskedText masked = MaskProtectedTags(job.normalizedSource);
        const PreProcessResult preRes = knowledge_.PreProcess(masked.masked, job.screenTag);
        std::string rawTranslated;
        std::string transErr;
        const auto t0 = std::chrono::steady_clock::now();
        if (std::this_thread::get_id() != workerThreadId_) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++renderThreadBackendCalls_;
            if (backend_ && backend_->GetHostTelemetry().isHostBackend) {
                ++renderThreadHostIpcCalls_;
            }
        }
        const bool transOk = backend_->TranslateUtf8(preRes.text, rawTranslated, &transErr);
        const auto t1 = std::chrono::steady_clock::now();
        const double jobMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (!transOk) {
            const BackendFailureKind failKind = backend_->LastFailureKind();
            if (failKind == BackendFailureKind::TransportCrash || failKind == BackendFailureKind::Timeout) {
                retryTransport(std::move(job), transErr);
            } else {
                // Deterministic content/engine rejection: suppress this source,
                // while keeping all other queued translations operational.
                std::lock_guard lock(mutex_);
                pendingKeys_.erase(job.normalizedSource);
                failedKeys_.insert(job.normalizedSource);
                ++jobsFailed_;
            }
            continue;
        }

        consecutiveFailures = 0;
        retryAt = std::chrono::steady_clock::time_point::min();
        std::string restoredWithTags;
        std::string restored;
        std::string restoreReason;
        PostProcessResult postRes;
        bool valid = RestoreProtectedTags(rawTranslated, masked.tokens, restoredWithTags, &restoreReason);
        if (valid) {
            postRes = knowledge_.PostProcess(restoredWithTags, preRes, job.screenTag);
            restored = std::move(postRes.text);
            const auto tv = ValidateTagPreservation(job.normalizedSource, restored);
            if (!tv.valid) {
                valid = false;
                restoreReason = tv.reason;
            }
        }

        if (!valid) {
            bool logRej = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pendingKeys_.erase(job.normalizedSource);
                failedKeys_.insert(job.normalizedSource);
                ++tagRejected_;
                ++jobsFailed_;
                if (tagLogCount_ < 20) {
                    ++tagLogCount_;
                    logRej = true;
                }
            }
            if (logRej) {
                Logger::Instance().Logf(
                    "[AUTO] AUTO_TRANSLATE_REJECTED_TAG_MISMATCH reason=%s src='%s' dst='%s'",
                    restoreReason.c_str(),
                    MakeSingleLinePreview(job.normalizedSource).c_str(),
                    MakeSingleLinePreview(rawTranslated).c_str());
            }
            continue;
        }

        std::string engineVerCopy;
        std::string klVerCopy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            engineVerCopy = engineVersion_;
            klVerCopy = knowledge_.IsLoaded() ? knowledge_.Version() : std::string();
        }
        bool logSample = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cache_[job.normalizedSource] = restored;
            cacheKlVer_[job.normalizedSource] = klVerCopy;
            pendingKeys_.erase(job.normalizedSource);
            ++jobsCompleted_;
            completionRevision_.fetch_add(1, std::memory_order_release);
            klPreRewrites_ += preRes.preRulesApplied;
            klPostRewrites_ += postRes.postRulesApplied;
            klSentinelsRestored_ += postRes.sentinelsRestored;
            klParticleFixes_ += postRes.particleFixes;
            totalTranslateMs_ += jobMs;
            if (jobMs > maxTranslateMs_) {
                maxTranslateMs_ = jobMs;
            }
            if (cfg_.logAutoTranslate && sampleLogCount_ < 50) {
                ++sampleLogCount_;
                logSample = true;
            }
        }
        // Publish to the UI before persistence; disk latency must not delay refresh.
        AppendJsonlRecord(job, restored, engineVerCopy, klVerCopy);
        if (logSample) {
            Logger::Instance().Logf(
                "[AUTO_TR] (%.2f ms) src='%s' -> dst='%s'",
                jobMs,
                MakeSingleLinePreview(job.normalizedSource, 64).c_str(),
                MakeSingleLinePreview(restored, 64).c_str());
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    activeJob_ = false;
    idleCv_.notify_all();
}

bool AutoTranslateManager::WaitForIdleForTesting(uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idleCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
        return !activeJob_ && queue_.empty();
    });
}

AutoTranslateStats AutoTranslateManager::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AutoTranslateStats s;
    s.enabled = cfg_.enabled;
    s.backendAvailable = backendAvailable_;
    s.backendInitialized = backendInitOk_;
    s.engineVersion = engineVersion_;
    s.cachePathUtf8 = WideToUtf8(cfg_.cacheFile);
    s.overridesLoaded = overridesLoaded_;
    s.seedCacheLoaded = seedCacheLoaded_;
    s.runtimeCacheLoaded = runtimeCacheLoaded_;
    s.malformedCacheLines = malformedCacheLines_;
    s.rejectedCacheLines = rejectedCacheLines_;
    s.currentCacheEntries = cache_.size();
    s.overrideHits = overrideHits_;
    s.cacheHits = cacheHits_;
    s.cacheMisses = cacheMisses_;
    s.filteredOut = filteredOut_;
    s.jobsQueued = jobsQueued_;
    s.jobsCompleted = jobsCompleted_;
    s.jobsFailed = jobsFailed_;
    s.jobsDeduplicated = jobsDeduplicated_;
    s.jobsDroppedQueueFull = jobsDroppedQueueFull_;
    s.tagRejected = tagRejected_;
    s.renderThreadBackendCalls = renderThreadBackendCalls_;
    s.initMs = initMs_;
    s.avgTranslateMs = jobsCompleted_ > 0 ? (totalTranslateMs_ / static_cast<double>(jobsCompleted_)) : 0.0;
    s.maxTranslateMs = maxTranslateMs_;
    const auto kst = knowledge_.GetStats();
    s.knowledgeLoaded = kst.loaded;
    s.knowledgeVersion = kst.version;
    s.knowledgeExactOverrides = kst.exactOverrides;
    s.knowledgeProperNouns = kst.properNouns;
    s.knowledgePreRules = kst.preRules;
    s.knowledgePostRules = kst.postRules;
    s.staleCacheSkipped = staleCacheSkipped_;
    s.klExactHits = klExactHits_;
    s.klPreRewrites = klPreRewrites_;
    s.klPostRewrites = klPostRewrites_;
    s.klSentinelsRestored = klSentinelsRestored_;
    s.klParticleFixes = klParticleFixes_;

    s.hostMode = HdTransModeName(cfg_.hdTransMode);
    s.hostRestartCount = hostRestartCount_;
    s.hostEngineRootType = hostEngineRootType_;
    s.renderThreadHostIpcCalls = renderThreadHostIpcCalls_;
    const HostTelemetrySnapshot hst = backend_ ? backend_->GetHostTelemetry() : cachedHostTelemetry_;
    if (hst.isHostBackend) {
        s.hostSpawned = hst.hostSpawned;
        s.hostSpawnCount = hst.hostSpawnCount;
        s.hostProtocolVer = hst.hostProtocolVersion;
        s.hostVersion = hst.hostVersion;
        s.hostEngineVer = hst.engineVersion;
        if (!hst.engineRootType.empty() && hst.engineRootType != "none") {
            s.hostEngineRootType = hst.engineRootType;
        }
        s.ipcRequests = hst.ipcRequests;
        s.ipcCompleted = hst.ipcCompleted;
        s.ipcFailed = hst.ipcFailed;
        s.ipcTimeout = hst.ipcTimeout;
        s.hostExitCode = hst.hostExitCode;
    }
    return s;
}

}  // namespace aigiskr::core
