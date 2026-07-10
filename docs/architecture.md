# Architecture

## 목표

NppQuickReplace의 첫 공개 알파는 Notepad++ 입력 경로를 방해하지 않으면서 구분 문자 기반
치환의 기술적 위험을 검증하는 데 집중한다.

## 입력 흐름

```text
SCN_CHARADDED
  -> IME tentative 입력인지 확인
  -> 전체/문서별 활성 상태 확인
  -> 읽기 전용 및 다중 선택 확인
  -> 입력 문자를 activation으로 분류
  -> 커서 앞 한 단어만 UTF-8 범위로 읽기
  -> RuleStore 해시 인덱스 조회
  -> SCI_BEGINUNDOACTION
  -> SCI_SETTARGETRANGE + SCI_REPLACETARGET
  -> ${cursor} 또는 결과 뒤로 커서 이동
  -> SCI_ENDUNDOACTION
```

구분 문자는 이미 문서에 입력된 상태이므로 치환 대상 범위에 포함하지 않는다. 따라서 치환
결과 뒤에 공백, 줄바꿈, Tab, 문장 부호가 그대로 남는다. Undo 한 번은 자동 치환만 되돌려
`축약어 + 구분 문자` 상태로 복원한다.

## 컴포넌트

### RuleStore

- JSON을 임시 구조로 모두 검증한 뒤 성공할 때만 현재 인덱스와 교체한다.
- 로드 실패 시 마지막 정상 규칙 집합을 유지한다.
- 대소문자 구분 규칙은 원문 해시맵, 미구분 규칙은 ASCII 소문자 해시맵을 사용한다.
- 한글 등 비 ASCII UTF-8 바이트는 case folding 중 변경하지 않는다.
- 동일 축약어 또는 대소문자 정책이 충돌하는 규칙은 거부한다.

### ConfigStore

- Notepad++의 `NPPM_GETPLUGINSCONFIGDIR`가 제공한 폴더만 사용한다.
- 임시 파일을 완전히 기록한 뒤 `MoveFileExW`의 replace/write-through 옵션으로 교체한다.
- 기존 파일이 손상된 경우 기본값을 메모리에서만 사용하며 원본을 덮어쓰지 않는다.

### Plugin

- `SCN_CHARADDED`에서만 자동 치환하므로 문서 전체 붙여넣기를 처리하지 않는다.
- `SC_CHARACTERSOURCE_TENTATIVE_INPUT`을 무시해 IME 조합 중 오작동을 줄인다.
- 플러그인이 발생시킨 편집은 `gInternalEdit`로 재진입을 막는다.
- Notepad++ API 경계의 예외를 모두 잡아 호스트 프로세스로 예외가 넘어가지 않게 한다.

## 현재 제한

- 대소문자 미구분은 영문 ASCII에만 적용된다.
- 즉시 치환 규칙은 파싱하지만 입력 이벤트에서는 아직 실행하지 않는다.
- 여러 선택/커서가 있으면 자동 및 수동 치환을 건너뛴다.
- 목록 관리 UI가 없어 JSON을 직접 편집해야 한다.
- 실제 Notepad++ 프로세스 안에서의 IME/Undo 통합 테스트는 별도 수동 테스트가 필요하다.

