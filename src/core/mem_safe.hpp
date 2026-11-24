#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace aigiskr::core {

// Physical Host protocol payload bound, shared by complete native text readers.
inline constexpr size_t kMaxNativeTextBytes = 65536;
bool SafeReadCompleteCString(const char* ptr, std::string& out);

// Checks VirtualQuery so we never blindly dereference low ints or unmapped addresses.
bool IsReadableRange(const void* ptr, size_t minBytes) noexcept;

// Pure SEH leaf readers (safe from C2712 in C++ callers).
bool SafeReadDword(const void* addr, uint32_t* out) noexcept;
bool SafeReadBytes(const void* addr, void* outBuf, size_t len) noexcept;
bool SafeCopyCString(const char* ptr, char* outBuf, size_t maxBytes, size_t* outLen) noexcept;
// Requires a terminator within the buffer. Unlike the diagnostic preview reader,
// this must never report a truncated prefix as a complete translation input.
bool SafeCopyCompleteCString(const char* ptr, char* outBuf, size_t capacity, size_t* outLen) noexcept;
bool SafeCopyWString(const wchar_t* ptr, wchar_t* outBuf, size_t maxChars, size_t* outChars) noexcept;
bool SafeCopyUtf32String(const uint32_t* ptr, uint32_t* outBuf, size_t maxChars, size_t* outChars) noexcept;

}  // namespace aigiskr::core
