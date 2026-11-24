#include "hooks/live_text_refresh.hpp"
#include "core/auto_translate.hpp"
#include "core/logger.hpp"
#include "core/mem_safe.hpp"
#include "core/pending_text_refresh.hpp"
#include "core/pending_text_sequence.hpp"
#include "core/tag_parser.hpp"
#include "resolver/resolver.hpp"
#include <windows.h>
#include <safetyhook.hpp>
#include <atomic>
#include <cstring>

namespace aigiskr::hooks {
namespace {
core::PendingTextRefresh pending;
core::PendingTextSequence sequences;
std::atomic<bool> enabled{false};
SafetyHookMid renderHook, clearHook, destroyHook;
core::AutoTranslateManager* translator = nullptr;
uintptr_t ownerVtable = 0, wrapperVtable = 0;
using TextSetter = void(__thiscall*)(void*, const char*);
TextSetter setter = nullptr;
TextSetter writeText = nullptr;
using ClearDraw = void(__thiscall*)(void*);
ClearDraw clearDraw = nullptr;
bool storyVerified = false;
RefreshStreamOrigin streamOrigin;
uintptr_t exeBase = 0;
uint32_t storyCaller = 0;
using LineAdvance = void(__thiscall*)(void*, float);
LineAdvance advanceLine = nullptr;
struct Replay { uintptr_t draw; const std::string* source; const std::string* target; };
thread_local const Replay* replay = nullptr;
std::atomic<uint64_t> tracked{0}, refreshed{0}, stale{0}, unsupported{0}, directRefreshed{0};
std::atomic<uint64_t> cancelled[static_cast<size_t>(LiveTextReset::Count)]{}, appendSkipped{0}, capacitySkipped{0};
std::atomic<uint64_t> storyParts{0}, storyRefreshed{0}, storyRejected{0};

bool Read(uintptr_t base, size_t offset, uint32_t& out) {
    return core::SafeReadDword(reinterpret_cast<const void*>(base + offset), &out);
}

// The engine is drawing this instance NOW. No saved owner pointer is dereferenced.
// +1c is ALTextImp, +70 its ALTextDraw, +8 its public ALText wrapper;
// ALTextImp+90 is an ALString handle whose +4 points to the retained UTF-8 source.
bool ReadCurrentOwner(uintptr_t draw, uint32_t& wrapper, std::string& source) {
    uint32_t owner = 0, vt = 0, back = 0, handle = 0, chars = 0;
    if (!Read(draw, 0x1c, owner) || !Read(owner, 0, vt) || vt != ownerVtable ||
        !Read(owner, 0x70, back) || back != draw || !Read(owner, 8, wrapper) ||
        !Read(wrapper, 0, vt) || vt != wrapperVtable ||
        !Read(wrapper, 4, back) || back != owner ||
        !Read(owner, 0x90, handle) || !Read(handle, 4, chars)) return false;
    return core::SafeReadCompleteCString(reinterpret_cast<const char*>(chars), source);
}

void OnClear(SafetyHookContext& ctx) { ForgetLiveText(ctx.ecx, LiveTextReset::Clear); }
void OnDestroy(SafetyHookContext& ctx) { ForgetLiveText(ctx.ecx, LiveTextReset::Destroy); }

void RestoreColors(uintptr_t draw, const std::array<uint32_t, 10>& colors) {
    std::memcpy(reinterpret_cast<void*>(draw + 0xc0), colors.data(), 8);
    std::memcpy(reinterpret_cast<void*>(draw + 0xf8), colors.data() + 2, 32);
}

void RefreshStory(uintptr_t draw, uint64_t revision) {
    core::PendingTextSequence::Binding binding;
    if (!sequences.Candidate(draw, GetCurrentThreadId(), revision, binding)) return;
    if (!core::PendingTextSequence::Resolve(binding, [](const std::string& key, std::string& out) {
        return translator->LookupCompleted(key, out);
    })) return;
    uint32_t wrapper = 0;
    std::string current;
    if (!ReadCurrentOwner(draw, wrapper, current) || !current.empty()) {
        sequences.Block(draw);
        ++storyRejected;
        return;
    }
    if (!sequences.Commit(draw, binding, true)) { ++storyRejected; return; }

    // Rebuild glyphs only. Re-entering the script writer would advance dialogue.
    // Its native contract is slot28(line), slot32(1.0), repeated in source order;
    // ALTextImp+90 deliberately stays empty. Keep pending and cached lines alike.
    Replay active{draw, nullptr, nullptr};
    replay = &active; // suppress our own clear/write invalidation
    clearDraw(reinterpret_cast<void*>(draw));
    RestoreColors(draw, binding.initialColors);
    for (const auto& part : binding.parts) {
        active.source = &part.source;
        active.target = &part.rendered;
        writeText(reinterpret_cast<void*>(draw), part.source.c_str());
        advanceLine(reinterpret_cast<void*>(draw), 1.0f);
    }
    replay = nullptr;
    if (++storyRefreshed <= 32)
        core::Logger::Instance().Logf("[LIVE_STORY] applied draw=%08x lines=%zu revision=%llu thread=%lu",
            static_cast<uint32_t>(draw), binding.parts.size(), revision, GetCurrentThreadId());
}

void OnRender(SafetyHookContext& ctx) {
    if (!enabled.load(std::memory_order_acquire) || replay) return;
    core::PendingTextRefresh::Binding binding;
    const auto revision = translator->CompletionRevision();
    RefreshStory(ctx.ecx, revision);
    if (!pending.Candidate(ctx.ecx, GetCurrentThreadId(), revision, binding)) return;
    std::string target;
    if (!translator->LookupCompleted(binding.key, target)) return;
    uint32_t wrapper = 0;
    std::string current;
    if (!ReadCurrentOwner(ctx.ecx, wrapper, current)) {
        pending.Consume(ctx.ecx, binding.serial, {});
        ++unsupported;
        return;
    }
    if (!pending.Consume(ctx.ecx, binding.serial, current)) {
        if (++stale <= 16) core::Logger::Instance().Logf("[LIVE_REFRESH] stale draw=%08x source_bytes=%zu owner_bytes=%zu", ctx.ecx, binding.source.size(), current.size());
        return;
    }
    // Same-thread, live owner, unchanged source. Use the game's own text setter:
    // dirty forces its equality fast-path to rebuild, Clear releases old glyphs,
    // and the usual slot28 hook substitutes target. The engine retains ORIGINAL
    // logical text, so game comparisons and future setters keep their semantics.
    const Replay active{ctx.ecx, &binding.source, &target};
    replay = &active;
    if (!current.empty()) {
        *reinterpret_cast<uint8_t*>(ctx.ecx + 0xb5) = 1;
        setter(reinterpret_cast<void*>(wrapper), binding.source.c_str());
    } else {
        // Single-write owners may clear ALTextImp+90 after emitting glyphs.
        // The write generation still identifies the visible content. Rebuild only glyphs; keep logical text and dialogue state intact.
        // Clear does not reset color. The original tail may have changed it
        // (e.g. black body -> red reward -> white reset); restore the START
        // colors, including packed gradient endpoints and their float forms.
        clearDraw(reinterpret_cast<void*>(ctx.ecx));
        RestoreColors(ctx.ecx, binding.initialColors);
        writeText(reinterpret_cast<void*>(ctx.ecx), binding.source.c_str());
        ++directRefreshed;
    }
    replay = nullptr;
    {
        if (++refreshed <= 16)
            core::Logger::Instance().Logf("[LIVE_REFRESH] applied draw=%08x source_bytes=%zu target_bytes=%zu revision=%llu thread=%lu",
                ctx.ecx, binding.source.size(), target.size(), revision, GetCurrentThreadId());
    }
}
}

void ForgetLiveText(uintptr_t draw, LiveTextReset reason, uint32_t caller) {
    if (!enabled.load(std::memory_order_acquire) || replay) return;
    const auto index = static_cast<size_t>(reason);
    const bool single = pending.Forget(draw);
    const bool sequence = sequences.Forget(draw);
    if ((single || sequence) && index < static_cast<size_t>(LiveTextReset::Count) && ++cancelled[index] <= 24)
        core::Logger::Instance().Logf("[LIVE_REFRESH] cancelled draw=%08x reason=%u caller=%08x", static_cast<uint32_t>(draw), static_cast<uint32_t>(reason), caller);
}

bool BeginLiveTextWrite(uintptr_t draw, uint32_t caller) {
    if (!enabled.load(std::memory_order_acquire) || replay) return false;
    if (storyVerified && caller == storyCaller) {
        pending.Forget(draw);
        return true;
    }
    ForgetLiveText(draw, LiveTextReset::Write, caller);
    return false;
}

void CaptureStoryText(uintptr_t draw, std::string_view source, std::string_view rendered,
                      bool awaiting, bool readable) {
    if (!enabled.load(std::memory_order_acquire) || replay || !storyVerified) return;
    uint32_t glyphs = 0;
    std::array<uint32_t, 10> colors{};
    if (!readable || !Read(draw, 0x6c, glyphs) ||
        !core::SafeReadBytes(reinterpret_cast<void*>(draw + 0xc0), colors.data(), 8) ||
        !core::SafeReadBytes(reinterpret_cast<void*>(draw + 0xf8), colors.data() + 2, 32)) {
        sequences.Block(draw);
        ++storyRejected;
        return;
    }
    if (sequences.Append(draw, glyphs == 0, GetCurrentThreadId(),
        {std::string(source), core::NormalizeLineEndings(source), std::string(rendered), awaiting}, colors))
        ++storyParts;
    else ++storyRejected;
}

void TrackPendingLiveText(uintptr_t draw, std::string_view source, std::string_view key, uint32_t caller) {
    if (!enabled.load(std::memory_order_acquire) || replay) return;
    uint32_t glyphCount = 0;
    if (!Read(draw, 0x6c, glyphCount) || glyphCount != 0) {
        if (++appendSkipped <= 24) core::Logger::Instance().Logf("[LIVE_REFRESH] append_skip draw=%08x glyphs=%u caller=%08x source_bytes=%zu", static_cast<uint32_t>(draw), glyphCount, caller, source.size());
        return;
    } // no append/composite replay
    // Zero also catches completion racing with registration. Revision starts at
    // zero and increments only after an accepted result is in the memory cache.
    std::array<uint32_t, 10> colors{};
    if (!core::SafeReadBytes(reinterpret_cast<void*>(draw + 0xc0), colors.data(), 8) ||
        !core::SafeReadBytes(reinterpret_cast<void*>(draw + 0xf8), colors.data() + 2, 32)) {
        ++unsupported;
        return;
    }
    if (pending.Track(draw, source, key, GetCurrentThreadId(), colors)) ++tracked;
    else ++capacitySkipped;
}

bool LiveReplayReplacement(uintptr_t draw, std::string_view source, std::string& out) {
    if (!replay || replay->draw != draw || !replay->source || *replay->source != source) return false;
    out = *replay->target;
    return true;
}

bool IsCurrentRefreshPreservingStream(uintptr_t draw, uint32_t caller, uintptr_t frame) {
    uint8_t measurement = 0;
    core::SafeReadBytes(reinterpret_cast<const void*>(draw + 0xbc), &measurement, 1);
    uint32_t parent = 0;
    if (caller == streamOrigin.utf32Return) {
        uint32_t address = 0;
        if (Read(frame, 4, address) && address >= exeBase) parent = address - static_cast<uint32_t>(exeBase);
    }
    return streamOrigin.Preserves(caller, parent, measurement != 0);
}

// Validate the UTF-8 method's actual call graph. Its entry has already been
// hooked, so inspect the unmodified body and RTTI slot30 wrapper instead.
bool ResolveStreamOrigin(const resolver::ModuleLayout& layout, uintptr_t vtable,
                         uintptr_t write, uintptr_t stream, RefreshStreamOrigin& out) {
    uint32_t wrapper = 0;
    if (!Read(vtable, 30 * 4, wrapper)) return false;
    uint8_t body[512]{}, utf32[23]{};
    if (!core::SafeReadBytes(reinterpret_cast<const void*>(write + 5), body, sizeof(body)) ||
        !core::SafeReadBytes(reinterpret_cast<const void*>(wrapper), utf32, sizeof(utf32))) return false;
    const uint8_t prefix[] = {0x55,0x8b,0xec,0xff,0x75,0x0c,0x68};
    const uint8_t args[] = {0xff,0x75,0x08,0xe8};
    const uint8_t tail[] = {0x5d,0xc2,0x08,0x00};
    int32_t rel = 0;
    std::memcpy(&rel, utf32 + 15, 4);
    if (std::memcmp(utf32, prefix, sizeof(prefix)) || std::memcmp(utf32+11, args, sizeof(args)) ||
        std::memcmp(utf32+19, tail, sizeof(tail)) || wrapper+19+rel != stream) return false;
    out.utf32Return = wrapper + 19 - static_cast<uint32_t>(layout.base);
    const uint8_t parent[] = {0x6a,0xff,0xff,0x75,0xf4,0x8b,0x01,0xff,0x50,0x78};
    const uint8_t direct[] = {0xff,0x75,0x08,0x8b,0xcb,0xe8};
    size_t parents = 0, directs = 0;
    for (size_t i=0; i+21 <= sizeof(body); ++i) {
        const auto address = write + 5 + i;
        if (!std::memcmp(body+i, parent, sizeof(parent))) {
            ++parents; out.utf32ParentReturn = address + sizeof(parent) - layout.base;
        }
        if (body[i]==0x6a && body[i+1]==0xff && body[i+2]==0x68 &&
            !std::memcmp(body+i+7, direct, sizeof(direct)) && body[i+17]==0x5b &&
            body[i+18]==0x8b && body[i+19]==0xe5 && body[i+20]==0x5d) {
            std::memcpy(&rel, body+i+13, 4);
            if (address+17+rel != stream) continue;
            ++directs; out.utf8Return = address + 17 - layout.base;
        }
    }
    return parents == 1 && directs == 1;
}

bool InstallLiveTextRefresh(const resolver::ModuleLayout& layout, core::AutoTranslateManager& manager,
                            uint32_t writeRva, uint32_t streamRva) {
    resolver::TargetSpec specs[] = {
        {"ALTextDraw_render", "55 8B EC 83 EC 54 57 FF 75 08 8B F9 89 7D E8 E8 ? ? ? ? 8B 4F 6C", ".?AVALTextDraw@@", 14, 0x6e4bc0},
        {"ALTextDraw_clear", "53 56 8B F1 33 DB 39 5E 6C 76 4F 57 BF FF FF 00 00", ".?AVALTextDraw@@", 31, 0x6e4500},
        {"ALTextDraw_destroy", "55 8B EC 6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 53 56 57 A1 ? ? ? ? 33 C5 50 8D 45 F4 64 A3 00 00 00 00 8B F1 33 DB C7 45 FC 00 00 00 00 C7 06 ? ? ? ? 39 5E 54", ".?AVALTextDraw@@", 1, 0x6e2da0},
        {"ALText_setText", "55 8B EC 6A FF 68 ? ? ? ? 64 A1 00 00 00 00 50 53 56 57 A1 ? ? ? ? 33 C5 50 8D 45 F4 64 A3 00 00 00 00 8B 71 04 8B 7D 08 8B CF 8B 86 90 00 00 00", nullptr, -1, 0x74e40}
    };
    uintptr_t addresses[4]{};
    for (size_t i=0; i<4; ++i) {
        const auto r = resolver::ResolveTarget(layout, specs[i]);
        core::Logger::Instance().Logf("[LIVE_REFRESH] %s %s", specs[i].name, r.detail.c_str());
        if (!r.ok) return false;
        addresses[i] = r.va;
    }
    ownerVtable = reinterpret_cast<uintptr_t>(resolver::FindPrimaryVtableByRtti(layout, ".?AVALTextImp@@"));
    wrapperVtable = reinterpret_cast<uintptr_t>(resolver::FindPrimaryVtableByRtti(layout, ".?AVALText@@"));
    const auto drawVtable = reinterpret_cast<uintptr_t>(resolver::FindPrimaryVtableByRtti(layout, ".?AVALTextDraw@@"));
    uint32_t writeAddress = 0;
    if (!ownerVtable || !wrapperVtable || !drawVtable || !Read(drawVtable, 28 * 4, writeAddress) ||
        writeAddress != layout.base + writeRva ||
        !ResolveStreamOrigin(layout, drawVtable, writeAddress, layout.base + streamRva, streamOrigin)) return false;
    core::Logger::Instance().Logf("[LIVE_REFRESH] stream origins utf8=%08x utf32=%08x parent=%08x measurement=flag",
        streamOrigin.utf8Return, streamOrigin.utf32Return, streamOrigin.utf32ParentReturn);
    const auto story = resolver::ResolveTarget(layout, {
        "Story_lineWriter", "8B 5D D4 8B 00 8B 73 04 FF 70 04 8B 4E 70 8B 11 FF 52 70 68 ? ? ? ?",
        nullptr, -1, 0x262cc9u});
    const auto storyNewline = resolver::ResolveTarget(layout, {
        "Story_lineAdvance", "8B 43 04 8B 48 70 51 C7 04 24 00 00 80 3F 8B 01 FF 90 80 00 00 00",
        nullptr, -1, 0x262d0eu});
    const auto line = resolver::ResolveTarget(layout, {
        "ALTextDraw_lineAdvance", "55 8B EC 56 57 8B F9 0F B7 87 D6 00 00 00 8B C8 66 85 C0 75 10",
        ".?AVALTextDraw@@", 32, 0x6e39e0u});
    storyVerified = story.ok && storyNewline.ok && line.ok;
    advanceLine = storyVerified ? reinterpret_cast<LineAdvance>(line.va) : nullptr;
    core::Logger::Instance().Logf("[LIVE_STORY] enabled=%d writer={%s} newline={%s} method={%s}",
        storyVerified, story.detail.c_str(), storyNewline.detail.c_str(), line.detail.c_str());
    auto render = SafetyHookMid::create(reinterpret_cast<void*>(addresses[0]), &OnRender);
    auto clear = SafetyHookMid::create(reinterpret_cast<void*>(addresses[1]), &OnClear);
    auto destroy = SafetyHookMid::create(reinterpret_cast<void*>(addresses[2]), &OnDestroy);
    if (!render || !clear || !destroy) return false;
    setter = reinterpret_cast<TextSetter>(addresses[3]);
    exeBase = layout.base;
    storyCaller = story.rva + 19;
    clearDraw = reinterpret_cast<ClearDraw>(addresses[1]);
    writeText = reinterpret_cast<TextSetter>(writeAddress);
    translator = &manager;
    renderHook = std::move(*render); clearHook = std::move(*clear); destroyHook = std::move(*destroy);
    enabled.store(true, std::memory_order_release);
    core::Logger::Instance().LogLine("[LIVE_REFRESH] enabled: completion -> live render; generation-bound single writes, no screen/build allowlist");
    return true;
}

void ShutdownLiveTextRefresh(bool releaseHooks) {
    enabled.store(false, std::memory_order_release);
    if (releaseHooks) { renderHook = {}; clearHook = {}; destroyHook = {}; }
    core::Logger::Instance().Logf("[LIVE_REFRESH_STATS] tracked=%llu refreshed=%llu stale=%llu unsupported=%llu pending=%zu",
        tracked.load(), refreshed.load(), stale.load(), unsupported.load(), pending.Size());
    pending.Clear();
    core::Logger::Instance().Logf("[LIVE_STORY_STATS] parts=%llu refreshed=%llu rejected=%llu bindings=%zu",
        storyParts.load(), storyRefreshed.load(), storyRejected.load(), sequences.Size());
    sequences.Clear();
    core::Logger::Instance().Logf("[LIVE_REFRESH_DIAG] write=%llu clear=%llu destroy=%llu stream=%llu append_skipped=%llu capacity_skipped=%llu direct_refreshed=%llu", cancelled[0].load(), cancelled[1].load(), cancelled[2].load(), cancelled[3].load(), appendSkipped.load(), capacitySkipped.load(), directRefreshed.load());
}
}
