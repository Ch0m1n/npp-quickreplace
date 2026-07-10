# Contributing

Build and test on Windows x64 before opening a pull request:

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\build.ps1 -Configuration Release -Package
```

Please keep the automatic replacement path small and synchronous. Do not scan the full
document from `SCN_CHARADDED`, log document text, add network calls, or execute values from
the JSON files.

Changes that affect IME handling, caret placement, EOL conversion, or undo grouping should
include both unit coverage where possible and a manual Notepad++ reproduction checklist.

