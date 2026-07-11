[CmdletBinding()]
param(
    [string]$NotepadPlusPlusDirectory = "$env:ProgramFiles\Notepad++",
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = 'build',
    [ValidateSet('Install', 'Rollback', 'Uninstall')]
    [string]$Action = 'Install',
    [ValidateRange(1, 50)]
    [int]$MaxInstallerBackups = 10,
    [switch]$NoElevate
)

$ErrorActionPreference = 'Stop'

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-Elevated {
    $arguments = @(
        '-NoLogo',
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $PSCommandPath),
        '-NotepadPlusPlusDirectory', ('"{0}"' -f $NotepadPlusPlusDirectory),
        '-Configuration', $Configuration,
        '-BuildDirectory', ('"{0}"' -f $BuildDirectory),
        '-Action', $Action,
        '-MaxInstallerBackups', $MaxInstallerBackups,
        '-NoElevate'
    )
    $process = Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs -ArgumentList $arguments -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "Elevated installer exited with code $($process.ExitCode)."
    }
}

function Assert-NotepadPlusPlusClosed([string]$Executable) {
    $expectedPath = [IO.Path]::GetFullPath($Executable)
    $running = @(Get-Process -Name 'notepad++' -ErrorAction SilentlyContinue | Where-Object {
        try {
            $_.Path -and [IO.Path]::GetFullPath($_.Path).Equals(
                $expectedPath, [StringComparison]::OrdinalIgnoreCase)
        }
        catch { $false }
    })
    if ($running.Count -eq 0) { return }
    $details = ($running | ForEach-Object {
        if ($_.MainWindowTitle) { "PID $($_.Id): $($_.MainWindowTitle)" }
        else { "PID $($_.Id)" }
    }) -join [Environment]::NewLine
    throw "Notepad++ is still running. Close every Notepad++ window and retry.$([Environment]::NewLine)$details"
}

function Get-BackupFiles([string]$Target) {
    @(Get-ChildItem -LiteralPath (Split-Path -Parent $Target) -Filter ((Split-Path -Leaf $Target) + '.bak_*') -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending)
}

$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root "$BuildDirectory\$Configuration\NppQuickReplace.dll"
$resolvedNpp = Resolve-Path -LiteralPath $NotepadPlusPlusDirectory -ErrorAction Stop
$notepadExecutable = Join-Path $resolvedNpp 'notepad++.exe'
if (-not (Test-Path -LiteralPath $notepadExecutable -PathType Leaf)) {
    throw "notepad++.exe was not found in: $resolvedNpp"
}

$targetDirectory = Join-Path $resolvedNpp 'plugins\NppQuickReplace'
$target = Join-Path $targetDirectory 'NppQuickReplace.dll'
$programFilesRoots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) |
    Where-Object { $_ } |
    ForEach-Object { [IO.Path]::GetFullPath($_).TrimEnd('\') }
$requiresElevation = $programFilesRoots | Where-Object {
    [IO.Path]::GetFullPath($resolvedNpp.Path).StartsWith(
        $_ + '\', [StringComparison]::OrdinalIgnoreCase)
}

if ($requiresElevation -and -not (Test-IsAdministrator)) {
    if ($NoElevate) {
        throw 'Administrator permission is required to modify this Notepad++ installation.'
    }
    Write-Host 'Requesting administrator permission for the Notepad++ plugin folder...'
    Invoke-Elevated
    return
}

Assert-NotepadPlusPlusClosed $notepadExecutable
New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'

switch ($Action) {
    'Install' {
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Build output was not found: $source"
        }
        $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        if (Test-Path -LiteralPath $target -PathType Leaf) {
            $targetHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
            if ($sourceHash -eq $targetHash) {
                Write-Host "Already installed and verified: $target"
                return
            }
            Copy-Item -LiteralPath $target -Destination "$target.bak_$timestamp" -Force
        }
        Copy-Item -LiteralPath $source -Destination $target -Force
        $installedHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        if ($installedHash -ne $sourceHash) {
            throw "Installed DLL verification failed. Expected $sourceHash but found $installedHash."
        }
        $backups = Get-BackupFiles $target
        $backups | Select-Object -Skip $MaxInstallerBackups |
            Remove-Item -Force -ErrorAction Stop
        Write-Host "Installed and verified: $target"
        Write-Host "SHA256: $installedHash"
        Write-Host 'Start Notepad++ to load the plugin.'
    }
    'Rollback' {
        $backup = Get-BackupFiles $target | Select-Object -First 1
        if (-not $backup) {
            throw "No installer backup was found next to: $target"
        }
        if (Test-Path -LiteralPath $target -PathType Leaf) {
            Copy-Item -LiteralPath $target -Destination "$target.pre_rollback_$timestamp" -Force
        }
        $backupHash = (Get-FileHash -LiteralPath $backup.FullName -Algorithm SHA256).Hash
        Copy-Item -LiteralPath $backup.FullName -Destination $target -Force
        $restoredHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        if ($restoredHash -ne $backupHash) {
            throw "Rollback verification failed. Expected $backupHash but found $restoredHash."
        }
        Write-Host "Rolled back from: $($backup.FullName)"
        Write-Host "Restored: $target"
    }
    'Uninstall' {
        if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
            Write-Host "Plugin is already absent: $target"
            return
        }
        $quarantine = "$target.uninstalled_$timestamp"
        Move-Item -LiteralPath $target -Destination $quarantine
        Write-Host "Plugin disabled without deletion: $quarantine"
    }
}
