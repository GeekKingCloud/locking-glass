param(
    [string]$WatchExe,
    [string]$WorkingDirectory,
    [string]$HelperDllPath,
    [string]$ProofDir
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

. (Join-Path $PSScriptRoot 'resolve-virtual-desktop-helper.ps1')

if ([string]::IsNullOrWhiteSpace($WatchExe)) {
    $watchExe = Join-Path $repoRoot 'build-win\bin\locking_glass.exe'
}

if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
    $WorkingDirectory = Split-Path -Parent $WatchExe
}

if (-not (Test-Path $watchExe)) {
    throw "Missing Windows build: $watchExe. Build it first with mingw."
}

if ([string]::IsNullOrWhiteSpace($ProofDir)) {
    $ProofDir = Join-Path $repoRoot 'build\windows-live-background-proof'
}

New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

$defaultHelperDllPath = Join-Path $repoRoot 'build\windows-live-desktop-probe\VirtualDesktopAccessor.dll'
$adjacentHelperDllPath = Join-Path (Split-Path -Parent $WatchExe) 'VirtualDesktopAccessor.dll'
if ([string]::IsNullOrWhiteSpace($HelperDllPath)) {
    $HelperDllPath = $defaultHelperDllPath
}
else {
    $requestedHelperPath = [System.IO.Path]::GetFullPath($HelperDllPath)
    $repoBuildRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'build-win'))
    $watchExeFullPath = [System.IO.Path]::GetFullPath($WatchExe)
    $isRepoBuild = $watchExeFullPath.StartsWith($repoBuildRoot, [System.StringComparison]::OrdinalIgnoreCase)
    $isStagedHelper = [System.IO.Path]::GetFullPath($defaultHelperDllPath) -eq $requestedHelperPath
    $isInstalledAdjacentHelper = -not $isRepoBuild -and [System.IO.Path]::GetFullPath($adjacentHelperDllPath) -eq $requestedHelperPath
    if (-not ($isStagedHelper -or $isInstalledAdjacentHelper)) {
        throw 'Locking Glass no longer loads arbitrary helper DLL paths at runtime. Use the staged helper before running this proof.'
    }
}

$HelperDllPath = Resolve-LockingGlassVirtualDesktopHelper -HelperDllPath $HelperDllPath

$watchStdout = Join-Path $ProofDir 'background.stdout.txt'
$watchStderr = Join-Path $ProofDir 'background.stderr.txt'
$backgroundReportPath = Join-Path $ProofDir 'background.desktop-switch-reports.txt'
$sessionPath = Join-Path $ProofDir 'session.tsv'
$stateJson = Join-Path $ProofDir 'proof-state.json'
Remove-Item $watchStdout, $watchStderr, $backgroundReportPath, $sessionPath, ($sessionPath + '.invalid'), $stateJson -ErrorAction SilentlyContinue

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class LockingGlassBackgroundProofWin32
{
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr FindWindowW(string lpClassName, string lpWindowName);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SendMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);

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

    [DllImport("user32.dll", SetLastError = true)]
    private static extern int GetMenuItemCount(IntPtr hMenu);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetMenuStringW(IntPtr hMenu, uint uIDItem, StringBuilder lpString, int cchMax, uint flags);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetMenuState(IntPtr hMenu, uint uId, uint uFlags);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetMenuItemRect(IntPtr hWnd, IntPtr hMenu, uint uItem, out RECT lprcItem);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    private const uint MN_GETHMENU = 0x01E1;
    private const uint MF_BYPOSITION = 0x00000400;
    private const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    private const uint MOUSEEVENTF_LEFTUP = 0x0004;

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

    public static IntPtr FindBackgroundWindow()
    {
        return FindWindowW("LockingGlassBackgroundWindow", null);
    }

    public static IntPtr FindPopupMenuWindow()
    {
        return FindWindowW("#32768", null);
    }

    public static IntPtr GetPopupMenuHandle(IntPtr menuWindow)
    {
        if (menuWindow == IntPtr.Zero)
        {
            return IntPtr.Zero;
        }

        return SendMessageW(menuWindow, MN_GETHMENU, IntPtr.Zero, IntPtr.Zero);
    }

    public static int GetPopupMenuItemCount(IntPtr menuHandle)
    {
        return menuHandle == IntPtr.Zero ? 0 : GetMenuItemCount(menuHandle);
    }

    public static string GetPopupMenuItemText(IntPtr menuHandle, int index)
    {
        if (menuHandle == IntPtr.Zero)
        {
            return string.Empty;
        }

        var builder = new StringBuilder(512);
        var length = GetMenuStringW(menuHandle, (uint)index, builder, builder.Capacity, MF_BYPOSITION);
        return length <= 0 ? string.Empty : builder.ToString();
    }

    public static uint GetPopupMenuItemState(IntPtr menuHandle, int index)
    {
        return menuHandle == IntPtr.Zero ? 0U : GetMenuState(menuHandle, (uint)index, MF_BYPOSITION);
    }

    public static bool TryGetPopupMenuItemCenter(IntPtr menuHandle, int index, out int centerX, out int centerY)
    {
        centerX = 0;
        centerY = 0;
        if (menuHandle == IntPtr.Zero)
        {
            return false;
        }

        RECT rect;
        if (!GetMenuItemRect(IntPtr.Zero, menuHandle, (uint)index, out rect))
        {
            return false;
        }

        centerX = rect.Left + ((rect.Right - rect.Left) / 2);
        centerY = rect.Top + ((rect.Bottom - rect.Top) / 2);
        return true;
    }

    public static void ClickScreenPoint(int x, int y)
    {
        if (!SetCursorPos(x, y))
        {
            throw new InvalidOperationException("SetCursorPos failed for " + x + "," + y + ".");
        }

        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, UIntPtr.Zero);
    }

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
}

public sealed class LockingGlassBackgroundProofHelper : IDisposable
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

    public LockingGlassBackgroundProofHelper(string helperDllPath)
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
        if ($line -notmatch '^\s*-\s+(Display \d+)(?:\s+"([^"]*)")?\s+\[id=(.+?)\]\s+\(([-\d]+),([-\d]+)\)-\(([-\d]+),([-\d]+)\)(.*)$') {
            continue
        }

        $tail = $Matches[8]
        $devicePath = $Matches[3]
        if ($tail -match 'path=([^ ]+)') {
            $devicePath = $Matches[1]
        }

        $monitors += [pscustomobject]@{
            Label = $Matches[1]
            DisplayName = $Matches[2]
            StableId = $Matches[3]
            DevicePath = $devicePath
            Left = [int]$Matches[4]
            Top = [int]$Matches[5]
            Right = [int]$Matches[6]
            Bottom = [int]$Matches[7]
            IsPrimary = $tail -match '\bprimary\b'
        }
    }

    return $monitors
}

function Escape-Field([string]$value) {
    if ($null -eq $value) {
        return ''
    }
    return $value.Replace('\', '\\').Replace("`t", '\t').Replace("`n", '\n')
}

function Write-SessionFile([array]$monitors, [string]$lockedLabel, [string]$path) {
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

function Read-LockState([string]$path, [string]$monitorLabel) {
    if (-not (Test-Path $path)) {
        return $null
    }

    foreach ($line in [System.IO.File]::ReadAllLines($path)) {
        if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith('version')) {
            continue
        }

        $fields = $line.Split("`t")
        if ($fields.Length -lt 13 -or $fields[0] -ne 'monitor' -or $fields[5] -ne $monitorLabel) {
            continue
        }

        return $fields[11] -eq '1'
    }

    return $null
}

function Wait-ForLockState([string]$path, [string]$monitorLabel, [bool]$expected) {
    for ($attempt = 0; $attempt -lt 80; $attempt += 1) {
        $value = Read-LockState -path $path -monitorLabel $monitorLabel
        if ($null -ne $value -and $value -eq $expected) {
            return
        }
        Start-Sleep -Milliseconds 100
    }

    throw "Timed out waiting for monitor '$monitorLabel' lock state $expected in $path."
}

function Start-NotepadWindow([string]$titleFragment) {
    $filePath = Join-Path $ProofDir ($titleFragment + '.txt')
    Set-Content -Path $filePath -Value $titleFragment -Encoding UTF8
    $process = Start-Process -FilePath 'notepad.exe' -ArgumentList ('"' + $filePath + '"') -PassThru
    for ($attempt = 0; $attempt -lt 80; $attempt += 1) {
        $handle = [LockingGlassBackgroundProofWin32]::FindWindowByTitleFragment($titleFragment)
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
    [LockingGlassBackgroundProofWin32]::PositionWindow($window.Handle, $x, $y, $width, $height)
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
    $currentDesktop = $helper.GetCurrentDesktopNumber()
    if ($currentDesktop -eq $targetDesktop) {
        return
    }

    if ($targetDesktop -eq ($currentDesktop + 1)) {
        [LockingGlassBackgroundProofWin32]::SwitchDesktop($true)
    }
    elseif ($targetDesktop -eq ($currentDesktop - 1)) {
        [LockingGlassBackgroundProofWin32]::SwitchDesktop($false)
    }
    else {
        throw "Normal desktop switch proof only supports adjacent desktop transitions. Current=$currentDesktop target=$targetDesktop."
    }

    for ($attempt = 0; $attempt -lt 40; $attempt += 1) {
        Start-Sleep -Milliseconds 250
        if ($helper.GetCurrentDesktopNumber() -eq $targetDesktop) {
            return
        }
    }

    throw "Desktop switch to $targetDesktop did not complete."
}

function Get-WindowDesktopMap($helper, [array]$windows) {
    $result = @{}
    foreach ($window in $windows) {
        $result[$window.TitleFragment] = $helper.GetWindowDesktopNumber($window.Handle)
    }
    return $result
}

function Get-BackgroundReportCount([string]$path) {
    if (-not (Test-Path $path)) {
        return 0
    }

    $content = [System.IO.File]::ReadAllText($path)
    return ([regex]::Matches($content, [regex]::Escape('Locking Glass desktop switch policy'))).Count
}

function Wait-ForBackgroundReportCount([string]$path, [int]$expectedMinimum) {
    for ($attempt = 0; $attempt -lt 120; $attempt += 1) {
        if ((Get-BackgroundReportCount -path $path) -ge $expectedMinimum) {
            return
        }
        Start-Sleep -Milliseconds 250
    }

    throw "Timed out waiting for $expectedMinimum background desktop switch report(s) in $path."
}

function Get-BackgroundReportSummary([string]$path, [array]$windowTitles) {
    if (-not (Test-Path $path)) {
        return [pscustomobject]@{
            exists = $false
            report_count = 0
            size_bytes = 0
            window_mentions = @{}
            excerpt = @()
        }
    }

    $item = Get-Item $path
    $content = [System.IO.File]::ReadAllText($path)
    $lines = [System.IO.File]::ReadAllLines($path)
    $windowMentions = @{}
    foreach ($title in $windowTitles) {
        $windowMentions[$title] = ([regex]::Matches($content, [regex]::Escape($title))).Count
    }

    $excerpt = @(
        $lines |
            Where-Object {
                $_ -eq 'Locking Glass desktop switch policy' -or
                $_ -eq 'Locked monitors:' -or
                $_ -eq 'Moves:' -or
                $_ -eq 'Move results:' -or
                $_ -eq 'Resulting windows:' -or
                $_.Contains('  - restored locks:') -or
                $_.Contains('  - locked monitors:') -or
                $_.Contains('  - planned moves:') -or
                $_.Contains('  - from desktop:') -or
                $_.Contains('  - to desktop:') -or
                $_.Contains('lg-bg-') -or
                $_ -match ' : (moved|failed) \('
            } |
            Select-Object -First 160
    )

    return [pscustomobject]@{
        exists = $true
        path = $path
        report_count = ([regex]::Matches($content, [regex]::Escape('Locking Glass desktop switch policy'))).Count
        size_bytes = $item.Length
        window_mentions = $windowMentions
        excerpt = $excerpt
    }
}

function Get-WatcherProcessEvidence([int]$parentProcessId) {
    $all = Get-CimInstance Win32_Process
    $children = $all | Where-Object { $_.ParentProcessId -eq $parentProcessId }
    $grandchildren = foreach ($child in $children) { $all | Where-Object { $_.ParentProcessId -eq $child.ProcessId } }
    return [pscustomobject]@{
        children = @($children | Select-Object ProcessId, ParentProcessId, Name, CommandLine)
        grandchildren = @($grandchildren | Select-Object ProcessId, ParentProcessId, Name, CommandLine)
    }
}

function Stop-ProcessTree([int]$parentProcessId) {
    try {
        $all = @(Get-CimInstance Win32_Process)
        $children = @($all | Where-Object { $_.ParentProcessId -eq $parentProcessId })
        foreach ($child in $children) {
            Stop-ProcessTree -parentProcessId $child.ProcessId
        }

        foreach ($child in $children | Sort-Object -Property ProcessId -Descending) {
            Stop-Process -Id $child.ProcessId -Force -ErrorAction SilentlyContinue
        }
    }
    catch {
    }
}

function Open-MonitorLockMenu([IntPtr]$backgroundWindow) {
    if ($backgroundWindow -eq [IntPtr]::Zero) {
        throw 'Could not find Locking Glass background window.'
    }

    [void][LockingGlassBackgroundProofWin32]::PostMessageW($backgroundWindow, 0x8001, [IntPtr]::Zero, [IntPtr]0x0202)
}

function Toggle-MonitorLockFromTray([string]$monitorLabel, [bool]$expectedLockState, [string]$sessionPath) {
    $backgroundWindow = [LockingGlassBackgroundProofWin32]::FindBackgroundWindow()
    Open-MonitorLockMenu -backgroundWindow $backgroundWindow
    Start-Sleep -Milliseconds 500

    $menuWindow = [IntPtr]::Zero
    $menuHandle = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 80; $attempt += 1) {
        $menuWindow = [LockingGlassBackgroundProofWin32]::FindPopupMenuWindow()
        $menuHandle = [LockingGlassBackgroundProofWin32]::GetPopupMenuHandle($menuWindow)
        if ($menuWindow -ne [IntPtr]::Zero -and $menuHandle -ne [IntPtr]::Zero) {
            break
        }
        Start-Sleep -Milliseconds 100
    }

    if ($menuWindow -eq [IntPtr]::Zero -or $menuHandle -eq [IntPtr]::Zero) {
        throw 'Could not find the Win32 tray popup menu window.'
    }

    $itemCount = [LockingGlassBackgroundProofWin32]::GetPopupMenuItemCount($menuHandle)
    for ($index = 0; $index -lt $itemCount; $index += 1) {
        $text = [LockingGlassBackgroundProofWin32]::GetPopupMenuItemText($menuHandle, $index)
        if (-not $text.StartsWith($monitorLabel, [System.StringComparison]::OrdinalIgnoreCase)) {
            continue
        }

        $state = [LockingGlassBackgroundProofWin32]::GetPopupMenuItemState($menuHandle, $index)
        if (($state -band 0x0001) -ne 0 -or ($state -band 0x0002) -ne 0) {
            continue
        }

        $centerX = 0
        $centerY = 0
        if (-not [LockingGlassBackgroundProofWin32]::TryGetPopupMenuItemCenter($menuHandle, $index, [ref]$centerX, [ref]$centerY)) {
            throw "Could not resolve the tray popup bounds for '$text'."
        }

        [LockingGlassBackgroundProofWin32]::ClickScreenPoint($centerX, $centerY)
        Wait-ForLockState -path $sessionPath -monitorLabel $monitorLabel -expected $expectedLockState
        return
    }

    throw "Could not find an enabled tray menu item starting with '$monitorLabel'."
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

Write-SessionFile -monitors $monitors -lockedLabel '' -path $sessionPath

$watchProcess = $null
$createdWindows = @()

try {
    $helper = [LockingGlassBackgroundProofHelper]::new($HelperDllPath)
    try {
        $desktopCount = $helper.GetDesktopCount()
        if ($desktopCount -lt 2) {
            throw 'Target-runtime proof requires at least two virtual desktops.'
        }

        $env:LOCKING_GLASS_SESSION_PATH = $sessionPath
        $env:LOCKING_GLASS_BACKGROUND_REPORT_PATH = $backgroundReportPath
        $watchProcess = Start-Process -FilePath $watchExe -ArgumentList '--background' -WorkingDirectory $WorkingDirectory -WindowStyle Hidden -PassThru -RedirectStandardOutput $watchStdout -RedirectStandardError $watchStderr
        Start-Sleep -Seconds 7

        $watcherEvidence = Get-WatcherProcessEvidence -parentProcessId $watchProcess.Id
        $initialDesktop = $helper.GetCurrentDesktopNumber()
        $alternateDesktop = if ($initialDesktop -lt ($desktopCount - 1)) { $initialDesktop + 1 } else { $initialDesktop - 1 }

        $createdWindows += Start-NotepadWindow -titleFragment 'lg-bg-locked-source'
        Place-WindowOnMonitor -window $createdWindows[-1] -monitor $lockedMonitor
        Move-WindowToDesktop -helper $helper -window $createdWindows[-1] -desktopNumber $initialDesktop -label 'lg-bg-locked-source'

        $createdWindows += Start-NotepadWindow -titleFragment 'lg-bg-unlocked-source'
        Place-WindowOnMonitor -window $createdWindows[-1] -monitor $unlockedMonitor
        Move-WindowToDesktop -helper $helper -window $createdWindows[-1] -desktopNumber $initialDesktop -label 'lg-bg-unlocked-source'

        Invoke-DesktopSwitch -helper $helper -targetDesktop $alternateDesktop
        Start-Sleep -Seconds 2

        $createdWindows += Start-NotepadWindow -titleFragment 'lg-bg-locked-target'
        Place-WindowOnMonitor -window $createdWindows[-1] -monitor $lockedMonitor
        Move-WindowToDesktop -helper $helper -window $createdWindows[-1] -desktopNumber $alternateDesktop -label 'lg-bg-locked-target'

        $createdWindows += Start-NotepadWindow -titleFragment 'lg-bg-unlocked-target'
        Place-WindowOnMonitor -window $createdWindows[-1] -monitor $unlockedMonitor
        Move-WindowToDesktop -helper $helper -window $createdWindows[-1] -desktopNumber $alternateDesktop -label 'lg-bg-unlocked-target'

        Invoke-DesktopSwitch -helper $helper -targetDesktop $initialDesktop
        Start-Sleep -Seconds 2

        Toggle-MonitorLockFromTray -monitorLabel $lockedMonitor.Label -expectedLockState $true -sessionPath $sessionPath
        $lockStateAfterTrayLock = Read-LockState -path $sessionPath -monitorLabel $lockedMonitor.Label
        Start-Sleep -Seconds 2

        $switchStates = @()

        Invoke-DesktopSwitch -helper $helper -targetDesktop $alternateDesktop
        Wait-ForBackgroundReportCount -path $backgroundReportPath -expectedMinimum 1
        Start-Sleep -Seconds 1
        $switchStates += [pscustomobject]@{
            label = 'after-tray-lock'
            from = $initialDesktop
            to = $alternateDesktop
            desktops = Get-WindowDesktopMap -helper $helper -windows $createdWindows
        }

        Invoke-DesktopSwitch -helper $helper -targetDesktop $initialDesktop
        Wait-ForBackgroundReportCount -path $backgroundReportPath -expectedMinimum 2
        Start-Sleep -Seconds 1
        $switchStates += [pscustomobject]@{
            label = 'after-tray-lock-return'
            from = $alternateDesktop
            to = $initialDesktop
            desktops = Get-WindowDesktopMap -helper $helper -windows $createdWindows
        }

        Toggle-MonitorLockFromTray -monitorLabel $lockedMonitor.Label -expectedLockState $false -sessionPath $sessionPath
        $lockStateAfterTrayUnlock = Read-LockState -path $sessionPath -monitorLabel $lockedMonitor.Label
        Start-Sleep -Seconds 2

        Invoke-DesktopSwitch -helper $helper -targetDesktop $alternateDesktop
        Wait-ForBackgroundReportCount -path $backgroundReportPath -expectedMinimum 3
        Start-Sleep -Seconds 1
        $switchStates += [pscustomobject]@{
            label = 'after-tray-unlock'
            from = $initialDesktop
            to = $alternateDesktop
            desktops = Get-WindowDesktopMap -helper $helper -windows $createdWindows
        }

        $proofState = [pscustomobject]@{
            locked_monitor = $lockedMonitor.Label
            unlocked_monitor = $unlockedMonitor.Label
            initial_desktop = $initialDesktop
            alternate_desktop = $alternateDesktop
            switch_states = $switchStates
            lock_state_after_tray_lock = $lockStateAfterTrayLock
            lock_state_after_tray_unlock = $lockStateAfterTrayUnlock
            watcher_processes = $watcherEvidence
            background_pid = $watchProcess.Id
            background_executable = $watchExe
            background_working_directory = $WorkingDirectory
            background_stdout = $watchStdout
            background_stderr = $watchStderr
            background_desktop_reports = Get-BackgroundReportSummary -path $backgroundReportPath -windowTitles @(
                'lg-bg-locked-source',
                'lg-bg-unlocked-source',
                'lg-bg-locked-target',
                'lg-bg-unlocked-target'
            )
        }
        $proofState | ConvertTo-Json -Depth 6 | Set-Content -Path $stateJson -Encoding UTF8

        Write-Host ('proof state: ' + $stateJson)
        Write-Host ('background stdout: ' + $watchStdout)
        Write-Host ('background stderr: ' + $watchStderr)
        Write-Host ('background desktop reports: ' + $backgroundReportPath)
    }
    finally {
        if ($helper) {
            $helper.Dispose()
        }
    }
}
finally {
    if ($watchProcess -and -not $watchProcess.HasExited) {
        Stop-ProcessTree -parentProcessId $watchProcess.Id
        $watchProcess.Kill()
        $watchProcess.WaitForExit(5000)
    }

    Remove-Item Env:LOCKING_GLASS_SESSION_PATH -ErrorAction SilentlyContinue
    Remove-Item Env:LOCKING_GLASS_BACKGROUND_REPORT_PATH -ErrorAction SilentlyContinue

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
