#include "RuleTester.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Localization.h"
#include "Notepad_plus_msgs.h"
#include "RuleStore.h"

namespace nppqr {
namespace {

constexpr wchar_t kWindowClass[] = L"NppQuickReplace.RuleTester";
HWND gTesterWindow = nullptr;

enum ControlId : int {
    idInput = 4101,
    idActivation,
    idExtension,
    idPath,
    idLanguage,
    idTest,
    idClose,
};

const wchar_t* tr(const wchar_t* english, const wchar_t* korean) noexcept {
    return localization::text(english, korean);
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
    const int length = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
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

void replaceAll(std::string& text, std::string_view marker, std::string_view value) {
    std::size_t position = text.find(marker);
    while (position != std::string::npos) {
        text.replace(position, marker.size(), value);
        position = text.find(marker, position + value.size());
    }
}

class RuleTesterWindow {
public:
    RuleTesterWindow(
        HWND owner, HWND notepadHandle, HINSTANCE module, std::string document)
        : owner_(owner), notepadHandle_(notepadHandle), module_(module) {
        loadResult_ = store_.loadFromText(document);
    }

    void show() {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_DBLCLKS;
        windowClass.lpfnWndProc = &RuleTesterWindow::windowProcedure;
        windowClass.hInstance = module_;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kWindowClass;
        ::RegisterClassExW(&windowClass);

        RECT ownerRect{};
        ::GetWindowRect(owner_, &ownerRect);
        const UINT ownerDpi = ::GetDpiForWindow(owner_);
        const int width = ::MulDiv(760, static_cast<int>(ownerDpi), 96);
        const int height = ::MulDiv(600, static_cast<int>(ownerDpi), 96);
        const int x = ownerRect.left + std::max(20L, (ownerRect.right - ownerRect.left - width) / 2);
        const int y = ownerRect.top + std::max(20L, (ownerRect.bottom - ownerRect.top - height) / 2);
        window_ = ::CreateWindowExW(
            WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME,
            kWindowClass, tr(L"NppQuickReplace · Rule Tester", L"NppQuickReplace · 규칙 테스트"),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
            x, y, width, height, owner_, nullptr, module_, this);
        if (window_ == nullptr) {
            ::MessageBoxW(owner_, tr(
                L"The rule tester window could not be created.",
                L"규칙 테스트 창을 만들 수 없어요."),
                L"NppQuickReplace", MB_OK | MB_ICONERROR);
            return;
        }
        gTesterWindow = window_;
        ::EnableWindow(owner_, FALSE);
        ::ShowWindow(window_, SW_SHOW);
        ::UpdateWindow(window_);

        MSG message{};
        bool quit = false;
        while (::IsWindow(window_)) {
            const BOOL result = ::GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) {
                quit = result == 0;
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
        if (quit) ::PostQuitMessage(static_cast<int>(message.wParam));
    }

    void handleDarkModeChange() const {
        if (window_ == nullptr) return;
        ::SendMessageW(notepadHandle_, NPPM_DARKMODESUBCLASSANDTHEME,
            static_cast<WPARAM>(NppDarkMode::dmfHandleChange),
            reinterpret_cast<LPARAM>(window_));
        ::RedrawWindow(window_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
    }

private:
    static LRESULT CALLBACK windowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<RuleTesterWindow*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<RuleTesterWindow*>(create->lpCreateParams);
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
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {scale(620), scale(520)};
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == idTest && HIWORD(wParam) == BN_CLICKED) test();
            else if (LOWORD(wParam) == idClose && HIWORD(wParam) == BN_CLICKED) {
                ::DestroyWindow(window_);
            }
            return 0;
        case WM_CLOSE:
            ::DestroyWindow(window_);
            return 0;
        case WM_NCDESTROY:
            if (font_ != nullptr) ::DeleteObject(font_);
            gTesterWindow = nullptr;
            ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            window_ = nullptr;
            return 0;
        default:
            return ::DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    HWND makeControl(
        DWORD exStyle, const wchar_t* className, const wchar_t* text, DWORD style, int id) {
        HWND control = ::CreateWindowExW(exStyle, className, text,
            WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module_, nullptr);
        controls_.push_back(control);
        return control;
    }

    HWND label(const wchar_t* text) {
        return makeControl(0, WC_STATICW, text, SS_LEFT | SS_NOPREFIX, 0);
    }

    bool onCreate() {
        dpi_ = ::GetDpiForWindow(window_);
        title_ = label(tr(L"Test the current draft", L"현재 초안 테스트"));
        hint_ = label(tr(
            L"Enter the text before the delimiter and optional document context.",
            L"구분 문자 앞의 텍스트와 필요한 문서 조건을 입력하세요."));
        inputLabel_ = label(tr(L"Input text", L"입력 텍스트"));
        input_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idInput);
        activationLabel_ = label(tr(L"Activation", L"실행 조건"));
        activation_ = makeControl(0, WC_COMBOBOXW, L"",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, idActivation);
        for (const wchar_t* item : {
                tr(L"Space", L"스페이스"), tr(L"Enter", L"엔터"),
                tr(L"Tab", L"탭"), tr(L"Punctuation", L"문장 부호"),
                tr(L"Manual shortcut", L"수동 단축키")}) {
            ::SendMessageW(activation_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        }
        ::SendMessageW(activation_, CB_SETCURSEL, 0, 0);

        extensionLabel_ = label(tr(L"File extension", L"파일 확장자"));
        extension_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idExtension);
        pathLabel_ = label(tr(L"Full path", L"전체 경로"));
        path_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idPath);
        languageLabel_ = label(tr(L"Notepad++ language", L"Notepad++ 언어"));
        language_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_AUTOHSCROLL, idLanguage);
        test_ = makeControl(0, WC_BUTTONW, tr(L"Test rules", L"규칙 테스트"),
            WS_TABSTOP | BS_DEFPUSHBUTTON, idTest);
        resultLabel_ = label(tr(L"Diagnostic result", L"진단 결과"));
        result_ = makeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"",
            WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 0);
        close_ = makeControl(0, WC_BUTTONW, tr(L"Close", L"닫기"),
            WS_TABSTOP | BS_PUSHBUTTON, idClose);

        updateFont();
        ::SendMessageW(notepadHandle_, NPPM_DARKMODESUBCLASSANDTHEME,
            static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(window_));
        if (!loadResult_.ok) {
            ::SetWindowTextW(result_, (std::wstring(tr(
                L"The draft could not be tested:\r\n\r\n",
                L"초안을 테스트할 수 없어요:\r\n\r\n")) +
                utf8ToWide(loadResult_.error)).c_str());
            ::EnableWindow(test_, FALSE);
        } else {
            ::SetWindowTextW(result_, tr(
                L"Enter a sample and choose Test rules.",
                L"예시를 입력하고 규칙 테스트를 누르세요."));
        }
        ::SetFocus(input_);
        return true;
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

    int scale(int value) const {
        return ::MulDiv(value, static_cast<int>(dpi_), 96);
    }

    void place(HWND control, int x, int y, int width, int height) const {
        ::SetWindowPos(control, nullptr, x, y, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void layout(int width, int height) {
        if (title_ == nullptr) return;
        const int margin = scale(18);
        const int gap = scale(12);
        const int row = scale(28);
        place(title_, margin, margin, width - margin * 2, scale(28));
        place(hint_, margin, margin + scale(30), width - margin * 2, scale(24));
        const int firstY = margin + scale(68);
        const int activationWidth = scale(150);
        const int activationX = width - margin - activationWidth;
        place(inputLabel_, margin, firstY, width - margin * 2 - activationWidth - gap, scale(18));
        place(activationLabel_, activationX, firstY, activationWidth, scale(18));
        place(input_, margin, firstY + scale(20),
            width - margin * 2 - activationWidth - gap, row);
        place(activation_, activationX, firstY + scale(20), activationWidth, scale(180));

        const int contextY = firstY + scale(62);
        const int fieldWidth = (width - margin * 2 - gap * 2) / 3;
        place(extensionLabel_, margin, contextY, fieldWidth, scale(18));
        place(pathLabel_, margin + fieldWidth + gap, contextY, fieldWidth, scale(18));
        place(languageLabel_, margin + (fieldWidth + gap) * 2, contextY, fieldWidth, scale(18));
        place(extension_, margin, contextY + scale(20), fieldWidth, row);
        place(path_, margin + fieldWidth + gap, contextY + scale(20), fieldWidth, row);
        place(language_, margin + (fieldWidth + gap) * 2, contextY + scale(20), fieldWidth, row);
        place(test_, margin, contextY + scale(60), scale(120), scale(32));
        place(resultLabel_, margin, contextY + scale(108), width - margin * 2, scale(20));
        place(result_, margin, contextY + scale(130), width - margin * 2,
            std::max(scale(180), height - contextY - scale(188)));
        place(close_, width - margin - scale(90), height - scale(48), scale(90), scale(32));
    }

    Activation selectedActivation(bool& manual) const {
        const LRESULT selected = ::SendMessageW(activation_, CB_GETCURSEL, 0, 0);
        manual = selected == 4;
        if (selected == 1) return Activation::enter;
        if (selected == 2) return Activation::tab;
        if (selected == 3) return Activation::punctuation;
        return Activation::space;
    }

    std::wstring blockerText(std::string_view blocker) const {
        if (blocker == "rule_disabled") return tr(L"The rule is disabled.", L"규칙이 꺼져 있어요.");
        if (blocker == "group_disabled") return tr(L"The rule group is disabled.", L"규칙 그룹이 꺼져 있어요.");
        if (blocker == "activation_mismatch") return tr(L"The activation condition does not match.", L"실행 조건이 맞지 않아요.");
        if (blocker == "extension_mismatch") return tr(L"The file extension filter blocks it.", L"파일 확장자 조건에 막혔어요.");
        if (blocker == "path_mismatch") return tr(L"The path filter blocks it.", L"경로 조건에 막혔어요.");
        if (blocker == "language_mismatch") return tr(L"The language filter blocks it.", L"언어 조건에 막혔어요.");
        return tr(L"No trigger or capture template matched the input.", L"입력과 맞는 트리거나 캡처 템플릿이 없어요.");
    }

    void test() {
        const std::string input = wideToUtf8(windowText(input_));
        if (input.empty()) {
            ::MessageBoxW(window_, tr(
                L"Enter the text that appears immediately before the delimiter.",
                L"구분 문자 바로 앞에 나타나는 텍스트를 입력하세요."),
                tr(L"Rule tester", L"규칙 테스트"), MB_OK | MB_ICONWARNING);
            ::SetFocus(input_);
            return;
        }
        bool manual = false;
        const Activation activation = selectedActivation(manual);
        RuleDiagnosticResult diagnostic = store_.diagnose(
            input, activation, wideToUtf8(windowText(extension_)),
            wideToUtf8(windowText(path_)), wideToUtf8(windowText(language_)), manual);

        std::wstring output;
        if (diagnostic.matchedRule != nullptr) {
            const ReplacementRule& rule = *diagnostic.matchedRule;
            std::string preview = rule.replacement;
            for (std::size_t number = 1; number < diagnostic.captures.values.size(); ++number) {
                replaceAll(preview, "${capture:" + std::to_string(number) + "}",
                    diagnostic.captures.values[number]);
            }
            output = std::wstring(tr(L"Matched rule", L"일치한 규칙")) + L": " +
                utf8ToWide(rule.id) + L"\r\n" +
                tr(L"Trigger", L"트리거") + L": " + utf8ToWide(rule.trigger) + L"\r\n" +
                tr(L"Mode", L"방식") + L": " +
                (diagnostic.captureMatch ? tr(L"Capture template", L"캡처 템플릿")
                                         : tr(L"Literal", L"일반 문자열")) + L"\r\n" +
                tr(L"Eligible matches", L"실행 가능한 일치 규칙") + L": " +
                std::to_wstring(diagnostic.eligibleMatchCount) + L"\r\n\r\n" +
                tr(L"Replacement preview", L"치환 미리 보기") + L":\r\n" +
                utf8ToWide(preview);
            if (diagnostic.eligibleMatchCount > 1) {
                output += L"\r\n\r\n" + std::wstring(tr(
                    L"Warning: more than one rule can run. The result above has priority.",
                    L"경고: 실행 가능한 규칙이 둘 이상이에요. 위 규칙이 우선해요."));
            }
        } else {
            output = std::wstring(tr(
                L"No rule would run for this context.",
                L"이 조건에서 실행되는 규칙이 없어요.")) +
                L"\r\n" + tr(L"Trigger matches", L"트리거 일치 수") + L": " +
                std::to_wstring(diagnostic.triggerMatchCount);
            for (const std::string& blocker : diagnostic.blockers) {
                output += L"\r\n• " + blockerText(blocker);
            }
        }
        ::SetWindowTextW(result_, output.c_str());
    }

    HWND owner_ = nullptr;
    HWND notepadHandle_ = nullptr;
    HINSTANCE module_ = nullptr;
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    std::vector<HWND> controls_;
    RuleStore store_;
    RuleLoadResult loadResult_;

    HWND title_ = nullptr;
    HWND hint_ = nullptr;
    HWND inputLabel_ = nullptr;
    HWND input_ = nullptr;
    HWND activationLabel_ = nullptr;
    HWND activation_ = nullptr;
    HWND extensionLabel_ = nullptr;
    HWND extension_ = nullptr;
    HWND pathLabel_ = nullptr;
    HWND path_ = nullptr;
    HWND languageLabel_ = nullptr;
    HWND language_ = nullptr;
    HWND test_ = nullptr;
    HWND resultLabel_ = nullptr;
    HWND result_ = nullptr;
    HWND close_ = nullptr;
};

RuleTesterWindow* gTester = nullptr;

} // namespace

void showRuleTester(
    HWND owner,
    HWND notepadHandle,
    HINSTANCE module,
    std::string ruleDocument) {
    if (gTesterWindow != nullptr) {
        ::SetForegroundWindow(gTesterWindow);
        return;
    }
    RuleTesterWindow tester(owner, notepadHandle, module, std::move(ruleDocument));
    gTester = &tester;
    tester.show();
    gTester = nullptr;
}

bool isRuleTesterOpen() {
    return gTesterWindow != nullptr && ::IsWindow(gTesterWindow);
}

void closeRuleTester() {
    if (gTesterWindow != nullptr) ::SendMessageW(gTesterWindow, WM_CLOSE, 0, 0);
}

void handleRuleTesterDarkModeChange() {
    if (gTester != nullptr) gTester->handleDarkModeChange();
}

} // namespace nppqr
