# Changelog

## 0.5.0-alpha - 2026-07-11

- Prevent stale manager drafts from overwriting externally changed rules and offer reload or save-copy recovery.
- Add a configurable 1 MiB default expansion cap across templates, clipboard, selection, and encoded document output.
- Harden CSV/TSV parsing with deterministic import IDs, row/column diagnostics, duplicate-header checks, formula warnings, spreadsheet-safe export, and `extraJson` preservation.
- Add path glob and Notepad++ language filters to matching, GUI editing, and CSV/TSV exchange.
- Add `${selection}`, `${uuid}`, `${line}`, `${column}`, and first-position `${tabstop:N}` expansion.
- Add opt-in final-trigger paste processing without rescanning an entire pasted document range.
- Replace the manager rule list with a cached virtual ListView and debounce search input.
- Show effective rule/group state, add bulk enable/disable, dynamic-value preview, external-change banner, and persisted window placement.
- Aggregate group usage counts in one pass and force-close nested manager windows safely during Notepad++ shutdown.
- Add tests for write conflicts, config expansion limits, CSV safety and field preservation, deterministic import, malformed input diagnostics, and contextual filters.

## 0.4.0-alpha - 2026-07-10

- Add UTF-8 BOM CSV/TSV import and export with quoted comma, quote, and multiline field support.
- Add append and replace import modes with full RuleStore validation before the draft changes.
- Add a native group manager for create, rename, enable/disable, and safe delete workflows.
- Update linked rules automatically when a group ID changes and ungroup rules when a group is deleted.
- Add editable group selection and automatic group creation from typed IDs or imported data.
- Add import/export controls to the rule manager and keep all imported changes draft-only until save.
- Avoid appending a second newline when atomically writing already terminated CSV/TSV content.
- Add round-trip, validation, import-mode, Korean text, and multiline exchange tests.

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

