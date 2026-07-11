#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>

#include "ConfigStore.h"

namespace nppqr {

struct RuleManagerOptions {
    HWND notepadHandle = nullptr;
    HINSTANCE module = nullptr;
    std::filesystem::path dataDirectory;
    std::filesystem::path replacementsPath;
    PluginConfig config;
    std::function<void()> onRulesSaved;
};

void showRuleManager(const RuleManagerOptions& options);
void closeRuleManager(bool discardChanges = false);
void handleRuleManagerDarkModeChange();

} // namespace nppqr
