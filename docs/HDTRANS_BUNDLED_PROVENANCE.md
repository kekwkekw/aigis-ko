# AigisKR Bundled hdTrans Provenance Record

> **Target Release Candidate**: `AigisKR 0.1.0`  
> **Verification Rule**: Every bundled binary, dictionary, and notice file below was extracted directly from the official upstream Windows release archive verified against `SHA256SUMS.txt` (never copied from local developer directories).

---

## 1. Upstream Identification

| Field | Value |
| --- | --- |
| **Upstream Project** | [`sokcuri/hdTrans`](https://github.com/sokcuri/hdTrans) ("하드트랜스") |
| **Release Version / Tag** | `0.9.5` / `v0.9.5` (annotated tag `870004425b9c76d3042a014739f9fcd4f2e201a3`) |
| **Source Commit** | `acd67a792a99ac5da3d6622d4d3dd203e957acd1` (`chore(main): release 0.9.5`, `2026-11-09 00:11:48 +0000`) |
| **Dictionary Version (`dicts.txt`)** | `0.9.5+ec1346c72426` (`ec1346c72426` verified as first 12 hex chars of SHA-256 across sorted `dicts/*.tsv` in commit `acd67a79...`) |
| **License Expression** | **`AGPL-3.0-only`** (`LICENSE` / `NOTICE` inside extracted official archive) |
| **Official Windows Archive** | [`hdTrans-0.9.5-windows.zip`](https://github.com/sokcuri/hdTrans/releases/download/v0.9.5/hdTrans-0.9.5-windows.zip) (`36,674,315` bytes) |
| **Official Archive SHA-256** | `acdd19248a362fb8b84476c70470526e3aa41f08547527bc02fa72a3f54b9472` (matches official `SHA256SUMS.txt`) |
| **Upstream Source Archive (`v0.9.5.zip`)** | [`v0.9.5.zip`](https://github.com/sokcuri/hdTrans/archive/refs/tags/v0.9.5.zip) (`29,993,195` bytes, SHA-256 `7ad4d06d0acc245b4687e53669e32c20b10a9d4a4d2a168615831da6d011e1de`) |

---

## 2. Extracted Runtime Files & Cryptographic Verification

All files below were extracted cleanly from `hdTrans-0.9.5-windows.zip` and verified 100% byte-identical against the official upstream Windows release checksums:

| Bundled Relative Path (`scripts/AigisKR/hdtrans/...`) | Size (bytes) | SHA-256 | Role / Notes |
| --- | ---: | --- | --- |
| `J2KEngine.dll` | `7,114,752` | `e2bef2ee016e3e508f6b5607d15fd8e30d23e4009acbfaebaf0b540cc3581e35` | 32-bit x86 (`PE 0x014c`) `J2K_*` DLL; imports only `KERNEL32.dll`, `USER32.dll`, `bcrypt.dll` |
| `hdtrans-data/dicts.hdsnap` | `36,904,899` | `46111a76bf352452b81543e96aca84242fec4834cd422920ccb90e9512fe749c` | Compiled immutable snapshot dictionary |
| `hdtrans-data/dicts.txt` | `19` | `d88c9b3952c486eb3b93597d9de8d304be7003f820c48dd3a628a5f37a47b42a` | `0.9.5+ec1346c72426` |
| `LICENSE` | `34,523` | `0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0` | Full GNU AGPL v3.0 license text |
| `NOTICE` | `375` | `4825433c16f86e366900d62ea4463cd63478accbced52310086fcf318751566a` | Upstream copyright & `AGPL-3.0-only` notice (Sokcuri) |
| `README.txt` | `3,865` | `d7268061dc7dc0bcae80b42957e912c5354d381e0e95cbebbdeb28d99c2e8b19` | Upstream Russian/English/Korean distribution README |
| `THIRD-PARTY-NOTICES.txt` | `15,823` | `89b623e11a33711d0ccc65e9a368e6bdd286629ae9140b988937a74f773dbcf4` | PCRE2 (`BSD-3-Clause WITH PCRE2-exception`), SLJIT (`BSD-2-Clause`), libyaml (`MIT`) |
| `hdtrans-data/ehnd/*` (`4` files) | `1,458` | Verified verbatim | Upstream `ehnd` filter examples & README |
| `hdtrans-data/user/*` (`3` files) | `3,291` | Verified verbatim | Upstream `user` config/dictionary examples |

*(Note: Unused standalone executables/x64 DLLs from the upstream zip — `hdTransGUI.exe`, `hdtrans.exe`, `hdtrans_x86.exe`, `hdTransEngine.dll`, `hdTransEngine_x86.dll` — are omitted from the runtime `hdtrans/` folder while keeping `J2KEngine.dll` + `hdtrans-data/` + license/notice files intact in exact upstream relative paths.)*

---

## 3. Binary String Sanitization Audit on `J2KEngine.dll`

Scanning `J2KEngine.dll` with `tools/release/build_release.py` confirmed:
- Zero developer workstation paths (`C:\Users\...` matches = `0`).
- Two harmless upstream internal artifacts present in the unmodified official PE image:
  1. `windows-x86\J2KEngine.pdb` (upstream GitHub Actions build PDB path).
  2. `conjugation family (OUT/senses_prefer.tsv of ...)` (uppercase `OUT/senses_prefer.tsv` linguistic rule diagnostic string inside `hdTrans`).
- Both are explicitly checked/exempted strictly when `J2KEngine.dll` matches its verified official SHA-256 (`e2bef2ee...`).
