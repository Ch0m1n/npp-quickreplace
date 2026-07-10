#pragma once

#include <windows.h>

#include <nlohmann/json.hpp>

namespace nppqr {

bool showGroupManager(
    HWND owner,
    HWND notepadHandle,
    HINSTANCE module,
    nlohmann::json& document);

} // namespace nppqr
