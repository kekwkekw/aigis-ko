#include "core/hdtrans_adapter.hpp"

#include "core/logger.hpp"
#include "core/tag_parser.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace aigiskr::core {

namespace {

using J2KInitFn = int(__stdcall*)(const char* dataDir);
using J2KInitExFn = int(__stdcall*)(const char* dataDir, const char* key);
using J2KTransWFn = wchar_t*(__stdcall*)(HWND hwnd, const wchar_t* text);
using J2KFreeFn = void(__stdcall*)(void* ptr);
using J2KTermFn = void(__cdecl*)();

std::wstring TrimTrailingSlashes(std::wstring p) {
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) {
        p.pop_back();
    }
    return p;
}

bool FileExistsW(const std::wstring& path) noexcept {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ResolveHdTransPaths(const std::wstring& rootPath,
                         std::wstring& outDllPath,
                         std::wstring& outDataDir,
                         std::string* detailOut) {
    const std::wstring base = TrimTrailingSlashes(rootPath);
    if (base.empty()) {
        if (detailOut) *detailOut = "HdTransPath is empty";
        return false;
    }

    const std::wstring dll = base + L"\\J2KEngine.dll";
    if (!FileExistsW(dll)) {
        if (detailOut) *detailOut = "J2KEngine.dll not found under '" + WideToUtf8(base) + "'";
        return false;
    }

    const std::wstring subData = base + L"\\hdtrans-data";
    const std::wstring subSnap = subData + L"\\dicts.hdsnap";
    const std::wstring rootSnap = base + L"\\dicts.hdsnap";

    if (FileExistsW(subSnap)) {
        outDllPath = dll;
        outDataDir = subData;
        return true;
    }
    if (FileExistsW(rootSnap)) {
        outDllPath = dll;
        outDataDir = base;
        return true;
    }

    if (detailOut) {
        *detailOut = "dicts.hdsnap not found in '" + WideToUtf8(subData) + "' or '" + WideToUtf8(base) + "'";
    }
    return false;
}

std::string ReadDictsVersionTag(const std::wstring& dataDir) {
    const std::wstring txtPath = dataDir + L"\\dicts.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, txtPath.c_str(), L"rb") != 0 || !f) {
        return "hdTrans-J2K";
    }
    char buf[128] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string ver(buf, n);
    while (!ver.empty() && (ver.back() == '\r' || ver.back() == '\n' || ver.back() == ' ')) {
        ver.pop_back();
    }
    return ver.empty() ? std::string("hdTrans-J2K") : ("hdTrans/" + ver);
}

bool WidePathToAnsi(const std::wstring& widePath, std::string& outAnsi) {
    auto convert = [](const std::wstring& w, std::string& dst) -> bool {
        BOOL usedDefault = FALSE;
        const int needed = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
                                               w.c_str(), -1, nullptr, 0, nullptr, &usedDefault);
        if (needed <= 1 || usedDefault) {
            return false;
        }
        std::string buf(static_cast<size_t>(needed - 1), '\0');
        const int wrote = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
                                              w.c_str(), -1, buf.data(), needed, nullptr, &usedDefault);
        if (wrote <= 1 || usedDefault) {
            return false;
        }
        dst = std::move(buf);
        return true;
    };

    if (convert(widePath, outAnsi)) {
        return true;
    }
    // Non-ACP directory names: fall back to Win32 8.3 short path for ANSI APIs.
    wchar_t shortBuf[MAX_PATH * 2] = {};
    const DWORD shortLen = GetShortPathNameW(widePath.c_str(), shortBuf, static_cast<DWORD>(_countof(shortBuf)));
    if (shortLen > 0 && shortLen < _countof(shortBuf)) {
        return convert(std::wstring(shortBuf, shortLen), outAnsi);
    }
    return false;
}

struct ScopedJ2KMem {
    J2KFreeFn fn = nullptr;
    void* ptr = nullptr;
    ~ScopedJ2KMem() {
        if (fn && ptr) {
            fn(ptr);
        }
    }
};

}  // namespace

struct HdTransJ2KAdapter::Impl {
    HMODULE module = nullptr;
    J2KInitFn fnInit = nullptr;
    J2KInitExFn fnInitEx = nullptr;
    J2KTransWFn fnTransW = nullptr;
    J2KFreeFn fnFree = nullptr;
    J2KTermFn fnTerm = nullptr;
};

HdTransJ2KAdapter::~HdTransJ2KAdapter() {
    Shutdown(true);
    delete impl_;
    impl_ = nullptr;
}

bool HdTransJ2KAdapter::ProbeAvailability(const std::wstring& rootPath, std::string* detailOut) const {
    std::wstring dllPath;
    std::wstring dataDir;
    return ResolveHdTransPaths(rootPath, dllPath, dataDir, detailOut);
}

bool HdTransJ2KAdapter::Initialize(const std::wstring& rootPath, std::string* detailOut) {
    if (initialized_) {
        return true;
    }

    std::wstring dllPath;
    std::wstring dataDir;
    if (!ResolveHdTransPaths(rootPath, dllPath, dataDir, detailOut)) {
        return false;
    }

    if (!impl_) {
        impl_ = new Impl();
    }

    const auto t0 = std::chrono::steady_clock::now();
    HMODULE mod = LoadLibraryW(dllPath.c_str());
    if (!mod) {
        if (detailOut) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "LoadLibraryW failed (err=%lu)", GetLastError());
            *detailOut = msg;
        }
        return false;
    }

    impl_->module = mod;
    impl_->fnInit = reinterpret_cast<J2KInitFn>(GetProcAddress(mod, "J2K_Initialize"));
    impl_->fnInitEx = reinterpret_cast<J2KInitExFn>(GetProcAddress(mod, "J2K_InitializeEx"));
    impl_->fnTransW = reinterpret_cast<J2KTransWFn>(GetProcAddress(mod, "J2K_TranslateMMNTW"));
    impl_->fnFree = reinterpret_cast<J2KFreeFn>(GetProcAddress(mod, "J2K_FreeMem"));
    impl_->fnTerm = reinterpret_cast<J2KTermFn>(GetProcAddress(mod, "J2K_Terminate"));

    if ((!impl_->fnInitEx && !impl_->fnInit) || !impl_->fnTransW || !impl_->fnFree || !impl_->fnTerm) {
        if (detailOut) {
            *detailOut = "Missing required J2KEngine.dll exports";
        }
        Shutdown(true);
        return false;
    }

    std::string dataDirAnsi;
    if (!WidePathToAnsi(dataDir, dataDirAnsi)) {
        if (detailOut) {
            *detailOut = "Failed to convert hdtrans data path to ANSI/short path";
        }
        Shutdown(true);
        return false;
    }

    const int initOk = impl_->fnInitEx
                           ? impl_->fnInitEx(dataDirAnsi.c_str(), "EZTR")
                           : impl_->fnInit(dataDirAnsi.c_str());
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (initOk == 0) {
        if (detailOut) {
            *detailOut = "J2K_InitializeEx returned 0 for dataDir='" + WideToUtf8(dataDir) + "'";
        }
        Shutdown(true);
        return false;
    }

    initialized_ = true;
    engineVersion_ = ReadDictsVersionTag(dataDir);
    if (detailOut) {
        char buf[384];
        std::snprintf(buf, sizeof(buf), "initialized %s in %.1f ms (data='%s')",
                      engineVersion_.c_str(), ms, WideToUtf8(dataDir).c_str());
        *detailOut = buf;
    }
    return true;
}

bool HdTransJ2KAdapter::TranslateUtf8(std::string_view inputUtf8,
                                      std::string& outTranslatedUtf8,
                                      std::string* errorOut) {
    if (!initialized_ || !impl_ || !impl_->fnTransW || !impl_->fnFree) {
        if (errorOut) *errorOut = "backend_not_initialized";
        return false;
    }

    const std::wstring inputWide = Utf8ToWide(inputUtf8);
    if (inputWide.empty() && !inputUtf8.empty()) {
        if (errorOut) *errorOut = "utf8_to_wide_failed";
        return false;
    }

    wchar_t* rawOut = impl_->fnTransW(nullptr, inputWide.c_str());
    ScopedJ2KMem guard{impl_->fnFree, rawOut};
    if (!rawOut) {
        if (errorOut) *errorOut = "J2K_TranslateMMNTW_returned_null";
        return false;
    }

    outTranslatedUtf8 = NormalizeLineEndings(WideToUtf8(rawOut));
    if (outTranslatedUtf8.empty()) {
        if (errorOut) *errorOut = "empty_translation_output";
        return false;
    }
    return true;
}

void HdTransJ2KAdapter::Shutdown(bool unloadModule) {
    if (!impl_) {
        initialized_ = false;
        return;
    }
    if (initialized_ && unloadModule && impl_->fnTerm) {
        impl_->fnTerm();
    }
    HMODULE mod = impl_->module;
    impl_->module = nullptr;
    impl_->fnInit = nullptr;
    impl_->fnInitEx = nullptr;
    impl_->fnTransW = nullptr;
    impl_->fnFree = nullptr;
    impl_->fnTerm = nullptr;
    initialized_ = false;
    if (mod && unloadModule) {
        FreeLibrary(mod);
    }
}

}  // namespace aigiskr::core
