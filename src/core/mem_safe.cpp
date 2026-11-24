#include "core/mem_safe.hpp"

#include <windows.h>
#include <vector>

namespace aigiskr::core {

bool IsReadableRange(const void* ptr, size_t minBytes) noexcept {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000u || minBytes == 0) return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;

    const DWORD readableMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readableMask) == 0) return false;

    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return (addr + minBytes >= addr) && (addr + minBytes <= regionEnd);
}

bool SafeReadDword(const void* addr, uint32_t* out) noexcept {
    if (!out || !IsReadableRange(addr, sizeof(uint32_t))) return false;
    __try {
        *out = *reinterpret_cast<const uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadBytes(const void* addr, void* outBuf, size_t len) noexcept {
    if (!outBuf || len == 0 || !IsReadableRange(addr, 1)) return false;
    __try {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(addr);
        uint8_t* dst = reinterpret_cast<uint8_t*>(outBuf);
        for (size_t i = 0; i < len; ++i) {
            dst[i] = src[i];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCopyCString(const char* ptr, char* outBuf, size_t maxBytes, size_t* outLen) noexcept {
    if (!outBuf || maxBytes == 0 || !IsReadableRange(ptr, 1)) return false;
    __try {
        size_t i = 0;
        for (; i + 1 < maxBytes; ++i) {
            char c = ptr[i];
            outBuf[i] = c;
            if (c == '\0') {
                if (outLen) *outLen = i;
                return true;
            }
        }
        outBuf[i] = '\0';
        if (outLen) *outLen = i;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outBuf[0] = '\0';
        if (outLen) *outLen = 0;
        return false;
    }
}

bool SafeCopyCompleteCString(const char* ptr, char* outBuf, size_t capacity, size_t* outLen) noexcept {
    size_t length = 0;
    if (!SafeCopyCString(ptr, outBuf, capacity, &length) || length + 1 >= capacity) {
        if (outLen) *outLen = 0;
        if (outBuf && capacity) outBuf[0] = '\0';
        return false;
    }
    if (outLen) *outLen = length;
    return true;
}

bool SafeReadCompleteCString(const char* ptr, std::string& out) {
    // A reusable scratch buffer avoids a large hook stack frame and per-frame
    // allocations. Copy out before returning, so nested hooks cannot alias it.
    thread_local std::vector<char> scratch(kMaxNativeTextBytes + 2);
    size_t length = 0;
    out.clear();
    if (!SafeCopyCompleteCString(ptr, scratch.data(), scratch.size(), &length)) return false;
    out.assign(scratch.data(), length);
    return true;
}

bool SafeCopyWString(const wchar_t* ptr, wchar_t* outBuf, size_t maxChars, size_t* outChars) noexcept {
    if (!outBuf || maxChars == 0 || !IsReadableRange(ptr, sizeof(wchar_t))) return false;
    __try {
        size_t i = 0;
        for (; i + 1 < maxChars; ++i) {
            wchar_t c = ptr[i];
            outBuf[i] = c;
            if (c == L'\0') {
                if (outChars) *outChars = i;
                return true;
            }
        }
        outBuf[i] = L'\0';
        if (outChars) *outChars = i;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outBuf[0] = L'\0';
        if (outChars) *outChars = 0;
        return false;
    }
}

bool SafeCopyUtf32String(const uint32_t* ptr, uint32_t* outBuf, size_t maxChars, size_t* outChars) noexcept {
    if (!outBuf || maxChars == 0 || !IsReadableRange(ptr, sizeof(uint32_t))) return false;
    __try {
        size_t i = 0;
        for (; i + 1 < maxChars; ++i) {
            uint32_t c = ptr[i];
            outBuf[i] = c;
            if (c == 0u) {
                if (outChars) *outChars = i;
                return true;
            }
        }
        outBuf[i] = 0u;
        if (outChars) *outChars = i;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outBuf[0] = 0u;
        if (outChars) *outChars = 0;
        return false;
    }
}

}  // namespace aigiskr::core
