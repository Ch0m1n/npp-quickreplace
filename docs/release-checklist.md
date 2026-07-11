# Release and Plugin Admin checklist

## Current readiness

- Version target: `0.7.0-alpha` for preview testing.
- Supported binary: Windows x64, statically linked MSVC runtime.
- ARM64 generation and package naming are supported by the build script, but this machine lacks the optional Visual Studio ARM64 C++ tools, so the ARM64 artifact is not yet verified.
- Verified host baseline: Notepad++ 8.5.3 x64 on Windows.
- The CPack ZIP places `NppQuickReplace.dll` at the archive root, as required by Plugins Admin.
- Authenticode signing is prepared but cannot be completed without a code-signing certificate.
- Submit to Plugins Admin only after promoting the build to a non-alpha `0.7.0.0` binary and publishing an immutable stable release URL.

## Build and package

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\build.ps1 -Configuration Debug
pwsh -NoLogo -NoProfile -File .\scripts\build.ps1 -Configuration Release -Architecture x64 -Package
# After installing the optional Visual Studio ARM64 C++ tools:
pwsh -NoLogo -NoProfile -File .\scripts\build.ps1 -Configuration Release -Architecture ARM64 -BuildDirectory build-arm64 -Package
```

Confirm:

- Debug and Release compile with `/W4`, `/sdl`, CFG, ASLR, DEP, and high-entropy VA settings already defined by CMake.
- `ctest` passes in both configurations.
- `build/NppQuickReplace-0.7.0-x64.zip` exists.
- `build/SHA256SUMS.txt` matches the package.
- The ZIP root contains `NppQuickReplace.dll`; it must not be nested under another `NppQuickReplace` directory.

## Optional Authenticode signing

Run this before packaging, using the certificate thumbprint from the Current User or Local Machine certificate store:

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\sign-release.ps1 `
  -Path .\build\Release\NppQuickReplace.dll `
  -CertificateThumbprint 0123456789ABCDEF0123456789ABCDEF01234567
```

The script uses SHA-256 file and timestamp digests and verifies the signature after signing. Timestamping requires network access. Never commit a PFX file, password, token, or private key.

## Host compatibility smoke tests

Test at minimum:

- Notepad++ 8.5.3 x64, the currently verified baseline.
- The latest stable Notepad++ x64 before a stable release.
- UTF-8, ANSI/DBCS, CRLF, and LF documents.
- Korean IME composition, literal triggers, capture templates, tabstop navigation, multi-cursor replacement, undo/redo, dark mode, and manager shutdown.
- A clean profile and an upgrade profile containing older `config.json` and `replacements.json` files.

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\smoke-test-gui.ps1
pwsh -NoLogo -NoProfile -File .\scripts\smoke-test-features.ps1 -NotepadPlusPlus .\build\portable-npp\notepad++.exe
```

## Stable GitHub release

1. Remove the `-alpha` suffix from the DLL string version and release notes when stability criteria are met.
2. Keep the numeric binary version and Plugin Admin version exactly aligned, for example `0.7.0.0`.
3. Build from a clean tagged commit.
4. Sign the DLL when a trusted certificate is available, then package it.
5. Upload the ZIP and `SHA256SUMS.txt` to an immutable `v0.7.0` GitHub release.
6. Download the uploaded ZIP again and verify its SHA-256 and root layout.

## Plugins Admin entry

The official schema requires `folder-name`, `display-name`, `version`, a 64-character SHA-256 `id`, a direct ZIP `repository` URL, description, author, and homepage. The `id` is the SHA-256 of the plugin DLL, not the ZIP.

Generate the final entry only after the stable artifact URL exists:

```powershell
pwsh -NoLogo -NoProfile -File .\scripts\generate-plugin-admin-entry.ps1 `
  -DllPath .\build\Release\NppQuickReplace.dll `
  -Version 0.7.0.0 `
  -CompatibleNotepadVersions '[8.5.3,]' `
  -Repository https://github.com/Ch0m1n/npp-quickreplace/releases/download/v0.7.0/NppQuickReplace-0.7.0-x64.zip
```

Then:

1. Fork `notepad-plus-plus/nppPluginList`.
2. Add the generated object to the x64 plugin list without reformatting unrelated entries.
3. Run that repository’s validator and local Plugins Admin test procedure.
4. Open a focused pull request containing the x64 entry only.

References:

- https://github.com/notepad-plus-plus/nppPluginList
- https://www.npp-user-manual.org/docs/plugins/#plugins-admin