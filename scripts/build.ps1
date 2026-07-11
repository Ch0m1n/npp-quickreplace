[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Package,
    [string]$BuildDirectory = 'build'
)

$ErrorActionPreference = 'Stop'

# Layered launchers can expose both spellings. MSBuild treats them as duplicate keys.
$environmentKeys = [Environment]::GetEnvironmentVariables().Keys
if (($environmentKeys -ccontains 'Path') -and ($environmentKeys -ccontains 'PATH')) {
    Remove-Item Env:PATH
}

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
$toolDirectory = Split-Path -Parent $cmake
$ctest = Join-Path $toolDirectory 'ctest.exe'
$cpack = Join-Path $toolDirectory 'cpack.exe'

& $cmake -S $root -B $build -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build $build --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& $ctest --test-dir $build -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }

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

Write-Host "Build complete: $build\$Configuration\NppQuickReplace.dll"