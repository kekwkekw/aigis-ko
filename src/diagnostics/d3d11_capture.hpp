#pragma once

#include <cstdint>
#include <string>

namespace aigiskr::diagnostics {

struct CaptureOptions {
    bool enabled = false;
    uint32_t intervalMs = 2000;
    uint32_t maxSnapshots = 30;
    std::wstring snapshotDir;
    std::wstring gameDir;
};

void StartBackbufferCapture(const CaptureOptions& opts);
void StopBackbufferCapture(bool cleanUnload);

}  // namespace aigiskr::diagnostics
