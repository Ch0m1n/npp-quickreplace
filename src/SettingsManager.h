#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>

#include "ConfigStore.h"

namespace nppqr {

struct SettingsManagerOptions {
    HWND notepadHandle = nullptr;
    HINSTANCE module = nullptr;
    std::filesystem::path dataDirectory;
    std::filesystem::path replacementsPath;
    std::filesystem::path configPath;
    PluginConfig config;
    std::function<void()> onDataImported;
    std::function<void(const PluginConfig&)> onSaved;
};

void showSettingsManager(const SettingsManagerOptions& options);
void closeSettingsManager(bool discardChanges = false);
void handleSettingsManagerDarkModeChange();

} // namespace nppqr
