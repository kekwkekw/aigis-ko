#include "core/config.hpp"
#include "core/logger.hpp"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <string>

namespace aigiskr::core {

namespace {

int ReadIniInt(const wchar_t* sec, const wchar_t* key, int defVal, const wchar_t* path) {
    return GetPrivateProfileIntW(sec, key, defVal, path);
}

bool AsciiIEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string TrimAscii(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string ReadIniUtf8(const wchar_t* sec, const wchar_t* key, const wchar_t* defVal, const wchar_t* path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") == 0 && f) {
        std::string raw;
        char chunk[4096];
        size_t n = 0;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
            raw.append(chunk, n);
        }
        std::fclose(f);

        const bool isUtf16Le = raw.size() >= 2 &&
            static_cast<unsigned char>(raw[0]) == 0xFF &&
            static_cast<unsigned char>(raw[1]) == 0xFE;
        if (!isUtf16Le) {
            if (raw.size() >= 3 &&
                static_cast<unsigned char>(raw[0]) == 0xEF &&
                static_cast<unsigned char>(raw[1]) == 0xBB &&
                static_cast<unsigned char>(raw[2]) == 0xBF) {
                raw.erase(0, 3);
            }
            const std::string wantSec = WideToUtf8(sec);
            const std::string wantKey = WideToUtf8(key);
            std::string curSec;
            size_t pos = 0;
            while (pos < raw.size()) {
                size_t nl = raw.find('\n', pos);
                std::string line = (nl == std::string::npos) ? raw.substr(pos) : raw.substr(pos, nl - pos);
                pos = (nl == std::string::npos) ? raw.size() : nl + 1;
                std::string trimmed = TrimAscii(line);
                if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;
                if (trimmed.front() == '[' && trimmed.back() == ']') {
                    curSec = TrimAscii(trimmed.substr(1, trimmed.size() - 2));
                    continue;
                }
                if (AsciiIEquals(curSec, wantSec)) {
                    const size_t eq = trimmed.find('=');
                    if (eq != std::string::npos) {
                        const std::string k = TrimAscii(trimmed.substr(0, eq));
                        if (AsciiIEquals(k, wantKey)) {
                            return TrimAscii(trimmed.substr(eq + 1));
                        }
                    }
                }
            }
            return WideToUtf8(defVal ? defVal : L"");
        }
    }

    wchar_t buf[512] = {};
    GetPrivateProfileStringW(sec, key, defVal, buf, _countof(buf), path);
    return WideToUtf8(buf);
}

bool IsAbsolutePathW(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L':') return true;
    if (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/')) return true;
    return false;
}

std::wstring EnsureTrailingSlash(std::wstring dir) {
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
        dir.push_back(L'\\');
    }
    return dir;
}

std::wstring ExtractDirectoryWithSlash(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return {};
    return path.substr(0, pos + 1);
}

std::wstring ReadIniStringW(const wchar_t* sec, const wchar_t* key, const wchar_t* defVal, const wchar_t* path) {
    // Release INIs are UTF-8; Win32 profile APIs interpret BOM-less files as ACP.
    // Keep the legacy ACP fallback for existing user-created INIs.
    const std::string utf8 = ReadIniUtf8(sec, key, defVal, path);
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed > 0) {
        std::wstring value(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), value.data(), needed);
        return value;
    }
    wchar_t buf[MAX_PATH] = {};
    GetPrivateProfileStringW(sec, key, defVal ? defVal : L"", buf, _countof(buf), path);
    return std::wstring(buf);
}

// Resolve runtime input files relative to pluginDir first (scripts/AigisKR/data/...),
// falling back to gameDir only when present there and absent under pluginDir.
std::wstring ResolveInputPath(const std::wstring& relOrAbs,
                              const std::wstring& pluginDir,
                              const std::wstring& gameDir) {
    if (relOrAbs.empty()) return {};
    if (IsAbsolutePathW(relOrAbs)) return relOrAbs;
    const std::wstring primaryBase = pluginDir.empty() ? gameDir : pluginDir;
    const std::wstring primary = primaryBase + relOrAbs;
    if (GetFileAttributesW(primary.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return primary;
    }
    if (!gameDir.empty() && gameDir != primaryBase) {
        const std::wstring secondary = gameDir + relOrAbs;
        if (GetFileAttributesW(secondary.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return secondary;
        }
    }
    return primary;
}

}  // namespace

const char* HdTransModeName(HdTransMode m) noexcept {
    switch (m) {
        case HdTransMode::Auto: return "Auto";
        case HdTransMode::Bundled: return "Bundled";
        case HdTransMode::External: return "External";
    }
    return "Auto";
}

ProbeConfig LoadProbeConfig(const std::wstring& iniPath,
                            const std::wstring& gameDir,
                            const std::wstring& pluginDir) {
    ProbeConfig cfg;
    const std::wstring normGameDir = EnsureTrailingSlash(gameDir);
    const std::wstring derivedPlugin = pluginDir.empty() ? ExtractDirectoryWithSlash(iniPath) : EnsureTrailingSlash(pluginDir);
    const std::wstring normPluginDir = derivedPlugin.empty() ? normGameDir : derivedPlugin;
    cfg.gameDir = normGameDir;
    cfg.pluginDir = normPluginDir;

    // Release-safe defaults apply even when AigisKR.ini is absent.
    cfg.enableHooks = (ReadIniInt(L"Probe", L"EnableHooks", 1, iniPath.c_str()) != 0);
    // Legacy Phase1/2/3/4 modes no longer gate production translation.

    const int hasEnableTextHooks = ReadIniInt(L"Probe", L"EnableTextHooks", -1, iniPath.c_str());
    const int hasLogTextEvents = ReadIniInt(L"Probe", L"LogTextEvents", -1, iniPath.c_str());
    const int legacyTextDraw = ReadIniInt(L"Probe", L"LogALTextDraw", -1, iniPath.c_str());
    cfg.enableTextHooks = (hasEnableTextHooks >= 0)
                              ? (hasEnableTextHooks != 0)
                              : (legacyTextDraw >= 0 ? (legacyTextDraw != 0) : true);
    cfg.logTextEvents = (hasLogTextEvents >= 0)
                            ? (hasLogTextEvents != 0)
                            : (hasEnableTextHooks < 0 && legacyTextDraw > 0);
    cfg.logALTextProp = (ReadIniInt(L"Probe", L"LogALTextProp", 0, iniPath.c_str()) != 0);
    cfg.logALFontW3 = (ReadIniInt(L"Probe", L"LogALFontW3", 0, iniPath.c_str()) != 0);
    cfg.deduplicate = (ReadIniInt(L"Probe", L"Deduplicate", 1, iniPath.c_str()) != 0);
    cfg.maxCaptureEntries = static_cast<uint32_t>(ReadIniInt(L"Probe", L"MaxCaptureEntries", 1500, iniPath.c_str()));
    cfg.maxLogsPerSecond = static_cast<uint32_t>(ReadIniInt(L"Probe", L"MaxLogsPerSecond", 200, iniPath.c_str()));

    const std::wstring sampleRel = ReadIniStringW(L"Phase4", L"SampleDict", L"", iniPath.c_str());
    cfg.sampleDictPath = ResolveInputPath(sampleRel, normPluginDir, normGameDir);

    // R1/R3 localization DB paths default under scripts/AigisKR/data/ relative to AigisKR.asi.
    cfg.enableTranslation = cfg.enableHooks && (ReadIniInt(L"R1", L"EnableTranslation", 1, iniPath.c_str()) != 0);
    const std::wstring r1DbRel = ReadIniStringW(L"R1", L"TranslationsDb", L"AigisKR\\data\\translations.json", iniPath.c_str());
    if (!r1DbRel.empty()) {
        cfg.translationsDbPath = ResolveInputPath(r1DbRel, normPluginDir, normGameDir);
    } else if (!cfg.sampleDictPath.empty()) {
        cfg.translationsDbPath = cfg.sampleDictPath;
    }

    const std::wstring r1TplRel = ReadIniStringW(L"R1", L"TemplatesDb", L"AigisKR\\data\\templates.json", iniPath.c_str());
    cfg.templatesDbPath = ResolveInputPath(r1TplRel, normPluginDir, normGameDir);

    const std::wstring r2NoticeRel = ReadIniStringW(L"R2", L"NoticeOverlayDb", L"AigisKR\\data\\notices.json", iniPath.c_str());
    cfg.noticeOverlayPath = ResolveInputPath(r2NoticeRel, normPluginDir, normGameDir);

    cfg.enableCoverage = cfg.enableHooks && (ReadIniInt(L"Coverage", L"EnableCoverage", 0, iniPath.c_str()) != 0);
    const std::wstring covRel = ReadIniStringW(L"Coverage", L"CoverageDir", L"AigisKR\\coverage", iniPath.c_str());
    if (!covRel.empty()) {
        cfg.coverageDir = IsAbsolutePathW(covRel) ? covRel : (normPluginDir + covRel);
    }
    const std::wstring phRel = ReadIniStringW(L"Coverage", L"PlaceholdersPath", L"", iniPath.c_str());
    cfg.placeholdersPath = cfg.enableCoverage ? ResolveInputPath(phRel, normPluginDir, normGameDir) : std::wstring();
    cfg.coverageFlushIntervalMs = static_cast<uint32_t>(ReadIniInt(L"Coverage", L"FlushIntervalMs", 15000, iniPath.c_str()));

    cfg.enableFontStress = cfg.enableHooks && (ReadIniInt(L"FontStress", L"EnableFontStress", 0, iniPath.c_str()) != 0);
    cfg.fontStressCycles = static_cast<uint32_t>(ReadIniInt(L"FontStress", L"Cycles", 2500, iniPath.c_str()));

    cfg.enableHangulFont = cfg.enableHooks && (ReadIniInt(L"Phase5", L"EnableHangulFont", 1, iniPath.c_str()) != 0);

    cfg.captureBackbuffer = (ReadIniInt(L"Diagnostics", L"CaptureBackbuffer", 0, iniPath.c_str()) != 0);
    cfg.captureIntervalMs = static_cast<uint32_t>(ReadIniInt(L"Diagnostics", L"CaptureIntervalMs", 2000, iniPath.c_str()));
    cfg.maxSnapshots = static_cast<uint32_t>(ReadIniInt(L"Diagnostics", L"MaxSnapshots", 30, iniPath.c_str()));
    const std::wstring snapRel = ReadIniStringW(L"Diagnostics", L"SnapshotDir", L"AigisKR_snaps", iniPath.c_str());
    cfg.snapshotDir = (IsAbsolutePathW(snapRel) ? EnsureTrailingSlash(snapRel) : (normGameDir + EnsureTrailingSlash(snapRel)));
    cfg.enableVirtualClick = (ReadIniInt(L"Diagnostics", L"EnableVirtualClick", 0, iniPath.c_str()) != 0);

    cfg.enableExtendedGlyphFallback = (ReadIniInt(L"Phase5", L"EnableExtendedGlyphFallback", 1, iniPath.c_str()) != 0);

    const std::wstring basePluginDir = cfg.pluginDir.empty() ? normGameDir : EnsureTrailingSlash(cfg.pluginDir);
    auto resolvePluginRel = [&](const std::wstring& val) -> std::wstring {
        if (val.empty()) {
            return {};
        }
        return IsAbsolutePathW(val) ? val : (basePluginDir + val);
    };

    auto& at = cfg.autoTranslate;
    at.enabled = (ReadIniInt(L"AutoTranslate", L"Enabled", 0, iniPath.c_str()) != 0);
    {
        const std::wstring provW = ReadIniStringW(L"AutoTranslate", L"Provider", L"hdTrans", iniPath.c_str());
        std::string prov;
        prov.reserve(provW.size());
        for (wchar_t ch : provW) {
            if (ch > 0 && ch < 0x80) {
                prov.push_back(static_cast<char>(ch));
            }
        }
        at.provider = prov.empty() ? std::string("hdTrans") : std::move(prov);
    }
    {
        const std::string modeStr = TrimAscii(ReadIniUtf8(L"AutoTranslate", L"HdTransMode", L"Auto", iniPath.c_str()));
        if (AsciiIEquals(modeStr, "Bundled")) {
            at.hdTransMode = HdTransMode::Bundled;
        } else if (AsciiIEquals(modeStr, "External")) {
            at.hdTransMode = HdTransMode::External;
        } else {
            at.hdTransMode = HdTransMode::Auto;
        }
    }
    {
        const std::wstring rawPath = ReadIniStringW(L"AutoTranslate", L"HdTransPath", L"", iniPath.c_str());
        if (!rawPath.empty()) {
            at.hdTransPath = IsAbsolutePathW(rawPath) ? rawPath : (normGameDir + rawPath);
        }
    }
    at.bundledHdTransPath = resolvePluginRel(ReadIniStringW(
        L"AutoTranslate", L"BundledHdTransPath", L"AigisKR\\hdtrans", iniPath.c_str()));
    at.hdTransHostPath = resolvePluginRel(ReadIniStringW(
        L"AutoTranslate", L"HdTransHostPath", L"AigisKR\\bin\\AigisKR.HdTransHost.exe", iniPath.c_str()));
    at.hostTimeoutMs = static_cast<uint32_t>(
        std::max<int>(50, ReadIniInt(L"AutoTranslate", L"HostTimeoutMs", 15000, iniPath.c_str())));
    at.cacheEnabled = (ReadIniInt(L"AutoTranslate", L"CacheEnabled", 1, iniPath.c_str()) != 0);
    at.cacheFile = resolvePluginRel(ReadIniStringW(
        L"AutoTranslate", L"CacheFile", L"AigisKR\\cache\\hdtrans_cache.jsonl", iniPath.c_str()));
    at.seedCacheFile = resolvePluginRel(ReadIniStringW(
        L"AutoTranslate", L"SeedCacheFile", L"AigisKR\\data\\hdtrans_seed_cache.jsonl", iniPath.c_str()));
    at.overridesFile = resolvePluginRel(ReadIniStringW(
        L"AutoTranslate", L"OverridesFile", L"AigisKR\\data\\overrides.json", iniPath.c_str()));
    at.knowledgeDir = resolvePluginRel(ReadIniStringW(
        L"AutoTranslate", L"KnowledgeDir", L"AigisKR\\data\\knowledge", iniPath.c_str()));
    at.retranslateStale = (ReadIniInt(L"AutoTranslate", L"RetranslateStale", 0, iniPath.c_str()) != 0);
    at.workerThreads = static_cast<uint32_t>(std::max<int>(1, ReadIniInt(L"AutoTranslate", L"WorkerThreads", 1, iniPath.c_str())));
    at.queueMax = static_cast<uint32_t>(std::max<int>(1, ReadIniInt(L"AutoTranslate", L"QueueMax", 4096, iniPath.c_str())));
    at.translateUI = (ReadIniInt(L"AutoTranslate", L"TranslateUI", 1, iniPath.c_str()) != 0);
    at.translateStory = (ReadIniInt(L"AutoTranslate", L"TranslateStory", 1, iniPath.c_str()) != 0);
    at.useSessionContext = (ReadIniInt(L"AutoTranslate", L"UseSessionContext", 0, iniPath.c_str()) != 0);
    at.minLength = static_cast<size_t>(std::max<int>(1, ReadIniInt(L"AutoTranslate", L"MinLength", 1, iniPath.c_str())));
    at.maxLength = static_cast<size_t>(std::max<int>(0, ReadIniInt(L"AutoTranslate", L"MaxLength", 0, iniPath.c_str())));
    at.logAutoTranslate = (ReadIniInt(L"AutoTranslate", L"LogAutoTranslate", 0, iniPath.c_str()) != 0);
    at.liveTextRefresh = (ReadIniInt(L"AutoTranslate", L"LiveTextRefresh", 1, iniPath.c_str()) != 0);

    return cfg;
}

}  // namespace aigiskr::core
