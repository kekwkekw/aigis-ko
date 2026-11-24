#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aigiskr::core {

enum class TagKind {
    Literal,
    Parameter,
    Style,
    EngineControl,
    UnknownTag,
};

struct ProtectedTag {
    TagKind kind = TagKind::UnknownTag;
    std::string token;

    bool operator==(const ProtectedTag& other) const noexcept {
        return kind == other.kind && token == other.token;
    }
    bool operator<(const ProtectedTag& other) const noexcept {
        if (kind != other.kind) {
            return static_cast<int>(kind) < static_cast<int>(other.kind);
        }
        return token < other.token;
    }
};

struct ProtectedTagSpan {
    size_t offset = 0;
    size_t length = 0;
    TagKind kind = TagKind::UnknownTag;
};

struct MaskedText {
    std::string masked;
    std::vector<ProtectedTag> tokens;
};

struct TagValidationResult {
    bool valid = true;
    std::string reason;
    std::vector<ProtectedTag> sourceTags;
    std::vector<ProtectedTag> candidateTags;
};

// Normalize CRLF/CR line endings to LF without allocating when '\r' is absent.
std::string NormalizeLineEndings(std::string_view input);

// Locate all protected tag spans (%c[...], <...>, [...], printf specifiers, %%) in order.
std::vector<ProtectedTagSpan> ExtractProtectedTagSpans(std::string_view text);

// Extract all protected tags (%c[...], <...>, [...], printf specifiers, %%).
std::vector<ProtectedTag> ExtractProtectedTags(std::string_view text);

// Replace protected tags with deterministic sentinels (__AIGIS_TAG_0000__, ...).
MaskedText MaskProtectedTags(std::string_view text);

// Restore sentinels (__AIGIS_TAG_0000__, ...) back to the exact original protected tags.
bool RestoreProtectedTags(std::string_view translatedMasked,
                          const std::vector<ProtectedTag>& tokens,
                          std::string& outRestored,
                          std::string* reasonOut = nullptr);

// Classify a single extracted token into TagKind.
TagKind ClassifyTag(std::string_view token);

// Compare multiset equality (including exact case, order-independent) of protected tags.
TagValidationResult ValidateTagPreservation(std::string_view source, std::string_view candidate);

// Human-readable name of TagKind for diagnostics/tests.
const char* TagKindName(TagKind kind);

}  // namespace aigiskr::core
