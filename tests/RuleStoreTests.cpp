#include "RuleStore.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>
#include "ConfigStore.h"
#include "RuleExchange.h"

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


void testImmediateRulesAndPrefixSafety() {
    nppqr::RuleStore store;
    const auto loaded = store.loadFromText(R"json(
    {"version":1,"items":[
      {"id":"instant","trigger":"zz","replacement":"instant","activation":["immediate"]},
      {"id":"delimited","trigger":"word","replacement":"value","activation":["space"]}
    ]}
    )json");
    expect(loaded.ok, "immediate rules load");
    expect(store.hasImmediateRules(), "immediate rule presence is cached");
    expect(store.findImmediate("zz", "") != nullptr, "immediate rule can be found");
    expect(store.findImmediate("word", "") == nullptr, "delimiter rule is not treated as immediate");

    const auto conflict = store.loadFromText(R"json(
    {"version":1,"items":[
      {"trigger":"aa","replacement":"short","activation":["immediate"]},
      {"trigger":"aaa","replacement":"long","activation":["space"]}
    ]}
    )json");
    expect(!conflict.ok, "an immediate prefix conflict is rejected");
    expect(store.size() == 2, "failed immediate load remains transactional");
}

void testUnicodeNormalization() {
    nppqr::RuleStore store;
    const auto loaded = store.loadFromText(R"json(
    {"version":1,"items":[
      {"trigger":"é","replacement":"accent","activation":["space"]}
    ]}
    )json");
    expect(loaded.ok, "NFC trigger loads");
    expect(store.find("é", nppqr::Activation::space, "") != nullptr,
        "canonically equivalent NFD input matches an NFC rule");
}

void testConfigPreservationAndBackups() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() /
        (L"NppQuickReplaceTests-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
         std::to_wstring(::GetTickCount64()));
    std::error_code directoryError;
    std::filesystem::create_directories(directory, directoryError);
    expect(!directoryError, "temporary config test directory is created");

    const std::filesystem::path configPath = directory / "config.json";
    std::string error;
    expect(nppqr::ConfigStore::writeUtf8FileAtomic(configPath,
        R"json({"pluginEnabled":true,"futureField":{"keep":42}})json", error),
        "test config is written atomically");
    nppqr::PluginConfig config;
    const auto loaded = nppqr::ConfigStore::loadConfig(configPath, config);
    expect(loaded.ok, "test config loads");
    config.pluginEnabled = false;
    const bool configSaved = nppqr::ConfigStore::saveConfigAtomic(configPath, config, error);
    if (!configSaved) {
        std::cerr << "Config save detail: " << error << '\n';
    }
    expect(configSaved, "config saves while preserving unknown fields");
    std::string saved;
    expect(nppqr::ConfigStore::readUtf8File(configPath, saved, error), "saved config is readable");
    const nlohmann::json savedJson = nlohmann::json::parse(saved);
    expect(savedJson["futureField"]["keep"] == 42, "unknown config fields survive a save");
    expect(savedJson["pluginEnabled"] == false, "known config fields are updated");

    const std::filesystem::path replacementsPath = directory / "replacements.json";
    expect(nppqr::ConfigStore::writeUtf8FileAtomic(replacementsPath,
        R"json({"version":1,"items":[]})json", error), "test replacements file is written");
    std::filesystem::path backupPath;
    expect(nppqr::ConfigStore::backupReplacements(
        directory, replacementsPath, 3, backupPath, error), "replacement backup is created");
    expect(!backupPath.empty() && std::filesystem::exists(backupPath), "backup path exists");

    std::filesystem::remove_all(directory, directoryError);
}

void testDelimitedExchangeRoundTrip() {
    const char* source = R"json({
      "version": 1,
      "groups": [{"id":"team","name":"팀 규칙","enabled":true}],
      "items": [{
        "id":"csv-roundtrip",
        "enabled":true,
        "trigger":"인사,말",
        "replacement":"첫째 줄\n\"인용문\", 둘째 줄",
        "group":"team",
        "matchMode":"wholeWord",
        "caseSensitive":true,
        "activation":["space","enter"],
        "fileExtensions":[".md",".txt"],
        "description":"쉼표와 줄바꿈 테스트"
      }]
    })json";

    const auto exported = nppqr::RuleExchange::exportDelimited(source, ',');
    expect(exported.ok, "CSV export succeeds");
    expect(exported.itemCount == 1, "CSV export reports one rule");
    expect(exported.text.starts_with("\xEF\xBB\xBF"), "CSV export includes a UTF-8 BOM");
    expect(exported.text.find("\"\"인용문\"\"") != std::string::npos,
        "CSV export escapes embedded quotes");

    const auto imported = nppqr::RuleExchange::importDelimited(
        R"json({"version":1,"groups":[],"items":[]})json",
        exported.text,
        ',',
        nppqr::DelimitedImportMode::replace);
    expect(imported.ok, "exported CSV imports again");
    expect(imported.itemCount == 1, "CSV import reports one rule");
    if (imported.ok) {
        const auto root = nlohmann::json::parse(imported.text);
        const auto& item = root["items"][0];
        expect(item["trigger"] == "인사,말", "CSV round-trip preserves Korean and commas");
        expect(item["replacement"] == "첫째 줄\n\"인용문\", 둘째 줄",
            "CSV round-trip preserves multiline quoted replacements");
        expect(item["activation"] == nlohmann::json::array({"space", "enter"}),
            "CSV round-trip preserves activation lists");
        expect(item["fileExtensions"] == nlohmann::json::array({".md", ".txt"}),
            "CSV round-trip preserves extension lists");
        expect(root["groups"].size() == 1 && root["groups"][0]["id"] == "team",
            "CSV import creates a referenced group");
    }
}

void testDelimitedImportModesAndValidation() {
    constexpr std::string_view existing = R"json({
      "version":1,
      "groups":[{"id":"old","name":"Old","enabled":true}],
      "items":[{
        "id":"same-id","trigger":"old","replacement":"old value",
        "group":"old","activation":["space"]
      }]
    })json";
    constexpr std::string_view tsv =
        "id\tenabled\ttrigger\treplacement\tgroup\tactivation\tcaseSensitive\tfileExtensions\tdescription\r\n"
        "same-id\tfalse\tnew\t새 값\timported\ttab|enter\tyes\tmd|TXT\t설명\r\n";

    const auto replace = nppqr::RuleExchange::importDelimited(
        existing, tsv, '\t', nppqr::DelimitedImportMode::replace);
    expect(replace.ok, "replace import may reuse an id removed by replacement");
    if (replace.ok) {
        const auto root = nlohmann::json::parse(replace.text);
        expect(root["items"].size() == 1 && root["items"][0]["trigger"] == "new",
            "replace mode removes previous rules");
        expect(root["items"][0]["enabled"] == false, "TSV boolean false is parsed");
        expect(root["items"][0]["caseSensitive"] == true, "TSV yes boolean is parsed");
        expect(root["items"][0]["fileExtensions"] == nlohmann::json::array({".md", ".TXT"}),
            "TSV extension list is normalized with dots");
    }

    const auto appendConflict = nppqr::RuleExchange::importDelimited(
        existing, tsv, '\t', nppqr::DelimitedImportMode::append);
    expect(!appendConflict.ok, "append import rejects an existing id conflict");

    const auto missingHeader = nppqr::RuleExchange::importDelimited(
        existing, "id,name\r\n1,test\r\n", ',', nppqr::DelimitedImportMode::append);
    expect(!missingHeader.ok, "import rejects a file without required headers");

    const auto emptyReplacement = nppqr::RuleExchange::importDelimited(
        existing, "trigger,replacement\r\nbad,\r\n", ',', nppqr::DelimitedImportMode::replace);
    expect(!emptyReplacement.ok, "import rejects a row with an empty replacement");
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
    testImmediateRulesAndPrefixSafety();
    testUnicodeNormalization();
    testConfigPreservationAndBackups();
    testDelimitedExchangeRoundTrip();
    testDelimitedImportModesAndValidation();
    testTenThousandRules();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "All RuleStore tests passed.\n";
    return EXIT_SUCCESS;
}

