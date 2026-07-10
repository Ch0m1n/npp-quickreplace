#include "ConfigStore.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace nppqr {
namespace {

using Json = nlohmann::json;

std::string readUtf8File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to open the configuration file.");
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

Json configToJson(const PluginConfig& config) {
    return Json{
        {"pluginEnabled", config.pluginEnabled},
        {"rememberEnabledState", config.rememberEnabledState},
        {"punctuationTriggers", config.punctuationTriggers},
        {"processPaste", false},
        {"skipReadOnlyDocuments", config.skipReadOnlyDocuments},
        {"skipMultiSelection", config.skipMultiSelection},
        {"maxTriggerBytes", config.maxTriggerBytes},
        {"logging", {{"enabled", false}, {"level", "warning"}}},
    };
}

Json defaultReplacementsJson() {
    return Json{
        {"version", 1},
        {"groups",
         Json::array({
             {{"id", "common"}, {"name", "공통"}, {"enabled", true}},
             {{"id", "wows"}, {"name", "World of Warships"}, {"enabled", true}},
             {{"id", "templates"}, {"name", "작업 템플릿"}, {"enabled", true}},
         })},
        {"items",
         Json::array({
             {
                 {"id", "sample-wows-aa"},
                 {"enabled", true},
                 {"trigger", "ㄱ123"},
                 {"replacement", "대공 방어 사격"},
                 {"group", "wows"},
                 {"matchMode", "wholeWord"},
                 {"caseSensitive", false},
                 {"activation", Json::array({"space", "enter", "tab", "punctuation"})},
                 {"fileExtensions", Json::array()},
                 {"description", "샘플 한글/숫자 혼합 축약어"},
             },
             {
                 {"id", "sample-thanks"},
                 {"enabled", true},
                 {"trigger", "ㅅㄱㅈ"},
                 {"replacement", "수고하셨습니다."},
                 {"group", "common"},
                 {"matchMode", "wholeWord"},
                 {"caseSensitive", false},
                 {"activation", Json::array({"space", "enter"})},
                 {"fileExtensions", Json::array()},
             },
             {
                 {"id", "sample-patch-template"},
                 {"enabled", true},
                 {"trigger", "patchtpl"},
                 {"replacement", "변경 전:\n변경 후:\n변경 사유: ${cursor}"},
                 {"group", "templates"},
                 {"matchMode", "wholeWord"},
                 {"caseSensitive", false},
                 {"activation", Json::array({"space", "enter", "tab"})},
                 {"fileExtensions", Json::array({".txt", ".md"})},
             },
         })},
    };
}

} // namespace

bool ConfigStore::ensureDataFiles(
    const std::filesystem::path& dataDirectory,
    std::string& error) {
    std::error_code fileError;
    std::filesystem::create_directories(dataDirectory / "backups", fileError);
    if (fileError) {
        error = "Unable to create the NppQuickReplace data directory.";
        return false;
    }

    const std::filesystem::path configPath = dataDirectory / "config.json";
    if (!std::filesystem::exists(configPath, fileError)) {
        if (!atomicWriteUtf8(configPath, configToJson(PluginConfig{}).dump(2), error)) {
            return false;
        }
    }

    const std::filesystem::path replacementsPath = dataDirectory / "replacements.json";
    if (!std::filesystem::exists(replacementsPath, fileError)) {
        if (!atomicWriteUtf8(replacementsPath, defaultReplacementsJson().dump(2), error)) {
            return false;
        }
    }
    return true;
}

ConfigLoadResult ConfigStore::loadConfig(
    const std::filesystem::path& path,
    PluginConfig& config) {
    ConfigLoadResult result;
    try {
        const Json root = Json::parse(readUtf8File(path));
        if (!root.is_object()) {
            throw std::runtime_error("The configuration root must be a JSON object.");
        }

        PluginConfig parsed;
        parsed.pluginEnabled = root.value("pluginEnabled", parsed.pluginEnabled);
        parsed.rememberEnabledState = root.value(
            "rememberEnabledState", parsed.rememberEnabledState);
        parsed.punctuationTriggers = root.value(
            "punctuationTriggers", parsed.punctuationTriggers);
        parsed.skipReadOnlyDocuments = root.value(
            "skipReadOnlyDocuments", parsed.skipReadOnlyDocuments);
        parsed.skipMultiSelection = root.value(
            "skipMultiSelection", parsed.skipMultiSelection);
        parsed.maxTriggerBytes = root.value(
            "maxTriggerBytes", parsed.maxTriggerBytes);

        if (parsed.maxTriggerBytes < 16 || parsed.maxTriggerBytes > 4096) {
            throw std::runtime_error("maxTriggerBytes must be between 16 and 4096.");
        }

        config = std::move(parsed);
        result.ok = true;
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
        return result;
    }
}

bool ConfigStore::saveConfigAtomic(
    const std::filesystem::path& path,
    const PluginConfig& config,
    std::string& error) {
    return atomicWriteUtf8(path, configToJson(config).dump(2), error);
}

std::vector<std::filesystem::path> ConfigStore::replacementBackupsNewestFirst(
    const std::filesystem::path& dataDirectory) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    const std::filesystem::path backupDirectory = dataDirectory / "backups";
    if (!std::filesystem::exists(backupDirectory, error)) {
        return result;
    }

    for (const auto& entry : std::filesystem::directory_iterator(backupDirectory, error)) {
        if (error || !entry.is_regular_file()) {
            continue;
        }
        const std::wstring name = entry.path().filename().wstring();
        if (name.starts_with(L"replacements_") && entry.path().extension() == L".json") {
            result.push_back(entry.path());
        }
    }

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        std::error_code leftError;
        std::error_code rightError;
        const auto leftTime = std::filesystem::last_write_time(left, leftError);
        const auto rightTime = std::filesystem::last_write_time(right, rightError);
        if (leftError || rightError) {
            return left.filename().wstring() > right.filename().wstring();
        }
        return leftTime > rightTime;
    });
    return result;
}

bool ConfigStore::atomicWriteUtf8(
    const std::filesystem::path& path,
    std::string_view content,
    std::string& error) {
    const std::filesystem::path temporaryPath = path.wstring() + L".tmp";
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to open a temporary settings file for writing.";
            return false;
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.put('\n');
        output.flush();
        if (!output) {
            error = "Unable to finish writing the temporary settings file.";
            return false;
        }
    }

    if (!::MoveFileExW(
            temporaryPath.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Unable to atomically replace the settings file (Windows error " +
                std::to_string(::GetLastError()) + ").";
        ::DeleteFileW(temporaryPath.c_str());
        return false;
    }
    return true;
}

} // namespace nppqr

