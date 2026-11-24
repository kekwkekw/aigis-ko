#include "core/logger.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

namespace aigiskr::core {

namespace {

std::mutex g_logMutex;
std::mutex g_capMutex;
FILE* g_logFile = nullptr;
FILE* g_capFile = nullptr;
ULONGLONG g_startTick = 0;

}  // namespace

std::wstring GetModuleDirectory(void* hmod) {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(reinterpret_cast<HMODULE>(hmod), buf, MAX_PATH);
    std::wstring path(buf, len);
    const size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : path.substr(0, pos + 1);
}

std::string WideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::string SanitizeCommandLine(const wchar_t* cmdline) {
    std::string utf8 = cmdline ? WideToUtf8(cmdline) : std::string();
    const char* secretKeys[] = {
        "/viewer_id=", "/onetime_token=", "/open_id=", "/pf_access_token=",
        "/token=", "/access_token=", "/session="
    };
    for (const char* key : secretKeys) {
        size_t pos = 0;
        while ((pos = utf8.find(key, pos)) != std::string::npos) {
            size_t valStart = pos + strlen(key);
            size_t valEnd = utf8.find_first_of(" \t\r\n\"", valStart);
            if (valEnd == std::string::npos) valEnd = utf8.size();
            utf8.replace(valStart, valEnd - valStart, "<REDACTED>");
            pos = valStart + 10;
        }
    }
    return utf8;
}

bool ComputeFileSha256Hex(const std::wstring& path, std::string& hexOut, uint64_t& sizeOut) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER liSize = {};
    if (!GetFileSizeEx(hFile, &liSize)) {
        CloseHandle(hFile);
        return false;
    }
    sizeOut = static_cast<uint64_t>(liSize.QuadPart);

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        CloseHandle(hFile);
        return false;
    }

    DWORD hashObjSize = 0, cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjSize), sizeof(DWORD), &cbData, 0);
    std::vector<uint8_t> hashObj(hashObjSize);

    if (BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjSize, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(hFile);
        return false;
    }

    std::vector<uint8_t> buf(65536);
    DWORD bytesRead = 0;
    while (ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr) && bytesRead > 0) {
        BCryptHashData(hHash, buf.data(), bytesRead, 0);
    }

    uint8_t digest[32] = {};
    bool ok = (BCryptFinishHash(hHash, digest, sizeof(digest), 0) == 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    CloseHandle(hFile);

    if (!ok) return false;
    static const char kHex[] = "0123456789abcdef";
    hexOut.clear();
    hexOut.reserve(64);
    for (uint8_t b : digest) {
        hexOut.push_back(kHex[b >> 4]);
        hexOut.push_back(kHex[b & 0x0F]);
    }
    return true;
}

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    Shutdown();
}

void Logger::Init(const std::wstring& logPath, const std::wstring& capturePath) {
    g_startTick = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (!g_logFile) {
            g_logFile = _wfsopen(logPath.c_str(), L"a", _SH_DENYNO);
        }
    }
    if (!capturePath.empty()) {
        std::lock_guard<std::mutex> lock(g_capMutex);
        if (!g_capFile) {
            bool needHeader = (GetFileAttributesW(capturePath.c_str()) == INVALID_FILE_ATTRIBUTES);
            g_capFile = _wfsopen(capturePath.c_str(), L"a", _SH_DENYNO);
            if (g_capFile && needHeader) {
                static const char kHdr[] =
                    "uptime_ms\ttid\thook_id\ttarget_rva\tcaller\tthis_ptr\tvtable_rva\trtti\tregs\tstr_ptr\treader\tbyte_len\tencoding\ttext\thex_preview\n";
                fwrite(kHdr, 1, sizeof(kHdr) - 1, g_capFile);
                fflush(g_capFile);
            }
        }
    }
}

void Logger::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logFile) {
            fflush(g_logFile);
            fclose(g_logFile);
            g_logFile = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_capMutex);
        if (g_capFile) {
            fflush(g_capFile);
            fclose(g_capFile);
            g_capFile = nullptr;
        }
    }
}

uint64_t Logger::UptimeMs() const {
    return g_startTick ? (GetTickCount64() - g_startTick) : 0;
}

void Logger::LogLine(std::string_view line) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logFile) return;
    fprintf(g_logFile, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [T%lu] %.*s\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentThreadId(),
            static_cast<int>(line.size()), line.data());
    fflush(g_logFile);
}

void Logger::Logf(const char* fmt, ...) {
    char msg[2048] = {};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    LogLine(msg);
}

void Logger::WriteCaptureTsv(std::string_view tsvLine) {
    std::lock_guard<std::mutex> lock(g_capMutex);
    if (!g_capFile) return;
    fwrite(tsvLine.data(), 1, tsvLine.size(), g_capFile);
    fputc('\n', g_capFile);
    fflush(g_capFile);
}

}  // namespace aigiskr::core
