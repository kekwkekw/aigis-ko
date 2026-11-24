#include "core/hdtrans_host_client.hpp"

#include "core/logger.hpp"
#include "hdtrans_host/host_protocol.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

namespace aigiskr::core {

namespace {

std::wstring TrimTrailingSlashesW(std::wstring p) {
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) {
        p.pop_back();
    }
    return p;
}

bool FileExistsW(const std::wstring& path) noexcept {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExistsW(const std::wstring& path) noexcept {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

struct TimeoutWatchState {
    HANDLE hProcess = nullptr;
    HANDLE hCallingThread = nullptr;
    std::atomic<bool> timedOut{false};
};

VOID CALLBACK OnIpcTimeoutCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
    auto* state = static_cast<TimeoutWatchState*>(context);
    if (!state) return;
    state->timedOut.store(true, std::memory_order_release);
    if (state->hProcess) {
        TerminateProcess(state->hProcess, 1);
    }
    if (state->hCallingThread) {
        CancelSynchronousIo(state->hCallingThread);
    }
}

void SafeCloseHandle(HANDLE& h) noexcept {
    if (h && h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
    h = nullptr;
}

}  // namespace

bool ProbeHdTransEngineRoot(const std::wstring& rootPath, std::string* detailOut) {
    const std::wstring base = TrimTrailingSlashesW(rootPath);
    if (base.empty()) {
        if (detailOut) *detailOut = "hdTrans root path is empty";
        return false;
    }
    if (!DirExistsW(base)) {
        if (detailOut) *detailOut = "directory does not exist: '" + WideToUtf8(base) + "'";
        return false;
    }

    const std::wstring dllPath = base + L"\\J2KEngine.dll";
    if (!FileExistsW(dllPath)) {
        if (detailOut) *detailOut = "J2KEngine.dll not found under '" + WideToUtf8(base) + "'";
        return false;
    }

    const std::wstring subSnap = base + L"\\hdtrans-data\\dicts.hdsnap";
    const std::wstring rootSnap = base + L"\\dicts.hdsnap";
    if (!FileExistsW(subSnap) && !FileExistsW(rootSnap)) {
        if (detailOut) *detailOut = "dicts.hdsnap not found under '" + WideToUtf8(base) + "\\hdtrans-data' or root";
        return false;
    }

    if (detailOut) *detailOut = "ready";
    return true;
}

HdTransSelectionResult ResolveHdTransSelection(const AutoTranslateConfig& cfg) {
    HdTransSelectionResult res;
    res.requestedMode = cfg.hdTransMode;

    if (!FileExistsW(cfg.hdTransHostPath)) {
        res.available = false;
        res.rootType = "none";
        res.detail = "AigisKR.HdTransHost.exe not found at '" + WideToUtf8(cfg.hdTransHostPath) + "'";
        return res;
    }

    if (cfg.hdTransMode == HdTransMode::Bundled) {
        std::string bErr;
        if (ProbeHdTransEngineRoot(cfg.bundledHdTransPath, &bErr)) {
            res.available = true;
            res.rootType = "bundled";
            res.resolvedEngineRoot = TrimTrailingSlashesW(cfg.bundledHdTransPath);
            res.detail = "bundled(" + WideToUtf8(res.resolvedEngineRoot) + ")";
        } else {
            res.available = false;
            res.rootType = "none";
            res.detail = "Bundled hdTrans unavailable (" + bErr + ")";
        }
        return res;
    }

    if (cfg.hdTransMode == HdTransMode::External) {
        if (cfg.hdTransPath.empty()) {
            res.available = false;
            res.rootType = "none";
            res.detail = "External mode requires non-empty HdTransPath";
            return res;
        }
        std::string eErr;
        if (ProbeHdTransEngineRoot(cfg.hdTransPath, &eErr)) {
            res.available = true;
            res.rootType = "external";
            res.resolvedEngineRoot = TrimTrailingSlashesW(cfg.hdTransPath);
            res.detail = "external(" + WideToUtf8(res.resolvedEngineRoot) + ")";
        } else {
            res.available = false;
            res.rootType = "none";
            res.detail = "External hdTrans unavailable (" + eErr + ")";
        }
        return res;
    }

    // HdTransMode::Auto
    if (!cfg.hdTransPath.empty()) {
        std::string eErr;
        if (ProbeHdTransEngineRoot(cfg.hdTransPath, &eErr)) {
            res.available = true;
            res.rootType = "external";
            res.resolvedEngineRoot = TrimTrailingSlashesW(cfg.hdTransPath);
            res.detail = "auto->external(" + WideToUtf8(res.resolvedEngineRoot) + ")";
            return res;
        }
        res.warning = "Configured HdTransPath invalid (" + eErr + "); falling back to Bundled hdTrans";
    }

    std::string bErr;
    if (ProbeHdTransEngineRoot(cfg.bundledHdTransPath, &bErr)) {
        res.available = true;
        res.rootType = "bundled";
        res.resolvedEngineRoot = TrimTrailingSlashesW(cfg.bundledHdTransPath);
        res.detail = "auto->bundled(" + WideToUtf8(res.resolvedEngineRoot) + ")";
    } else {
        res.available = false;
        res.rootType = "none";
        res.detail = "No bundled or external hdTrans engine available (" + bErr + ")";
    }
    return res;
}

struct HdTransHostClient::Impl {
    HANDLE hJob = nullptr;
    HANDLE hProcess = nullptr;
    HANDLE hStdinWrite = nullptr;
    HANDLE hStdoutRead = nullptr;
    std::wstring activeEngineRoot;
};

HdTransHostClient::~HdTransHostClient() {
    Shutdown(true);
    delete impl_;
    impl_ = nullptr;
}

void HdTransHostClient::Configure(std::wstring hostExePath,
                                  std::string engineRootType,
                                  uint32_t timeoutMs) {
    std::lock_guard<std::mutex> lock(statsMutex_);
    hostExePath_ = std::move(hostExePath);
    engineRootType_ = engineRootType.empty() ? std::string("none") : std::move(engineRootType);
    timeoutMs_ = timeoutMs < 50u ? 50u : timeoutMs;
}

bool HdTransHostClient::ProbeAvailability(const std::wstring& rootPath, std::string* detailOut) const {
    if (!FileExistsW(hostExePath_)) {
        if (detailOut) *detailOut = "Host executable missing: '" + WideToUtf8(hostExePath_) + "'";
        return false;
    }
    return ProbeHdTransEngineRoot(rootPath, detailOut);
}

bool HdTransHostClient::Initialize(const std::wstring& rootPath, std::string* detailOut) {
    if (initialized_) {
        return true;
    }

    lastFailureKind_ = BackendFailureKind::None;
    const std::wstring cleanRoot = TrimTrailingSlashesW(rootPath);
    std::string probeErr;
    if (!ProbeAvailability(cleanRoot, &probeErr)) {
        if (detailOut) *detailOut = probeErr;
        return false;
    }

    if (!impl_) {
        impl_ = new Impl();
    }

    // Ensure no stale handles survive across restarts.
    Shutdown(true);
    if (!impl_) {
        impl_ = new Impl();
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE hChildStdinRead = nullptr;
    HANDLE hParentStdinWrite = nullptr;
    HANDLE hParentStdoutRead = nullptr;
    HANDLE hChildStdoutWrite = nullptr;
    HANDLE hChildStderr = nullptr;

    auto closeTempHandles = [&]() {
        SafeCloseHandle(hChildStdinRead);
        SafeCloseHandle(hParentStdinWrite);
        SafeCloseHandle(hParentStdoutRead);
        SafeCloseHandle(hChildStdoutWrite);
        SafeCloseHandle(hChildStderr);
    };

    if (!CreatePipe(&hChildStdinRead, &hParentStdinWrite, &sa, 0) ||
        !SetHandleInformation(hParentStdinWrite, HANDLE_FLAG_INHERIT, 0)) {
        if (detailOut) *detailOut = "CreatePipe(stdin) failed";
        closeTempHandles();
        return false;
    }
    if (!CreatePipe(&hParentStdoutRead, &hChildStdoutWrite, &sa, 0) ||
        !SetHandleInformation(hParentStdoutRead, HANDLE_FLAG_INHERIT, 0)) {
        if (detailOut) *detailOut = "CreatePipe(stdout) failed";
        closeTempHandles();
        return false;
    }

    hChildStderr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hChildStderr == INVALID_HANDLE_VALUE) {
        hChildStderr = nullptr;
    }

    std::wstring cmdLine = L"\"" + hostExePath_ + L"\" --engine-root \"" + cleanRoot +
                           L"\" --parent-pid " + std::to_wstring(GetCurrentProcessId());
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hChildStdinRead;
    si.hStdOutput = hChildStdoutWrite;
    si.hStdError = hChildStderr ? hChildStderr : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    const auto t0 = std::chrono::steady_clock::now();
    const BOOL okProc = CreateProcessW(
        hostExePath_.c_str(),
        cmdBuf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        cleanRoot.c_str(),
        &si,
        &pi);

    // Parent must close child-side pipe ends immediately so broken-pipe detection works.
    SafeCloseHandle(hChildStdinRead);
    SafeCloseHandle(hChildStdoutWrite);
    SafeCloseHandle(hChildStderr);

    if (!okProc) {
        const DWORD err = GetLastError();
        closeTempHandles();
        if (detailOut) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "CreateProcessW failed (err=%lu)", err);
            *detailOut = msg;
        }
        lastFailureKind_ = BackendFailureKind::TransportCrash;
        return false;
    }

    SafeCloseHandle(pi.hThread);
    impl_->hProcess = pi.hProcess;
    impl_->hStdinWrite = hParentStdinWrite;
    impl_->hStdoutRead = hParentStdoutRead;
    impl_->activeEngineRoot = cleanRoot;

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) ||
            !AssignProcessToJobObject(job, impl_->hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    impl_->hJob = job;
    hostSpawned_ = true;
    ++hostSpawnCount_;
    hostExitCode_ = -1;

    const std::string helloReq =
        std::string("protocol=") + std::to_string(hdtrans_host::kProtocolVersion) +
        "\nengine_root=" + WideToUtf8(cleanRoot) + "\n";

    std::string helloResp;
    hdtrans_host::ResponseHeader respHdr{};

    TimeoutWatchState watch{};
    watch.hProcess = impl_->hProcess;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &watch.hCallingThread, THREAD_TERMINATE | SYNCHRONIZE, FALSE, 0);
    PTP_TIMER timer = CreateThreadpoolTimer(&OnIpcTimeoutCallback, &watch, nullptr);
    if (!timer || !watch.hCallingThread) {
        if (timer) CloseThreadpoolTimer(timer);
        SafeCloseHandle(watch.hCallingThread);
        lastFailureKind_ = BackendFailureKind::TransportCrash;
        if (detailOut) *detailOut = "Cannot establish HELLO timeout watchdog";
        Shutdown(false);
        return false;
    }
    if (timer) {
        ULARGE_INTEGER due;
        due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(timeoutMs_) * 10000LL);
        FILETIME ft{due.LowPart, due.HighPart};
        SetThreadpoolTimer(timer, &ft, 0, 0);
    }

    ++ipcRequests_;
    const uint32_t rid = nextRequestId_++;
    bool ioOk = hdtrans_host::WriteRequestFrame(impl_->hStdinWrite, hdtrans_host::Opcode::Hello, rid, helloReq);
    hdtrans_host::PipeIoStatus ioSt = hdtrans_host::PipeIoStatus::Error;
    if (ioOk) {
        ioSt = hdtrans_host::ReadResponseFrame(impl_->hStdoutRead, respHdr, helloResp);
        ioOk = (ioSt == hdtrans_host::PipeIoStatus::Ok && respHdr.requestId == rid);
    }

    if (timer) {
        SetThreadpoolTimer(timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(timer, TRUE);
        CloseThreadpoolTimer(timer);
    }
    SafeCloseHandle(watch.hCallingThread);

    if (watch.timedOut.load(std::memory_order_acquire)) {
        ++ipcFailed_;
        ++ipcTimeout_;
        lastFailureKind_ = BackendFailureKind::Timeout;
        if (detailOut) *detailOut = "HELLO handshake timed out";
        Shutdown(true);
        return false;
    }

    if (!ioOk) {
        ++ipcFailed_;
        lastFailureKind_ = BackendFailureKind::TransportCrash;
        if (detailOut) *detailOut = "HELLO IPC read/write failed";
        Shutdown(true);
        return false;
    }

    if (respHdr.status != static_cast<uint16_t>(hdtrans_host::Status::Ok)) {
        ++ipcFailed_;
        lastFailureKind_ = BackendFailureKind::TransportCrash;
        const std::string errTxt = hdtrans_host::ExtractKvField(helloResp, "error");
        if (detailOut) {
            *detailOut = errTxt.empty() ? ("HELLO failed status=" + std::to_string(respHdr.status)) : errTxt;
        }
        Shutdown(true);
        return false;
    }

    ++ipcCompleted_;
    hostProtocolVersion_.store(respHdr.protocolVersion, std::memory_order_relaxed);
    std::string localEngVer = hdtrans_host::ExtractKvField(helloResp, "engine_version");
    if (localEngVer.empty()) {
        localEngVer = "hdTrans";
    }
    std::string localHostVer = hdtrans_host::ExtractKvField(helloResp, "host_version");
    std::string localDictsVer = hdtrans_host::ExtractKvField(helloResp, "dicts_version");
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        hostVersion_ = localHostVer;
        engineVersion_ = localEngVer;
        dictsVersion_ = localDictsVer;
    }
    initialized_.store(true, std::memory_order_release);

    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (detailOut) {
        char buf[384];
        std::snprintf(buf, sizeof(buf), "host handshake ok mode=%s proto=%u engine=%s in %.1f ms",
                      engineRootType_.c_str(),
                      static_cast<unsigned>(hostProtocolVersion_.load(std::memory_order_relaxed)),
                      localEngVer.c_str(),
                      ms);
        *detailOut = buf;
    }
    return true;
}

bool HdTransHostClient::TranslateUtf8(std::string_view inputUtf8,
                                      std::string& outTranslatedUtf8,
                                      std::string* errorOut) {
    outTranslatedUtf8.clear();
    lastFailureKind_ = BackendFailureKind::None;

    if (!initialized_ || !impl_ || !impl_->hProcess || !impl_->hStdinWrite || !impl_->hStdoutRead) {
        lastFailureKind_ = BackendFailureKind::TransportCrash;
        if (errorOut) *errorOut = "host_not_initialized";
        return false;
    }
    if (inputUtf8.size() > hdtrans_host::kMaxPayloadBytes) {
        lastFailureKind_ = BackendFailureKind::TranslateError;
        if (errorOut) *errorOut = "payload_exceeds_64KiB";
        return false;
    }

    TimeoutWatchState watch{};
    watch.hProcess = impl_->hProcess;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &watch.hCallingThread, THREAD_TERMINATE | SYNCHRONIZE, FALSE, 0);
    PTP_TIMER timer = CreateThreadpoolTimer(&OnIpcTimeoutCallback, &watch, nullptr);
    if (!timer || !watch.hCallingThread) {
        if (timer) CloseThreadpoolTimer(timer);
        SafeCloseHandle(watch.hCallingThread);
        ++ipcFailed_;
        lastFailureKind_ = BackendFailureKind::TransportCrash;
        if (errorOut) *errorOut = "Cannot establish translation timeout watchdog";
        Shutdown(false);
        return false;
    }
    if (timer) {
        ULARGE_INTEGER due;
        due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(timeoutMs_) * 10000LL);
        FILETIME ft{due.LowPart, due.HighPart};
        SetThreadpoolTimer(timer, &ft, 0, 0);
    }

    ++ipcRequests_;
    const uint32_t rid = nextRequestId_++;
    bool writeOk = hdtrans_host::WriteRequestFrame(impl_->hStdinWrite, hdtrans_host::Opcode::Translate, rid, inputUtf8);
    hdtrans_host::ResponseHeader respHdr{};
    std::string respBody;
    hdtrans_host::PipeIoStatus readSt = hdtrans_host::PipeIoStatus::Error;
    if (writeOk) {
        readSt = hdtrans_host::ReadResponseFrame(impl_->hStdoutRead, respHdr, respBody);
    }

    if (timer) {
        SetThreadpoolTimer(timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(timer, TRUE);
        CloseThreadpoolTimer(timer);
    }
    SafeCloseHandle(watch.hCallingThread);

    if (watch.timedOut.load(std::memory_order_acquire)) {
        ++ipcFailed_;
        ++ipcTimeout_;
        lastFailureKind_ = BackendFailureKind::Timeout;
        if (errorOut) *errorOut = "ipc_timeout";
        Shutdown(true);
        return false;
    }

    if (!writeOk || readSt != hdtrans_host::PipeIoStatus::Ok || respHdr.requestId != rid) {
        ++ipcFailed_;
        lastFailureKind_ = BackendFailureKind::TransportCrash;
        if (errorOut) *errorOut = "host_pipe_transport_failure";
        Shutdown(true);
        return false;
    }

    if (respHdr.status != static_cast<uint16_t>(hdtrans_host::Status::Ok)) {
        ++ipcFailed_;
        lastFailureKind_ = BackendFailureKind::TranslateError;
        if (errorOut) {
            *errorOut = respBody.empty() ? ("translate_status_" + std::to_string(respHdr.status)) : respBody;
        }
        return false;
    }

    // Never let malformed or NUL-truncated text enter the game or persistent cache.
    const bool validText = respBody.find('\0') == std::string::npos &&
        (respBody.empty() ? inputUtf8.empty() :
         MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, respBody.data(),
                             static_cast<int>(respBody.size()), nullptr, 0) > 0);
    if (!validText) {
        ++ipcFailed_;
        lastFailureKind_ = BackendFailureKind::TransportCrash;
        if (errorOut) *errorOut = "invalid_utf8_translation_response";
        Shutdown(false);
        return false;
    }
    ++ipcCompleted_;
    outTranslatedUtf8 = std::move(respBody);
    return true;
}

void HdTransHostClient::Shutdown(bool allowCleanWait) {
    initialized_ = false;
    if (!impl_) {
        return;
    }

    if (impl_->hProcess) {
        DWORD code = STILL_ACTIVE;
        const bool alive = (GetExitCodeProcess(impl_->hProcess, &code) && code == STILL_ACTIVE);
        // EOF is a graceful shutdown signal understood by the Host. Do not issue
        // synchronous pipe I/O here: a hung/malformed Host may never send an ACK,
        // and even a SHUTDOWN write can block behind a full input pipe.
        SafeCloseHandle(impl_->hStdinWrite);
        SafeCloseHandle(impl_->hStdoutRead);
        if (alive) {
            const DWORD waitMs = allowCleanWait ? 1500u : 120u;
            if (WaitForSingleObject(impl_->hProcess, waitMs) != WAIT_OBJECT_0) {
                TerminateProcess(impl_->hProcess, 0);
                WaitForSingleObject(impl_->hProcess, 250u);
            }
        }

        DWORD finalCode = 0;
        if (GetExitCodeProcess(impl_->hProcess, &finalCode) && finalCode != STILL_ACTIVE) {
            hostExitCode_ = static_cast<int64_t>(finalCode);
        }
    }

    SafeCloseHandle(impl_->hStdinWrite);
    SafeCloseHandle(impl_->hStdoutRead);
    SafeCloseHandle(impl_->hProcess);
    SafeCloseHandle(impl_->hJob);
}

HostTelemetrySnapshot HdTransHostClient::GetHostTelemetry() const {
    HostTelemetrySnapshot s;
    s.isHostBackend = true;
    s.hostSpawned = hostSpawned_.load(std::memory_order_relaxed);
    s.hostSpawnCount = hostSpawnCount_.load(std::memory_order_relaxed);
    s.hostProtocolVersion = hostProtocolVersion_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        s.hostVersion = hostVersion_;
        s.engineVersion = engineVersion_;
        s.dictsVersion = dictsVersion_;
        s.engineRootType = engineRootType_;
    }
    s.ipcRequests = ipcRequests_.load(std::memory_order_relaxed);
    s.ipcCompleted = ipcCompleted_.load(std::memory_order_relaxed);
    s.ipcFailed = ipcFailed_.load(std::memory_order_relaxed);
    s.ipcTimeout = ipcTimeout_.load(std::memory_order_relaxed);
    s.hostExitCode = hostExitCode_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace aigiskr::core
