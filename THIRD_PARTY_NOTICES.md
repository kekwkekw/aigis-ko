# Third-Party Notices

AigisKR (`0.1.0`) ships, statically links, or bundles the following open-source components. Research repositories not compiled into or shipped with the release package are excluded.

---

## 1. Ultimate ASI Loader
- **Project / Repository**: `https://github.com/ThirteenAG/Ultimate-ASI-Loader`
- **Version / Commit**: `56fa0b03920f2a71174126e03a05c77d48b74744`
- **License**: MIT (`LICENSES/Ultimate-ASI-Loader-MIT.txt`)
- **How Used**: Shipped as `winmm.dll` to load native x86 `.asi` plugins under `scripts/`.
- **Modified?**: No (upstream binary shipped unmodified with release configuration in `scripts/global.ini`).

## 2. SafetyHook
- **Project / Repository**: `https://github.com/cursey/safetyhook`
- **Version / Commit**: `f44cc070a8340f2f26649553c49533475417304d`
- **License**: Boost Software License 1.0 (`LICENSES/SafetyHook-BSL-1.0.txt`)
- **How Used**: Statically linked into `scripts/AigisKR.asi` for safe runtime x86 hooking.
- **Modified?**: No.

## 3. Zydis
- **Project / Repository**: `https://github.com/zyantific/zydis`
- **Version / Commit**: `v4.1.0`
- **License**: MIT (`LICENSES/Zydis-MIT.txt`)
- **How Used**: Statically linked into `scripts/AigisKR.asi` (via SafetyHook) for x86 instruction decoding.
- **Modified?**: No.

## 4. Zycore
- **Project / Repository**: `https://github.com/zyantific/zycore-c`
- **Version / Commit**: `v1.5.0`
- **License**: MIT (`LICENSES/Zycore-MIT.txt`)
- **How Used**: Statically linked into `scripts/AigisKR.asi` (dependency of Zydis).
- **Modified?**: No.

## 5. Hooking.Patterns
- **Project / Repository**: `https://github.com/ThirteenAG/Hooking.Patterns`
- **Version / Commit**: `460a47bc813085b66c79768531caab1ed5ea96fc`
- **License**: MIT (`LICENSES/Hooking.Patterns-MIT.txt`)
- **How Used**: Statically linked into `scripts/AigisKR.asi` for executable `.text` signature resolution.
- **Modified?**: No.

## 6. Texture Toolkit
- **Project / Repository**: `https://github.com/BadassBaboon/Texture-Toolkit`
- **Version / Commit**: `e3f891043afdeb18bf97e1ee0059e9d211e38541`
- **License**: MIT (`LICENSES/Texture-Toolkit-MIT.txt`)
- **How Used**: Built into `scripts/TextureToolkit-x86.asi` for non-destructive D3D11 runtime UI texture replacement (`TT/inject/*.dds` and optional partial rect patching under `TT/patches/`).
- **Modified?**: **Yes** (consolidated local patch preserved at `docs/patches/texture_toolkit_partial_patch_r3_2.patch` in both this repository and `AigisKR-0.1.0-sources.zip`, combining all 4 modifications below):
  - Track `D3D11_USAGE_DYNAMIC` textures initialized with `pInitialData` for AquaLead UI atlases (`r0`).
  - Added `[TextureToolkit] ProductionMode=1` dimension allowlisting (`r1`).
  - Added `RoleManifest` multi-hash profile mapping and telemetry export (`r2`).
  - Added `PartialPatch` selective RGBA rect overlay support (`src/PartialPatch.h`, `src/PartialPatch.cpp`, `r3.2`).

## 7. Dear ImGui
- **Project / Repository**: `https://github.com/ocornut/imgui`
- **Version / Commit**: `v1.92.5`
- **License**: MIT (`LICENSES/Dear-ImGui-MIT.txt`)
- **How Used**: Statically linked into `scripts/TextureToolkit-x86.asi`.
- **Modified?**: No.

## 8. MinHook
- **Project / Repository**: `https://github.com/TsudaKageyu/minhook`
- **Version / Commit**: `v1.3.3`
- **License**: BSD-2-Clause (`LICENSES/MinHook-BSD-2-Clause.txt`)
- **How Used**: Statically linked into `scripts/TextureToolkit-x86.asi`.
- **Modified?**: No.

## 9. ReShade API Format Header (`reshade_api_format.hpp`)
- **Project / Repository**: `https://github.com/crosire/reshade` (bundled in Texture Toolkit `deps/reshade/`)
- **Copyright / License**: Copyright (C) 2021 Patrick Mours — `BSD-3-Clause OR MIT`
- **How Used**: Compile-time header included when building `scripts/TextureToolkit-x86.asi`.
- **Modified?**: No.

## 10. hdTrans (`J2KEngine.dll` & Snapshot Dictionary)
- **Project / Repository**: `https://github.com/sokcuri/hdTrans`
- **Copyright / Author**: Copyright (c) Sokcuri
- **Version / Tag / Commit**: `0.9.5` / `v0.9.5` (`acd67a792a99ac5da3d6622d4d3dd203e957acd1`)
- **Dictionary Build (`dicts.txt`)**: `0.9.5+ec1346c72426`
- **License**: **`AGPL-3.0-only`** (`LICENSES/hdTrans/LICENSE`, `LICENSES/hdTrans/NOTICE`, `LICENSES/hdTrans/SOURCE.txt`)
- **Third-Party Libraries Bundled Inside `hdTrans`** (`LICENSES/hdTrans/THIRD-PARTY-NOTICES.txt`):
  - **PCRE2** `10.49` (`BSD-3-Clause WITH PCRE2-exception`) and **SLJIT** (`BSD-2-Clause`)
  - **libyaml** (`MIT`)
- **How Used**: Extracted verbatim from the official `hdTrans-0.9.5-windows.zip` release (`SHA-256: acdd19248a362fb8b84476c70470526e3aa41f08547527bc02fa72a3f54b9472`) under `scripts/AigisKR/hdtrans/` and executed solely out-of-process by `scripts/AigisKR/bin/AigisKR.HdTransHost.exe` over anonymous pipe UTF-8 string IPC.
- **Corresponding Source**: Bundled in companion archive `AigisKR-0.1.0-sources.zip` (containing full `AigisKR` & `AigisKR.HdTransHost` source and the exact upstream `hdTrans v0.9.5` source snapshot) and published at `https://github.com/kekwkekw/aigis-ko`.
- **Modified?**: No (`hdTrans` binaries and dictionaries are shipped completely unmodified).

## 11. AigisKR & AigisKR.HdTransHost
- **License**: **`AGPL-3.0-only`** (`LICENSE` and `LICENSES/AigisKR-HdTransHost-AGPL-3.0.txt`)
- **How Used**: `scripts/AigisKR.asi` provides the native x86 runtime localization hooks/text engine, and `scripts/AigisKR/bin/AigisKR.HdTransHost.exe` is the standalone 32-bit x86 out-of-process IPC bridge dynamically loading `J2KEngine.dll` via `LoadLibraryExW`. Full corresponding source is published in this repository and in `AigisKR-0.1.0-sources.zip`.

---

## System Fonts Notice
AigisKR renders Hangul and extended fallback glyphs dynamically at runtime using Windows system fonts via Win32 GDI (`GetGlyphOutlineW`). No proprietary or third-party `.ttf`/`.otf` font files are bundled in the release package.
