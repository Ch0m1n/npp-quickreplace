[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$DllPath,
    [ValidatePattern('^\d+(\.\d+){1,3}$')]
    [string]$Version = '0.6.0.0',
    [ValidatePattern('^\[?,?(\d+(\.\d+){0,3},?){1,2}\]?$')]
    [string]$CompatibleNotepadVersions = '[8.5.3,]',
    [Parameter(Mandatory)]
    [uri]$Repository,
    [string]$OutputPath = 'packaging\plugin-admin-x64.entry.json'
    )

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dll = Resolve-Path -LiteralPath $DllPath -ErrorAction Stop
$output = if ([IO.Path]::IsPathRooted($OutputPath)) { $OutputPath } else { Join-Path $root $OutputPath }
$entry = [ordered]@{
    'folder-name' = 'NppQuickReplace'
    'display-name' = 'NppQuickReplace'
    version = $Version
    'npp-compatible-versions' = $CompatibleNotepadVersions
    id = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToUpperInvariant()
    repository = $Repository.AbsoluteUri
    description = 'Fast, offline trigger and capture-template text expansion with native rule management, tabstops, and multi-cursor support.'
    author = 'Ch0m1n'
    homepage = 'https://github.com/Ch0m1n/npp-quickreplace'
}
$json = $entry | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText($output, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
Write-Host "Plugin Admin entry written: $output"
