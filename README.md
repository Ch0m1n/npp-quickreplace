# NppQuickReplace

Notepad++에서 짧은 축약어를 긴 단어, 문장, 여러 줄 템플릿으로 바꾸는 Windows용 오프라인 플러그인이에요.

```text
입력: ㄱ123[공백]지속시간 30초
결과: 대공 방어 사격 지속시간 30초
```

> 현재 버전: **0.3.0-alpha**
> 저장소 이름: **npp-quickreplace**  
> DLL 및 플러그인 메뉴 이름: **NppQuickReplace**

## 현재 구현 범위

- 검색·그룹 필터·열 정렬을 지원하는 모델리스 규칙 관리자
- 규칙 추가, 수정, 복제, 다중 삭제, 전체 문서 검증
- 공백, Enter, Tab, 문장 부호 및 즉시 치환
- UTF-8 JSON과 Scintilla 문서 코드 페이지 간 안전한 변환
- Unicode NFC 정규화와 Windows IME tentative-input 보호
- 대소문자 정책, 파일 확장자 제한, 그룹 활성 상태
- 여러 줄 치환과 `${cursor}` 위치 지정
- `${date}`, `${time}`, `${filename}`, `${filepath}`, `${clipboard}` 내장 변수
- 한 번의 Notepad++ Undo 작업으로 되돌리는 RAII 편집 트랜잭션
- 읽기 전용 문서와 다중 선택 안전 모드
- 전체 토글, 문서별 일시 중지, `Ctrl+Alt+Space` 수동 치환
- 설정과 규칙 동시 재로드 및 상세 경고
- 저장 전 자동 백업, 순환 정리, GUI 백업 복원
- 미래 설정 필드를 보존하는 원자적 저장과 다중 인스턴스 쓰기 잠금
- 닫힌 버퍼 상태 정리와 버퍼별 확장자 캐시
- Notepad++ 다크 모드와 DPI 배율을 따르는 Win32 GUI
- 100,000개 규칙 해시 조회 자동 테스트
- 외부 통신이 없는 완전 오프라인 동작

## 아직 제한되는 기능

- `processPaste`는 안전상 비활성 상태이며 켜면 경고를 표시해요.
- 다중 커서 치환과 `${1}`, `${2}` 탭 정지점은 아직 지원하지 않아요.
- CSV/TSV 가져오기와 정규식 치환은 후속 버전 범위예요.
- 실제 Notepad++ 프로세스에서 IME·ANSI/DBCS 조합 수동 테스트도 권장해요.

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
- CMake 3.24 이상
- PowerShell 7 권장

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\build.ps1 -Configuration Release -Package
```

결과물:

```text
build/Release/NppQuickReplace.dll
build/NppQuickReplace-0.3.0-x64.zip
```

로컬 Notepad++에 설치할 때는 먼저 Notepad++를 닫고 실행해요. 기존 DLL은 삭제하지 않고 `.bak_날짜` 복사본을 만든 뒤 교체해요.

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\install-local.ps1
```

## 사용법

첫 실행 때 `NPPM_GETPLUGINSCONFIGDIR`가 제공하는 위치 아래에 데이터를 만들어요.

```text
NppQuickReplace/
├─ config.json
├─ replacements.json
├─ backups/
└─ logs/
```

플러그인 메뉴:

- `Automatic replacement enabled`: 전체 자동 치환 켜기/끄기
- `Pause for current document`: 현재 탭에서만 일시 중지
- `Replace trigger before caret`: 수동 치환 (`Ctrl+Alt+Space`)
- `Reload settings and rules`: 설정과 규칙을 함께 다시 읽기
- `Manage replacement rules…`: 규칙 관리자 열기
- `Open data folder`: 설정 폴더 열기

규칙 관리자에서는 좌측 목록을 검색·필터·정렬하고, 우측에서 트리거·치환문·그룹·실행 조건·확장자·설명을 편집해요. `Apply to draft`는 메모리 초안만 바꾸며, `Save changes`를 눌러야 `replacements.json`에 기록돼요. 저장 전에 현재 파일을 자동 백업하고, `Restore backup…`으로 검증된 백업을 복원할 수 있어요.

## 규칙 예시

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

즉시 치환은 `activation`에 `"immediate"`를 넣어요. 즉시 트리거가 더 긴 다른 트리거의 접두어라면 너무 일찍 치환되므로 저장을 거부해요.

`fileExtensions`가 비어 있으면 모든 문서에 적용돼요. 값이 있으면 `.md`처럼 점을 포함하거나 `md`처럼 생략해도 정규화해요. 저장되지 않은 새 문서에는 확장자 제한 규칙이 적용되지 않아요.

여러 줄 템플릿에서 첫 `${cursor}` 위치에 커서가 놓이고 모든 `${cursor}` 표시는 결과에서 제거돼요.

내장 변수는 삽입 순간의 로컬 날짜·시간, 현재 파일 경로, 클립보드 텍스트로 바뀌어요.

## 안전 동작

- IME 임시 조합 문자는 무시해요.
- 읽기 전용·다중 선택 문서는 기본적으로 건너뛰어요.
- 커서 앞의 제한된 범위만 한 번 읽고 메모리에서 경계를 찾아요.
- UTF-8이 아닌 문서는 Scintilla 코드 페이지로 변환하며 표현할 수 없는 결과는 삽입하지 않아요.
- 손상된 JSON은 덮어쓰지 않고 마지막 정상 규칙 집합이나 검증된 백업을 유지해요.
- 규칙 파일은 64 MiB, 규칙은 100,000개, 전체 텍스트는 128 MiB로 제한해요.
- 플러그인이 문서를 임의로 저장하지 않아요.

## 개발

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\build.ps1
```

```text
src/RuleStore.*       JSON 검증, 정규화, 인덱싱, 조회
src/ConfigStore.*     설정 보존, 원자 저장, 자동 백업
src/RuleManager.*     Win32 규칙 관리자 GUI
src/Plugin.cpp        Notepad++/Scintilla 이벤트 연결
```

기술 설계는 [docs/architecture.md](docs/architecture.md), 전체 기획은 [docs/planning-ko.md](docs/planning-ko.md)에서 볼 수 있어요.

## 로드맵

- 0.4: 그룹 편집 UI, CSV/TSV 가져오기·내보내기, 언어/경로 필터
- 0.5: 다중 탭 정지점, 선택 영역 변수, 미리보기 강화
- 1.0: 실제 호스트 호환성 매트릭스, 코드 서명, Plugin Admin 등록 준비

## 라이선스

GPL-3.0-or-later. 서드파티 고지는 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)를 확인해 주세요.
