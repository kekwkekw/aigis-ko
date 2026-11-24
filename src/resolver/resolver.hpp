#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aigiskr::resolver {

struct PeRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;
    uintptr_t rvaBegin = 0;
};

struct ModuleLayout {
    uintptr_t base = 0;
    PeRange text;
    PeRange rdata;
    PeRange data;
};

bool InspectModuleLayout(void* hmod, ModuleLayout& out);

struct TargetSpec {
    const char* name = "";
    const char* idaPattern = "";       // single '?' wildcards per Hooking.Patterns
    const char* rttiMangled = nullptr; // optional ".?AV...@\@"
    int vtableSlot = -1;               // >= 0 if rtti anchored
    uint32_t expectedRva = 0;          // baseline reference (for drift logging only)
};

struct ResolvedTarget {
    bool ok = false;
    uintptr_t va = 0;
    uint32_t rva = 0;
    size_t patternMatchCount = 0;
    bool rttiAnchored = false;
    bool rttiMatches = false;
    uint32_t rttiSlotRva = 0;
    bool rvaMatchesBaseline = false;
    std::string detail;
};

uintptr_t* FindPrimaryVtableByRtti(const ModuleLayout& layout, const char* mangledName);
ResolvedTarget ResolveTarget(const ModuleLayout& layout, const TargetSpec& spec);

// Safely formats caller address as "0xXXXXXX" (exe RVA) or "mod+0x..."
std::string FormatCodeAddress(uintptr_t exeBase, const ModuleLayout& layout, uintptr_t addr);

// Safely reads MSVC x86 RTTI class name from a polymorphic object pointer (returns "" if none).
std::string ReadObjectRttiName(const ModuleLayout& layout, const void* objPtr, uint32_t* outVtableRva = nullptr);

}  // namespace aigiskr::resolver
