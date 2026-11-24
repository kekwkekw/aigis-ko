#include "core/template_matcher.hpp"

#include "core/mini_json.hpp"
#include "core/tag_parser.hpp"

#include <cctype>

namespace aigiskr::core {

namespace {

bool IsDigitChar(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool ValidateSlotValue(SlotType st, std::string_view val) noexcept {
    if (val.empty()) {
        return false;
    }
    if (st == SlotType::Int) {
        for (char c : val) {
            if (!IsDigitChar(c)) {
                return false;
            }
        }
        return true;
    }
    if (st == SlotType::Num) {
        bool hasDigit = false;
        for (char c : val) {
            if (IsDigitChar(c)) {
                hasDigit = true;
            } else if (c != ',' && c != '.' && c != '+' && c != '-') {
                return false;
            }
        }
        return hasDigit;
    }
    // SlotType::Text: non-empty single-line or bounded segment
    return val.find('\n') == std::string_view::npos && val.find('\r') == std::string_view::npos;
}

bool CompileTemplatePattern(std::string_view id,
                            std::string_view sourcePattern,
                            std::string_view targetTemplate,
                            ParsedTemplateRule& outRule,
                            std::string& err) {
    outRule = ParsedTemplateRule{};
    outRule.id = std::string(id);
    outRule.targetTemplate = NormalizeLineEndings(targetTemplate);

    const std::string normSrc = NormalizeLineEndings(sourcePattern);
    size_t pos = 0;
    while (pos < normSrc.size()) {
        const size_t open = normSrc.find('{', pos);
        if (open == std::string::npos) {
            outRule.literals.emplace_back(normSrc.substr(pos));
            break;
        }
        outRule.literals.emplace_back(normSrc.substr(pos, open - pos));
        const size_t close = normSrc.find('}', open + 1);
        if (close == std::string::npos) {
            err = "unclosed '{' in template: " + std::string(id);
            return false;
        }
        const std::string_view spec(normSrc.data() + open + 1, close - open - 1);
        const size_t colon = spec.find(':');
        if (colon == std::string_view::npos) {
            err = "expected '{type:name}' slot in template: " + std::string(id);
            return false;
        }
        const std::string_view typeStr = spec.substr(0, colon);
        const std::string_view nameStr = spec.substr(colon + 1);
        SlotType st = SlotType::Text;
        if (typeStr == "int") {
            st = SlotType::Int;
        } else if (typeStr == "num") {
            st = SlotType::Num;
        } else if (typeStr == "text") {
            st = SlotType::Text;
        } else {
            err = "unknown slot type '" + std::string(typeStr) + "' in template: " + std::string(id);
            return false;
        }
        outRule.slots.emplace_back(std::string(nameStr), st);
        pos = close + 1;
        if (pos == normSrc.size()) {
            outRule.literals.emplace_back("");
        }
    }
    if (outRule.slots.empty() || outRule.literals.size() != outRule.slots.size() + 1) {
        err = "invalid slot/literal count in template: " + std::string(id);
        return false;
    }
    return true;
}

std::string ApplySlotValues(std::string_view tpl,
                            const std::vector<std::pair<std::string, std::string_view>>& captures) {
    std::string out;
    out.reserve(tpl.size() + 32);
    size_t pos = 0;
    while (pos < tpl.size()) {
        const size_t open = tpl.find('{', pos);
        if (open == std::string_view::npos) {
            out.append(tpl.substr(pos));
            break;
        }
        out.append(tpl.substr(pos, open - pos));
        const size_t close = tpl.find('}', open + 1);
        if (close == std::string_view::npos) {
            out.append(tpl.substr(open));
            break;
        }
        const std::string_view name = tpl.substr(open + 1, close - open - 1);
        bool replaced = false;
        for (const auto& kv : captures) {
            if (kv.first == name) {
                out.append(kv.second);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            out.append(tpl.substr(open, close - open + 1));
        }
        pos = close + 1;
    }
    return out;
}

std::string ExpandBackrefs(std::string_view replacement, const std::smatch& match) {
    std::string out;
    out.reserve(replacement.size() + 32);
    for (size_t i = 0; i < replacement.size(); ++i) {
        if (replacement[i] == '\\' && i + 1 < replacement.size() && IsDigitChar(replacement[i + 1])) {
            const size_t idx = static_cast<size_t>(replacement[i + 1] - '0');
            if (idx < match.size()) {
                out.append(match[idx].str());
            }
            ++i;
        } else {
            out.push_back(replacement[i]);
        }
    }
    return out;
}

}  // namespace

void TemplateMatcher::Clear() {
    templates_.clear();
    regexRules_.clear();
}

bool TemplateMatcher::LoadFromJsonFile(const std::string& path, std::string& errorMsg) {
    JsonValue root;
    if (!ParseJsonFile(path, root, errorMsg)) {
        return false;
    }
    return LoadFromJsonValue(root, errorMsg);
}

bool TemplateMatcher::LoadFromJsonBytes(std::string_view jsonText, std::string& errorMsg) {
    JsonValue root;
    if (!ParseJson(jsonText, root, errorMsg)) {
        return false;
    }
    return LoadFromJsonValue(root, errorMsg);
}

bool TemplateMatcher::LoadFromJsonValue(const JsonValue& root, std::string& errorMsg, bool append) {
    if (root.type != JsonValue::Type::Object) {
        errorMsg = "templates root must be JSON object";
        return false;
    }
    std::vector<ParsedTemplateRule> loadedTemplates = append ? templates_ : std::vector<ParsedTemplateRule>{};
    std::vector<WhitelistedRegexRule> loadedRegex = append ? regexRules_ : std::vector<WhitelistedRegexRule>{};

    if (const JsonValue* arr = root.Find("templates")) {
        if (arr->type != JsonValue::Type::Array) {
            errorMsg = "'templates' must be an array";
            return false;
        }
        for (const JsonValue& item : arr->arrVal) {
            const JsonValue* idV = item.Find("id");
            const JsonValue* srcV = item.Find("source_pattern");
            const JsonValue* dstV = item.Find("target_template");
            if (!idV || !srcV || !dstV ||
                idV->type != JsonValue::Type::String ||
                srcV->type != JsonValue::Type::String ||
                dstV->type != JsonValue::Type::String) {
                errorMsg = "malformed entry in 'templates'";
                return false;
            }
            ParsedTemplateRule rule;
            if (!CompileTemplatePattern(idV->strVal, srcV->strVal, dstV->strVal, rule, errorMsg)) {
                return false;
            }
            loadedTemplates.push_back(std::move(rule));
        }
    }

    if (const JsonValue* rarr = root.Find("regex_rules")) {
        if (rarr->type != JsonValue::Type::Array) {
            errorMsg = "'regex_rules' must be an array";
            return false;
        }
        for (const JsonValue& item : rarr->arrVal) {
            const JsonValue* idV = item.Find("id");
            const JsonValue* patV = item.Find("pattern");
            const JsonValue* repV = item.Find("replacement");
            if (!idV || !patV || !repV) {
                errorMsg = "malformed entry in 'regex_rules'";
                return false;
            }
            WhitelistedRegexRule r;
            r.id = idV->strVal;
            if (const JsonValue* ac = item.Find("anchor_contains")) {
                r.anchorContains = NormalizeLineEndings(ac->AsString());
            }
            if (const JsonValue* ap = item.Find("anchor_prefix")) {
                r.anchorPrefix = NormalizeLineEndings(ap->AsString());
            }
            if (r.anchorContains.empty() && r.anchorPrefix.empty()) {
                errorMsg = "whitelisted regex rule requires anchor_contains or anchor_prefix: " + r.id;
                return false;
            }
            try {
                r.compiled = std::regex(NormalizeLineEndings(patV->AsString()), std::regex::ECMAScript);
            } catch (const std::regex_error& e) {
                errorMsg = "regex compile error in " + r.id + ": " + e.what();
                return false;
            }
            r.replacement = NormalizeLineEndings(repV->AsString());
            loadedRegex.push_back(std::move(r));
        }
    }

    templates_ = std::move(loadedTemplates);
    regexRules_ = std::move(loadedRegex);
    return true;
}

bool TemplateMatcher::MatchTemplate(std::string_view normalizedSource,
                                    std::string& outTranslated,
                                    std::string& outRuleId) const {
    for (const ParsedTemplateRule& rule : templates_) {
        const std::string& firstLit = rule.literals.front();
        if (normalizedSource.size() < firstLit.size() ||
            normalizedSource.substr(0, firstLit.size()) != firstLit) {
            continue;
        }
        size_t cur = firstLit.size();
        bool matched = true;
        std::vector<std::pair<std::string, std::string_view>> captures;
        captures.reserve(rule.slots.size());

        for (size_t i = 0; i < rule.slots.size(); ++i) {
            const std::string& nextLit = rule.literals[i + 1];
            std::string_view val;
            if (nextLit.empty()) {
                val = normalizedSource.substr(cur);
                cur = normalizedSource.size();
            } else {
                const size_t found = normalizedSource.find(nextLit, cur);
                if (found == std::string_view::npos) {
                    matched = false;
                    break;
                }
                val = normalizedSource.substr(cur, found - cur);
                cur = found + nextLit.size();
            }
            if (!ValidateSlotValue(rule.slots[i].second, val)) {
                matched = false;
                break;
            }
            captures.emplace_back(rule.slots[i].first, val);
        }

        if (matched && cur == normalizedSource.size()) {
            outTranslated = ApplySlotValues(rule.targetTemplate, captures);
            outRuleId = rule.id;
            return true;
        }
    }
    return false;
}

bool TemplateMatcher::MatchRegex(std::string_view normalizedSource,
                                 std::string& outTranslated,
                                 std::string& outRuleId) const {
    if (regexRules_.empty() || normalizedSource.size() > kMaxRegexInputBytes) {
        return false;
    }
    for (const WhitelistedRegexRule& rule : regexRules_) {
        if (!rule.anchorPrefix.empty() &&
            (normalizedSource.size() < rule.anchorPrefix.size() ||
             normalizedSource.substr(0, rule.anchorPrefix.size()) != rule.anchorPrefix)) {
            continue;
        }
        if (!rule.anchorContains.empty() &&
            normalizedSource.find(rule.anchorContains) == std::string_view::npos) {
            continue;
        }
        const std::string srcStr(normalizedSource);
        std::smatch match;
        if (std::regex_match(srcStr, match, rule.compiled)) {
            outTranslated = ExpandBackrefs(rule.replacement, match);
            outRuleId = rule.id;
            return true;
        }
    }
    return false;
}

}  // namespace aigiskr::core
