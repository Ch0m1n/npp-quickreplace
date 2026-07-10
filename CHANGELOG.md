# Changelog

## 0.3.0-alpha - 2026-07-10

- Add a searchable, sortable, DPI-aware Win32 rule manager with draft editing and dark-mode integration.
- Add rule create, edit, duplicate, multi-delete, validation, backup, and restore workflows.
- Add immediate activation with prefix-conflict protection.
- Add document code-page conversion and Unicode NFC trigger normalization.
- Add date, time, filename, filepath, and clipboard replacement variables.
- Read the trigger window in one Scintilla range request and cache extensions by buffer.
- Add rotating automatic backups, durable atomic writes, and a cross-instance write lock.
- Preserve unknown config fields when saving known settings.
- Surface rule and config warnings instead of silently coercing unsupported values.
- Clear pause and extension state when a Notepad++ buffer closes.
- Add size limits, schema checks, group/id checks, and aggregate memory safeguards.
- Add tests for immediate matching, normalization, config preservation, and backups.

## 0.2.0-alpha - 2026-07-10

- Add delimiter-based replacement for space, Enter, Tab, and punctuation.
- Add UTF-8 JSON rule loading with group, case, activation, and extension filters.
- Add Korean IME tentative-input guard.
- Add single-step replacement undo grouping and recursive-edit protection.
- Add `${cursor}` and multiline replacement support.
- Add global toggle, per-document pause, manual replacement, and JSON reload commands.
- Add safe defaults, atomic config writes, and in-memory backup recovery.
- Add x64 CMake build, tests, packaging, and GitHub Actions.

