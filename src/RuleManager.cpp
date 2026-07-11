#include "RuleManager.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "GroupManager.h"
#include "Localization.h"
#include "Notepad_plus_msgs.h"
#include "RuleExchange.h"
#include "RuleStore.h"

namespace nppqr {
namespace {

using Json = nlohmann::json;

constexpr wchar_t kWindowClass[] = L"NppQuickReplace.RuleManager";
constexpr wchar_t kWindowTitle[] = L"NppQuickReplace · Replacement Manager";
constexpr UINT_PTR kExternalChangeTimer = 1;
constexpr UINT kExternalChangePollMs = 2000;
constexpr UINT_PTR kSearchTimer = 2;
constexpr UINT kSearchDebounceMs = 180;

enum ControlId : int {
    idSearch = 1001,
    idGroupFilter,
    idRuleList,
    idAdd,
    idDuplicate,
    idDelete,
    idManageGroups,
    idSetState,
    idImport,
    idExport,
    idEnabled = 1010,
    idTrigger,
    idGroup,
    idSpace,
    idEnter,
    idTab,
    idPunctuation,
    idImmediate,
    idCaseSensitive,
    idCaptureTemplate,
    idExtensions,
    idPathGlobs,
    idLanguages,
    idReplacement,
    idDescription,
    idPreview,
    idApplyDraft,
    idSave = 1030,
    idReload,
    idRestore,
    idOpenJson,
    idClose,
};

HWND gManagerWindow = nullptr;
bool gDiscardManagerChanges = false;

std::wstring utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return L"�";
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string wideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring windowText(HWND control) {
    const int length = std::max(0, ::GetWindowTextLengthW(control));
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    ::GetWindowTextW(control, result.data(), static_cast<int>(result.size()));
    result.resize(static_cast<std::size_t>(length));
    return result;
}

void setText(HWND control, std::wstring_view text) {
    ::SetWindowTextW(control, std::wstring(text).c_str());
}

bool isKoreanUi() noexcept {
    return localization::currentLanguage() == localization::Language::korean;
}

std::wstring lowerWide(std::wstring value) {
    if (!value.empty()) ::CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

std::wstring oneLine(std::wstring value) {
    for (wchar_t& character : value) {
        if (character == L'\r' || character == L'\n' || character == L'\t') character = L' ';
    }
    if (value.size() > 160) {
        value.resize(159);
        value.push_back(L'…');
    }
    return value;
}

std::wstring trimWide(std::wstring value) {
    while (!value.empty() && ::iswspace(value.front()) != 0) value.erase(value.begin());
    while (!value.empty() && ::iswspace(value.back()) != 0) value.pop_back();
    return value;
}

std::vector<std::string> parseExtensions(std::wstring value) {
    std::replace(value.begin(), value.end(), L';', L',');
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(L',', start);
        std::wstring extension = trimWide(value.substr(start, end - start));
        if (!extension.empty()) {
            if (extension.front() != L'.') extension.insert(extension.begin(), L'.');
            result.push_back(wideToUtf8(extension));
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::wstring joinExtensions(const Json& item) {
    std::wstring result;
    const auto extensions = item.find("fileExtensions");
    if (extensions == item.end() || !extensions->is_array()) return result;
    for (const auto& extension : *extensions) {
        if (!extension.is_string()) continue;
        if (!result.empty()) result.append(L", ");
        result.append(utf8ToWide(extension.get<std::string>()));
    }
    return result;
}

std::vector<std::string> parseFilterValues(std::wstring value) {
    std::replace(value.begin(), value.end(), L';', L',');
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(L',', start);
        std::wstring entry = trimWide(value.substr(start, end - start));
        if (!entry.empty()) result.push_back(wideToUtf8(entry));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::wstring joinStringArray(const Json& item, std::string_view field) {
    std::wstring result;
    const auto values = item.find(std::string(field));
    if (values == item.end() || !values->is_array()) return result;
    for (const auto& value : *values) {
        if (!value.is_string()) continue;
        if (!result.empty()) result.append(L", ");
        result.append(utf8ToWide(value.get<std::string>()));
    }
    return result;
}
bool hasActivation(const Json& item, std::string_view name) {
    const auto activation = item.find("activation");
    if (activation == item.end() || !activation->is_array()) {
        return name == "space" || name == "enter" || name == "tab";
    }
    return std::any_of(activation->begin(), activation->end(), [&](const Json& entry) {
        return entry.is_string() && entry.get<std::string>() == name;
    });
}

std::wstring activationSummary(const Json& item) {
    const std::array<std::pair<std::string_view, std::wstring_view>, 5> names{{
        {"space", L"Space"}, {"enter", L"Enter"}, {"tab", L"Tab"},
        {"punctuation", L"Punctuation"}, {"immediate", L"Immediate"},
    }};
    std::wstring result;
    for (const auto& [key, label] : names) {
        if (!hasActivation(item, key)) continue;
        if (!result.empty()) result.append(L", ");
        result.append(localization::text(label.data()));
    }
    if (item.value("matchMode", "wholeWord") == "captureTemplate") {
        if (!result.empty()) result.append(L" · ");
        result.append(L"Capture");
    }
    return result;
}

void replaceAllWide(std::wstring& text, std::wstring_view marker, std::wstring_view value) {
    std::size_t position = text.find(marker);
    while (position != std::wstring::npos) {
        text.replace(position, marker.size(), value);
        position = text.find(marker, position + value.size());
    }
}
std::string makeGuid() {
    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid))) return "rule-" + std::to_string(::GetTickCount64());
    wchar_t buffer[40]{};
    ::StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    std::wstring value(buffer);
    if (!value.empty() && value.front() == L'{') value.erase(value.begin());
    if (!value.empty() && value.back() == L'}') value.pop_back();
    return wideToUtf8(value);
}

struct CachedRuleRow {
    std::size_t documentIndex = 0;
    std::array<std::wstring, 5> columns;
    std::wstring searchText;
};
class RuleManagerWindow {
public:
    explicit RuleManagerWindow(RuleManagerOptions options) : options_(std::move(options)) {}

    bool create() {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_DBLCLKS;
        windowClass.lpfnWndProc = &RuleManagerWindow::windowProcedure;
        windowClass.hInstance = options_.module;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kWindowClass;
        ::RegisterClassExW(&windowClass);

        RECT ownerRect{};
        ::GetWindowRect(options_.notepadHandle, &ownerRect);
        int width = 1120;
        int height = 760;
        int x = ownerRect.left + std::max(20L, (ownerRect.right - ownerRect.left - width) / 2);
        int y = ownerRect.top + std::max(20L, (ownerRect.bottom - ownerRect.top - height) / 2);
        bool maximize = false;
        loadWindowState(x, y, width, height, maximize);
        window_ = ::CreateWindowExW(
            WS_EX_CONTROLPARENT, kWindowClass, localization::text(kWindowTitle),
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            x, y, width, height, options_.notepadHandle, nullptr, options_.module, this);
        if (window_ == nullptr) return false;
        gManagerWindow = window_;
        ::ShowWindow(window_, maximize ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
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
            static_cast<WPARAM>(NppDarkMode::dmfHandleChange), reinterpret_cast<LPARAM>(window_));
        ::SetWindowPos(window_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        ::RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
    }

private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<RuleManagerWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<RuleManagerWindow*>(create->lpCreateParams);
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
            updateFonts();
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {scale(900), scale(680)};
            return 0;
        case WM_COMMAND: onCommand(LOWORD(wParam), HIWORD(wParam)); return 0;
        case WM_NOTIFY: return onNotify(reinterpret_cast<NMHDR*>(lParam));
        case WM_TIMER:
            if (wParam == kExternalChangeTimer) checkExternalChange();
            else if (wParam == kSearchTimer) {
                ::KillTimer(window_, kSearchTimer);
                refreshList();
            }
            return 0;
        case WM_CLOSE:
            if (isGroupManagerOpen()) {
                pendingClose_ = true;
                closeGroupManager(gDiscardManagerChanges);
                return 0;
            }
            if (gDiscardManagerChanges) {
                ::DestroyWindow(window_);
                return 0;
            }
            if (confirmClose()) ::DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            saveWindowState();
            ::KillTimer(window_, kExternalChangeTimer);
            ::KillTimer(window_, kSearchTimer);
            ::SendMessageW(options_.notepadHandle, NPPM_MODELESSDIALOG,
                MODELESSDIALOGREMOVE, reinterpret_cast<LPARAM>(window_));
            return 0;
        case WM_NCDESTROY:
            if (baseFont_ != nullptr) ::DeleteObject(baseFont_);
            if (headingFont_ != nullptr) ::DeleteObject(headingFont_);
            gManagerWindow = nullptr;
            gDiscardManagerChanges = false;
            ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            delete this;
            return 0;
        default: return ::DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    std::filesystem::path windowStatePath() const {
        return options_.dataDirectory / L"manager-window.json";
    }

    void loadWindowState(int& x, int& y, int& width, int& height, bool& maximize) const {
        std::string content;
        std::string error;
        if (!ConfigStore::readUtf8File(windowStatePath(), content, error)) return;
        try {
            const Json state = Json::parse(content);
            width = std::clamp(state.value("width", width), 900, 3840);
            height = std::clamp(state.value("height", height), 680, 2160);
            x = state.value("x", x);
            y = state.value("y", y);
            maximize = state.value("maximized", false);
            RECT requested{x, y, x + width, y + height};
            const HMONITOR monitor = ::MonitorFromRect(&requested, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{sizeof(info)};
            if (::GetMonitorInfoW(monitor, &info)) {
                const int workLeft = static_cast<int>(info.rcWork.left);
                const int workTop = static_cast<int>(info.rcWork.top);
                const int workRight = static_cast<int>(info.rcWork.right);
                const int workBottom = static_cast<int>(info.rcWork.bottom);
                width = std::min(width, workRight - workLeft);
                height = std::min(height, workBottom - workTop);
                x = std::clamp(x, workLeft, workRight - width);
                y = std::clamp(y, workTop, workBottom - height);
            }
        } catch (...) {
            // A malformed optional UI state file must never block the manager.
        }
    }

    void saveWindowState() const {
        if (window_ == nullptr || !::IsWindow(window_)) return;
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (!::GetWindowPlacement(window_, &placement)) return;
        const RECT& rect = placement.rcNormalPosition;
        const Json state{
            {"x", rect.left}, {"y", rect.top},
            {"width", rect.right - rect.left}, {"height", rect.bottom - rect.top},
            {"maximized", placement.showCmd == SW_SHOWMAXIMIZED},
        };
        std::string error;
        ConfigStore::writeUtf8FileAtomic(windowStatePath(), state.dump(2), error);
    }
    HWND makeControl(DWORD exStyle, const wchar_t* className, const wchar_t* text, DWORD style, int id) {
        HWND control = ::CreateWindowExW(exStyle, className, localization::text(text),
            WS_CHILD | WS_VISIBLE | style,
            0, 0, 10, 10, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), options_.module, nullptr);
        controls_.push_back(control);
        return control;
    }

    HWND makeLabel(const wchar_t* text) {
        return makeControl(0, WC_STATICW, text, SS_LEFT | SS_NOPREFIX, 0);
    }

    HWND makeButton(const wchar_t* text, int id, DWORD style = BS_PUSHBUTTON) {
        return makeControl(0, WC_BUTTONW, text, WS_TABSTOP | style, id);
    }

    bool onCreate() {
        INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        ::InitCommonControlsEx(&commonControls);
        dpi_ = ::GetDpiForWindow(window_);

        title_ = makeLabel(L"Replacement rules");
        subtitle_ = makeLabel(L"Search, edit, validate, and save without touching raw JSON.");
        searchLabel_ = makeLabel(L"Search");
        search_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, idSearch);
        ::SendMessageW(search_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(localization::text(L"Trigger, replacement, group, or note")));
        groupFilterLabel_ = makeLabel(L"Group");
        groupFilter_ = makeControl(0, WC_COMBOBOXW, L"",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, idGroupFilter);

        list_ = makeControl(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA, idRuleList);
        ListView_SetExtendedListViewStyle(list_,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ::SetWindowTheme(list_, L"Explorer", nullptr);
        addListColumn(0, L"Effective state", 96);
        addListColumn(1, L"Trigger", 132);
        addListColumn(2, L"Replacement", 230);
        addListColumn(3, L"Group", 112);
        addListColumn(4, L"Activation", 150);
        listStatus_ = makeLabel(L"");
        add_ = makeButton(L"Add rule", idAdd);
        duplicate_ = makeButton(L"Duplicate", idDuplicate);
        delete_ = makeButton(L"Delete", idDelete);
        manageGroups_ = makeButton(L"&Groups…", idManageGroups);
        setState_ = makeButton(L"Set &state…", idSetState);

        detailsTitle_ = makeLabel(L"Rule details");
        detailsHint_ = makeLabel(L"Changes stay in the draft until you save the document.");
        enabled_ = makeButton(L"Rule enabled", idEnabled, BS_AUTOCHECKBOX);
        triggerLabel_ = makeLabel(L"Trigger / capture template");
        trigger_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idTrigger);
        ::SendMessageW(trigger_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(localization::text(L"Literal trigger, or ticket-${capture:1}")));
        groupLabel_ = makeLabel(L"Group ID");
        group_ = makeControl(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
            WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL, idGroup);
        activationLabel_ = makeLabel(L"Replace when");
        space_ = makeButton(L"Space", idSpace, BS_AUTOCHECKBOX);
        enter_ = makeButton(L"Enter", idEnter, BS_AUTOCHECKBOX);
        tab_ = makeButton(L"Tab", idTab, BS_AUTOCHECKBOX);
        punctuation_ = makeButton(L"Punctuation", idPunctuation, BS_AUTOCHECKBOX);
        immediate_ = makeButton(L"Immediate", idImmediate, BS_AUTOCHECKBOX);
        caseSensitive_ = makeButton(L"Case sensitive", idCaseSensitive, BS_AUTOCHECKBOX);
        captureTemplate_ = makeButton(L"Capture template", idCaptureTemplate, BS_AUTOCHECKBOX);
        extensionsLabel_ = makeLabel(L"File extensions");
        extensions_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idExtensions);
        ::SendMessageW(extensions_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(localization::text(L"All files, or .txt, .md, .xml")));
        pathGlobsLabel_ = makeLabel(L"Path globs");
        pathGlobs_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idPathGlobs);
        ::SendMessageW(pathGlobs_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(localization::text(L"Example: */docs/*, C:/work/*.md")));
        languagesLabel_ = makeLabel(L"Languages");
        languages_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idLanguages);
        ::SendMessageW(languages_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(localization::text(L"Example: Python, Markdown")));        replacementLabel_ = makeLabel(L"Replacement text");
        replacement_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
            idReplacement);
        descriptionLabel_ = makeLabel(L"Description / note");
        description_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, idDescription);
        detailStatus_ = makeLabel(L"Select a rule to edit it.");
        preview_ = makeButton(L"&Preview…", idPreview);
        applyDraft_ = makeButton(L"&Apply to draft", idApplyDraft);

        divider_ = makeControl(0, WC_STATICW, L"", SS_ETCHEDHORZ, 0);
        save_ = makeButton(L"Save changes", idSave, BS_DEFPUSHBUTTON);
        reload_ = makeButton(L"Reload from disk", idReload);
        restore_ = makeButton(L"Restore backup…", idRestore);
        openJson_ = makeButton(L"Open JSON", idOpenJson);
        import_ = makeButton(L"Import…", idImport);
        export_ = makeButton(L"Export…", idExport);
        close_ = makeButton(L"Close", idClose);

        updateFonts();
        if (!loadDocument(true)) {
            populateGroupFilter();
            populateGroupEditor();
            refreshList();
        }
        setDetailsEnabled(false);
        ::SendMessageW(options_.notepadHandle, NPPM_MODELESSDIALOG,
            MODELESSDIALOGADD, reinterpret_cast<LPARAM>(window_));
        ::SendMessageW(options_.notepadHandle, NPPM_DARKMODESUBCLASSANDTHEME,
            static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(window_));
        ::SetTimer(window_, kExternalChangeTimer, kExternalChangePollMs, nullptr);
        return true;
    }

    void addListColumn(int index, const wchar_t* text, int width) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(localization::text(text));
        column.cx = width;
        column.iSubItem = index;
        ListView_InsertColumn(list_, index, &column);
    }

    int scale(int value) const {
        return ::MulDiv(value, static_cast<int>(dpi_), 96);
    }

    void place(HWND control, int x, int y, int width, int height) const {
        ::SetWindowPos(control, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void layout(int width, int height) {
        if (width <= 0 || height <= 0 || list_ == nullptr) return;
        const int margin = scale(16);
        const int gap = scale(16);
        const int footerHeight = scale(58);
        const int contentBottom = height - footerHeight;
        const int available = width - margin * 2 - gap;
        const int leftWidth = std::max(scale(430), available * 54 / 100);
        const int rightX = margin + leftWidth + gap;
        const int rightWidth = std::max(scale(360), width - rightX - margin);
        const int row = scale(28);

        place(title_, margin, margin, leftWidth, scale(26));
        place(subtitle_, margin, margin + scale(28), leftWidth, scale(20));
        const int filterY = margin + scale(58);
        place(searchLabel_, margin, filterY, scale(52), row);
        const int searchWidth = std::max(scale(170), leftWidth - scale(52 + 12 + 54 + 144));
        place(search_, margin + scale(52), filterY, searchWidth, row);
        const int groupLabelX = margin + scale(52) + searchWidth + scale(12);
        place(groupFilterLabel_, groupLabelX, filterY, scale(54), row);
        place(groupFilter_, groupLabelX + scale(54), filterY, scale(144), scale(220));

        const int actionY = contentBottom - scale(38);
        const int statusY = actionY - scale(26);
        const int listY = filterY + scale(40);
        place(list_, margin, listY, leftWidth, std::max(scale(160), statusY - listY - scale(4)));
        place(listStatus_, margin, statusY, leftWidth, scale(20));
        place(add_, margin, actionY, scale(82), scale(30));
        place(duplicate_, margin + scale(90), actionY, scale(82), scale(30));
        place(delete_, margin + scale(180), actionY, scale(72), scale(30));
        place(manageGroups_, margin + scale(260), actionY, scale(82), scale(30));
        place(setState_, margin + scale(350), actionY, scale(92), scale(30));

        place(detailsTitle_, rightX, margin, rightWidth, scale(26));
        place(detailsHint_, rightX, margin + scale(28), rightWidth, scale(20));
        place(enabled_, rightX, margin + scale(54), scale(128), scale(24));
        const int fieldsY = margin + scale(86);
        const int half = (rightWidth - scale(12)) / 2;
        place(triggerLabel_, rightX, fieldsY, half, scale(18));
        place(groupLabel_, rightX + half + scale(12), fieldsY, half, scale(18));
        place(trigger_, rightX, fieldsY + scale(20), half, row);
        place(group_, rightX + half + scale(12), fieldsY + scale(20), half, scale(220));

        const int activationY = fieldsY + scale(58);
        place(activationLabel_, rightX, activationY, rightWidth, scale(18));
        const int checkY = activationY + scale(20);
        place(space_, rightX, checkY, scale(72), scale(24));
        place(enter_, rightX + scale(72), checkY, scale(70), scale(24));
        place(tab_, rightX + scale(142), checkY, scale(60), scale(24));
        place(punctuation_, rightX + scale(202), checkY, scale(106), scale(24));
        place(immediate_, rightX + scale(308), checkY, scale(94), scale(24));
        place(caseSensitive_, rightX, checkY + scale(28), scale(126), scale(24));
        place(captureTemplate_, rightX + scale(134), checkY + scale(28), scale(142), scale(24));

        const int extensionsY = checkY + scale(58);
        place(extensionsLabel_, rightX, extensionsY, rightWidth, scale(18));
        place(extensions_, rightX, extensionsY + scale(20), rightWidth, row);
        const int filtersY = extensionsY + scale(58);
        place(pathGlobsLabel_, rightX, filtersY, half, scale(18));
        place(languagesLabel_, rightX + half + scale(12), filtersY, half, scale(18));
        place(pathGlobs_, rightX, filtersY + scale(20), half, row);
        place(languages_, rightX + half + scale(12), filtersY + scale(20), half, row);
        const int replacementY = filtersY + scale(58);
        place(replacementLabel_, rightX, replacementY, rightWidth, scale(18));
        const int fixedBelow = scale(18 + 66 + 22 + 32 + 28);
        const int replacementHeight = std::max(scale(100),
            actionY - replacementY - scale(20) - fixedBelow);
        place(replacement_, rightX, replacementY + scale(20), rightWidth, replacementHeight);
        const int descriptionY = replacementY + scale(20) + replacementHeight + scale(8);
        place(descriptionLabel_, rightX, descriptionY, rightWidth, scale(18));
        place(description_, rightX, descriptionY + scale(20), rightWidth, scale(58));
        const int detailBottomY = descriptionY + scale(82);
        place(detailStatus_, rightX, detailBottomY, rightWidth - scale(230), scale(28));
        place(preview_, rightX + rightWidth - scale(222), detailBottomY, scale(100), scale(30));
        place(applyDraft_, rightX + rightWidth - scale(112), detailBottomY, scale(112), scale(30));

        place(divider_, margin, contentBottom + scale(8), width - margin * 2, scale(2));
        const int footerY = contentBottom + scale(18);
        place(save_, margin, footerY, scale(118), scale(32));
        place(reload_, margin + scale(128), footerY, scale(128), scale(32));
        place(restore_, margin + scale(266), footerY, scale(132), scale(32));
        place(openJson_, margin + scale(408), footerY, scale(100), scale(32));
        place(import_, margin + scale(518), footerY, scale(88), scale(32));
        place(export_, margin + scale(616), footerY, scale(88), scale(32));
        place(close_, width - margin - scale(90), footerY, scale(90), scale(32));
        RECT listClient{};
        ::GetClientRect(list_, &listClient);
        const int fixedColumns = scale(96 + 132 + 112 + 150);
        const int scrollbar = ::GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);

        ListView_SetColumnWidth(list_, 0, scale(96));
        ListView_SetColumnWidth(list_, 1, scale(132));
        ListView_SetColumnWidth(list_, 2,
            std::max(scale(160),
                static_cast<int>(listClient.right) - fixedColumns - scrollbar - scale(6)));
        ListView_SetColumnWidth(list_, 3, scale(112));
        ListView_SetColumnWidth(list_, 4, scale(150));
    }

    void updateFonts() {
        dpi_ = ::GetDpiForWindow(window_);
        HFONT oldBase = baseFont_;
        HFONT oldHeading = headingFont_;
        baseFont_ = ::CreateFontW(-::MulDiv(9, static_cast<int>(dpi_), 72), 0, 0, 0,
            FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        headingFont_ = ::CreateFontW(-::MulDiv(15, static_cast<int>(dpi_), 72), 0, 0, 0,
            FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        for (HWND control : controls_) {
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(baseFont_), TRUE);
        }
        ::SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
        ::SendMessageW(detailsTitle_, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
        if (oldBase != nullptr) ::DeleteObject(oldBase);
        if (oldHeading != nullptr) ::DeleteObject(oldHeading);
    }

    bool loadDocument(bool showError) {
        std::string content;
        std::string error;
        if (!ConfigStore::readUtf8File(options_.replacementsPath, content, error)) {
            if (showError) showMessage(utf8ToWide(error), MB_ICONERROR);
            return false;
        }
        try {
            Json parsed = Json::parse(content);
            RuleStore validator;
            const RuleLoadResult validation = validator.loadFromText(parsed.dump());
            if (!validation.ok) throw std::runtime_error(validation.error);
            document_ = std::move(parsed);
            loadedContentHash_ = ConfigStore::contentHash(content);
            std::string stampError;
            if (!ConfigStore::fileStamp(options_.replacementsPath, loadedFileStamp_, stampError)) {
                loadedFileStamp_ = {};
            }
            externalChangeDetected_ = false;
            setText(subtitle_, localization::text(L"Search, edit, validate, and save without touching raw JSON."));
            dirty_ = false;
            detailsDirty_ = false;
            selectedDocumentIndex_.reset();
            populateGroupFilter();
            populateGroupEditor();
            refreshList();
            setDetailsEnabled(false);
            updateWindowTitle();
            setDetailStatus(validation.warnings.empty()
                ? L"Loaded and validated."
                : (isKoreanUi()
                    ? L"경고 " + std::to_wstring(validation.warnings.size()) + L"개와 함께 불러왔어요."
                    : L"Loaded with " + std::to_wstring(validation.warnings.size()) + L" warning(s)."));
            return true;
        } catch (const std::exception& exception) {
            if (showError) {
                showMessage(L"replacements.json could not be opened in the manager.\n\n" +
                    utf8ToWide(exception.what()), MB_ICONERROR);
            }
            return false;
        }
    }

    void populateGroupFilter() {
        if (groupFilter_ == nullptr) return;
        loading_ = true;
        ::SendMessageW(groupFilter_, CB_RESETCONTENT, 0, 0);
        filterGroups_.clear();
        filterGroups_.push_back({});
        ::SendMessageW(groupFilter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(localization::text(L"All groups")));
        const auto groups = document_.find("groups");
        if (groups != document_.end() && groups->is_array()) {
            for (const auto& group : *groups) {
                if (!group.is_object()) continue;
                const std::string id = group.value("id", "");
                if (id.empty()) continue;
                const std::string name = group.value("name", id);
                filterGroups_.push_back(id);
                const std::wstring label = utf8ToWide(name) + L"  ·  " + utf8ToWide(id);
                ::SendMessageW(groupFilter_, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(label.c_str()));
            }
        }
        ::SendMessageW(groupFilter_, CB_SETCURSEL, 0, 0);
        loading_ = false;
    }

    void populateGroupEditor() {
        if (group_ == nullptr) return;
        const std::wstring current = windowText(group_);
        loading_ = true;
        ::SendMessageW(group_, CB_RESETCONTENT, 0, 0);
        const auto groups = document_.find("groups");
        if (groups != document_.end() && groups->is_array()) {
            for (const auto& group : *groups) {
                if (!group.is_object()) continue;
                const std::string id = group.value("id", "");
                if (id.empty()) continue;
                const std::wstring wideId = utf8ToWide(id);
                ::SendMessageW(
                    group_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wideId.c_str()));
            }
        }
        setText(group_, current);
        loading_ = false;
    }

    void refreshList() {
        if (list_ == nullptr || !document_.contains("items") || !document_["items"].is_array()) return;
        loading_ = true;
        visibleRows_.clear();
        const std::wstring query = lowerWide(windowText(search_));
        const LRESULT selection = ::SendMessageW(groupFilter_, CB_GETCURSEL, 0, 0);
        const std::string groupFilter = selection >= 0 &&
                static_cast<std::size_t>(selection) < filterGroups_.size()
            ? filterGroups_[static_cast<std::size_t>(selection)]
            : std::string{};

        std::unordered_map<std::string, bool> groupStates;
        if (document_.contains("groups") && document_["groups"].is_array()) {
            groupStates.reserve(document_["groups"].size());
            for (const auto& group : document_["groups"]) {
                if (group.is_object()) {
                    groupStates[group.value("id", "")] = group.value("enabled", true);
                }
            }
        }

        const Json& items = document_["items"];
        visibleRows_.reserve(items.size());
        for (std::size_t index = 0; index < items.size(); ++index) {
            const Json& item = items[index];
            const std::string group = item.value("group", "");
            if (!groupFilter.empty() && group != groupFilter) continue;

            CachedRuleRow row;
            row.documentIndex = index;
            const auto groupState = groupStates.find(group);
            if (!item.value("enabled", true)) row.columns[0] = localization::text(L"Rule off");
            else if (!group.empty() && groupState == groupStates.end()) row.columns[0] = localization::text(L"Missing group");
            else if (groupState != groupStates.end() && !groupState->second) row.columns[0] = localization::text(L"Group off");
            else row.columns[0] = localization::text(L"On");
            row.columns[1] = utf8ToWide(item.value("trigger", ""));
            const std::wstring fullReplacement = utf8ToWide(item.value("replacement", ""));
            row.columns[2] = oneLine(fullReplacement);
            row.columns[3] = group.empty() ? L"—" : utf8ToWide(group);
            row.columns[4] = activationSummary(item);
            row.searchText = lowerWide(
                row.columns[1] + L"\n" + fullReplacement + L"\n" + row.columns[3] + L"\n" +
                joinStringArray(item, "pathGlobs") + L"\n" +
                joinStringArray(item, "languages") + L"\n" +
                utf8ToWide(item.value("description", "")));
            if (!query.empty() && row.searchText.find(query) == std::wstring::npos) continue;
            visibleRows_.push_back(std::move(row));
        }
        sortVisibleRows();
        ListView_SetItemCountEx(list_, static_cast<int>(visibleRows_.size()),
            LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        ::InvalidateRect(list_, nullptr, FALSE);
        setText(listStatus_, isKoreanUi()
            ? L"전체 " + std::to_wstring(items.size()) + L"개 중 " +
                std::to_wstring(visibleRows_.size()) + L"개 규칙 표시"
            : std::to_wstring(visibleRows_.size()) + L" of " +
                std::to_wstring(items.size()) + L" rules shown");
        loading_ = false;
        if (selectedDocumentIndex_.has_value()) selectDocumentIndex(*selectedDocumentIndex_);
    }

    void sortVisibleRows() {
        const std::size_t column = static_cast<std::size_t>(std::clamp(sortColumn_, 0, 4));
        std::sort(visibleRows_.begin(), visibleRows_.end(), [&](const auto& left, const auto& right) {
            const std::wstring& leftText = left.columns[column];
            const std::wstring& rightText = right.columns[column];
            const int comparison = ::CompareStringOrdinal(
                leftText.c_str(), static_cast<int>(leftText.size()),
                rightText.c_str(), static_cast<int>(rightText.size()), TRUE);
            if (comparison == CSTR_EQUAL) return left.documentIndex < right.documentIndex;
            return sortAscending_ ? comparison == CSTR_LESS_THAN : comparison == CSTR_GREATER_THAN;
        });
    }

    std::optional<std::size_t> documentIndexForRow(int row) const {
        if (row < 0 || static_cast<std::size_t>(row) >= visibleRows_.size()) return std::nullopt;
        return visibleRows_[static_cast<std::size_t>(row)].documentIndex;
    }

    void selectDocumentIndex(std::size_t documentIndex) {
        for (std::size_t row = 0; row < visibleRows_.size(); ++row) {
            if (visibleRows_[row].documentIndex == documentIndex) {
                ListView_SetItemState(list_, static_cast<int>(row), LVIS_SELECTED | LVIS_FOCUSED,
                    LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(list_, static_cast<int>(row), FALSE);
                return;
            }
        }
    }
    void loadDetails(std::size_t index) {
        if (!document_.contains("items") || index >= document_["items"].size()) {
            selectedDocumentIndex_.reset();
            setDetailsEnabled(false);
            return;
        }
        selectedDocumentIndex_ = index;
        const Json& item = document_["items"][index];
        loading_ = true;
        setCheck(enabled_, item.value("enabled", true));
        setText(trigger_, utf8ToWide(item.value("trigger", "")));
        setText(group_, utf8ToWide(item.value("group", "")));
        setCheck(space_, hasActivation(item, "space"));
        setCheck(enter_, hasActivation(item, "enter"));
        setCheck(tab_, hasActivation(item, "tab"));
        setCheck(punctuation_, hasActivation(item, "punctuation"));
        setCheck(immediate_, hasActivation(item, "immediate"));
        setCheck(caseSensitive_, item.value("caseSensitive", false));
        setCheck(captureTemplate_, item.value("matchMode", "wholeWord") == "captureTemplate");
        setText(extensions_, joinExtensions(item));
        setText(pathGlobs_, joinStringArray(item, "pathGlobs"));
        setText(languages_, joinStringArray(item, "languages"));
        setText(replacement_, utf8ToWide(item.value("replacement", "")));
        setText(description_, utf8ToWide(item.value("description", "")));
        loading_ = false;
        detailsDirty_ = false;
        setDetailsEnabled(true);
        setDetailStatus(L"Ready to edit.");
    }

    void setCheck(HWND control, bool checked) const {
        ::SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    bool isChecked(HWND control) const {
        return ::SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    void setDetailsEnabled(bool enabled) {
        const std::array<HWND, 17> editable{{enabled_, trigger_, group_, space_, enter_, tab_,
            punctuation_, immediate_, caseSensitive_, captureTemplate_, extensions_, pathGlobs_,
            languages_, replacement_, description_, preview_, applyDraft_}};
        for (HWND control : editable) ::EnableWindow(control, enabled);
        if (!enabled) {
            loading_ = true;
            for (HWND control : {trigger_, group_, extensions_, pathGlobs_, languages_,
                    replacement_, description_}) setText(control, L"");
            for (HWND control : {enabled_, space_, enter_, tab_, punctuation_, immediate_,
                    caseSensitive_, captureTemplate_}) setCheck(control, false);
            loading_ = false;
            setDetailStatus(L"Select a rule to edit it.");
        }
    }

    bool commitDetails(bool showError, bool refresh) {
        if (!selectedDocumentIndex_.has_value() || !detailsDirty_) return true;
        const std::size_t index = *selectedDocumentIndex_;
        if (index >= document_["items"].size()) return false;
        Json oldItem = document_["items"][index];
        Json oldGroups = document_.value("groups", Json::array());
        Json& item = document_["items"][index];
        const std::string trigger = wideToUtf8(windowText(trigger_));
        const std::string replacement = wideToUtf8(windowText(replacement_));
        if (trigger.empty() || replacement.empty()) {
            if (showError) showMessage(L"Trigger and replacement text are both required.", MB_ICONWARNING);
            ::SetFocus(trigger.empty() ? trigger_ : replacement_);
            return false;
        }

        item["enabled"] = isChecked(enabled_);
        item["trigger"] = trigger;
        item["replacement"] = replacement;
        const std::string groupId = wideToUtf8(trimWide(windowText(group_)));
        bool groupCreated = false;
        if (!groupId.empty()) {
            if (!document_.contains("groups") || !document_["groups"].is_array()) {
                document_["groups"] = Json::array();
            }
            const bool exists = std::any_of(
                document_["groups"].begin(),
                document_["groups"].end(),
                [&](const Json& group) {
                    return group.is_object() && group.value("id", "") == groupId;
                });
            if (!exists) {
                document_["groups"].push_back(
                    {{"id", groupId}, {"name", groupId}, {"enabled", true}});
                groupCreated = true;
            }
        }
        item["group"] = groupId;
        item["matchMode"] = isChecked(captureTemplate_) ? "captureTemplate" : "wholeWord";
        item["caseSensitive"] = isChecked(caseSensitive_);
        item["description"] = wideToUtf8(windowText(description_));
        if (!item.contains("id") || !item["id"].is_string() || item["id"].get<std::string>().empty()) {
            item["id"] = makeGuid();
        }
        Json activation = Json::array();
        const auto addActivation = [&](HWND control, const char* name) {
            if (isChecked(control)) activation.push_back(name);
        };
        addActivation(space_, "space");
        addActivation(enter_, "enter");
        addActivation(tab_, "tab");
        addActivation(punctuation_, "punctuation");
        addActivation(immediate_, "immediate");
        item["activation"] = std::move(activation);
        item["fileExtensions"] = parseExtensions(windowText(extensions_));
        item["pathGlobs"] = parseFilterValues(windowText(pathGlobs_));
        item["languages"] = parseFilterValues(windowText(languages_));

        RuleStore validator;
        const RuleLoadResult validation = validator.loadFromText(document_.dump());
        if (!validation.ok) {
            item = std::move(oldItem);
            document_["groups"] = std::move(oldGroups);
            if (showError) {
                showMessage(L"This draft change is not valid yet.\n\n" +
                    utf8ToWide(validation.error), MB_ICONWARNING);
            }
            setDetailStatus(L"Fix the current rule before switching or saving.");
            return false;
        }
        detailsDirty_ = false;
        dirty_ = true;
        updateWindowTitle();
        if (groupCreated) {
            populateGroupFilter();
            populateGroupEditor();
            setText(group_, utf8ToWide(groupId));
            setDetailStatus(isKoreanUi()
                ? L"초안에 적용하고 그룹 '" + utf8ToWide(groupId) + L"'을(를) 만들었어요."
                : L"Applied to draft and created group '" + utf8ToWide(groupId) + L"'.");
        } else {
            setDetailStatus(validation.warnings.empty()
                ? L"Applied to draft. Save changes to write the file."
                : (isKoreanUi()
                    ? L"경고 " + std::to_wstring(validation.warnings.size()) + L"개와 함께 초안에 적용했어요."
                    : L"Applied with " + std::to_wstring(validation.warnings.size()) + L" warning(s)."));
        }
        if (refresh) {
            refreshList();
            selectDocumentIndex(index);
        }
        return true;
    }

    bool saveDocument() {
        if (!commitDetails(true, false)) return false;
        RuleStore validator;
        const RuleLoadResult validation = validator.loadFromText(document_.dump());
        if (!validation.ok) {
            showMessage(L"The rule document is not valid.\n\n" +
                utf8ToWide(validation.error), MB_ICONERROR);
            return false;
        }
        std::string error;
        std::filesystem::path backup;
        if (options_.config.backupEnabled &&
            !ConfigStore::backupReplacements(options_.dataDirectory, options_.replacementsPath,
                options_.config.maxBackupFiles, backup, error)) {
            showMessage(L"The current rules could not be backed up, so nothing was overwritten.\n\n" +
                utf8ToWide(error), MB_ICONERROR);
            return false;
        }
        const std::string serialized = document_.dump(2);
        const AtomicWriteResult writeResult = ConfigStore::writeUtf8FileAtomicIfUnchanged(
            options_.replacementsPath, loadedContentHash_, serialized, error);
        if (writeResult == AtomicWriteResult::conflict) {
            externalChangeDetected_ = true;
            const int choice = ::MessageBoxW(window_,
                localization::text(
                    L"replacements.json changed outside this manager.\n\n"
                    L"Yes: reload the newer file and discard this draft\n"
                    L"No: save this draft as a separate JSON file\n"
                    L"Cancel: keep editing without saving",
                    L"이 관리자 창 밖에서 replacements.json이 변경됐어요.\n\n"
                    L"예: 새 파일을 다시 불러오고 현재 초안 버리기\n"
                    L"아니요: 현재 초안을 별도 JSON 파일로 저장하기\n"
                    L"취소: 저장하지 않고 계속 편집하기"),
                localization::text(kWindowTitle), MB_YESNOCANCEL | MB_ICONWARNING);
            if (choice == IDYES) return loadDocument(true);
            if (choice == IDNO) return saveDraftCopy(serialized);
            setDetailStatus(L"Save cancelled · the external file was not overwritten.");
            return false;
        }
        if (writeResult == AtomicWriteResult::failed) {
            showMessage(L"The rules could not be saved.\n\n" + utf8ToWide(error), MB_ICONERROR);
            return false;
        }
        loadedContentHash_ = ConfigStore::contentHash(serialized);
        std::string stampError;
        if (!ConfigStore::fileStamp(options_.replacementsPath, loadedFileStamp_, stampError)) {
            loadedFileStamp_ = {};
        }
        externalChangeDetected_ = false;
        setText(subtitle_, localization::text(L"Search, edit, validate, and save without touching raw JSON."));
        dirty_ = false;
        detailsDirty_ = false;
        updateWindowTitle();
        if (options_.onRulesSaved) options_.onRulesSaved();
        std::wstring status = isKoreanUi()
            ? L"규칙 " + std::to_wstring(validation.loadedCount) + L"개 저장"
            : L"Saved " + std::to_wstring(validation.loadedCount) + L" rules";
        if (!backup.empty()) {
            status += localization::text(L" · backup created", L" · 백업 생성");
        }
        if (!validation.warnings.empty()) {
            status += L" · " + std::to_wstring(validation.warnings.size()) +
                localization::text(L" warning(s)", L"개 경고");
        }
        setDetailStatus(status + L".");
        refreshList();
        return true;
    }

    bool saveDraftCopy(std::string_view serialized) {
        wchar_t path[32768]{};
        const std::filesystem::path suggested =
            options_.replacementsPath.parent_path() / L"replacements.draft.json";
        wcsncpy_s(path, suggested.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = localization::text(
            L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0\0", L"JSON 파일 (*.json)\0*.json\0모든 파일 (*.*)\0*.*\0\0");
        dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.lpstrDefExt = L"json";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
            OFN_DONTADDTORECENT | OFN_EXPLORER;
        if (!::GetSaveFileNameW(&dialog)) return false;
        std::string error;
        if (!ConfigStore::writeUtf8FileAtomic(path, serialized, error)) {
            showMessage(L"The draft copy could not be saved.\n\n" + utf8ToWide(error), MB_ICONERROR);
            return false;
        }
        setDetailStatus(L"Draft copy saved · the externally changed file was preserved.");
        return true;
    }

    void checkExternalChange() {
        FileStamp current{};
        std::string error;
        if (!ConfigStore::fileStamp(options_.replacementsPath, current, error)) return;
        if (current == loadedFileStamp_) return;
        loadedFileStamp_ = current;
        std::string content;
        if (!ConfigStore::readUtf8File(options_.replacementsPath, content, error)) return;
        if (ConfigStore::contentHash(content) == loadedContentHash_) return;
        if (!externalChangeDetected_) {
            externalChangeDetected_ = true;
            setText(subtitle_, localization::text(L"Warning: replacements.json changed outside this window."));
            setDetailStatus(
                L"Warning · replacements.json changed outside this window. Reload or save a draft copy.");
        }
    }
    void addRule() {
        if (!commitDetails(true, false)) return;
        clearFilters();
        document_["items"].push_back(Json{
            {"id", makeGuid()}, {"enabled", true},
            {"trigger", uniqueTrigger("new-trigger")}, {"replacement", "New replacement"},
            {"group", ""}, {"matchMode", "wholeWord"}, {"caseSensitive", false},
            {"activation", Json::array({"space", "enter", "tab"})},
            {"fileExtensions", Json::array()}, {"pathGlobs", Json::array()},
            {"languages", Json::array()}, {"description", ""},
        });
        const std::size_t index = document_["items"].size() - 1;
        dirty_ = true;
        updateWindowTitle();
        refreshList();
        selectDocumentIndex(index);
        ::SetFocus(trigger_);
        ::SendMessageW(trigger_, EM_SETSEL, 0, -1);
    }

    void duplicateRule() {
        if (!selectedDocumentIndex_.has_value() || !commitDetails(true, false)) return;
        clearFilters();
        Json copy = document_["items"][*selectedDocumentIndex_];
        copy["id"] = makeGuid();
        copy["trigger"] = uniqueTrigger(copy.value("trigger", "trigger") + "-copy");
        copy["description"] = copy.value("description", "") + " (copy)";
        document_["items"].push_back(std::move(copy));
        const std::size_t index = document_["items"].size() - 1;
        dirty_ = true;
        updateWindowTitle();
        refreshList();
        selectDocumentIndex(index);
        ::SetFocus(trigger_);
        ::SendMessageW(trigger_, EM_SETSEL, 0, -1);
    }

    std::string uniqueTrigger(std::string base) const {
        const auto exists = [&](std::string_view candidate) {
            const std::string folded = RuleStore::foldAscii(candidate);
            return std::any_of(document_["items"].begin(), document_["items"].end(),
                [&](const Json& item) {
                    return RuleStore::foldAscii(item.value("trigger", "")) == folded;
                });
        };
        if (!exists(base)) return base;
        for (int suffix = 2; suffix < 10'000; ++suffix) {
            const std::string candidate = base + "-" + std::to_string(suffix);
            if (!exists(candidate)) return candidate;
        }
        return base + "-" + std::to_string(::GetTickCount64());
    }

    void deleteSelectedRules() {
        std::vector<std::size_t> indices;
        int row = -1;
        while ((row = ListView_GetNextItem(list_, row, LVNI_SELECTED)) >= 0) {
            if (const auto index = documentIndexForRow(row); index.has_value()) indices.push_back(*index);
        }
        if (indices.empty()) return;
        const std::wstring prompt = indices.size() == 1
            ? localization::text(L"Delete the selected rule from the draft?")
            : (isKoreanUi()
                ? L"선택한 규칙 " + std::to_wstring(indices.size()) + L"개를 초안에서 삭제할까요?"
                : L"Delete " + std::to_wstring(indices.size()) + L" selected rules from the draft?");
        if (::MessageBoxW(window_, prompt.c_str(), localization::text(kWindowTitle),
                MB_YESNO | MB_ICONWARNING) != IDYES) return;
        std::sort(indices.begin(), indices.end(), std::greater<>());
        for (const std::size_t index : indices) {
            document_["items"].erase(
                document_["items"].begin() + static_cast<Json::difference_type>(index));
        }
        selectedDocumentIndex_.reset();
        detailsDirty_ = false;
        dirty_ = true;
        updateWindowTitle();
        refreshList();
        setDetailsEnabled(false);
    }

    void setSelectedState() {
        if (!commitDetails(true, false)) return;
        std::vector<std::size_t> indices;
        int row = -1;
        while ((row = ListView_GetNextItem(list_, row, LVNI_SELECTED)) >= 0) {
            if (const auto index = documentIndexForRow(row); index.has_value()) indices.push_back(*index);
        }
        if (indices.empty()) {
            setDetailStatus(L"Select one or more rules before changing their state.");
            return;
        }
        const int choice = ::MessageBoxW(window_,
            localization::text(L"Change the selected rules?\n\nYes: enable\nNo: disable\nCancel: leave unchanged"),
            localization::text(kWindowTitle), MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL) return;
        const bool enabled = choice == IDYES;
        for (const std::size_t index : indices) document_["items"][index]["enabled"] = enabled;
        dirty_ = true;
        updateWindowTitle();
        refreshList();
        setDetailStatus(isKoreanUi()
            ? L"선택한 규칙 " + std::to_wstring(indices.size()) +
                (enabled ? L"개를 초안에서 사용하도록 설정했어요." :
                           L"개를 초안에서 사용하지 않도록 설정했어요.")
            : std::to_wstring(indices.size()) +
                (enabled ? L" selected rule(s) enabled in the draft." :
                           L" selected rule(s) disabled in the draft."));
    }

    void previewCurrentRule() {
        if (!selectedDocumentIndex_.has_value()) return;
        std::wstring preview = windowText(replacement_);
        replaceAllWide(preview, L"${cursor}", L"│");
        replaceAllWide(preview, L"${date}", L"[current date]");
        replaceAllWide(preview, L"${time}", L"[current time]");
        replaceAllWide(preview, L"${filename}", L"[current filename]");
        replaceAllWide(preview, L"${filepath}", L"[current file path]");
        replaceAllWide(preview, L"${clipboard}", L"[clipboard text]");
        replaceAllWide(preview, L"${selection}", L"[selected text]");
        replaceAllWide(preview, L"${uuid}", L"[new UUID]");
        replaceAllWide(preview, L"${line}", L"[line number]");
        replaceAllWide(preview, L"${column}", L"[column number]");
        for (unsigned int capture = 1; capture <= 9; ++capture) {
            replaceAllWide(preview, L"${capture:" + std::to_wstring(capture) + L"}",
                L"[capture " + std::to_wstring(capture) + L"]");
        }
        replaceAllWide(preview, L"${tabstop:0}", L"│");
        replaceAllWide(preview, L"${tabstop:1}", L"│");
        replaceAllWide(preview, L"${tabstop:2}", L"│");
        replaceAllWide(preview, L"${tabstop:3}", L"│");
        replaceAllWide(preview, L"${tabstop:4}", L"│");
        replaceAllWide(preview, L"${tabstop:5}", L"│");
        replaceAllWide(preview, L"${tabstop:6}", L"│");
        replaceAllWide(preview, L"${tabstop:7}", L"│");
        replaceAllWide(preview, L"${tabstop:8}", L"│");
        replaceAllWide(preview, L"${tabstop:9}", L"│");
        if (preview.size() > 4000) {
            preview.resize(3999);
            preview.push_back(L'…');
        }
        ::MessageBoxW(window_,
            (std::wstring(localization::text(L"Preview (dynamic values are shown in brackets)")) +
                L"\n\n" + preview).c_str(),
            localization::text(L"NppQuickReplace · Rule Preview"), MB_OK | MB_ICONINFORMATION);
    }
    void clearFilters() {
        loading_ = true;
        setText(search_, L"");
        ::SendMessageW(groupFilter_, CB_SETCURSEL, 0, 0);
        loading_ = false;
    }

    void reloadFromDisk() {
        if ((dirty_ || detailsDirty_) && ::MessageBoxW(window_,
                localization::text(L"Discard the current draft and reload replacements.json?"), localization::text(kWindowTitle),
                MB_YESNO | MB_ICONWARNING) != IDYES) return;
        loadDocument(true);
    }

    void manageGroups() {
        if (!commitDetails(true, false)) return;
        const auto selected = selectedDocumentIndex_;
        const bool changed = showGroupManager(
            window_, options_.notepadHandle, options_.module, document_);
        if (changed) {
            dirty_ = true;
            detailsDirty_ = false;
            populateGroupFilter();
            populateGroupEditor();
            refreshList();
            if (selected.has_value() && *selected < document_["items"].size()) {
                loadDetails(*selected);
                selectDocumentIndex(*selected);
            } else {
                selectedDocumentIndex_.reset();
                setDetailsEnabled(false);
            }
            updateWindowTitle();
            setDetailStatus(
                L"Group changes applied to the draft. Save changes to write the file.");
        }
        if (pendingClose_) {
            pendingClose_ = false;
            ::PostMessageW(window_, WM_CLOSE, 0, 0);
        }
    }

    void importRules() {
        if (!commitDetails(true, false)) return;
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = localization::text(
            L"CSV rule files (*.csv)\0*.csv\0TSV rule files (*.tsv)\0*.tsv\0All files (*.*)\0*.*\0\0",
            L"CSV 규칙 파일 (*.csv)\0*.csv\0TSV 규칙 파일 (*.tsv)\0*.tsv\0모든 파일 (*.*)\0*.*\0\0");
        dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.lpstrInitialDir = options_.dataDirectory.c_str();
        dialog.nFilterIndex = 1;
        dialog.Flags =
            OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT | OFN_EXPLORER;
        if (!::GetOpenFileNameW(&dialog)) return;

        const std::filesystem::path selectedPath(path);
        const std::wstring extension = lowerWide(selectedPath.extension().wstring());
        const char delimiter = extension == L".tsv" || dialog.nFilterIndex == 2 ? '\t' : ',';

        std::string content;
        std::string error;
        if (!ConfigStore::readUtf8File(selectedPath, content, error)) {
            showMessage(L"The import file could not be read.\n\n" + utf8ToWide(error), MB_ICONERROR);
            return;
        }
        const int choice = ::MessageBoxW(
            window_,
            localization::text(
                L"How should these rules be imported?\n\n"
                L"Yes  — append to the current draft\n"
                L"No   — replace every rule in the current draft\n"
                L"Cancel — leave the draft unchanged",
                L"이 규칙을 어떻게 가져올까요?\n\n"
                L"예  — 현재 초안에 추가\n"
                L"아니요 — 현재 초안의 모든 규칙 교체\n"
                L"취소 — 초안을 변경하지 않음"),
            localization::text(kWindowTitle),
            MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL) return;
        const DelimitedImportMode mode =
            choice == IDYES ? DelimitedImportMode::append : DelimitedImportMode::replace;

        const RuleExchangeResult imported =
            RuleExchange::importDelimited(document_.dump(), content, delimiter, mode);
        if (!imported.ok) {
            showMessage(L"The rules could not be imported.\n\n" +
                utf8ToWide(imported.error), MB_ICONERROR);
            return;
        }
        try {
            document_ = Json::parse(imported.text);
        } catch (const std::exception& exception) {
            showMessage(L"The imported draft could not be opened.\n\n" +
                utf8ToWide(exception.what()), MB_ICONERROR);
            return;
        }

        dirty_ = true;
        detailsDirty_ = false;
        selectedDocumentIndex_.reset();
        loading_ = true;
        setText(search_, L"");
        loading_ = false;
        populateGroupFilter();
        populateGroupEditor();
        refreshList();
        setDetailsEnabled(false);
        updateWindowTitle();

        std::wstring status;
        if (isKoreanUi()) {
            status = mode == DelimitedImportMode::append
                ? L"가져온 규칙 " + std::to_wstring(imported.itemCount) + L"개를 초안에 추가"
                : L"초안을 가져온 규칙 " + std::to_wstring(imported.itemCount) + L"개로 교체";
        } else {
            status = (mode == DelimitedImportMode::append ? L"Appended " : L"Replaced the draft with ") +
                std::to_wstring(imported.itemCount) + L" imported rule(s)";
        }
        if (!imported.warnings.empty()) {
            status += L" · " + std::to_wstring(imported.warnings.size()) +
                localization::text(L" warning(s)", L"개 경고");
        }
        setDetailStatus(status + localization::text(
            L". Save changes to write the file.", L". 파일에 쓰려면 변경 내용을 저장하세요."));
    }

    void exportRules() {
        if (!commitDetails(true, false)) return;
        wchar_t path[32768]{};
        ::wcscpy_s(path, L"NppQuickReplace-rules.csv");
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = localization::text(
            L"CSV rule files (*.csv)\0*.csv\0TSV rule files (*.tsv)\0*.tsv\0\0",
            L"CSV 규칙 파일 (*.csv)\0*.csv\0TSV 규칙 파일 (*.tsv)\0*.tsv\0\0");
        dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.lpstrInitialDir = options_.dataDirectory.c_str();
        dialog.lpstrDefExt = L"csv";
        dialog.nFilterIndex = 1;
        dialog.Flags =
            OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT |
            OFN_EXPLORER | OFN_NOREADONLYRETURN;
        if (!::GetSaveFileNameW(&dialog)) return;

        std::filesystem::path selectedPath(path);
        std::wstring extension = lowerWide(selectedPath.extension().wstring());
        if (dialog.nFilterIndex == 2 && extension != L".tsv") {
            selectedPath.replace_extension(L".tsv");
            extension = L".tsv";
        } else if (extension.empty()) {
            selectedPath.replace_extension(L".csv");
            extension = L".csv";
        }
        const char delimiter = extension == L".tsv" ? '\t' : ',';
        RuleExchangeResult exported =
            RuleExchange::exportDelimited(document_.dump(), delimiter);
        if (!exported.ok) {
            showMessage(L"The rules could not be exported.\n\n" +
                utf8ToWide(exported.error), MB_ICONERROR);
            return;
        }
        if (!exported.warnings.empty()) {
            const int choice = ::MessageBoxW(window_,
                localization::text(
                    L"Some cells begin with =, +, -, or @ and may run as formulas in spreadsheet software.\n\n"
                    L"Yes: prefix risky cells with an apostrophe (safer for spreadsheets)\n"
                    L"No: keep the exact original text (lossless)\n"
                    L"Cancel: do not export",
                    L"일부 셀이 =, +, -, @ 문자로 시작해 스프레드시트에서 수식으로 실행될 수 있어요.\n\n"
                    L"예: 위험한 셀 앞에 작은따옴표 추가 (스프레드시트에서 더 안전함)\n"
                    L"아니요: 원본 텍스트를 그대로 유지 (무손실)\n"
                    L"취소: 내보내지 않음"),
                localization::text(kWindowTitle), MB_YESNOCANCEL | MB_ICONWARNING);
            if (choice == IDCANCEL) return;
            if (choice == IDYES) {
                exported = RuleExchange::exportDelimited(document_.dump(), delimiter, true);
                if (!exported.ok) {
                    showMessage(L"The spreadsheet-safe export could not be created.\n\n" +
                        utf8ToWide(exported.error), MB_ICONERROR);
                    return;
                }
            }
        }

        std::string error;
        if (!ConfigStore::writeUtf8FileAtomic(selectedPath, exported.text, error)) {
            showMessage(L"The export file could not be written.\n\n" +
                utf8ToWide(error), MB_ICONERROR);
            return;
        }
        setDetailStatus(isKoreanUi()
            ? L"규칙 " + std::to_wstring(exported.itemCount) + L"개를 " +
                selectedPath.filename().wstring() + L"(으)로 내보냈어요."
            : L"Exported " + std::to_wstring(exported.itemCount) + L" rule(s) to " +
                selectedPath.filename().wstring() + L".");
    }

    void restoreBackup() {
        wchar_t path[32768]{};
        const std::filesystem::path backupDirectory = options_.dataDirectory / "backups";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = localization::text(
            L"Replacement backups (*.json)\0*.json\0All files (*.*)\0*.*\0\0", L"치환 백업 (*.json)\0*.json\0모든 파일 (*.*)\0*.*\0\0");
        dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.lpstrInitialDir = backupDirectory.c_str();
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT;
        if (!::GetOpenFileNameW(&dialog)) return;

        std::string content;
        std::string error;
        if (!ConfigStore::readUtf8File(path, content, error)) {
            showMessage(utf8ToWide(error), MB_ICONERROR);
            return;
        }
        RuleStore validator;
        const RuleLoadResult validation = validator.loadFromText(content);
        if (!validation.ok) {
            showMessage(L"That backup is not valid.\n\n" + utf8ToWide(validation.error), MB_ICONERROR);
            return;
        }
        const std::wstring prompt = isKoreanUi()
            ? L"규칙 " + std::to_wstring(validation.loadedCount) +
                L"개가 들어 있는 이 백업을 복원할까요?\n\n현재 파일을 먼저 백업해요."
            : L"Restore this backup with " + std::to_wstring(validation.loadedCount) +
                L" rules?\n\nThe current file will be backed up first.";
        if (::MessageBoxW(window_, prompt.c_str(), localization::text(kWindowTitle),
                MB_YESNO | MB_ICONQUESTION) != IDYES) return;

        std::filesystem::path currentBackup;
        if (!ConfigStore::backupReplacements(options_.dataDirectory, options_.replacementsPath,
                options_.config.maxBackupFiles, currentBackup, error) ||
            !ConfigStore::writeUtf8FileAtomic(options_.replacementsPath, content, error)) {
            showMessage(L"The backup could not be restored.\n\n" + utf8ToWide(error), MB_ICONERROR);
            return;
        }
        loadDocument(true);
        if (options_.onRulesSaved) options_.onRulesSaved();
        setDetailStatus(L"Backup restored and rules reloaded.");
    }

    void openJson() const {
        ::SendMessageW(options_.notepadHandle, NPPM_DOOPEN, 0,
            reinterpret_cast<LPARAM>(options_.replacementsPath.c_str()));
    }

    bool confirmClose() {
        if (!dirty_ && !detailsDirty_) return true;
        const int answer = ::MessageBoxW(window_,
            localization::text(L"Save the current rule changes before closing?"), localization::text(kWindowTitle),
            MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) return false;
        return answer != IDYES || saveDocument();
    }

    void onCommand(int id, int notification) {
        if (loading_) return;
        if (id == idSearch && notification == EN_CHANGE) {
            ::KillTimer(window_, kSearchTimer);
            ::SetTimer(window_, kSearchTimer, kSearchDebounceMs, nullptr);
            setText(listStatus_, localization::text(L"Filtering…"));
            return;
        }
        if (id == idGroupFilter && notification == CBN_SELCHANGE) {
            refreshList();
            return;
        }
        const bool textChanged = (id == idTrigger || id == idExtensions ||
            id == idPathGlobs || id == idLanguages || id == idReplacement ||
            id == idDescription) && notification == EN_CHANGE;
        const bool groupChanged = id == idGroup &&
            (notification == CBN_EDITCHANGE || notification == CBN_SELCHANGE);
        if (textChanged || groupChanged) {
            detailsDirty_ = selectedDocumentIndex_.has_value();
            if (detailsDirty_) setDetailStatus(L"Draft detail has unapplied changes.");
            updateWindowTitle();
            return;
        }
        if ((id == idEnabled || id == idSpace || id == idEnter || id == idTab ||
             id == idPunctuation || id == idImmediate || id == idCaseSensitive ||
             id == idCaptureTemplate) && notification == BN_CLICKED) {
            std::wstring status = L"Draft detail has unapplied changes.";
            if (id == idCaptureTemplate && isChecked(captureTemplate_) && isChecked(immediate_)) {
                setCheck(immediate_, false);
                status = L"Capture templates cannot use Immediate; Immediate was turned off.";
            } else if (id == idImmediate && isChecked(immediate_) && isChecked(captureTemplate_)) {
                setCheck(captureTemplate_, false);
                status = L"Immediate uses literal matching; Capture template was turned off.";
            }
            detailsDirty_ = selectedDocumentIndex_.has_value();
            if (detailsDirty_) setDetailStatus(status);
            updateWindowTitle();
            return;
        }
        if (notification != BN_CLICKED) return;
        switch (id) {
        case idAdd: addRule(); break;
        case idDuplicate: duplicateRule(); break;
        case idDelete: deleteSelectedRules(); break;
        case idManageGroups: manageGroups(); break;
        case idSetState: setSelectedState(); break;
        case idPreview: previewCurrentRule(); break;
        case idApplyDraft: commitDetails(true, true); break;
        case idSave: saveDocument(); break;
        case idReload: reloadFromDisk(); break;
        case idRestore: restoreBackup(); break;
        case idOpenJson: openJson(); break;
        case idImport: importRules(); break;
        case idExport: exportRules(); break;
        case idClose: ::SendMessageW(window_, WM_CLOSE, 0, 0); break;
        default: break;
        }
    }

    LRESULT onNotify(NMHDR* header) {
        if (header == nullptr || header->hwndFrom != list_) return 0;
        if (header->code == LVN_GETDISPINFOW) {
            auto* display = reinterpret_cast<NMLVDISPINFOW*>(header);
            const int row = display->item.iItem;
            const int column = display->item.iSubItem;
            if ((display->item.mask & LVIF_TEXT) != 0 && display->item.pszText != nullptr &&
                row >= 0 && static_cast<std::size_t>(row) < visibleRows_.size() &&
                column >= 0 && column < 5) {
                const std::wstring& value =
                    visibleRows_[static_cast<std::size_t>(row)].columns[static_cast<std::size_t>(column)];
                wcsncpy_s(display->item.pszText,
                    static_cast<std::size_t>(display->item.cchTextMax), value.c_str(), _TRUNCATE);
            }
            return 0;
        }
        if (header->code == LVN_ITEMCHANGING && !loading_ && detailsDirty_) {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
            const bool becomingSelected = (change->uChanged & LVIF_STATE) != 0 &&
                (change->uNewState & LVIS_SELECTED) != 0 &&
                (change->uOldState & LVIS_SELECTED) == 0;
            if (becomingSelected) {
                const auto newIndex = documentIndexForRow(change->iItem);
                if (newIndex.has_value() && newIndex != selectedDocumentIndex_ &&
                    !commitDetails(true, false)) return TRUE;
            }
        }
        if (header->code == LVN_ITEMCHANGED && !loading_) {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
            if ((change->uNewState & LVIS_SELECTED) != 0 &&
                (change->uOldState & LVIS_SELECTED) == 0) {
                if (const auto index = documentIndexForRow(change->iItem); index.has_value()) {
                    loadDetails(*index);
                }
            }
        } else if (header->code == LVN_COLUMNCLICK) {
            const auto* click = reinterpret_cast<NMLISTVIEW*>(header);
            if (sortColumn_ == click->iSubItem) sortAscending_ = !sortAscending_;
            else {
                sortColumn_ = click->iSubItem;
                sortAscending_ = true;
            }
            refreshList();
        } else if (header->code == NM_DBLCLK && selectedDocumentIndex_.has_value()) {
            ::SetFocus(trigger_);
            ::SendMessageW(trigger_, EM_SETSEL, 0, -1);
        }
        return 0;
    }

    void showMessage(std::wstring_view message, UINT icon) const {
        const std::wstring value(message);
        ::MessageBoxW(window_, localization::text(value.c_str()), localization::text(kWindowTitle), MB_OK | icon);
    }

    void setDetailStatus(std::wstring_view text) {
        const std::wstring value(text);
        ::SetWindowTextW(detailStatus_, localization::text(value.c_str()));
    }

    void updateWindowTitle() const {
        const std::wstring title = std::wstring(localization::text(kWindowTitle)) +
            (dirty_ || detailsDirty_ ? L" *" : L"");
        ::SetWindowTextW(window_, title.c_str());
    }

    RuleManagerOptions options_;
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    HFONT baseFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    std::vector<HWND> controls_;
    Json document_ = Json{{"version", 1}, {"groups", Json::array()}, {"items", Json::array()}};
    std::vector<CachedRuleRow> visibleRows_;
    std::vector<std::string> filterGroups_;
    std::optional<std::size_t> selectedDocumentIndex_;
    bool dirty_ = false;
    bool detailsDirty_ = false;
    bool loading_ = false;
    bool pendingClose_ = false;
    bool externalChangeDetected_ = false;
    std::uint64_t loadedContentHash_ = 0;
    FileStamp loadedFileStamp_{};
    int sortColumn_ = 1;
    bool sortAscending_ = true;

    HWND title_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND searchLabel_ = nullptr;
    HWND search_ = nullptr;
    HWND groupFilterLabel_ = nullptr;
    HWND groupFilter_ = nullptr;
    HWND list_ = nullptr;
    HWND listStatus_ = nullptr;
    HWND add_ = nullptr;
    HWND duplicate_ = nullptr;
    HWND delete_ = nullptr;
    HWND manageGroups_ = nullptr;
    HWND setState_ = nullptr;
    HWND detailsTitle_ = nullptr;
    HWND detailsHint_ = nullptr;
    HWND enabled_ = nullptr;
    HWND triggerLabel_ = nullptr;
    HWND trigger_ = nullptr;
    HWND groupLabel_ = nullptr;
    HWND group_ = nullptr;
    HWND activationLabel_ = nullptr;
    HWND space_ = nullptr;
    HWND enter_ = nullptr;
    HWND tab_ = nullptr;
    HWND punctuation_ = nullptr;
    HWND immediate_ = nullptr;
    HWND caseSensitive_ = nullptr;
    HWND captureTemplate_ = nullptr;
    HWND extensionsLabel_ = nullptr;
    HWND extensions_ = nullptr;
    HWND pathGlobsLabel_ = nullptr;
    HWND pathGlobs_ = nullptr;
    HWND languagesLabel_ = nullptr;
    HWND languages_ = nullptr;
    HWND replacementLabel_ = nullptr;
    HWND replacement_ = nullptr;
    HWND descriptionLabel_ = nullptr;
    HWND description_ = nullptr;
    HWND detailStatus_ = nullptr;
    HWND preview_ = nullptr;
    HWND applyDraft_ = nullptr;
    HWND divider_ = nullptr;
    HWND save_ = nullptr;
    HWND reload_ = nullptr;
    HWND restore_ = nullptr;
    HWND openJson_ = nullptr;
    HWND import_ = nullptr;
    HWND export_ = nullptr;
    HWND close_ = nullptr;
};

} // namespace

void showRuleManager(const RuleManagerOptions& options) {
    if (gManagerWindow != nullptr) {
        auto* manager = reinterpret_cast<RuleManagerWindow*>(
            ::GetWindowLongPtrW(gManagerWindow, GWLP_USERDATA));
        if (manager != nullptr) manager->activate();
        return;
    }
    auto* manager = new RuleManagerWindow(options);
    if (!manager->create()) {
        delete manager;
        ::MessageBoxW(options.notepadHandle,
            L"The replacement manager window could not be created.",
            localization::text(kWindowTitle), MB_OK | MB_ICONERROR);
    }
}

void closeRuleManager(bool discardChanges) {
    if (gManagerWindow != nullptr) {
        gDiscardManagerChanges = discardChanges;
        ::SendMessageW(gManagerWindow, WM_CLOSE, 0, 0);
    }
}

void handleRuleManagerDarkModeChange() {
    if (gManagerWindow != nullptr) {
        auto* manager = reinterpret_cast<RuleManagerWindow*>(
            ::GetWindowLongPtrW(gManagerWindow, GWLP_USERDATA));
        if (manager != nullptr) manager->handleDarkModeChange();
    }
    handleGroupManagerDarkModeChange();
}

} // namespace nppqr
