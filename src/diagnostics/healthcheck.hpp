#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifndef AIGISKR_VERSION_STR
#define AIGISKR_VERSION_STR "0.1.0-rc.1"
#endif

namespace aigiskr::diagnostics {

enum class BuildSafetyState {
    Known,
    UnknownCompatible,
    HooksDisabled,
};

enum class TextureProfileState {
    ManifestMissing,
    AwaitingTextures,
    SharedOnly,
    VanillaKnown,
    LegacyKnown,
    PartialUnknown,
};

struct SignatureCheckRecord {
    std::string name;
    bool required = true;
    size_t matchCount = 0;
    size_t expectedCount = 1;
    uint32_t rva = 0;
    bool armed = false;
    std::string note;
};

struct TextureHealthReport {
    TextureProfileState state = TextureProfileState::ManifestMissing;
    bool manifestPresent = false;
    bool statusPresent = false;
    size_t manifestRolesTotal = 0;
    size_t manifestConfirmedRoles = 0;
    size_t manifestUniqueHashes = 0;
    std::string manifestPath;
    std::string statusPath;
    std::string statusProfile;
    size_t injectedFilesCount = 0;
    size_t injectedActiveCount = 0;
    size_t failedInjectionsCount = 0;
    uint64_t skippedUploads = 0;
    std::vector<std::string> matchedVanillaRoles;
    std::vector<std::string> matchedLegacyRoles;
    std::vector<std::string> matchedSharedRoles;
    std::vector<std::string> failedRoles;
    std::vector<std::string> unknownWatchedHashes;
    std::string reason;
};

struct AutoTranslateHealthReport {
    bool enabled = false;
    std::string hostMode = "Auto";
    std::string hostVersion;
    uint16_t hostProtocolVer = 0;
    std::string hdtransEngineVer;
    std::string engineRootType = "none";
    bool hostSpawned = false;
    std::string knowledgeVersion;
};

struct HealthcheckReport {
    BuildSafetyState state = BuildSafetyState::HooksDisabled;
    std::string exePath;
    uint64_t fileSize = 0;
    std::string sha256;
    uint32_t peTimeDateStamp = 0;
    uint32_t peSizeOfImage = 0;
    uint16_t peMachine = 0;
    bool textHooksArmed = false;
    bool fontHookArmed = false;
    std::vector<SignatureCheckRecord> signatures;
    std::string reason;
    std::string pluginVersion = AIGISKR_VERSION_STR;
    std::string releasePackageVersion = AIGISKR_VERSION_STR;
    std::string translationDbVersion;
    std::string translationDbSha256;
    std::string textureManifestVersion;
    std::string textureManifestSha256;
    TextureHealthReport texture;
    AutoTranslateHealthReport autoTranslate;
};

constexpr uint64_t kKnownExeSize = 13175680ULL;
constexpr const char* kKnownExeSha256 =
    "d0be510fb38b1d6dfe63c3562c97e45f49d318bf7e155dee309d88efe963f92b";

// Compute host process EXE metadata (path, size, SHA-256, PE headers).
bool InspectHostBinary(HealthcheckReport& report);

// Evaluate overall BuildSafetyState given resolved signature records.
BuildSafetyState EvaluateBuildSafety(HealthcheckReport& report,
                                     bool textHookReady,
                                     bool fontHookReady);

// Evaluate Dual Healthcheck Axis 2: R2 texture manifest & runtime profile status.
TextureProfileState EvaluateTextureHealth(HealthcheckReport& report,
                                          const std::string& manifestPath,
                                          const std::string& ttStatusPath,
                                          bool requireFreshStatus = true);

const char* BuildSafetyStateName(BuildSafetyState state) noexcept;
const char* TextureProfileStateName(TextureProfileState state) noexcept;

// Format one-line summary for AigisKR.log and write JSON file.
std::string FormatHealthcheckSummaryLine(const HealthcheckReport& report);
bool WriteHealthcheckJson(const HealthcheckReport& report, const std::string& path);

}  // namespace aigiskr::diagnostics
