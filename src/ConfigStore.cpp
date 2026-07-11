#include "ConfigStore.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace nppqr {
namespace {

using Json = nlohmann::json;

constexpr wchar_t kWriteMutexName[] = L"Local\\NppQuickReplace.FileWrite";

class ScopedWriteMutex {
public:
    explicit ScopedWriteMutex(std::string& error) {
        handle_ = ::CreateMutexW(nullptr, FALSE, kWriteMutexName);
        if (handle_ == nullptr) {
            error = "Unable to create the settings write lock (Windows error " +
                    std::to_string(::GetLastError()) + ").";
            return;
        }
        const DWORD wait = ::WaitForSingleObject(handle_, 2'000);
        locked_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!locked_) {
            error = wait == WAIT_TIMEOUT
                ? "Another NppQuickReplace instance is writing settings. Try again in a moment."
                : "Unable to acquire the settings write lock (Windows error " +
                      std::to_string(::GetLastError()) + ").";
        }
    }

    ~ScopedWriteMutex() {
        if (locked_) {
            ::ReleaseMutex(handle_);
        }
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
        }
    }

    [[nodiscard]] bool locked() const noexcept { return locked_; }

private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

std::string stripUtf8Bom(std::string content) {
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEFU &&
        static_cast<unsigned char>(content[1]) == 0xBBU &&
        static_cast<unsigned char>(content[2]) == 0xBFU) {
        content.erase(0, 3);
    }
    return content;
}

Json mergeConfigIntoJson(const PluginConfig& config, Json root) {
    if (!root.is_object()) {
        root = Json::object();
    }
    root["pluginEnabled"] = config.pluginEnabled;
    root["rememberEnabledState"] = config.rememberEnabledState;
    root["punctuationTriggers"] = config.punctuationTriggers;
    root["processPaste"] = config.processPaste;
    root["skipReadOnlyDocuments"] = config.skipReadOnlyDocuments;
    root["skipMultiSelection"] = config.skipMultiSelection;
    root["maxTriggerBytes"] = config.maxTriggerBytes;
    root["maxExpandedBytes"] = config.maxExpandedBytes;
    root["backup"]["enabled"] = config.backupEnabled;
    root["backup"]["maxFiles"] = config.maxBackupFiles;
    root["logging"]["enabled"] = config.loggingEnabled;
    root["logging"]["level"] = config.loggingLevel;
    return root;
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

std::filesystem::path uniqueTemporaryPath(const std::filesystem::path& path) {
    return std::filesystem::path(
        path.wstring() + L".tmp." + std::to_wstring(::GetCurrentProcessId()) + L"." +
        std::to_wstring(::GetTickCount64()));
}

bool writeHandle(HANDLE file, std::string_view content, std::string& error) {
    std::string payload(content);
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const std::size_t remaining = payload.size() - offset;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!::WriteFile(file, payload.data() + offset, chunk, &written, nullptr) || written == 0) {
            error = "Unable to write the temporary settings file (Windows error " +
                    std::to_string(::GetLastError()) + ").";
            return false;
        }
        offset += written;
    }
    if (!::FlushFileBuffers(file)) {
        error = "Unable to flush the temporary settings file (Windows error " +
                std::to_string(::GetLastError()) + ").";
        return false;
    }
    return true;
}

bool writeUtf8FileAtomicUnlocked(
    const std::filesystem::path& path,
    std::string_view content,
    std::string& error) {
    const std::filesystem::path temporaryPath = uniqueTemporaryPath(path);
    HANDLE file = ::CreateFileW(
        temporaryPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Unable to create a temporary settings file (Windows error " +
                std::to_string(::GetLastError()) + ").";
        return false;
    }

    const bool wrote = writeHandle(file, content, error);
    ::CloseHandle(file);
    if (!wrote) {
        ::DeleteFileW(temporaryPath.c_str());
        return false;
    }

    if (::ReplaceFileW(
            path.c_str(),
            temporaryPath.c_str(),
            nullptr,
            REPLACEFILE_WRITE_THROUGH,
            nullptr,
            nullptr)) {
        return true;
    }

    const DWORD replaceError = ::GetLastError();
    if (::MoveFileExW(
            temporaryPath.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }

    const DWORD moveError = ::GetLastError();
    error = "Unable to atomically replace the settings file (ReplaceFile error " +
            std::to_string(replaceError) + ", MoveFileEx error " +
            std::to_string(moveError) + ").";
    ::DeleteFileW(temporaryPath.c_str());
    return false;
}

std::wstring backupFileName() {
    SYSTEMTIME time{};
    ::GetLocalTime(&time);
    wchar_t buffer[96]{};
    ::swprintf_s(
        buffer,
        L"replacements_%04u-%02u-%02u_%02u%02u%02u_%03u.json",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds);
    return buffer;
}

} // namespace

bool ConfigStore::ensureDataFiles(
    const std::filesystem::path& dataDirectory,
    std::string& error) {
    std::error_code fileError;
    std::filesystem::create_directories(dataDirectory / "backups", fileError);
    if (!fileError) {
        std::filesystem::create_directories(dataDirectory / "logs", fileError);
    }
    if (fileError) {
        error = "Unable to create the NppQuickReplace data directory.";
        return false;
    }

    const std::filesystem::path configPath = dataDirectory / "config.json";
    if (!std::filesystem::exists(configPath, fileError)) {
        if (!writeUtf8FileAtomic(
                configPath,
                mergeConfigIntoJson(PluginConfig{}, Json::object()).dump(2),
                error)) {
            return false;
        }
    }

    const std::filesystem::path replacementsPath = dataDirectory / "replacements.json";
    if (!std::filesystem::exists(replacementsPath, fileError)) {
        if (!writeUtf8FileAtomic(replacementsPath, defaultReplacementsJson().dump(2), error)) {
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
        std::string content;
        if (!readUtf8File(path, content, result.error)) {
            return result;
        }
        const Json root = Json::parse(content);
        if (!root.is_object()) {
            throw std::runtime_error("The configuration root must be a JSON object.");
        }

        PluginConfig parsed;
        parsed.pluginEnabled = root.value("pluginEnabled", parsed.pluginEnabled);
        parsed.rememberEnabledState = root.value(
            "rememberEnabledState", parsed.rememberEnabledState);
        parsed.punctuationTriggers = root.value(
            "punctuationTriggers", parsed.punctuationTriggers);
        parsed.processPaste = root.value("processPaste", parsed.processPaste);
        parsed.skipReadOnlyDocuments = root.value(
            "skipReadOnlyDocuments", parsed.skipReadOnlyDocuments);
        parsed.skipMultiSelection = root.value(
            "skipMultiSelection", parsed.skipMultiSelection);
        parsed.maxTriggerBytes = root.value("maxTriggerBytes", parsed.maxTriggerBytes);
        parsed.maxExpandedBytes = root.value(
            "maxExpandedBytes", parsed.maxExpandedBytes);

        if (const auto backup = root.find("backup"); backup != root.end() && backup->is_object()) {
            parsed.backupEnabled = backup->value("enabled", parsed.backupEnabled);
            parsed.maxBackupFiles = backup->value("maxFiles", parsed.maxBackupFiles);
        }
        if (const auto logging = root.find("logging"); logging != root.end() && logging->is_object()) {
            parsed.loggingEnabled = logging->value("enabled", parsed.loggingEnabled);
            parsed.loggingLevel = logging->value("level", parsed.loggingLevel);
        }

        if (parsed.maxTriggerBytes < 16 || parsed.maxTriggerBytes > 4096) {
            throw std::runtime_error("maxTriggerBytes must be between 16 and 4096.");
        }
        if (parsed.maxExpandedBytes < 4096 ||
            parsed.maxExpandedBytes > 16U * 1024U * 1024U) {
            throw std::runtime_error(
                "maxExpandedBytes must be between 4096 and 16777216.");
        }
        if (parsed.maxBackupFiles < 1 || parsed.maxBackupFiles > 100) {
            throw std::runtime_error("backup.maxFiles must be between 1 and 100.");
        }
        if (parsed.loggingLevel != "debug" && parsed.loggingLevel != "info" &&
            parsed.loggingLevel != "warning" && parsed.loggingLevel != "error") {
            throw std::runtime_error("logging.level must be debug, info, warning, or error.");
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
    Json root = Json::object();
    std::error_code fileError;
    if (std::filesystem::exists(path, fileError)) {
        std::string content;
        if (!readUtf8File(path, content, error)) {
            return false;
        }
        try {
            root = Json::parse(content);
        } catch (const std::exception& exception) {
            error = "The existing config.json is invalid and was not overwritten: " +
                    std::string(exception.what());
            return false;
        }
    }
    return writeUtf8FileAtomic(path, mergeConfigIntoJson(config, std::move(root)).dump(2), error);
}

bool ConfigStore::readUtf8File(
    const std::filesystem::path& path,
    std::string& content,
    std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Unable to open '" + path.filename().string() + "'.";
        return false;
    }
    std::ostringstream stream;
    stream << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "Unable to finish reading '" + path.filename().string() + "'.";
        return false;
    }
    content = stripUtf8Bom(stream.str());
    return true;
}

bool ConfigStore::writeUtf8FileAtomic(
    const std::filesystem::path& path,
    std::string_view content,
    std::string& error) {
    ScopedWriteMutex lock(error);
    return lock.locked() && writeUtf8FileAtomicUnlocked(path, content, error);
}

AtomicWriteResult ConfigStore::writeUtf8FileAtomicIfUnchanged(
    const std::filesystem::path& path,
    std::uint64_t expectedContentHash,
    std::string_view content,
    std::string& error) {
    ScopedWriteMutex lock(error);
    if (!lock.locked()) return AtomicWriteResult::failed;

    std::string current;
    if (!readUtf8File(path, current, error)) return AtomicWriteResult::failed;
    if (contentHash(current) != expectedContentHash) {
        error = "replacements.json changed on disk after this draft was opened.";
        return AtomicWriteResult::conflict;
    }
    return writeUtf8FileAtomicUnlocked(path, content, error)
        ? AtomicWriteResult::written
        : AtomicWriteResult::failed;
}

std::uint64_t ConfigStore::contentHash(std::string_view content) noexcept {
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEFU &&
        static_cast<unsigned char>(content[1]) == 0xBBU &&
        static_cast<unsigned char>(content[2]) == 0xBFU) {
        content.remove_prefix(3);
    }
    // Atomic writes add one final newline. Ignore that transport detail so the
    // in-memory draft and its just-written file share the same revision hash.
    if (!content.empty() && content.back() == '\n') {
        content.remove_suffix(1);
        if (!content.empty() && content.back() == '\r') content.remove_suffix(1);
    }
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t result = offset;
    for (const unsigned char byte : content) {
        result ^= byte;
        result *= prime;
    }
    return result;
}

bool ConfigStore::fileStamp(
    const std::filesystem::path& path,
    FileStamp& stamp,
    std::string& error) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!::GetFileAttributesExW(
            path.c_str(), GetFileExInfoStandard, &attributes)) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            stamp = {};
            return true;
        }
        error = "Unable to inspect '" + path.filename().string() +
            "' (Windows error " + std::to_string(code) + ").";
        return false;
    }
    ULARGE_INTEGER size{};
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    ULARGE_INTEGER write{};
    write.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    write.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    stamp = {
        .exists = true,
        .size = size.QuadPart,
        .lastWrite = write.QuadPart,
    };
    return true;
}

bool ConfigStore::backupReplacements(
    const std::filesystem::path& dataDirectory,
    const std::filesystem::path& replacementsPath,
    std::size_t maxFiles,
    std::filesystem::path& backupPath,
    std::string& error) {
    std::error_code fileError;
    if (!std::filesystem::exists(replacementsPath, fileError)) {
        backupPath.clear();
        return true;
    }

    const std::filesystem::path backupDirectory = dataDirectory / "backups";
    std::filesystem::create_directories(backupDirectory, fileError);
    if (fileError) {
        error = "Unable to create the replacements backup directory.";
        return false;
    }

    backupPath = backupDirectory / backupFileName();
    if (!::CopyFileW(replacementsPath.c_str(), backupPath.c_str(), TRUE)) {
        error = "Unable to back up replacements.json (Windows error " +
                std::to_string(::GetLastError()) + ").";
        backupPath.clear();
        return false;
    }

    auto backups = replacementBackupsNewestFirst(dataDirectory);
    for (std::size_t index = maxFiles; index < backups.size(); ++index) {
        std::error_code removeError;
        std::filesystem::remove(backups[index], removeError);
    }
    return true;
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

} // namespace nppqr
