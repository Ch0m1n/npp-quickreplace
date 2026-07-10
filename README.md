# NppQuickReplace

Notepad++에서 짧은 축약어를 긴 단어, 문장, 여러 줄 템플릿으로 바꾸는 Windows용
오프라인 플러그인이에요.

```text
입력: ㄱ123[공백]지속시간 30초
결과: 대공 방어 사격 지속시간 30초
```

> 현재 버전: **0.2.0-alpha**  
> 저장소 이름: **npp-quickreplace**  
> DLL 및 플러그인 메뉴 이름: **NppQuickReplace**

## 현재 구현 범위

- 공백, Enter, Tab, 설정된 문장 부호 입력 시 자동 치환
- 한글, 영문, 숫자, 일본어, 이모지를 포함한 UTF-8 규칙
- Windows IME의 조합 중(`SC_CHARACTERSOURCE_TENTATIVE_INPUT`) 이벤트 무시
- 영문 대소문자 구분/미구분
- 파일 확장자별 규칙 제한
- 그룹 활성화/비활성화
- 여러 줄 치환 및 `${cursor}` 위치 지정
- 한 번의 Notepad++ Undo 작업으로 치환 복원
- 내부 편집 재진입 방지
- 읽기 전용 문서와 다중 선택에서 안전하게 건너뛰기
- 전체 활성화 토글 및 현재 문서 일시 중지
- `Ctrl+Alt+Space` 수동 치환
- JSON 규칙 다시 불러오기
- 잘못된 JSON을 덮어쓰지 않고, 정상 백업이 있으면 메모리에서 복구
- 10,000개 규칙을 해시 인덱스로 조회하는 자동 테스트
- 문서 내용, 규칙, 경로를 외부로 전송하지 않는 완전 오프라인 동작

아직 목록 관리 GUI, 즉시 치환 모드, CSV/TSV 가져오기, 다중 커서 치환은 구현 전이에요.
현재 목록 관리는 Notepad++ 안에서 `Manage replacements (JSON)` 메뉴로 JSON 파일을 열어
수정한 뒤 `Reload replacements`를 실행하는 방식이에요.

## 설치

### 미리 빌드된 ZIP

릴리스 ZIP을 다음 구조로 Notepad++ 설치 폴더에 풀면 돼요.

```text
Notepad++/
└─ plugins/
   └─ NppQuickReplace/
      └─ NppQuickReplace.dll
```

Notepad++를 다시 시작하면 `Plugins > NppQuickReplace` 메뉴가 나타나요.

### 소스에서 빌드

필요한 도구:

- Windows 10 또는 11
- Visual Studio 2022 Build Tools의 C++ 데스크톱 워크로드
- CMake 3.24 이상(Visual Studio에 포함된 CMake도 지원)
- PowerShell 7 권장

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\build.ps1 -Configuration Release -Package
```

주요 결과물:

```text
build/Release/NppQuickReplace.dll
build/NppQuickReplace-0.2.0-x64.zip
```

로컬 Notepad++에 설치하려면 먼저 Notepad++를 닫고 실행해요. 기존 DLL이 있으면 삭제하지
않고 시간 스탬프가 붙은 `.bak_날짜` 복사본을 만든 뒤 교체해요.

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\install-local.ps1
```

Notepad++가 다른 위치에 있다면:

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\install-local.ps1 `
  -NotepadPlusPlusDirectory 'D:\Apps\Notepad++'
```

## 사용법

첫 실행 때 Notepad++가 알려주는 플러그인 설정 폴더 아래에 다음 파일을 만들어요.
일반 설치에서는 보통 `%APPDATA%\Notepad++\plugins\Config\NppQuickReplace`예요.
휴대용 설치도 `NPPM_GETPLUGINSCONFIGDIR` 결과를 사용하므로 고정된 사용자 경로를 가정하지
않아요.

```text
NppQuickReplace/
├─ config.json
├─ replacements.json
└─ backups/
```

메뉴:

- `Automatic replacement enabled`: 전체 자동 치환 켜기/끄기
- `Pause for current document`: 현재 탭에서만 일시 중지
- `Replace trigger before caret`: 커서 앞 축약어를 수동 치환 (`Ctrl+Alt+Space`)
- `Reload replacements`: `replacements.json` 다시 읽기
- `Manage replacements (JSON)`: JSON 파일을 현재 Notepad++에서 열기
- `Open data folder`: 설정 폴더 열기

기본 샘플 규칙:

```json
{
  "id": "sample-wows-aa",
  "enabled": true,
  "trigger": "ㄱ123",
  "replacement": "대공 방어 사격",
  "group": "wows",
  "matchMode": "wholeWord",
  "caseSensitive": false,
  "activation": ["space", "enter", "tab", "punctuation"],
  "fileExtensions": []
}
```

`fileExtensions`가 비어 있으면 모든 문서에 적용돼요. 값이 있으면 `.md`처럼 점을 포함하거나
`md`처럼 생략해도 읽을 때 정규화해요. 확장자 제한이 있는 규칙은 저장되지 않은 새 문서에는
적용되지 않아요.

여러 줄 템플릿에서 첫 `${cursor}`는 제거되고 그 위치에 커서가 놓여요. 같은 문자열이 여러
번 있으면 나머지도 모두 제거해요.

## 안전 동작

- 일반 붙여넣기는 `SCN_CHARADDED`의 일반 키 입력 흐름이 아니므로 자동 치환 대상이 아니에요.
- IME 임시 조합 문자는 무시하고, 조합이 끝난 뒤 구분 문자를 입력했을 때만 검사해요.
- 전체 문서를 검색하지 않고 커서 앞 최대 512 UTF-8 바이트만 확인해요.
- 치환 결과 때문에 발생한 편집 알림은 내부 플래그로 무시해 재귀 치환을 막아요.
- 손상된 설정 파일은 자동으로 덮어쓰지 않아요.
- 플러그인이 문서를 임의로 저장하지 않아요.

## 개발

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\build.ps1
```

또는 직접 실행할 수 있어요.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

구조:

```text
src/RuleStore.*       JSON 규칙 검증, 인덱싱, 조회
src/ConfigStore.*     기본 파일, 안전한 원자적 설정 저장
src/Plugin.cpp        Notepad++/Scintilla 이벤트 및 메뉴 연결
tests/                규칙 엔진 자동 테스트
samples/              기본 config.json과 replacements.json
docs/planning-ko.md   원본 기획서
```

기술 설계는 [docs/architecture.md](docs/architecture.md), 전체 기획은
[docs/planning-ko.md](docs/planning-ko.md)에서 볼 수 있어요.

## 로드맵

- 0.3: Win32 목록 관리 창, 추가/수정/삭제/검색/중복 검사
- 0.4: 그룹 UI, 현재 문서 범위 정책, CSV/TSV 가져오기/내보내기
- 0.5: 날짜·파일명 변수와 미리보기
- 1.0: 최신 Notepad++ 호환성 검증, 자동 백업, 사용자 문서, Plugin Admin 등록 준비

## 라이선스

GPL-3.0-or-later. 서드파티 고지는 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)를
확인해 주세요.

