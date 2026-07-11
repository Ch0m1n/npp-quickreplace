#pragma once

#include <windows.h>

#include <nlohmann/json.hpp>

namespace nppqr {

bool showGroupManager(
    HWND owner,
    HWND notepadHandle,
    HINSTANCE module,
    nlohmann::json& document);
[[nodiscard]] bool isGroupManagerOpen();
void closeGroupManager(bool discardChanges = false);
void handleGroupManagerDarkModeChange();


} // namespace nppqr
