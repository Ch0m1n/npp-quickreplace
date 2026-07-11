#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "CapturePattern.h"

namespace nppqr {

enum class Activation : std::uint32_t {
    none = 0,
    space = 1U << 0U,
    enter = 1U << 1U,
    tab = 1U << 2U,
    punctuation = 1U << 3U,
    immediate = 1U << 4U,
};

constexpr Activation operator|(Activation left, Activation right) noexcept {
    return static_cast<Activation>(
        static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr bool contains(Activation value, Activation flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0U;
}

struct ReplacementRule {
    std::string id;
    bool enabled = true;
    bool groupEnabled = true;
    std::string trigger;
    std::string replacement;
    std::string group;
    bool caseSensitive = false;
    bool wholeWord = true;
    bool captureTemplate = false;
    CapturePattern compiledCaptureTemplate;
    Activation activation = Activation::space | Activation::enter | Activation::tab;
    std::vector<std::string> fileExtensions;
    std::vector<std::string> pathGlobs;
    std::vector<std::string> languages;
};

struct RuleLoadResult {
    bool ok = false;
    std::size_t loadedCount = 0;
    std::string error;
    std::vector<std::string> warnings;
};

class RuleStore {
public:
    RuleLoadResult loadFromFile(const std::filesystem::path& path);
    RuleLoadResult loadFromText(std::string_view jsonText);

    [[nodiscard]] const ReplacementRule* find(
        std::string_view trigger,
        Activation activation,
        std::string_view currentExtension,
        std::string_view currentPath = {},
        std::string_view currentLanguage = {}) const;

    [[nodiscard]] const ReplacementRule* findImmediate(
        std::string_view trigger,
        std::string_view currentExtension,
        std::string_view currentPath = {},
        std::string_view currentLanguage = {}) const;

    [[nodiscard]] const ReplacementRule* findManual(
        std::string_view trigger,
        std::string_view currentExtension,
        std::string_view currentPath = {},
        std::string_view currentLanguage = {}) const;

    [[nodiscard]] const ReplacementRule* findCaptureTemplate(
        std::string_view trigger,
        Activation activation,
        std::string_view currentExtension,
        std::string_view currentPath,
        std::string_view currentLanguage,
        CaptureMatch& captures,
        bool manual = false) const;
    [[nodiscard]] std::size_t size() const noexcept { return rules_.size(); }
    [[nodiscard]] const std::vector<ReplacementRule>& rules() const noexcept { return rules_; }
    [[nodiscard]] bool hasImmediateRules() const noexcept { return hasImmediateRules_; }

    [[nodiscard]] static Activation activationForCharacter(
        int character,
        std::string_view punctuationCharacters) noexcept;

    [[nodiscard]] static std::string foldAscii(std::string_view value);

private:
    struct TransparentStringHash {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
        [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
            return operator()(std::string_view(value));
        }
    };

    [[nodiscard]] const ReplacementRule* findIndexed(
        std::string_view trigger,
        Activation activation,
        std::string_view currentExtension,
        std::string_view currentPath,
        std::string_view currentLanguage,
        bool manual) const;

    [[nodiscard]] static bool filtersMatch(
        const ReplacementRule& rule,
        std::string_view currentExtension,
        std::string_view currentPath,
        std::string_view currentLanguage);

    using RuleIndex = std::unordered_map<
        std::string,
        std::size_t,
        TransparentStringHash,
        std::equal_to<>>;

    std::vector<ReplacementRule> rules_;
    RuleIndex exactIndex_;
    RuleIndex foldedIndex_;
    std::vector<std::size_t> captureTemplateIndices_;
    bool hasImmediateRules_ = false;
};

} // namespace nppqr

