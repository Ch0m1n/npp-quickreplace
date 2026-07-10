[CmdletBinding()]
param(
    [string]$NotepadPlusPlusDirectory = "$env:ProgramFiles\Notepad++",
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "build\$Configuration\NppQuickReplace.dll"
$targetDirectory = Join-Path $NotepadPlusPlusDirectory 'plugins\NppQuickReplace'
$target = Join-Path $targetDirectory 'NppQuickReplace.dll'

if (-not (Test-Path -LiteralPath $source)) {
    throw "Build output was not found: $source"
}

$resolvedNpp = Resolve-Path -LiteralPath $NotepadPlusPlusDirectory -ErrorAction Stop
if (-not (Test-Path -LiteralPath (Join-Path $resolvedNpp 'notepad++.exe'))) {
    throw "notepad++.exe was not found in: $resolvedNpp"
}

New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
if (Test-Path -LiteralPath $target) {
    $timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    Copy-Item -LiteralPath $target -Destination "$target.bak_$timestamp" -Force
}
Copy-Item -LiteralPath $source -Destination $target -Force

Write-Host "Installed: $target"
Write-Host 'Restart Notepad++ to load the plugin.'

