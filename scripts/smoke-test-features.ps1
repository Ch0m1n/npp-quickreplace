[CmdletBinding()]
param(
    [string]$NotepadPlusPlus = "$PSScriptRoot\..\build\portable-npp\notepad++.exe",
    [int]$TimeoutSeconds = 20
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$exe = (Resolve-Path -LiteralPath $NotepadPlusPlus).Path
$portableRoot = Split-Path -Parent $exe
if (-not (Test-Path -LiteralPath (Join-Path $portableRoot 'doLocalConf.xml'))) {
    throw 'Feature smoke tests require a portable Notepad++ copy with doLocalConf.xml.'
}

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NppQrFeatureNative {
    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, IntPtr processId);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int processId);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] public static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, UIntPtr size, uint allocationType, uint protect);
    [DllImport("kernel32.dll")] public static extern bool VirtualFreeEx(IntPtr process, IntPtr address, UIntPtr size, uint freeType);
    [DllImport("kernel32.dll")] public static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, UIntPtr size, out UIntPtr written);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, UIntPtr size, out UIntPtr read);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint attach, uint attachTo, bool value);
    [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr menu, int position);
    [DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr menu, int position);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetMenuString(IntPtr menu, uint item, StringBuilder text, int count, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Ansi, EntryPoint="SendMessageA")] public static extern IntPtr SendMessageString(IntPtr hwnd, uint message, IntPtr wParam, string text);
    [DllImport("user32.dll", CharSet=CharSet.Ansi, EntryPoint="SendMessageA")] public static extern IntPtr SendMessageBuffer(IntPtr hwnd, uint message, IntPtr wParam, StringBuilder text);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
}
"@

function Find-MenuCommand([IntPtr]$Menu, [string]$Text) {
    if ($Menu -eq [IntPtr]::Zero) { return $null }
    for ($index = 0; $index -lt [NppQrFeatureNative]::GetMenuItemCount($Menu); $index++) {
        $buffer = [Text.StringBuilder]::new(512)
        [void][NppQrFeatureNative]::GetMenuString($Menu, [uint32]$index, $buffer, $buffer.Capacity, 0x400)
        if ($buffer.ToString().Replace('&','') -like "*$Text*") {
            $id = [NppQrFeatureNative]::GetMenuItemID($Menu, $index)
            if ($id -ne 0xFFFFFFFF) { return $id }
        }
        $subMenu = [NppQrFeatureNative]::GetSubMenu($Menu, $index)
        if ($subMenu -ne [IntPtr]::Zero) {
            $nested = Find-MenuCommand $subMenu $Text
            if ($null -ne $nested) { return $nested }
        }
    }
    return $null
}

function Find-VisibleScintilla([IntPtr]$Parent) {
    $script:scintilla = [IntPtr]::Zero
    $callback = [NppQrFeatureNative+EnumWindowsProc]{
        param([IntPtr]$handle, [IntPtr]$unused)
        $class = [Text.StringBuilder]::new(128)
        [void][NppQrFeatureNative]::GetClassName($handle, $class, $class.Capacity)
        if ($class.ToString() -eq 'Scintilla' -and [NppQrFeatureNative]::IsWindowVisible($handle)) {
            $script:scintilla = $handle
            return $false
        }
        return $true
    }
    [void][NppQrFeatureNative]::EnumChildWindows($Parent, $callback, [IntPtr]::Zero)
    $script:scintilla
}

function Focus-Editor([IntPtr]$Main, [IntPtr]$Editor) {
    $currentThread = [NppQrFeatureNative]::GetCurrentThreadId()
    $targetThread = [NppQrFeatureNative]::GetWindowThreadProcessId($Editor, [IntPtr]::Zero)
    [void][NppQrFeatureNative]::AttachThreadInput($currentThread, $targetThread, $true)
    try {
        [void][NppQrFeatureNative]::SetForegroundWindow($Main)
        [void][NppQrFeatureNative]::SetFocus($Editor)
    }
    finally {
        [void][NppQrFeatureNative]::AttachThreadInput($currentThread, $targetThread, $false)
    }
}

function Set-EditorText([IntPtr]$Editor, [string]$Text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text + [char]0)
    $remote = [NppQrFeatureNative]::VirtualAllocEx($script:processHandle, [IntPtr]::Zero, [UIntPtr]$bytes.Length, 0x3000, 0x04)
    if ($remote -eq [IntPtr]::Zero) { throw 'VirtualAllocEx failed for SCI_SETTEXT.' }
    try {
        $written = [UIntPtr]::Zero
        if (-not [NppQrFeatureNative]::WriteProcessMemory($script:processHandle, $remote, $bytes, [UIntPtr]$bytes.Length, [ref]$written)) { throw 'WriteProcessMemory failed.' }
        [void][NppQrFeatureNative]::SendMessage($Editor, 2181, [IntPtr]::Zero, $remote)
    }
    finally { [void][NppQrFeatureNative]::VirtualFreeEx($script:processHandle, $remote, [UIntPtr]::Zero, 0x8000) }
}
function Get-EditorText([IntPtr]$Editor) {
    $length = [int][NppQrFeatureNative]::SendMessage($Editor, 2183, [IntPtr]::Zero, [IntPtr]::Zero)
    $size = $length + 1
    $remote = [NppQrFeatureNative]::VirtualAllocEx($script:processHandle, [IntPtr]::Zero, [UIntPtr]$size, 0x3000, 0x04)
    if ($remote -eq [IntPtr]::Zero) { throw 'VirtualAllocEx failed for SCI_GETTEXT.' }
    try {
        [void][NppQrFeatureNative]::SendMessage($Editor, 2182, [IntPtr]$size, $remote)
        $bytes = [byte[]]::new($size)
        $read = [UIntPtr]::Zero
        if (-not [NppQrFeatureNative]::ReadProcessMemory($script:processHandle, $remote, $bytes, [UIntPtr]$size, [ref]$read)) { throw 'ReadProcessMemory failed.' }
        return [Text.Encoding]::UTF8.GetString($bytes, 0, $length)
    }
    finally { [void][NppQrFeatureNative]::VirtualFreeEx($script:processHandle, $remote, [UIntPtr]::Zero, 0x8000) }
}
function Get-Caret([IntPtr]$Editor) {
    [int][NppQrFeatureNative]::SendMessage($Editor, 2008, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Set-Caret([IntPtr]$Editor, [int]$Position) {
    [void][NppQrFeatureNative]::SendMessage($Editor, 2556, [IntPtr]$Position, [IntPtr]::Zero)
}
function Send-Characters([IntPtr]$Editor, [string]$Text) {
    foreach ($character in $Text.ToCharArray()) {
        $token = if ([int]$character -eq 9) { '{TAB}' } else { [string]$character }
        [Windows.Forms.SendKeys]::SendWait($token)
        Start-Sleep -Milliseconds 20
    }
    Start-Sleep -Milliseconds 120
}

function Assert-Equal($Actual, $Expected, [string]$Description) {
    if ($Actual -cne $Expected) { throw "$Description failed. Expected '$Expected', got '$Actual'." }
}

$dataDirectory = Join-Path $portableRoot 'plugins\Config\NppQuickReplace'
New-Item -ItemType Directory -Path $dataDirectory -Force | Out-Null
$configPath = Join-Path $dataDirectory 'config.json'
$rulesPath = Join-Path $dataDirectory 'replacements.json'
$configBackup = if (Test-Path -LiteralPath $configPath) { [IO.File]::ReadAllBytes($configPath) } else { $null }
$rulesBackup = if (Test-Path -LiteralPath $rulesPath) { [IO.File]::ReadAllBytes($rulesPath) } else { $null }
$config = @"
{
  "pluginEnabled": true,
  "rememberEnabledState": false,
  "uiLanguage": "en",
  "autoReloadRules": true,
  "autoReloadIntervalMs": 250,
  "punctuationTriggers": ".,:;!?)]}",
  "processPaste": false,
  "skipReadOnlyDocuments": true,
  "skipMultiSelection": false,
  "maxTriggerBytes": 512,
  "maxExpandedBytes": 1048576
}
"@
$rules = @"
{
  "version": 1,
  "items": [
    {
      "id": "capture",
      "trigger": "ticket-`${capture:1}-`${capture:2}",
      "replacement": "#`${capture:1}:`${capture:2}",
      "matchMode": "captureTemplate",
      "activation": ["space"]
    },
    {
      "id": "snippet",
      "trigger": "snip",
      "replacement": "A`${tabstop:1}B`${tabstop:2}C`${tabstop:0}D",
      "matchMode": "wholeWord",
      "activation": ["space"]
    },
    {
      "id": "multi",
      "trigger": "mm",
      "replacement": "XX",
      "matchMode": "wholeWord",
      "activation": ["space"]
    }
  ]
}
"@
[IO.File]::WriteAllText($configPath, $config, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($rulesPath, $rules, [Text.UTF8Encoding]::new($false))

$process = $null
try {
    $process = Start-Process -FilePath $exe -ArgumentList '-multiInst','-nosession' -PassThru
    $script:processHandle = [NppQrFeatureNative]::OpenProcess(0x0438, $false, $process.Id)
    if ($script:processHandle -eq [IntPtr]::Zero) { throw 'OpenProcess failed for the portable test host.' }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $process.Refresh()
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            $manualCommand = Find-MenuCommand ([NppQrFeatureNative]::GetMenu($process.MainWindowHandle)) 'Replace trigger before caret'
            if ($null -ne $manualCommand) { break }
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($process.MainWindowHandle -eq [IntPtr]::Zero) { throw 'Notepad++ did not create its main window.' }
    if ($null -eq $manualCommand) { throw 'The manual replacement command was not found in the English override menu.' }
    $editor = Find-VisibleScintilla $process.MainWindowHandle
    if ($editor -eq [IntPtr]::Zero) { throw 'The visible Scintilla editor was not found.' }
    Focus-Editor $process.MainWindowHandle $editor

    Set-EditorText $editor 'ticket-42-open'
    Set-Caret $editor 14
    [void][NppQrFeatureNative]::SendMessage($process.MainWindowHandle, 0x0111, [IntPtr]$manualCommand, [IntPtr]::Zero)
    Assert-Equal (Get-EditorText $editor) '#42:open' 'Capture-template replacement'

    Set-EditorText $editor 'snip'
    Set-Caret $editor 4
    [void][NppQrFeatureNative]::SendMessage($process.MainWindowHandle, 0x0111, [IntPtr]$manualCommand, [IntPtr]::Zero)
    Assert-Equal (Get-EditorText $editor) 'ABCD' 'Snippet expansion'
    Assert-Equal (Get-Caret $editor) 1 'First tabstop'

    Set-EditorText $editor "mm`nmm"
    [void][NppQrFeatureNative]::SendMessage($editor, 2571, [IntPtr]::Zero, [IntPtr]::Zero)
    Set-Caret $editor 2
    [void][NppQrFeatureNative]::SendMessage($editor, 2573, [IntPtr]5, [IntPtr]5)
    [void][NppQrFeatureNative]::SendMessage($process.MainWindowHandle, 0x0111, [IntPtr]$manualCommand, [IntPtr]::Zero)
    Assert-Equal (Get-EditorText $editor) "XX`nXX" 'Multi-cursor replacement'
    [void][NppQrFeatureNative]::SendMessage($editor, 2176, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    Assert-Equal (Get-EditorText $editor) "mm`nmm" 'Single multi-cursor Undo transaction'

    $reloadedRules = @"
{
  "version": 1,
  "items": [
    {
      "id": "hot-reload",
      "trigger": "hot",
      "replacement": "RELOADED",
      "activation": ["space"]
    }
  ]
}
"@
    [IO.File]::WriteAllText($rulesPath, $reloadedRules, [Text.UTF8Encoding]::new($false))
    Start-Sleep -Milliseconds 900
    Set-EditorText $editor 'hot'
    Set-Caret $editor 3
    [void][NppQrFeatureNative]::SendMessage($process.MainWindowHandle, 0x0111, [IntPtr]$manualCommand, [IntPtr]::Zero)
    Assert-Equal (Get-EditorText $editor) 'RELOADED' 'Automatic valid rule-file reload'

    [IO.File]::WriteAllText($rulesPath, '{ invalid json', [Text.UTF8Encoding]::new($false))
    Start-Sleep -Milliseconds 900
    Set-EditorText $editor 'hot'
    Set-Caret $editor 3
    [void][NppQrFeatureNative]::SendMessage($process.MainWindowHandle, 0x0111, [IntPtr]$manualCommand, [IntPtr]::Zero)
    Assert-Equal (Get-EditorText $editor) 'RELOADED' 'Invalid hot reload preserves last valid in-memory rules'

    $process.Refresh()
    if (-not $process.Responding) { throw 'Notepad++ stopped responding during feature tests.' }
    [void][NppQrFeatureNative]::SendMessage($editor, 2014, [IntPtr]::Zero, [IntPtr]::Zero)
    [void][NppQrFeatureNative]::PostMessage($process.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) { throw 'Notepad++ did not exit after feature tests.' }
    Write-Host 'Feature smoke test passed: captures, tabstops, multi-cursor Undo, automatic reload, and invalid-file fallback.'
}
finally {
    if ($script:processHandle -and $script:processHandle -ne [IntPtr]::Zero) {
        [void][NppQrFeatureNative]::CloseHandle($script:processHandle)
        $script:processHandle = [IntPtr]::Zero
    }
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
    if ($null -eq $configBackup) { Remove-Item -LiteralPath $configPath -Force -ErrorAction SilentlyContinue }
    else { [IO.File]::WriteAllBytes($configPath, $configBackup) }
    if ($null -eq $rulesBackup) { Remove-Item -LiteralPath $rulesPath -Force -ErrorAction SilentlyContinue }
    else { [IO.File]::WriteAllBytes($rulesPath, $rulesBackup) }
}
