#pragma once

#include <filesystem>
#include <string>

namespace nppqr {

struct DataPackageResult {
    bool ok = false;
    std::string error;
    std::filesystem::path recoveryDirectory;
};

class DataPackage {
public:
    static DataPackageResult exportPackage(
        const std::filesystem::path& configPath,
        const std::filesystem::path& replacementsPath,
        const std::filesystem::path& packagePath);

    static DataPackageResult importPackage(
        const std::filesystem::path& dataDirectory,
        const std::filesystem::path& configPath,
        const std::filesystem::path& replacementsPath,
        const std::filesystem::path& packagePath);
};

} // namespace nppqr
