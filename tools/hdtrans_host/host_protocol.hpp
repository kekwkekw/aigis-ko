// SPDX-License-Identifier: AGPL-3.0-only
// AigisKR.HdTransHost - Wire Protocol v1 definitions.
// Self-contained header with no AigisKR core, KnowledgeLayer, or TranslationCore dependencies.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace aigiskr::hdtrans_host {

inline constexpr uint32_t kProtocolMagic = 0x48445431u;  // "1TDH" little-endian
inline constexpr uint16_t kProtocolVersion = 1u;
inline constexpr uint32_t kMaxPayloadBytes = 65536u;     // 64 KiB cap
inline constexpr size_t kFrameHeaderBytes = 16u;

#ifndef AIGISKR_VERSION_STR
#define AIGISKR_VERSION_STR "0.1.0-rc.2"
#endif
inline constexpr const char* kHostVersion = AIGISKR_VERSION_STR;

enum class Opcode : uint16_t {
    Hello = 1,
    Translate = 2,
    Shutdown = 3,
};

enum class Status : uint16_t {
    Ok = 0,
    InvalidFrame = 1,
    UnsupportedProtocol = 2,
    UnsupportedOpcode = 3,
    PayloadTooLarge = 4,
    EngineNotInitialized = 5,
    EngineInitFailed = 6,
    TranslateFailed = 7,
};

struct RequestHeader {
    uint32_t magic = kProtocolMagic;
    uint16_t protocolVersion = kProtocolVersion;
    uint16_t opcode = 0;
    uint32_t requestId = 0;
    uint32_t payloadSize = 0;
};

struct ResponseHeader {
    uint32_t magic = kProtocolMagic;
    uint16_t protocolVersion = kProtocolVersion;
    uint16_t status = 0;
    uint32_t requestId = 0;
    uint32_t payloadSize = 0;
};

inline void WriteLe32(uint8_t* dst, uint32_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline void WriteLe16(uint8_t* dst, uint16_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline uint32_t ReadLe32(const uint8_t* src) noexcept {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

inline uint16_t ReadLe16(const uint8_t* src) noexcept {
    return static_cast<uint16_t>(src[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(src[1]) << 8);
}

inline void EncodeRequestHeader(const RequestHeader& h, uint8_t out[kFrameHeaderBytes]) noexcept {
    WriteLe32(out + 0, h.magic);
    WriteLe16(out + 4, h.protocolVersion);
    WriteLe16(out + 6, h.opcode);
    WriteLe32(out + 8, h.requestId);
    WriteLe32(out + 12, h.payloadSize);
}

inline RequestHeader DecodeRequestHeader(const uint8_t in[kFrameHeaderBytes]) noexcept {
    RequestHeader h;
    h.magic = ReadLe32(in + 0);
    h.protocolVersion = ReadLe16(in + 4);
    h.opcode = ReadLe16(in + 6);
    h.requestId = ReadLe32(in + 8);
    h.payloadSize = ReadLe32(in + 12);
    return h;
}

inline void EncodeResponseHeader(const ResponseHeader& h, uint8_t out[kFrameHeaderBytes]) noexcept {
    WriteLe32(out + 0, h.magic);
    WriteLe16(out + 4, h.protocolVersion);
    WriteLe16(out + 6, h.status);
    WriteLe32(out + 8, h.requestId);
    WriteLe32(out + 12, h.payloadSize);
}

inline ResponseHeader DecodeResponseHeader(const uint8_t in[kFrameHeaderBytes]) noexcept {
    ResponseHeader h;
    h.magic = ReadLe32(in + 0);
    h.protocolVersion = ReadLe16(in + 4);
    h.status = ReadLe16(in + 6);
    h.requestId = ReadLe32(in + 8);
    h.payloadSize = ReadLe32(in + 12);
    return h;
}

enum class PipeIoStatus {
    Ok,
    Eof,
    Error,
    InvalidMagic,
    UnsupportedProtocol,
    PayloadTooLarge,
};

inline PipeIoStatus ReadExactBytes(HANDLE h, uint8_t* dst, size_t len, bool* outAtZeroEof = nullptr) noexcept {
    if (outAtZeroEof) {
        *outAtZeroEof = false;
    }
    size_t done = 0;
    while (done < len) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(len - done);
        if (!ReadFile(h, dst + done, want, &chunk, nullptr)) {
            const DWORD err = GetLastError();
            if ((err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) && done == 0) {
                if (outAtZeroEof) *outAtZeroEof = true;
                return PipeIoStatus::Eof;
            }
            return PipeIoStatus::Error;
        }
        if (chunk == 0) {
            if (done == 0) {
                if (outAtZeroEof) *outAtZeroEof = true;
                return PipeIoStatus::Eof;
            }
            return PipeIoStatus::Error;
        }
        done += chunk;
    }
    return PipeIoStatus::Ok;
}

inline bool WriteExactBytes(HANDLE h, const uint8_t* src, size_t len) noexcept {
    size_t done = 0;
    while (done < len) {
        DWORD written = 0;
        const DWORD want = static_cast<DWORD>(len - done);
        if (!WriteFile(h, src + done, want, &written, nullptr) || written == 0) {
            return false;
        }
        done += written;
    }
    return true;
}

inline bool WriteRequestFrame(HANDLE hOut,
                              Opcode op,
                              uint32_t requestId,
                              std::string_view payload) noexcept {
    if (payload.size() > kMaxPayloadBytes) {
        return false;
    }
    RequestHeader hdr;
    hdr.magic = kProtocolMagic;
    hdr.protocolVersion = kProtocolVersion;
    hdr.opcode = static_cast<uint16_t>(op);
    hdr.requestId = requestId;
    hdr.payloadSize = static_cast<uint32_t>(payload.size());
    uint8_t rawHdr[kFrameHeaderBytes];
    EncodeRequestHeader(hdr, rawHdr);
    if (!WriteExactBytes(hOut, rawHdr, kFrameHeaderBytes)) {
        return false;
    }
    if (!payload.empty() &&
        !WriteExactBytes(hOut, reinterpret_cast<const uint8_t*>(payload.data()), payload.size())) {
        return false;
    }
    return true;
}

inline bool WriteResponseFrame(HANDLE hOut,
                               Status status,
                               uint32_t requestId,
                               std::string_view payload) noexcept {
    if (payload.size() > kMaxPayloadBytes) {
        return false;
    }
    ResponseHeader hdr;
    hdr.magic = kProtocolMagic;
    hdr.protocolVersion = kProtocolVersion;
    hdr.status = static_cast<uint16_t>(status);
    hdr.requestId = requestId;
    hdr.payloadSize = static_cast<uint32_t>(payload.size());
    uint8_t rawHdr[kFrameHeaderBytes];
    EncodeResponseHeader(hdr, rawHdr);
    if (!WriteExactBytes(hOut, rawHdr, kFrameHeaderBytes)) {
        return false;
    }
    if (!payload.empty() &&
        !WriteExactBytes(hOut, reinterpret_cast<const uint8_t*>(payload.data()), payload.size())) {
        return false;
    }
    return true;
}

inline PipeIoStatus ReadRequestFrame(HANDLE hIn, RequestHeader& outHdr, std::string& outPayload) {
    outPayload.clear();
    uint8_t rawHdr[kFrameHeaderBytes] = {};
    bool cleanEof = false;
    const PipeIoStatus hdrSt = ReadExactBytes(hIn, rawHdr, kFrameHeaderBytes, &cleanEof);
    if (hdrSt != PipeIoStatus::Ok) {
        return hdrSt;
    }
    outHdr = DecodeRequestHeader(rawHdr);
    if (outHdr.magic != kProtocolMagic) {
        return PipeIoStatus::InvalidMagic;
    }
    if (outHdr.payloadSize > kMaxPayloadBytes) {
        return PipeIoStatus::PayloadTooLarge;
    }
    if (outHdr.payloadSize > 0) {
        outPayload.resize(outHdr.payloadSize);
        const PipeIoStatus bodySt =
            ReadExactBytes(hIn, reinterpret_cast<uint8_t*>(outPayload.data()), outHdr.payloadSize);
        if (bodySt != PipeIoStatus::Ok) {
            return PipeIoStatus::Error;
        }
    }
    if (outHdr.protocolVersion != kProtocolVersion) {
        return PipeIoStatus::UnsupportedProtocol;
    }
    return PipeIoStatus::Ok;
}

inline PipeIoStatus ReadResponseFrame(HANDLE hIn, ResponseHeader& outHdr, std::string& outPayload) {
    outPayload.clear();
    uint8_t rawHdr[kFrameHeaderBytes] = {};
    const PipeIoStatus hdrSt = ReadExactBytes(hIn, rawHdr, kFrameHeaderBytes);
    if (hdrSt != PipeIoStatus::Ok) {
        return hdrSt;
    }
    outHdr = DecodeResponseHeader(rawHdr);
    if (outHdr.magic != kProtocolMagic) {
        return PipeIoStatus::InvalidMagic;
    }
    if (outHdr.protocolVersion != kProtocolVersion) {
        return PipeIoStatus::UnsupportedProtocol;
    }
    if (outHdr.payloadSize > kMaxPayloadBytes) {
        return PipeIoStatus::PayloadTooLarge;
    }
    if (outHdr.payloadSize > 0) {
        outPayload.resize(outHdr.payloadSize);
        const PipeIoStatus bodySt =
            ReadExactBytes(hIn, reinterpret_cast<uint8_t*>(outPayload.data()), outHdr.payloadSize);
        if (bodySt != PipeIoStatus::Ok) {
            return PipeIoStatus::Error;
        }
    }
    return PipeIoStatus::Ok;
}

inline std::string ExtractKvField(std::string_view payload, std::string_view key) {
    size_t pos = 0;
    while (pos < payload.size()) {
        size_t nl = payload.find('\n', pos);
        if (nl == std::string_view::npos) nl = payload.size();
        std::string_view line = payload.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const size_t eq = line.find('=');
        if (eq != std::string_view::npos && line.substr(0, eq) == key) {
            return std::string(line.substr(eq + 1));
        }
        pos = nl + 1;
    }
    return {};
}

}  // namespace aigiskr::hdtrans_host
