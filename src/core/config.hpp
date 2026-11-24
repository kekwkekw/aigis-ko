#pragma once

#include <string>

namespace aigiskr::core {

enum class HdTransMode {
    Auto,
    Bundled,
    External,
};

const char* HdTransModeName(HdTransMode m) noexcept;

struct ProbeConfig {
    bool enableHooks = true;
    bool enableTextHooks = true;
    bool logTextEvents = false;
    bool logALTextProp = false;
    bool logALFontW3 = false;
    bool deduplicate = true;
    uint32_t maxCaptureEntries = 1500;
    uint32_t maxLogsPerSecond = 200;

    // Optional legacy dictionary path.
    std::wstring sampleDictPath;

    // R1 Full Localization DBs & Controls
    bool enableTranslation = true;
    std::wstring translationsDbPath;
    std::wstring templatesDbPath;
    std::wstring noticeOverlayPath;

    // Coverage telemetry
    bool enableCoverage = false;
    std::wstring coverageDir;
    std::wstring placeholdersPath;
    uint32_t coverageFlushIntervalMs = 15000;

    // Font hardening & diagnostic stress
    bool enableFontStress = false;
    uint32_t fontStressCycles = 2500;

    // Phase 5 GDI Hangul runtime font hook
    bool enableHangulFont = false;

    // Headless D3D11 backbuffer diagnostic snapshot & input driver
    bool captureBackbuffer = false;
    uint32_t captureIntervalMs = 2000;
    uint32_t maxSnapshots = 30;
    std::wstring snapshotDir;
    bool enableVirtualClick = false;
    std::wstring gameDir;
    std::wstring pluginDir;
    bool enableExtendedGlyphFallback = true;

    struct AutoTranslateConfig {
        bool enabled = false;
        bool liveTextRefresh = true;
        std::string provider = "hdTrans";
        HdTransMode hdTransMode = HdTransMode::Auto;
        std::wstring hdTransPath;         // External override path; empty by default.
        std::wstring bundledHdTransPath;  // Default: pluginDir\AigisKR\hdtrans
        std::wstring hdTransHostPath;     // Default: pluginDir\AigisKR\bin\AigisKR.HdTransHost.exe
        uint32_t hostTimeoutMs = 15000;
        bool cacheEnabled = true;
        std::wstring cacheFile;
        std::wstring seedCacheFile;
        std::wstring overridesFile;
        std::wstring knowledgeDir;
        bool retranslateStale = false;
        uint32_t workerThreads = 1;
        uint32_t queueMax = 4096;
        bool translateUI = true;
        bool translateStory = true;
        bool useSessionContext = false;
        size_t minLength = 1;
        size_t maxLength = 0; // 0: no optional limit; IPC payload bound still applies.
        bool logAutoTranslate = false;
    } autoTranslate;
};

using AutoTranslateConfig = ProbeConfig::AutoTranslateConfig;

ProbeConfig LoadProbeConfig(const std::wstring& iniPath,
                            const std::wstring& gameDir,
                            const std::wstring& pluginDir = L"");

}  // namespace aigiskr::core
