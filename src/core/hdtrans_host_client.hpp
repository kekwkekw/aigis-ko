#pragma once

#include "core/config.hpp"
#include "core/hdtrans_adapter.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace aigiskr::core {

struct HdTransSelectionResult {
    bool available = false;
    HdTransMode requestedMode = HdTransMode::Auto;
    std::string rootType = "none";  // "bundled", "external", "none"
    std::wstring resolvedEngineRoot;
    std::string warning;
    std::string detail;
};

/// Probe whether a directory contains a valid hdTrans engine layout (J2KEngine.dll + dicts.hdsnap).
bool ProbeHdTransEngineRoot(const std::wstring& rootPath, std::string* detailOut = nullptr);

/// Resolve host executable + Bundled/External/Auto hdTrans selection cleanly without loading J2KEngine.dll.
HdTransSelectionResult ResolveHdTransSelection(const AutoTranslateConfig& cfg);

/// Out-of-process IPC client managing AigisKR.HdTransHost.exe over anonymous stdin/stdout pipes.
class HdTransHostClient final : public IAutoTranslateBackend {
public:
    HdTransHostClient() = default;
    ~HdTransHostClient() override;

    HdTransHostClient(const HdTransHostClient&) = delete;
    HdTransHostClient& operator=(const HdTransHostClient&) = delete;

    void Configure(std::wstring hostExePath,
                   std::string engineRootType,
                   uint32_t timeoutMs = 15000);

    bool ProbeAvailability(const std::wstring& rootPath, std::string* detailOut = nullptr) const override;
    bool Initialize(const std::wstring& rootPath, std::string* detailOut = nullptr) override;
    bool TranslateUtf8(std::string_view inputUtf8,
                       std::string& outTranslatedUtf8,
                       std::string* errorOut = nullptr) override;
    void Shutdown(bool allowCleanWait = true) override;

    bool IsInitialized() const noexcept override { return initialized_; }
    std::string EngineVersion() const override { return engineVersion_; }
    BackendFailureKind LastFailureKind() const noexcept override { return lastFailureKind_; }
    HostTelemetrySnapshot GetHostTelemetry() const override;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    mutable std::mutex statsMutex_;
    std::wstring hostExePath_;
    std::string engineRootType_ = "none";
    uint32_t timeoutMs_ = 15000;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> hostSpawned_{false};
    std::atomic<uint64_t> hostSpawnCount_{0};
    std::atomic<uint16_t> hostProtocolVersion_{0};
    std::string hostVersion_;
    std::string engineVersion_;
    std::string dictsVersion_;
    std::atomic<uint64_t> ipcRequests_{0};
    std::atomic<uint64_t> ipcCompleted_{0};
    std::atomic<uint64_t> ipcFailed_{0};
    std::atomic<uint64_t> ipcTimeout_{0};
    std::atomic<int64_t> hostExitCode_{-1};
    uint32_t nextRequestId_ = 1;
    std::atomic<BackendFailureKind> lastFailureKind_{BackendFailureKind::None};
};

}  // namespace aigiskr::core
