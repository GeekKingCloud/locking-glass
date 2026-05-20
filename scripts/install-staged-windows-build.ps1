param(
    [string]$SourceDir,
    [string]$InstallDir,
    # Retained for older setup wrappers; autostart is now enabled unless -NoAutostart is supplied.
    [switch]$EnableAutostart,
    [switch]$NoAutostart,
    [switch]$LaunchAfterInstall
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($SourceDir)) {
    $SourceDir = $PSScriptRoot
}

if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    $InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\Locking Glass'
}

function Resolve-InstallDirectory([string]$TargetInstallDir) {
    if ([string]::IsNullOrWhiteSpace($TargetInstallDir)) {
        throw 'InstallDir must not be empty.'
    }

    return [System.IO.Path]::GetFullPath($TargetInstallDir).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
}

function Test-SafeInstallDirectory([string]$TargetInstallDir) {
    $normalizedInstallDir = Resolve-InstallDirectory -TargetInstallDir $TargetInstallDir
    $root = [System.IO.Path]::GetPathRoot($normalizedInstallDir)
    if ($normalizedInstallDir -eq $root.TrimEnd(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar)) {
        throw "Refusing to install Locking Glass into a filesystem root: '$normalizedInstallDir'."
    }

    $leafName = Split-Path -Path $normalizedInstallDir -Leaf
    if ([string]::IsNullOrWhiteSpace($leafName)) {
        throw "InstallDir must name a concrete Locking Glass install directory: '$normalizedInstallDir'."
    }

    if ($leafName -ne 'Locking Glass') {
        throw "InstallDir must end in 'Locking Glass' so the installer cannot overwrite a broad shared directory: '$normalizedInstallDir'."
    }

    # The installer replaces files in place, so it must only ever target the
    # app-specific leaf directory. This prevents a bad argument from wiping a
    # profile, temp folder, or shared program directory.
    $blockedParentPaths = @(
        [Environment]::GetFolderPath('UserProfile'),
        $env:LOCALAPPDATA,
        $env:APPDATA,
        $env:ProgramFiles,
        ${env:ProgramFiles(x86)},
        $env:WINDIR,
        $env:TEMP,
        $env:TMP
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { Resolve-InstallDirectory -TargetInstallDir $_ }

    foreach ($blockedParentPath in $blockedParentPaths) {
        if ([string]::Equals(
                $normalizedInstallDir,
                $blockedParentPath,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "InstallDir must be an app-specific Locking Glass directory, not a shared parent directory: '$normalizedInstallDir'."
        }
    }

    return $normalizedInstallDir
}

function Stop-InstalledRuntimeProcesses([string]$TargetInstallDir) {
    $normalizedInstallDir = Resolve-InstallDirectory -TargetInstallDir $TargetInstallDir
    $expectedExecutablePath = Join-Path $normalizedInstallDir 'Locking Glass.exe'
    # Stop only the installed executable path, not every process with the same
    # image name. A user may be running a portable test build elsewhere.
    $processes = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            if ([string]::IsNullOrWhiteSpace($_.ExecutablePath)) {
                return $false
            }

            $candidatePath = [System.IO.Path]::GetFullPath($_.ExecutablePath)
            return [string]::Equals(
                $candidatePath,
                $expectedExecutablePath,
                [System.StringComparison]::OrdinalIgnoreCase)
        })

    foreach ($process in $processes | Sort-Object -Property ProcessId -Descending) {
        Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
    }

    if ($processes.Count -gt 0) {
        Start-Sleep -Milliseconds 750
    }
}

function Copy-InstallFile([string]$SourcePath, [string]$DestinationPath) {
    $maxAttempts = 5
    for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
        try {
            Copy-Item -Path $SourcePath -Destination $DestinationPath -Force
            return
        } catch {
            if ($attempt -eq $maxAttempts) {
                throw
            }

            Start-Sleep -Milliseconds (250 * $attempt)
        }
    }
}

function Get-VersionText([string]$Path) {
    if (-not (Test-Path $Path)) {
        return $null
    }

    return (Get-Content -Path $Path -Raw).Trim()
}

$InstallDir = Test-SafeInstallDirectory -TargetInstallDir $InstallDir

$requiredFiles = @(
    'Locking Glass.exe',
    'Install-LockingGlass.ps1',
    'Uninstall-LockingGlass.ps1',
    'run-live-desktop-probe.ps1',
    'resolve-virtual-desktop-helper.ps1',
    'VirtualDesktopAccessor.dll',
    'Start-LockingGlass.cmd',
    'README.txt',
    'VERSION.txt',
    'LICENSE.txt',
    'THIRD_PARTY_NOTICES.txt',
    'DOTNET_RUNTIME_LICENSE.txt',
    'DOTNET_RUNTIME_THIRD_PARTY_NOTICES.txt',
    'LOCKING_GLASS_PAYLOAD_MANIFEST.txt'
)

foreach ($requiredFile in $requiredFiles) {
    $candidate = Join-Path $SourceDir $requiredFile
    if (-not (Test-Path $candidate)) {
        throw "Staged install source is missing '$requiredFile' at '$candidate'."
    }
}

$bundledProbeFiles = Get-ChildItem -Path $SourceDir -File |
    Where-Object { $_.Name -like 'LockingGlass.WindowsLiveDesktopProbe*' }
if ($bundledProbeFiles.Count -eq 0) {
    throw 'Staged install source is missing the bundled Windows live desktop probe publish output.'
}

$installedExe = Join-Path $InstallDir 'Locking Glass.exe'
$installedVersionPath = Join-Path $InstallDir 'VERSION.txt'
$incomingVersionPath = Join-Path $SourceDir 'VERSION.txt'
$existingVersion = Get-VersionText -Path $installedVersionPath
$incomingVersion = Get-VersionText -Path $incomingVersionPath
$wasInstalled = (Test-Path $installedExe) -or (Test-Path $installedVersionPath)
Stop-InstalledRuntimeProcesses -TargetInstallDir $InstallDir

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

$filesToCopy = @(
    'Locking Glass.exe',
    'run-live-desktop-probe.ps1',
    'resolve-virtual-desktop-helper.ps1',
    'VirtualDesktopAccessor.dll',
    'Start-LockingGlass.cmd',
    'README.txt',
    'VERSION.txt',
    'LICENSE.txt',
    'THIRD_PARTY_NOTICES.txt',
    'DOTNET_RUNTIME_LICENSE.txt',
    'DOTNET_RUNTIME_THIRD_PARTY_NOTICES.txt',
    'LOCKING_GLASS_PAYLOAD_MANIFEST.txt',
    'Install-LockingGlass.ps1',
    'Uninstall-LockingGlass.ps1'
)

foreach ($fileName in $filesToCopy) {
    $sourcePath = Join-Path $SourceDir $fileName
    if (Test-Path $sourcePath) {
        Copy-InstallFile -SourcePath $sourcePath -DestinationPath (Join-Path $InstallDir $fileName)
    }
}

foreach ($probeFile in $bundledProbeFiles) {
    Copy-InstallFile -SourcePath $probeFile.FullName -DestinationPath (Join-Path $InstallDir $probeFile.Name)
}

$startMenuDir = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Locking Glass'
New-Item -ItemType Directory -Force -Path $startMenuDir | Out-Null

$shell = New-Object -ComObject WScript.Shell
$launchShortcut = $shell.CreateShortcut((Join-Path $startMenuDir 'Locking Glass Tray.lnk'))
$launchShortcut.TargetPath = Join-Path $InstallDir 'Start-LockingGlass.cmd'
$launchShortcut.WorkingDirectory = $InstallDir
$launchShortcut.IconLocation = (Join-Path $InstallDir 'Locking Glass.exe') + ',0'
$launchShortcut.Description = 'Start the Locking Glass tray app that keeps selected monitors pinned while other monitors follow Windows desktop switches. It fails closed if the live controller cannot start.'
$launchShortcut.Save()

$readmeShortcut = $shell.CreateShortcut((Join-Path $startMenuDir 'Locking Glass README.lnk'))
$readmeShortcut.TargetPath = Join-Path $InstallDir 'README.txt'
$readmeShortcut.WorkingDirectory = $InstallDir
$readmeShortcut.Description = 'Open the Locking Glass Windows install notes and current limits.'
$readmeShortcut.Save()

$licenseShortcut = $shell.CreateShortcut((Join-Path $startMenuDir 'Locking Glass License.lnk'))
$licenseShortcut.TargetPath = Join-Path $InstallDir 'LICENSE.txt'
$licenseShortcut.WorkingDirectory = $InstallDir
$licenseShortcut.Description = 'Open the Locking Glass project license.'
$licenseShortcut.Save()

$thirdPartyShortcut = $shell.CreateShortcut((Join-Path $startMenuDir 'Locking Glass Third-Party Notices.lnk'))
$thirdPartyShortcut.TargetPath = Join-Path $InstallDir 'THIRD_PARTY_NOTICES.txt'
$thirdPartyShortcut.WorkingDirectory = $InstallDir
$thirdPartyShortcut.Description = 'Open bundled third-party license notices for Locking Glass.'
$thirdPartyShortcut.Save()

$uninstallShortcut = $shell.CreateShortcut((Join-Path $startMenuDir 'Uninstall Locking Glass.lnk'))
$uninstallShortcut.TargetPath = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$uninstallShortcut.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $InstallDir 'Uninstall-LockingGlass.ps1') + '"'
$uninstallShortcut.WorkingDirectory = $InstallDir
$uninstallShortcut.IconLocation = (Join-Path $InstallDir 'Locking Glass.exe') + ',0'
$uninstallShortcut.Description = 'Uninstall Locking Glass for the current user.'
$uninstallShortcut.Save()

if (-not $NoAutostart) {
    # Delegate Run-key formatting to the app so installer and runtime quoting
    # stay in one place, especially now that the installed path contains spaces.
    & $installedExe --install-autostart
    if ($LASTEXITCODE -ne 0) {
        throw "Locking Glass autostart registration failed from '$installedExe'."
    }
}

if ($LaunchAfterInstall) {
    Start-Process -FilePath $installedExe -ArgumentList '--background' -WorkingDirectory $InstallDir -WindowStyle Hidden | Out-Null
}

$installVerb = if ($wasInstalled) { 'Updated' } else { 'Installed' }
Write-Host ($installVerb + ' Locking Glass at: ' + $InstallDir)
if (-not [string]::IsNullOrWhiteSpace($incomingVersion)) {
    if ($wasInstalled -and -not [string]::IsNullOrWhiteSpace($existingVersion) -and $existingVersion -ne $incomingVersion) {
        Write-Host ('Version: ' + $existingVersion + ' -> ' + $incomingVersion)
    } elseif ($wasInstalled -and -not [string]::IsNullOrWhiteSpace($existingVersion)) {
        Write-Host ('Version: refreshed ' + $incomingVersion)
    } else {
        Write-Host ('Version: ' + $incomingVersion)
    }
}
Write-Host ('Launch shortcut: ' + (Join-Path $startMenuDir 'Locking Glass Tray.lnk'))
Write-Host ('README shortcut: ' + (Join-Path $startMenuDir 'Locking Glass README.lnk'))
Write-Host ('License shortcut: ' + (Join-Path $startMenuDir 'Locking Glass License.lnk'))
Write-Host ('Third-party notices shortcut: ' + (Join-Path $startMenuDir 'Locking Glass Third-Party Notices.lnk'))
Write-Host ('Uninstall shortcut: ' + (Join-Path $startMenuDir 'Uninstall Locking Glass.lnk'))
Write-Host 'Updates: rerun a newer Locking Glass installer executable or Install-LockingGlass.ps1 over the existing install'
if (-not $NoAutostart) {
    Write-Host 'Autostart: enabled for the current user'
} else {
    Write-Host 'Autostart: not changed'
}
if ($LaunchAfterInstall) {
    Write-Host 'Launch: started the installed background tray app'
} else {
    Write-Host 'Launch: not started by the installer script'
}
