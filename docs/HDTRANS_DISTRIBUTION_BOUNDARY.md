# AigisKR & hdTrans Architecture and Licensing Distribution Boundary

> **Purpose**: Document the technical separation between the AigisKR runtime localization plugin (`AigisKR.asi`) and the out-of-process `hdTrans` host bridge (`AigisKR.HdTransHost.exe`), together with our conservative license compliance and corresponding-source packaging practices.

---

## 1. Component & Process Boundary

```text
Aigis_r.exe (Vanilla x86 game process)
  └─ scripts/AigisKR.asi (AigisKR core runtime localization)
       ├─ TranslationCore (Curated DBs), TagParser, KnowledgeLayer, AutoCache
       ├─ Never loads or links J2KEngine.dll in-process
       │
       │  (Parent-created anonymous stdin/stdout pipes;
       │   binary framed UTF-8 request/response strings only;
       │   AIGIS_HDTRANS_HOST_PROTOCOL = 1)
       ▼
scripts/AigisKR/bin/AigisKR.HdTransHost.exe (Independent x86 bridge)
  ├─ Separate Windows process (CREATE_NO_WINDOW + Job Object kill-on-close)
  ├─ Zero AigisKR game hooks, KnowledgeLayer, TranslationCore, or AutoCache code
  ├─ Treated conservatively as AGPL-3.0-only compatible component
  │
  │  (LoadLibraryExW explicit directory load; zero CWD search)
  ▼
Bundled (`scripts/AigisKR/hdtrans/`) OR User-Specified External (`HdTransPath`)
  └─ J2KEngine.dll + hdtrans-data/dicts.hdsnap (hdTrans v0.9.5, AGPL-3.0-only)
```

---

## 2. Compliance Posture & Separation of Concerns

1. **Out-of-Process Execution & Minimal Interface**:
   - `AigisKR.asi` and `AigisKR.HdTransHost.exe` run in separate OS address spaces and exchange only stateless UTF-8 translation strings (`HELLO`, `TRANSLATE`, `SHUTDOWN`) over anonymous stdin/stdout pipes.
   - No internal `hdTrans` data structures, memory pointers, C++ objects, game state objects, or shared static engine libraries cross the IPC boundary.
   - By design, `AigisKR.HdTransHost.exe` links zero AigisKR-specific localization logic (`KnowledgeLayer`, `TranslationCore`, `TagParser`, `CoverageTracker`, `ProbeHooks`).
2. **Unified `AGPL-3.0-only` Source Publication**:
   - In addition to maintaining the clean out-of-process IPC boundary above, AigisKR's own runtime plugin (`AigisKR.asi`), out-of-process host bridge (`AigisKR.HdTransHost.exe`), build scripts, and independently authored tools are all published under **`AGPL-3.0-only`** (`LICENSE`). Game-original text, proper nouns, and derived UI artwork remain the property of their respective copyright holders and are not relicensed under AGPL-3.0.
3. **Complete Source & Notice Fulfillment**:
   - **Bundled Notices in Binary Release (`AigisKR-0.1.0.zip`)**:
     - Root `LICENSE` (`AGPL-3.0-only`), `README_KO.md`, `KNOWN_LIMITATIONS.md`, `RELEASE_NOTES_KO.md`, and `THIRD_PARTY_NOTICES.md`.
     - Upstream `LICENSE`, `NOTICE`, `README.txt`, and `THIRD-PARTY-NOTICES.txt` preserved inside `scripts/AigisKR/hdtrans/`.
     - Duplicate compliance bundle stored under `LICENSES/hdTrans/` (`LICENSE`, `NOTICE`, `THIRD-PARTY-NOTICES.txt`, `SOURCE.txt`), plus `LICENSES/AigisKR-HdTransHost-AGPL-3.0.txt` and all other third-party licenses under `LICENSES/`.
   - **Companion Corresponding Source Package (`AigisKR-0.1.0-sources.zip`) & Public Repository (`https://github.com/kekwkekw/aigis-ko`)**:
     - Complete source code and CMake build instructions for both `AigisKR.asi` (`CMakeLists.txt`, `src/`, `tests/`, `packaging/`) and `AigisKR.HdTransHost.exe` (`tools/hdtrans_host/`) under `AGPL-3.0-only`.
     - Complete pinned upstream source snapshot for `sokcuri/hdTrans` `v0.9.5` (commit `acd67a792a99ac5da3d6622d4d3dd203e957acd1`, `dicts.txt` `0.9.5+ec1346c72426`), `THIRD-PARTY-NOTICES.txt`, and the consolidated Texture Toolkit local patch (`docs/patches/texture_toolkit_partial_patch_r3_2.patch`).
4. **Opt-Out Without Disabling Curated Localization**:
   - Users setting `[AutoTranslate] Enabled=0` in `scripts/AigisKR.ini` (or deleting `scripts/AigisKR/bin/AigisKR.HdTransHost.exe` / `scripts/AigisKR/hdtrans/`) never spawn `AigisKR.HdTransHost.exe` and never load `J2KEngine.dll`, while retaining full curated text/font/texture localization.
