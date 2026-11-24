# AigisKR Corresponding Source Package (0.1.0)

This companion archive provides the complete corresponding source code and build scripts
for `AigisKR.asi`, `AigisKR.HdTransHost.exe`, and the bundled `hdTrans v0.9.5` components
shipped with `AigisKR-0.1.0.zip` under `AGPL-3.0-only`, together with the MIT-licensed
Texture Toolkit patch series.

## 1. Contents
- `CMakeLists.txt`, `src/`, `tests/`, `packaging/`: Complete `AigisKR.asi` x86 plugin source, unit tests, and configs (`AGPL-3.0-only`).
- `tools/hdtrans_host/`: Standalone 32-bit x86 anonymous-pipe IPC bridge (`AigisKR.HdTransHost.exe`, `AGPL-3.0-only`).
- `third_party/safetyhook/`, `third_party/Hooking.Patterns/`: Pinned CMake build dependencies (`BSL-1.0`, `MIT`; Zydis fetched by SafetyHook during configure).
- `upstream/hdTrans-0.9.5/`: Complete upstream `sokcuri/hdTrans` `v0.9.5` source snapshot (commit `acd67a792a99ac5da3d6622d4d3dd203e957acd1`, `dicts.txt` `0.9.5+ec1346c72426`, `AGPL-3.0-only`).
- `docs/patches/texture_toolkit_partial_patch_r3_2.patch`: Consolidated MIT-licensed patch series for Texture Toolkit.
- `LICENSE`, `LICENSES/`, `THIRD_PARTY_NOTICES.md`, `docs/`: License texts, notices, and provenance documentation.

## 2. Building `AigisKR.asi` and `AigisKR.HdTransHost.exe` (Windows x86, MSVC)
```powershell
cmake -S . -B build32 -A Win32
cmake --build build32 --config Release --target AigisKR AigisKR_HdTransHost translation_core_tests
```
Outputs: `build32/Release/AigisKR.asi` and `build32/tools/hdtrans_host/Release/AigisKR.HdTransHost.exe`.

## 3. Standalone `AigisKR.HdTransHost.exe` Build
```powershell
cd tools/hdtrans_host
cmake -S . -B build32 -A Win32
cmake --build build32 --config Release
```

## 4. Building Upstream `hdTrans` (`J2KEngine.dll` + `dicts.hdsnap`)
Refer to `upstream/hdTrans-0.9.5/CMakeLists.txt` and `upstream/hdTrans-0.9.5/CMakePresets.json`.
