#include "hooks/hangul_font.hpp"

#include <Windows.h>
#include <safetyhook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/logger.hpp"

namespace aigiskr::hooks {

namespace {

struct Rect16 {
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
};
static_assert(sizeof(Rect16) == 8, "Rect16 layout must match ALRect");

struct GlyphRect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
};
static_assert(sizeof(GlyphRect) == 8, "GlyphRect layout must match ALFontImp_dyn");

using RangeLookupFn = int(__thiscall*)(void* self, uint32_t cp);
using ImageSetPixelFn = void(__thiscall*)(void* img, int x, int y, uint32_t argb);
using ImageFillRectFn = void(__thiscall*)(void* img, const Rect16* r, uint32_t argb);

constexpr size_t kOffsetRangeTable = 0x40;
constexpr size_t kOffsetActivePage = 0x48;
constexpr size_t kOffsetBlockW = 0x54;
constexpr size_t kOffsetBlockH = 0x56;
constexpr size_t kOffsetGlyphRects = 0x88;
constexpr size_t kOffsetStagingImgs = 0x8C;

constexpr size_t kVtblRangeLookup = 0x28;
constexpr size_t kVtblImgSetPixel = 0x34;
constexpr size_t kVtblImgFillRect = 0x44;
constexpr size_t kMaxTrackedSlots = 4096;

struct CachePageState {
    void* fontSelf = nullptr;
    uint32_t page = 0;
    void* imgPtr = nullptr;
    std::array<uint32_t, kMaxTrackedSlots> slotCp{};
};

struct GdiFontEntry {
    HFONT hFont = nullptr;
    int ascent = 0;
    int descent = 0;
    int internalLeading = 0;
};

class GdiHangulRasterizer {
public:
    ~GdiHangulRasterizer() { Shutdown(); }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& kv : fonts_) {
            if (kv.second.hFont) DeleteObject(kv.second.hFont);
        }
        fonts_.clear();
        if (hdc_) {
            DeleteDC(hdc_);
            hdc_ = nullptr;
        }
    }

    bool Rasterize(uint32_t cp,
                   uint16_t blockW,
                   uint16_t blockH,
                   void* img,
                   int16_t dstX,
                   int16_t dstY,
                   int16_t& outAdvanceX) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!hdc_) {
            hdc_ = CreateCompatibleDC(nullptr);
            if (!hdc_) return false;
            SetMapMode(hdc_, MM_TEXT);
        }

        const GdiFontEntry* font = GetOrCreateFontLocked(blockH);
        if (!font || !font->hFont) return false;
        HGDIOBJ oldObj = SelectObject(hdc_, font->hFont);

        GLYPHMETRICS gm{};
        MAT2 mat{};
        mat.eM11.value = 1;
        mat.eM22.value = 1;

        DWORD need = GetGlyphOutlineW(hdc_, static_cast<UINT>(cp), GGO_GRAY8_BITMAP, &gm, 0, nullptr, &mat);
        std::vector<uint8_t> buf;
        bool hasBitmap = false;
        if (need != GDI_ERROR && need > 0) {
            buf.resize(need);
            DWORD got = GetGlyphOutlineW(hdc_, static_cast<UINT>(cp), GGO_GRAY8_BITMAP, &gm, need, buf.data(), &mat);
            if (got != GDI_ERROR) {
                hasBitmap = true;
            }
        }
        if (oldObj) SelectObject(hdc_, oldObj);

        if (!IsValidPtr(img, 0x10)) return false;
        void** vtbl = *reinterpret_cast<void***>(img);
        if (!IsValidPtr(vtbl, 0x48)) return false;

        auto fillRect = reinterpret_cast<ImageFillRectFn>(vtbl[kVtblImgFillRect / sizeof(void*)]);
        auto setPixel = reinterpret_cast<ImageSetPixelFn>(vtbl[kVtblImgSetPixel / sizeof(void*)]);
        if (!fillRect || !setPixel) return false;

        uint16_t* imgFlags = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(img) + 6);
        *imgFlags &= 0xFFFCu;

        Rect16 clearRect{
            dstX,
            dstY,
            static_cast<int16_t>(dstX + static_cast<int16_t>(blockW)),
            static_cast<int16_t>(dstY + static_cast<int16_t>(blockH)),
        };
        fillRect(img, &clearRect, 0x00000000u);

        if (hasBitmap && gm.gmBlackBoxX > 0 && gm.gmBlackBoxY > 0) {
            const int stride = static_cast<int>((gm.gmBlackBoxX + 3u) & ~3u);
            const int visualAscent = std::max(1, font->ascent - font->internalLeading);
            const int totalTextH = visualAscent + font->descent;
            const int topPad = std::max(0, (static_cast<int>(blockH) - totalTextH) / 2);
            const int baseline = std::clamp(topPad + font->ascent, 1, static_cast<int>(blockH) - 1);
            // Adjust baseline up when internalLeading consumes top headroom so descenders never clip.
            const int effectiveBaseline = std::clamp(topPad + visualAscent, 1, static_cast<int>(blockH) - 2);
            (void)baseline;
            const int originX = std::max(0, static_cast<int>(gm.gmptGlyphOrigin.x));
            const int originY = effectiveBaseline - static_cast<int>(gm.gmptGlyphOrigin.y);

            for (unsigned int row = 0; row < gm.gmBlackBoxY; ++row) {
                const int py = originY + static_cast<int>(row);
                if (py < 0 || py >= static_cast<int>(blockH)) continue;
                const size_t rowOff = static_cast<size_t>(row) * static_cast<size_t>(stride);
                if (rowOff + gm.gmBlackBoxX > buf.size()) break;
                for (unsigned int col = 0; col < gm.gmBlackBoxX; ++col) {
                    const uint8_t gray = buf[rowOff + col];
                    if (gray == 0) continue;
                    const int px = originX + static_cast<int>(col);
                    if (px < 0 || px >= static_cast<int>(blockW)) continue;
                    const double norm = std::clamp(static_cast<double>(gray) / 64.0, 0.0, 1.0);
                    const uint32_t alpha = std::clamp<uint32_t>(
                        static_cast<uint32_t>(std::lround(255.0 * std::pow(norm, 0.68))), 0u, 255u);
                    const uint32_t argb = (alpha << 24) | 0x00FFFFFFu;
                    *imgFlags &= 0xFFFCu;
                    setPixel(img, dstX + px, dstY + py, argb);
                }
            }
        }

        const int cellInc = gm.gmCellIncX > 0 ? static_cast<int>(gm.gmCellIncX) : static_cast<int>(blockW);
        outAdvanceX = static_cast<int16_t>(std::clamp(cellInc, 4, static_cast<int>(blockW)));
        return true;
    }

private:
    static bool IsValidPtr(const void* p, size_t /*bytes*/) {
        return p != nullptr;
    }

    const GdiFontEntry* GetOrCreateFontLocked(uint16_t blockH) {
        auto it = fonts_.find(blockH);
        if (it != fonts_.end()) return &it->second;

        LOGFONTW lf{};
        lf.lfHeight = -std::max(8, static_cast<int>(std::lround(static_cast<double>(blockH) * 0.78)));
        lf.lfWeight = FW_BOLD;
        lf.lfCharSet = HANGUL_CHARSET;
        lf.lfQuality = ANTIALIASED_QUALITY;
        wcsncpy_s(lf.lfFaceName, L"Malgun Gothic", _TRUNCATE);

        HFONT hf = CreateFontIndirectW(&lf);
        if (!hf) {
            wcsncpy_s(lf.lfFaceName, L"Gulim", _TRUNCATE);
            hf = CreateFontIndirectW(&lf);
        }
        if (!hf) return nullptr;

        TEXTMETRICW tm{};
        HGDIOBJ old = SelectObject(hdc_, hf);
        GetTextMetricsW(hdc_, &tm);
        if (old) SelectObject(hdc_, old);

        GdiFontEntry entry{hf, tm.tmAscent, tm.tmDescent, tm.tmInternalLeading};
        auto [insIt, _] = fonts_.emplace(blockH, entry);
        return &insIt->second;
    }

    std::mutex mu_;
    HDC hdc_ = nullptr;
    std::unordered_map<uint16_t, GdiFontEntry> fonts_;
};

SafetyHookInline g_hookGetOrCacheGlyph{};
std::atomic<bool> g_hangulEnabled{true};
std::mutex g_cacheMu;
std::vector<CachePageState> g_pageStates;
GdiHangulRasterizer g_rasterizer;
HangulFontStats g_stats;

bool IsHangulCodepoint(uint32_t cp) {
    return (cp >= 0xAC00u && cp <= 0xD7A3u) ||
           (cp >= 0x1100u && cp <= 0x11FFu) ||
           (cp >= 0x3130u && cp <= 0x318Fu) ||
           (cp >= 0xA960u && cp <= 0xA97Fu) ||
           (cp >= 0xD7B0u && cp <= 0xD7FFu);
}

bool IsExtendedFallbackCodepoint(uint32_t cp) noexcept {
    return cp == 0x00B7u ||                  // Middle dot '·'
           cp == 0x2015u ||                  // Horizontal bar '―'
           (cp >= 0x2160u && cp <= 0x2164u); // Roman numerals 'Ⅰ'..'Ⅴ'
}

bool RangeTableHasGlyph(void* self, uint32_t cp) {
    if (!self) return false;
    void* rangeTable = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(self) + kOffsetRangeTable);
    if (!rangeTable) return false;
    void** vtbl = *reinterpret_cast<void***>(rangeTable);
    if (!vtbl) return false;
    auto lookup = reinterpret_cast<RangeLookupFn>(vtbl[kVtblRangeLookup / sizeof(void*)]);
    if (!lookup) return false;
    return lookup(rangeTable, cp) != -1;
}

struct PageExtraState {
    void* fontSelf = nullptr;
    uint32_t page = 0;
    void* imgPtr = nullptr;
    std::array<bool, kMaxTrackedSlots> used{};
};

std::vector<PageExtraState> g_pageExtras;
std::atomic<bool> g_extendedFallbackEnabled{true};
std::unordered_set<uint32_t> g_loggedUnsupportedGlyphs;
std::vector<HangulFontInstanceStat> g_instances;
std::atomic<bool> g_fontStressEnabled{false};
std::atomic<uint32_t> g_fontStressCycles{2500};
std::atomic<bool> g_fontStressDone{false};
thread_local bool g_inStressRun = false;

PageExtraState& GetPageExtraLocked(void* self, uint32_t page, void* img) {
    for (auto& ex : g_pageExtras) {
        if (ex.fontSelf == self && ex.page == page) {
            if (img != nullptr && ex.imgPtr != nullptr && ex.imgPtr != img) {
                ex.used.fill(false);
            }
            if (img != nullptr) {
                ex.imgPtr = img;
            }
            return ex;
        }
    }
    g_pageExtras.push_back(PageExtraState{self, page, img, {}});
    return g_pageExtras.back();
}

HangulFontInstanceStat& GetOrCreateInstanceLocked(void* self, uint16_t blockW, uint16_t blockH) {
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
    for (auto& inst : g_instances) {
        if (inst.selfAddr == addr) {
            return inst;
        }
    }
    HangulFontInstanceStat inst;
    inst.selfAddr = addr;
    inst.blockW = blockW;
    inst.blockH = blockH;
    g_instances.push_back(inst);
    return g_instances.back();
}

uint64_t QpcDeltaUs(LARGE_INTEGER t0, LARGE_INTEGER t1) {
    static LARGE_INTEGER s_freq = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    if (s_freq.QuadPart <= 0 || t1.QuadPart <= t0.QuadPart) {
        return 0;
    }
    return static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart);
}

int32_t __fastcall DetourGetOrCacheGlyph(void* self, void* /*edx*/, uint32_t codepoint);

void RunFontStressPass(void* self, uint16_t blockW, uint16_t blockH, uint32_t activeCodepoint) {
    const uint32_t cycles = (g_fontStressCycles.load(std::memory_order_relaxed) >= 2500u)
        ? g_fontStressCycles.load(std::memory_order_relaxed)
        : 2500u;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(self);
    const uint32_t maxSlots = *reinterpret_cast<const uint32_t*>(base + 0x5C);
    const uintptr_t* const* vptr = reinterpret_cast<const uintptr_t* const*>(self);
    const uintptr_t* vtbl = vptr ? *vptr : nullptr;
    using ReleaseSlotFn = void(__thiscall*)(void* self, int32_t slot);
    ReleaseSlotFn releaseFn = nullptr;
    if (vtbl) {
        MEMORY_BASIC_INFORMATION mbi{};
        const uintptr_t relAddr = vtbl[15];
        if (VirtualQuery(reinterpret_cast<LPCVOID>(relAddr), &mbi, sizeof(mbi)) == sizeof(mbi) &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0) {
            releaseFn = reinterpret_cast<ReleaseSlotFn>(relAddr);
        }
    }

    if (!releaseFn || maxSlots < 64 || maxSlots > kMaxTrackedSlots) {
        core::Logger::Instance().Logf(
            "[FONT_STRESS] Aborted safely: unsupported ALFontImp_dyn layout (maxSlots=%u releaseFn=%p)",
            maxSlots, reinterpret_cast<void*>(releaseFn));
        return;
    }

    // Keep <= 25% of atlas capacity pinned during stress so LRU free-list stays well-fed and cycles clean evictions.
    const uint32_t window = (maxSlots > 256u) ? (maxSlots / 4u) : (maxSlots / 2u);
    std::vector<int32_t> heldSlots;
    heldSlots.reserve(window);

    g_inStressRun = true;
    core::Logger::Instance().Logf(
        "[FONT_STRESS] Starting live eviction stress pass: self=0x%08X cell=%ux%u maxSlots=%u window=%u cycles=%u",
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)),
        static_cast<unsigned>(blockW),
        static_cast<unsigned>(blockH),
        maxSlots,
        window,
        cycles);

    uint32_t executed = 0;
    for (uint32_t i = 0; i < cycles; ++i) {
        if (heldSlots.size() >= window) {
            const size_t ringIdx = static_cast<size_t>(i % window);
            if (heldSlots[ringIdx] >= 0) {
                releaseFn(self, heldSlots[ringIdx]);
                heldSlots[ringIdx] = -1;
            }
        }
        // Stride 5 is coprime to 11172 (U+AC00..U+D7A3), yielding 100% distinct Hangul syllables.
        uint32_t cp = 0xAC00u + ((i * 5u) % 11172u);
        if (cp == activeCodepoint) {
            cp = (cp + 1u <= 0xD7A3u) ? (cp + 1u) : 0xAC00u;
        }
        const int16_t slot = static_cast<int16_t>(DetourGetOrCacheGlyph(self, nullptr, cp) & 0xFFFF);
        if (slot < 0) {
            core::Logger::Instance().Logf("[FONT_STRESS] Early stop at i=%u (slot=-1 free list empty)", i);
            break;
        }
        ++executed;
        if (heldSlots.size() < window) {
            heldSlots.push_back(static_cast<int32_t>(slot));
        } else {
            heldSlots[static_cast<size_t>(i % window)] = static_cast<int32_t>(slot);
        }
    }

    // Release all remaining stress slots cleanly back to ALFontImp_dyn free list.
    for (int32_t s : heldSlots) {
        if (s >= 0) {
            releaseFn(self, s);
        }
    }

    HangulFontStats snap;
    {
        std::lock_guard<std::mutex> lock(g_cacheMu);
        snap = g_stats;
    }
    const double avgUs = snap.hangulRasterized
        ? static_cast<double>(snap.rasterTotalUs) / static_cast<double>(snap.hangulRasterized)
        : 0.0;
    core::Logger::Instance().Logf(
        "[FONT_STRESS] Completed: executed=%u/%u total=%llu hangul=%llu hits=%llu misses=%llu rasterized=%llu reuses=%llu evictions=%llu pageSwitches=%llu errors=%llu rasterAvgUs=%.2f rasterMaxUs=%llu",
        executed, cycles,
        snap.totalCalls, snap.hangulCalls, snap.hangulCacheHits, snap.hangulMisses,
        snap.hangulRasterized, snap.slotReuses, snap.evictions, snap.pageSwitches,
        snap.hangulErrors, avgUs, snap.rasterMaxUs);
    g_inStressRun = false;
}

CachePageState& GetPageStateLocked(void* self, uint32_t page, void* img) {
    for (auto& st : g_pageStates) {
        if (st.fontSelf == self && st.page == page) {
            // Note: ALFontImp_dyn unlocks and nulls stagingImages[page] after upload,
            // so img == nullptr does NOT clear GPU cache contents. Only reset if a
            // different non-null staging ALImage* object is later allocated.
            if (img != nullptr && st.imgPtr != nullptr && st.imgPtr != img) {
                st.slotCp.fill(0);
            }
            if (img != nullptr) {
                st.imgPtr = img;
            }
            return st;
        }
    }
    g_pageStates.push_back(CachePageState{self, page, img, {}});
    return g_pageStates.back();
}

int32_t __fastcall DetourGetOrCacheGlyph(void* self, void* /*edx*/, uint32_t codepoint) {
    const int32_t rawSlot = g_hookGetOrCacheGlyph.thiscall<int32_t>(self, codepoint);
    const int16_t slot = static_cast<int16_t>(rawSlot & 0xFFFF);

    if (!self || slot < 0 || static_cast<size_t>(slot) >= kMaxTrackedSlots) {
        return rawSlot;
    }

    const uint8_t* base = reinterpret_cast<const uint8_t*>(self);
    const uint32_t page = *reinterpret_cast<const uint32_t*>(base + kOffsetActivePage);
    if (page >= 2) return rawSlot;

    void* img = *reinterpret_cast<void* const*>(base + kOffsetStagingImgs + page * sizeof(void*));
    const uint16_t blockW = *reinterpret_cast<const uint16_t*>(base + kOffsetBlockW);
    const uint16_t blockH = *reinterpret_cast<const uint16_t*>(base + kOffsetBlockH);
    GlyphRect* rects = *reinterpret_cast<GlyphRect* const*>(base + kOffsetGlyphRects);

    const bool enabled = g_hangulEnabled.load(std::memory_order_relaxed);
    const bool missingFromRange = enabled && !RangeTableHasGlyph(self, codepoint);
    const bool isHangulCp = IsHangulCodepoint(codepoint);
    const bool isExtCp = !isHangulCp &&
                         g_extendedFallbackEnabled.load(std::memory_order_relaxed) &&
                         IsExtendedFallbackCodepoint(codepoint);
    const bool isHangul = missingFromRange && (isHangulCp || isExtCp);

    bool triggerStress = false;
    {
        std::unique_lock<std::mutex> lock(g_cacheMu);
        ++g_stats.totalCalls;
        CachePageState& st = GetPageStateLocked(self, page, img);
        PageExtraState& ex = GetPageExtraLocked(self, page, img);
        HangulFontInstanceStat& inst = GetOrCreateInstanceLocked(self, blockW, blockH);
        ++inst.totalCalls;
        if (inst.lastPage != 0xFFFFFFFFu && inst.lastPage != page) {
            ++inst.pageSwitches;
            ++g_stats.pageSwitches;
        }
        inst.lastPage = page;
        if (rects && inst.rectH == 0) {
            inst.rectH = rects[slot].h;
        }

        const size_t slotIdx = static_cast<size_t>(slot);
        const uint32_t prevHangulCp = st.slotCp[slotIdx];
        const bool wasUsed = ex.used[slotIdx];

        if (!isHangul) {
            bool logUnsupported = false;
            if (missingFromRange && codepoint > 0x20u && codepoint != 0x00A0u && codepoint != 0x3000u) {
                if (g_loggedUnsupportedGlyphs.insert(codepoint).second) {
                    ++g_stats.unsupportedGlyphs;
                    logUnsupported = true;
                }
            }
            if (prevHangulCp != 0) {
                ++g_stats.slotReuses;
                ++g_stats.evictions;
                ++inst.evictions;
            }
            st.slotCp[slotIdx] = 0;
            ex.used[slotIdx] = true;
            if (logUnsupported) {
                lock.unlock();
                core::Logger::Instance().Logf("[FONT] AUTO_GLYPH_UNSUPPORTED U+%04X", codepoint);
            }
            return rawSlot;
        }

        if (isExtCp) {
            ++g_stats.extendedGlyphCalls;
        }
        ++g_stats.hangulCalls;
        ++inst.hangulCalls;
        if (prevHangulCp == codepoint) {
            ++g_stats.hangulCacheHits;
            return rawSlot;
        }

        ++g_stats.hangulMisses;
        if (wasUsed) {
            ++g_stats.slotReuses;
        }
        if (prevHangulCp != 0) {
            ++g_stats.evictions;
            ++inst.evictions;
        }

        if (!img || !rects || blockW < 8 || blockW > 128 || blockH < 8 || blockH > 128) {
            ++g_stats.hangulErrors;
            ex.used[slotIdx] = true;
            return rawSlot;
        }

        GlyphRect& rect = rects[slot];
        int16_t advX = static_cast<int16_t>(blockW);
        LARGE_INTEGER t0{}, t1{};
        QueryPerformanceCounter(&t0);
        const bool ok = g_rasterizer.Rasterize(codepoint, blockW, blockH, img, rect.x, rect.y, advX);
        QueryPerformanceCounter(&t1);
        const uint64_t us = QpcDeltaUs(t0, t1);
        g_stats.rasterTotalUs += us;
        if (us > g_stats.rasterMaxUs) {
            g_stats.rasterMaxUs = us;
        }

        ex.used[slotIdx] = true;
        if (ok) {
            rect.w = advX;
            st.slotCp[slotIdx] = codepoint;
            ++g_stats.hangulRasterized;
            ++inst.hangulRasterized;
            if (inst.minAdvX == 0 || advX < inst.minAdvX) inst.minAdvX = advX;
            if (advX > inst.maxAdvX) inst.maxAdvX = advX;
            if (g_stats.hangulRasterized <= 5) {
                core::Logger::Instance().Logf(
                    "[FONT] rasterized Hangul U+%04X self=0x%08X page=%u slot=%d rect=(%d,%d %dx%d)",
                    codepoint,
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self)),
                    page,
                    static_cast<int>(slot),
                    static_cast<int>(rect.x),
                    static_cast<int>(rect.y),
                    static_cast<int>(rect.w),
                    static_cast<int>(rect.h));
            }
            if (!g_inStressRun &&
                g_fontStressEnabled.load(std::memory_order_relaxed) &&
                !g_fontStressDone.exchange(true, std::memory_order_acq_rel)) {
                triggerStress = true;
            }
        } else {
            st.slotCp[slotIdx] = 0;
            ++g_stats.hangulErrors;
        }
    }

    if (triggerStress) {
        RunFontStressPass(self, blockW, blockH, codepoint);
    }

    return rawSlot;
}

} // namespace

void ConfigureHangulFontStress(bool enabled, uint32_t cycles) {
    g_fontStressEnabled.store(enabled, std::memory_order_relaxed);
    g_fontStressCycles.store(cycles, std::memory_order_relaxed);
    g_fontStressDone.store(false, std::memory_order_relaxed);
}

std::vector<HangulFontInstanceStat> GetHangulFontInstances() {
    std::lock_guard<std::mutex> lock(g_cacheMu);
    return g_instances;
}

void LogHangulFontReport() {
    std::lock_guard<std::mutex> lock(g_cacheMu);
    const double avgUs = g_stats.hangulRasterized
        ? static_cast<double>(g_stats.rasterTotalUs) / static_cast<double>(g_stats.hangulRasterized)
        : 0.0;
    core::Logger::Instance().Logf(
        "[FONT_STATS_R1] total=%llu hangul=%llu hits=%llu misses=%llu rasterized=%llu reuses=%llu evictions=%llu pageSwitches=%llu errors=%llu extFallback=%llu unsupported=%llu rasterAvgUs=%.2f rasterMaxUs=%llu instances=%zu",
        g_stats.totalCalls, g_stats.hangulCalls, g_stats.hangulCacheHits, g_stats.hangulMisses,
        g_stats.hangulRasterized, g_stats.slotReuses, g_stats.evictions, g_stats.pageSwitches,
        g_stats.hangulErrors, g_stats.extendedGlyphCalls, g_stats.unsupportedGlyphs,
        avgUs, g_stats.rasterMaxUs, g_instances.size());
    for (const auto& inst : g_instances) {
        core::Logger::Instance().Logf(
            "[FONT_INSTANCE] self=0x%08X cell=%ux%u rectH=%d advX=[%d..%d] total=%llu hangul=%llu rasterized=%llu evictions=%llu pageSwitches=%llu",
            inst.selfAddr,
            static_cast<unsigned>(inst.blockW),
            static_cast<unsigned>(inst.blockH),
            static_cast<int>(inst.rectH),
            inst.minAdvX,
            inst.maxAdvX,
            inst.totalCalls,
            inst.hangulCalls,
            inst.hangulRasterized,
            inst.evictions,
            inst.pageSwitches);
    }
}

bool InstallHangulFontHook(uintptr_t /*moduleBase*/, uintptr_t targetVa) {
    if (targetVa == 0) return false;
    g_hookGetOrCacheGlyph = safetyhook::create_inline(
        reinterpret_cast<void*>(targetVa),
        reinterpret_cast<void*>(&DetourGetOrCacheGlyph));
    if (!g_hookGetOrCacheGlyph) {
        core::Logger::Instance().Logf("[FONT] FAILED create_inline ALFontImp_dyn::GetOrCacheGlyph @ 0x%08X",
                                      static_cast<uint32_t>(targetVa));
        return false;
    }
    core::Logger::Instance().Logf("[FONT] ARMED ALFontImp_dyn::GetOrCacheGlyph @ va=0x%08X",
                                  static_cast<uint32_t>(targetVa));
    return true;
}

void ShutdownHangulFontHook(bool releaseHook) {
    HangulFontStats snap = GetHangulFontStats();
    core::Logger::Instance().Logf(
        "[FONT_STATS] total=%llu hangul=%llu hits=%llu rasterized=%llu errors=%llu",
        snap.totalCalls, snap.hangulCalls, snap.hangulCacheHits, snap.hangulRasterized, snap.hangulErrors);

    if (releaseHook) {
        g_hookGetOrCacheGlyph = {};
        g_rasterizer.Shutdown();
        std::lock_guard<std::mutex> lock(g_cacheMu);
        g_pageStates.clear();
        g_loggedUnsupportedGlyphs.clear();
        g_stats = {};
    }
}

void SetHangulFontEnabled(bool enabled) {
    g_hangulEnabled.store(enabled, std::memory_order_relaxed);
}

void SetExtendedGlyphFallbackEnabled(bool enabled) {
    g_extendedFallbackEnabled.store(enabled, std::memory_order_relaxed);
}

HangulFontStats GetHangulFontStats() {
    std::lock_guard<std::mutex> lock(g_cacheMu);
    return g_stats;
}

} // namespace aigiskr::hooks
