[CmdletBinding()]
param(
    [string]$NotepadPlusPlus = "$env:ProgramFiles\Notepad++\notepad++.exe",
    [int]$TimeoutSeconds = 15
)

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NppQrNative {
    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr menu, int position);
    [DllImport("user32.dll")] public static extern uint GetMenuItemID(IntPtr menu, int position);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetMenuString(IntPtr menu, uint item, StringBuilder text, int count, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
}
"@

function Get-WindowText([IntPtr]$Handle) {
    $buffer = [Text.StringBuilder]::new(1024)
    [void][NppQrNative]::GetWindowText($Handle, $buffer, $buffer.Capacity)
    $buffer.ToString()
}

function Get-TopWindows {
    $result = [Collections.Generic.List[object]]::new()
    $callback = [NppQrNative+EnumWindowsProc]{
        param([IntPtr]$handle, [IntPtr]$unused)
        $title = Get-WindowText $handle
        if ($title) { $result.Add([pscustomobject]@{ Handle=$handle; Title=$title }) }
        return $true
    }
    [void][NppQrNative]::EnumWindows($callback, [IntPtr]::Zero)
    $result
}

function Wait-Window([scriptblock]$Predicate, [string]$Description) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $found = Get-TopWindows | Where-Object $Predicate | Select-Object -First 1
        if ($found) { return $found }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for $Description."
}

function Find-MenuCommand([IntPtr]$Menu, [string]$Text) {
    if ($Menu -eq [IntPtr]::Zero) { return $null }
    $count = [NppQrNative]::GetMenuItemCount($Menu)
    for ($index = 0; $index -lt $count; $index++) {
        $buffer = [Text.StringBuilder]::new(512)
        [void][NppQrNative]::GetMenuString($Menu, [uint32]$index, $buffer, $buffer.Capacity, 0x400)
        $label = $buffer.ToString().Replace('&','')
        $subMenu = [NppQrNative]::GetSubMenu($Menu, $index)
        if ($label -like "*$Text*") {
            $id = [NppQrNative]::GetMenuItemID($Menu, $index)
            if ($id -ne 0xFFFFFFFF) { return $id }
        }
        if ($subMenu -ne [IntPtr]::Zero) {
            $nested = Find-MenuCommand $subMenu $Text
            if ($null -ne $nested) { return $nested }
        }
    }
    return $null
}

function Get-ChildTexts([IntPtr]$Parent) {
    $result = [Collections.Generic.List[string]]::new()
    $callback = [NppQrNative+EnumWindowsProc]{
        param([IntPtr]$handle, [IntPtr]$unused)
        $title = Get-WindowText $handle
        if ($title) { $result.Add($title) }
        return $true
    }
    [void][NppQrNative]::EnumChildWindows($Parent, $callback, [IntPtr]::Zero)
    $result
}

function Find-ChildByText([IntPtr]$Parent, [string]$Text) {
    $script:matchedChild = [IntPtr]::Zero
    $callback = [NppQrNative+EnumWindowsProc]{
        param([IntPtr]$handle, [IntPtr]$unused)
        if ((Get-WindowText $handle).Replace('&','') -like "*$Text*") {
            $script:matchedChild = $handle
            return $false
        }
        return $true
    }
    [void][NppQrNative]::EnumChildWindows($Parent, $callback, [IntPtr]::Zero)
    $script:matchedChild
}

$started = Get-Date
$process = Start-Process -FilePath $NotepadPlusPlus -ArgumentList '-multiInst','-nosession' -PassThru
try {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $process.Refresh()
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($process.MainWindowHandle -eq [IntPtr]::Zero) { throw 'The test Notepad++ instance has no main window.' }
    $main = [pscustomobject]@{ Handle = $process.MainWindowHandle; Title = $process.MainWindowTitle }

    $command = $null
    do {
        $command = Find-MenuCommand ([NppQrNative]::GetMenu($main.Handle)) 'Manage replacement rules'
        if ($null -ne $command) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($null -eq $command) { throw 'The NppQuickReplace manager command was not found in the Notepad++ menu.' }
    [void][NppQrNative]::SendMessage($main.Handle, 0x0111, [IntPtr]$command, [IntPtr]::Zero)

    $manager = Wait-Window { $_.Title -like '*NppQuickReplace*Replacement Manager*' } 'the replacement manager'
    $texts = Get-ChildTexts $manager.Handle
    foreach ($required in @('Path globs','Languages','Set state','Preview')) {
        if (-not ($texts | Where-Object { $_.Replace('&','') -like "*$required*" })) {
            throw "Manager control '$required' was not found."
        }
    }

    $groupsButton = Find-ChildByText $manager.Handle 'Groups'
    if ($groupsButton -eq [IntPtr]::Zero) { throw 'The Groups button was not found.' }
    [void][NppQrNative]::PostMessage($groupsButton, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    $groupManager = Wait-Window { $_.Title -like '*NppQuickReplace*Group Manager*' } 'the group manager'

    $process.Refresh()
    if (-not $process.Responding) { throw 'Notepad++ stopped responding while both managers were open.' }
    [void][NppQrNative]::PostMessage($main.Handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        throw 'Notepad++ did not exit cleanly with the nested group manager open.'
    }

    $crashes = Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$started; Id=1000} -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -like '*notepad++.exe*' }
    if ($crashes) { throw 'Windows recorded an Application Error for notepad++.exe during the smoke test.' }
    Write-Host 'GUI smoke test passed: manager controls, nested group window, responsiveness, and clean shutdown.'
}
finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
}