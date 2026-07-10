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
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "Notepad_plus_msgs.h"
#include "RuleStore.h"

namespace nppqr {
namespace {

using Json = nlohmann::json;

constexpr wchar_t kWindowClass[] = L"NppQuickReplace.RuleManager";
constexpr wchar_t kWindowTitle[] = L"NppQuickReplace · Replacement Manager";

enum ControlId : int {
    idSearch = 1001,
    idGroupFilter,
    idRuleList,
    idAdd,
    idDuplicate,
    idDelete,
    idEnabled = 1010,
    idTrigger,
    idGroup,
    idSpace,
    idEnter,
    idTab,
    idPunctuation,
    idImmediate,
    idCaseSensitive,
    idWholeWord,
    idExtensions,
    idReplacement,
    idDescription,
    idApplyDraft,
    idSave = 1030,
    idReload,
    idRestore,
    idOpenJson,
    idClose,
};

HWND gManagerWindow = nullptr;

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
        result.append(label);
    }
    return result;
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
        constexpr int width = 1120;
        constexpr int height = 760;
        const int x = ownerRect.left + std::max(20L, (ownerRect.right - ownerRect.left - width) / 2);
        const int y = ownerRect.top + std::max(20L, (ownerRect.bottom - ownerRect.top - height) / 2);
        window_ = ::CreateWindowExW(
            WS_EX_CONTROLPARENT, kWindowClass, kWindowTitle,
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            x, y, width, height, options_.notepadHandle, nullptr, options_.module, this);
        if (window_ == nullptr) return false;
        gManagerWindow = window_;
        ::ShowWindow(window_, SW_SHOWNORMAL);
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
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {900, 620};
            return 0;
        case WM_COMMAND: onCommand(LOWORD(wParam), HIWORD(wParam)); return 0;
        case WM_NOTIFY: return onNotify(reinterpret_cast<NMHDR*>(lParam));
        case WM_CLOSE:
            if (confirmClose()) ::DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            ::SendMessageW(options_.notepadHandle, NPPM_MODELESSDIALOG,
                MODELESSDIALOGREMOVE, reinterpret_cast<LPARAM>(window_));
            return 0;
        case WM_NCDESTROY:
            if (baseFont_ != nullptr) ::DeleteObject(baseFont_);
            if (headingFont_ != nullptr) ::DeleteObject(headingFont_);
            gManagerWindow = nullptr;
            ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            delete this;
            return 0;
        default: return ::DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    HWND makeControl(DWORD exStyle, const wchar_t* className, const wchar_t* text, DWORD style, int id) {
        HWND control = ::CreateWindowExW(exStyle, className, text, WS_CHILD | WS_VISIBLE | style,
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
            reinterpret_cast<LPARAM>(L"Trigger, replacement, group, or note"));
        groupFilterLabel_ = makeLabel(L"Group");
        groupFilter_ = makeControl(0, WC_COMBOBOXW, L"",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, idGroupFilter);

        list_ = makeControl(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, idRuleList);
        ListView_SetExtendedListViewStyle(list_,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ::SetWindowTheme(list_, L"Explorer", nullptr);
        addListColumn(0, L"State", 64);
        addListColumn(1, L"Trigger", 132);
        addListColumn(2, L"Replacement", 230);
        addListColumn(3, L"Group", 112);
        addListColumn(4, L"Activation", 150);
        listStatus_ = makeLabel(L"");
        add_ = makeButton(L"Add rule", idAdd);
        duplicate_ = makeButton(L"Duplicate", idDuplicate);
        delete_ = makeButton(L"Delete", idDelete);

        detailsTitle_ = makeLabel(L"Rule details");
        detailsHint_ = makeLabel(L"Changes stay in the draft until you save the document.");
        enabled_ = makeButton(L"Rule enabled", idEnabled, BS_AUTOCHECKBOX);
        triggerLabel_ = makeLabel(L"Trigger");
        trigger_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idTrigger);
        groupLabel_ = makeLabel(L"Group ID");
        group_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idGroup);
        activationLabel_ = makeLabel(L"Replace when");
        space_ = makeButton(L"Space", idSpace, BS_AUTOCHECKBOX);
        enter_ = makeButton(L"Enter", idEnter, BS_AUTOCHECKBOX);
        tab_ = makeButton(L"Tab", idTab, BS_AUTOCHECKBOX);
        punctuation_ = makeButton(L"Punctuation", idPunctuation, BS_AUTOCHECKBOX);
        immediate_ = makeButton(L"Immediate", idImmediate, BS_AUTOCHECKBOX);
        caseSensitive_ = makeButton(L"Case sensitive", idCaseSensitive, BS_AUTOCHECKBOX);
        wholeWord_ = makeButton(L"Whole word", idWholeWord, BS_AUTOCHECKBOX);
        ::EnableWindow(wholeWord_, FALSE);
        extensionsLabel_ = makeLabel(L"File extensions");
        extensions_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idExtensions);
        ::SendMessageW(extensions_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"All files, or .txt, .md, .xml"));
        replacementLabel_ = makeLabel(L"Replacement text");
        replacement_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
            idReplacement);
        descriptionLabel_ = makeLabel(L"Description / note");
        description_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, idDescription);
        detailStatus_ = makeLabel(L"Select a rule to edit it.");
        applyDraft_ = makeButton(L"Apply to draft", idApplyDraft);

        divider_ = makeControl(0, WC_STATICW, L"", SS_ETCHEDHORZ, 0);
        save_ = makeButton(L"Save changes", idSave, BS_DEFPUSHBUTTON);
        reload_ = makeButton(L"Reload from disk", idReload);
        restore_ = makeButton(L"Restore backup…", idRestore);
        openJson_ = makeButton(L"Open JSON", idOpenJson);
        close_ = makeButton(L"Close", idClose);

        updateFonts();
        if (!loadDocument(true)) {
            populateGroupFilter();
            refreshList();
        }
        setDetailsEnabled(false);
        ::SendMessageW(options_.notepadHandle, NPPM_MODELESSDIALOG,
            MODELESSDIALOGADD, reinterpret_cast<LPARAM>(window_));
        ::SendMessageW(options_.notepadHandle, NPPM_DARKMODESUBCLASSANDTHEME,
            static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(window_));
        return true;
    }

    void addListColumn(int index, const wchar_t* text, int width) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(text);
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
        place(add_, margin, actionY, scale(96), scale(30));
        place(duplicate_, margin + scale(104), actionY, scale(92), scale(30));
        place(delete_, margin + scale(204), actionY, scale(82), scale(30));

        place(detailsTitle_, rightX, margin, rightWidth, scale(26));
        place(detailsHint_, rightX, margin + scale(28), rightWidth, scale(20));
        place(enabled_, rightX, margin + scale(54), scale(128), scale(24));
        const int fieldsY = margin + scale(86);
        const int half = (rightWidth - scale(12)) / 2;
        place(triggerLabel_, rightX, fieldsY, half, scale(18));
        place(groupLabel_, rightX + half + scale(12), fieldsY, half, scale(18));
        place(trigger_, rightX, fieldsY + scale(20), half, row);
        place(group_, rightX + half + scale(12), fieldsY + scale(20), half, row);

        const int activationY = fieldsY + scale(58);
        place(activationLabel_, rightX, activationY, rightWidth, scale(18));
        const int checkY = activationY + scale(20);
        place(space_, rightX, checkY, scale(72), scale(24));
        place(enter_, rightX + scale(72), checkY, scale(70), scale(24));
        place(tab_, rightX + scale(142), checkY, scale(60), scale(24));
        place(punctuation_, rightX + scale(202), checkY, scale(106), scale(24));
        place(immediate_, rightX + scale(308), checkY, scale(94), scale(24));
        place(caseSensitive_, rightX, checkY + scale(28), scale(126), scale(24));
        place(wholeWord_, rightX + scale(134), checkY + scale(28), scale(110), scale(24));

        const int extensionsY = checkY + scale(58);
        place(extensionsLabel_, rightX, extensionsY, rightWidth, scale(18));
        place(extensions_, rightX, extensionsY + scale(20), rightWidth, row);
        const int replacementY = extensionsY + scale(58);
        place(replacementLabel_, rightX, replacementY, rightWidth, scale(18));
        const int fixedBelow = scale(18 + 66 + 22 + 32 + 28);
        const int replacementHeight = std::max(scale(100),
            actionY - replacementY - scale(20) - fixedBelow);
        place(replacement_, rightX, replacementY + scale(20), rightWidth, replacementHeight);
        const int descriptionY = replacementY + scale(20) + replacementHeight + scale(8);
        place(descriptionLabel_, rightX, descriptionY, rightWidth, scale(18));
        place(description_, rightX, descriptionY + scale(20), rightWidth, scale(58));
        const int detailBottomY = descriptionY + scale(82);
        place(detailStatus_, rightX, detailBottomY, rightWidth - scale(120), scale(28));
        place(applyDraft_, rightX + rightWidth - scale(112), detailBottomY, scale(112), scale(30));

        place(divider_, margin, contentBottom + scale(8), width - margin * 2, scale(2));
        const int footerY = contentBottom + scale(18);
        place(save_, margin, footerY, scale(118), scale(32));
        place(reload_, margin + scale(128), footerY, scale(128), scale(32));
        place(restore_, margin + scale(266), footerY, scale(132), scale(32));
        place(openJson_, margin + scale(408), footerY, scale(100), scale(32));
        place(close_, width - margin - scale(90), footerY, scale(90), scale(32));
        RECT listClient{};
        ::GetClientRect(list_, &listClient);
        const int fixedColumns = scale(64 + 132 + 112 + 150);
        const int scrollbar = ::GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);

        ListView_SetColumnWidth(list_, 0, scale(64));
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
            dirty_ = false;
            detailsDirty_ = false;
            selectedDocumentIndex_.reset();
            populateGroupFilter();
            refreshList();
            setDetailsEnabled(false);
            updateWindowTitle();
            setDetailStatus(validation.warnings.empty()
                ? L"Loaded and validated."
                : L"Loaded with " + std::to_wstring(validation.warnings.size()) + L" warning(s).");
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
        ::SendMessageW(groupFilter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All groups"));
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

    void refreshList() {
        if (list_ == nullptr || !document_.contains("items") || !document_["items"].is_array()) return;
        loading_ = true;
        ListView_DeleteAllItems(list_);
        visibleIndices_.clear();
        const std::wstring query = lowerWide(windowText(search_));
        const LRESULT selection = ::SendMessageW(groupFilter_, CB_GETCURSEL, 0, 0);
        const std::string groupFilter = selection >= 0 &&
                static_cast<std::size_t>(selection) < filterGroups_.size()
            ? filterGroups_[static_cast<std::size_t>(selection)]
            : std::string{};

        const Json& items = document_["items"];
        for (std::size_t index = 0; index < items.size(); ++index) {
            const Json& item = items[index];
            const std::string group = item.value("group", "");
            if (!groupFilter.empty() && group != groupFilter) continue;
            std::wstring haystack = utf8ToWide(item.value("trigger", "")) + L"\n" +
                utf8ToWide(item.value("replacement", "")) + L"\n" +
                utf8ToWide(group) + L"\n" + utf8ToWide(item.value("description", ""));
            if (!query.empty() && lowerWide(std::move(haystack)).find(query) == std::wstring::npos) continue;
            visibleIndices_.push_back(index);
        }
        sortVisibleIndices();

        int row = 0;
        for (const std::size_t index : visibleIndices_) {
            const Json& item = items[index];
            const std::wstring state = item.value("enabled", true) ? L"On" : L"Off";
            const std::wstring trigger = utf8ToWide(item.value("trigger", ""));
            const std::wstring replacement = oneLine(utf8ToWide(item.value("replacement", "")));
            const std::wstring group = utf8ToWide(item.value("group", ""));
            const std::wstring activation = activationSummary(item);
            LVITEMW listItem{};
            listItem.mask = LVIF_TEXT | LVIF_PARAM;
            listItem.iItem = row;
            listItem.pszText = const_cast<wchar_t*>(state.c_str());
            listItem.lParam = static_cast<LPARAM>(index);
            ListView_InsertItem(list_, &listItem);
            setListText(row, 1, trigger);
            setListText(row, 2, replacement);
            setListText(row, 3, group.empty() ? L"—" : group);
            setListText(row, 4, activation);
            ++row;
        }
        setText(listStatus_, std::to_wstring(visibleIndices_.size()) + L" of " +
            std::to_wstring(items.size()) + L" rules shown");
        loading_ = false;
        if (selectedDocumentIndex_.has_value()) selectDocumentIndex(*selectedDocumentIndex_);
    }

    void sortVisibleIndices() {
        const Json& items = document_["items"];
        const auto textFor = [&](std::size_t index) {
            const Json& item = items[index];
            switch (sortColumn_) {
            case 0: return std::wstring(item.value("enabled", true) ? L"1" : L"0");
            case 1: return lowerWide(utf8ToWide(item.value("trigger", "")));
            case 2: return lowerWide(utf8ToWide(item.value("replacement", "")));
            case 3: return lowerWide(utf8ToWide(item.value("group", "")));
            default: return lowerWide(activationSummary(item));
            }
        };
        std::sort(visibleIndices_.begin(), visibleIndices_.end(), [&](std::size_t left, std::size_t right) {
            const std::wstring leftText = textFor(left);
            const std::wstring rightText = textFor(right);
            const int comparison = ::CompareStringOrdinal(
                leftText.c_str(), static_cast<int>(leftText.size()),
                rightText.c_str(), static_cast<int>(rightText.size()), TRUE);
            return sortAscending_ ? comparison == CSTR_LESS_THAN : comparison == CSTR_GREATER_THAN;
        });
    }

    void setListText(int row, int column, const std::wstring& text) {
        ListView_SetItemText(list_, row, column, const_cast<wchar_t*>(text.c_str()));
    }

    std::optional<std::size_t> documentIndexForRow(int row) const {
        if (row < 0) return std::nullopt;
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (!ListView_GetItem(list_, &item)) return std::nullopt;
        return static_cast<std::size_t>(item.lParam);
    }

    void selectDocumentIndex(std::size_t documentIndex) {
        for (int row = 0; row < ListView_GetItemCount(list_); ++row) {
            if (documentIndexForRow(row) == documentIndex) {
                ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED,
                    LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(list_, row, FALSE);
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
        setCheck(wholeWord_, true);
        setText(extensions_, joinExtensions(item));
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
        const std::array<HWND, 13> editable{{enabled_, trigger_, group_, space_, enter_, tab_,
            punctuation_, immediate_, caseSensitive_, extensions_, replacement_, description_, applyDraft_}};
        for (HWND control : editable) ::EnableWindow(control, enabled);
        ::EnableWindow(wholeWord_, FALSE);
        if (!enabled) {
            loading_ = true;
            for (HWND control : {trigger_, group_, extensions_, replacement_, description_}) setText(control, L"");
            for (HWND control : {enabled_, space_, enter_, tab_, punctuation_, immediate_,
                    caseSensitive_, wholeWord_}) setCheck(control, false);
            loading_ = false;
            setDetailStatus(L"Select a rule to edit it.");
        }
    }

    bool commitDetails(bool showError, bool refresh) {
        if (!selectedDocumentIndex_.has_value() || !detailsDirty_) return true;
        const std::size_t index = *selectedDocumentIndex_;
        if (index >= document_["items"].size()) return false;
        Json oldItem = document_["items"][index];
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
        item["group"] = wideToUtf8(trimWide(windowText(group_)));
        item["matchMode"] = "wholeWord";
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

        RuleStore validator;
        const RuleLoadResult validation = validator.loadFromText(document_.dump());
        if (!validation.ok) {
            item = std::move(oldItem);
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
        setDetailStatus(validation.warnings.empty()
            ? L"Applied to draft. Save changes to write the file."
            : L"Applied with " + std::to_wstring(validation.warnings.size()) + L" warning(s).");
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
        if (!ConfigStore::writeUtf8FileAtomic(options_.replacementsPath, document_.dump(2), error)) {
            showMessage(L"The rules could not be saved.\n\n" + utf8ToWide(error), MB_ICONERROR);
            return false;
        }
        dirty_ = false;
        detailsDirty_ = false;
        updateWindowTitle();
        if (options_.onRulesSaved) options_.onRulesSaved();
        std::wstring status = L"Saved " + std::to_wstring(validation.loadedCount) + L" rules";
        if (!backup.empty()) status += L" · backup created";
        if (!validation.warnings.empty()) {
            status += L" · " + std::to_wstring(validation.warnings.size()) + L" warning(s)";
        }
        setDetailStatus(status + L".");
        refreshList();
        return true;
    }

    void addRule() {
        if (!commitDetails(true, false)) return;
        clearFilters();
        document_["items"].push_back(Json{
            {"id", makeGuid()}, {"enabled", true},
            {"trigger", uniqueTrigger("new-trigger")}, {"replacement", "New replacement"},
            {"group", ""}, {"matchMode", "wholeWord"}, {"caseSensitive", false},
            {"activation", Json::array({"space", "enter", "tab"})},
            {"fileExtensions", Json::array()}, {"description", ""},
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
            ? L"Delete the selected rule from the draft?"
            : L"Delete " + std::to_wstring(indices.size()) + L" selected rules from the draft?";
        if (::MessageBoxW(window_, prompt.c_str(), kWindowTitle,
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

    void clearFilters() {
        loading_ = true;
        setText(search_, L"");
        ::SendMessageW(groupFilter_, CB_SETCURSEL, 0, 0);
        loading_ = false;
    }

    void reloadFromDisk() {
        if ((dirty_ || detailsDirty_) && ::MessageBoxW(window_,
                L"Discard the current draft and reload replacements.json?", kWindowTitle,
                MB_YESNO | MB_ICONWARNING) != IDYES) return;
        loadDocument(true);
    }

    void restoreBackup() {
        wchar_t path[32768]{};
        const std::filesystem::path backupDirectory = options_.dataDirectory / "backups";
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"Replacement backups (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
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
        const std::wstring prompt = L"Restore this backup with " +
            std::to_wstring(validation.loadedCount) +
            L" rules?\n\nThe current file will be backed up first.";
        if (::MessageBoxW(window_, prompt.c_str(), kWindowTitle,
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
            L"Save the current rule changes before closing?", kWindowTitle,
            MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) return false;
        return answer != IDYES || saveDocument();
    }

    void onCommand(int id, int notification) {
        if (loading_) return;
        if (id == idSearch && notification == EN_CHANGE) {
            refreshList();
            return;
        }
        if (id == idGroupFilter && notification == CBN_SELCHANGE) {
            refreshList();
            return;
        }
        if ((id == idTrigger || id == idGroup || id == idExtensions ||
             id == idReplacement || id == idDescription) && notification == EN_CHANGE) {
            detailsDirty_ = selectedDocumentIndex_.has_value();
            if (detailsDirty_) setDetailStatus(L"Draft detail has unapplied changes.");
            updateWindowTitle();
            return;
        }
        if ((id == idEnabled || id == idSpace || id == idEnter || id == idTab ||
             id == idPunctuation || id == idImmediate || id == idCaseSensitive) &&
            notification == BN_CLICKED) {
            detailsDirty_ = selectedDocumentIndex_.has_value();
            if (detailsDirty_) setDetailStatus(L"Draft detail has unapplied changes.");
            updateWindowTitle();
            return;
        }
        if (notification != BN_CLICKED) return;
        switch (id) {
        case idAdd: addRule(); break;
        case idDuplicate: duplicateRule(); break;
        case idDelete: deleteSelectedRules(); break;
        case idApplyDraft: commitDetails(true, true); break;
        case idSave: saveDocument(); break;
        case idReload: reloadFromDisk(); break;
        case idRestore: restoreBackup(); break;
        case idOpenJson: openJson(); break;
        case idClose: ::SendMessageW(window_, WM_CLOSE, 0, 0); break;
        default: break;
        }
    }

    LRESULT onNotify(NMHDR* header) {
        if (header == nullptr || header->hwndFrom != list_) return 0;
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
        ::MessageBoxW(window_, std::wstring(message).c_str(), kWindowTitle, MB_OK | icon);
    }

    void setDetailStatus(std::wstring_view text) {
        setText(detailStatus_, text);
    }

    void updateWindowTitle() const {
        const std::wstring title = std::wstring(kWindowTitle) +
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
    std::vector<std::size_t> visibleIndices_;
    std::vector<std::string> filterGroups_;
    std::optional<std::size_t> selectedDocumentIndex_;
    bool dirty_ = false;
    bool detailsDirty_ = false;
    bool loading_ = false;
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
    HWND wholeWord_ = nullptr;
    HWND extensionsLabel_ = nullptr;
    HWND extensions_ = nullptr;
    HWND replacementLabel_ = nullptr;
    HWND replacement_ = nullptr;
    HWND descriptionLabel_ = nullptr;
    HWND description_ = nullptr;
    HWND detailStatus_ = nullptr;
    HWND applyDraft_ = nullptr;
    HWND divider_ = nullptr;
    HWND save_ = nullptr;
    HWND reload_ = nullptr;
    HWND restore_ = nullptr;
    HWND openJson_ = nullptr;
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
            kWindowTitle, MB_OK | MB_ICONERROR);
    }
}

void closeRuleManager() {
    if (gManagerWindow != nullptr) ::SendMessageW(gManagerWindow, WM_CLOSE, 0, 0);
}

void handleRuleManagerDarkModeChange() {
    if (gManagerWindow == nullptr) return;
    auto* manager = reinterpret_cast<RuleManagerWindow*>(
        ::GetWindowLongPtrW(gManagerWindow, GWLP_USERDATA));
    if (manager != nullptr) manager->handleDarkModeChange();
}

} // namespace nppqr
