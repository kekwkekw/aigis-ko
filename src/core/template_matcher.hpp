#pragma once

#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace aigiskr::core {

struct JsonValue;

enum class SlotType {
    Int,   // [0-9]+
    Num,   // [0-9][0-9,./+-]* (stops neatly at delimiter)
    Text,  // non-empty substring (up to next literal delimiter)
};

struct ParsedTemplateRule {
    std::string id;
    std::vector<std::string> literals;  // size == slots.size() + 1
    std::vector<std::pair<std::string, SlotType>> slots;
    std::string targetTemplate;
};

struct WhitelistedRegexRule {
    std::string id;
    std::string anchorContains;
    std::string anchorPrefix;
    std::regex compiled;
    std::string replacement;
};

class TemplateMatcher {
public:
    bool LoadFromJsonFile(const std::string& path, std::string& errorMsg);
    bool LoadFromJsonBytes(std::string_view jsonText, std::string& errorMsg);
    bool LoadFromJsonValue(const JsonValue& root, std::string& errorMsg, bool append = false);
    void Clear();

    // Level 2: Exact prefix/literal parameterized template matching (zero regex overhead).
    bool MatchTemplate(std::string_view normalizedSource,
                       std::string& outTranslated,
                       std::string& outRuleId) const;

    // Level 3: Anchored bounded whitelisted regex matching.
    bool MatchRegex(std::string_view normalizedSource,
                    std::string& outTranslated,
                    std::string& outRuleId) const;

    size_t TemplateCount() const noexcept { return templates_.size(); }
    size_t RegexCount() const noexcept { return regexRules_.size(); }

private:
    static constexpr size_t kMaxRegexInputBytes = 512;
    std::vector<ParsedTemplateRule> templates_;
    std::vector<WhitelistedRegexRule> regexRules_;
};

}  // namespace aigiskr::core
