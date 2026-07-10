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
    bool skipReadOnlyDocuments = true;
    bool skipMultiSelection = true;
    std::size_t maxTriggerBytes = 512;
};

struct ConfigLoadResult {
    bool ok = false;
    std::string error;
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

    static std::vector<std::filesystem::path> replacementBackupsNewestFirst(
        const std::filesystem::path& dataDirectory);

private:
    static bool atomicWriteUtf8(
        const std::filesystem::path& path,
        std::string_view content,
        std::string& error);
};

} // namespace nppqr

