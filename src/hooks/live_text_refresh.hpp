#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace aigiskr::core { class AutoTranslateManager; }
namespace aigiskr::resolver { struct ModuleLayout; }
namespace aigiskr::hooks {
// Derived from the current RTTI-anchored UTF-8/UTF32 methods, never fixed RVAs.
// The engine's measurement flag covers all read-only width/line probes.
struct RefreshStreamOrigin {
    uint32_t utf8Return = 0, utf32Return = 0, utf32ParentReturn = 0;
    constexpr bool Preserves(uint32_t caller, uint32_t parent, bool measurement) const {
        return measurement || (utf8Return && caller == utf8Return) ||
            (utf32Return && caller == utf32Return && parent == utf32ParentReturn);
    }
};
bool IsCurrentRefreshPreservingStream(uintptr_t draw, uint32_t caller, uintptr_t frame);
bool InstallLiveTextRefresh(const resolver::ModuleLayout&, core::AutoTranslateManager&,
                            uint32_t writeRva, uint32_t streamRva);
void ShutdownLiveTextRefresh(bool releaseHooks);
enum class LiveTextReset : uint32_t { Write, Clear, Destroy, Stream, Count };
// Called before EVERY top-level text write, even if the new source is not translatable.
void ForgetLiveText(uintptr_t draw, LiveTextReset reason = LiveTextReset::Write, uint32_t caller = 0);
// Returns true only for the verified recollection writer, whose slot28 calls
// append one script line each. Other writes invalidate both binding types.
bool BeginLiveTextWrite(uintptr_t draw, uint32_t caller);
void CaptureStoryText(uintptr_t draw, std::string_view source, std::string_view rendered,
                      bool awaiting, bool readable);
void TrackPendingLiveText(uintptr_t draw, std::string_view source, std::string_view key, uint32_t caller = 0);
bool LiveReplayReplacement(uintptr_t draw, std::string_view source, std::string& out);
}
