#include "SettingsManager.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Localization.h"
#include "DataPackage.h"
#include "Notepad_plus_msgs.h"

namespace nppqr {
namespace {

constexpr wchar_t kWindowClass[] = L"NppQuickReplace.SettingsManager";
constexpr wchar_t kWindowTitleEnglish[] = L"NppQuickReplace · Settings";
constexpr wchar_t kWindowTitleKorean[] = L"NppQuickReplace · 설정";

HWND gSettingsWindow = nullptr;
bool gDiscardSettingsChanges = false;

enum ControlId : int {
    idPluginEnabled = 3101,
    idRememberEnabled,
    idProcessPaste,
    idSkipReadOnly,
    idSkipMultiSelection,
    idAutoReload,
    idBackupEnabled,
    idLoggingEnabled,
    idLanguage,
    idPunctuation,
    idMaxTrigger,
    idMaxExpanded,
    idMaxBackups,
    idReloadInterval,
    idLoggingLevel,
    idSave,
    idDefaults,
    idOpenConfig,
    idExportPackage,
    idImportPackage,
    idClose,
};

const wchar_t* tr(const wchar_t* english, const wchar_t* korean) noexcept {
    return localization::text(english, korean);
}

const wchar_t* windowTitle() noexcept {
    return tr(kWindowTitleEnglish, kWindowTitleKorean);
}

std::wstring utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return L"�";
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string wideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring windowText(HWND control) {
    const int length = std::max(0, ::GetWindowTextLengthW(control));
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    ::GetWindowTextW(control, result.data(), static_cast<int>(result.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}

class SettingsWindow {
public:
    explicit SettingsWindow(SettingsManagerOptions options)
        : options_(std::move(options)), saved_(options_.config) {}

    bool create() {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_DBLCLKS;
        windowClass.lpfnWndProc = &SettingsWindow::windowProcedure;
        windowClass.hInstance = options_.module;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kWindowClass;
        ::RegisterClassExW(&windowClass);

        RECT owner{};
        ::GetWindowRect(options_.notepadHandle, &owner);
        const UINT ownerDpi = ::GetDpiForWindow(options_.notepadHandle);
        const int width = ::MulDiv(780, static_cast<int>(ownerDpi), 96);
        const int height = ::MulDiv(610, static_cast<int>(ownerDpi), 96);
        const int x = owner.left + std::max(20L, (owner.right - owner.left - width) / 2);
        const int y = owner.top + std::max(20L, (owner.bottom - owner.top - height) / 2);
        window_ = ::CreateWindowExW(
            WS_EX_CONTROLPARENT, kWindowClass, windowTitle(),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
            x, y, width, height, options_.notepadHandle, nullptr, options_.module, this);
        if (window_ == nullptr) return false;
        gSettingsWindow = window_;
        ::ShowWindow(window_, SW_SHOW);
        ::UpdateWindow(window_);
        return true;
    }

    void activate() const {
        if (::IsIconic(window_)) ::ShowWindow(window_, SW_RESTORE);
        ::ShowWindow(window_, SW_SHOW);
        ::SetForegroundWindow(window_);
    }

    void handleDarkModeChange() const {
        if (window_ == nullptr) return;
        ::SendMessageW(options_.notepadHandle, NPPM_DARKMODESUBCLASSANDTHEME,
            static_cast<WPARAM>(NppDarkMode::dmfHandleChange),
            reinterpret_cast<LPARAM>(window_));
        ::RedrawWindow(window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
    }

private:
    static LRESULT CALLBACK windowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<SettingsWindow*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<SettingsWindow*>(create->lpCreateParams);
            self->window_ = window;
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self == nullptr
            ? ::DefWindowProcW(window, message, wParam, lParam)
            : self->handleMessage(message, wParam, lParam);
    }

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: return onCreate() ? 0 : -1;
        case WM_SIZE: layout(LOWORD(lParam), HIWORD(lParam)); return 0;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            updateFont();
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {scale(780), scale(560)};
            return 0;
        case WM_COMMAND:
            onCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_CLOSE:
            if (gDiscardSettingsChanges || !dirty_ || confirmDiscard()) {
                ::DestroyWindow(window_);
            }
            return 0;
        case WM_DESTROY:
            ::SendMessageW(options_.notepadHandle, NPPM_MODELESSDIALOG,
                MODELESSDIALOGREMOVE, reinterpret_cast<LPARAM>(window_));
            return 0;
        case WM_NCDESTROY:
            if (font_ != nullptr) ::DeleteObject(font_);
            gSettingsWindow = nullptr;
            gDiscardSettingsChanges = false;
            ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            delete this;
            return 0;
        default:
            return ::DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    HWND makeControl(
        DWORD exStyle, const wchar_t* className, const wchar_t* text, DWORD style, int id) {
        HWND control = ::CreateWindowExW(exStyle, className, text,
            WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), options_.module, nullptr);
        controls_.push_back(control);
        return control;
    }

    HWND makeLabel(const wchar_t* text) {
        return makeControl(0, WC_STATICW, text, SS_LEFT | SS_NOPREFIX, 0);
    }

    HWND makeCheck(const wchar_t* text, int id) {
        return makeControl(0, WC_BUTTONW, text,
            WS_TABSTOP | BS_AUTOCHECKBOX, id);
    }

    HWND makeEdit(int id) {
        return makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER, id);
    }

    HWND makeButton(const wchar_t* text, int id, DWORD style = BS_PUSHBUTTON) {
        return makeControl(0, WC_BUTTONW, text, WS_TABSTOP | style, id);
    }

    bool onCreate() {
        INITCOMMONCONTROLSEX common{
            sizeof(common), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
        ::InitCommonControlsEx(&common);
        dpi_ = ::GetDpiForWindow(window_);

        title_ = makeLabel(tr(L"Plugin settings", L"플러그인 설정"));
        subtitle_ = makeLabel(tr(
            L"Changes are validated and saved atomically. Your rule file is not modified.",
            L"변경 내용은 검사 후 원자적으로 저장돼요. 규칙 파일은 건드리지 않아요."));

        generalTitle_ = makeLabel(tr(L"Behavior", L"동작"));
        pluginEnabled_ = makeCheck(tr(L"Automatic replacement enabled", L"자동 치환 사용"), idPluginEnabled);
        rememberEnabled_ = makeCheck(tr(L"Remember enabled state", L"사용 상태 기억"), idRememberEnabled);
        processPaste_ = makeCheck(tr(L"Process a trigger at the end of pasted text", L"붙여넣은 텍스트 끝의 트리거 처리"), idProcessPaste);
        skipReadOnly_ = makeCheck(tr(L"Skip read-only documents", L"읽기 전용 문서 건너뛰기"), idSkipReadOnly);
        skipMultiSelection_ = makeCheck(tr(L"Disable replacement with multiple carets", L"다중 커서에서 치환 사용 안 함"), idSkipMultiSelection);
        autoReload_ = makeCheck(tr(L"Reload rules automatically when the file changes", L"규칙 파일 변경 시 자동으로 다시 불러오기"), idAutoReload);
        backupEnabled_ = makeCheck(tr(L"Create rotating backups before saving rules", L"규칙 저장 전에 순환 백업 만들기"), idBackupEnabled);
        loggingEnabled_ = makeCheck(tr(L"Write diagnostic logs", L"진단 로그 기록"), idLoggingEnabled);

        limitsTitle_ = makeLabel(tr(L"Language, limits, and diagnostics", L"언어·제한·진단"));
        languageLabel_ = makeLabel(tr(L"Interface language", L"인터페이스 언어"));
        language_ = makeControl(0, WC_COMBOBOXW, L"",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, idLanguage);
        addCombo(language_, tr(L"Automatic (Windows display language)", L"자동 (Windows 표시 언어)"));
        addCombo(language_, L"English");
        addCombo(language_, L"한국어");

        punctuationLabel_ = makeLabel(tr(L"Punctuation triggers", L"문장 부호 트리거"));
        punctuation_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idPunctuation);
        maxTriggerLabel_ = makeLabel(tr(L"Maximum trigger bytes (16–4096)", L"최대 트리거 바이트 (16–4096)"));
        maxTrigger_ = makeEdit(idMaxTrigger);
        maxExpandedLabel_ = makeLabel(tr(L"Maximum expansion size in KiB (4–16384)", L"최대 치환 크기 KiB (4–16384)"));
        maxExpanded_ = makeEdit(idMaxExpanded);
        maxBackupsLabel_ = makeLabel(tr(L"Rule backup count (1–100)", L"규칙 백업 개수 (1–100)"));
        maxBackups_ = makeEdit(idMaxBackups);
        reloadIntervalLabel_ = makeLabel(tr(L"Auto-reload interval in ms (250–10000)", L"자동 재로드 간격 ms (250–10000)"));
        reloadInterval_ = makeEdit(idReloadInterval);
        loggingLevelLabel_ = makeLabel(tr(L"Logging level", L"로그 수준"));
        loggingLevel_ = makeControl(0, WC_COMBOBOXW, L"",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, idLoggingLevel);
        for (const wchar_t* value : {L"debug", L"info", L"warning", L"error"}) {
            addCombo(loggingLevel_, value);
        }

        languageNote_ = makeLabel(tr(
            L"Menu language changes fully after restarting Notepad++.",
            L"메뉴 언어는 Notepad++를 다시 시작하면 완전히 적용돼요."));
        status_ = makeLabel(tr(L"Review the settings, then save.", L"설정을 확인한 뒤 저장하세요."));
        save_ = makeButton(tr(L"Save settings", L"설정 저장"), idSave, BS_DEFPUSHBUTTON);
        defaults_ = makeButton(tr(L"Restore defaults", L"기본값 불러오기"), idDefaults);
        exportPackage_ = makeButton(tr(L"Export data…", L"데이터 내보내기…"), idExportPackage);
        importPackage_ = makeButton(tr(L"Import data…", L"데이터 가져오기…"), idImportPackage);
        openConfig_ = makeButton(tr(L"Open config.json", L"config.json 열기"), idOpenConfig);
        close_ = makeButton(tr(L"Close", L"닫기"), idClose);

        updateFont();
        loadControls(saved_);
        ::SendMessageW(options_.notepadHandle, NPPM_MODELESSDIALOG,
            MODELESSDIALOGADD, reinterpret_cast<LPARAM>(window_));
        ::SendMessageW(options_.notepadHandle, NPPM_DARKMODESUBCLASSANDTHEME,
            static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(window_));
        return true;
    }

    void addCombo(HWND combo, const wchar_t* text) {
        ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    }

    int scale(int value) const {
        return ::MulDiv(value, static_cast<int>(dpi_), 96);
    }

    void place(HWND control, int x, int y, int width, int height) const {
        ::SetWindowPos(control, nullptr, x, y, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void layout(int width, int height) {
        if (width <= 0 || height <= 0 || title_ == nullptr) return;
        const int margin = scale(18);
        const int gap = scale(26);
        const int columnWidth = (width - margin * 2 - gap) / 2;
        const int rightX = margin + columnWidth + gap;
        int y = margin;

        place(title_, margin, y, width - margin * 2, scale(28));
        place(subtitle_, margin, y + scale(30), width - margin * 2, scale(34));
        y += scale(76);
        place(generalTitle_, margin, y, columnWidth, scale(24));
        place(limitsTitle_, rightX, y, columnWidth, scale(24));

        int leftY = y + scale(34);
        for (HWND check : {pluginEnabled_, rememberEnabled_, processPaste_, skipReadOnly_,
                skipMultiSelection_, autoReload_, backupEnabled_, loggingEnabled_}) {
            place(check, margin, leftY, columnWidth, scale(28));
            leftY += scale(34);
        }

        int rightY = y + scale(34);
        placeField(languageLabel_, language_, rightX, rightY, columnWidth);
        rightY += scale(58);
        placeField(punctuationLabel_, punctuation_, rightX, rightY, columnWidth);
        rightY += scale(58);
        placeField(maxTriggerLabel_, maxTrigger_, rightX, rightY, columnWidth);
        rightY += scale(58);
        placeField(maxExpandedLabel_, maxExpanded_, rightX, rightY, columnWidth);
        rightY += scale(58);
        placeField(maxBackupsLabel_, maxBackups_, rightX, rightY, columnWidth);
        rightY += scale(58);
        placeField(reloadIntervalLabel_, reloadInterval_, rightX, rightY, columnWidth);
        rightY += scale(58);
        placeField(loggingLevelLabel_, loggingLevel_, rightX, rightY, columnWidth);

        const int footerY = height - scale(96);
        place(languageNote_, margin, footerY - scale(36), width - margin * 2, scale(26));
        place(status_, margin, footerY, width - margin * 2, scale(24));
        const int buttonY = height - scale(48);
        place(save_, margin, buttonY, scale(112), scale(32));
        place(defaults_, margin + scale(122), buttonY, scale(112), scale(32));
        place(exportPackage_, margin + scale(244), buttonY, scale(122), scale(32));
        place(importPackage_, margin + scale(376), buttonY, scale(122), scale(32));
        place(openConfig_, margin + scale(508), buttonY, scale(116), scale(32));
        place(close_, width - margin - scale(90), buttonY, scale(90), scale(32));
    }

    void placeField(HWND label, HWND control, int x, int y, int width) const {
        place(label, x, y, width, scale(20));
        place(control, x, y + scale(22), width, scale(28));
    }

    void updateFont() {
        dpi_ = ::GetDpiForWindow(window_);
        if (font_ != nullptr) ::DeleteObject(font_);
        font_ = ::CreateFontW(-::MulDiv(10, static_cast<int>(dpi_), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        for (HWND control : controls_) {
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
    }

    void setCheck(HWND control, bool value) {
        ::SendMessageW(control, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    bool checked(HWND control) const {
        return ::SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    void setNumber(HWND control, std::size_t value) {
        ::SetWindowTextW(control, std::to_wstring(value).c_str());
    }

    void loadControls(const PluginConfig& config) {
        loading_ = true;
        setCheck(pluginEnabled_, config.pluginEnabled);
        setCheck(rememberEnabled_, config.rememberEnabledState);
        setCheck(processPaste_, config.processPaste);
        setCheck(skipReadOnly_, config.skipReadOnlyDocuments);
        setCheck(skipMultiSelection_, config.skipMultiSelection);
        setCheck(autoReload_, config.autoReloadRules);
        setCheck(backupEnabled_, config.backupEnabled);
        setCheck(loggingEnabled_, config.loggingEnabled);
        ::SetWindowTextW(punctuation_, utf8ToWide(config.punctuationTriggers).c_str());
        setNumber(maxTrigger_, config.maxTriggerBytes);
        setNumber(maxExpanded_, config.maxExpandedBytes / 1024U);
        setNumber(maxBackups_, config.maxBackupFiles);
        setNumber(reloadInterval_, config.autoReloadIntervalMs);
        const int languageIndex = config.uiLanguage == "en" ? 1 : config.uiLanguage == "ko" ? 2 : 0;
        ::SendMessageW(language_, CB_SETCURSEL, languageIndex, 0);
        const int loggingIndex = config.loggingLevel == "debug" ? 0 :
            config.loggingLevel == "info" ? 1 : config.loggingLevel == "error" ? 3 : 2;
        ::SendMessageW(loggingLevel_, CB_SETCURSEL, loggingIndex, 0);
        loading_ = false;
        dirty_ = config != saved_;
        updateTitle();
    }

    bool parseNumber(
        HWND control, std::size_t minimum, std::size_t maximum,
        std::size_t& value, const wchar_t* fieldName) {
        const std::wstring text = windowText(control);
        if (text.empty()) {
            showValidation(fieldName, tr(L"A value is required.", L"값을 입력해야 해요."), control);
            return false;
        }
        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long long parsed = ::wcstoull(text.c_str(), &end, 10);
        if (errno != 0 || end == text.c_str() || *end != L'\0' ||
            parsed < minimum || parsed > maximum) {
            const std::wstring reason = tr(L"Enter a number inside the displayed range.",
                L"표시된 범위 안의 숫자를 입력하세요.");
            showValidation(fieldName, reason, control);
            return false;
        }
        value = static_cast<std::size_t>(parsed);
        return true;
    }

    void showValidation(
        std::wstring_view field, std::wstring_view reason, HWND control) {
        const std::wstring message = std::wstring(field) + L"\n\n" +
            std::wstring(reason) + L"\n\n" +
            tr(L"Your current input was preserved. Correct it and try saving again.",
               L"현재 입력은 그대로 보존했어요. 수정한 뒤 다시 저장하세요.");
        ::MessageBoxW(window_, message.c_str(), windowTitle(), MB_OK | MB_ICONWARNING);
        ::SetFocus(control);
        ::SendMessageW(control, EM_SETSEL, 0, -1);
    }

    bool readControls(PluginConfig& result) {
        result = saved_;
        result.pluginEnabled = checked(pluginEnabled_);
        result.rememberEnabledState = checked(rememberEnabled_);
        result.processPaste = checked(processPaste_);
        result.skipReadOnlyDocuments = checked(skipReadOnly_);
        result.skipMultiSelection = checked(skipMultiSelection_);
        result.autoReloadRules = checked(autoReload_);
        result.backupEnabled = checked(backupEnabled_);
        result.loggingEnabled = checked(loggingEnabled_);
        result.punctuationTriggers = wideToUtf8(windowText(punctuation_));
        if (result.punctuationTriggers.empty()) {
            showValidation(tr(L"Punctuation triggers", L"문장 부호 트리거"),
                tr(L"Enter at least one character.", L"문자를 하나 이상 입력하세요."), punctuation_);
            return false;
        }
        std::size_t expandedKiB = 0;
        if (!parseNumber(maxTrigger_, 16, 4096, result.maxTriggerBytes,
                tr(L"Maximum trigger bytes", L"최대 트리거 바이트")) ||
            !parseNumber(maxExpanded_, 4, 16384, expandedKiB,
                tr(L"Maximum expansion size", L"최대 치환 크기")) ||
            !parseNumber(maxBackups_, 1, 100, result.maxBackupFiles,
                tr(L"Rule backup count", L"규칙 백업 개수")) ||
            !parseNumber(reloadInterval_, 250, 10'000, result.autoReloadIntervalMs,
                tr(L"Auto-reload interval", L"자동 재로드 간격"))) {
            return false;
        }
        result.maxExpandedBytes = expandedKiB * 1024U;
        const LRESULT languageIndex = ::SendMessageW(language_, CB_GETCURSEL, 0, 0);
        result.uiLanguage = languageIndex == 1 ? "en" : languageIndex == 2 ? "ko" : "auto";
        const LRESULT loggingIndex = ::SendMessageW(loggingLevel_, CB_GETCURSEL, 0, 0);
        result.loggingLevel = loggingIndex == 0 ? "debug" :
            loggingIndex == 1 ? "info" : loggingIndex == 3 ? "error" : "warning";
        return true;
    }

    void exportDataPackage() {
        wchar_t path[32768]{};
        ::wcscpy_s(path, L"NppQuickReplace-data-package.json");
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = tr(
            L"NppQuickReplace data packages (*.json)\0*.json\0All files (*.*)\0*.*\0\0",
            L"NppQuickReplace 데이터 패키지 (*.json)\0*.json\0모든 파일 (*.*)\0*.*\0\0");
        dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.lpstrDefExt = L"json";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
            OFN_DONTADDTORECENT | OFN_EXPLORER;
        if (!::GetSaveFileNameW(&dialog)) return;

        const DataPackageResult result = DataPackage::exportPackage(
            options_.configPath, options_.replacementsPath, path);
        if (!result.ok) {
            const std::wstring message =
                std::wstring(tr(L"Data could not be exported.", L"데이터를 내보낼 수 없어요.")) +
                L"\n\n" + utf8ToWide(result.error) + L"\n\n" +
                tr(L"No settings or rules were changed.",
                   L"설정과 규칙은 변경하지 않았어요.");
            ::MessageBoxW(window_, message.c_str(), windowTitle(), MB_OK | MB_ICONERROR);
            return;
        }
        ::SetWindowTextW(status_, tr(
            L"Settings and rules exported to one validated package.",
            L"설정과 규칙을 검증된 패키지 하나로 내보냈어요."));
    }

    void importDataPackage() {
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = tr(
            L"NppQuickReplace data packages (*.json)\0*.json\0All files (*.*)\0*.*\0\0",
            L"NppQuickReplace 데이터 패키지 (*.json)\0*.json\0모든 파일 (*.*)\0*.*\0\0");
        dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
            OFN_DONTADDTORECENT | OFN_EXPLORER;
        if (!::GetOpenFileNameW(&dialog)) return;
        if (::MessageBoxW(window_, tr(
                L"Import this package?\n\nThe package is validated first. Current settings and rules are copied to a recovery folder before anything is replaced.",
                L"이 패키지를 가져올까요?\n\n먼저 패키지를 검사하고, 교체 전에 현재 설정과 규칙을 복구 폴더에 복사해요."),
                windowTitle(), MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }

        const DataPackageResult result = DataPackage::importPackage(
            options_.dataDirectory, options_.configPath,
            options_.replacementsPath, path);
        if (!result.ok) {
            const std::wstring message =
                std::wstring(tr(L"Data could not be imported.", L"데이터를 가져올 수 없어요.")) +
                L"\n\n" + utf8ToWide(result.error) + L"\n\n" +
                tr(L"Use the recovery folder shown in the logs if a partial write occurred.",
                   L"부분 쓰기가 발생했다면 로그에 표시된 복구 폴더를 사용하세요.");
            ::MessageBoxW(window_, message.c_str(), windowTitle(), MB_OK | MB_ICONERROR);
            return;
        }

        if (options_.onDataImported) options_.onDataImported();
        PluginConfig imported;
        const ConfigLoadResult loaded = ConfigStore::loadConfig(options_.configPath, imported);
        if (loaded.ok) {
            saved_ = imported;
            options_.config = imported;
            localization::setLanguagePreference(imported.uiLanguage);
            loadControls(imported);
            dirty_ = false;
            updateTitle();
        }
        const std::wstring message =
            std::wstring(tr(L"Data imported successfully.", L"데이터를 가져왔어요.")) +
            L"\n\n" + tr(L"Recovery copy:", L"복구 복사본:") + L"\n" +
            result.recoveryDirectory.wstring();
        ::MessageBoxW(window_, message.c_str(), windowTitle(), MB_OK | MB_ICONINFORMATION);
        ::SetWindowTextW(status_, tr(
            L"Imported settings and rules are active.",
            L"가져온 설정과 규칙을 적용했어요."));
    }

    void save() {
        PluginConfig candidate;
        if (!readControls(candidate)) return;
        std::string error;
        if (!ConfigStore::saveConfigAtomic(options_.configPath, candidate, error)) {
            const std::wstring message =
                std::wstring(tr(L"Settings could not be saved.", L"설정을 저장할 수 없어요.")) +
                L"\n\n" + utf8ToWide(error) + L"\n\n" +
                tr(L"Your input is still open. Resolve the problem and retry.",
                   L"입력 내용은 그대로 열려 있어요. 문제를 해결한 뒤 다시 시도하세요.");
            ::MessageBoxW(window_, message.c_str(), windowTitle(), MB_OK | MB_ICONERROR);
            return;
        }
        saved_ = candidate;
        options_.config = candidate;
        localization::setLanguagePreference(candidate.uiLanguage);
        dirty_ = false;
        updateTitle();
        ::SetWindowTextW(status_, tr(
            L"Settings saved. Menu language changes fully after restart.",
            L"설정을 저장했어요. 메뉴 언어는 다시 시작하면 완전히 적용돼요."));
        if (options_.onSaved) options_.onSaved(candidate);
    }

    void markDirty() {
        if (loading_) return;
        dirty_ = true;
        updateTitle();
        ::SetWindowTextW(status_, tr(
            L"Unsaved setting changes.", L"저장하지 않은 설정 변경 내용이 있어요."));
    }

    void updateTitle() const {
        const std::wstring title = std::wstring(windowTitle()) + (dirty_ ? L" *" : L"");
        ::SetWindowTextW(window_, title.c_str());
    }

    bool confirmDiscard() const {
        return ::MessageBoxW(window_, tr(
            L"Discard the unsaved setting changes?",
            L"저장하지 않은 설정 변경 내용을 버릴까요?"),
            windowTitle(), MB_YESNO | MB_ICONQUESTION) == IDYES;
    }

    void onCommand(int id, int notification) {
        if (id == idSave && notification == BN_CLICKED) {
            save();
            return;
        }
        if (id == idDefaults && notification == BN_CLICKED) {
            loadControls(PluginConfig{});
            dirty_ = true;
            updateTitle();
            ::SetWindowTextW(status_, tr(
                L"Defaults loaded for review. Save to apply them.",
                L"기본값을 불러왔어요. 적용하려면 저장하세요."));
            return;
        }
        if (id == idExportPackage && notification == BN_CLICKED) {
            exportDataPackage();
            return;
        }
        if (id == idImportPackage && notification == BN_CLICKED) {
            importDataPackage();
            return;
        }
        if (id == idOpenConfig && notification == BN_CLICKED) {
            ::ShellExecuteW(window_, L"open", options_.configPath.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (id == idClose && notification == BN_CLICKED) {
            ::SendMessageW(window_, WM_CLOSE, 0, 0);
            return;
        }
        const bool changed =
            notification == BN_CLICKED || notification == EN_CHANGE ||
            notification == CBN_SELCHANGE;
        if (changed) markDirty();
    }

    SettingsManagerOptions options_;
    PluginConfig saved_;
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    std::vector<HWND> controls_;
    bool loading_ = false;
    bool dirty_ = false;

    HWND title_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND generalTitle_ = nullptr;
    HWND limitsTitle_ = nullptr;
    HWND pluginEnabled_ = nullptr;
    HWND rememberEnabled_ = nullptr;
    HWND processPaste_ = nullptr;
    HWND skipReadOnly_ = nullptr;
    HWND skipMultiSelection_ = nullptr;
    HWND autoReload_ = nullptr;
    HWND backupEnabled_ = nullptr;
    HWND loggingEnabled_ = nullptr;
    HWND languageLabel_ = nullptr;
    HWND language_ = nullptr;
    HWND punctuationLabel_ = nullptr;
    HWND punctuation_ = nullptr;
    HWND maxTriggerLabel_ = nullptr;
    HWND maxTrigger_ = nullptr;
    HWND maxExpandedLabel_ = nullptr;
    HWND maxExpanded_ = nullptr;
    HWND maxBackupsLabel_ = nullptr;
    HWND maxBackups_ = nullptr;
    HWND reloadIntervalLabel_ = nullptr;
    HWND reloadInterval_ = nullptr;
    HWND loggingLevelLabel_ = nullptr;
    HWND loggingLevel_ = nullptr;
    HWND languageNote_ = nullptr;
    HWND status_ = nullptr;
    HWND save_ = nullptr;
    HWND defaults_ = nullptr;
    HWND openConfig_ = nullptr;
    HWND exportPackage_ = nullptr;
    HWND importPackage_ = nullptr;
    HWND close_ = nullptr;
};

} // namespace

void showSettingsManager(const SettingsManagerOptions& options) {
    if (gSettingsWindow != nullptr) {
        auto* manager = reinterpret_cast<SettingsWindow*>(
            ::GetWindowLongPtrW(gSettingsWindow, GWLP_USERDATA));
        if (manager != nullptr) manager->activate();
        return;
    }
    auto* manager = new SettingsWindow(options);
    if (!manager->create()) {
        delete manager;
        ::MessageBoxW(options.notepadHandle,
            tr(L"The settings window could not be created.",
               L"설정 창을 만들 수 없어요."),
            windowTitle(), MB_OK | MB_ICONERROR);
    }
}

void closeSettingsManager(bool discardChanges) {
    if (gSettingsWindow == nullptr) return;
    gDiscardSettingsChanges = discardChanges;
    ::SendMessageW(gSettingsWindow, WM_CLOSE, 0, 0);
}

void handleSettingsManagerDarkModeChange() {
    if (gSettingsWindow == nullptr) return;
    auto* manager = reinterpret_cast<SettingsWindow*>(
        ::GetWindowLongPtrW(gSettingsWindow, GWLP_USERDATA));
    if (manager != nullptr) manager->handleDarkModeChange();
}

} // namespace nppqr
