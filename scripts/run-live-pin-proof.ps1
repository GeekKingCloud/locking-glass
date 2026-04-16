$ErrorActionPreference = 'Stop'

$repoRoot = 'C:\Users\thete\Documents\Code\Tools\locking-glass\.reef\octopush\worktrees\004\17-implement-real-pinned-monitor-window-mov'
$proofDir = Join-Path $repoRoot 'build\windows-live-pin-proof'
$watchExe = Join-Path $repoRoot 'build-win\bin\locking_glass.exe'
$helperDllPath = Join-Path $repoRoot 'build\windows-live-desktop-probe\VirtualDesktopAccessor.dll'
$watchStdout = Join-Path $proofDir 'locking-glass-watch.stdout.txt'
$watchStderr = Join-Path $proofDir 'locking-glass-watch.stderr.txt'
$sessionPath = Join-Path $proofDir 'proof-session-state.tsv'
$stateJson = Join-Path $proofDir 'proof-state.json'
$helperReleaseUrl = 'https://github.com/Ciantic/VirtualDesktopAccessor/releases/download/2024-12-16-windows11/VirtualDesktopAccessor.dll'

New-Item -ItemType Directory -Force -Path $proofDir | Out-Null
Remove-Item $watchStdout, $watchStderr, $sessionPath, ($sessionPath + '.invalid'), $stateJson -ErrorAction SilentlyContinue

if (-not (Test-Path $helperDllPath)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $helperDllPath) | Out-Null
    Invoke-WebRequest -Uri $helperReleaseUrl -OutFile $helperDllPath
}

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class LockingGlassProofWin32
{
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextLengthW(IntPtr hwnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int maxCount);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hwnd);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool MoveWindow(IntPtr hwnd, int x, int y, int width, int height, bool repaint);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hwnd, int nCmdShow);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern void keybd_event(byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);

    public static IntPtr FindWindowByTitleFragment(string titleFragment)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows(
            delegate(IntPtr hwnd, IntPtr lParam)
            {
                if (!IsWindowVisible(hwnd))
                {
                    return true;
                }

                var length = GetWindowTextLengthW(hwnd);
                if (length <= 0)
                {
                    return true;
                }

                var builder = new StringBuilder(length + 1);
                GetWindowTextW(hwnd, builder, builder.Capacity);
                if (builder.ToString().IndexOf(titleFragment, StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    found = hwnd;
                    return false;
                }

                return true;
            },
            IntPtr.Zero);
        return found;
    }

    public static void PositionWindow(IntPtr hwnd, int x, int y, int width, int height)
    {
        if (!MoveWindow(hwnd, x, y, width, height, true))
        {
            throw new InvalidOperationException("MoveWindow failed for HWND 0x" + hwnd.ToInt64().ToString("X") + ".");
        }

        ShowWindow(hwnd, 5);
        SetForegroundWindow(hwnd);
    }

    public static void SwitchDesktop(bool moveRight)
    {
        const uint KEYEVENTF_KEYUP = 0x0002;
        const byte VK_CONTROL = 0x11;
        const byte VK_LWIN = 0x5B;
        byte arrow = moveRight ? (byte)0x27 : (byte)0x25;

        keybd_event(VK_CONTROL, 0, 0, UIntPtr.Zero);
        keybd_event(VK_LWIN, 0, 0, UIntPtr.Zero);
        keybd_event(arrow, 0, 0, UIntPtr.Zero);
        keybd_event(arrow, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
        keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
        keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    }
}

public sealed class LockingGlassProofHelper : IDisposable
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryW(string fileName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string procName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeLibrary(IntPtr module);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate int GetDesktopCountDelegate();

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate int GetCurrentDesktopNumberDelegate();

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate int GetWindowDesktopNumberDelegate(IntPtr hwnd);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate int MoveWindowToDesktopNumberDelegate(IntPtr hwnd, int desktopNumber);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate int GoToDesktopNumberDelegate(int desktopNumber);

    private readonly IntPtr _library;
    private readonly GetDesktopCountDelegate _getDesktopCount;
    private readonly GetCurrentDesktopNumberDelegate _getCurrentDesktopNumber;
    private readonly GetWindowDesktopNumberDelegate _getWindowDesktopNumber;
    private readonly MoveWindowToDesktopNumberDelegate _moveWindowToDesktopNumber;
    private readonly GoToDesktopNumberDelegate _goToDesktopNumber;

    public LockingGlassProofHelper(string helperDllPath)
    {
        _library = LoadLibraryW(helperDllPath);
        if (_library == IntPtr.Zero)
        {
            throw new InvalidOperationException("LoadLibraryW failed for " + helperDllPath + ".");
        }

        _getDesktopCount = LoadRequiredDelegate<GetDesktopCountDelegate>("GetDesktopCount");
        _getCurrentDesktopNumber = LoadRequiredDelegate<GetCurrentDesktopNumberDelegate>("GetCurrentDesktopNumber");
        _getWindowDesktopNumber = LoadRequiredDelegate<GetWindowDesktopNumberDelegate>("GetWindowDesktopNumber");
        _moveWindowToDesktopNumber = LoadRequiredDelegate<MoveWindowToDesktopNumberDelegate>("MoveWindowToDesktopNumber");
        _goToDesktopNumber = LoadRequiredDelegate<GoToDesktopNumberDelegate>("GoToDesktopNumber");
    }

    public int GetDesktopCount()
    {
        return _getDesktopCount();
    }

    public int GetCurrentDesktopNumber()
    {
        return _getCurrentDesktopNumber();
    }

    public int GetWindowDesktopNumber(IntPtr hwnd)
    {
        return _getWindowDesktopNumber(hwnd);
    }

    public void MoveWindowToDesktopNumber(IntPtr hwnd, int desktopNumber)
    {
        var result = _moveWindowToDesktopNumber(hwnd, desktopNumber);
        if (result < 0)
        {
            throw new InvalidOperationException("MoveWindowToDesktopNumber(" + desktopNumber + ") failed for HWND 0x" + hwnd.ToInt64().ToString("X") + ".");
        }
    }

    public void GoToDesktopNumber(int desktopNumber)
    {
        var result = _goToDesktopNumber(desktopNumber);
        if (result < 0)
        {
            throw new InvalidOperationException("GoToDesktopNumber(" + desktopNumber + ") failed.");
        }
    }

    public void Dispose()
    {
        if (_library != IntPtr.Zero)
        {
            FreeLibrary(_library);
        }
    }

    private T LoadRequiredDelegate<T>(string exportName) where T : class
    {
        var exportHandle = GetProcAddress(_library, exportName);
        if (exportHandle == IntPtr.Zero)
        {
            throw new InvalidOperationException("Missing helper export '" + exportName + "'.");
        }

        var delegateObject = Marshal.GetDelegateForFunctionPointer(exportHandle, typeof(T)) as T;
        if (delegateObject == null)
        {
            throw new InvalidOperationException("Failed to bind helper export '" + exportName + "'.");
        }

        return delegateObject;
    }
}
"@

function Parse-Monitors {
    $selfCheck = & $watchExe --self-check
    $monitors = @()
    foreach ($line in $selfCheck) {
        if ($line -notmatch '^\s*-\s+(Display \d+)\s+"([^"]*)"\s+\[id=(.+?)\]\s+\(([-\d]+),([-\d]+)\)-\(([-\d]+),([-\d]+)\)\s*(.*)$') {
            continue
        }

        $status = $Matches[8]
        $monitors += [pscustomobject]@{
            Label = $Matches[1]
            DisplayName = $Matches[2]
            StableId = $Matches[3]
            DevicePath = $Matches[3]
            Left = [int]$Matches[4]
            Top = [int]$Matches[5]
            Right = [int]$Matches[6]
            Bottom = [int]$Matches[7]
            IsPrimary = $status -match '\bprimary\b'
        }
    }

    return $monitors
}

function Write-SessionFile([array]$monitors, [string]$lockedLabel, [string]$path) {
    function Escape-Field([string]$value) {
        return $value.Replace('\', '\\').Replace("`t", '\t').Replace("`n", '\n')
    }

    $lines = @('version' + [char]9 + '1')
    foreach ($monitor in $monitors) {
        $locked = if ($monitor.Label -eq $lockedLabel) { '1' } else { '0' }
        $primary = if ($monitor.IsPrimary) { '1' } else { '0' }
        $fields = @(
            'monitor',
            $monitor.StableId,
            $monitor.DevicePath,
            '',
            $monitor.DisplayName,
            $monitor.Label,
            [string]$monitor.Left,
            [string]$monitor.Top,
            [string]$monitor.Right,
            [string]$monitor.Bottom,
            $primary,
            $locked,
            '0'
        ) | ForEach-Object { Escape-Field $_ }
        $lines += ($fields -join [char]9)
    }

    [System.IO.File]::WriteAllText(
        $path,
        (($lines -join "`n") + "`n"),
        [System.Text.Encoding]::ASCII)
}

function Start-NotepadWindow([string]$titleFragment) {
    $filePath = Join-Path $proofDir ($titleFragment + '.txt')
    Set-Content -Path $filePath -Value $titleFragment -Encoding UTF8
    $process = Start-Process -FilePath 'notepad.exe' -ArgumentList ('"' + $filePath + '"') -PassThru
    for ($attempt = 0; $attempt -lt 80; $attempt += 1) {
        $handle = [LockingGlassProofWin32]::FindWindowByTitleFragment($titleFragment)
        if ($handle -ne [IntPtr]::Zero) {
            return [pscustomobject]@{
                Process = $process
                Handle = $handle
                FilePath = $filePath
                TitleFragment = $titleFragment
            }
        }

        Start-Sleep -Milliseconds 100
    }

    throw "Could not find a Notepad window containing title fragment '$titleFragment'."
}

function Place-WindowOnMonitor($window, $monitor) {
    $width = [Math]::Max(500, [Math]::Min(900, $monitor.Right - $monitor.Left - 160))
    $height = [Math]::Max(360, [Math]::Min(700, $monitor.Bottom - $monitor.Top - 160))
    $x = $monitor.Left + 80
    $y = $monitor.Top + 80
    [LockingGlassProofWin32]::PositionWindow($window.Handle, $x, $y, $width, $height)
}

function Place-AmbiguousWindow($window, $leftMonitor, $rightMonitor) {
    $height = [Math]::Max(360, [Math]::Min(700, $leftMonitor.Bottom - $leftMonitor.Top - 220))
    $x = $leftMonitor.Right - 300
    $y = $leftMonitor.Top + 120
    $width = [Math]::Min(900, ($rightMonitor.Left + 600) - $x)
    [LockingGlassProofWin32]::PositionWindow($window.Handle, $x, $y, $width, $height)
}

function Get-WindowDesktopMap($helper, [array]$windows) {
    $result = @{}
    foreach ($window in $windows) {
        $result[$window.TitleFragment] = $helper.GetWindowDesktopNumber($window.Handle)
    }
    return $result
}

function Move-WindowToDesktop($helper, $window, [int]$desktopNumber, [string]$label) {
    $helper.MoveWindowToDesktopNumber($window.Handle, $desktopNumber)
    for ($attempt = 0; $attempt -lt 40; $attempt += 1) {
        Start-Sleep -Milliseconds 100
        if ($helper.GetWindowDesktopNumber($window.Handle) -eq $desktopNumber) {
            return
        }
    }

    throw "Window '$label' did not settle onto desktop $desktopNumber."
}

function Invoke-DesktopSwitch($helper, [int]$targetDesktop) {
    $helper.GoToDesktopNumber($targetDesktop)
    for ($attempt = 0; $attempt -lt 40; $attempt += 1) {
        Start-Sleep -Milliseconds 250
        if ($helper.GetCurrentDesktopNumber() -eq $targetDesktop) {
            return
        }
    }

    throw "Desktop switch to $targetDesktop did not complete."
}

$monitors = Parse-Monitors
if ($monitors.Count -lt 2) {
    throw 'Target-runtime proof requires at least two monitors.'
}

$lockedMonitor = $monitors | Where-Object { $_.Label -eq 'Display 2' } | Select-Object -First 1
$unlockedMonitor = $monitors | Where-Object { $_.Label -eq 'Display 3' } | Select-Object -First 1
if ($null -eq $lockedMonitor -or $null -eq $unlockedMonitor) {
    $lockedMonitor = $monitors[0]
    $unlockedMonitor = $monitors[1]
}

if ($lockedMonitor.Right -ge $unlockedMonitor.Left) {
    $candidate = $monitors |
        Where-Object { $_.Left -gt $lockedMonitor.Left } |
        Sort-Object Left |
        Select-Object -First 1
    if ($null -ne $candidate) {
        $unlockedMonitor = $candidate
    }
}

Write-SessionFile -monitors $monitors -lockedLabel $lockedMonitor.Label -path $sessionPath

$watchProcess = $null
$createdWindows = @()

try {
    $helper = [LockingGlassProofHelper]::new($helperDllPath)
    try {
        $desktopCount = $helper.GetDesktopCount()
        if ($desktopCount -lt 2) {
            throw 'Target-runtime proof requires at least two virtual desktops.'
        }

        $initialDesktop = $helper.GetCurrentDesktopNumber()
        $alternateDesktop = if ($initialDesktop -eq 0) { 1 } else { 0 }

        $createdWindows += Start-NotepadWindow -titleFragment 'lg-locked-source'
        Place-WindowOnMonitor -window $createdWindows[-1] -monitor $lockedMonitor
        Move-WindowToDesktop -helper $helper -window $createdWindows[-1] -desktopNumber $initialDesktop -label 'lg-locked-source'

        $createdWindows += Start-NotepadWindow -titleFragment 'lg-unlocked-source'
        Place-WindowOnMonitor -window $createdWindows[-1] -monitor $unlockedMonitor
        Move-WindowToDesktop -helper $helper -window $createdWindows[-1] -desktopNumber $initialDesktop -label 'lg-unlocked-source'

        $createdWindows += Start-NotepadWindow -titleFragment 'lg-ambiguous-source'
        Place-AmbiguousWindow -window $createdWindows[-1] -leftMonitor $lockedMonitor -rightMonitor $unlockedMonitor
        Move-WindowToDesktop -helper $helper -window $createdWindows[-1] -desktopNumber $initialDesktop -label 'lg-ambiguous-source'

        Invoke-DesktopSwitch -helper $helper -targetDesktop $alternateDesktop
        Start-Sleep -Seconds 2

        $createdWindows += Start-NotepadWindow -titleFragment 'lg-locked-target'
        Place-WindowOnMonitor -window $createdWindows[-1] -monitor $lockedMonitor
        Move-WindowToDesktop -helper $helper -window $createdWindows[-1] -desktopNumber $alternateDesktop -label 'lg-locked-target'

        $createdWindows += Start-NotepadWindow -titleFragment 'lg-unlocked-target'
        Place-WindowOnMonitor -window $createdWindows[-1] -monitor $unlockedMonitor
        Move-WindowToDesktop -helper $helper -window $createdWindows[-1] -desktopNumber $alternateDesktop -label 'lg-unlocked-target'

        Invoke-DesktopSwitch -helper $helper -targetDesktop $initialDesktop
        Start-Sleep -Seconds 2

        Remove-Item $watchStdout, $watchStderr, $stateJson -ErrorAction SilentlyContinue

        $watchCommand =
            'set "LOCKING_GLASS_SESSION_PATH=' + $sessionPath + '" && ' +
            'set "LOCKING_GLASS_VIRTUAL_DESKTOP_HELPER=' + $helperDllPath + '" && ' +
            '"' + $watchExe + '" --watch-virtual-desktops'
        $watchProcess = Start-Process `
            -FilePath 'cmd.exe' `
            -ArgumentList '/d', '/s', '/c', $watchCommand `
            -WorkingDirectory $repoRoot `
            -WindowStyle Hidden `
            -PassThru `
            -RedirectStandardOutput $watchStdout `
            -RedirectStandardError $watchStderr
        Start-Sleep -Seconds 5

        $switchStates = @()

        Invoke-DesktopSwitch -helper $helper -targetDesktop $alternateDesktop
        Start-Sleep -Seconds 6
        $switchStates += [pscustomobject]@{
            from = $initialDesktop
            to = $alternateDesktop
            desktops = Get-WindowDesktopMap -helper $helper -windows $createdWindows
        }

        Invoke-DesktopSwitch -helper $helper -targetDesktop $initialDesktop
        Start-Sleep -Seconds 6
        $switchStates += [pscustomobject]@{
            from = $alternateDesktop
            to = $initialDesktop
            desktops = Get-WindowDesktopMap -helper $helper -windows $createdWindows
        }

        Invoke-DesktopSwitch -helper $helper -targetDesktop $alternateDesktop
        Start-Sleep -Seconds 6
        $switchStates += [pscustomobject]@{
            from = $initialDesktop
            to = $alternateDesktop
            desktops = Get-WindowDesktopMap -helper $helper -windows $createdWindows
        }

        $watchTimedOut = $false
        if (-not $watchProcess.WaitForExit(120000)) {
            $watchTimedOut = $true
            $watchProcess.Kill()
            $watchProcess.WaitForExit(5000)
        }
        $watchProcess.WaitForExit()

        $stdout = if (Test-Path $watchStdout) {
            [System.IO.File]::ReadAllText($watchStdout)
        }
        else {
            ''
        }
        $stderr = if (Test-Path $watchStderr) {
            [System.IO.File]::ReadAllText($watchStderr)
        }
        else {
            ''
        }

        $proofState = [pscustomobject]@{
            locked_monitor = $lockedMonitor.Label
            unlocked_monitor = $unlockedMonitor.Label
            initial_desktop = $initialDesktop
            alternate_desktop = $alternateDesktop
            switch_states = $switchStates
            watch_exit_code = $watchProcess.ExitCode
            watch_timed_out = $watchTimedOut
        }
        $proofState | ConvertTo-Json -Depth 5 | Set-Content -Path $stateJson -Encoding UTF8

        Write-Host ('watch stdout: ' + $watchStdout)
        Write-Host ('watch stderr: ' + $watchStderr)
        Write-Host ('proof state: ' + $stateJson)
        Write-Host ('watch exit code: ' + $watchProcess.ExitCode)
        if ($stdout.Length -gt 0) {
            $stdout
        }
        if ($stderr.Length -gt 0) {
            $stderr
        }

        if ($watchTimedOut) {
            throw 'locking_glass.exe did not exit after the driven live desktop switches.'
        }
    }
    finally {
        if ($helper) {
            $helper.Dispose()
        }
    }
}
finally {
    if ($watchProcess -and -not $watchProcess.HasExited) {
        $watchProcess.Kill()
        $watchProcess.WaitForExit(5000)
    }

    foreach ($window in $createdWindows) {
        try {
            if ($window.Process -and -not $window.Process.HasExited) {
                $window.Process.Kill()
                $window.Process.WaitForExit(2000)
            }
        }
        catch {
        }
    }
}
