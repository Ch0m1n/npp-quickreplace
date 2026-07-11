#pragma once

#include <windows.h>

namespace nppqr::localization {

enum class Language {
    english,
    korean,
};

Language languageForLangId(LANGID languageId) noexcept;
Language currentLanguage() noexcept;
const wchar_t* text(const wchar_t* english) noexcept;
const wchar_t* text(const wchar_t* english, const wchar_t* korean) noexcept;

} // namespace nppqr::localization
