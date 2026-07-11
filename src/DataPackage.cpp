#include "DataPackage.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

#include "ConfigStore.h"
#include "RuleStore.h"

namespace nppqr {
namespace {

using Json = nlohmann::json;

std::wstring timestamp() {
    SYSTEMTIME time{};
    ::GetLocalTime(&time);
    wchar_t value[48]{};
    ::swprintf_s(value, L"%04u%02u%02u_%02u%02u%02u_%03u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds);
    return value;
}

std::string createdUtc() {
    SYSTEMTIME time{};
    ::GetSystemTime(&time);
    char value[40]{};
    ::sprintf_s(value, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds);
    return value;
}

bool samePath(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    const std::wstring normalizedLeft =
        std::filesystem::absolute(left).lexically_normal().wstring();
    const std::wstring normalizedRight =
        std::filesystem::absolute(right).lexically_normal().wstring();
    return ::CompareStringOrdinal(
        normalizedLeft.c_str(), static_cast<int>(normalizedLeft.size()),
        normalizedRight.c_str(), static_cast<int>(normalizedRight.size()), TRUE) == CSTR_EQUAL;
}

bool validateConfig(
    const std::filesystem::path& dataDirectory,
    std::string_view text,
    PluginConfig& config,
    std::string& error) {
    const std::filesystem::path temporary = dataDirectory /
        (L".package-config-validation-" + std::to_wstring(::GetCurrentProcessId()) +
            L"-" + std::to_wstring(::GetTickCount64()) + L".json");
    if (!ConfigStore::writeUtf8FileAtomic(temporary, text, error)) return false;
    const ConfigLoadResult loaded = ConfigStore::loadConfig(temporary, config);
    std::error_code removeError;
    std::filesystem::remove(temporary, removeError);
    if (!loaded.ok) {
        error = "The package configuration is invalid: " + loaded.error;
        return false;
    }
    return true;
}

bool copyForRecovery(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string& error) {
    std::error_code fileError;
    if (!std::filesystem::exists(source, fileError)) return true;
    if (!::CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
        error = "Unable to create the pre-import recovery copy (Windows error " +
            std::to_string(::GetLastError()) + ").";
        return false;
    }
    return true;
}

} // namespace

DataPackageResult DataPackage::exportPackage(
    const std::filesystem::path& configPath,
    const std::filesystem::path& replacementsPath,
    const std::filesystem::path& packagePath) {
    DataPackageResult result;
    try {
        if (samePath(packagePath, configPath) || samePath(packagePath, replacementsPath)) {
            result.error = "Choose a package path different from config.json and replacements.json.";
            return result;
        }
        std::string configText;
        std::string replacementsText;
        if (!ConfigStore::readUtf8File(configPath, configText, result.error) ||
            !ConfigStore::readUtf8File(replacementsPath, replacementsText, result.error)) {
            return result;
        }

        Json config = Json::parse(configText);
        Json replacements = Json::parse(replacementsText);
        if (!config.is_object() || !replacements.is_object()) {
            result.error = "config.json and replacements.json must both contain JSON objects.";
            return result;
        }
        PluginConfig configValidation;
        if (!validateConfig(configPath.parent_path(), configText, configValidation, result.error)) {
            result.error = "The configuration is invalid and was not exported: " + result.error;
            return result;
        }
        RuleStore rules;
        const RuleLoadResult validation = rules.loadFromText(replacementsText);
        if (!validation.ok) {
            result.error = "The replacement rules are invalid and were not exported: " +
                validation.error;
            return result;
        }

        Json package{
            {"packageType", "NppQuickReplaceData"},
            {"packageVersion", 1},
            {"createdUtc", createdUtc()},
            {"config", std::move(config)},
            {"replacements", std::move(replacements)},
        };
        if (!ConfigStore::writeUtf8FileAtomic(
                packagePath, package.dump(2), result.error)) {
            return result;
        }
        result.ok = true;
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
        return result;
    }
}

DataPackageResult DataPackage::importPackage(
    const std::filesystem::path& dataDirectory,
    const std::filesystem::path& configPath,
    const std::filesystem::path& replacementsPath,
    const std::filesystem::path& packagePath) {
    DataPackageResult result;
    try {
        if (samePath(packagePath, configPath) || samePath(packagePath, replacementsPath)) {
            result.error = "The package file must be separate from config.json and replacements.json.";
            return result;
        }
        std::error_code fileError;
        const auto size = std::filesystem::file_size(packagePath, fileError);
        if (fileError || size > 256U * 1024U * 1024U) {
            result.error = fileError
                ? "Unable to inspect the selected data package."
                : "The selected data package exceeds 256 MiB.";
            return result;
        }

        std::string packageText;
        if (!ConfigStore::readUtf8File(packagePath, packageText, result.error)) {
            return result;
        }
        const Json package = Json::parse(packageText);
        if (!package.is_object() ||
            package.value("packageType", "") != "NppQuickReplaceData" ||
            package.value("packageVersion", 0) != 1 ||
            !package.contains("config") || !package["config"].is_object() ||
            !package.contains("replacements") || !package["replacements"].is_object()) {
            result.error = "The selected file is not a supported NppQuickReplace data package.";
            return result;
        }

        const std::string configText = package["config"].dump(2);
        const std::string replacementsText = package["replacements"].dump(2);
        PluginConfig configValidation;
        if (!validateConfig(dataDirectory, configText, configValidation, result.error)) {
            return result;
        }
        RuleStore ruleValidation;
        const RuleLoadResult rules = ruleValidation.loadFromText(replacementsText);
        if (!rules.ok) {
            result.error = "The package replacement rules are invalid: " + rules.error;
            return result;
        }

        result.recoveryDirectory = dataDirectory / "package-backups" /
            (L"before_import_" + timestamp());
        std::filesystem::create_directories(result.recoveryDirectory, fileError);
        if (fileError) {
            result.error = "Unable to create the pre-import recovery directory.";
            result.recoveryDirectory.clear();
            return result;
        }
        const std::filesystem::path configRecovery =
            result.recoveryDirectory / L"config.json";
        const std::filesystem::path replacementsRecovery =
            result.recoveryDirectory / L"replacements.json";
        if (!copyForRecovery(configPath, configRecovery, result.error) ||
            !copyForRecovery(replacementsPath, replacementsRecovery, result.error)) {
            return result;
        }

        if (!ConfigStore::writeUtf8FileAtomic(configPath, configText, result.error)) {
            return result;
        }
        if (!ConfigStore::writeUtf8FileAtomic(
                replacementsPath, replacementsText, result.error)) {
            std::string rollbackError;
            std::string oldConfig;
            if (ConfigStore::readUtf8File(configRecovery, oldConfig, rollbackError)) {
                ConfigStore::writeUtf8FileAtomic(configPath, oldConfig, rollbackError);
            }
            result.error += " The previous config.json was restored from the recovery copy.";
            return result;
        }

        result.ok = true;
        return result;
    } catch (const std::exception& exception) {
        result.error = exception.what();
        return result;
    }
}

} // namespace nppqr
