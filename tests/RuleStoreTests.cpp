#include "Localization.h"
#include "RuleStore.h"
#include "SnippetTemplate.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

void testCaptureTemplates() {
    nppqr::CapturePattern pattern;
    const auto compiled = pattern.compile("Issue-${capture:1}-${capture:2}", false);
    expect(compiled.ok, "capture template compiles");
    const auto matched = pattern.match("issue-42-open");
    expect(matched.has_value(), "capture template matches ASCII case-insensitively");
    expect(matched.has_value() && matched->values[1] == "42" && matched->values[2] == "open",
        "capture template preserves original captured text");

    nppqr::CapturePattern invalid;
    expect(!invalid.compile("${capture:1}${capture:2}", true).ok,
        "adjacent capture markers are rejected as ambiguous");

    nppqr::RuleStore store;
    const auto loaded = store.loadFromText(R"json({"version":1,"items":[{
      "id":"capture","trigger":"ticket-${capture:1}-${capture:2}",
      "replacement":"Ticket ${capture:1}: ${capture:2}",
      "matchMode":"captureTemplate","activation":["space"]
    }]})json");
    expect(loaded.ok, "capture-template rule loads");
    expect(store.find("ticket-17-ready", nppqr::Activation::space, "") == nullptr,
        "capture-template rules stay out of the literal hash index");
    nppqr::CaptureMatch captures;
    const auto* rule = store.findCaptureTemplate(
        "ticket-17-ready", nppqr::Activation::space, "", "", "", captures);
    expect(rule != nullptr && captures.values[1] == "17" && captures.values[2] == "ready",
        "RuleStore returns a capture-template match and captures");

    const auto undefinedCapture = store.loadFromText(R"json({"version":1,"items":[{
      "trigger":"x-${capture:1}","replacement":"${capture:2}",
      "matchMode":"captureTemplate","activation":["space"]
    }]})json");
    expect(!undefinedCapture.ok, "undefined replacement captures are rejected");

    const auto immediate = store.loadFromText(R"json({"version":1,"items":[{
      "trigger":"x-${capture:1}","replacement":"bad",
      "matchMode":"captureTemplate","activation":["immediate"]
    }]})json");
    expect(!immediate.ok, "capture templates reject immediate activation");
    nppqr::CaptureMatch preserved;
    expect(store.findCaptureTemplate(
        "ticket-18-kept", nppqr::Activation::space, "", "", "", preserved) != nullptr,
        "failed capture-template reload keeps the previous valid rule set");
}
void testPathAndLanguageFilters() {
    nppqr::RuleStore store;
    const auto loaded = store.loadFromText(R"json({"version":1,"items":[{
      "id":"context","trigger":"ctx","replacement":"matched","activation":["space"],
      "fileExtensions":[".md"],"pathGlobs":["*/docs/*.md"],"languages":["markdown"]
    }]})json");
    expect(loaded.ok, "path and language filters load");
    expect(store.find("ctx", nppqr::Activation::space, ".MD",
        "C:\\Work\\docs\\guide.md", "Markdown") != nullptr,
        "matching extension, path glob, and language allow a rule");
    expect(store.find("ctx", nppqr::Activation::space, ".md",
        "C:/Work/source/guide.md", "Markdown") == nullptr,
        "non-matching path glob blocks a rule");
    expect(store.find("ctx", nppqr::Activation::space, ".md",
        "C:/Work/docs/guide.md", "Python") == nullptr,
        "non-matching language blocks a rule");
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
        R"json({"pluginEnabled":true,"maxExpandedBytes":2097152,"futureField":{"keep":42}})json", error),
        "test config is written atomically");
    nppqr::PluginConfig config;
    const auto loaded = nppqr::ConfigStore::loadConfig(configPath, config);
    expect(loaded.ok, "test config loads");
    expect(config.maxExpandedBytes == 2U * 1024U * 1024U,
        "maxExpandedBytes loads from config");
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
    expect(savedJson["maxExpandedBytes"] == 2U * 1024U * 1024U,
        "maxExpandedBytes survives a config save");

    const std::filesystem::path replacementsPath = directory / "replacements.json";
    const std::string originalRules = R"json({"version":1,"items":[]})json";
    expect(nppqr::ConfigStore::writeUtf8FileAtomic(replacementsPath,
        originalRules, error), "test replacements file is written");
    const std::uint64_t originalHash = nppqr::ConfigStore::contentHash(originalRules);
    const std::string externalRules =
        R"json({"version":1,"items":[{"trigger":"external","replacement":"kept"}]})json";
    expect(nppqr::ConfigStore::writeUtf8FileAtomic(replacementsPath, externalRules, error),
        "simulated external edit is written");
    expect(nppqr::ConfigStore::writeUtf8FileAtomicIfUnchanged(
        replacementsPath, originalHash, "stale draft", error) == nppqr::AtomicWriteResult::conflict,
        "stale draft detects an external write conflict");
    std::string preservedExternal;
    expect(nppqr::ConfigStore::readUtf8File(replacementsPath, preservedExternal, error) &&
        preservedExternal == externalRules + "\n", "conflict leaves the external file unchanged");
    expect(nppqr::ConfigStore::writeUtf8FileAtomicIfUnchanged(replacementsPath,
        nppqr::ConfigStore::contentHash(externalRules), originalRules, error) ==
        nppqr::AtomicWriteResult::written, "matching revision may be written");
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
        "pathGlobs":["*/docs/*.md"],
        "languages":["Markdown"],
        "description":"쉼표와 줄바꿈 테스트",
        "futureMetadata":{"keep":42}
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
        expect(item["pathGlobs"] == nlohmann::json::array({"*/docs/*.md"}) &&
            item["languages"] == nlohmann::json::array({"Markdown"}),
            "CSV round-trip preserves path and language filters");
        expect(root["groups"].size() == 1 && root["groups"][0]["id"] == "team",
            "CSV import creates a referenced group");
        expect(item["futureMetadata"]["keep"] == 42,
            "CSV extraJson preserves unknown per-rule fields");
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
    const auto duplicateHeader = nppqr::RuleExchange::importDelimited(
        existing, "trigger,TRIGGER,replacement\r\na,b,c\r\n", ',',
        nppqr::DelimitedImportMode::replace);
    expect(!duplicateHeader.ok && duplicateHeader.error.find("column 2") != std::string::npos,
        "duplicate headers report the offending column");

    const auto malformedQuote = nppqr::RuleExchange::importDelimited(
        existing, "trigger,replacement\r\n\"bad\"x,value\r\n", ',',
        nppqr::DelimitedImportMode::replace);
    expect(!malformedQuote.ok && malformedQuote.error.find("line 2") != std::string::npos,
        "malformed quoted fields report the source line");

    constexpr std::string_view generatedIdCsv =
        "trigger,replacement\r\nstable,deterministic\r\n";
    const auto generatedA = nppqr::RuleExchange::importDelimited(
        R"json({"version":1,"items":[]})json", generatedIdCsv, ',',
        nppqr::DelimitedImportMode::replace);
    const auto generatedB = nppqr::RuleExchange::importDelimited(
        R"json({"version":1,"items":[]})json", generatedIdCsv, ',',
        nppqr::DelimitedImportMode::replace);
    expect(generatedA.ok && generatedB.ok &&
        nlohmann::json::parse(generatedA.text)["items"][0]["id"] ==
        nlohmann::json::parse(generatedB.text)["items"][0]["id"],
        "generated import ids are deterministic across runs");

    constexpr std::string_view formulaJson = R"json({"version":1,"items":[{
      "id":"formula","trigger":"=2+2","replacement":"@SUM(A1:A2)","activation":["space"]
    }]})json";
    const auto losslessFormula = nppqr::RuleExchange::exportDelimited(formulaJson, ',');
    expect(losslessFormula.ok && !losslessFormula.warnings.empty(),
        "lossless export warns about spreadsheet formula cells");
    const auto safeFormula = nppqr::RuleExchange::exportDelimited(formulaJson, ',', true);
    expect(safeFormula.ok && safeFormula.text.find("'=2+2") != std::string::npos &&
        safeFormula.text.find("'@SUM") != std::string::npos,
        "spreadsheet-safe export prefixes risky formula cells");
}

void testSnippetMarkers() {
    const auto ordered = nppqr::parseSnippetMarkers(
        "A${tabstop:2}B${tabstop:1}C${tabstop:0}D");
    expect(ordered.text == "ABCD", "snippet markers are removed from output");
    expect(ordered.tabstopOffsets == std::vector<std::size_t>({2, 1, 3}),
        "tabstops are ordered as 1..9 followed by 0");

    const auto cursorFinal = nppqr::parseSnippetMarkers(
        "x${tabstop:1}y${cursor}z");
    expect(cursorFinal.tabstopOffsets == std::vector<std::size_t>({1, 2}),
        "cursor marker becomes the final stop when tabstop 0 is absent");

    const auto cursorOnly = nppqr::parseSnippetMarkers("a${cursor}b");
    expect(cursorOnly.cursorOffset == 1 && cursorOnly.tabstopOffsets.empty(),
        "a lone cursor marker remains a simple cursor position");

    const auto invalid = nppqr::parseSnippetMarkers("${tabstop:10}");
    expect(invalid.text == "${tabstop:10}" && invalid.tabstopOffsets.empty(),
        "unsupported tabstop numbers remain literal");
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

void testLanguageDetection() {
    using nppqr::localization::Language;
    using nppqr::localization::languageForLangId;
    expect(languageForLangId(MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN)) == Language::korean,
        "Korean Windows UI language selects Korean text");
    expect(languageForLangId(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)) == Language::english,
        "English Windows UI language selects English text");
    expect(languageForLangId(MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN)) == Language::english,
        "unsupported Windows UI languages fall back to English");
    const wchar_t* expected = nppqr::localization::currentLanguage() == Language::korean
        ? L"치환 규칙"
        : L"Replacement rules";
    expect(std::wstring(nppqr::localization::text(L"Replacement rules")) == expected,
        "runtime localization follows the current Windows UI language");
}

} // namespace

int main() {
    testBasicMatching();
    testDelimiterClassification();
    testValidationAndTransactionalLoad();
    testImmediateRulesAndPrefixSafety();
    testCaptureTemplates();
    testPathAndLanguageFilters();
    testUnicodeNormalization();
    testConfigPreservationAndBackups();
    testDelimitedExchangeRoundTrip();
    testDelimitedImportModesAndValidation();
    testSnippetMarkers();
    testTenThousandRules();

    testLanguageDetection();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "All RuleStore tests passed.\n";
    return EXIT_SUCCESS;
}

