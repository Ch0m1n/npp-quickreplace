#include "GroupManager.h"

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <utility>

#include <nlohmann/json.hpp>

#include "Notepad_plus_msgs.h"
#include "Localization.h"
#include "RuleStore.h"

namespace nppqr {
namespace {

using Json = nlohmann::json;

constexpr wchar_t kGroupWindowClass[] = L"NppQuickReplace.GroupManager";
constexpr wchar_t kGroupWindowTitle[] = L"NppQuickReplace · Group Manager";

class GroupManagerWindow;
HWND gGroupWindow = nullptr;
GroupManagerWindow* gGroupManager = nullptr;
bool gDiscardGroupChanges = false;

enum GroupControlId : int {
    idGroupList = 2101,
    idGroupEnabled,
    idGroupId,
    idGroupName,
    idNewGroup,
    idApplyGroup,
    idDeleteGroup,
    idCloseGroups,
};

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

std::wstring trimWide(std::wstring value) {
    while (!value.empty() && ::iswspace(value.front()) != 0) value.erase(value.begin());
    while (!value.empty() && ::iswspace(value.back()) != 0) value.pop_back();
    return value;
}

class GroupManagerWindow {
public:
    GroupManagerWindow(HWND owner, HWND notepadHandle, HINSTANCE module, Json& document)
        : owner_(owner), notepadHandle_(notepadHandle), module_(module), document_(document) {}

    void handleDarkModeChange() const {
        if (window_ == nullptr || !::IsWindow(window_)) return;
        ::SendMessageW(notepadHandle_, NPPM_DARKMODESUBCLASSANDTHEME,
            static_cast<WPARAM>(NppDarkMode::dmfHandleChange),
            reinterpret_cast<LPARAM>(window_));
        ::SetWindowPos(window_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        ::RedrawWindow(window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
    }

    bool show() {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_DBLCLKS;
        windowClass.lpfnWndProc = &GroupManagerWindow::windowProcedure;
        windowClass.hInstance = module_;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kGroupWindowClass;
        ::RegisterClassExW(&windowClass);

        RECT ownerRect{};
        ::GetWindowRect(owner_, &ownerRect);
        constexpr int width = 720;
        constexpr int height = 470;
        const int x = ownerRect.left + std::max(16L, (ownerRect.right - ownerRect.left - width) / 2);
        const int y = ownerRect.top + std::max(16L, (ownerRect.bottom - ownerRect.top - height) / 2);
        window_ = ::CreateWindowExW(
            WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME,
            kGroupWindowClass,
            localization::text(kGroupWindowTitle),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
            x,
            y,
            width,
            height,
            owner_,
            nullptr,
            module_,
            this);
        if (window_ == nullptr) return false;
        gGroupWindow = window_;
        gGroupManager = this;

        if (::IsWindow(owner_)) ::EnableWindow(owner_, FALSE);
        ::ShowWindow(window_, SW_SHOW);
        ::UpdateWindow(window_);

        MSG message{};
        bool quitReceived = false;
        while (::IsWindow(window_)) {
            const BOOL result = ::GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) {
                quitReceived = result == 0;
                break;
            }
            if (!::IsDialogMessageW(window_, &message)) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
        }
        if (::IsWindow(owner_)) {
            ::EnableWindow(owner_, TRUE);
            ::SetActiveWindow(owner_);
        }
        if (quitReceived) ::PostQuitMessage(static_cast<int>(message.wParam));
        return changed_;
    }

private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<GroupManagerWindow*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<GroupManagerWindow*>(create->lpCreateParams);
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
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {scale(600), scale(400)};
            return 0;
        case WM_COMMAND: onCommand(LOWORD(wParam), HIWORD(wParam)); return 0;
        case WM_NOTIFY: return onNotify(reinterpret_cast<NMHDR*>(lParam));
        case WM_CLOSE:
            if (gDiscardGroupChanges || confirmDiscard()) ::DestroyWindow(window_);
            return 0;
        case WM_NCDESTROY:
            if (baseFont_ != nullptr) ::DeleteObject(baseFont_);
            gGroupWindow = nullptr;
            gGroupManager = nullptr;
            gDiscardGroupChanges = false;
            ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            window_ = nullptr;
            return 0;
        default: return ::DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    HWND makeControl(
        DWORD exStyle,
        const wchar_t* className,
        const wchar_t* text,
        DWORD style,
        int id) {
        HWND control = ::CreateWindowExW(
            exStyle,
            className,
            localization::text(text),
            WS_CHILD | WS_VISIBLE | style,
            0,
            0,
            10,
            10,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            module_,
            nullptr);
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
        INITCOMMONCONTROLSEX commonControls{
            sizeof(commonControls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        ::InitCommonControlsEx(&commonControls);
        dpi_ = ::GetDpiForWindow(window_);

        title_ = makeLabel(L"Rule groups");
        subtitle_ = makeLabel(
            L"Group changes stay in the replacement draft until the main window is saved.");
        list_ = makeControl(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            L"",
            WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            idGroupList);
        ListView_SetExtendedListViewStyle(
            list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ::SetWindowTheme(list_, L"Explorer", nullptr);
        addColumn(0, L"State", 60);
        addColumn(1, L"Group ID", 140);
        addColumn(2, L"Display name", 190);
        addColumn(3, L"Rules", 60);

        editTitle_ = makeLabel(L"Group details");
        enabled_ = makeButton(L"Group enabled", idGroupEnabled, BS_AUTOCHECKBOX);
        idLabel_ = makeLabel(L"Group ID");
        id_ = makeControl(
            WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, idGroupId);
        ::SendMessageW(
            id_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(localization::text(L"Example: templates")));
        nameLabel_ = makeLabel(L"Display name");
        name_ = makeControl(
            WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, idGroupName);
        ::SendMessageW(
            name_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(localization::text(L"Example: Work templates")));
        hint_ = makeLabel(
            L"Changing an ID updates every rule that uses it. Deleting a group leaves those rules ungrouped.");
        status_ = makeLabel(L"Select a group, or create a new one.");

        new_ = makeButton(L"New group", idNewGroup);
        apply_ = makeButton(L"Apply to draft", idApplyGroup, BS_DEFPUSHBUTTON);
        delete_ = makeButton(L"Delete group", idDeleteGroup);
        close_ = makeButton(L"Close", idCloseGroups);

        updateFont();
        setEditorEnabled(false);
        refreshList({});
        ::SendMessageW(
            notepadHandle_,
            NPPM_DARKMODESUBCLASSANDTHEME,
            static_cast<WPARAM>(NppDarkMode::dmfInit),
            reinterpret_cast<LPARAM>(window_));
        return true;
    }

    void addColumn(int index, const wchar_t* text, int width) {
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
        ::SetWindowPos(
            control, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void layout(int width, int height) {
        if (width <= 0 || height <= 0 || list_ == nullptr) return;
        const int margin = scale(16);
        const int gap = scale(18);
        const int footerHeight = scale(58);
        const int contentBottom = height - footerHeight;
        const int leftWidth = std::max(scale(310), (width - margin * 2 - gap) * 55 / 100);
        const int rightX = margin + leftWidth + gap;
        const int rightWidth = width - rightX - margin;

        place(title_, margin, margin, width - margin * 2, scale(26));
        place(subtitle_, margin, margin + scale(28), width - margin * 2, scale(20));
        const int bodyY = margin + scale(58);
        place(list_, margin, bodyY, leftWidth, std::max(scale(210), contentBottom - bodyY));

        place(editTitle_, rightX, bodyY, rightWidth, scale(24));
        place(enabled_, rightX, bodyY + scale(32), rightWidth, scale(24));
        place(idLabel_, rightX, bodyY + scale(68), rightWidth, scale(18));
        place(id_, rightX, bodyY + scale(88), rightWidth, scale(28));
        place(nameLabel_, rightX, bodyY + scale(128), rightWidth, scale(18));
        place(name_, rightX, bodyY + scale(148), rightWidth, scale(28));
        place(hint_, rightX, bodyY + scale(188), rightWidth, scale(54));
        place(status_, rightX, bodyY + scale(250), rightWidth, scale(44));

        const int footerY = contentBottom + scale(16);
        place(new_, margin, footerY, scale(100), scale(32));
        place(apply_, rightX, footerY, scale(118), scale(32));
        place(delete_, rightX + scale(128), footerY, scale(104), scale(32));
        place(close_, margin + scale(110), footerY, scale(82), scale(32));

        RECT listClient{};
        ::GetClientRect(list_, &listClient);
        const int fixed = scale(60 + 140 + 60);
        const int scrollbar = ::GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);
        ListView_SetColumnWidth(list_, 0, scale(60));
        ListView_SetColumnWidth(list_, 1, scale(140));
        ListView_SetColumnWidth(list_, 2,
            std::max(scale(120), static_cast<int>(listClient.right) - fixed - scrollbar - scale(6)));
        ListView_SetColumnWidth(list_, 3, scale(60));
    }

    void updateFont() {
        baseFont_ = ::CreateFontW(
            -::MulDiv(9, static_cast<int>(dpi_), 72),
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
        for (HWND control : controls_) {
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(baseFont_), TRUE);
        }
    }

    std::size_t ruleCount(std::string_view groupId) const {
        if (!document_.contains("items") || !document_["items"].is_array()) return 0;
        return static_cast<std::size_t>(std::count_if(
            document_["items"].begin(),
            document_["items"].end(),
            [&](const Json& item) {
                return item.is_object() && item.value("group", "") == groupId;
            }));
    }

    void refreshList(std::string_view selectId) {
        if (!document_.contains("groups") || !document_["groups"].is_array()) {
            document_["groups"] = Json::array();
        }
        std::unordered_map<std::string, std::size_t> ruleCounts;
        if (document_.contains("items") && document_["items"].is_array()) {
            ruleCounts.reserve(document_["items"].size());
            for (const auto& item : document_["items"]) {
                if (item.is_object()) ++ruleCounts[item.value("group", "")];
            }
        }
        loading_ = true;
        ListView_DeleteAllItems(list_);
        int selectedRow = -1;
        for (std::size_t index = 0; index < document_["groups"].size(); ++index) {
            const Json& group = document_["groups"][index];
            if (!group.is_object()) continue;
            const std::string id = group.value("id", "");
            const std::wstring state = localization::text(group.value("enabled", true) ? L"On" : L"Off");
            const std::wstring wideId = utf8ToWide(id);
            const std::wstring name = utf8ToWide(group.value("name", id));
            const std::wstring rules = std::to_wstring(ruleCounts[id]);

            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = ListView_GetItemCount(list_);
            item.pszText = const_cast<wchar_t*>(state.c_str());
            item.lParam = static_cast<LPARAM>(index);
            const int row = ListView_InsertItem(list_, &item);
            ListView_SetItemText(list_, row, 1, const_cast<wchar_t*>(wideId.c_str()));
            ListView_SetItemText(list_, row, 2, const_cast<wchar_t*>(name.c_str()));
            ListView_SetItemText(list_, row, 3, const_cast<wchar_t*>(rules.c_str()));
            if (id == selectId) selectedRow = row;
        }
        loading_ = false;
        if (selectedRow < 0 && ListView_GetItemCount(list_) > 0) {
            selectedRow = 0;
        }
        if (selectedRow >= 0) {
            ListView_SetItemState(
                list_,
                selectedRow,
                LVIS_SELECTED | LVIS_FOCUSED,
                LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list_, selectedRow, FALSE);
            loadSelected();
        } else {
            beginNew();
        }
    }

    std::optional<std::size_t> selectedIndex() const {
        const int row = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (row < 0) return std::nullopt;
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (!ListView_GetItem(list_, &item)) return std::nullopt;
        return static_cast<std::size_t>(item.lParam);
    }

    void loadSelected() {
        const auto index = selectedIndex();
        if (!index.has_value() || *index >= document_["groups"].size()) return;
        currentIndex_ = *index;
        creating_ = false;
        const Json& group = document_["groups"][*index];
        loading_ = true;
        ::SendMessageW(
            enabled_,
            BM_SETCHECK,
            group.value("enabled", true) ? BST_CHECKED : BST_UNCHECKED,
            0);
        ::SetWindowTextW(id_, utf8ToWide(group.value("id", "")).c_str());
        ::SetWindowTextW(
            name_, utf8ToWide(group.value("name", group.value("id", ""))).c_str());
        loading_ = false;
        editorDirty_ = false;
        setEditorEnabled(true);
        setStatus(L"Editing an existing group.");
    }

    void beginNew() {
        currentIndex_.reset();
        creating_ = true;
        loading_ = true;
        ListView_SetItemState(
            list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ::SendMessageW(enabled_, BM_SETCHECK, BST_CHECKED, 0);
        ::SetWindowTextW(id_, L"");
        ::SetWindowTextW(name_, L"");
        loading_ = false;
        editorDirty_ = false;
        setEditorEnabled(true);
        setStatus(L"Enter a unique ID and display name, then apply to the draft.");
        ::SetFocus(id_);
    }

    void setEditorEnabled(bool enabled) {
        for (HWND control : {enabled_, id_, name_, apply_, delete_}) {
            ::EnableWindow(control, enabled);
        }
        ::EnableWindow(delete_, enabled && !creating_);
    }

    bool duplicateId(std::string_view id, std::optional<std::size_t> except) const {
        if (!document_.contains("groups") || !document_["groups"].is_array()) return false;
        for (std::size_t index = 0; index < document_["groups"].size(); ++index) {
            if (except.has_value() && *except == index) continue;
            const Json& group = document_["groups"][index];
            if (group.is_object() && group.value("id", "") == id) return true;
        }
        return false;
    }

    void applyGroup() {
        if (!creating_ && !currentIndex_.has_value()) return;
        const std::string id = wideToUtf8(trimWide(windowText(id_)));
        std::string name = wideToUtf8(trimWide(windowText(name_)));
        if (id.empty()) {
            showMessage(L"Group ID is required.", MB_ICONWARNING);
            ::SetFocus(id_);
            return;
        }
        if (name.empty()) name = id;
        if (duplicateId(id, creating_ ? std::nullopt : currentIndex_)) {
            showMessage(L"That group ID is already in use.", MB_ICONWARNING);
            ::SetFocus(id_);
            return;
        }

        Json previous = document_;
        if (!document_.contains("groups") || !document_["groups"].is_array()) {
            document_["groups"] = Json::array();
        }
        std::size_t index = 0;
        if (creating_) {
            document_["groups"].push_back(Json::object());
            index = document_["groups"].size() - 1;
        } else {
            index = *currentIndex_;
        }
        Json& group = document_["groups"][index];
        const std::string oldId = group.value("id", "");
        group["id"] = id;
        group["name"] = name;
        group["enabled"] = ::SendMessageW(enabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!oldId.empty() && oldId != id && document_.contains("items") &&
            document_["items"].is_array()) {
            for (auto& item : document_["items"]) {
                if (item.is_object() && item.value("group", "") == oldId) item["group"] = id;
            }
        }

        RuleStore validator;
        const RuleLoadResult validation = validator.loadFromText(document_.dump());
        if (!validation.ok) {
            document_ = std::move(previous);
            showMessage(
                std::wstring(localization::text(L"This group change is not valid.\n\n",
                    L"이 그룹 변경 내용은 올바르지 않아요.\n\n")) + utf8ToWide(validation.error),
                MB_ICONWARNING);
            return;
        }
        changed_ = true;
        editorDirty_ = false;
        creating_ = false;
        currentIndex_ = index;
        refreshList(id);
        setStatus(
            oldId.empty() || oldId == id
                ? L"Group change applied to the draft."
                : L"Group ID and linked rules were updated in the draft.");
    }

    void deleteGroup() {
        if (!currentIndex_.has_value() || *currentIndex_ >= document_["groups"].size()) return;
        const Json& group = document_["groups"][*currentIndex_];
        const std::string id = group.value("id", "");
        const std::size_t usedBy = ruleCount(id);
        std::wstring prompt = std::wstring(localization::text(L"Delete group '", L"그룹 '")) +
            utf8ToWide(id) + localization::text(L"' from the draft?", L"'을(를) 초안에서 삭제할까요?");
        if (usedBy != 0) {
            prompt += L"\n\n" + std::to_wstring(usedBy) + localization::text(
                L" linked rule(s) will remain, but become ungrouped.", L"개의 연결된 규칙은 남지만 그룹 없음 상태가 돼요.");
        }
        if (::MessageBoxW(
                window_, prompt.c_str(), localization::text(kGroupWindowTitle), MB_YESNO | MB_ICONWARNING) != IDYES) {
            return;
        }

        Json previous = document_;
        if (usedBy != 0) {
            for (auto& item : document_["items"]) {
                if (item.is_object() && item.value("group", "") == id) item["group"] = "";
            }
        }
        document_["groups"].erase(
            document_["groups"].begin() +
            static_cast<Json::difference_type>(*currentIndex_));

        RuleStore validator;
        const RuleLoadResult validation = validator.loadFromText(document_.dump());
        if (!validation.ok) {
            document_ = std::move(previous);
            showMessage(
                std::wstring(localization::text(L"The group could not be deleted.\n\n",
                    L"그룹을 삭제할 수 없어요.\n\n")) + utf8ToWide(validation.error),
                MB_ICONERROR);
            return;
        }
        changed_ = true;
        editorDirty_ = false;
        currentIndex_.reset();
        refreshList({});
        setStatus(L"Group deleted from the draft.");
    }

    bool confirmDiscard() {
        if (!editorDirty_) return true;
        return ::MessageBoxW(
                   window_,
                   localization::text(L"Discard the unapplied group detail changes?"),
                   localization::text(kGroupWindowTitle),
                   MB_YESNO | MB_ICONQUESTION) == IDYES;
    }

    void onCommand(int id, int notification) {
        if (loading_) return;
        if ((id == idGroupId || id == idGroupName) && notification == EN_CHANGE) {
            editorDirty_ = true;
            setStatus(L"Group details have unapplied changes.");
            return;
        }
        if (id == idGroupEnabled && notification == BN_CLICKED) {
            editorDirty_ = true;
            setStatus(L"Group details have unapplied changes.");
            return;
        }
        if (notification != BN_CLICKED) return;
        switch (id) {
        case idNewGroup: if (confirmDiscard()) beginNew(); break;
        case idApplyGroup: applyGroup(); break;
        case idDeleteGroup: deleteGroup(); break;
        case idCloseGroups: ::SendMessageW(window_, WM_CLOSE, 0, 0); break;
        default: break;
        }
    }

    LRESULT onNotify(NMHDR* header) {
        if (header == nullptr || header->hwndFrom != list_ || loading_) return 0;
        if (header->code == LVN_ITEMCHANGING && editorDirty_) {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
            const bool becomingSelected = (change->uChanged & LVIF_STATE) != 0 &&
                (change->uNewState & LVIS_SELECTED) != 0 &&
                (change->uOldState & LVIS_SELECTED) == 0;
            if (becomingSelected) {
                if (!confirmDiscard()) return TRUE;
                editorDirty_ = false;
            }
        }
        if (header->code == LVN_ITEMCHANGED) {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
            if ((change->uNewState & LVIS_SELECTED) != 0 &&
                (change->uOldState & LVIS_SELECTED) == 0) {
                loadSelected();
            }
        } else if (header->code == NM_DBLCLK) {
            ::SetFocus(name_);
            ::SendMessageW(name_, EM_SETSEL, 0, -1);
        }
        return 0;
    }

    void showMessage(std::wstring_view message, UINT icon) const {
        const std::wstring value(message);
        ::MessageBoxW(window_, localization::text(value.c_str()),
            localization::text(kGroupWindowTitle), MB_OK | icon);
    }

    void setStatus(std::wstring_view text) {
        const std::wstring value(text);
        ::SetWindowTextW(status_, localization::text(value.c_str()));
    }

    HWND owner_ = nullptr;
    HWND notepadHandle_ = nullptr;
    HINSTANCE module_ = nullptr;
    Json& document_;
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    HFONT baseFont_ = nullptr;
    std::vector<HWND> controls_;
    bool changed_ = false;
    bool loading_ = false;
    bool creating_ = false;
    bool editorDirty_ = false;
    std::optional<std::size_t> currentIndex_;

    HWND title_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND list_ = nullptr;
    HWND editTitle_ = nullptr;
    HWND enabled_ = nullptr;
    HWND idLabel_ = nullptr;
    HWND id_ = nullptr;
    HWND nameLabel_ = nullptr;
    HWND name_ = nullptr;
    HWND hint_ = nullptr;
    HWND status_ = nullptr;
    HWND new_ = nullptr;
    HWND apply_ = nullptr;
    HWND delete_ = nullptr;
    HWND close_ = nullptr;
};

} // namespace

bool showGroupManager(
    HWND owner,
    HWND notepadHandle,
    HINSTANCE module,
    nlohmann::json& document) {
    nlohmann::json workingCopy = document;
    GroupManagerWindow manager(owner, notepadHandle, module, workingCopy);
    if (!manager.show()) return false;
    document = std::move(workingCopy);
    return true;
}

bool isGroupManagerOpen() {
    return gGroupWindow != nullptr && ::IsWindow(gGroupWindow);
}

void closeGroupManager(bool discardChanges) {
    if (isGroupManagerOpen()) {
        gDiscardGroupChanges = discardChanges;
        ::SendMessageW(gGroupWindow, WM_CLOSE, 0, 0);
    }
}

void handleGroupManagerDarkModeChange() {
    if (gGroupManager != nullptr) {
        gGroupManager->handleDarkModeChange();
    }
}

} // namespace nppqr
