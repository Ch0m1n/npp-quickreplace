[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64', 'ARM64')]
    [string]$Architecture = 'x64',
    [switch]$Package,
    [string]$BuildDirectory = 'build'
)

$ErrorActionPreference = 'Stop'

# Layered launchers can expose both spellings. MSBuild treats them as duplicate keys.
# Rebuild one canonical uppercase PATH entry before launching CMake/MSBuild.
$pathValue = $env:PATH
Remove-Item Env:PATH -ErrorAction SilentlyContinue
Remove-Item Env:Path -ErrorAction SilentlyContinue
$env:PATH = $pathValue

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDirectory
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
    if (-not $visualStudio) { throw 'Visual Studio C++ Build Tools were not found.' }
    $cmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake was not found at: $cmake" }
if ($Architecture -eq 'ARM64') {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $arm64Tools = if (Test-Path -LiteralPath $vswhere) {
        & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath
    }
    if (-not $arm64Tools) {
        throw 'ARM64 C++ build tools are not installed. Add the Visual Studio component "MSVC ARM64 build tools", then retry.'
    }
}
$toolDirectory = Split-Path -Parent $cmake
$ctest = Join-Path $toolDirectory 'ctest.exe'
$cpack = Join-Path $toolDirectory 'cpack.exe'

& $cmake -S $root -B $build -A $Architecture
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build $build --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
$nativeArchitecture = if ($env:PROCESSOR_ARCHITEW6432) {
    $env:PROCESSOR_ARCHITEW6432
}
else {
    $env:PROCESSOR_ARCHITECTURE
}
$canRunTarget = $Architecture -eq 'x64' -or $nativeArchitecture -eq 'ARM64'
if ($canRunTarget) {
    & $ctest --test-dir $build -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
else {
    Write-Host "Skipping execution of $Architecture tests on $nativeArchitecture Windows (cross-build only)."
}

if ($Package) {
    $packageStarted = [DateTime]::UtcNow
    & $cpack --config (Join-Path $build 'CPackConfig.cmake') -C $Configuration -G ZIP -B $build
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
    $packages = Get-ChildItem -LiteralPath $build -Filter 'NppQuickReplace-*.zip' -File |
        Where-Object { $_.LastWriteTimeUtc -ge $packageStarted.AddSeconds(-2) } |
        Sort-Object Name
    if (-not $packages) { throw 'Packaging completed without creating a new ZIP file.' }
    $checksumLines = foreach ($packageFile in $packages) {
        $hash = Get-FileHash -LiteralPath $packageFile.FullName -Algorithm SHA256
        "$($hash.Hash.ToLowerInvariant())  $($packageFile.Name)"
    }
    $checksumPath = Join-Path $build 'SHA256SUMS.txt'
    [IO.File]::WriteAllText(
        $checksumPath,
        (($checksumLines -join [Environment]::NewLine) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
    Write-Host "Checksums: $checksumPath"
}

Write-Host "Build complete ($Architecture): $build\$Configuration\NppQuickReplace.dll"
