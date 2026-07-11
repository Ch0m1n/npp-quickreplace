#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

#include "ConfigStore.h"
#include "Localization.h"
#include "RuleManager.h"
#include "RuleStore.h"
#include "SnippetTemplate.h"
#include "PluginInterface.h"

namespace {

using nppqr::Activation;
using nppqr::CaptureMatch;
using nppqr::ConfigStore;
using nppqr::PluginConfig;
using nppqr::ReplacementRule;
using nppqr::RuleManagerOptions;
using nppqr::RuleStore;

constexpr wchar_t kPluginName[] = L"NppQuickReplace";
constexpr wchar_t kPluginVersion[] = L"0.6.0-alpha";
constexpr int kCommandCount = 7;

enum CommandIndex : int {
    commandToggleEnabled = 0,
    commandPauseCurrentDocument,
    commandManualReplace,
    commandReload,
    commandManageReplacements,
    commandOpenDataFolder,
    commandAbout,
};

NppData gNppData;
FuncItem gFunctionItems[kCommandCount];
HINSTANCE gModule = nullptr;
ShortcutKey gManualShortcut{true, true, false, VK_SPACE};
RuleStore gRules;
PluginConfig gConfig;
std::filesystem::path gDataDirectory;
std::filesystem::path gConfigPath;
std::filesystem::path gReplacementsPath;
std::unordered_set<UINT_PTR> gPausedBuffers;
std::unordered_map<UINT_PTR, std::string> gExtensionCache;
struct TabstopSession {
    HWND editor = nullptr;
    std::vector<Sci_Position> positions;
    std::size_t current = 0;
};
std::unordered_map<UINT_PTR, TabstopSession> gTabstopSessions;
std::wstring gPunctuationCharacters;
bool gReady = false;
bool gInternalEdit = false;
bool gConfigValid = false;

const wchar_t* tr(const wchar_t* english, const wchar_t* korean) noexcept {
    return nppqr::localization::text(english, korean);
}

std::wstring utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string wideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

void logMessage(std::wstring_view level, std::wstring_view message) {
    const std::wstring debug = L"NppQuickReplace [" + std::wstring(level) + L"] " +
        std::wstring(message) + L"\n";
    ::OutputDebugStringW(debug.c_str());
    if (!gConfig.loggingEnabled || gDataDirectory.empty()) return;
    const std::filesystem::path path = gDataDirectory / "logs" / "NppQuickReplace.log";
    std::error_code fileError;
    std::filesystem::create_directories(path.parent_path(), fileError);
    if (fileError) return;
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (output) output << wideToUtf8(debug);
}

void showMessage(std::wstring_view message, UINT icon = MB_ICONINFORMATION) {
    ::MessageBoxW(gNppData._nppHandle, std::wstring(message).c_str(), kPluginName, MB_OK | icon);
}

void showError(std::string_view error) {
    showMessage(utf8ToWide(error), MB_ICONERROR);
}

std::wstring warningSummary(const std::vector<std::string>& warnings) {
    std::wstring result;
    const std::size_t shown = std::min<std::size_t>(warnings.size(), 5);
    for (std::size_t index = 0; index < shown; ++index) {
        result += L"\n• " + utf8ToWide(warnings[index]);
    }
    if (warnings.size() > shown) {
        result += tr(L"\n• …and ", L"\n• …외 ") +
            std::to_wstring(warnings.size() - shown) + tr(L" more", L"개");
    }
    return result;
}

HWND currentEditor() {
    int view = -1;
    ::SendMessageW(gNppData._nppHandle, NPPM_GETCURRENTSCINTILLA, 0,
        reinterpret_cast<LPARAM>(&view));
    if (view == 0) return gNppData._scintillaMainHandle;
    if (view == 1) return gNppData._scintillaSecondHandle;
    return nullptr;
}

UINT_PTR currentBufferId() {
    return static_cast<UINT_PTR>(
        ::SendMessageW(gNppData._nppHandle, NPPM_GETCURRENTBUFFERID, 0, 0));
}

std::filesystem::path filePathForBuffer(UINT_PTR bufferId) {
    if (bufferId == 0) return {};
    std::vector<wchar_t> path(32768, L'\0');
    const LRESULT length = ::SendMessageW(gNppData._nppHandle,
        NPPM_GETFULLPATHFROMBUFFERID, static_cast<WPARAM>(bufferId),
        reinterpret_cast<LPARAM>(path.data()));
    if (length < 0 || path.front() == L'\0') return {};
    return std::filesystem::path(path.data());
}

void refreshBufferContext(UINT_PTR bufferId) {
    if (bufferId == 0) return;
    const std::wstring extension = filePathForBuffer(bufferId).extension().wstring();
    gExtensionCache[bufferId] = RuleStore::foldAscii(wideToUtf8(extension));
}

std::string currentFileExtension() {
    const UINT_PTR bufferId = currentBufferId();
    const auto existing = gExtensionCache.find(bufferId);
    if (existing != gExtensionCache.end()) return existing->second;
    refreshBufferContext(bufferId);
    const auto refreshed = gExtensionCache.find(bufferId);
    return refreshed == gExtensionCache.end() ? std::string{} : refreshed->second;
}

std::string currentFilePathUtf8() {
    return RuleStore::foldAscii(wideToUtf8(filePathForBuffer(currentBufferId()).wstring()));
}

std::string currentLanguageName() {
    LangType language = L_TEXT;
    if (::SendMessageW(gNppData._nppHandle, NPPM_GETCURRENTLANGTYPE, 0,
            reinterpret_cast<LPARAM>(&language)) == FALSE) {
        return {};
    }
    const LRESULT length = ::SendMessageW(
        gNppData._nppHandle, NPPM_GETLANGUAGENAME, static_cast<WPARAM>(language), 0);
    if (length <= 0 || length > 1024) return {};
    std::wstring name(static_cast<std::size_t>(length) + 1, L'\0');
    ::SendMessageW(gNppData._nppHandle, NPPM_GETLANGUAGENAME,
        static_cast<WPARAM>(language), reinterpret_cast<LPARAM>(name.data()));
    name.resize(static_cast<std::size_t>(length));
    return RuleStore::foldAscii(wideToUtf8(name));
}
bool isCurrentBufferPaused() {
    const UINT_PTR bufferId = currentBufferId();
    return bufferId != 0 && gPausedBuffers.contains(bufferId);
}

void setMenuCheck(CommandIndex command, bool checked) {
    const int commandId = gFunctionItems[command]._cmdID;
    if (commandId != 0) {
        ::SendMessageW(gNppData._nppHandle, NPPM_SETMENUITEMCHECK,
            static_cast<WPARAM>(commandId), static_cast<LPARAM>(checked));
    }
}

void refreshMenuChecks() {
    setMenuCheck(commandToggleEnabled, gConfig.pluginEnabled);
    setMenuCheck(commandPauseCurrentDocument, isCurrentBufferPaused());
}

UINT editorCodePage(HWND editor) {
    const LRESULT codePage = ::SendMessageW(editor, SCI_GETCODEPAGE, 0, 0);
    return codePage > 0 ? static_cast<UINT>(codePage) : CP_ACP;
}

bool documentBytesToWide(HWND editor, std::string_view bytes, std::wstring& result) {
    if (bytes.empty()) {
        result.clear();
        return true;
    }
    const UINT codePage = editorCodePage(editor);
    const DWORD flags = codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    const int length = ::MultiByteToWideChar(codePage, flags, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) return false;
    result.assign(static_cast<std::size_t>(length), L'\0');
    return ::MultiByteToWideChar(codePage, flags, bytes.data(),
        static_cast<int>(bytes.size()), result.data(), length) > 0;
}

bool wideToDocumentBytes(HWND editor, std::wstring_view text, std::string& result) {
    if (text.empty()) {
        result.clear();
        return true;
    }
    const UINT codePage = editorCodePage(editor);
    const DWORD flags = codePage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL usedDefault = FALSE;
    BOOL* usedDefaultPointer = codePage == CP_UTF8 ? nullptr : &usedDefault;
    const int length = ::WideCharToMultiByte(codePage, flags, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, usedDefaultPointer);
    if (length <= 0 || usedDefault) return false;
    result.assign(static_cast<std::size_t>(length), '\0');
    usedDefault = FALSE;
    const int converted = ::WideCharToMultiByte(codePage, flags, text.data(),
        static_cast<int>(text.size()), result.data(), length, nullptr, usedDefaultPointer);
    return converted > 0 && !usedDefault;
}

std::wstring normalizeNfc(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = ::NormalizeString(NormalizationC, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return std::wstring(text);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    const int converted = ::NormalizeString(NormalizationC, text.data(),
        static_cast<int>(text.size()), result.data(), length);
    if (converted <= 0) return std::wstring(text);
    result.resize(static_cast<std::size_t>(converted));
    return result;
}

bool isBoundaryCharacter(wchar_t character) {
    return ::iswspace(character) != 0 ||
        gPunctuationCharacters.find(character) != std::wstring::npos;
}

struct TriggerRange {
    Sci_Position start = 0;
    Sci_Position end = 0;
    std::string text;
};

std::optional<TriggerRange> readTriggerBefore(HWND editor, Sci_Position end) {
    if (editor == nullptr || end <= 0) return std::nullopt;
    const Sci_Position configuredLimit = static_cast<Sci_Position>(gConfig.maxTriggerBytes);
    const Sci_Position candidate = std::max<Sci_Position>(0, end - configuredLimit);
    Sci_Position rangeStart = 0;
    if (candidate > 0) {
        rangeStart = static_cast<Sci_Position>(::SendMessageW(editor, SCI_POSITIONBEFORE,
            static_cast<WPARAM>(candidate + 1), 0));
        if (rangeStart < 0) rangeStart = candidate;
    }
    const std::size_t byteLength = static_cast<std::size_t>(end - rangeStart);
    std::string bytes(byteLength + 1, '\0');
    Sci_TextRangeFull range{{rangeStart, end}, bytes.data()};
    ::SendMessageW(editor, SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<LPARAM>(&range));
    bytes.resize(byteLength);

    std::wstring wide;
    if (!documentBytesToWide(editor, bytes, wide)) {
        logMessage(L"warning", L"Skipped a trigger that could not be decoded from the document code page.");
        return std::nullopt;
    }
    std::size_t triggerStart = 0;
    for (std::size_t index = wide.size(); index > 0; --index) {
        if (isBoundaryCharacter(wide[index - 1])) {
            triggerStart = index;
            break;
        }
    }
    if (triggerStart == 0 && rangeStart > 0) return std::nullopt;
    const std::wstring triggerWide = wide.substr(triggerStart);
    if (triggerWide.empty()) return std::nullopt;

    std::string triggerDocumentBytes;
    if (!wideToDocumentBytes(editor, triggerWide, triggerDocumentBytes)) return std::nullopt;
    const std::wstring normalized = normalizeNfc(triggerWide);
    return TriggerRange{
        end - static_cast<Sci_Position>(triggerDocumentBytes.size()),
        end,
        wideToUtf8(normalized),
    };
}

Sci_Position delimiterStartFor(HWND editor, Sci_Position caret, int character) {
    Sci_Position start = static_cast<Sci_Position>(
        ::SendMessageW(editor, SCI_POSITIONBEFORE, static_cast<WPARAM>(caret), 0));
    if (start < 0) return caret;
    if (character == '\n' && start > 0) {
        const int atStart = static_cast<int>(
            ::SendMessageW(editor, SCI_GETCHARAT, static_cast<WPARAM>(start), 0));
        const Sci_Position previous = static_cast<Sci_Position>(
            ::SendMessageW(editor, SCI_POSITIONBEFORE, static_cast<WPARAM>(start), 0));
        if ((atStart & 0xFF) == '\n' && previous >= 0) {
            const int atPrevious = static_cast<int>(
                ::SendMessageW(editor, SCI_GETCHARAT, static_cast<WPARAM>(previous), 0));
            if ((atPrevious & 0xFF) == '\r') start = previous;
        }
    }
    return start;
}

std::string normalizeEol(std::string_view text, int eolMode) {
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') ++index;
            normalized.push_back('\n');
        } else {
            normalized.push_back(text[index]);
        }
    }
    if (eolMode == SC_EOL_LF) return normalized;
    std::string result;
    result.reserve(normalized.size() + normalized.size() / 8);
    for (const char character : normalized) {
        if (character == '\n') {
            if (eolMode == SC_EOL_CRLF) result.append("\r\n");
            else result.push_back('\r');
        } else {
            result.push_back(character);
        }
    }
    return result;
}


bool replaceAllBounded(
    std::string& text,
    std::string_view marker,
    std::string_view value,
    std::size_t maxSize) {
    if (marker.empty() || text.size() > maxSize) return false;
    std::size_t count = 0;
    for (std::size_t position = text.find(marker); position != std::string::npos;
         position = text.find(marker, position + marker.size())) {
        ++count;
    }
    if (value.size() > marker.size()) {
        const std::size_t growth = value.size() - marker.size();
        if (count > (maxSize - text.size()) / growth) return false;
    }
    std::size_t position = text.find(marker);
    while (position != std::string::npos) {
        text.replace(position, marker.size(), value);
        position = text.find(marker, position + value.size());
    }
    return true;
}

std::optional<std::string> clipboardTextUtf8(std::wstring& error) {
    if (!::OpenClipboard(gNppData._nppHandle)) {
        error = L"Skipped a replacement because the clipboard could not be opened.";
        return std::nullopt;
    }
    struct ClipboardCloser {
        ~ClipboardCloser() { ::CloseClipboard(); }
    } closer;
    HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) {
        error = L"Skipped a replacement because the clipboard does not contain Unicode text.";
        return std::nullopt;
    }
    const SIZE_T allocationBytes = ::GlobalSize(data);
    if (allocationBytes > (gConfig.maxExpandedBytes + 1U) * sizeof(wchar_t)) {
        error = L"Skipped a replacement because the clipboard exceeds maxExpandedBytes.";
        return std::nullopt;
    }
    const auto* text = static_cast<const wchar_t*>(::GlobalLock(data));
    if (text == nullptr) {
        error = L"Skipped a replacement because the clipboard text could not be locked.";
        return std::nullopt;
    }
    const std::wstring value(text);
    ::GlobalUnlock(data);
    std::string result = wideToUtf8(value);
    if (result.size() > gConfig.maxExpandedBytes) {
        error = L"Skipped a replacement because the UTF-8 clipboard text exceeds maxExpandedBytes.";
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> selectionTextUtf8(
    HWND editor, int selectionIndex, std::wstring& error) {
    const int resolvedIndex = selectionIndex >= 0 ? selectionIndex : static_cast<int>(
        ::SendMessageW(editor, SCI_GETMAINSELECTION, 0, 0));
    const Sci_Position start = static_cast<Sci_Position>(::SendMessageW(
        editor, SCI_GETSELECTIONNSTART, static_cast<WPARAM>(resolvedIndex), 0));
    const Sci_Position end = static_cast<Sci_Position>(::SendMessageW(
        editor, SCI_GETSELECTIONNEND, static_cast<WPARAM>(resolvedIndex), 0));
    if (end <= start) return std::string{};
    const std::size_t byteCount = static_cast<std::size_t>(end - start);
    if (byteCount > gConfig.maxExpandedBytes) {
        error = L"Skipped a replacement because the selected text exceeds maxExpandedBytes.";
        return std::nullopt;
    }
    std::string bytes(byteCount + 1, '\0');
    Sci_TextRangeFull range{{start, end}, bytes.data()};
    ::SendMessageW(editor, SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<LPARAM>(&range));
    bytes.resize(byteCount);
    std::wstring wide;
    if (!documentBytesToWide(editor, bytes, wide)) {
        error = L"Skipped a replacement because the selected text could not be decoded.";
        return std::nullopt;
    }
    std::string utf8 = wideToUtf8(wide);
    if (utf8.size() > gConfig.maxExpandedBytes) {
        error = L"Skipped a replacement because the selected text exceeds maxExpandedBytes.";
        return std::nullopt;
    }
    return utf8;
}

std::string makeUuidUtf8() {
    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid))) return {};
    wchar_t buffer[40]{};
    ::StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    std::wstring value(buffer);
    if (!value.empty() && value.front() == L'{') value.erase(value.begin());
    if (!value.empty() && value.back() == L'}') value.pop_back();
    return wideToUtf8(value);
}
std::optional<std::string> expandBuiltInVariables(
    HWND editor, std::string source, const CaptureMatch& captures,
    int selectionIndex, std::wstring& error) {
    if (source.size() > gConfig.maxExpandedBytes) {
        error = L"Skipped a replacement because its template exceeds maxExpandedBytes.";
        return std::nullopt;
    }
    const auto replaceVariable = [&](std::string_view marker, std::string_view value) {
        if (replaceAllBounded(source, marker, value, gConfig.maxExpandedBytes)) return true;
        error = L"Skipped a replacement because repeated variables exceed maxExpandedBytes.";
        return false;
    };
    SYSTEMTIME time{};
    ::GetLocalTime(&time);
    wchar_t date[16]{};
    wchar_t clock[16]{};
    ::swprintf_s(date, L"%04u-%02u-%02u", time.wYear, time.wMonth, time.wDay);
    ::swprintf_s(clock, L"%02u:%02u:%02u", time.wHour, time.wMinute, time.wSecond);
    if (!replaceVariable("${date}", wideToUtf8(date)) ||
        !replaceVariable("${time}", wideToUtf8(clock))) {
        return std::nullopt;
    }
    if (source.find("${uuid}") != std::string::npos &&
        !replaceVariable("${uuid}", makeUuidUtf8())) {
        return std::nullopt;
    }
    const Sci_Position caret = selectionIndex >= 0
        ? static_cast<Sci_Position>(::SendMessageW(editor, SCI_GETSELECTIONNCARET,
            static_cast<WPARAM>(selectionIndex), 0))
        : static_cast<Sci_Position>(::SendMessageW(editor, SCI_GETCURRENTPOS, 0, 0));
    const LRESULT line = ::SendMessageW(editor, SCI_LINEFROMPOSITION, static_cast<WPARAM>(caret), 0);
    const LRESULT column = ::SendMessageW(editor, SCI_GETCOLUMN, static_cast<WPARAM>(caret), 0);
    if (!replaceVariable("${line}", std::to_string(line + 1)) ||
        !replaceVariable("${column}", std::to_string(column + 1))) {
        return std::nullopt;
    }

    const std::filesystem::path path = filePathForBuffer(currentBufferId());
    if (!replaceVariable("${filename}", wideToUtf8(path.filename().wstring())) ||
        !replaceVariable("${filepath}", wideToUtf8(path.wstring()))) {
        return std::nullopt;
    }
    if (source.find("${clipboard}") != std::string::npos) {
        const auto clipboard = clipboardTextUtf8(error);
        if (!clipboard.has_value()) return std::nullopt;
        if (!replaceVariable("${clipboard}", *clipboard)) return std::nullopt;
    }
    if (source.find("${selection}") != std::string::npos) {
        const auto selection = selectionTextUtf8(editor, selectionIndex, error);
        if (!selection.has_value()) return std::nullopt;
        if (!replaceVariable("${selection}", *selection)) return std::nullopt;
    }
    for (std::size_t number = 1; number < captures.values.size(); ++number) {
        const std::string marker = "${capture:" + std::to_string(number) + "}";
        if (!replaceVariable(marker, captures.values[number])) return std::nullopt;
    }
    if (source.size() > gConfig.maxExpandedBytes) {
        error = L"Skipped a replacement because expanded variables exceed maxExpandedBytes.";
        return std::nullopt;
    }
    return source;
}
struct ExpandedReplacement {
    std::string documentBytes;
    std::optional<std::size_t> cursorByteOffset;
    std::vector<std::size_t> tabstopByteOffsets;
};

std::optional<ExpandedReplacement> expandReplacement(
    HWND editor, std::string source, const CaptureMatch& captures,
    int selectionIndex, std::wstring& error) {
    const auto expanded = expandBuiltInVariables(
        editor, std::move(source), captures, selectionIndex, error);
    if (!expanded.has_value()) return std::nullopt;
    const nppqr::SnippetExpansion snippet = nppqr::parseSnippetMarkers(*expanded);
    source = snippet.text;
    const int eolMode = static_cast<int>(::SendMessageW(editor, SCI_GETEOLMODE, 0, 0));
    const std::string normalized = normalizeEol(source, eolMode);
    if (normalized.size() > gConfig.maxExpandedBytes) {
        error = L"Skipped a replacement because normalized output exceeds maxExpandedBytes.";
        return std::nullopt;
    }
    const std::wstring wide = normalizeNfc(utf8ToWide(normalized));
    ExpandedReplacement result;
    if (!wideToDocumentBytes(editor, wide, result.documentBytes)) {
        error = L"Skipped a replacement that cannot be represented in the document encoding.";
        return std::nullopt;
    }
    if (result.documentBytes.size() > gConfig.maxExpandedBytes) {
        error = L"Skipped a replacement because encoded output exceeds maxExpandedBytes.";
        return std::nullopt;
    }

    const auto encodeOffset = [&](std::size_t utf8Offset) -> std::optional<std::size_t> {
        const std::string beforeNormalized = normalizeEol(source.substr(0, utf8Offset), eolMode);
        std::string beforeBytes;
        if (!wideToDocumentBytes(
                editor, normalizeNfc(utf8ToWide(beforeNormalized)), beforeBytes)) {
            return std::nullopt;
        }
        return beforeBytes.size();
    };
    if (snippet.cursorOffset.has_value()) {
        result.cursorByteOffset = encodeOffset(*snippet.cursorOffset);
        if (!result.cursorByteOffset.has_value()) {
            error = L"Skipped a replacement with an unrepresentable cursor position.";
            return std::nullopt;
        }
    }
    result.tabstopByteOffsets.reserve(snippet.tabstopOffsets.size());
    for (const std::size_t offset : snippet.tabstopOffsets) {
        const auto encoded = encodeOffset(offset);
        if (!encoded.has_value()) {
            error = L"Skipped a replacement with an unrepresentable tabstop position.";
            return std::nullopt;
        }
        result.tabstopByteOffsets.push_back(*encoded);
    }
    return result;
}

class ScopedUndoAction {
public:
    explicit ScopedUndoAction(HWND editor) : editor_(editor) {
        ::SendMessageW(editor_, SCI_BEGINUNDOACTION, 0, 0);
    }
    ~ScopedUndoAction() {
        ::SendMessageW(editor_, SCI_ENDUNDOACTION, 0, 0);
    }
private:
    HWND editor_;
};

class ScopedInternalEdit {
public:
    ScopedInternalEdit() : previous_(gInternalEdit) { gInternalEdit = true; }
    ~ScopedInternalEdit() { gInternalEdit = previous_; }
private:
    bool previous_;
};

void adjustTabstopPositions(
    TabstopSession& session, Sci_Position position, Sci_Position length, bool insertion) {
    if (length <= 0) return;
    const Sci_Position end = position + length;
    for (Sci_Position& tabstop : session.positions) {
        if (insertion) {
            if (tabstop >= position) tabstop += length;
        } else if (tabstop >= end) {
            tabstop -= length;
        } else if (tabstop >= position) {
            tabstop = position;
        }
    }
}

void updateTabstopSession(const SCNotification& notification) {
    if (gInternalEdit ||
        (notification.modificationType & (SC_MOD_BEFOREINSERT | SC_MOD_BEFOREDELETE)) != 0) {
        return;
    }
    const UINT_PTR bufferId = currentBufferId();
    const auto existing = gTabstopSessions.find(bufferId);
    if (existing == gTabstopSessions.end()) return;
    TabstopSession& session = existing->second;
    if (session.editor != static_cast<HWND>(notification.nmhdr.hwndFrom)) return;
    if ((notification.modificationType & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO)) != 0) {
        gTabstopSessions.erase(existing);
        return;
    }
    if ((notification.modificationType & SC_MOD_INSERTTEXT) != 0) {
        adjustTabstopPositions(session, notification.position, notification.length, true);
    } else if ((notification.modificationType & SC_MOD_DELETETEXT) != 0) {
        adjustTabstopPositions(session, notification.position, notification.length, false);
    }
}

bool navigateTabstop(const SCNotification& notification) {
    if (notification.ch != '\t' || (notification.modifiers & SCMOD_SHIFT) != 0) return false;
    HWND editor = static_cast<HWND>(notification.nmhdr.hwndFrom);
    const UINT_PTR bufferId = currentBufferId();
    const auto existing = gTabstopSessions.find(bufferId);
    if (existing == gTabstopSessions.end() || existing->second.editor != editor) return false;
    TabstopSession& session = existing->second;
    if (session.current >= session.positions.size()) {
        gTabstopSessions.erase(existing);
        return false;
    }
    const Sci_Position caret = static_cast<Sci_Position>(
        ::SendMessageW(editor, SCI_GETCURRENTPOS, 0, 0));
    const Sci_Position insertedTab = caret - 1;
    if (insertedTab < 0 || ::SendMessageW(
            editor, SCI_GETCHARAT, static_cast<WPARAM>(insertedTab), 0) != '\t') {
        gTabstopSessions.erase(existing);
        return false;
    }
    if (session.current + 1 >= session.positions.size()) {
        gTabstopSessions.erase(existing);
        return true;
    }

    {
        ScopedUndoAction undo(editor);
        ScopedInternalEdit internalEdit;
        ::SendMessageW(editor, SCI_DELETERANGE,
            static_cast<WPARAM>(insertedTab), static_cast<LPARAM>(1));
    }
    adjustTabstopPositions(session, insertedTab, 1, false);
    ++session.current;
    ::SendMessageW(editor, SCI_SETEMPTYSELECTION,
        static_cast<WPARAM>(session.positions[session.current]), 0);
    return true;
}

void validateTabstopSelection(const SCNotification& notification) {
    if (gInternalEdit || (notification.updated & SC_UPDATE_SELECTION) == 0) return;
    HWND editor = static_cast<HWND>(notification.nmhdr.hwndFrom);
    const UINT_PTR bufferId = currentBufferId();
    const auto existing = gTabstopSessions.find(bufferId);
    if (existing == gTabstopSessions.end() || existing->second.editor != editor) return;
    const TabstopSession& session = existing->second;
    if (session.current >= session.positions.size()) {
        gTabstopSessions.erase(existing);
        return;
    }
    const Sci_Position caret = static_cast<Sci_Position>(
        ::SendMessageW(editor, SCI_GETCURRENTPOS, 0, 0));
    if (caret != session.positions[session.current]) gTabstopSessions.erase(existing);
}

void applyReplacement(HWND editor, const TriggerRange& trigger,
    const ReplacementRule& rule, Sci_Position trailingDelimiterBytes,
    const CaptureMatch& captures = {}, int selectionIndex = -1) {
    std::wstring expansionError;
    const auto replacement = expandReplacement(
        editor, rule.replacement, captures, selectionIndex, expansionError);
    if (!replacement.has_value()) {
        logMessage(L"warning", expansionError.empty() ? L"Skipped an invalid replacement." : expansionError);
        return;
    }
    ScopedUndoAction undo(editor);
    ScopedInternalEdit internalEdit;
    ::SendMessageW(editor, SCI_SETTARGETRANGE,
        static_cast<WPARAM>(trigger.start), static_cast<LPARAM>(trigger.end));
    ::SendMessageW(editor, SCI_REPLACETARGET,
        static_cast<WPARAM>(replacement->documentBytes.size()),
        reinterpret_cast<LPARAM>(replacement->documentBytes.data()));
    const UINT_PTR bufferId = currentBufferId();
    gTabstopSessions.erase(bufferId);
    Sci_Position newCaret = replacement->cursorByteOffset.has_value()
        ? trigger.start + static_cast<Sci_Position>(*replacement->cursorByteOffset)
        : trigger.start + static_cast<Sci_Position>(replacement->documentBytes.size()) +
            trailingDelimiterBytes;
    if (!replacement->tabstopByteOffsets.empty() && bufferId != 0) {
        TabstopSession session;
        session.editor = editor;
        session.positions.reserve(replacement->tabstopByteOffsets.size());
        for (const std::size_t offset : replacement->tabstopByteOffsets) {
            session.positions.push_back(trigger.start + static_cast<Sci_Position>(offset));
        }
        newCaret = session.positions.front();
        gTabstopSessions.insert_or_assign(bufferId, std::move(session));
    }
    ::SendMessageW(editor, SCI_SETEMPTYSELECTION, static_cast<WPARAM>(newCaret), 0);
}

struct PreparedMultiReplacement {
    TriggerRange trigger;
    ExpandedReplacement replacement;
    Sci_Position trailingDelimiterBytes = 0;
    int selectionIndex = 0;
};

bool replaceMultipleSelections(
    HWND editor, Activation activation, bool immediate, int delimiterCharacter, bool manual) {
    const int selectionCount = static_cast<int>(::SendMessageW(editor, SCI_GETSELECTIONS, 0, 0));
    if (selectionCount <= 1) return false;
    if (selectionCount > 256) {
        logMessage(L"warning", L"Skipped multi-cursor replacement with more than 256 carets.");
        return true;
    }
    const int mainSelection = static_cast<int>(
        ::SendMessageW(editor, SCI_GETMAINSELECTION, 0, 0));
    const std::string extension = currentFileExtension();
    const std::string path = currentFilePathUtf8();
    const std::string language = currentLanguageName();
    std::vector<PreparedMultiReplacement> operations;
    operations.reserve(static_cast<std::size_t>(selectionCount));
    std::size_t totalOutputBytes = 0;

    for (int selection = 0; selection < selectionCount; ++selection) {
        const Sci_Position caret = static_cast<Sci_Position>(::SendMessageW(
            editor, SCI_GETSELECTIONNCARET, static_cast<WPARAM>(selection), 0));
        const Sci_Position triggerEnd = manual || immediate
            ? caret
            : delimiterStartFor(editor, caret, delimiterCharacter);
        const auto trigger = readTriggerBefore(editor, triggerEnd);
        if (!trigger.has_value()) {
            logMessage(L"warning", L"Skipped multi-cursor replacement because a caret has no trigger.");
            return true;
        }

        CaptureMatch captures;
        const ReplacementRule* rule = nullptr;
        if (manual) {
            rule = gRules.findManual(trigger->text, extension, path, language);
            if (rule == nullptr) {
                rule = gRules.findCaptureTemplate(trigger->text, Activation::none,
                    extension, path, language, captures, true);
            }
        } else if (immediate) {
            rule = gRules.findImmediate(trigger->text, extension, path, language);
        } else {
            rule = gRules.find(trigger->text, activation, extension, path, language);
            if (rule == nullptr) {
                rule = gRules.findCaptureTemplate(trigger->text, activation,
                    extension, path, language, captures);
            }
        }
        if (rule == nullptr) {
            logMessage(L"warning", L"Skipped multi-cursor replacement because not every caret matched a rule.");
            return true;
        }

        std::wstring expansionError;
        auto replacement = expandReplacement(
            editor, rule->replacement, captures, selection, expansionError);
        if (!replacement.has_value()) {
            logMessage(L"warning", expansionError.empty()
                ? L"Skipped an invalid multi-cursor replacement." : expansionError);
            return true;
        }
        if (replacement->documentBytes.size() > gConfig.maxExpandedBytes - totalOutputBytes) {
            logMessage(L"warning",
                L"Skipped multi-cursor replacement because aggregate output exceeds maxExpandedBytes.");
            return true;
        }
        totalOutputBytes += replacement->documentBytes.size();
        operations.push_back({*trigger, std::move(*replacement),
            manual || immediate ? 0 : caret - triggerEnd, selection});
    }

    std::sort(operations.begin(), operations.end(),
        [](const PreparedMultiReplacement& left, const PreparedMultiReplacement& right) {
            return left.trigger.start > right.trigger.start;
        });
    for (std::size_t index = 1; index < operations.size(); ++index) {
        if (operations[index].trigger.end > operations[index - 1].trigger.start) {
            logMessage(L"warning", L"Skipped overlapping multi-cursor trigger ranges.");
            return true;
        }
    }

    std::vector<std::pair<int, Sci_Position>> carets;
    carets.reserve(operations.size());
    gTabstopSessions.erase(currentBufferId());
    {
        ScopedUndoAction undo(editor);
        ScopedInternalEdit internalEdit;
        for (const PreparedMultiReplacement& operation : operations) {
            const Sci_Position removed = operation.trigger.end - operation.trigger.start;
            const Sci_Position inserted = static_cast<Sci_Position>(
                operation.replacement.documentBytes.size());
            const Sci_Position delta = inserted - removed;
            for (auto& [selection, caret] : carets) {
                static_cast<void>(selection);
                if (caret >= operation.trigger.end) caret += delta;
            }
            ::SendMessageW(editor, SCI_SETTARGETRANGE,
                static_cast<WPARAM>(operation.trigger.start),
                static_cast<LPARAM>(operation.trigger.end));
            ::SendMessageW(editor, SCI_REPLACETARGET,
                static_cast<WPARAM>(operation.replacement.documentBytes.size()),
                reinterpret_cast<LPARAM>(operation.replacement.documentBytes.data()));
            Sci_Position caret = operation.trigger.start + inserted +
                operation.trailingDelimiterBytes;
            if (!operation.replacement.tabstopByteOffsets.empty()) {
                caret = operation.trigger.start + static_cast<Sci_Position>(
                    operation.replacement.tabstopByteOffsets.front());
            } else if (operation.replacement.cursorByteOffset.has_value()) {
                caret = operation.trigger.start + static_cast<Sci_Position>(
                    *operation.replacement.cursorByteOffset);
            }
            carets.emplace_back(operation.selectionIndex, caret);
        }

        const auto main = std::find_if(carets.begin(), carets.end(),
            [&](const auto& entry) { return entry.first == mainSelection; });
        if (main == carets.end()) return true;
        ::SendMessageW(editor, SCI_CLEARSELECTIONS, 0, 0);
        ::SendMessageW(editor, SCI_SETEMPTYSELECTION, static_cast<WPARAM>(main->second), 0);
        for (const auto& [selection, caret] : carets) {
            if (selection == mainSelection) continue;
            ::SendMessageW(editor, SCI_ADDSELECTION,
                static_cast<WPARAM>(caret), static_cast<LPARAM>(caret));
        }
        ::SendMessageW(editor, SCI_SETMAINSELECTION, 0, 0);
    }
    return true;
}

void updatePunctuationCache() {
    gPunctuationCharacters = normalizeNfc(utf8ToWide(gConfig.punctuationTriggers));
}

bool loadRules(bool showResult) {
    const nppqr::RuleLoadResult primary = gRules.loadFromFile(gReplacementsPath);
    if (primary.ok) {
        if (showResult) {
            std::wstring message = tr(L"Loaded ", L"치환 규칙 ") +
                std::to_wstring(primary.loadedCount) + tr(L" replacement rules.", L"개를 불러왔어요.");
            if (!primary.warnings.empty()) {
                message += tr(L"\n\nWarnings:", L"\n\n경고:") + warningSummary(primary.warnings);
            }
            showMessage(message, primary.warnings.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
        }
        if (!primary.warnings.empty()) {
            logMessage(L"warning", L"Rules loaded with " + std::to_wstring(primary.warnings.size()) + L" warning(s).");
        }
        return true;
    }

    for (const auto& backup : ConfigStore::replacementBackupsNewestFirst(gDataDirectory)) {
        const nppqr::RuleLoadResult recovered = gRules.loadFromFile(backup);
        if (recovered.ok) {
            showMessage(std::wstring(tr(
                L"replacements.json could not be loaded.\n\nThe newest valid backup was loaded in memory instead. The original file was not overwritten.\n\n",
                L"replacements.json을 불러올 수 없어요.\n\n대신 가장 최근의 유효한 백업을 메모리에 불러왔어요. 원본 파일은 덮어쓰지 않았어요.\n\n")) +
                utf8ToWide(primary.error), MB_ICONWARNING);
            logMessage(L"error", L"Loaded rules from backup after replacements.json failed validation.");
            return true;
        }
    }
    showMessage(std::wstring(tr(
        L"No replacement rules were loaded. The existing file was left unchanged.\n\n",
        L"치환 규칙을 불러오지 못했어요. 기존 파일은 변경하지 않았어요.\n\n")) +
        utf8ToWide(primary.error), MB_ICONERROR);
    return false;
}

bool loadConfiguration(bool showWarnings) {
    PluginConfig loaded;
    const nppqr::ConfigLoadResult result = ConfigStore::loadConfig(gConfigPath, loaded);
    gConfigValid = result.ok;
    if (!result.ok) {
        gConfig = PluginConfig{};
        updatePunctuationCache();
        showMessage(std::wstring(tr(
            L"config.json is invalid. Safe in-memory defaults are active, and the existing file was not overwritten.\n\n",
            L"config.json이 올바르지 않아요. 안전한 기본 설정을 메모리에서 사용하며 기존 파일은 덮어쓰지 않았어요.\n\n")) +
            utf8ToWide(result.error), MB_ICONWARNING);
        return false;
    }
    gConfig = std::move(loaded);
    updatePunctuationCache();
    if (showWarnings && !result.warnings.empty()) {
        showMessage(std::wstring(tr(L"Configuration loaded with warnings:",
            L"설정을 경고와 함께 불러왔어요:")) + warningSummary(result.warnings), MB_ICONWARNING);
    }
    return true;
}

bool initializePlugin() {
    std::vector<wchar_t> configDirectory(32768, L'\0');
    const LRESULT length = ::SendMessageW(gNppData._nppHandle, NPPM_GETPLUGINSCONFIGDIR,
        static_cast<WPARAM>(configDirectory.size()), reinterpret_cast<LPARAM>(configDirectory.data()));
    if (length <= 0 || configDirectory.front() == L'\0') {
        showMessage(tr(L"Notepad++ did not provide a plugin configuration directory.",
            L"Notepad++에서 플러그인 설정 폴더를 제공하지 않았어요."), MB_ICONERROR);
        return false;
    }
    gDataDirectory = std::filesystem::path(configDirectory.data()) / kPluginName;
    gConfigPath = gDataDirectory / "config.json";
    gReplacementsPath = gDataDirectory / "replacements.json";
    std::string error;
    if (!ConfigStore::ensureDataFiles(gDataDirectory, error)) {
        showError(error);
        return false;
    }
    loadConfiguration(true);
    loadRules(false);
    refreshBufferContext(currentBufferId());
    gReady = true;
    refreshMenuChecks();
    return true;
}

void saveEnabledState() {
    if (!gConfigValid || !gConfig.rememberEnabledState || gConfigPath.empty()) return;
    std::string error;
    if (!ConfigStore::saveConfigAtomic(gConfigPath, gConfig, error)) {
        logMessage(L"error", L"Could not save enabled state: " + utf8ToWide(error));
    }
}

Activation activationForCharacter(int character) {
    const Activation basic = RuleStore::activationForCharacter(character, gConfig.punctuationTriggers);
    if (basic != Activation::none) return basic;
    if (character > 0 && character <= 0xFFFF &&
        gPunctuationCharacters.find(static_cast<wchar_t>(character)) != std::wstring::npos) {
        return Activation::punctuation;
    }
    return Activation::none;
}

void processCharacterAdded(const SCNotification& notification) {
    if (gInternalEdit) return;
    if (navigateTabstop(notification)) return;
    if (!gReady || !gConfig.pluginEnabled || isCurrentBufferPaused()) return;
    if (notification.characterSource == SC_CHARACTERSOURCE_TENTATIVE_INPUT) return;
    HWND editor = static_cast<HWND>(notification.nmhdr.hwndFrom);
    if (editor != gNppData._scintillaMainHandle && editor != gNppData._scintillaSecondHandle) return;
    if (gConfig.skipReadOnlyDocuments && ::SendMessageW(editor, SCI_GETREADONLY, 0, 0) != 0) return;
    const int selectionCount = static_cast<int>(
        ::SendMessageW(editor, SCI_GETSELECTIONS, 0, 0));
    if (gConfig.skipMultiSelection && selectionCount > 1) return;

    Activation activation = activationForCharacter(notification.ch);
    if (activation == Activation::none && !gRules.hasImmediateRules()) return;
    const Sci_Position caret = static_cast<Sci_Position>(
        ::SendMessageW(editor, SCI_GETCURRENTPOS, 0, 0));
    const bool immediate = activation == Activation::none;
    if (immediate) activation = Activation::immediate;
    if (selectionCount > 1) {
        replaceMultipleSelections(editor, activation, immediate, notification.ch, false);
        return;
    }
    const Sci_Position triggerEnd = immediate
        ? caret
        : delimiterStartFor(editor, caret, notification.ch);
    const auto trigger = readTriggerBefore(editor, triggerEnd);
    if (!trigger.has_value()) return;
    const std::string extension = currentFileExtension();
    const std::string path = currentFilePathUtf8();
    const std::string language = currentLanguageName();
    CaptureMatch captures;
    const ReplacementRule* rule = immediate
        ? gRules.findImmediate(trigger->text, extension, path, language)
        : gRules.find(trigger->text, activation, extension, path, language);
    if (rule == nullptr && !immediate) {
        rule = gRules.findCaptureTemplate(
            trigger->text, activation, extension, path, language, captures);
    }
    if (rule == nullptr) return;
    applyReplacement(editor, *trigger, *rule, immediate ? 0 : caret - triggerEnd, captures);
}

void processPastedText(const SCNotification& notification) {
    if (!gConfig.processPaste || gInternalEdit || notification.text == nullptr ||
        notification.length <= 1 ||
        (notification.modificationType & SC_MOD_INSERTTEXT) == 0) {
        return;
    }
    const unsigned char last = static_cast<unsigned char>(notification.text[notification.length - 1]);
    if (last > 0x7FU || activationForCharacter(last) == Activation::none) return;
    SCNotification synthetic = notification;
    synthetic.ch = last;
    synthetic.characterSource = SC_CHARACTERSOURCE_DIRECT_INPUT;
    processCharacterAdded(synthetic);
}
void toggleEnabled() {
    gConfig.pluginEnabled = !gConfig.pluginEnabled;
    refreshMenuChecks();
    saveEnabledState();
}

void togglePauseCurrentDocument() {
    const UINT_PTR bufferId = currentBufferId();
    if (bufferId == 0) return;
    if (!gPausedBuffers.erase(bufferId)) gPausedBuffers.insert(bufferId);
    refreshMenuChecks();
}

void manualReplace() {
    if (!gReady) return;
    HWND editor = currentEditor();
    if (editor == nullptr || ::SendMessageW(editor, SCI_GETREADONLY, 0, 0) != 0) return;
    const int selectionCount = static_cast<int>(
        ::SendMessageW(editor, SCI_GETSELECTIONS, 0, 0));
    if (gConfig.skipMultiSelection && selectionCount > 1) return;
    if (selectionCount > 1) {
        replaceMultipleSelections(editor, Activation::none, false, 0, true);
        return;
    }
    const Sci_Position caret = static_cast<Sci_Position>(
        ::SendMessageW(editor, SCI_GETCURRENTPOS, 0, 0));
    const auto trigger = readTriggerBefore(editor, caret);
    if (!trigger.has_value()) return;
    const std::string extension = currentFileExtension();
    const std::string path = currentFilePathUtf8();
    const std::string language = currentLanguageName();
    CaptureMatch captures;
    const ReplacementRule* rule = gRules.findManual(
        trigger->text, extension, path, language);
    if (rule == nullptr) {
        rule = gRules.findCaptureTemplate(
            trigger->text, Activation::none, extension, path, language, captures, true);
    }
    if (rule != nullptr) applyReplacement(editor, *trigger, *rule, 0, captures);
}

void reloadAll() {
    if (!gReady) return;
    loadConfiguration(true);
    loadRules(true);
    gExtensionCache.clear();
    refreshBufferContext(currentBufferId());
    refreshMenuChecks();
}

void reloadAfterManagerSave() {
    loadRules(false);
}

void manageReplacements() {
    if (!gReady) return;
    nppqr::showRuleManager(RuleManagerOptions{
        .notepadHandle = gNppData._nppHandle,
        .module = gModule,
        .dataDirectory = gDataDirectory,
        .replacementsPath = gReplacementsPath,
        .config = gConfig,
        .onRulesSaved = reloadAfterManagerSave,
    });
}

void openDataFolder() {
    if (!gDataDirectory.empty()) {
        ::ShellExecuteW(gNppData._nppHandle, L"open", gDataDirectory.c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void showAbout() {
    const std::wstring message = std::wstring(kPluginName) + L" " + kPluginVersion +
        tr(L"\n\nFast, local text expansion for Notepad++.\nLoaded rules: ",
           L"\n\nNotepad++용 빠른 로컬 텍스트 확장 플러그인이에요.\n불러온 규칙: ") +
        std::to_wstring(gRules.size()) +
        tr(L"\n\nIncludes a searchable rule manager, immediate replacement, validation, and rotating backups.\nNo document content is sent over the network.",
           L"\n\n검색 가능한 규칙 관리자, 즉시 치환, 유효성 검사, 순환 백업 기능을 포함해요.\n문서 내용은 네트워크로 전송하지 않아요.");
    showMessage(message);
}

bool setCommand(int index, const wchar_t* name, PFUNCPLUGINCMD function,
    ShortcutKey* shortcut = nullptr, bool initiallyChecked = false) {
    if (index < 0 || index >= kCommandCount || name == nullptr || function == nullptr) return false;
    ::wcsncpy_s(gFunctionItems[index]._itemName, name, _TRUNCATE);
    gFunctionItems[index]._pFunc = function;
    gFunctionItems[index]._pShKey = shortcut;
    gFunctionItems[index]._init2Check = initiallyChecked;
    return true;
}

void initializeCommands() {
    setCommand(commandToggleEnabled,
        tr(L"Automatic replacement enabled", L"자동 치환 사용"), toggleEnabled, nullptr, true);
    setCommand(commandPauseCurrentDocument,
        tr(L"Pause for current document", L"현재 문서에서 일시 중지"), togglePauseCurrentDocument);
    setCommand(commandManualReplace,
        tr(L"Replace trigger before caret", L"캐럿 앞 트리거 치환"), manualReplace, &gManualShortcut);
    setCommand(commandReload,
        tr(L"Reload settings and rules", L"설정과 규칙 다시 불러오기"), reloadAll);
    setCommand(commandManageReplacements,
        tr(L"Manage replacement rules…", L"치환 규칙 관리…"), manageReplacements);
    setCommand(commandOpenDataFolder,
        tr(L"Open data folder", L"데이터 폴더 열기"), openDataFolder);
    setCommand(commandAbout,
        tr(L"About NppQuickReplace", L"NppQuickReplace 정보"), showAbout);
}

void handleBufferClosed(UINT_PTR bufferId) {
    if (bufferId == 0) return;
    gPausedBuffers.erase(bufferId);
    gExtensionCache.erase(bufferId);
    gTabstopSessions.erase(bufferId);
}

} // namespace

BOOL APIENTRY DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        gModule = module;
        ::DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void setInfo(NppData notepadPlusData) {
    gNppData = notepadPlusData;
    initializeCommands();
}

extern "C" __declspec(dllexport) const wchar_t* getName() {
    return kPluginName;
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count) {
    if (count != nullptr) *count = kCommandCount;
    return gFunctionItems;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* notification) {
    if (notification == nullptr) return;
    try {
        switch (notification->nmhdr.code) {
        case NPPN_READY:
            initializePlugin();
            break;
        case NPPN_BUFFERACTIVATED:
        case NPPN_FILEOPENED:
        case NPPN_FILESAVED:
        case NPPN_FILERENAMED:
            refreshBufferContext(static_cast<UINT_PTR>(notification->nmhdr.idFrom));
            refreshMenuChecks();
            break;
        case NPPN_FILECLOSED:
            handleBufferClosed(static_cast<UINT_PTR>(notification->nmhdr.idFrom));
            break;
        case NPPN_DARKMODECHANGED:
            nppqr::handleRuleManagerDarkModeChange();
            break;
        case NPPN_SHUTDOWN:
            saveEnabledState();
            gReady = false;
            gTabstopSessions.clear();
            nppqr::closeRuleManager(true);
            break;
        case SCN_CHARADDED:
            processCharacterAdded(*notification);
            break;
        case SCN_MODIFIED:
            updateTabstopSession(*notification);
            processPastedText(*notification);
            break;
        case SCN_UPDATEUI:
            validateTabstopSelection(*notification);
            break;
        default:
            break;
        }
    } catch (const std::exception& exception) {
        gInternalEdit = false;
        logMessage(L"error", L"Unexpected exception in notification handler: " +
            utf8ToWide(exception.what()));
    } catch (...) {
        gInternalEdit = false;
        logMessage(L"error", L"Unexpected non-standard exception in notification handler.");
    }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) {
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL isUnicode() {
    return TRUE;
}
