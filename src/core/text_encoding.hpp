#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aigiskr::core {

enum class DetectedEncoding {
    Empty,
    Ascii,
    Utf8,
    ShiftJis,
    Utf16Le,
    Utf32Le,
    BinaryOrInvalid,
};

const char* EncodingName(DetectedEncoding enc);

struct DecodedText {
    DetectedEncoding encoding = DetectedEncoding::Empty;
    std::string utf8;
    std::string hexPreview;
    size_t rawByteLen = 0;
};

DecodedText DecodeMultibyteBytes(const uint8_t* data, size_t len);
DecodedText DecodeUtf16String(const wchar_t* data, size_t charLen);
DecodedText DecodeUtf32String(const uint32_t* data, size_t charLen);

std::string EscapeForTsv(std::string_view utf8);
uint64_t Fnv1a64(std::string_view s);
std::string Utf8ToShiftJis(std::string_view utf8);

}  // namespace aigiskr::core
