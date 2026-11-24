#include "diagnostics/healthcheck.hpp"

#include "core/logger.hpp"
#include "core/mini_json.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace aigiskr::diagnostics {

namespace {

bool IsStatusWrittenInCurrentProcess(const std::string& pathUtf8) {
    const std::wstring wpath = aigiskr::core::Utf8ToWide(pathUtf8);
    if (wpath.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
    if (!GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
        return true;
    }
    ULARGE_INTEGER writeTs{}, procTs{};
    writeTs.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    writeTs.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    procTs.LowPart = ftCreate.dwLowDateTime;
    procTs.HighPart = ftCreate.dwHighDateTime;
    return writeTs.QuadPart >= procTs.QuadPart;
}

std::vector<std::string> ExtractStringArray(const aigiskr::core::JsonValue& obj, const char* key) {
    std::vector<std::string> out;
    const auto* v = obj.Find(key);
    if (!v || v->type != aigiskr::core::JsonValue::Type::Array) return out;
    out.reserve(v->arrVal.size());
    for (const auto& item : v->arrVal) {
        if (item.type == aigiskr::core::JsonValue::Type::String && !item.strVal.empty()) {
            out.push_back(item.strVal);
        }
    }
    return out;
}

uint64_t ExtractUint64(const aigiskr::core::JsonValue& obj, const char* key, uint64_t fallback = 0) {
    const auto* v = obj.Find(key);
    if (!v || v->type != aigiskr::core::JsonValue::Type::Number || v->numVal < 0.0) return fallback;
    return static_cast<uint64_t>(v->numVal);
}

void AppendJsonStringArray(std::ostringstream& jss, const std::vector<std::string>& items) {
    jss << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) jss << ", ";
        jss << aigiskr::core::QuoteJsonString(items[i]);
    }
    jss << "]";
}

}  // namespace

bool InspectHostBinary(HealthcheckReport& report) {
    wchar_t exeW[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, exeW, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        report.reason = "GetModuleFileNameW failed";
        return false;
    }
    const std::wstring wpath(exeW, len);
    report.exePath = aigiskr::core::WideToUtf8(wpath);
    aigiskr::core::ComputeFileSha256Hex(wpath, report.sha256, report.fileSize);

    const HMODULE hMod = GetModuleHandleW(nullptr);
    if (hMod) {
        const auto* base = reinterpret_cast<const uint8_t*>(hMod);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                report.peMachine = nt->FileHeader.Machine;
                report.peTimeDateStamp = nt->FileHeader.TimeDateStamp;
                report.peSizeOfImage = nt->OptionalHeader.SizeOfImage;
            }
        }
    }
    return !report.sha256.empty();
}

BuildSafetyState EvaluateBuildSafety(HealthcheckReport& report,
                                     bool textHookReady,
                                     bool fontHookReady) {
    report.textHooksArmed = textHookReady;
    report.fontHookArmed = fontHookReady;

    bool requiredMissing = false;
    std::string failedNames;
    for (const auto& sig : report.signatures) {
        if (sig.required && sig.matchCount != sig.expectedCount) {
            requiredMissing = true;
            if (!failedNames.empty()) failedNames += ", ";
            failedNames += sig.name + "(" + std::to_string(sig.matchCount) + "/" +
                           std::to_string(sig.expectedCount) + ")";
        }
    }

    const bool knownHash = (report.sha256 == kKnownExeSha256 && report.fileSize == kKnownExeSize);

    if (requiredMissing || !textHookReady) {
        report.state = BuildSafetyState::HooksDisabled;
        report.reason = requiredMissing
                            ? ("required signature mismatch: " + failedNames)
                            : "text hook disabled by config or failed verification";
    } else if (knownHash) {
        report.state = BuildSafetyState::Known;
        report.reason = "exact baseline SHA-256 and unique signatures verified";
    } else {
        report.state = BuildSafetyState::UnknownCompatible;
        report.reason = "exe SHA-256 changed, but all unique .text signatures + RTTI/vtable checks passed";
    }
    return report.state;
}

const char* BuildSafetyStateName(BuildSafetyState state) noexcept {
    switch (state) {
        case BuildSafetyState::Known: return "KNOWN";
        case BuildSafetyState::UnknownCompatible: return "UNKNOWN_COMPATIBLE";
        case BuildSafetyState::HooksDisabled: return "HOOKS_DISABLED";
    }
    return "HOOKS_DISABLED";
}

const char* TextureProfileStateName(TextureProfileState state) noexcept {
    switch (state) {
        case TextureProfileState::ManifestMissing: return "MANIFEST_MISSING";
        case TextureProfileState::AwaitingTextures: return "AWAITING_TEXTURES";
        case TextureProfileState::SharedOnly: return "SHARED_ONLY";
        case TextureProfileState::VanillaKnown: return "VANILLA_KNOWN";
        case TextureProfileState::LegacyKnown: return "LEGACY_KNOWN";
        case TextureProfileState::PartialUnknown: return "MIXED_OR_PARTIAL_UNKNOWN";
    }
    return "MANIFEST_MISSING";
}

TextureProfileState EvaluateTextureHealth(HealthcheckReport& report,
                                          const std::string& manifestPath,
                                          const std::string& ttStatusPath,
                                          bool requireFreshStatus) {
    TextureHealthReport tex;
    tex.manifestPath = manifestPath;
    tex.statusPath = ttStatusPath;

    if (!manifestPath.empty()) {
        aigiskr::core::JsonValue root;
        std::string parseErr;
        if (aigiskr::core::ParseJsonFile(manifestPath, root, parseErr) &&
            root.type == aigiskr::core::JsonValue::Type::Object) {
            const auto* rolesVal = root.Find("roles");
            if (rolesVal && rolesVal->type == aigiskr::core::JsonValue::Type::Array) {
                tex.manifestPresent = true;
                tex.manifestRolesTotal = rolesVal->arrVal.size();
                std::unordered_set<std::string> uniqueHashes;
                for (const auto& entry : rolesVal->arrVal) {
                    if (entry.type != aigiskr::core::JsonValue::Type::Object) continue;
                    const auto* statusVal = entry.Find("status");
                    if (statusVal && statusVal->AsString() == "CONFIRMED") {
                        ++tex.manifestConfirmedRoles;
                    }
                    for (const auto& h : ExtractStringArray(entry, "all_hashes")) {
                        uniqueHashes.insert(h);
                    }
                }
                tex.manifestUniqueHashes = uniqueHashes.size();
                uint64_t schemaVer = ExtractUint64(root, "schema_version", 0);
                uint64_t manifestBytes = 0;
                std::string manifestSha;
                aigiskr::core::ComputeFileSha256Hex(aigiskr::core::Utf8ToWide(manifestPath), manifestSha, manifestBytes);
                report.textureManifestSha256 = manifestSha;
                report.textureManifestVersion = "schema_v" + std::to_string(schemaVer) +
                                                " (roles=" + std::to_string(tex.manifestConfirmedRoles) + "/" +
                                                std::to_string(tex.manifestRolesTotal) +
                                                ", hashes=" + std::to_string(tex.manifestUniqueHashes) + ")";
            }
        }
    }

    if (!tex.manifestPresent) {
        tex.state = TextureProfileState::ManifestMissing;
        tex.reason = "texture_manifest_r2.json missing or unreadable";
        report.textureManifestVersion = "MISSING";
        report.textureManifestSha256.clear();
        report.texture = std::move(tex);
        return report.texture.state;
    }

    bool statusFresh = !ttStatusPath.empty();
    if (statusFresh && requireFreshStatus) {
        statusFresh = IsStatusWrittenInCurrentProcess(ttStatusPath);
    }

    if (statusFresh) {
        aigiskr::core::JsonValue stRoot;
        std::string parseErr;
        if (aigiskr::core::ParseJsonFile(ttStatusPath, stRoot, parseErr) &&
            stRoot.type == aigiskr::core::JsonValue::Type::Object) {
            tex.statusPresent = true;
            if (const auto* profVal = stRoot.Find("profile")) {
                tex.statusProfile = std::string(profVal->AsString());
            }
            tex.injectedFilesCount = static_cast<size_t>(ExtractUint64(stRoot, "injected_files_count"));
            tex.injectedActiveCount = static_cast<size_t>(ExtractUint64(stRoot, "injected_active_count"));
            tex.failedInjectionsCount = static_cast<size_t>(ExtractUint64(stRoot, "failed_injections_count"));
            tex.skippedUploads = ExtractUint64(stRoot, "skipped_uploads");
            tex.matchedVanillaRoles = ExtractStringArray(stRoot, "matched_vanilla_roles");
            tex.matchedLegacyRoles = ExtractStringArray(stRoot, "matched_legacy_roles");
            tex.matchedSharedRoles = ExtractStringArray(stRoot, "matched_shared_roles");
            tex.failedRoles = ExtractStringArray(stRoot, "failed_roles");
            tex.unknownWatchedHashes = ExtractStringArray(stRoot, "unknown_watched_hashes");

            if (!tex.failedRoles.empty() || tex.failedInjectionsCount > 0 ||
                tex.statusProfile == "MIXED_OR_PARTIAL_UNKNOWN") {
                tex.state = TextureProfileState::PartialUnknown;
                tex.reason = tex.failedRoles.empty()
                                 ? "TextureToolkit detected mixed vanilla+legacy hashes or unknown watched-dim hashes"
                                 : ("texture injection failed roles detected (" + std::to_string(tex.failedRoles.size()) + ")");
            } else if (tex.statusProfile == "VANILLA_KNOWN") {
                tex.state = TextureProfileState::VanillaKnown;
                tex.reason = "vanilla UI role hashes matched and manifest verified";
            } else if (tex.statusProfile == "LEGACY_KNOWN") {
                tex.state = TextureProfileState::LegacyKnown;
                tex.reason = "legacy UI role hashes matched and manifest verified";
            } else if (tex.statusProfile == "SHARED_ONLY") {
                tex.state = TextureProfileState::SharedOnly;
                tex.reason = "only shared cross-profile UI hashes observed so far";
            } else if (tex.statusProfile == "MANIFEST_MISSING") {
                tex.state = TextureProfileState::ManifestMissing;
                tex.reason = "TextureToolkit reported missing manifest";
            } else {
                tex.state = TextureProfileState::AwaitingTextures;
                tex.reason = "manifest ready; awaiting UI atlas uploads";
            }
        }
    }

    if (!tex.statusPresent) {
        tex.state = TextureProfileState::AwaitingTextures;
        tex.reason = "manifest verified; waiting for TextureToolkit runtime profile telemetry";
    }

    report.texture = std::move(tex);
    return report.texture.state;
}

std::string FormatHealthcheckSummaryLine(const HealthcheckReport& report) {
    std::ostringstream oss;
    oss << "[Healthcheck] state=" << BuildSafetyStateName(report.state)
        << " size=" << report.fileSize
        << " sha256=" << (report.sha256.size() >= 16 ? report.sha256.substr(0, 16) : report.sha256)
        << " pe_ts=0x" << std::hex << report.peTimeDateStamp
        << std::dec
        << " text_armed=" << (report.textHooksArmed ? 1 : 0)
        << " font_armed=" << (report.fontHookArmed ? 1 : 0)
        << " tex_profile=" << TextureProfileStateName(report.texture.state)
        << " manifest_roles=" << report.texture.manifestConfirmedRoles << "/" << report.texture.manifestRolesTotal
        << " active_inject=" << report.texture.injectedActiveCount
        << " reason=\"" << report.reason << "\"";
    return oss.str();
}

bool WriteHealthcheckJson(const HealthcheckReport& report, const std::string& path) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::ostringstream jss;
    jss << "{\n";
    jss << "  \"plugin_version\": " << aigiskr::core::QuoteJsonString(report.pluginVersion) << ",\n";
    jss << "  \"release_package_version\": " << aigiskr::core::QuoteJsonString(report.releasePackageVersion) << ",\n";
    jss << "  \"translation_db_version\": " << aigiskr::core::QuoteJsonString(report.translationDbVersion) << ",\n";
    jss << "  \"translation_db_sha256\": " << aigiskr::core::QuoteJsonString(report.translationDbSha256) << ",\n";
    jss << "  \"texture_manifest_version\": " << aigiskr::core::QuoteJsonString(report.textureManifestVersion) << ",\n";
    jss << "  \"texture_manifest_sha256\": " << aigiskr::core::QuoteJsonString(report.textureManifestSha256) << ",\n";
    jss << "  \"state\": " << aigiskr::core::QuoteJsonString(BuildSafetyStateName(report.state)) << ",\n";
    jss << "  \"exe_path\": " << aigiskr::core::QuoteJsonString(report.exePath) << ",\n";
    jss << "  \"file_size\": " << report.fileSize << ",\n";
    jss << "  \"sha256\": " << aigiskr::core::QuoteJsonString(report.sha256) << ",\n";
    jss << "  \"known_sha256\": " << aigiskr::core::QuoteJsonString(kKnownExeSha256) << ",\n";
    jss << "  \"pe_machine\": " << report.peMachine << ",\n";
    jss << "  \"pe_timestamp\": " << report.peTimeDateStamp << ",\n";
    jss << "  \"pe_size_of_image\": " << report.peSizeOfImage << ",\n";
    jss << "  \"text_hooks_armed\": " << (report.textHooksArmed ? "true" : "false") << ",\n";
    jss << "  \"font_hook_armed\": " << (report.fontHookArmed ? "true" : "false") << ",\n";
    jss << "  \"reason\": " << aigiskr::core::QuoteJsonString(report.reason) << ",\n";
    jss << "  \"texture\": {\n";
    jss << "    \"state\": " << aigiskr::core::QuoteJsonString(TextureProfileStateName(report.texture.state)) << ",\n";
    jss << "    \"manifest_present\": " << (report.texture.manifestPresent ? "true" : "false") << ",\n";
    jss << "    \"status_present\": " << (report.texture.statusPresent ? "true" : "false") << ",\n";
    jss << "    \"manifest_roles_total\": " << report.texture.manifestRolesTotal << ",\n";
    jss << "    \"manifest_confirmed_roles\": " << report.texture.manifestConfirmedRoles << ",\n";
    jss << "    \"manifest_unique_hashes\": " << report.texture.manifestUniqueHashes << ",\n";
    jss << "    \"status_profile\": " << aigiskr::core::QuoteJsonString(report.texture.statusProfile) << ",\n";
    jss << "    \"injected_files_count\": " << report.texture.injectedFilesCount << ",\n";
    jss << "    \"injected_active_count\": " << report.texture.injectedActiveCount << ",\n";
    jss << "    \"failed_injections_count\": " << report.texture.failedInjectionsCount << ",\n";
    jss << "    \"skipped_uploads\": " << report.texture.skippedUploads << ",\n";
    jss << "    \"matched_vanilla_roles\": "; AppendJsonStringArray(jss, report.texture.matchedVanillaRoles); jss << ",\n";
    jss << "    \"matched_legacy_roles\": "; AppendJsonStringArray(jss, report.texture.matchedLegacyRoles); jss << ",\n";
    jss << "    \"matched_shared_roles\": "; AppendJsonStringArray(jss, report.texture.matchedSharedRoles); jss << ",\n";
    jss << "    \"failed_roles\": "; AppendJsonStringArray(jss, report.texture.failedRoles); jss << ",\n";
    jss << "    \"unknown_watched_hashes\": "; AppendJsonStringArray(jss, report.texture.unknownWatchedHashes); jss << ",\n";
    jss << "    \"reason\": " << aigiskr::core::QuoteJsonString(report.texture.reason) << "\n";
    jss << "  },\n";
    jss << "  \"auto_translate\": {\n";
    jss << "    \"enabled\": " << (report.autoTranslate.enabled ? "true" : "false") << ",\n";
    jss << "    \"host_mode\": " << aigiskr::core::QuoteJsonString(report.autoTranslate.hostMode) << ",\n";
    jss << "    \"host_version\": " << aigiskr::core::QuoteJsonString(report.autoTranslate.hostVersion) << ",\n";
    jss << "    \"host_protocol_ver\": " << report.autoTranslate.hostProtocolVer << ",\n";
    jss << "    \"hdtrans_engine_ver\": " << aigiskr::core::QuoteJsonString(report.autoTranslate.hdtransEngineVer) << ",\n";
    jss << "    \"engine_root_type\": " << aigiskr::core::QuoteJsonString(report.autoTranslate.engineRootType) << ",\n";
    jss << "    \"host_spawned\": " << (report.autoTranslate.hostSpawned ? "true" : "false") << ",\n";
    jss << "    \"knowledge_version\": " << aigiskr::core::QuoteJsonString(report.autoTranslate.knowledgeVersion) << "\n";
    jss << "  },\n";
    jss << "  \"signatures\": [\n";
    for (size_t i = 0; i < report.signatures.size(); ++i) {
        const auto& s = report.signatures[i];
        char rvaHex[16];
        std::snprintf(rvaHex, sizeof(rvaHex), "0x%x", s.rva);
        jss << "    {\"name\": " << aigiskr::core::QuoteJsonString(s.name)
            << ", \"required\": " << (s.required ? "true" : "false")
            << ", \"match_count\": " << s.matchCount
            << ", \"expected_count\": " << s.expectedCount
            << ", \"rva\": " << aigiskr::core::QuoteJsonString(rvaHex)
            << ", \"armed\": " << (s.armed ? "true" : "false")
            << ", \"note\": " << aigiskr::core::QuoteJsonString(s.note) << "}";
        if (i + 1 < report.signatures.size()) jss << ",";
        jss << "\n";
    }
    jss << "  ]\n";
    jss << "}\n";

    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    const std::string body = jss.str();
    ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(ofs);
}

}  // namespace aigiskr::diagnostics
