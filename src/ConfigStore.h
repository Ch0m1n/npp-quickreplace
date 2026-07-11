#pragma once

#include <cstddef>
#include <cstdint>
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
    bool skipMultiSelection = false;
    bool autoReloadRules = true;
    std::size_t autoReloadIntervalMs = 1000;
    std::string uiLanguage = "auto";
    std::size_t maxTriggerBytes = 512;
    std::size_t maxExpandedBytes = 1U * 1024U * 1024U;
    bool backupEnabled = true;
    std::size_t maxBackupFiles = 10;
    bool loggingEnabled = false;
    std::string loggingLevel = "warning";

    bool operator==(const PluginConfig&) const = default;
};

struct ConfigLoadResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
};

struct FileStamp {
    bool exists = false;
    std::uint64_t size = 0;
    std::uint64_t lastWrite = 0;

    bool operator==(const FileStamp&) const = default;
};

enum class AtomicWriteResult {
    written,
    conflict,
    failed,
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

    static AtomicWriteResult writeUtf8FileAtomicIfUnchanged(
        const std::filesystem::path& path,
        std::uint64_t expectedContentHash,
        std::string_view content,
        std::string& error);

    static bool fileStamp(
        const std::filesystem::path& path,
        FileStamp& stamp,
        std::string& error);

    [[nodiscard]] static std::uint64_t contentHash(
        std::string_view content) noexcept;

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

