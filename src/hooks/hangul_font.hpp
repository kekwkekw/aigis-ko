#pragma once

#include <cstdint>
#include <vector>

namespace aigiskr::hooks {

struct HangulFontStats {
    uint64_t totalCalls = 0;
    uint64_t hangulCalls = 0;
    uint64_t hangulCacheHits = 0;
    uint64_t hangulMisses = 0;
    uint64_t hangulRasterized = 0;
    uint64_t hangulErrors = 0;
    uint64_t slotReuses = 0;
    uint64_t evictions = 0;
    uint64_t pageSwitches = 0;
    uint64_t rasterTotalUs = 0;
    uint64_t rasterMaxUs = 0;
    uint64_t extendedGlyphCalls = 0;
    uint64_t unsupportedGlyphs = 0;
};

struct HangulFontInstanceStat {
    uint32_t selfAddr = 0;
    uint16_t blockW = 0;
    uint16_t blockH = 0;
    int16_t rectH = 0;
    int32_t minAdvX = 0;
    int32_t maxAdvX = 0;
    uint32_t lastPage = 0xFFFFFFFFu;
    uint64_t totalCalls = 0;
    uint64_t hangulCalls = 0;
    uint64_t hangulRasterized = 0;
    uint64_t evictions = 0;
    uint64_t pageSwitches = 0;
};

bool InstallHangulFontHook(uintptr_t moduleBase, uintptr_t targetVa);
void ShutdownHangulFontHook(bool releaseHook);
void SetHangulFontEnabled(bool enabled);
void SetExtendedGlyphFallbackEnabled(bool enabled);
void ConfigureHangulFontStress(bool enabled, uint32_t cycles);
HangulFontStats GetHangulFontStats();
std::vector<HangulFontInstanceStat> GetHangulFontInstances();
void LogHangulFontReport();

} // namespace aigiskr::hooks
