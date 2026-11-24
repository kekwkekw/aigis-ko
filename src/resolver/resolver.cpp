#include "resolver/resolver.hpp"
#include "core/logger.hpp"
#include "core/mem_safe.hpp"

#include <Hooking.Patterns.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace aigiskr::resolver {

bool InspectModuleLayout(void* hmod, ModuleLayout& out) {
    if (!hmod) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(hmod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    out = {};
    out.base = base;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = {};
        std::memcpy(name, sec[i].Name, 8);
        const uintptr_t start = base + sec[i].VirtualAddress;
        const uintptr_t span = std::max<uintptr_t>(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
        const PeRange r{start, start + span, sec[i].VirtualAddress};
        if (std::strcmp(name, ".text") == 0) out.text = r;
        else if (std::strcmp(name, ".rdata") == 0) out.rdata = r;
        else if (std::strcmp(name, ".data") == 0) out.data = r;
    }
    return out.text.begin != 0 && out.rdata.begin != 0 && out.data.begin != 0;
}

uintptr_t* FindPrimaryVtableByRtti(const ModuleLayout& layout, const char* mangledName) {
    if (!mangledName || !layout.data.begin || !layout.rdata.begin || !layout.text.begin) return nullptr;
    const size_t nameLen = std::strlen(mangledName);
    if (nameLen == 0 || nameLen > 120) return nullptr;

    // 1) Scan .data for TypeDescriptor whose +8 name matches mangledName + '\0'.
    const uint8_t* dataStart = reinterpret_cast<const uint8_t*>(layout.data.begin);
    const uint8_t* dataEnd = reinterpret_cast<const uint8_t*>(layout.data.end);
    std::vector<uintptr_t> tdCandidates;

    const void* typeInfoVtbl = nullptr;
    // Also limit search to reasonable committed initial part of .data if huge BSS exists:
    // Use VirtualQuery chunks rather than trusting raw VirtualSize blindly if guard pages exist.
    uintptr_t cur = layout.data.begin;
    while (cur < layout.data.end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        const uintptr_t chunkEnd = std::min<uintptr_t>(layout.data.end, reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(cur);
            const size_t avail = chunkEnd - cur;
            const void* found = nullptr;
            size_t searchOff = 0;
            while (searchOff + 8 + nameLen + 1 <= avail) {
                const void* hit = std::memchr(p + searchOff, mangledName[0], avail - searchOff - nameLen);
                if (!hit) break;
                const size_t idx = static_cast<const uint8_t*>(hit) - p;
                if (idx >= 8 && idx + nameLen < avail &&
                    std::memcmp(p + idx, mangledName, nameLen + 1) == 0) {
                    const uintptr_t tdVa = cur + idx - 8;
                    if ((tdVa & 3u) == 0) {
                        uint32_t vptr = 0;
                        if (core::SafeReadDword(reinterpret_cast<const void*>(tdVa), &vptr) &&
                            vptr >= layout.rdata.begin && vptr < layout.rdata.end) {
                            (void)typeInfoVtbl;
                            tdCandidates.push_back(tdVa);
                        }
                    }
                }
                searchOff = idx + 1;
            }
        }
        if (chunkEnd <= cur) break;
        cur = chunkEnd;
    }
    if (tdCandidates.empty()) return nullptr;

    // 2) Scan .rdata aligned DWORDs for MSVC x86 COL {sig=0, offset=0, cdOffset=0, tdVa, chdVa in .rdata}
    uintptr_t colVa = 0;
    for (uintptr_t a = (layout.rdata.begin + 3u) & ~uintptr_t(3u); a + 20 <= layout.rdata.end; a += 4) {
        const auto* dw = reinterpret_cast<const uint32_t*>(a);
        if (dw[0] == 0 && dw[1] == 0 && dw[2] == 0 &&
            dw[4] >= layout.rdata.begin && dw[4] < layout.rdata.end) {
            for (uintptr_t tdVa : tdCandidates) {
                if (dw[3] == tdVa) {
                    colVa = a;
                    break;
                }
            }
            if (colVa) break;
        }
    }
    if (!colVa) return nullptr;

    // 3) Scan .rdata for vtable[-1] == colVa with vtable[0] in .text
    for (uintptr_t a = (layout.rdata.begin + 3u) & ~uintptr_t(3u); a + 8 <= layout.rdata.end; a += 4) {
        const auto* dw = reinterpret_cast<const uint32_t*>(a);
        if (dw[0] == colVa && dw[1] >= layout.text.begin && dw[1] < layout.text.end) {
            return reinterpret_cast<uintptr_t*>(a + 4);
        }
    }
    return nullptr;
}

ResolvedTarget ResolveTarget(const ModuleLayout& layout, const TargetSpec& spec) {
    ResolvedTarget res;
    if (!layout.text.begin || !spec.idaPattern || spec.idaPattern[0] == '\0') {
        res.detail = "invalid_layout_or_pattern";
        return res;
    }

    uintptr_t patternVa = 0;
    try {
        auto pat = hook::txn::make_range_pattern(layout.text.begin, layout.text.end, spec.idaPattern);
        res.patternMatchCount = pat.size();
        if (res.patternMatchCount == 1) {
            patternVa = reinterpret_cast<uintptr_t>(pat.get(0).get<void>());
        }
    } catch (...) {
        res.detail = "pattern_exception";
        return res;
    }

    if (spec.rttiMangled && spec.vtableSlot >= 0) {
        res.rttiAnchored = true;
        uintptr_t* vt = FindPrimaryVtableByRtti(layout, spec.rttiMangled);
        if (vt) {
            uint32_t fnVa = 0;
            if (core::SafeReadDword(vt + spec.vtableSlot, &fnVa) &&
                fnVa >= layout.text.begin && fnVa < layout.text.end) {
                res.rttiSlotRva = static_cast<uint32_t>(fnVa - layout.base);
                res.rttiMatches = (patternVa != 0 && fnVa == patternVa);
            }
        }
    }

    if (res.patternMatchCount != 1) {
        char buf[128];
        snprintf(buf, sizeof(buf), "fail_closed_pattern_count=%zu", res.patternMatchCount);
        res.detail = buf;
        return res;
    }

    if (res.rttiAnchored && !res.rttiMatches) {
        char buf[160];
        snprintf(buf, sizeof(buf), "fail_closed_rtti_mismatch pat=0x%x rtti_slot=0x%x",
                 static_cast<uint32_t>(patternVa - layout.base), res.rttiSlotRva);
        res.detail = buf;
        return res;
    }

    res.ok = true;
    res.va = patternVa;
    res.rva = static_cast<uint32_t>(patternVa - layout.base);
    res.rvaMatchesBaseline = (spec.expectedRva == 0 || res.rva == spec.expectedRva);
    char buf[160];
    snprintf(buf, sizeof(buf), "OK rva=0x%x baseline=0x%x rtti=%s",
             res.rva, spec.expectedRva, res.rttiAnchored ? (res.rttiMatches ? "match" : "no") : "n/a");
    res.detail = buf;
    return res;
}

std::string FormatCodeAddress(uintptr_t exeBase, const ModuleLayout& layout, uintptr_t addr) {
    char buf[96];
    if (addr >= layout.text.begin && addr < layout.text.end) {
        snprintf(buf, sizeof(buf), "0x%x", static_cast<uint32_t>(addr - exeBase));
        return buf;
    }
    HMODULE hmod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &hmod) && hmod) {
        wchar_t modPath[MAX_PATH] = {};
        if (GetModuleFileNameW(hmod, modPath, MAX_PATH)) {
            std::wstring wp(modPath);
            size_t p = wp.find_last_of(L"\\/");
            std::string modName = core::WideToUtf8(p == std::wstring::npos ? wp : wp.substr(p + 1));
            snprintf(buf, sizeof(buf), "%s+0x%x", modName.c_str(),
                     static_cast<uint32_t>(addr - reinterpret_cast<uintptr_t>(hmod)));
            return buf;
        }
    }
    snprintf(buf, sizeof(buf), "va:0x%08x", static_cast<uint32_t>(addr));
    return buf;
}

std::string ReadObjectRttiName(const ModuleLayout& layout, const void* objPtr, uint32_t* outVtableRva) {
    if (!objPtr) return {};
    uint32_t vtableVa = 0;
    if (!core::SafeReadDword(objPtr, &vtableVa)) return {};
    if (vtableVa < layout.rdata.begin + 4 || vtableVa >= layout.rdata.end) return {};
    if (outVtableRva) *outVtableRva = static_cast<uint32_t>(vtableVa - layout.base);

    uint32_t colVa = 0;
    if (!core::SafeReadDword(reinterpret_cast<const void*>(vtableVa - 4), &colVa)) return {};
    if (colVa < layout.rdata.begin || colVa + 20 > layout.rdata.end) return {};

    uint32_t sig = 1;
    uint32_t tdVa = 0;
    if (!core::SafeReadDword(reinterpret_cast<const void*>(colVa), &sig) || sig != 0) return {};
    if (!core::SafeReadDword(reinterpret_cast<const void*>(colVa + 12), &tdVa)) return {};
    if (tdVa < layout.data.begin || tdVa + 16 > layout.data.end) return {};

    char raw[96] = {};
    size_t len = 0;
    if (!core::SafeCopyCString(reinterpret_cast<const char*>(tdVa + 8), raw, sizeof(raw), &len)) return {};
    if (std::strncmp(raw, ".?AV", 4) == 0) {
        std::string s(raw + 4);
        const size_t at = s.find("@@");
        if (at != std::string::npos) s.resize(at);
        return s;
    }
    return {};
}

}  // namespace aigiskr::resolver
