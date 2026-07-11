[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Package
)

$ErrorActionPreference = 'Stop'

# PowerShell 7 can inherit both `Path` and `PATH` from layered launchers. MSBuild
# treats them as duplicate environment keys and refuses to start CL.exe.
$environmentKeys = [Environment]::GetEnvironmentVariables().Keys
if (($environmentKeys -ccontains 'Path') -and ($environmentKeys -ccontains 'PATH')) {
    Remove-Item Env:PATH
}

$root = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $root 'build'

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
}
else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'CMake was not found, and Visual Studio Installer\vswhere.exe is unavailable.'
    }

    $visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $visualStudio) {
        throw 'Visual Studio C++ Build Tools were not found.'
    }
    $cmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}

if (-not (Test-Path -LiteralPath $cmake)) {
    throw "CMake was not found at: $cmake"
}

& $cmake -S $root -B $buildDirectory -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

& $cmake --build $buildDirectory --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
& $ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }

if ($Package) {
    & $cmake --build $buildDirectory --config $Configuration --target package
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
    $packages = Get-ChildItem -LiteralPath $buildDirectory -Filter 'NppQuickReplace-*.zip' -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $packages) { throw 'The package target completed without producing a ZIP file.' }
    $checksumLines = foreach ($packageFile in $packages) {
        $hash = Get-FileHash -LiteralPath $packageFile.FullName -Algorithm SHA256
        "$($hash.Hash.ToLowerInvariant())  $($packageFile.Name)"
    }
    $checksumPath = Join-Path $buildDirectory 'SHA256SUMS.txt'
    Set-Content -LiteralPath $checksumPath -Value $checksumLines -Encoding utf8NoBOM
    Write-Host "Checksums: $checksumPath"
}

Write-Host "Build complete: $buildDirectory\$Configuration\NppQuickReplace.dll"

