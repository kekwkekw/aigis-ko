// SPDX-License-Identifier: AGPL-3.0-only
// AigisKR.HdTransHost - Out-of-process binary IPC bridge to J2KEngine.dll.
// Accepts UTF-8 Japanese text over stdin anonymous pipe frames and returns raw UTF-8 Korean text on stdout.
// Never contains Aigis game hooks, KnowledgeLayer, TranslationCore, or AutoCache logic.

#include "hdtrans_engine.hpp"
#include "host_protocol.hpp"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <thread>

namespace {

using namespace aigiskr::hdtrans_host;

std::atomic<bool> g_shutdownRequested{false};

void StartParentWatchdog(DWORD parentPid) {
    if (parentPid == 0 || parentPid == GetCurrentProcessId()) {
        return;
    }
    HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!hParent) {
        return;
    }
    std::thread([hParent]() {
        WaitForSingleObject(hParent, INFINITE);
        CloseHandle(hParent);
        if (!g_shutdownRequested.load(std::memory_order_acquire)) {
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }).detach();
}

void PrintUsageStderr() {
    std::fprintf(stderr,
                 "Usage: AigisKR.HdTransHost.exe --engine-root <path> [--parent-pid <pid>] [--version]\n");
}

}  // namespace

int main() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argvW) {
        std::fprintf(stderr, "CommandLineToArgvW failed\n");
        return 2;
    }

    std::wstring defaultEngineRoot;
    DWORD parentPid = 0;
    bool showVersion = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argvW[i];
        if (arg == L"--engine-root" && i + 1 < argc) {
            defaultEngineRoot = argvW[++i];
        } else if (arg == L"--parent-pid" && i + 1 < argc) {
            parentPid = static_cast<DWORD>(std::wcstoul(argvW[++i], nullptr, 10));
        } else if (arg == L"--version") {
            showVersion = true;
        } else if (arg == L"--help" || arg == L"-h") {
            LocalFree(argvW);
            PrintUsageStderr();
            return 0;
        } else {
            LocalFree(argvW);
            std::fprintf(stderr, "Unknown or incomplete argument at index %d\n", i);
            PrintUsageStderr();
            return 2;
        }
    }
    LocalFree(argvW);

    if (showVersion) {
        std::fprintf(stderr, "AigisKR.HdTransHost %s (protocol=%u)\n",
                     kHostVersion, static_cast<unsigned>(kProtocolVersion));
        return 0;
    }

    StartParentWatchdog(parentPid);

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hOut == INVALID_HANDLE_VALUE || !hIn || !hOut) {
        std::fprintf(stderr, "Invalid stdin/stdout handles\n");
        return 3;
    }

    HdTransEngine engine;

    while (true) {
        RequestHeader hdr;
        std::string payload;
        const PipeIoStatus st = ReadRequestFrame(hIn, hdr, payload);
        if (st == PipeIoStatus::Eof) {
            break;
        }
        if (st == PipeIoStatus::PayloadTooLarge) {
            WriteResponseFrame(hOut, Status::PayloadTooLarge, hdr.requestId, "payload_exceeds_64KiB");
            break;
        }
        if (st == PipeIoStatus::UnsupportedProtocol) {
            WriteResponseFrame(hOut, Status::UnsupportedProtocol, hdr.requestId, "unsupported_protocol_version");
            continue;
        }
        if (st != PipeIoStatus::Ok) {
            WriteResponseFrame(hOut, Status::InvalidFrame, hdr.requestId, "invalid_request_frame");
            break;
        }

        const auto op = static_cast<Opcode>(hdr.opcode);
        if (op == Opcode::Hello) {
            const std::string reqRootUtf8 = ExtractKvField(payload, "engine_root");
            std::wstring rootToUse = !reqRootUtf8.empty() ? Utf8ToWide(reqRootUtf8) : defaultEngineRoot;
            std::string initDetail;
            if (!engine.Initialize(rootToUse, &initDetail)) {
                const std::string errBody = "status=error\nerror=" + initDetail + "\n";
                WriteResponseFrame(hOut, Status::EngineInitFailed, hdr.requestId, errBody);
                continue;
            }
            const std::string okBody =
                std::string("status=ok\n") +
                "host_version=" + kHostVersion + "\n" +
                "protocol_version=" + std::to_string(kProtocolVersion) + "\n" +
                "engine_version=" + engine.EngineVersion() + "\n" +
                "dicts_version=" + engine.DictsVersion() + "\n" +
                "engine_root=" + WideToUtf8(engine.ResolvedRoot()) + "\n";
            if (!WriteResponseFrame(hOut, Status::Ok, hdr.requestId, okBody)) {
                break;
            }
        } else if (op == Opcode::Translate) {
            if (!engine.IsInitialized()) {
                if (!defaultEngineRoot.empty()) {
                    std::string initDetail;
                    if (!engine.Initialize(defaultEngineRoot, &initDetail)) {
                        WriteResponseFrame(hOut, Status::EngineNotInitialized, hdr.requestId, initDetail);
                        continue;
                    }
                } else {
                    WriteResponseFrame(hOut, Status::EngineNotInitialized, hdr.requestId, "hello_required");
                    continue;
                }
            }
            std::string outUtf8;
            std::string transErr;
            if (!engine.TranslateUtf8(payload, outUtf8, &transErr)) {
                WriteResponseFrame(hOut, Status::TranslateFailed, hdr.requestId, transErr);
                continue;
            }
            if (outUtf8.size() > kMaxPayloadBytes) {
                WriteResponseFrame(hOut, Status::PayloadTooLarge, hdr.requestId, "translated_payload_exceeds_64KiB");
                continue;
            }
            if (!WriteResponseFrame(hOut, Status::Ok, hdr.requestId, outUtf8)) {
                break;
            }
        } else if (op == Opcode::Shutdown) {
            g_shutdownRequested.store(true, std::memory_order_release);
            WriteResponseFrame(hOut, Status::Ok, hdr.requestId, "status=bye\n");
            break;
        } else {
            WriteResponseFrame(hOut, Status::UnsupportedOpcode, hdr.requestId, "unsupported_opcode");
        }
    }

    g_shutdownRequested.store(true, std::memory_order_release);
    engine.Shutdown();
    return 0;
}
