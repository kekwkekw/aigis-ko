#pragma once

#include "core/config.hpp"

namespace aigiskr::hooks {

bool InstallProbeHooks(const core::ProbeConfig& cfg);
void ShutdownProbeHooks(bool releaseHooks);

}  // namespace aigiskr::hooks
