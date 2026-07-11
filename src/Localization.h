#pragma once

#include <string_view>

#include <windows.h>

namespace nppqr::localization {

enum class Language {
    english,
    korean,
};

enum class LanguagePreference {
    automatic,
    english,
    korean,
};

Language languageForLangId(LANGID languageId) noexcept;
Language currentLanguage() noexcept;
void setLanguagePreference(std::string_view preference) noexcept;
LanguagePreference languagePreference() noexcept;
Language detectedLanguage() noexcept;
const wchar_t* text(const wchar_t* english) noexcept;
const wchar_t* text(const wchar_t* english, const wchar_t* korean) noexcept;

} // namespace nppqr::localization
