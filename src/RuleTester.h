#pragma once

#include <windows.h>

#include <string>

namespace nppqr {

void showRuleTester(
    HWND owner,
    HWND notepadHandle,
    HINSTANCE module,
    std::string ruleDocument);
[[nodiscard]] bool isRuleTesterOpen();
void closeRuleTester();
void handleRuleTesterDarkModeChange();

} // namespace nppqr
