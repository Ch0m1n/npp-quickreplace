#include "RuleStore.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace nppqr {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxRuleFileBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaxRuleCount = 100'000;
constexpr std::size_t kMaxTriggerBytes = 512;
constexpr std::size_t kMaxReplacementBytes = 100'000;
constexpr std::size_t kMaxAggregateTextBytes = 128U * 1024U * 1024U;

std::string normalizeUtf8(std::string_view value) {
    if (value.empty()) return {};
    if (std::all_of(value.begin(), value.end(), [](unsigned char character) { return character < 0x80U; })) {
        return std::string(value);
    }
    const int wideLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (wideLength <= 0) return std::string(value);
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), wide.data(), wideLength);
    const int normalizedLength = ::NormalizeString(NormalizationC, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0);
    if (normalizedLength <= 0) return std::string(value);
    std::wstring normalized(static_cast<std::size_t>(normalizedLength), L'\0');
    const int normalizedCount = ::NormalizeString(NormalizationC, wide.data(),
        static_cast<int>(wide.size()), normalized.data(), normalizedLength);
    if (normalizedCount <= 0) return std::string(value);
    normalized.resize(static_cast<std::size_t>(normalizedCount));
    const int utf8Length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        normalized.data(), static_cast<int>(normalized.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) return std::string(value);
    std::string result(static_cast<std::size_t>(utf8Length), '\0');
    ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, normalized.data(),
        static_cast<int>(normalized.size()), result.data(), utf8Length, nullptr, nullptr);
    return result;
}

std::string readUtf8File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Unable to open the replacements file.");
    }
    const std::streampos size = input.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > kMaxRuleFileBytes) {
        throw std::runtime_error("The replacements file exceeds the 64 MiB safety limit.");
    }
    input.seekg(0, std::ios::beg);

    std::ostringstream stream;
    stream << input.rdbuf();
    std::string content = stream.str();
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEFU &&
        static_cast<unsigned char>(content[1]) == 0xBBU &&
        static_cast<unsigned char>(content[2]) == 0xBFU) {
        content.erase(0, 3);
    }
    return content;
}

bool hasOuterWhitespace(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    return isSpace(static_cast<unsigned char>(value.front())) ||
           isSpace(static_cast<unsigned char>(value.back()));
}

Activation parseActivation(
    const Json& value,
    std::string_view trigger,
    std::vector<std::string>& warnings) {
    Activation result = Activation::none;
    if (!value.is_array()) {
        warnings.push_back("Trigger '" + std::string(trigger) + "' has a non-array activation value.");
        return result;
    }

    for (const auto& entry : value) {
        if (!entry.is_string()) {
            warnings.push_back("Trigger '" + std::string(trigger) + "' has a non-text activation entry.");
            continue;
        }
        const std::string name = RuleStore::foldAscii(entry.get<std::string>());
        if (name == "space") {
            result = result | Activation::space;
        } else if (name == "enter") {
            result = result | Activation::enter;
        } else if (name == "tab") {
            result = result | Activation::tab;
        } else if (name == "punctuation") {
            result = result | Activation::punctuation;
        } else if (name == "immediate") {
            result = result | Activation::immediate;
        } else {
            warnings.push_back(
                "Trigger '" + std::string(trigger) + "' contains unsupported activation '" + name + "'.");
        }
    }
    return result;
}

std::vector<std::string> parseExtensions(
    const Json& item,
    std::string_view trigger,
    std::vector<std::string>& warnings) {
    std::vector<std::string> result;
    const auto iterator = item.find("fileExtensions");
    if (iterator == item.end()) {
        return result;
    }
    if (!iterator->is_array()) {
        warnings.push_back("Trigger '" + std::string(trigger) + "' has non-array fileExtensions.");
        return result;
    }

    for (const auto& entry : *iterator) {
        if (!entry.is_string()) {
            warnings.push_back("Trigger '" + std::string(trigger) + "' has a non-text file extension.");
            continue;
        }
        std::string extension = RuleStore::foldAscii(entry.get<std::string>());
        if (!extension.empty() && extension.front() != '.') {
            extension.insert(extension.begin(), '.');
        }
        if (!extension.empty()) {
            result.push_back(std::move(extension));
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::string> parseFilterList(
    const Json& item,
    std::string_view field,
    std::string_view trigger,
    std::vector<std::string>& warnings) {
    std::vector<std::string> result;
    const auto iterator = item.find(std::string(field));
    if (iterator == item.end()) return result;
    if (!iterator->is_array()) {
        warnings.push_back("Trigger '" + std::string(trigger) + "' has non-array " +
            std::string(field) + ".");
        return result;
    }
    if (iterator->size() > 256) {
        throw std::runtime_error("Trigger '" + std::string(trigger) + "' has too many " +
            std::string(field) + " entries.");
    }
    for (const auto& entry : *iterator) {
        if (!entry.is_string()) {
            warnings.push_back("Trigger '" + std::string(trigger) + "' has a non-text " +
                std::string(field) + " entry.");
            continue;
        }
        std::string value = RuleStore::foldAscii(entry.get<std::string>());
        std::replace(value.begin(), value.end(), '\\', '/');
        if (value.size() > 1024) {
            throw std::runtime_error("A " + std::string(field) + " entry for trigger '" +
                std::string(trigger) + "' exceeds 1024 bytes.");
        }
        if (!value.empty()) result.push_back(std::move(value));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool globMatches(std::string_view pattern, std::string_view value) {
    std::size_t patternIndex = 0;
    std::size_t valueIndex = 0;
    std::size_t star = std::string_view::npos;
    std::size_t retry = 0;
    while (valueIndex < value.size()) {
        if (patternIndex < pattern.size() &&
            (pattern[patternIndex] == '?' || pattern[patternIndex] == value[valueIndex])) {
            ++patternIndex;
            ++valueIndex;
        } else if (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
            star = patternIndex++;
            retry = valueIndex;
        } else if (star != std::string_view::npos) {
            patternIndex = star + 1;
            valueIndex = ++retry;
        } else {
            return false;
        }
    }
    while (patternIndex < pattern.size() && pattern[patternIndex] == '*') ++patternIndex;
    return patternIndex == pattern.size();
}
bool startsWithForRule(const ReplacementRule& prefix, const ReplacementRule& candidate) {
    if (prefix.trigger.size() >= candidate.trigger.size()) {
        return false;
    }
    if (prefix.caseSensitive) {
        return candidate.trigger.starts_with(prefix.trigger);
    }
    return RuleStore::foldAscii(candidate.trigger).starts_with(RuleStore::foldAscii(prefix.trigger));
}

} // namespace

RuleLoadResult RuleStore::loadFromFile(const std::filesystem::path& path) {
    try {
        return loadFromText(readUtf8File(path));
    } catch (const std::exception& error) {
        return {.ok = false, .error = error.what()};
    }
}

RuleLoadResult RuleStore::loadFromText(std::string_view jsonText) {
    RuleLoadResult result;

    try {
        if (jsonText.size() > kMaxRuleFileBytes) {
            throw std::runtime_error("The replacements document exceeds the 64 MiB safety limit.");
        }
        const Json root = Json::parse(jsonText.begin(), jsonText.end());
        if (!root.is_object()) {
            throw std::runtime_error("The replacements root must be a JSON object.");
        }

        const auto version = root.find("version");
        if (version == root.end()) {
            result.warnings.push_back("The replacements document has no version; version 1 is assumed.");
        } else if (!version->is_number_integer() || version->get<int>() != 1) {
            throw std::runtime_error("Only replacements schema version 1 is supported.");
        }

        const auto itemsIterator = root.find("items");
        if (itemsIterator == root.end() || !itemsIterator->is_array()) {
            throw std::runtime_error("The replacements file must contain an items array.");
        }
        if (itemsIterator->size() > kMaxRuleCount) {
            throw std::runtime_error("The replacements file exceeds the 100,000 rule safety limit.");
        }

        std::unordered_map<std::string, bool> groupStates;
        std::unordered_set<std::string> groupIds;
        const auto groupsIterator = root.find("groups");
        if (groupsIterator != root.end()) {
            if (!groupsIterator->is_array()) {
                throw std::runtime_error("groups must be an array when present.");
            }
            for (const auto& group : *groupsIterator) {
                if (!group.is_object()) {
                    throw std::runtime_error("Every group must be an object.");
                }
                const std::string id = group.value("id", "");
                const std::string name = group.value("name", "");
                const bool enabled = group.value("enabled", true);
                if (id.empty()) {
                    throw std::runtime_error("Every group must have a non-empty id.");
                }
                if (!groupIds.insert(id).second) {
                    throw std::runtime_error("Duplicate group id: '" + id + "'.");
                }
                groupStates[id] = enabled;
                if (!name.empty()) {
                    if (groupStates.contains(name) && name != id) {
                        result.warnings.push_back("Group name '" + name + "' is used more than once.");
                    }
                    groupStates[name] = enabled;
                }
            }
        }

        std::vector<ReplacementRule> parsedRules;
        parsedRules.reserve(itemsIterator->size());
        std::unordered_map<std::string, std::vector<std::size_t>> duplicateIndex;
        std::unordered_set<std::string> ruleIds;
        std::size_t aggregateTextBytes = 0;

        std::size_t itemNumber = 0;
        for (const auto& item : *itemsIterator) {
            ++itemNumber;
            if (!item.is_object()) {
                throw std::runtime_error(
                    "Replacement item " + std::to_string(itemNumber) + " must be an object.");
            }

            ReplacementRule rule;
            rule.id = item.value("id", "");
            rule.enabled = item.value("enabled", true);
            rule.trigger = normalizeUtf8(item.value("trigger", ""));
            rule.replacement = item.value("replacement", "");
            rule.group = item.value("group", "");
            rule.caseSensitive = item.value("caseSensitive", false);

            const std::string matchMode = item.value("matchMode", "wholeWord");
            rule.captureTemplate = matchMode == "captureTemplate";
            rule.wholeWord = matchMode == "wholeWord";
            rule.fileExtensions = parseExtensions(item, rule.trigger, result.warnings);
            rule.pathGlobs = parseFilterList(item, "pathGlobs", rule.trigger, result.warnings);
            rule.languages = parseFilterList(item, "languages", rule.trigger, result.warnings);

            const auto activationIterator = item.find("activation");
            rule.activation = activationIterator == item.end()
                ? Activation::space | Activation::enter | Activation::tab
                : parseActivation(*activationIterator, rule.trigger, result.warnings);

            if (!rule.id.empty() && !ruleIds.insert(rule.id).second) {
                throw std::runtime_error("Duplicate replacement id: '" + rule.id + "'.");
            }
            if (!rule.group.empty()) {
                const auto groupState = groupStates.find(rule.group);
                if (groupState == groupStates.end()) {
                    result.warnings.push_back(
                        "Trigger '" + rule.trigger + "' references unknown group '" + rule.group + "'.");
                } else {
                    rule.groupEnabled = groupState->second;
                }
            }

            if (rule.trigger.empty()) {
                throw std::runtime_error(
                    "Replacement item " + std::to_string(itemNumber) + " has an empty trigger.");
            }
            if (hasOuterWhitespace(rule.trigger)) {
                throw std::runtime_error(
                    "Trigger '" + rule.trigger + "' has leading or trailing whitespace.");
            }
            if (rule.trigger.size() > kMaxTriggerBytes) {
                throw std::runtime_error("Trigger '" + rule.trigger + "' exceeds 512 UTF-8 bytes.");
            }
            if (rule.replacement.empty()) {
                throw std::runtime_error("Trigger '" + rule.trigger + "' has an empty replacement.");
            }
            if (rule.replacement.size() > kMaxReplacementBytes) {
                throw std::runtime_error(
                    "Replacement for '" + rule.trigger + "' exceeds 100,000 UTF-8 bytes.");
            }
            aggregateTextBytes += rule.trigger.size() + rule.replacement.size();
            if (aggregateTextBytes > kMaxAggregateTextBytes) {
                throw std::runtime_error(
                    "The aggregate trigger and replacement text exceeds the 128 MiB safety limit.");
            }
            if (rule.captureTemplate) {
                const auto compiled = rule.compiledCaptureTemplate.compile(
                    rule.trigger, rule.caseSensitive);
                if (!compiled.ok) {
                    throw std::runtime_error("Capture template '" + rule.trigger +
                        "' is invalid: " + compiled.error);
                }
                const auto replacementCaptures =
                    rule.compiledCaptureTemplate.validateReplacement(rule.replacement);
                if (!replacementCaptures.ok) {
                    throw std::runtime_error("Replacement for capture template '" + rule.trigger +
                        "' is invalid: " + replacementCaptures.error);
                }
                if (contains(rule.activation, Activation::immediate)) {
                    throw std::runtime_error("Capture template '" + rule.trigger +
                        "' cannot use immediate activation.");
                }
            } else if (!rule.wholeWord) {
                result.warnings.push_back(
                    "Trigger '" + rule.trigger + "' requests unsupported matchMode '" + matchMode +
                    "'; whole-word matching is used.");
                rule.wholeWord = true;
            }
            if (rule.activation == Activation::none) {
                throw std::runtime_error("Trigger '" + rule.trigger + "' has no supported activation mode.");
            }

            const std::string folded = foldAscii(rule.trigger);
            const auto duplicateCandidates = duplicateIndex.find(folded);
            if (duplicateCandidates != duplicateIndex.end()) {
                for (const std::size_t existingIndex : duplicateCandidates->second) {
                    const ReplacementRule& existing = parsedRules[existingIndex];
                    if (existing.trigger == rule.trigger || !existing.caseSensitive || !rule.caseSensitive) {
                        throw std::runtime_error(
                            "Duplicate or case-conflicting trigger: '" + rule.trigger + "'.");
                    }
                }
            }

            duplicateIndex[folded].push_back(parsedRules.size());
            parsedRules.push_back(std::move(rule));
        }

        std::vector<std::pair<std::string, std::size_t>> prefixOrder;
        prefixOrder.reserve(parsedRules.size());
        for (std::size_t index = 0; index < parsedRules.size(); ++index) {
            if (!parsedRules[index].captureTemplate) {
                prefixOrder.emplace_back(foldAscii(parsedRules[index].trigger), index);
            }
        }
        std::sort(prefixOrder.begin(), prefixOrder.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        for (std::size_t position = 0; position < prefixOrder.size(); ++position) {
            const auto& [foldedPrefix, prefixIndex] = prefixOrder[position];
            const ReplacementRule& prefix = parsedRules[prefixIndex];
            bool warned = false;
            for (std::size_t next = position + 1; next < prefixOrder.size(); ++next) {
                const auto& [foldedCandidate, candidateIndex] = prefixOrder[next];
                if (!foldedCandidate.starts_with(foldedPrefix)) {
                    break;
                }
                const ReplacementRule& candidate = parsedRules[candidateIndex];
                if (!startsWithForRule(prefix, candidate)) {
                    continue;
                }
                if (contains(prefix.activation, Activation::immediate)) {
                    throw std::runtime_error(
                        "Immediate trigger '" + prefix.trigger + "' is a prefix of '" +
                        candidate.trigger + "' and would replace it too early.");
                }
                if (!warned) {
                    result.warnings.push_back(
                        "Trigger '" + prefix.trigger + "' is a prefix of another trigger.");
                    warned = true;
                }
            }
        }
        RuleIndex exactIndex;
        RuleIndex foldedIndex;
        std::array<std::vector<std::size_t>, 256> captureTemplateBuckets;
        std::vector<std::size_t> captureTemplateFallback;
        exactIndex.reserve(parsedRules.size());
        foldedIndex.reserve(parsedRules.size());
        for (std::size_t index = 0; index < parsedRules.size(); ++index) {
            const ReplacementRule& rule = parsedRules[index];
            if (rule.captureTemplate) {
                const auto key = rule.compiledCaptureTemplate.leadingByte();
                if (key.has_value()) captureTemplateBuckets[*key].push_back(index);
                else captureTemplateFallback.push_back(index);
            } else if (rule.caseSensitive) {
                exactIndex.emplace(rule.trigger, index);
            } else {
                foldedIndex.emplace(foldAscii(rule.trigger), index);
            }
        }

        const bool hasImmediateRules = std::any_of(parsedRules.begin(), parsedRules.end(), [](const ReplacementRule& rule) {
            return rule.enabled && rule.groupEnabled && contains(rule.activation, Activation::immediate);
        });
        rules_ = std::move(parsedRules);
        exactIndex_ = std::move(exactIndex);
        foldedIndex_ = std::move(foldedIndex);
        captureTemplateBuckets_ = std::move(captureTemplateBuckets);
        captureTemplateFallback_ = std::move(captureTemplateFallback);
        hasImmediateRules_ = hasImmediateRules;
        result.ok = true;
        result.loadedCount = rules_.size();
        return result;
    } catch (const std::exception& error) {
        result.error = error.what();
        return result;
    }
}

const ReplacementRule* RuleStore::find(
    std::string_view trigger,
    Activation activation,
    std::string_view currentExtension,
    std::string_view currentPath,
    std::string_view currentLanguage) const {
    return findIndexed(
        trigger, activation, currentExtension, currentPath, currentLanguage, false);
}

const ReplacementRule* RuleStore::findImmediate(
    std::string_view trigger,
    std::string_view currentExtension,
    std::string_view currentPath,
    std::string_view currentLanguage) const {
    return findIndexed(trigger, Activation::immediate,
        currentExtension, currentPath, currentLanguage, false);
}

const ReplacementRule* RuleStore::findManual(
    std::string_view trigger,
    std::string_view currentExtension,
    std::string_view currentPath,
    std::string_view currentLanguage) const {
    return findIndexed(trigger, Activation::none,
        currentExtension, currentPath, currentLanguage, true);
}

const ReplacementRule* RuleStore::findCaptureTemplate(
    std::string_view trigger,
    Activation activation,
    std::string_view currentExtension,
    std::string_view currentPath,
    std::string_view currentLanguage,
    CaptureMatch& captures,
    bool manual) const {
    const std::vector<std::size_t>* bucket = nullptr;
    if (!trigger.empty()) {
        unsigned char key = static_cast<unsigned char>(trigger.front());
        if (key < 0x80U) key = static_cast<unsigned char>(std::tolower(key));
        bucket = &captureTemplateBuckets_[key];
    }

    std::size_t bucketPosition = 0;
    std::size_t fallbackPosition = 0;
    while ((bucket != nullptr && bucketPosition < bucket->size()) ||
           fallbackPosition < captureTemplateFallback_.size()) {
        const bool useBucket = bucket != nullptr && bucketPosition < bucket->size() &&
            (fallbackPosition >= captureTemplateFallback_.size() ||
             (*bucket)[bucketPosition] < captureTemplateFallback_[fallbackPosition]);
        const std::size_t index = useBucket
            ? (*bucket)[bucketPosition++]
            : captureTemplateFallback_[fallbackPosition++];
        const ReplacementRule& rule = rules_[index];
        if (!rule.enabled || !rule.groupEnabled ||
            !filtersMatch(rule, currentExtension, currentPath, currentLanguage) ||
            (!manual && !contains(rule.activation, activation))) {
            continue;
        }
        const auto matched = rule.compiledCaptureTemplate.match(trigger);
        if (!matched.has_value()) continue;
        captures = *matched;
        return &rule;
    }
    return nullptr;
}
const ReplacementRule* RuleStore::findIndexed(
    std::string_view trigger,
    Activation activation,
    std::string_view currentExtension,
    std::string_view currentPath,
    std::string_view currentLanguage,
    bool manual) const {
    const auto isEligible = [&](const ReplacementRule& rule) {
        return rule.enabled && rule.groupEnabled &&
               filtersMatch(rule, currentExtension, currentPath, currentLanguage) &&
               (manual || contains(rule.activation, activation));
    };

    const bool ascii = std::all_of(trigger.begin(), trigger.end(), [](unsigned char character) {
        return character < 0x80U;
    });
    const std::string normalizedStorage = ascii ? std::string{} : normalizeUtf8(trigger);
    const std::string_view normalizedTrigger = ascii
        ? trigger
        : std::string_view(normalizedStorage);
    const auto exact = exactIndex_.find(normalizedTrigger);
    if (exact != exactIndex_.end()) {
        const ReplacementRule& rule = rules_[exact->second];
        if (isEligible(rule)) return &rule;
    }

    const std::string foldedTrigger = foldAscii(normalizedTrigger);
    const auto folded = foldedIndex_.find(foldedTrigger);
    if (folded != foldedIndex_.end()) {
        const ReplacementRule& rule = rules_[folded->second];
        if (isEligible(rule)) return &rule;
    }
    return nullptr;
}
RuleDiagnosticResult RuleStore::diagnose(
    std::string_view trigger,
    Activation activation,
    std::string_view currentExtension,
    std::string_view currentPath,
    std::string_view currentLanguage,
    bool manual) const {
    RuleDiagnosticResult result;
    const std::string normalizedTrigger = normalizeUtf8(trigger);

    CaptureMatch chosenCaptures;
    const ReplacementRule* chosen = manual
        ? findManual(normalizedTrigger, currentExtension, currentPath, currentLanguage)
        : find(normalizedTrigger, activation, currentExtension, currentPath, currentLanguage);
    if (chosen == nullptr) {
        chosen = findCaptureTemplate(normalizedTrigger, activation,
            currentExtension, currentPath, currentLanguage, chosenCaptures, manual);
        result.captureMatch = chosen != nullptr;
    }
    result.matchedRule = chosen;
    if (result.captureMatch) result.captures = chosenCaptures;

    const auto addBlocker = [&](std::string value) {
        if (std::find(result.blockers.begin(), result.blockers.end(), value) ==
            result.blockers.end()) {
            result.blockers.push_back(std::move(value));
        }
    };

    for (const ReplacementRule& rule : rules_) {
        std::optional<CaptureMatch> captures;
        bool triggerMatches = false;
        if (rule.captureTemplate) {
            captures = rule.compiledCaptureTemplate.match(normalizedTrigger);
            triggerMatches = captures.has_value();
        } else {
            triggerMatches = rule.caseSensitive
                ? rule.trigger == normalizedTrigger
                : foldAscii(rule.trigger) == foldAscii(normalizedTrigger);
        }
        if (!triggerMatches) continue;
        ++result.triggerMatchCount;

        const bool activationMatches = manual || contains(rule.activation, activation);
        const bool extensionMatches = [&] {
            if (rule.fileExtensions.empty()) return true;
            if (currentExtension.empty()) return false;
            return std::binary_search(rule.fileExtensions.begin(), rule.fileExtensions.end(),
                foldAscii(currentExtension));
        }();
        const bool languageMatches = [&] {
            if (rule.languages.empty()) return true;
            if (currentLanguage.empty()) return false;
            return std::binary_search(rule.languages.begin(), rule.languages.end(),
                foldAscii(currentLanguage));
        }();
        const bool pathMatches = [&] {
            if (rule.pathGlobs.empty()) return true;
            if (currentPath.empty()) return false;
            std::string normalizedPath = foldAscii(currentPath);
            std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
            return std::any_of(rule.pathGlobs.begin(), rule.pathGlobs.end(),
                [&](const std::string& pattern) {
                    return globMatches(pattern, normalizedPath);
                });
        }();

        const bool eligible = rule.enabled && rule.groupEnabled && activationMatches &&
            extensionMatches && languageMatches && pathMatches;
        if (eligible) {
            ++result.eligibleMatchCount;
            continue;
        }
        if (result.matchedRule != nullptr || !result.blockers.empty()) continue;
        if (!rule.enabled) addBlocker("rule_disabled");
        if (!rule.groupEnabled) addBlocker("group_disabled");
        if (!activationMatches) addBlocker("activation_mismatch");
        if (!extensionMatches) addBlocker("extension_mismatch");
        if (!pathMatches) addBlocker("path_mismatch");
        if (!languageMatches) addBlocker("language_mismatch");
    }
    if (result.triggerMatchCount == 0) addBlocker("trigger_not_found");
    return result;
}

Activation RuleStore::activationForCharacter(
    int character,
    std::string_view punctuationCharacters) noexcept {
    switch (character) {
    case ' ':
        return Activation::space;
    case '\r':
    case '\n':
        return Activation::enter;
    case '\t':
        return Activation::tab;
    default:
        if (character >= 0 && character <= 0x7F &&
            punctuationCharacters.find(static_cast<char>(character)) != std::string_view::npos) {
            return Activation::punctuation;
        }
        return Activation::none;
    }
}

std::string RuleStore::foldAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return ch < 0x80U ? static_cast<char>(std::tolower(ch)) : static_cast<char>(ch);
    });
    return result;
}

bool RuleStore::filtersMatch(
    const ReplacementRule& rule,
    std::string_view currentExtension,
    std::string_view currentPath,
    std::string_view currentLanguage) {
    if (!rule.fileExtensions.empty()) {
        if (currentExtension.empty()) return false;
        const std::string normalizedExtension = foldAscii(currentExtension);
        if (!std::binary_search(
                rule.fileExtensions.begin(), rule.fileExtensions.end(), normalizedExtension)) {
            return false;
        }
    }
    if (!rule.languages.empty()) {
        if (currentLanguage.empty()) return false;
        const std::string normalizedLanguage = foldAscii(currentLanguage);
        if (!std::binary_search(
                rule.languages.begin(), rule.languages.end(), normalizedLanguage)) {
            return false;
        }
    }
    if (!rule.pathGlobs.empty()) {
        if (currentPath.empty()) return false;
        std::string normalizedPath = foldAscii(currentPath);
        std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
        if (std::none_of(rule.pathGlobs.begin(), rule.pathGlobs.end(),
                [&](const std::string& pattern) { return globMatches(pattern, normalizedPath); })) {
            return false;
        }
    }
    return true;
}
} // namespace nppqr
