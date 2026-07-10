#include "RuleStore.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace nppqr {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxTriggerBytes = 512;
constexpr std::size_t kMaxReplacementBytes = 100'000;

std::string readUtf8File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open the replacements file.");
    }

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

Activation parseActivation(const Json& value) {
    Activation result = Activation::none;
    if (!value.is_array()) {
        return result;
    }

    for (const auto& entry : value) {
        if (!entry.is_string()) {
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
        }
    }
    return result;
}

std::vector<std::string> parseExtensions(const Json& item) {
    std::vector<std::string> result;
    const auto iterator = item.find("fileExtensions");
    if (iterator == item.end() || !iterator->is_array()) {
        return result;
    }

    for (const auto& entry : *iterator) {
        if (!entry.is_string()) {
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
        const Json root = Json::parse(jsonText.begin(), jsonText.end());
        if (!root.is_object()) {
            throw std::runtime_error("The replacements root must be a JSON object.");
        }

        const auto itemsIterator = root.find("items");
        if (itemsIterator == root.end() || !itemsIterator->is_array()) {
            throw std::runtime_error("The replacements file must contain an items array.");
        }

        std::unordered_map<std::string, bool> groupStates;
        const auto groupsIterator = root.find("groups");
        if (groupsIterator != root.end() && groupsIterator->is_array()) {
            for (const auto& group : *groupsIterator) {
                if (!group.is_object()) {
                    continue;
                }
                const std::string id = group.value("id", "");
                const std::string name = group.value("name", "");
                const bool enabled = group.value("enabled", true);
                if (!id.empty()) {
                    groupStates[id] = enabled;
                }
                if (!name.empty()) {
                    groupStates[name] = enabled;
                }
            }
        }

        std::vector<ReplacementRule> parsedRules;
        parsedRules.reserve(itemsIterator->size());
        std::unordered_map<std::string, std::vector<std::size_t>> duplicateIndex;

        std::size_t itemNumber = 0;
        for (const auto& item : *itemsIterator) {
            ++itemNumber;
            if (!item.is_object()) {
                throw std::runtime_error("Replacement item " + std::to_string(itemNumber) + " must be an object.");
            }

            ReplacementRule rule;
            rule.id = item.value("id", "");
            rule.enabled = item.value("enabled", true);
            rule.trigger = item.value("trigger", "");
            rule.replacement = item.value("replacement", "");
            rule.group = item.value("group", "");
            rule.caseSensitive = item.value("caseSensitive", false);
            rule.wholeWord = item.value("matchMode", "wholeWord") == "wholeWord";
            rule.fileExtensions = parseExtensions(item);

            const auto activationIterator = item.find("activation");
            rule.activation = activationIterator == item.end()
                ? Activation::space | Activation::enter | Activation::tab
                : parseActivation(*activationIterator);

            if (!rule.group.empty()) {
                const auto groupState = groupStates.find(rule.group);
                rule.groupEnabled = groupState == groupStates.end() || groupState->second;
            }

            if (rule.trigger.empty()) {
                throw std::runtime_error("Replacement item " + std::to_string(itemNumber) + " has an empty trigger.");
            }
            if (hasOuterWhitespace(rule.trigger)) {
                throw std::runtime_error("Trigger '" + rule.trigger + "' has leading or trailing whitespace.");
            }
            if (rule.trigger.size() > kMaxTriggerBytes) {
                throw std::runtime_error("Trigger '" + rule.trigger + "' exceeds 512 UTF-8 bytes.");
            }
            if (rule.replacement.empty()) {
                throw std::runtime_error("Trigger '" + rule.trigger + "' has an empty replacement.");
            }
            if (rule.replacement.size() > kMaxReplacementBytes) {
                throw std::runtime_error("Replacement for '" + rule.trigger + "' exceeds 100,000 UTF-8 bytes.");
            }
            if (!rule.wholeWord) {
                result.warnings.push_back(
                    "Trigger '" + rule.trigger + "' requests a match mode not implemented in this alpha; whole-word matching is used.");
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

        std::unordered_map<std::string, std::size_t> exactIndex;
        std::unordered_map<std::string, std::size_t> foldedIndex;
        for (std::size_t index = 0; index < parsedRules.size(); ++index) {
            const ReplacementRule& rule = parsedRules[index];
            if (rule.caseSensitive) {
                exactIndex.emplace(rule.trigger, index);
            } else {
                foldedIndex.emplace(foldAscii(rule.trigger), index);
            }
        }

        rules_ = std::move(parsedRules);
        exactIndex_ = std::move(exactIndex);
        foldedIndex_ = std::move(foldedIndex);
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
    std::string_view currentExtension) const {
    return findIndexed(trigger, activation, currentExtension, false);
}

const ReplacementRule* RuleStore::findManual(
    std::string_view trigger,
    std::string_view currentExtension) const {
    return findIndexed(trigger, Activation::none, currentExtension, true);
}

const ReplacementRule* RuleStore::findIndexed(
    std::string_view trigger,
    Activation activation,
    std::string_view currentExtension,
    bool manual) const {
    const auto isEligible = [&](const ReplacementRule& rule) {
        return rule.enabled && rule.groupEnabled && extensionMatches(rule, currentExtension) &&
               (manual || contains(rule.activation, activation));
    };

    const auto exact = exactIndex_.find(std::string(trigger));
    if (exact != exactIndex_.end()) {
        const ReplacementRule& rule = rules_[exact->second];
        if (isEligible(rule)) {
            return &rule;
        }
    }

    const auto folded = foldedIndex_.find(foldAscii(trigger));
    if (folded != foldedIndex_.end()) {
        const ReplacementRule& rule = rules_[folded->second];
        if (isEligible(rule)) {
            return &rule;
        }
    }
    return nullptr;
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

bool RuleStore::extensionMatches(
    const ReplacementRule& rule,
    std::string_view currentExtension) {
    if (rule.fileExtensions.empty()) {
        return true;
    }
    if (currentExtension.empty()) {
        return false;
    }

    const std::string normalized = foldAscii(currentExtension);
    return std::binary_search(
        rule.fileExtensions.begin(), rule.fileExtensions.end(), normalized);
}

} // namespace nppqr

