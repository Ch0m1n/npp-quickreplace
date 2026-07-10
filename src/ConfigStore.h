#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nppqr {

struct PluginConfig {
    bool pluginEnabled = true;
    bool rememberEnabledState = true;
    std::string punctuationTriggers = ".,:;!?)]}";
    bool processPaste = false;
    bool skipReadOnlyDocuments = true;
    bool skipMultiSelection = true;
    std::size_t maxTriggerBytes = 512;
    bool backupEnabled = true;
    std::size_t maxBackupFiles = 10;
    bool loggingEnabled = false;
    std::string loggingLevel = "warning";
};

struct ConfigLoadResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
};

class ConfigStore {
public:
    static bool ensureDataFiles(
        const std::filesystem::path& dataDirectory,
        std::string& error);

    static ConfigLoadResult loadConfig(
        const std::filesystem::path& path,
        PluginConfig& config);

    static bool saveConfigAtomic(
        const std::filesystem::path& path,
        const PluginConfig& config,
        std::string& error);

    static bool readUtf8File(
        const std::filesystem::path& path,
        std::string& content,
        std::string& error);

    static bool writeUtf8FileAtomic(
        const std::filesystem::path& path,
        std::string_view content,
        std::string& error);

    static bool backupReplacements(
        const std::filesystem::path& dataDirectory,
        const std::filesystem::path& replacementsPath,
        std::size_t maxFiles,
        std::filesystem::path& backupPath,
        std::string& error);

    static std::vector<std::filesystem::path> replacementBackupsNewestFirst(
        const std::filesystem::path& dataDirectory);
};

} // namespace nppqr

