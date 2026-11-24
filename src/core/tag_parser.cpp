#include "core/tag_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace aigiskr::core {

namespace {

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool IsPrintfFlag(char c) noexcept {
    return c == '-' || c == '+' || c == '0';
}

bool IsPrintfSpecifier(char c) noexcept {
    switch (c) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'c':
        case 's':
        case 'p':
            return true;
        default:
            return false;
    }
}

bool IsKnownEngineAngleTag(std::string_view inner) noexcept {
    // Uppercase ASCII tag names / underscores / digits like <TIME>, <ATK>, <HERO_POW>
    if (inner.empty()) {
        return false;
    }
    for (char c : inner) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!((uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9') || uc == '_')) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string NormalizeLineEndings(std::string_view input) {
    if (input.find('\r') == std::string_view::npos) {
        return std::string(input);
    }
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                continue;
            }
            out.push_back('\n');
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

TagKind ClassifyTag(std::string_view token) {
    if (token == "%%") {
        return TagKind::Literal;
    }
    if (token.size() >= 4 && token.rfind("%c[", 0) == 0 && token.back() == ']') {
        return TagKind::Style;
    }
    if (token.size() >= 3 && token.front() == '<' && token.back() == '>') {
        const std::string_view inner = token.substr(1, token.size() - 2);
        return IsKnownEngineAngleTag(inner) ? TagKind::EngineControl : TagKind::UnknownTag;
    }
    if (token.size() >= 3 && token.front() == '[' && token.back() == ']') {
        return TagKind::Parameter;
    }
    if (token.size() >= 2 && token.front() == '%' && IsPrintfSpecifier(token.back())) {
        return TagKind::Parameter;
    }
    return TagKind::UnknownTag;
}

std::vector<ProtectedTagSpan> ExtractProtectedTagSpans(std::string_view text) {
    std::vector<ProtectedTagSpan> spans;
    const size_t n = text.size();

    for (size_t i = 0; i < n;) {
        const char ch = text[i];

        // 1. Style color tag: %c[...]
        if (ch == '%' && i + 2 < n && text[i + 1] == 'c' && text[i + 2] == '[') {
            const size_t close = text.find(']', i + 3);
            if (close != std::string_view::npos) {
                const size_t nl = text.find('\n', i + 3);
                if (nl == std::string_view::npos || close < nl) {
                    const size_t len = close - i + 1;
                    spans.push_back({i, len, ClassifyTag(text.substr(i, len))});
                    i = close + 1;
                    continue;
                }
            }
        }

        // 2. Escaped literal percent %%
        if (ch == '%' && i + 1 < n && text[i + 1] == '%') {
            spans.push_back({i, 2, TagKind::Literal});
            i += 2;
            continue;
        }

        // 3. Printf format specifier: %(?:[-+0]*(?:\d+)?(?:\.\d+)?)[spec]
        // Note: intentionally forbid spaces after '%' so "50% 상승" or "% 減少" is never misparsed.
        if (ch == '%' && i + 1 < n) {
            size_t j = i + 1;
            while (j < n && IsPrintfFlag(text[j])) {
                ++j;
            }
            while (j < n && IsDigit(text[j])) {
                ++j;
            }
            if (j < n && text[j] == '.') {
                ++j;
                while (j < n && IsDigit(text[j])) {
                    ++j;
                }
            }
            // Optional single length modifier ('l' or 'h')
            if (j < n && (text[j] == 'l' || text[j] == 'h')) {
                ++j;
            }
            if (j < n && IsPrintfSpecifier(text[j])) {
                // Reject "%c" not followed by '[' only if clearly prose? Actually "%c" alone can exist;
                // "%c[...]" was handled above. Safe unless followed directly by ASCII letters.
                const bool trailingAlpha = (j + 1 < n && std::isalpha(static_cast<unsigned char>(text[j + 1])));
                if (!trailingAlpha) {
                    spans.push_back({i, j - i + 1, TagKind::Parameter});
                    i = j + 1;
                    continue;
                }
            }
        }

        // 4. Angle bracket tag <...> (single line, non-empty, no nested '<')
        if (ch == '<') {
            const size_t close = text.find('>', i + 1);
            if (close != std::string_view::npos && close > i + 1) {
                const std::string_view inner = text.substr(i + 1, close - i - 1);
                if (inner.find('\n') == std::string_view::npos &&
                    inner.find('\r') == std::string_view::npos &&
                    inner.find('<') == std::string_view::npos) {
                    const size_t len = close - i + 1;
                    spans.push_back({i, len, ClassifyTag(text.substr(i, len))});
                    i = close + 1;
                    continue;
                }
            }
        }

        // 5. Square bracket parameter tag [...] (single line, non-empty, ASCII delimiters)
        if (ch == '[') {
            const size_t close = text.find(']', i + 1);
            if (close != std::string_view::npos && close > i + 1) {
                const std::string_view inner = text.substr(i + 1, close - i - 1);
                if (inner.find('\n') == std::string_view::npos &&
                    inner.find('\r') == std::string_view::npos &&
                    inner.find('[') == std::string_view::npos) {
                    const size_t len = close - i + 1;
                    spans.push_back({i, len, ClassifyTag(text.substr(i, len))});
                    i = close + 1;
                    continue;
                }
            }
        }

        ++i;
    }

    return spans;
}

std::vector<ProtectedTag> ExtractProtectedTags(std::string_view text) {
    const auto spans = ExtractProtectedTagSpans(text);
    std::vector<ProtectedTag> tags;
    tags.reserve(spans.size());
    for (const auto& s : spans) {
        tags.push_back({s.kind, std::string(text.substr(s.offset, s.length))});
    }
    return tags;
}

MaskedText MaskProtectedTags(std::string_view text) {
    MaskedText result;
    const auto spans = ExtractProtectedTagSpans(text);
    if (spans.empty()) {
        result.masked.assign(text);
        return result;
    }

    result.masked.reserve(text.size() + spans.size() * 16);
    result.tokens.reserve(spans.size());
    size_t cursor = 0;
    for (size_t idx = 0; idx < spans.size(); ++idx) {
        const auto& sp = spans[idx];
        result.masked.append(text.substr(cursor, sp.offset - cursor));
        char sentinel[32];
        const int n = std::snprintf(sentinel, sizeof(sentinel), "__AIGIS_TAG_%04zu__", idx);
        if (n > 0) {
            result.masked.append(sentinel, static_cast<size_t>(n));
        }
        result.tokens.push_back({sp.kind, std::string(text.substr(sp.offset, sp.length))});
        cursor = sp.offset + sp.length;
    }
    result.masked.append(text.substr(cursor));
    return result;
}

bool RestoreProtectedTags(std::string_view translatedMasked,
                          const std::vector<ProtectedTag>& tokens,
                          std::string& outRestored,
                          std::string* reasonOut) {
    static constexpr std::string_view kPrefix = "__AIGIS_TAG_";
    static constexpr std::string_view kSuffix = "__";

    outRestored.clear();
    outRestored.reserve(translatedMasked.size() + tokens.size() * 8);
    std::vector<size_t> counts(tokens.size(), 0);

    size_t pos = 0;
    while (pos < translatedMasked.size()) {
        const size_t hit = translatedMasked.find(kPrefix, pos);
        if (hit == std::string_view::npos) {
            outRestored.append(translatedMasked.substr(pos));
            break;
        }
        outRestored.append(translatedMasked.substr(pos, hit - pos));

        const size_t digitsStart = hit + kPrefix.size();
        size_t digitsEnd = digitsStart;
        while (digitsEnd < translatedMasked.size() && IsDigit(translatedMasked[digitsEnd])) {
            ++digitsEnd;
        }
        if (digitsEnd == digitsStart ||
            digitsEnd + kSuffix.size() > translatedMasked.size() ||
            translatedMasked.substr(digitsEnd, kSuffix.size()) != kSuffix) {
            if (reasonOut) *reasonOut = "malformed_sentinel";
            return false;
        }

        size_t idx = 0;
        for (size_t k = digitsStart; k < digitsEnd; ++k) {
            idx = idx * 10 + static_cast<size_t>(translatedMasked[k] - '0');
            if (idx >= tokens.size() && tokens.empty()) {
                if (reasonOut) *reasonOut = "unexpected_sentinel_for_untagged_source";
                return false;
            }
        }
        if (idx >= tokens.size()) {
            if (reasonOut) *reasonOut = "sentinel_index_out_of_range";
            return false;
        }
        if (++counts[idx] > 1) {
            if (reasonOut) *reasonOut = "duplicate_sentinel_in_translation";
            return false;
        }

        outRestored.append(tokens[idx].token);
        pos = digitsEnd + kSuffix.size();
    }

    for (size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] != 1) {
            if (reasonOut) *reasonOut = "missing_sentinel_in_translation";
            return false;
        }
    }
    return true;
}

TagValidationResult ValidateTagPreservation(std::string_view source, std::string_view candidate) {
    TagValidationResult res;
    res.sourceTags = ExtractProtectedTags(source);
    res.candidateTags = ExtractProtectedTags(candidate);

    if (res.sourceTags.empty() && res.candidateTags.empty()) {
        res.valid = true;
        return res;
    }

    auto sortedSrc = res.sourceTags;
    auto sortedDst = res.candidateTags;
    std::sort(sortedSrc.begin(), sortedSrc.end());
    std::sort(sortedDst.begin(), sortedDst.end());

    if (sortedSrc == sortedDst) {
        res.valid = true;
        return res;
    }

    res.valid = false;
    std::ostringstream oss;
    oss << "tag_multiset_mismatch(src=" << sortedSrc.size() << ",dst=" << sortedDst.size() << ")";
    res.reason = oss.str();
    return res;
}

const char* TagKindName(TagKind kind) {
    switch (kind) {
        case TagKind::Literal: return "TAG_LITERAL";
        case TagKind::Parameter: return "TAG_PARAMETER";
        case TagKind::Style: return "TAG_STYLE";
        case TagKind::EngineControl: return "TAG_ENGINE_CONTROL";
        default: return "UNKNOWN_TAG";
    }
}

}  // namespace aigiskr::core
