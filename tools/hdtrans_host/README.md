# AigisKR.HdTransHost

Out-of-process 32-bit x86 IPC bridge (`AIGIS_HDTRANS_HOST_PROTOCOL = 1`) connecting AigisKR to `J2KEngine.dll` (`hdTrans`).

## Architecture & License Boundary
- This component dynamically loads `J2KEngine.dll` via `LoadLibraryExW` without searching the current working directory.
- It contains **zero** AigisKR game hooks, `TranslationCore`, `KnowledgeLayer`, or `AutoCache` code.
- It communicates strictly using UTF-8 strings framed over parent-created anonymous stdin/stdout binary pipes.
- Distributed under `AGPL-3.0-only` (see [`LICENSE`](LICENSE)) for full compatibility with bundled/external `hdTrans`.

## Standalone Build (Windows 32-bit x86, MSVC)
```powershell
cmake -S . -B build32 -A Win32
cmake --build build32 --config Release
```
Output binary: `build32/Release/AigisKR.HdTransHost.exe`.
