#include "core/config.hpp"
#include "core/logger.hpp"
#include "diagnostics/d3d11_capture.hpp"
#include "diagnostics/healthcheck.hpp"
#include "hooks/probe_hooks.hpp"

#include <windows.h>

#include <atomic>

namespace {

HMODULE g_selfModule = nullptr;
std::atomic<bool> g_initialized{false};

void RunProbeInit() {
    if (g_initialized.exchange(true)) return;

    const std::wstring gameDir = aigiskr::core::GetModuleDirectory(nullptr);
    const std::wstring pluginDir = aigiskr::core::GetModuleDirectory(g_selfModule);

    const std::wstring iniPath = pluginDir + L"AigisKR.ini";
    const std::wstring legacyIniPath = pluginDir + L"AigisKR_probe.ini";
    const bool iniMissing = (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES);
    const bool legacyIniPresent = (GetFileAttributesW(legacyIniPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    const auto cfg = aigiskr::core::LoadProbeConfig(iniPath, gameDir, pluginDir);

    const std::wstring logPath = gameDir + L"AigisKR.log";
    const std::wstring capPath = gameDir + L"AigisKR_text_capture.tsv";
    const bool wantCaptureFile = (cfg.logTextEvents || cfg.logALTextProp || cfg.logALFontW3);
    auto& log = aigiskr::core::Logger::Instance();
    log.Init(logPath, wantCaptureFile ? capPath : std::wstring());

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::string exeSha;
    uint64_t exeSize = 0;
    aigiskr::core::ComputeFileSha256Hex(exePath, exeSha, exeSize);

    log.Logf("=== AigisKR %s InitializeASI ===", AIGISKR_VERSION_STR);
    if (iniMissing && legacyIniPresent) {
        log.LogLine("[CONFIG] Warning: legacy AigisKR_probe.ini ignored; canonical config is AigisKR.ini (using release-safe defaults)");
    } else if (iniMissing) {
        log.LogLine("[CONFIG] AigisKR.ini not found; using release-safe defaults");
    }
    log.Logf("pid=%lu exe=%s exe_base=0x%p plugin_base=0x%p",
             GetCurrentProcessId(),
             aigiskr::core::WideToUtf8(exePath).c_str(),
             GetModuleHandleW(nullptr),
             g_selfModule);
    log.Logf("exe_size=%llu sha256=%s", static_cast<unsigned long long>(exeSize), exeSha.c_str());
    log.Logf("cmdline=%s", aigiskr::core::SanitizeCommandLine(GetCommandLineW()).c_str());
    log.Logf("ini=%s enable_hooks=%d enable_text_hooks=%d log_text_events=%d enable_coverage=%d dedup=%d max_entries=%u capture_bb=%d",
             aigiskr::core::WideToUtf8(iniPath).c_str(),
             cfg.enableHooks ? 1 : 0,
             cfg.enableTextHooks ? 1 : 0,
             cfg.logTextEvents ? 1 : 0,
             cfg.enableCoverage ? 1 : 0,
             cfg.deduplicate ? 1 : 0,
             cfg.maxCaptureEntries,
             cfg.captureBackbuffer ? 1 : 0);

    aigiskr::hooks::InstallProbeHooks(cfg);

    if (cfg.captureBackbuffer) {
        aigiskr::diagnostics::CaptureOptions copts;
        copts.enabled = true;
        copts.intervalMs = cfg.captureIntervalMs;
        copts.maxSnapshots = cfg.maxSnapshots;
        copts.snapshotDir = cfg.snapshotDir;
        copts.gameDir = gameDir;
        aigiskr::diagnostics::StartBackbufferCapture(copts);
    }
}

}  // namespace

extern "C" __declspec(dllexport) void InitializeASI() {
    RunProbeInit();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_selfModule = hModule;
        DisableThreadLibraryCalls(hModule);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        if (g_initialized.load()) {
            const bool cleanUnload = (lpReserved == nullptr);
            aigiskr::diagnostics::StopBackbufferCapture(cleanUnload);
            aigiskr::hooks::ShutdownProbeHooks(cleanUnload);
            aigiskr::core::Logger::Instance().Logf("=== AigisKR %s DETACH ===", AIGISKR_VERSION_STR);
            aigiskr::core::Logger::Instance().Shutdown();
        }
    }
    return TRUE;
}
