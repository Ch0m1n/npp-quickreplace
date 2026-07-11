#include "Localization.h"

#include <cwchar>

namespace nppqr::localization {
namespace {
LanguagePreference gPreference = LanguagePreference::automatic;
}

Language languageForLangId(LANGID languageId) noexcept {
    return PRIMARYLANGID(languageId) == LANG_KOREAN
        ? Language::korean
        : Language::english;
}

Language currentLanguage() noexcept {
    if (gPreference == LanguagePreference::english) return Language::english;
    if (gPreference == LanguagePreference::korean) return Language::korean;
    return detectedLanguage();
}

void setLanguagePreference(std::string_view preference) noexcept {
    if (preference == "en") {
        gPreference = LanguagePreference::english;
    } else if (preference == "ko") {
        gPreference = LanguagePreference::korean;
    } else {
        gPreference = LanguagePreference::automatic;
    }
}

LanguagePreference languagePreference() noexcept {
    return gPreference;
}

Language detectedLanguage() noexcept {
    static const Language language = languageForLangId(::GetUserDefaultUILanguage());
    return language;
}

const wchar_t* text(const wchar_t* english) noexcept {
    if (english == nullptr || currentLanguage() != Language::korean) return english;
    struct Translation { const wchar_t* english; const wchar_t* korean; };
    static constexpr Translation translations[]{
        {L"NppQuickReplace · Replacement Manager", L"NppQuickReplace · 치환 규칙 관리자"},
        {L"Replacement rules", L"치환 규칙"},
        {L"Search, edit, validate, and save without touching raw JSON.", L"JSON을 직접 편집하지 않고 검색·편집·검사·저장할 수 있어요."},
        {L"Search", L"검색"},
        {L"Trigger, replacement, group, or note", L"트리거, 치환문, 그룹 또는 메모"},
        {L"Group", L"그룹"},
        {L"Effective state", L"적용 상태"},
        {L"Trigger", L"트리거"},
        {L"Replacement", L"치환문"},
        {L"Activation", L"실행 조건"},
        {L"Add rule", L"규칙 추가"},
        {L"Duplicate", L"복제"},
        {L"Delete", L"삭제"},
        {L"&Groups…", L"그룹(&G)…"},
        {L"Set &state…", L"상태 설정(&S)…"},
        {L"Rule details", L"규칙 세부 정보"},
        {L"Changes stay in the draft until you save the document.", L"문서를 저장하기 전까지 변경 내용은 초안에만 남아요."},
        {L"Rule enabled", L"규칙 사용"},
        {L"Trigger / capture template", L"트리거 / 캡처 템플릿"},
        {L"Literal trigger, or ticket-${capture:1}", L"일반 트리거 또는 ticket-${capture:1}"},
        {L"Group ID", L"그룹 ID"},
        {L"Replace when", L"치환 시점"},
        {L"Space", L"스페이스"},
        {L"Enter", L"엔터"},
        {L"Tab", L"탭"},
        {L"Punctuation", L"문장 부호"},
        {L"Immediate", L"즉시"},
        {L"Case sensitive", L"대/소문자 구분"},
        {L"Capture template", L"캡처 템플릿"},
        {L"File extensions", L"파일 확장자"},
        {L"All files, or .txt, .md, .xml", L"모든 파일 또는 .txt, .md, .xml"},
        {L"Path globs", L"경로 패턴"},
        {L"Example: */docs/*, C:/work/*.md", L"예: */docs/*, C:/work/*.md"},
        {L"Languages", L"언어"},
        {L"Example: Python, Markdown", L"예: Python, Markdown"},
        {L"Replacement text", L"치환할 텍스트"},
        {L"Description / note", L"설명 / 메모"},
        {L"Select a rule to edit it.", L"편집할 규칙을 선택하세요."},
        {L"&Preview…", L"미리 보기(&P)…"},
        {L"&Apply to draft", L"초안에 적용(&A)"},
        {L"Save changes", L"변경 내용 저장"},
        {L"Reload from disk", L"디스크에서 다시 불러오기"},
        {L"Restore backup…", L"백업 복원…"},
        {L"Open JSON", L"JSON 열기"},
        {L"Import…", L"가져오기…"},
        {L"Export…", L"내보내기…"},
        {L"Close", L"닫기"},
        {L"All groups", L"모든 그룹"},
        {L"Rule off", L"규칙 꺼짐"},
        {L"Missing group", L"그룹 없음"},
        {L"Group off", L"그룹 꺼짐"},
        {L"On", L"켜짐"},
        {L"Off", L"꺼짐"},
        {L"Loaded and validated.", L"불러오기와 검사를 마쳤어요."},
        {L"Ready to edit.", L"편집할 준비가 됐어요."},
        {L"Fix the current rule before switching or saving.", L"전환하거나 저장하기 전에 현재 규칙을 수정하세요."},
        {L"Save cancelled · the external file was not overwritten.", L"저장 취소 · 외부 파일은 덮어쓰지 않았어요."},
        {L"Draft copy saved · the externally changed file was preserved.", L"초안 복사본 저장 · 외부에서 변경된 파일은 보존했어요."},
        {L"Select one or more rules before changing their state.", L"상태를 바꾸기 전에 규칙을 하나 이상 선택하세요."},
        {L"Discard the current draft and reload replacements.json?", L"현재 초안을 버리고 replacements.json을 다시 불러올까요?"},
        {L"Group changes applied to the draft. Save changes to write the file.", L"그룹 변경 내용을 초안에 적용했어요. 파일에 쓰려면 변경 내용을 저장하세요."},
        {L"Backup restored and rules reloaded.", L"백업을 복원하고 규칙을 다시 불러왔어요."},
        {L"Filtering…", L"필터링 중…"},
        {L"Draft detail has unapplied changes.", L"규칙 세부 정보에 적용하지 않은 변경 내용이 있어요."},
        {L"Capture templates cannot use Immediate; Immediate was turned off.", L"캡처 템플릿은 즉시 실행을 사용할 수 없어 즉시 실행을 껐어요."},
        {L"Immediate uses literal matching; Capture template was turned off.", L"즉시 실행은 일반 문자열 일치를 사용하므로 캡처 템플릿을 껐어요."},
        {L"Save the current rule changes before closing?", L"닫기 전에 현재 규칙 변경 내용을 저장할까요?"},
        {L"The replacement manager window could not be created.", L"치환 규칙 관리자 창을 만들 수 없어요."},
        {L"NppQuickReplace · Group Manager", L"NppQuickReplace · 그룹 관리자"},
        {L"Rule groups", L"규칙 그룹"},
        {L"Group changes stay in the replacement draft until the main window is saved.", L"기본 창을 저장하기 전까지 그룹 변경 내용은 치환 초안에만 남아요."},
        {L"State", L"상태"},
        {L"Display name", L"표시 이름"},
        {L"Rules", L"규칙 수"},
        {L"Group details", L"그룹 세부 정보"},
        {L"Group enabled", L"그룹 사용"},
        {L"Example: templates", L"예: templates"},
        {L"Example: Work templates", L"예: 작업 템플릿"},
        {L"Changing an ID updates every rule that uses it. Deleting a group leaves those rules ungrouped.", L"ID를 바꾸면 연결된 모든 규칙도 갱신돼요. 그룹을 삭제하면 해당 규칙은 그룹 없음 상태가 돼요."},
        {L"Select a group, or create a new one.", L"그룹을 선택하거나 새로 만드세요."},
        {L"New group", L"새 그룹"},
        {L"Apply to draft", L"초안에 적용"},
        {L"Delete group", L"그룹 삭제"},
        {L"Editing an existing group.", L"기존 그룹을 편집하고 있어요."},
        {L"Enter a unique ID and display name, then apply to the draft.", L"고유 ID와 표시 이름을 입력한 뒤 초안에 적용하세요."},
        {L"Group ID is required.", L"그룹 ID가 필요해요."},
        {L"That group ID is already in use.", L"이미 사용 중인 그룹 ID예요."},
        {L"Group change applied to the draft.", L"그룹 변경 내용을 초안에 적용했어요."},
        {L"Group ID and linked rules were updated in the draft.", L"그룹 ID와 연결된 규칙을 초안에서 갱신했어요."},
        {L"Group deleted from the draft.", L"초안에서 그룹을 삭제했어요."},
        {L"Discard the unapplied group detail changes?", L"적용하지 않은 그룹 세부 변경 내용을 버릴까요?"},
        {L"Group details have unapplied changes.", L"그룹 세부 정보에 적용하지 않은 변경 내용이 있어요."},
        {L"Trigger and replacement text are both required.", L"트리거와 치환할 텍스트를 모두 입력해야 해요."},
        {L"Applied to draft. Save changes to write the file.", L"초안에 적용했어요. 파일에 쓰려면 변경 내용을 저장하세요."},
        {L"Warning: replacements.json changed outside this window.", L"경고: 이 창 밖에서 replacements.json이 변경됐어요."},
        {L"Warning · replacements.json changed outside this window. Reload or save a draft copy.", L"경고 · 이 창 밖에서 replacements.json이 변경됐어요. 다시 불러오거나 초안 복사본을 저장하세요."},
        {L"Delete the selected rule from the draft?", L"선택한 규칙을 초안에서 삭제할까요?"},
        {L"Change the selected rules?\n\nYes: enable\nNo: disable\nCancel: leave unchanged", L"선택한 규칙의 상태를 바꿀까요?\n\n예: 사용\n아니요: 사용 안 함\n취소: 변경하지 않음"},
        {L"Preview (dynamic values are shown in brackets)", L"미리 보기 (동적 값은 대괄호로 표시돼요)"},
        {L"NppQuickReplace · Rule Preview", L"NppQuickReplace · 규칙 미리 보기"},
    };
    for (const Translation& translation : translations) {
        if (::wcscmp(english, translation.english) == 0) return translation.korean;
    }
    return english;
}

const wchar_t* text(const wchar_t* english, const wchar_t* korean) noexcept {
    return currentLanguage() == Language::korean ? korean : english;
}

} // namespace nppqr::localization
