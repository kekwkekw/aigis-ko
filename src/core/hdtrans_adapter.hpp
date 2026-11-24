#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aigiskr::core {

enum class BackendFailureKind {
    None,
    TranslateError,
    TransportCrash,
    Timeout,
};

struct HostTelemetrySnapshot {
    bool isHostBackend = false;
    bool hostSpawned = false;
    uint64_t hostSpawnCount = 0;
    uint16_t hostProtocolVersion = 0;
    std::string hostVersion;
    std::string engineVersion;
    std::string dictsVersion;
    std::string engineRootType = "none";  // "bundled", "external", "none"
    uint64_t ipcRequests = 0;
    uint64_t ipcCompleted = 0;
    uint64_t ipcFailed = 0;
    uint64_t ipcTimeout = 0;
    int64_t hostExitCode = -1;
};

/// Abstraction over a local offline runtime translation backend (e.g. hdTrans Host or test double).
class IAutoTranslateBackend {
public:
    virtual ~IAutoTranslateBackend() = default;

    /// Check quickly whether the configured runtime files appear present on disk.
    virtual bool ProbeAvailability(const std::wstring& rootPath, std::string* detailOut = nullptr) const = 0;

    /// Initialize backend dictionaries/state (called only on background worker thread).
    virtual bool Initialize(const std::wstring& rootPath, std::string* detailOut = nullptr) = 0;

    /// Translate one masked UTF-8 string into Korean UTF-8 (called only on background worker thread).
    virtual bool TranslateUtf8(std::string_view inputUtf8,
                               std::string& outTranslatedUtf8,
                               std::string* errorOut = nullptr) = 0;

    /// Release backend resources cleanly (skip unsafe unload steps if unloadModule=false during detach).
    virtual void Shutdown(bool unloadModule = true) = 0;

    virtual bool IsInitialized() const noexcept = 0;
    virtual std::string EngineVersion() const = 0;
    virtual BackendFailureKind LastFailureKind() const noexcept { return BackendFailureKind::None; }
    virtual HostTelemetrySnapshot GetHostTelemetry() const { return {}; }
};

/// 32-bit x86 J2KEngine.dll adapter for local hdTrans (e.g. D:\hdTrans).
class HdTransJ2KAdapter final : public IAutoTranslateBackend {
public:
    HdTransJ2KAdapter() = default;
    ~HdTransJ2KAdapter() override;

    HdTransJ2KAdapter(const HdTransJ2KAdapter&) = delete;
    HdTransJ2KAdapter& operator=(const HdTransJ2KAdapter&) = delete;

    bool ProbeAvailability(const std::wstring& rootPath, std::string* detailOut = nullptr) const override;
    bool Initialize(const std::wstring& rootPath, std::string* detailOut = nullptr) override;
    bool TranslateUtf8(std::string_view inputUtf8,
                       std::string& outTranslatedUtf8,
                       std::string* errorOut = nullptr) override;
    void Shutdown(bool unloadModule = true) override;

    bool IsInitialized() const noexcept override { return initialized_; }
    std::string EngineVersion() const override { return engineVersion_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool initialized_ = false;
    std::string engineVersion_;
};

}  // namespace aigiskr::core
