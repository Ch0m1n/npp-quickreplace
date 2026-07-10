#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ConfigStore.h"
#include "RuleStore.h"
#include "PluginInterface.h"

namespace {

using nppqr::Activation;
using nppqr::ConfigStore;
using nppqr::PluginConfig;
using nppqr::ReplacementRule;
using nppqr::RuleStore;

constexpr wchar_t kPluginName[] = L"NppQuickReplace";
constexpr wchar_t kPluginVersion[] = L"0.2.0-alpha";
constexpr int kCommandCount = 7;

enum CommandIndex : int {
    commandToggleEnabled = 0,
    commandPauseCurrentDocument,
    commandManualReplace,
    commandReload,
    commandOpenReplacements,
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

bool gReady = false;
bool gInternalEdit = false;
bool gConfigValid = false;

std::wstring utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return L"An unknown UTF-8 error occurred.";
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string wideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

void showMessage(std::wstring_view message, UINT icon = MB_ICONINFORMATION) {
    ::MessageBoxW(
        gNppData._nppHandle,
        std::wstring(message).c_str(),
        kPluginName,
        MB_OK | icon);
}

void showError(std::string_view error) {
    showMessage(utf8ToWide(error), MB_ICONERROR);
}

HWND currentEditor() {
    int view = -1;
    ::SendMessageW(
        gNppData._nppHandle,
        NPPM_GETCURRENTSCINTILLA,
        0,
        reinterpret_cast<LPARAM>(&view));
    if (view == 0) {
        return gNppData._scintillaMainHandle;
    }
    if (view == 1) {
        return gNppData._scintillaSecondHandle;
    }
    return nullptr;
}

UINT_PTR currentBufferId() {
    return static_cast<UINT_PTR>(
        ::SendMessageW(gNppData._nppHandle, NPPM_GETCURRENTBUFFERID, 0, 0));
}

bool isCurrentBufferPaused() {
    const UINT_PTR bufferId = currentBufferId();
    return bufferId != 0 && gPausedBuffers.contains(bufferId);
}

void setMenuCheck(CommandIndex command, bool checked) {
    const int commandId = gFunctionItems[command]._cmdID;
    if (commandId != 0) {
        ::SendMessageW(
            gNppData._nppHandle,
            NPPM_SETMENUITEMCHECK,
            static_cast<WPARAM>(commandId),
            static_cast<LPARAM>(checked));
    }
}

void refreshMenuChecks() {
    setMenuCheck(commandToggleEnabled, gConfig.pluginEnabled);
    setMenuCheck(commandPauseCurrentDocument, isCurrentBufferPaused());
}

std::filesystem::path currentFilePath() {
    std::vector<wchar_t> buffer(4096, L'\0');
    ::SendMessageW(
        gNppData._nppHandle,
        NPPM_GETFULLCURRENTPATH,
        static_cast<WPARAM>(buffer.size()),
        reinterpret_cast<LPARAM>(buffer.data()));
    return std::filesystem::path(buffer.data());
}

std::string currentFileExtension() {
    const std::wstring extension = currentFilePath().extension().wstring();
    return RuleStore::foldAscii(wideToUtf8(extension));
}

bool isBoundaryByte(unsigned char byte) {
    if (byte >= 0x80U) {
        return false;
    }
    return std::isspace(byte) != 0 ||
           gConfig.punctuationTriggers.find(static_cast<char>(byte)) != std::string::npos;
}

struct TriggerRange {
    Sci_Position start = 0;
    Sci_Position end = 0;
    std::string text;
};

std::optional<TriggerRange> readTriggerBefore(HWND editor, Sci_Position end) {
    if (editor == nullptr || end <= 0) {
        return std::nullopt;
    }

    Sci_Position start = end;
    while (start > 0) {
        const Sci_Position previous = static_cast<Sci_Position>(
            ::SendMessageW(editor, SCI_POSITIONBEFORE, static_cast<WPARAM>(start), 0));
        if (previous < 0 || previous >= start) {
            break;
        }
        if (end - previous > static_cast<Sci_Position>(gConfig.maxTriggerBytes)) {
            return std::nullopt;
        }

        const int character = static_cast<int>(
            ::SendMessageW(editor, SCI_GETCHARAT, static_cast<WPARAM>(previous), 0));
        if (isBoundaryByte(static_cast<unsigned char>(character & 0xFF))) {
            break;
        }
        start = previous;
    }

    if (start == end) {
        return std::nullopt;
    }

    const std::size_t length = static_cast<std::size_t>(end - start);
    std::string text(length + 1, '\0');
    Sci_TextRangeFull range{{start, end}, text.data()};
    ::SendMessageW(editor, SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<LPARAM>(&range));
    text.resize(length);
    return TriggerRange{start, end, std::move(text)};
}

Sci_Position delimiterStartFor(HWND editor, Sci_Position caret, int character) {
    Sci_Position start = static_cast<Sci_Position>(
        ::SendMessageW(editor, SCI_POSITIONBEFORE, static_cast<WPARAM>(caret), 0));
    if (start < 0) {
        return caret;
    }

    if (character == '\n' && start > 0) {
        const int atStart = static_cast<int>(
            ::SendMessageW(editor, SCI_GETCHARAT, static_cast<WPARAM>(start), 0));
        const Sci_Position previous = static_cast<Sci_Position>(
            ::SendMessageW(editor, SCI_POSITIONBEFORE, static_cast<WPARAM>(start), 0));
        if ((atStart & 0xFF) == '\n' && previous >= 0) {
            const int atPrevious = static_cast<int>(
                ::SendMessageW(editor, SCI_GETCHARAT, static_cast<WPARAM>(previous), 0));
            if ((atPrevious & 0xFF) == '\r') {
                start = previous;
            }
        }
    }
    return start;
}

std::string normalizeEol(std::string_view text, int eolMode) {
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                ++index;
            }
            normalized.push_back('\n');
        } else {
            normalized.push_back(text[index]);
        }
    }

    if (eolMode == SC_EOL_LF) {
        return normalized;
    }

    std::string result;
    result.reserve(normalized.size() + normalized.size() / 8);
    for (const char character : normalized) {
        if (character == '\n') {
            if (eolMode == SC_EOL_CRLF) {
                result.append("\r\n");
            } else {
                result.push_back('\r');
            }
        } else {
            result.push_back(character);
        }
    }
    return result;
}

struct ExpandedReplacement {
    std::string text;
    std::optional<std::size_t> cursorOffset;
};

ExpandedReplacement expandReplacement(HWND editor, std::string source) {
    constexpr std::string_view marker = "${cursor}";
    const std::size_t firstMarker = source.find(marker);
    std::string beforeMarker;
    if (firstMarker != std::string::npos) {
        beforeMarker = source.substr(0, firstMarker);
    }

    std::size_t markerPosition = source.find(marker);
    while (markerPosition != std::string::npos) {
        source.erase(markerPosition, marker.size());
        markerPosition = source.find(marker, markerPosition);
    }

    const int eolMode = static_cast<int>(::SendMessageW(editor, SCI_GETEOLMODE, 0, 0));
    ExpandedReplacement result;
    result.text = normalizeEol(source, eolMode);
    if (firstMarker != std::string::npos) {
        result.cursorOffset = normalizeEol(beforeMarker, eolMode).size();
    }
    return result;
}

void applyReplacement(
    HWND editor,
    const TriggerRange& trigger,
    const ReplacementRule& rule,
    Sci_Position trailingDelimiterBytes) {
    const ExpandedReplacement replacement = expandReplacement(editor, rule.replacement);

    ::SendMessageW(editor, SCI_BEGINUNDOACTION, 0, 0);
    gInternalEdit = true;
    ::SendMessageW(
        editor,
        SCI_SETTARGETRANGE,
        static_cast<WPARAM>(trigger.start),
        static_cast<LPARAM>(trigger.end));
    ::SendMessageW(
        editor,
        SCI_REPLACETARGET,
        static_cast<WPARAM>(replacement.text.size()),
        reinterpret_cast<LPARAM>(replacement.text.data()));

    const Sci_Position newCaret = replacement.cursorOffset.has_value()
        ? trigger.start + static_cast<Sci_Position>(*replacement.cursorOffset)
        : trigger.start + static_cast<Sci_Position>(replacement.text.size()) + trailingDelimiterBytes;
    ::SendMessageW(editor, SCI_SETEMPTYSELECTION, static_cast<WPARAM>(newCaret), 0);
    gInternalEdit = false;
    ::SendMessageW(editor, SCI_ENDUNDOACTION, 0, 0);
}

bool loadRules(bool showSuccess) {
    const nppqr::RuleLoadResult primary = gRules.loadFromFile(gReplacementsPath);
    if (primary.ok) {
        if (showSuccess) {
            showMessage(
                L"Loaded " + std::to_wstring(primary.loadedCount) + L" replacement rules.");
        }
        return true;
    }

    for (const auto& backup : ConfigStore::replacementBackupsNewestFirst(gDataDirectory)) {
        const nppqr::RuleLoadResult recovered = gRules.loadFromFile(backup);
        if (recovered.ok) {
            showMessage(
                L"replacements.json could not be loaded.\n\n"
                L"NppQuickReplace loaded the newest valid backup in memory instead. "
                L"The damaged original file was not overwritten.\n\n" +
                utf8ToWide(primary.error),
                MB_ICONWARNING);
            return true;
        }
    }

    showMessage(
        L"No replacement rules were loaded. The existing file was left unchanged.\n\n" +
            utf8ToWide(primary.error),
        MB_ICONERROR);
    return false;
}

bool initializePlugin() {
    std::vector<wchar_t> configDirectory(4096, L'\0');
    const LRESULT length = ::SendMessageW(
        gNppData._nppHandle,
        NPPM_GETPLUGINSCONFIGDIR,
        static_cast<WPARAM>(configDirectory.size()),
        reinterpret_cast<LPARAM>(configDirectory.data()));
    if (length <= 0 || configDirectory.front() == L'\0') {
        showMessage(L"Notepad++ did not provide a plugin configuration directory.", MB_ICONERROR);
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

    const nppqr::ConfigLoadResult configResult = ConfigStore::loadConfig(gConfigPath, gConfig);
    gConfigValid = configResult.ok;
    if (!configResult.ok) {
        gConfig = PluginConfig{};
        showMessage(
            L"config.json is invalid. Safe in-memory defaults are active, and the existing file was not overwritten.\n\n" +
                utf8ToWide(configResult.error),
            MB_ICONWARNING);
    }

    loadRules(false);
    gReady = true;
    refreshMenuChecks();
    return true;
}

void saveEnabledState() {
    if (!gConfigValid || !gConfig.rememberEnabledState || gConfigPath.empty()) {
        return;
    }
    std::string error;
    if (!ConfigStore::saveConfigAtomic(gConfigPath, gConfig, error)) {
        ::OutputDebugStringW((L"NppQuickReplace: " + utf8ToWide(error) + L"\n").c_str());
    }
}

void processCharacterAdded(const SCNotification& notification) {
    if (!gReady || gInternalEdit || !gConfig.pluginEnabled || isCurrentBufferPaused()) {
        return;
    }
    if (notification.characterSource == SC_CHARACTERSOURCE_TENTATIVE_INPUT) {
        return;
    }

    HWND editor = static_cast<HWND>(notification.nmhdr.hwndFrom);
    if (editor != gNppData._scintillaMainHandle && editor != gNppData._scintillaSecondHandle) {
        return;
    }

    const Activation activation = RuleStore::activationForCharacter(
        notification.ch, gConfig.punctuationTriggers);
    if (activation == Activation::none) {
        return;
    }
    if (gConfig.skipReadOnlyDocuments && ::SendMessageW(editor, SCI_GETREADONLY, 0, 0) != 0) {
        return;
    }
    if (gConfig.skipMultiSelection && ::SendMessageW(editor, SCI_GETSELECTIONS, 0, 0) > 1) {
        return;
    }

    const Sci_Position caret = static_cast<Sci_Position>(
        ::SendMessageW(editor, SCI_GETCURRENTPOS, 0, 0));
    const Sci_Position delimiterStart = delimiterStartFor(editor, caret, notification.ch);
    const auto trigger = readTriggerBefore(editor, delimiterStart);
    if (!trigger.has_value()) {
        return;
    }

    const ReplacementRule* rule = gRules.find(
        trigger->text, activation, currentFileExtension());
    if (rule == nullptr) {
        return;
    }

    applyReplacement(editor, *trigger, *rule, caret - delimiterStart);
}

void toggleEnabled() {
    gConfig.pluginEnabled = !gConfig.pluginEnabled;
    refreshMenuChecks();
    saveEnabledState();
}

void togglePauseCurrentDocument() {
    const UINT_PTR bufferId = currentBufferId();
    if (bufferId == 0) {
        return;
    }
    if (!gPausedBuffers.erase(bufferId)) {
        gPausedBuffers.insert(bufferId);
    }
    refreshMenuChecks();
}

void manualReplace() {
    if (!gReady) {
        return;
    }
    HWND editor = currentEditor();
    if (editor == nullptr || ::SendMessageW(editor, SCI_GETREADONLY, 0, 0) != 0) {
        return;
    }
    if (gConfig.skipMultiSelection && ::SendMessageW(editor, SCI_GETSELECTIONS, 0, 0) > 1) {
        return;
    }

    const Sci_Position caret = static_cast<Sci_Position>(
        ::SendMessageW(editor, SCI_GETCURRENTPOS, 0, 0));
    const auto trigger = readTriggerBefore(editor, caret);
    if (!trigger.has_value()) {
        return;
    }

    const ReplacementRule* rule = gRules.findManual(trigger->text, currentFileExtension());
    if (rule != nullptr) {
        applyReplacement(editor, *trigger, *rule, 0);
    }
}

void reloadRules() {
    if (gReady) {
        loadRules(true);
    }
}

void openReplacements() {
    if (!gReplacementsPath.empty()) {
        ::SendMessageW(
            gNppData._nppHandle,
            NPPM_DOOPEN,
            0,
            reinterpret_cast<LPARAM>(gReplacementsPath.c_str()));
    }
}

void openDataFolder() {
    if (!gDataDirectory.empty()) {
        ::ShellExecuteW(
            gNppData._nppHandle,
            L"open",
            gDataDirectory.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL);
    }
}

void showAbout() {
    showMessage(
        std::wstring(kPluginName) + L" " + kPluginVersion +
        L"\n\nFast, local text expansion for Notepad++.\n"
        L"Loaded rules: " + std::to_wstring(gRules.size()) +
        L"\n\nNo document content is sent over the network.");
}

bool setCommand(
    int index,
    const wchar_t* name,
    PFUNCPLUGINCMD function,
    ShortcutKey* shortcut = nullptr,
    bool initiallyChecked = false) {
    if (index < 0 || index >= kCommandCount || name == nullptr || function == nullptr) {
        return false;
    }
    ::wcsncpy_s(gFunctionItems[index]._itemName, name, _TRUNCATE);
    gFunctionItems[index]._pFunc = function;
    gFunctionItems[index]._pShKey = shortcut;
    gFunctionItems[index]._init2Check = initiallyChecked;
    return true;
}

void initializeCommands() {
    setCommand(commandToggleEnabled, L"Automatic replacement enabled", toggleEnabled, nullptr, true);
    setCommand(commandPauseCurrentDocument, L"Pause for current document", togglePauseCurrentDocument);
    setCommand(commandManualReplace, L"Replace trigger before caret", manualReplace, &gManualShortcut);
    setCommand(commandReload, L"Reload replacements", reloadRules);
    setCommand(commandOpenReplacements, L"Manage replacements (JSON)", openReplacements);
    setCommand(commandOpenDataFolder, L"Open data folder", openDataFolder);
    setCommand(commandAbout, L"About NppQuickReplace", showAbout);
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
    if (count != nullptr) {
        *count = kCommandCount;
    }
    return gFunctionItems;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* notification) {
    if (notification == nullptr) {
        return;
    }

    try {
        switch (notification->nmhdr.code) {
        case NPPN_READY:
            initializePlugin();
            break;
        case NPPN_BUFFERACTIVATED:
            refreshMenuChecks();
            break;
        case NPPN_SHUTDOWN:
            saveEnabledState();
            gReady = false;
            break;
        case SCN_CHARADDED:
            processCharacterAdded(*notification);
            break;
        default:
            break;
        }
    } catch (...) {
        gInternalEdit = false;
        ::OutputDebugStringW(L"NppQuickReplace: unexpected exception in notification handler.\n");
    }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) {
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL isUnicode() {
    return TRUE;
}

