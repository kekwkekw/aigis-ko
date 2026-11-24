#include "hooks/probe_hooks.hpp"
#include "core/auto_translate.hpp"
#include "core/coverage.hpp"
#include "core/logger.hpp"
#include "core/mem_safe.hpp"
#include "core/mini_json.hpp"
#include "core/text_encoding.hpp"
#include "core/translation_core.hpp"
#include "diagnostics/healthcheck.hpp"
#include "hooks/hangul_font.hpp"
#include "hooks/live_text_refresh.hpp"
#include "resolver/resolver.hpp"

#include <windows.h>
#include <safetyhook.hpp>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace aigiskr::hooks {

namespace {

core::ProbeConfig g_cfg;
resolver::ModuleLayout g_layout;
diagnostics::HealthcheckReport g_healthReport;
std::mutex g_healthMutex;
core::CoverageTracker g_coverageTracker;
core::TranslationCore g_translationCore;
core::AutoTranslateManager g_autoTranslate;
std::atomic<ULONGLONG> g_lastScreenPollTick{0};
std::atomic<ULONGLONG> g_lastFlushTick{0};
std::atomic<ULONGLONG> g_lastTexPollTick{0};
int g_lastLoggedTexState = -1;
size_t g_lastLoggedTexActive = 0;
size_t g_lastLoggedTexFailed = 0;
std::string g_translationDbVersion = "DISABLED";
std::string g_translationDbSha256;

std::string ReadTranslationDbVersionTag(const std::wstring& path) {
    core::JsonValue root;
    std::string err;
    if (!core::ParseJsonFile(core::WideToUtf8(path), root, err) ||
        root.type != core::JsonValue::Type::Object) {
        return "UNKNOWN";
    }
    const auto* meta = root.Find("_meta");
    if (meta && meta->type == core::JsonValue::Type::Object) {
        if (const auto* ver = meta->Find("version")) {
            if (!ver->AsString().empty()) {
                return std::string(ver->AsString());
            }
        }
    }
    return "UNKNOWN";
}

void UpdateTextureAndWriteHealthcheck(bool writeJson,
                                       bool logSummary,
                                       bool requireFreshStatus = true,
                                       bool logTextureLine = false) {
    const std::string gameDirUtf8 = core::WideToUtf8(g_cfg.gameDir);
    const std::string pluginDirUtf8 = core::WideToUtf8(g_cfg.pluginDir.empty() ? g_cfg.gameDir : g_cfg.pluginDir);
    const std::string covDirUtf8 = core::WideToUtf8(g_cfg.coverageDir);
    const std::string manifestPath = gameDirUtf8.empty() ? std::string() : (gameDirUtf8 + "TT\\texture_manifest_r2.json");
    const std::string statusPath = gameDirUtf8.empty() ? std::string() : (gameDirUtf8 + "TT\\tt_profile_status.json");

    diagnostics::HealthcheckReport snapshot;
    bool texChanged = false;
    {
        std::lock_guard<std::mutex> lock(g_healthMutex);
        g_healthReport.pluginVersion = AIGISKR_VERSION_STR;
        g_healthReport.releasePackageVersion = AIGISKR_VERSION_STR;
        g_healthReport.translationDbVersion = g_translationDbVersion;
        g_healthReport.translationDbSha256 = g_translationDbSha256;
        if (g_cfg.autoTranslate.enabled) {
            const auto ast = g_autoTranslate.GetStats();
            g_healthReport.autoTranslate.enabled = ast.enabled;
            g_healthReport.autoTranslate.hostMode = ast.hostMode;
            g_healthReport.autoTranslate.hostVersion = ast.hostVersion;
            g_healthReport.autoTranslate.hostProtocolVer = ast.hostProtocolVer;
            g_healthReport.autoTranslate.hdtransEngineVer = !ast.hostEngineVer.empty() ? ast.hostEngineVer : ast.engineVersion;
            g_healthReport.autoTranslate.engineRootType = ast.hostEngineRootType;
            g_healthReport.autoTranslate.hostSpawned = ast.hostSpawned;
            g_healthReport.autoTranslate.knowledgeVersion = ast.knowledgeVersion;
        }
        diagnostics::EvaluateTextureHealth(g_healthReport, manifestPath, statusPath, requireFreshStatus);
        snapshot = g_healthReport;
        const int stInt = static_cast<int>(snapshot.texture.state);
        if (stInt != g_lastLoggedTexState ||
            snapshot.texture.injectedActiveCount != g_lastLoggedTexActive ||
            snapshot.texture.failedInjectionsCount != g_lastLoggedTexFailed) {
            if (snapshot.texture.statusPresent ||
                snapshot.texture.state == diagnostics::TextureProfileState::ManifestMissing) {
                g_lastLoggedTexState = stInt;
                g_lastLoggedTexActive = snapshot.texture.injectedActiveCount;
                g_lastLoggedTexFailed = snapshot.texture.failedInjectionsCount;
                texChanged = true;
            }
        }
    }
    if (logSummary) {
        core::Logger::Instance().LogLine(diagnostics::FormatHealthcheckSummaryLine(snapshot));
    }
    if (logTextureLine || texChanged) {
        core::Logger::Instance().Logf(
            "[TEXTURE] Texture profile: %s | Active roles: %zu/%zu | Failed: %zu",
            diagnostics::TextureProfileStateName(snapshot.texture.state),
            snapshot.texture.injectedActiveCount,
            snapshot.texture.manifestRolesTotal,
            snapshot.texture.failedInjectionsCount);
    }
    if (writeJson && !pluginDirUtf8.empty()) {
        diagnostics::WriteHealthcheckJson(snapshot, pluginDirUtf8 + "AigisKR\\healthcheck_latest.json");
    }
    if (writeJson && g_cfg.enableCoverage && !covDirUtf8.empty()) {
        diagnostics::WriteHealthcheckJson(snapshot, covDirUtf8 + "\\healthcheck_latest.json");
    }
}

uintptr_t g_nextSjisVa = 0;
uintptr_t g_nextUtf16Va = 0;
uintptr_t g_nextUtf32Va = 0;

struct ActiveHook {
    const char* id = nullptr;
    uint32_t targetRva = 0;
    SafetyHookMid mid;
};

ActiveHook g_hTextDrawSlot28;
ActiveHook g_hTextDrawSlot40;
ActiveHook g_hTextDrawStream;
ActiveHook g_hTextProp70d350;
ActiveHook g_hFontW3Slot12;
ActiveHook g_hFontW3Slot14;
ActiveHook g_hMousePoll;

std::mutex g_internMutex;
std::unordered_set<std::string> g_internedStrings;
std::unordered_set<std::string> g_loggedReplacedSources;

struct VirtualClickState {
    bool active = false;
    bool isDrag = false;
    int32_t startX = 0;
    int32_t startY = 0;
    int32_t clientX = 0;
    int32_t clientY = 0;
    int frame = 0;
    ULONGLONG lastPollTick = 0;
};
VirtualClickState g_vclick;

std::mutex g_dedupMutex;
std::unordered_set<uint64_t> g_seenKeys;
uint64_t g_rateWindowSec = 0;
uint32_t g_rateWindowCount = 0;

std::atomic<uint32_t> g_totalLogged{0};
std::atomic<uint32_t> g_totalHitsTextDraw28{0};
std::atomic<uint32_t> g_totalHitsTextDraw40{0};
std::atomic<uint32_t> g_totalHitsDrawStream{0};
std::atomic<uint32_t> g_totalHitsProp70d350{0};
std::atomic<uint32_t> g_totalHitsFontW3_12{0};
std::atomic<uint32_t> g_totalHitsFontW3_14{0};

thread_local int t_hookReentry = 0;

uint64_t CombineDedupKey(const char* hookId, uint32_t callerAddr, uint64_t textHash) {
    uint64_t h = core::Fnv1a64(hookId);
    h ^= static_cast<uint64_t>(callerAddr) * 0x9e3779b97f4a7c15ull;
    h ^= (textHash << 1) | (textHash >> 63);
    return h;
}

bool ShouldLogEvent(uint64_t dedupKey) {
    if (g_totalLogged.load(std::memory_order_relaxed) >= g_cfg.maxCaptureEntries) {
        return false;
    }
    const uint64_t sec = GetTickCount64() / 1000ull;
    std::lock_guard<std::mutex> lock(g_dedupMutex);
    if (g_cfg.deduplicate) {
        if (!g_seenKeys.insert(dedupKey).second) {
            return false;
        }
    }
    if (sec != g_rateWindowSec) {
        g_rateWindowSec = sec;
        g_rateWindowCount = 0;
    }
    if (g_rateWindowCount >= g_cfg.maxLogsPerSecond) {
        return false;
    }
    ++g_rateWindowCount;
    g_totalLogged.fetch_add(1, std::memory_order_relaxed);
    return true;
}

core::DecodedText SafelyDecodeRawString(uintptr_t ptr, const char* readerHint) {
    if (ptr < 0x10000u || !core::IsReadableRange(reinterpret_cast<const void*>(ptr), 1)) {
        core::DecodedText d;
        d.encoding = core::DetectedEncoding::Empty;
        d.utf8 = "<NONPTR>";
        return d;
    }

    if (std::strcmp(readerHint, "UTF16") == 0) {
        wchar_t buf[512] = {};
        size_t chars = 0;
        if (!core::SafeCopyWString(reinterpret_cast<const wchar_t*>(ptr), buf, _countof(buf), &chars)) {
            core::DecodedText d;
            d.encoding = core::DetectedEncoding::BinaryOrInvalid;
            d.utf8 = "<UNREADABLE_W>";
            return d;
        }
        return core::DecodeUtf16String(buf, chars);
    }

    if (std::strcmp(readerHint, "UTF32") == 0) {
        uint32_t buf[512] = {};
        size_t chars = 0;
        if (!core::SafeCopyUtf32String(reinterpret_cast<const uint32_t*>(ptr), buf, _countof(buf), &chars)) {
            core::DecodedText d;
            d.encoding = core::DetectedEncoding::BinaryOrInvalid;
            d.utf8 = "<UNREADABLE_U32>";
            return d;
        }
        return core::DecodeUtf32String(buf, chars);
    }

    std::string source;
    if (!core::SafeReadCompleteCString(reinterpret_cast<const char*>(ptr), source)) {
        core::DecodedText d;
        d.encoding = core::DetectedEncoding::BinaryOrInvalid;
        d.utf8 = "<UNREADABLE_C>";
        return d;
    }
    return core::DecodeMultibyteBytes(reinterpret_cast<const uint8_t*>(source.data()), source.size());
}

void RecordProbeEvent(const char* hookId, uint32_t targetRva, const SafetyHookContext& ctx,
                      uintptr_t callerAddr, uintptr_t strPtr, const char* readerKind,
                      const core::DecodedText& decoded) {
    const uint64_t key = CombineDedupKey(hookId, static_cast<uint32_t>(callerAddr - g_layout.base),
                                         core::Fnv1a64(decoded.utf8));
    if (!ShouldLogEvent(key)) return;

    uint32_t vtblRva = 0;
    std::string rtti = resolver::ReadObjectRttiName(g_layout, reinterpret_cast<const void*>(ctx.ecx), &vtblRva);
    if (rtti.empty()) rtti = "-";

    std::string callerStr = resolver::FormatCodeAddress(g_layout.base, g_layout, callerAddr);

    char regs[160];
    snprintf(regs, sizeof(regs), "ecx=%08x edx=%08x esi=%08x edi=%08x esp=%08x",
             static_cast<uint32_t>(ctx.ecx), static_cast<uint32_t>(ctx.edx),
             static_cast<uint32_t>(ctx.esi), static_cast<uint32_t>(ctx.edi),
             static_cast<uint32_t>(ctx.esp));

    char tsv[3072];
    snprintf(tsv, sizeof(tsv),
             "%llu\t%lu\t%s\t0x%x\t%s\t0x%08x\t0x%x\t%s\t%s\t0x%08x\t%s\t%zu\t%s\t%s\t%s",
             static_cast<unsigned long long>(core::Logger::Instance().UptimeMs()),
             GetCurrentThreadId(),
             hookId,
             targetRva,
             callerStr.c_str(),
             static_cast<uint32_t>(ctx.ecx),
             vtblRva,
             rtti.c_str(),
             regs,
             static_cast<uint32_t>(strPtr),
             readerKind,
             decoded.rawByteLen,
             core::EncodingName(decoded.encoding),
             core::EscapeForTsv(decoded.utf8).c_str(),
             decoded.hexPreview.c_str());
    core::Logger::Instance().WriteCaptureTsv(tsv);
}

void ReadStackArgs(const SafetyHookContext& ctx, uint32_t& retAddr, uint32_t& arg0, uint32_t& arg1, uint32_t& arg2) {
    retAddr = arg0 = arg1 = arg2 = 0;
    core::SafeReadDword(reinterpret_cast<const void*>(ctx.esp + 0), &retAddr);
    core::SafeReadDword(reinterpret_cast<const void*>(ctx.esp + 4), &arg0);
    core::SafeReadDword(reinterpret_cast<const void*>(ctx.esp + 8), &arg1);
    core::SafeReadDword(reinterpret_cast<const void*>(ctx.esp + 12), &arg2);
}

void PollScreenAndFlushTick() {
    const ULONGLONG now = GetTickCount64();

    if (g_cfg.enableCoverage || g_cfg.autoTranslate.enabled) {
        const ULONGLONG prev = g_lastScreenPollTick.load(std::memory_order_relaxed);
        if (now - prev >= 250 &&
            g_lastScreenPollTick.compare_exchange_weak(
                const_cast<ULONGLONG&>(prev), now, std::memory_order_relaxed)) {
            const std::wstring scrPath = g_cfg.gameDir + L"aigiskr_screen.req";
            if (GetFileAttributesW(scrPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                FILE* f = nullptr;
                if (_wfopen_s(&f, scrPath.c_str(), L"rb") == 0 && f) {
                    char buf[128] = {};
                    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
                    std::fclose(f);
                    DeleteFileW(scrPath.c_str());
                    std::string scr(buf, n);
                    while (!scr.empty() && (scr.back() == '\r' || scr.back() == '\n' || scr.back() == ' ')) {
                        scr.pop_back();
                    }
                    if (!scr.empty()) {
                        g_coverageTracker.SetCurrentScreen(scr);
                        core::Logger::Instance().Logf("[COVERAGE] screen_tag='%s'", scr.c_str());
                    }
                }
            }

            if (g_cfg.enableCoverage) {
                const ULONGLONG lastFlush = g_lastFlushTick.load(std::memory_order_relaxed);
                if (g_cfg.coverageFlushIntervalMs > 0 && now - lastFlush >= g_cfg.coverageFlushIntervalMs) {
                    g_lastFlushTick.store(now, std::memory_order_relaxed);
                    g_coverageTracker.FlushToDisk();
                    UpdateTextureAndWriteHealthcheck(true, false, true);
                }
            }
        }
        if (g_cfg.enableCoverage) {
            return;
        }
    }

    // Release mode (EnableCoverage=0): periodically refresh healthcheck JSON & emit texture state transition once.
    const ULONGLONG prevTex = g_lastTexPollTick.load(std::memory_order_relaxed);
    if (now - prevTex >= 10000 &&
        g_lastTexPollTick.compare_exchange_weak(
            const_cast<ULONGLONG&>(prevTex), now, std::memory_order_relaxed)) {
        UpdateTextureAndWriteHealthcheck(true, false, true);
    }
}

void ApplyInternedReplacement(SafetyHookContext& ctx,
                              const std::string& replacementText,
                              const std::string& sourceText,
                              const char* logTag) {
    const char* internedPtr = nullptr;
    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lock(g_internMutex);
        auto [it, _] = g_internedStrings.insert(replacementText);
        internedPtr = it->c_str();
        if (g_cfg.logTextEvents) {
            shouldLog = g_loggedReplacedSources.insert(std::string(logTag) + ":" + sourceText).second;
        }
    }
    *reinterpret_cast<uint32_t*>(ctx.esp + 4) = reinterpret_cast<uint32_t>(internedPtr);
    if (g_cfg.logTextEvents && shouldLog) {
        core::Logger::Instance().Logf("[%s] translated '%s' -> '%s'",
                                       logTag, sourceText.c_str(), replacementText.c_str());
    }
}

void MaybeApplyTextReplacement(SafetyHookContext& ctx, const core::DecodedText& decoded, uint32_t retAddr,
                              bool measuring = false, bool* sequenceAwaiting = nullptr) {
    if (sequenceAwaiting) *sequenceAwaiting = false;
    std::string liveTarget;
    if (LiveReplayReplacement(ctx.ecx, decoded.utf8, liveTarget)) {
        ApplyInternedReplacement(ctx, liveTarget, decoded.utf8, "LIVE_REFRESH");
        return;
    }
    if (decoded.encoding != core::DetectedEncoding::Utf8 &&
        decoded.encoding != core::DetectedEncoding::Ascii) {
        return;
    }
    if (decoded.utf8.empty()) {
        return;
    }

    PollScreenAndFlushTick();
    const uint32_t callerRva = (retAddr >= g_layout.base)
                                   ? static_cast<uint32_t>(retAddr - g_layout.base)
                                   : 0u;

    const std::string normSource = core::NormalizeLineEndings(decoded.utf8);
    {
        if (!g_cfg.enableTranslation) {
            if (g_cfg.enableCoverage) {
                const auto bucket = g_coverageTracker.IsIgnoredText(normSource)
                                        ? core::CoverageBucket::Ignored
                                        : core::CoverageBucket::Unmatched;
                g_coverageTracker.Record(normSource, bucket, "translation_disabled", callerRva);
            }
            return;
        }

        std::string activeContext;
        const std::string curScreen = g_coverageTracker.GetCurrentScreen();
        if (callerRva == 0xAD6C0u) {
            activeContext = "Story.Speaker";
        } else if (curScreen == "UnitStatus") {
            activeContext = (callerRva == 0x74DFBu) ? "UnitStatus" : "NameText.RealName";
        } else if (!curScreen.empty()) {
            activeContext = curScreen;
        }

        // 1. Optional manual override takes precedence over curated dictionaries.
        std::string overrideText;
        if (g_cfg.autoTranslate.enabled &&
            g_autoTranslate.LookupOverride(normSource, activeContext, overrideText)) {
            ApplyInternedReplacement(ctx, overrideText, decoded.utf8, "OVERRIDE");
            if (g_cfg.enableCoverage) {
                g_coverageTracker.Record(normSource, core::CoverageBucket::Exact, "manual_override", callerRva);
            }
            return;
        }

        // 2. Curated deterministic matcher: ExactContext -> Exact -> Template -> WhitelistedRegex.
        auto tr = g_translationCore.Translate(decoded.utf8, activeContext);
        if (tr.outcome == core::TranslateOutcome::Hit) {
            ApplyInternedReplacement(ctx, tr.text, decoded.utf8, "PHASE4");
            if (g_cfg.enableCoverage) {
                core::CoverageBucket bucket = core::CoverageBucket::Exact;
                if (tr.tier == core::MatchTier::ExactContext) bucket = core::CoverageBucket::Context;
                else if (tr.tier == core::MatchTier::Template) bucket = core::CoverageBucket::Template;
                else if (tr.tier == core::MatchTier::WhitelistedRegex) bucket = core::CoverageBucket::Regex;
                g_coverageTracker.Record(normSource, bucket, tr.matchedRule, callerRva);
            }
        } else if (tr.outcome == core::TranslateOutcome::TagMismatchFallback) {
            bool shouldLog = false;
            {
                std::lock_guard<std::mutex> lock(g_internMutex);
                shouldLog = g_loggedReplacedSources.insert("P4ERR:" + decoded.utf8).second;
            }
            if (shouldLog) {
                core::Logger::Instance().Logf("[PHASE4] tag_mismatch_fallback src='%s'", decoded.utf8.c_str());
            }
            if (g_cfg.enableCoverage) {
                g_coverageTracker.Record(normSource, core::CoverageBucket::TagRejected, tr.matchedRule, callerRva);
            }
        } else {
            // 3. AutoTranslate cache hit or asynchronous non-blocking queue submission.
            if (g_cfg.autoTranslate.enabled) {
                std::string autoText;
                const auto autoOutcome = g_autoTranslate.LookupCacheOrQueue(
                    normSource, callerRva, curScreen, autoText);
                if (autoOutcome == core::AutoLookupOutcome::CacheHit) {
                    ApplyInternedReplacement(ctx, autoText, decoded.utf8, "AUTO_CACHE");
                } else if (autoOutcome == core::AutoLookupOutcome::Queued ||
                           autoOutcome == core::AutoLookupOutcome::AlreadyPending) {
                    if (sequenceAwaiting) *sequenceAwaiting = true;
                    else if (!measuring) TrackPendingLiveText(ctx.ecx, decoded.utf8, normSource, callerRva);
                }
            }
            if (g_cfg.enableCoverage) {
                const auto bucket = g_coverageTracker.IsIgnoredText(normSource)
                                        ? core::CoverageBucket::Ignored
                                        : core::CoverageBucket::Unmatched;
                g_coverageTracker.Record(normSource, bucket, {}, callerRva);
            }
        }
    }
}

BOOL CALLBACK FindThreadWindowProc(HWND hwnd, LPARAM lparam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    RECT rc = {};
    if (GetClientRect(hwnd, &rc) && (rc.right - rc.left) > 100 && (rc.bottom - rc.top) > 100) {
        *reinterpret_cast<HWND*>(lparam) = hwnd;
        return FALSE;
    }
    return TRUE;
}

void PollAndApplyVirtualClick(SafetyHookContext& ctx) {
    if (!g_cfg.enableVirtualClick) return;
    if (!g_vclick.active) {
        const ULONGLONG now = GetTickCount64();
        if (now - g_vclick.lastPollTick < 40) return;
        g_vclick.lastPollTick = now;

        const std::wstring reqPath = g_cfg.gameDir + L"aigiskr_click.req";
        if (GetFileAttributesW(reqPath.c_str()) == INVALID_FILE_ATTRIBUTES) return;

        float rx = 0.0f, ry = 0.0f, rx2 = 0.0f, ry2 = 0.0f;
        FILE* f = nullptr;
        int parsedCount = 0;
        if (_wfopen_s(&f, reqPath.c_str(), L"r") == 0 && f) {
            parsedCount = fscanf_s(f, "%f %f %f %f", &rx, &ry, &rx2, &ry2);
            fclose(f);
        }
        DeleteFileW(reqPath.c_str());
        if (parsedCount < 2) return;

        int winW = 919, winH = 612;
        HWND hwnd = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), &FindThreadWindowProc, reinterpret_cast<LPARAM>(&hwnd));
        if (hwnd) {
            RECT rc = {};
            if (GetClientRect(hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
                winW = rc.right - rc.left;
                winH = rc.bottom - rc.top;
            }
        }
        g_vclick.startX = static_cast<int32_t>(rx * static_cast<float>(winW) + 0.5f);
        g_vclick.startY = static_cast<int32_t>(ry * static_cast<float>(winH) + 0.5f);
        g_vclick.isDrag = (parsedCount >= 4);
        if (g_vclick.isDrag) {
            g_vclick.clientX = static_cast<int32_t>(rx2 * static_cast<float>(winW) + 0.5f);
            g_vclick.clientY = static_cast<int32_t>(ry2 * static_cast<float>(winH) + 0.5f);
        } else {
            g_vclick.clientX = g_vclick.startX;
            g_vclick.clientY = g_vclick.startY;
        }
        g_vclick.frame = 0;
        g_vclick.active = true;
        core::Logger::Instance().Logf(
            "[DIAG] Virtual%s armed (%.3f, %.3f)->(%.3f, %.3f) client=(%d,%d)->(%d,%d) win=%dx%d",
            g_vclick.isDrag ? "Drag" : "Click", rx, ry, g_vclick.isDrag ? rx2 : rx,
            g_vclick.isDrag ? ry2 : ry, g_vclick.startX, g_vclick.startY, g_vclick.clientX,
            g_vclick.clientY, winW, winH);
    }

    if (g_vclick.active) {
        int32_t curX = g_vclick.clientX;
        int32_t curY = g_vclick.clientY;
        bool down = false;
        int endFrame = 12;
        if (g_vclick.isDrag) {
            endFrame = 28;
            if (g_vclick.frame < 6) {
                curX = g_vclick.startX;
                curY = g_vclick.startY;
                down = (g_vclick.frame >= 2);
            } else if (g_vclick.frame < 19) {
                const float t = static_cast<float>(g_vclick.frame - 6) / 13.0f;
                curX = g_vclick.startX + static_cast<int32_t>(static_cast<float>(g_vclick.clientX - g_vclick.startX) * t);
                curY = g_vclick.startY + static_cast<int32_t>(static_cast<float>(g_vclick.clientY - g_vclick.startY) * t);
                down = true;
            } else {
                down = (g_vclick.frame < 24);
            }
        } else {
            down = (g_vclick.frame >= 3 && g_vclick.frame < 8);
        }
        *reinterpret_cast<int32_t*>(ctx.ebp - 0x0C) = curX;
        *reinterpret_cast<int32_t*>(ctx.ebp - 0x08) = curY;
        if (down) {
            ctx.ebx |= 1u;
        } else {
            ctx.ebx &= ~1u;
        }
        ++g_vclick.frame;
        if (g_vclick.frame >= endFrame) {
            g_vclick.active = false;
            core::Logger::Instance().LogLine("[DIAG] VirtualClick sequence finished.");
        }
    }
}

void __cdecl OnMousePollMid(SafetyHookContext& ctx) {
    PollScreenAndFlushTick();
    PollAndApplyVirtualClick(ctx);
}

void __cdecl OnTextDrawSlot28(SafetyHookContext& ctx) {
    if (t_hookReentry > 0) return;
    ++t_hookReentry;
    g_totalHitsTextDraw28.fetch_add(1, std::memory_order_relaxed);
    uint32_t retAddr = 0, strPtr = 0, a1 = 0, a2 = 0;
    ReadStackArgs(ctx, retAddr, strPtr, a1, a2);
    const bool sequence = BeginLiveTextWrite(ctx.ecx,
        retAddr >= g_layout.base ? static_cast<uint32_t>(retAddr - g_layout.base) : 0);
    auto decoded = SafelyDecodeRawString(strPtr, "CHAR");
    if (g_cfg.logTextEvents) {
        RecordProbeEvent(g_hTextDrawSlot28.id, g_hTextDrawSlot28.targetRva, ctx, retAddr, strPtr, "slot28_char", decoded);
    }
    bool awaiting = false;
    MaybeApplyTextReplacement(ctx, decoded, retAddr, false, sequence ? &awaiting : nullptr);
    if (sequence) {
        // Capture the effective input too: curated/cache/static lines must be
        // preserved when a different line completes asynchronously.
        ReadStackArgs(ctx, retAddr, strPtr, a1, a2);
        const auto rendered = SafelyDecodeRawString(strPtr, "CHAR");
        const auto valid = [](const core::DecodedText& d) {
            return d.encoding == core::DetectedEncoding::Utf8 || d.encoding == core::DetectedEncoding::Ascii ||
                (d.encoding == core::DetectedEncoding::Empty && d.utf8.empty());
        };
        CaptureStoryText(ctx.ecx, decoded.utf8, rendered.utf8, awaiting, valid(decoded) && valid(rendered));
    }
    --t_hookReentry;
}

void __cdecl OnTextDrawSlot40(SafetyHookContext& ctx) {
    if (t_hookReentry > 0) return;
    // Slot40 measures width (draw+bc=1, restores the cursor, emits no glyphs).
    // It often measures OTHER strings on the currently displayed draw object.
    // Translating its input is useful for layout, but it must neither cancel
    // the displayed text's pending refresh nor register a measured string.
    ++t_hookReentry;
    g_totalHitsTextDraw40.fetch_add(1, std::memory_order_relaxed);
    uint32_t retAddr = 0, strPtr = 0, a1 = 0, a2 = 0;
    ReadStackArgs(ctx, retAddr, strPtr, a1, a2);
    auto decoded = SafelyDecodeRawString(strPtr, "CHAR");
    if (g_cfg.logTextEvents) {
        RecordProbeEvent(g_hTextDrawSlot40.id, g_hTextDrawSlot40.targetRva, ctx, retAddr, strPtr, "slot40_char", decoded);
    }
    MaybeApplyTextReplacement(ctx, decoded, retAddr, true);
    --t_hookReentry;
}

void __cdecl OnTextDrawStream(SafetyHookContext& ctx) {
    if (t_hookReentry > 0) return;
    ++t_hookReentry;
    g_totalHitsDrawStream.fetch_add(1, std::memory_order_relaxed);
    uint32_t retAddr = 0, strPtr = 0, readerFn = 0, maxChars = 0;
    ReadStackArgs(ctx, retAddr, strPtr, readerFn, maxChars);

    // An independent UTF16/UTF32/custom append supersedes the captured single
    // text. Keep only streams produced inside our two UTF-8 entry points.
    const auto streamCaller = retAddr - static_cast<uint32_t>(g_layout.base);
    if (!IsCurrentRefreshPreservingStream(ctx.ecx, streamCaller, ctx.ebp))
        ForgetLiveText(ctx.ecx, LiveTextReset::Stream, streamCaller);

    const char* readerKind = "CUSTOM";
    if (g_nextUtf16Va && readerFn == g_nextUtf16Va) readerKind = "UTF16";
    else if (g_nextUtf32Va && readerFn == g_nextUtf32Va) readerKind = "UTF32";
    else if (g_nextSjisVa && readerFn == g_nextSjisVa) readerKind = "SJIS";

    auto decoded = SafelyDecodeRawString(strPtr, readerKind);
    if (g_cfg.logTextEvents) {
        RecordProbeEvent(g_hTextDrawStream.id, g_hTextDrawStream.targetRva, ctx, retAddr, strPtr, readerKind, decoded);
    }
    --t_hookReentry;
}

void __cdecl OnTextProp70d350(SafetyHookContext& ctx) {
    if (t_hookReentry > 0) return;
    ++t_hookReentry;
    g_totalHitsProp70d350.fetch_add(1, std::memory_order_relaxed);
    uint32_t retAddr = 0, arg0 = 0, a1 = 0, a2 = 0;
    ReadStackArgs(ctx, retAddr, arg0, a1, a2);

    core::DecodedText decoded;
    if (arg0 < 0x10000u) {
        decoded.encoding = core::DetectedEncoding::Empty;
        char b[32];
        snprintf(b, sizeof(b), "<SCALAR:0x%x>", arg0);
        decoded.utf8 = b;
    } else if (core::IsReadableRange(reinterpret_cast<const void*>(arg0), 1)) {
        std::string innerRtti = resolver::ReadObjectRttiName(g_layout, reinterpret_cast<const void*>(arg0));
        if (!innerRtti.empty()) {
            decoded.encoding = core::DetectedEncoding::Empty;
            decoded.utf8 = "<OBJ:" + innerRtti + ">";
        } else {
            decoded = SafelyDecodeRawString(arg0, "CHAR");
        }
    } else {
        decoded.encoding = core::DetectedEncoding::BinaryOrInvalid;
        char b[32];
        snprintf(b, sizeof(b), "<PTR:0x%08x>", arg0);
        decoded.utf8 = b;
    }

    RecordProbeEvent(g_hTextProp70d350.id, g_hTextProp70d350.targetRva, ctx, retAddr, arg0, "prop_arg0", decoded);
    --t_hookReentry;
}

void __cdecl OnFontW3Slot12(SafetyHookContext& ctx) {
    if (t_hookReentry > 0) return;
    ++t_hookReentry;
    g_totalHitsFontW3_12.fetch_add(1, std::memory_order_relaxed);
    uint32_t retAddr = 0, a0 = 0, a1 = 0, a2 = 0;
    ReadStackArgs(ctx, retAddr, a0, a1, a2);
    core::DecodedText d;
    char msg[96];
    snprintf(msg, sizeof(msg), "font_create arg0=%u(0x%x) arg1=%u", a0, a0, a1);
    d.utf8 = msg;
    d.encoding = core::DetectedEncoding::Ascii;
    RecordProbeEvent(g_hFontW3Slot12.id, g_hFontW3Slot12.targetRva, ctx, retAddr, 0, "ALFontImpW3_12", d);
    --t_hookReentry;
}

void __cdecl OnFontW3Slot14(SafetyHookContext& ctx) {
    if (t_hookReentry > 0) return;
    ++t_hookReentry;
    g_totalHitsFontW3_14.fetch_add(1, std::memory_order_relaxed);
    uint32_t retAddr = 0, cp = 0, a1 = 0, a2 = 0;
    ReadStackArgs(ctx, retAddr, cp, a1, a2);
    core::DecodedText d;
    char msg[96];
    snprintf(msg, sizeof(msg), "glyph_outline code=U+%04X (%u) arg1=0x%x", cp, cp, a1);
    d.utf8 = msg;
    d.encoding = core::DetectedEncoding::Ascii;
    RecordProbeEvent(g_hFontW3Slot14.id, g_hFontW3Slot14.targetRva, ctx, retAddr, 0, "ALFontImpW3_14", d);
    --t_hookReentry;
}

bool ArmMidHook(ActiveHook& slot,
                const resolver::TargetSpec& spec,
                safetyhook::MidHookFn fn,
                bool required = false) {
    auto res = resolver::ResolveTarget(g_layout, spec);
    core::Logger::Instance().Logf("[RESOLVE] %-24s -> %s", spec.name, res.detail.c_str());

    diagnostics::SignatureCheckRecord rec;
    rec.name = spec.name;
    rec.required = required;
    rec.matchCount = res.patternMatchCount;
    rec.expectedCount = 1;
    rec.rva = res.rva;
    rec.note = res.detail;

    if (!res.ok) {
        g_healthReport.signatures.push_back(std::move(rec));
        return false;
    }

    auto mid = safetyhook::MidHook::create(reinterpret_cast<void*>(res.va), fn);
    if (!mid) {
        core::Logger::Instance().Logf("[HOOK] FAILED MidHook on %s @ rva=0x%x", spec.name, res.rva);
        rec.note = "SafetyHookMid::create failed";
        g_healthReport.signatures.push_back(std::move(rec));
        return false;
    }
    slot.id = spec.name;
    slot.targetRva = res.rva;
    slot.mid = std::move(*mid);
    rec.armed = true;
    g_healthReport.signatures.push_back(std::move(rec));
    core::Logger::Instance().Logf("[HOOK] ARMED %s @ va=0x%08x (rva=0x%x)", spec.name, static_cast<uint32_t>(res.va), res.rva);
    return true;
}

}  // namespace

bool InstallProbeHooks(const core::ProbeConfig& cfg) {
    g_cfg = cfg;
    g_healthReport = diagnostics::HealthcheckReport{};
    g_lastLoggedTexState = -1;
    g_lastLoggedTexActive = 0;
    g_lastLoggedTexFailed = 0;
    g_translationDbVersion = "DISABLED";
    g_translationDbSha256.clear();
    diagnostics::InspectHostBinary(g_healthReport);

    const std::string covDirUtf8 = core::WideToUtf8(cfg.coverageDir);
    g_coverageTracker.Reset();
    g_coverageTracker.Configure(cfg.enableCoverage, covDirUtf8);
    if (cfg.enableCoverage && !cfg.placeholdersPath.empty()) {
        std::string phErr;
        if (g_coverageTracker.LoadClassificationRulesFromJsonFile(cfg.placeholdersPath, &phErr)) {
            core::Logger::Instance().Logf("[COVERAGE] Loaded R2 classification rules: placeholders=%zu",
                                           g_coverageTracker.PlaceholderRuleCount());
        } else {
            core::Logger::Instance().Logf("[COVERAGE] Note: placeholders file not loaded (%s)", phErr.c_str());
        }
    }
    g_lastFlushTick.store(GetTickCount64(), std::memory_order_relaxed);
    g_lastTexPollTick.store(GetTickCount64(), std::memory_order_relaxed);

    HMODULE exeMod = GetModuleHandleW(nullptr);
    if (!resolver::InspectModuleLayout(exeMod, g_layout)) {
        core::Logger::Instance().LogLine("[HOOK] Failed to inspect main module PE sections.");
        diagnostics::EvaluateBuildSafety(g_healthReport, false, false);
        UpdateTextureAndWriteHealthcheck(true, true, true);
        return false;
    }
    core::Logger::Instance().Logf("[PE] exe_base=0x%08x .text=[0x%08x..0x%08x) .rdata=[0x%08x..0x%08x) .data=[0x%08x..0x%08x)",
                                   static_cast<uint32_t>(g_layout.base),
                                   static_cast<uint32_t>(g_layout.text.begin), static_cast<uint32_t>(g_layout.text.end),
                                   static_cast<uint32_t>(g_layout.rdata.begin), static_cast<uint32_t>(g_layout.rdata.end),
                                   static_cast<uint32_t>(g_layout.data.begin), static_cast<uint32_t>(g_layout.data.end));

    // Resolve internal next-char readers (for accurate reader classification inside DrawStream, not hooked).
    auto recordCheckOnly = [](const char* name, const resolver::ResolvedTarget& r, bool req) {
        diagnostics::SignatureCheckRecord rec;
        rec.name = name;
        rec.required = req;
        rec.matchCount = r.patternMatchCount;
        rec.expectedCount = 1;
        rec.rva = r.rva;
        rec.armed = r.ok;
        rec.note = r.detail;
        g_healthReport.signatures.push_back(std::move(rec));
    };

    auto rSjis = resolver::ResolveTarget(g_layout, {
        "ALTextDraw_next_sjis",
        "55 8B EC 56 8B 75 08 57 8B 3E 8A 07 84 C0 74 35 3C 80 72 04 3C A0 72 04 3C E0 72 1B",
        nullptr, -1, 0x6e6ae0});
    if (rSjis.ok) g_nextSjisVa = rSjis.va;
    recordCheckOnly("ALTextDraw_next_sjis", rSjis, false);
    core::Logger::Instance().Logf("[RESOLVE] %-24s -> %s", "ALTextDraw_next_sjis", rSjis.detail.c_str());

    auto rUtf16 = resolver::ResolveTarget(g_layout, {
        "ALTextDraw_next_utf16",
        "55 8B EC 8B 55 08 8B 0A 0F B7 01 83 C1 02 89 0A 5D C3",
        nullptr, -1, 0x6e7990});
    if (rUtf16.ok) g_nextUtf16Va = rUtf16.va;
    recordCheckOnly("ALTextDraw_next_utf16", rUtf16, false);
    core::Logger::Instance().Logf("[RESOLVE] %-24s -> %s", "ALTextDraw_next_utf16", rUtf16.detail.c_str());

    auto rUtf32 = resolver::ResolveTarget(g_layout, {
        "ALTextDraw_next_utf32",
        "55 8B EC 8B 55 08 8B 0A 8B 01 83 C1 04 89 0A 5D C3",
        nullptr, -1, 0x6e79b0});
    if (rUtf32.ok) g_nextUtf32Va = rUtf32.va;
    recordCheckOnly("ALTextDraw_next_utf32", rUtf32, false);
    core::Logger::Instance().Logf("[RESOLVE] %-24s -> %s", "ALTextDraw_next_utf32", rUtf32.detail.c_str());

    if (!cfg.enableHooks) {
        diagnostics::EvaluateBuildSafety(g_healthReport, false, false);
        UpdateTextureAndWriteHealthcheck(true, true, true);
        core::Logger::Instance().LogLine("[HOOK] EnableHooks=0: 0 hooks armed.");
        return true;
    }

    int armed = 0;
    bool slot28Armed = false;
    bool slot40Armed = false;
    bool fontArmed = false;

    if (cfg.enableTextHooks) {
        slot28Armed = ArmMidHook(g_hTextDrawSlot28, {
            "ALTextDraw_slot28",
            "55 8B EC 83 EC 0C 53 8B D9 89 5D F8 8B 43 4C 8B 48 08 8B 01 FF 50 70 80 78 61 00",
            ".?AVALTextDraw@@", 28, 0x6e3b30}, &OnTextDrawSlot28, true);
        armed += slot28Armed ? 1 : 0;

        slot40Armed = ArmMidHook(g_hTextDrawSlot40, {
            "ALTextDraw_slot40",
            "55 8B EC 83 EC 10 53 56 57 8B F9 89 7D FC 8B 47 4C 8B 48 08 8B 01 FF 50 70 80 78 61 00",
            ".?AVALTextDraw@@", 40, 0x6e5ee0}, &OnTextDrawSlot40, true);
        armed += slot40Armed ? 1 : 0;

        // Fail-closed gate: if either primary display hook failed, disarm both immediately
        if (!slot28Armed || !slot40Armed) {
            core::Logger::Instance().LogLine("[SAFETY] Primary text hook gate failed (count!=1 or RTTI mismatch). Disarming text hooks.");
            g_hTextDrawSlot28.mid = {};
            g_hTextDrawSlot40.mid = {};
            slot28Armed = false;
            slot40Armed = false;
        }

        armed += ArmMidHook(g_hTextDrawStream, {
            "ALTextDraw_DrawStream",
            "55 8B EC 83 EC 48 53 56 8B F1 57 89 75 F8 8B 46 1C 8B 48 54 8B 01 FF 50 44",
            nullptr, -1, 0x6e3c40}, &OnTextDrawStream, false) ? 1 : 0;
    }

    if (cfg.logALTextProp) {
        armed += ArmMidHook(g_hTextProp70d350, {
            "ALTextProp_70d350",
            "55 8B EC 56 8B F1 8B 46 08 85 C0 74 13 8B 4E 04 50 8B 01 FF 90 D8 00 00 00 8B C8 E8 ? ? ? ?",
            nullptr, -1, 0x70d350}, &OnTextProp70d350, false) ? 1 : 0;
    }

    if (cfg.logALFontW3) {
        armed += ArmMidHook(g_hFontW3Slot12, {
            "ALFontImpW3_slot12",
            "55 8B EC 56 57 FF 75 08 8B F9 E8 ? ? ? ? 83 C4 04 8B 75 0C B9 E8 03 00 00",
            ".?AVALFontImpW3@@", 12, 0x7a3ae0}, &OnFontW3Slot12, false) ? 1 : 0;

        armed += ArmMidHook(g_hFontW3Slot14, {
            "ALFontImpW3_slot14",
            "55 8B EC 83 EC 50 A1 ? ? ? ? 33 C5 89 45 FC 53 8B D9 56 57 8B 7D 08 33 F6",
            ".?AVALFontImpW3@@", 14, 0x7a3820}, &OnFontW3Slot14, false) ? 1 : 0;
    }

    if (cfg.enableVirtualClick) {
        armed += ArmMidHook(g_hMousePoll, {
            "ALInput_mousePoll",
            "66 0F 6E 55 F4 66 0F 6E 5D F8 8B 0D ? ? ? ? 0F 5B D2 0F 5B DB",
            nullptr, -1, 0x7a1f12}, &OnMousePollMid, false) ? 1 : 0;
    }

    if (cfg.enableHangulFont && (slot28Armed && slot40Armed)) {
        const auto fontRes = resolver::ResolveTarget(g_layout, {
            "ALFontImp_dyn_GetOrCacheGlyph",
            "55 8B EC 83 EC 30 8B 45 08 53 8B D9 0F B7 C8 69 C0 43 B3 A6 FF 56 53 89 4D E0",
            nullptr, -1, 0x720d90});
        core::Logger::Instance().Logf("[RESOLVE] %-24s -> %s", "ALFontImp_dyn_GetOrCacheGlyph", fontRes.detail.c_str());
        diagnostics::SignatureCheckRecord fRec;
        fRec.name = "ALFontImp_dyn_GetOrCacheGlyph";
        fRec.required = false;
        fRec.matchCount = fontRes.patternMatchCount;
        fRec.expectedCount = 1;
        fRec.rva = fontRes.rva;
        fRec.note = fontRes.detail;
        if (fontRes.ok) {
            SetHangulFontEnabled(true);
            SetExtendedGlyphFallbackEnabled(cfg.enableExtendedGlyphFallback);
            ConfigureHangulFontStress(cfg.enableFontStress, cfg.fontStressCycles);
            fontArmed = InstallHangulFontHook(g_layout.base, fontRes.va);
            fRec.armed = fontArmed;
            armed += fontArmed ? 1 : 0;
        }
        g_healthReport.signatures.push_back(std::move(fRec));
    }

    if (cfg.enableTranslation) {
        std::string dictErr;
        if (g_translationCore.LoadFromJsonFile(cfg.translationsDbPath, &dictErr)) {
            uint64_t dbSize = 0;
            core::ComputeFileSha256Hex(cfg.translationsDbPath, g_translationDbSha256, dbSize);
            const std::string dbVerTag = ReadTranslationDbVersionTag(cfg.translationsDbPath);
            if (!cfg.templatesDbPath.empty()) {
                std::string tplErr;
                if (!g_translationCore.LoadTemplatesFromJsonFile(cfg.templatesDbPath, &tplErr)) {
                    core::Logger::Instance().Logf("[PHASE4] Warning: templates DB load failed: %s", tplErr.c_str());
                }
            }
            if (!cfg.noticeOverlayPath.empty()) {
                std::string ovErr;
                if (g_translationCore.LoadOverlayFromJsonFile(cfg.noticeOverlayPath, &ovErr)) {
                    core::Logger::Instance().LogLine("[R2] Loaded Notice overlay DB");
                } else {
                    core::Logger::Instance().Logf("[R2] Note: notice overlay not loaded: %s", ovErr.c_str());
                }
            }
            const auto st = g_translationCore.GetStats();
            g_translationDbVersion = dbVerTag +
                " (exact=" + std::to_string(st.loadedEntries) +
                ", tpl=" + std::to_string(st.loadedTemplates) +
                ", rx=" + std::to_string(st.loadedRegexRules) + ")";
            core::Logger::Instance().Logf(
                "[PHASE4] Loaded translations exact=%zu context=%zu templates=%zu regex=%zu",
                st.loadedEntries, st.loadedContextEntries, st.loadedTemplates, st.loadedRegexRules);
        } else {
            g_translationDbVersion = "MISSING";
            g_translationDbSha256.clear();
            core::Logger::Instance().Logf(
                "[PHASE4] Warning: translation DB not found/loaded (%s) -- falling back to original text",
                dictErr.c_str());
        }
    }

    if (cfg.autoTranslate.enabled) {
        g_autoTranslate.Initialize(cfg.autoTranslate, &g_coverageTracker);
        if (cfg.enableTranslation && cfg.autoTranslate.liveTextRefresh && slot28Armed && slot40Armed && g_hTextDrawStream.mid) {
            if (!InstallLiveTextRefresh(g_layout, g_autoTranslate, g_hTextDrawSlot28.targetRva, g_hTextDrawStream.targetRva))
                core::Logger::Instance().LogLine("[LIVE_REFRESH] unavailable: signature/layout gate failed; normal cache translation remains active");
        }
    }

    diagnostics::EvaluateBuildSafety(g_healthReport, slot28Armed && slot40Armed, fontArmed);
    UpdateTextureAndWriteHealthcheck(true, true, true);
    {
        diagnostics::HealthcheckReport summarySnap;
        {
            std::lock_guard<std::mutex> lock(g_healthMutex);
            summarySnap = g_healthReport;
        }
        auto shortSha = [](const std::string& sha) -> std::string {
            return sha.empty() ? std::string("NONE") : (sha.size() > 16 ? sha.substr(0, 16) : sha);
        };
        core::Logger::Instance().Logf("[SUMMARY] AigisKR %s", AIGISKR_VERSION_STR);
        core::Logger::Instance().Logf("[SUMMARY] Plugin: AigisKR.asi (%s)", AIGISKR_VERSION_STR);
        core::Logger::Instance().Logf("[SUMMARY] Translation DB: %s (sha256=%s)",
                                       summarySnap.translationDbVersion.c_str(),
                                       shortSha(summarySnap.translationDbSha256).c_str());
        core::Logger::Instance().Logf("[SUMMARY] Texture manifest: %s (sha256=%s)",
                                       summarySnap.textureManifestVersion.c_str(),
                                       shortSha(summarySnap.textureManifestSha256).c_str());
        core::Logger::Instance().Logf("[SUMMARY] Game EXE SHA-256: %s", summarySnap.sha256.c_str());
        core::Logger::Instance().Logf("[SUMMARY] Build state: %s", diagnostics::BuildSafetyStateName(summarySnap.state));
    }

    core::Logger::Instance().Logf("[HOOK] InstallProbeHooks complete: armed=%d", armed);
    return armed > 0;
}

void ShutdownProbeHooks(bool releaseHooks) {
    ShutdownLiveTextRefresh(releaseHooks);
    if (g_cfg.autoTranslate.enabled) {
        g_autoTranslate.Shutdown(releaseHooks);
    }
    LogHangulFontReport();
    ShutdownHangulFontHook(releaseHooks);
    UpdateTextureAndWriteHealthcheck(true, true, true, true);
    {
        const auto st = g_translationCore.GetStats();
        core::Logger::Instance().Logf(
            "[PHASE4_STATS] loaded=%zu lookups=%llu hits=%llu (exact=%llu ctx=%llu tpl=%llu rx=%llu) misses=%llu tag_fallbacks=%llu",
            st.loadedEntries, st.lookups, st.hits,
            st.exactHits, st.contextHits, st.templateHits, st.regexHits,
            st.misses, st.tagFallbacks);
    }
    if (g_cfg.autoTranslate.enabled) {
        const auto ast = g_autoTranslate.GetStats();
        const auto fst = GetHangulFontStats();
        core::Logger::Instance().Logf(
            "[AUTO_STATS] enabled=%d backend=%s init=%d ver=%s host_mode=%s host_spawned=%d host_spawn_count=%llu host_restart_count=%llu host_protocol_ver=%u host_ver=%s host_engine_ver=%s host_engine_root_type=%s ipc_requests=%llu ipc_completed=%llu ipc_failed=%llu ipc_timeout=%llu host_exit_code=%lld overrides=%zu seed_loaded=%zu runtime_loaded=%zu malformed=%zu rejected_lines=%zu stale_skipped=%zu cache_total=%zu override_hits=%llu cache_hits=%llu cache_misses=%llu filtered=%llu queued=%llu completed=%llu failed=%llu dedup=%llu dropped=%llu tag_rejected=%llu render_hdtrans_calls=%llu render_host_ipc_calls=%llu kl_loaded=%d kl_ver=%s kl_exact=%llu kl_pre=%llu kl_post=%llu kl_particles=%llu init_ms=%.1f avg_ms=%.2f max_ms=%.2f ext_glyph_calls=%llu unsupported_glyphs=%llu",
            ast.enabled ? 1 : 0,
            ast.backendAvailable ? "available" : "unavailable",
            ast.backendInitialized ? 1 : 0,
            ast.engineVersion.c_str(),
            ast.hostMode.c_str(),
            ast.hostSpawned ? 1 : 0,
            ast.hostSpawnCount,
            ast.hostRestartCount,
            static_cast<unsigned>(ast.hostProtocolVer),
            ast.hostVersion.empty() ? "none" : ast.hostVersion.c_str(),
            ast.hostEngineVer.empty() ? ast.engineVersion.c_str() : ast.hostEngineVer.c_str(),
            ast.hostEngineRootType.c_str(),
            ast.ipcRequests,
            ast.ipcCompleted,
            ast.ipcFailed,
            ast.ipcTimeout,
            static_cast<long long>(ast.hostExitCode),
            ast.overridesLoaded,
            ast.seedCacheLoaded,
            ast.runtimeCacheLoaded,
            ast.malformedCacheLines,
            ast.rejectedCacheLines,
            ast.staleCacheSkipped,
            ast.currentCacheEntries,
            ast.overrideHits,
            ast.cacheHits,
            ast.cacheMisses,
            ast.filteredOut,
            ast.jobsQueued,
            ast.jobsCompleted,
            ast.jobsFailed,
            ast.jobsDeduplicated,
            ast.jobsDroppedQueueFull,
            ast.tagRejected,
            ast.renderThreadBackendCalls,
            ast.renderThreadHostIpcCalls,
            ast.knowledgeLoaded ? 1 : 0,
            ast.knowledgeVersion.c_str(),
            ast.klExactHits,
            ast.klPreRewrites,
            ast.klPostRewrites,
            ast.klParticleFixes,
            ast.initMs,
            ast.avgTranslateMs,
            ast.maxTranslateMs,
            fst.extendedGlyphCalls,
            fst.unsupportedGlyphs);
    }
    if (g_cfg.enableCoverage) {
        std::string jsonPath, tsvPath;
        if (g_coverageTracker.FlushToDisk(&jsonPath, &tsvPath)) {
            const auto cs = g_coverageTracker.SnapshotSummary();
            const uint64_t tblCalls = cs.translatableCalls();
            const size_t tblUniq = cs.translatableUnique();
            const double callPct = tblCalls ? (100.0 * cs.translatedCalls() / tblCalls) : 0.0;
            const double uniqPct = tblUniq ? (100.0 * cs.translatedUnique() / tblUniq) : 0.0;
            const uint64_t coreTblCalls = cs.coreStaticTranslatableCalls();
            const double coreCallPct = coreTblCalls ? (100.0 * cs.coreStaticTranslatedCalls() / coreTblCalls) : 0.0;
            const uint64_t noticeTblCalls = cs.noticeTranslatableCalls();
            const double noticeCallPct = noticeTblCalls ? (100.0 * cs.noticeTranslatedCalls / noticeTblCalls) : 0.0;
            core::Logger::Instance().Logf(
                "[COVERAGE] call_cov=%.2f%% (%llu/%llu) uniq_cov=%.2f%% (%zu/%zu) core_static=%.2f%% (%llu/%llu) notice=%.2f%% (%llu/%llu) exact=%llu ctx=%llu tpl=%llu rx=%llu unm=%llu ign=%llu tag_rej=%llu json=%s",
                callPct, cs.translatedCalls(), tblCalls, uniqPct, cs.translatedUnique(), tblUniq,
                coreCallPct, cs.coreStaticTranslatedCalls(), coreTblCalls,
                noticeCallPct, cs.noticeTranslatedCalls, noticeTblCalls,
                cs.exactCalls, cs.contextCalls, cs.templateCalls, cs.regexCalls,
                cs.unmatchedCalls, cs.ignoredCalls, cs.tagRejectedCalls, jsonPath.c_str());
        }
    }
    core::Logger::Instance().Logf(
        "[STATS] slot28=%u slot40=%u drawStream=%u prop70d350=%u fontW3_12=%u fontW3_14=%u logged=%u",
        g_totalHitsTextDraw28.load(), g_totalHitsTextDraw40.load(),
        g_totalHitsDrawStream.load(), g_totalHitsProp70d350.load(),
        g_totalHitsFontW3_12.load(), g_totalHitsFontW3_14.load(),
        g_totalLogged.load());
    if (!releaseHooks) return;
    g_hTextDrawSlot28.mid = {};
    g_hTextDrawSlot40.mid = {};
    g_hTextDrawStream.mid = {};
    g_hTextProp70d350.mid = {};
    g_hFontW3Slot12.mid = {};
    g_hFontW3Slot14.mid = {};
    g_hMousePoll.mid = {};
}

}  // namespace aigiskr::hooks
