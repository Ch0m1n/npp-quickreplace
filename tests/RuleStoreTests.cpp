#include "RuleStore.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

const char* basicRules = R"json(
{
  "version": 1,
  "groups": [
    {"id": "enabled", "enabled": true},
    {"id": "disabled", "enabled": false}
  ],
  "items": [
    {
      "id": "korean",
      "trigger": "ㄱ123",
      "replacement": "대공 방어 사격",
      "group": "enabled",
      "activation": ["space", "enter", "tab", "punctuation"],
      "caseSensitive": false
    },
    {
      "id": "english",
      "trigger": "WoWsAA",
      "replacement": "대공 방어 사격",
      "group": "enabled",
      "activation": ["space"],
      "caseSensitive": false,
      "fileExtensions": ["md", ".txt"]
    },
    {
      "id": "strict",
      "trigger": "CaseOnly",
      "replacement": "strict",
      "group": "enabled",
      "activation": ["space"],
      "caseSensitive": true
    },
    {
      "id": "disabled-group",
      "trigger": "off",
      "replacement": "never",
      "group": "disabled",
      "activation": ["space"],
      "caseSensitive": false
    }
  ]
}
)json";

void testBasicMatching() {
    nppqr::RuleStore store;
    const auto loaded = store.loadFromText(basicRules);
    expect(loaded.ok, "basic JSON loads");
    expect(loaded.loadedCount == 4, "all four rules are indexed");

    const auto* korean = store.find("ㄱ123", nppqr::Activation::space, ".md");
    expect(korean != nullptr, "Korean and numeric trigger matches");
    expect(korean != nullptr && korean->replacement == "대공 방어 사격", "Korean replacement remains UTF-8");

    expect(
        store.find("WOWSAA", nppqr::Activation::space, ".MD") != nullptr,
        "ASCII case-insensitive matching and extension normalization work");
    expect(
        store.find("wowsaa", nppqr::Activation::enter, ".md") == nullptr,
        "activation restrictions are enforced");
    expect(
        store.find("wowsaa", nppqr::Activation::space, ".cpp") == nullptr,
        "file extension restrictions are enforced");

    expect(
        store.find("CaseOnly", nppqr::Activation::space, "") != nullptr,
        "case-sensitive exact form matches");
    expect(
        store.find("caseonly", nppqr::Activation::space, "") == nullptr,
        "case-sensitive different form does not match");
    expect(
        store.find("off", nppqr::Activation::space, "") == nullptr,
        "disabled groups do not match");
    expect(
        store.findManual("wowsaa", ".txt") != nullptr,
        "manual matching ignores automatic activation choice");
}

void testDelimiterClassification() {
    constexpr std::string_view punctuation = ".,!?";
    expect(
        nppqr::RuleStore::activationForCharacter(' ', punctuation) == nppqr::Activation::space,
        "space is classified");
    expect(
        nppqr::RuleStore::activationForCharacter('\n', punctuation) == nppqr::Activation::enter,
        "newline is classified");
    expect(
        nppqr::RuleStore::activationForCharacter('\t', punctuation) == nppqr::Activation::tab,
        "tab is classified");
    expect(
        nppqr::RuleStore::activationForCharacter(',', punctuation) == nppqr::Activation::punctuation,
        "configured punctuation is classified");
    expect(
        nppqr::RuleStore::activationForCharacter('/', punctuation) == nppqr::Activation::none,
        "unconfigured punctuation is ignored");
}

void testValidationAndTransactionalLoad() {
    nppqr::RuleStore store;
    expect(store.loadFromText(basicRules).ok, "baseline load succeeds");

    const char* duplicateRules = R"json(
    {"items": [
      {"trigger": "aa", "replacement": "one", "caseSensitive": false, "activation": ["space"]},
      {"trigger": "AA", "replacement": "two", "caseSensitive": true, "activation": ["space"]}
    ]}
    )json";
    const auto duplicate = store.loadFromText(duplicateRules);
    expect(!duplicate.ok, "case-conflicting duplicate is rejected");
    expect(store.size() == 4, "failed load keeps the previous valid rule set");

    const auto emptyReplacement = store.loadFromText(
        R"json({"items":[{"trigger":"x","replacement":"","activation":["space"]}]})json");
    expect(!emptyReplacement.ok, "empty replacement is rejected");
    expect(store.size() == 4, "another failed load is transactional");
}

void testTenThousandRules() {
    nlohmann::json root;
    root["version"] = 1;
    root["items"] = nlohmann::json::array();
    for (int index = 0; index < 10'000; ++index) {
        root["items"].push_back({
            {"trigger", "trigger" + std::to_string(index)},
            {"replacement", "replacement" + std::to_string(index)},
            {"activation", nlohmann::json::array({"space"})},
            {"caseSensitive", false},
        });
    }

    nppqr::RuleStore store;
    const auto loaded = store.loadFromText(root.dump());
    expect(loaded.ok && store.size() == 10'000, "10,000 rules load");

    const auto started = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < 100'000; ++iteration) {
        const auto* rule = store.find("TRIGGER9876", nppqr::Activation::space, ".md");
        if (rule == nullptr) {
            expect(false, "indexed lookup remains correct under repetition");
            break;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    std::cout << "100,000 indexed lookups: " << elapsed.count() << " ms\n";
}

} // namespace

int main() {
    testBasicMatching();
    testDelimiterClassification();
    testValidationAndTransactionalLoad();
    testTenThousandRules();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "All RuleStore tests passed.\n";
    return EXIT_SUCCESS;
}

