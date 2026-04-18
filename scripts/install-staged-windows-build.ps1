param(
    [string]$SourceDir,
    [string]$InstallDir,
    [switch]$EnableAutostart,
    [switch]$LaunchAfterInstall
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($SourceDir)) {
    $SourceDir = $PSScriptRoot
}

if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    $InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\LockingGlass'
}

function Stop-InstalledRuntimeProcesses([string]$TargetInstallDir) {
    $normalizedInstallDir = [System.IO.Path]::GetFullPath($TargetInstallDir).TrimEnd('\')
    $processes = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            if ([string]::IsNullOrWhiteSpace($_.ExecutablePath)) {
                return $false
            }

            $candidatePath = [System.IO.Path]::GetFullPath($_.ExecutablePath)
            return $candidatePath -like ($normalizedInstallDir + '\*')
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

$requiredFiles = @(
    'LockingGlass.exe',
    'Install-LockingGlass.ps1',
    'run-live-desktop-probe.ps1',
    'resolve-virtual-desktop-helper.ps1',
    'VirtualDesktopAccessor.dll',
    'Start-LockingGlass.cmd',
    'README.txt',
    'VERSION.txt',
    'LICENSE.txt',
    'THIRD_PARTY_NOTICES.txt'
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

$installedExe = Join-Path $InstallDir 'LockingGlass.exe'
$installedVersionPath = Join-Path $InstallDir 'VERSION.txt'
$incomingVersionPath = Join-Path $SourceDir 'VERSION.txt'
$existingVersion = Get-VersionText -Path $installedVersionPath
$incomingVersion = Get-VersionText -Path $incomingVersionPath
$wasInstalled = (Test-Path $installedExe) -or (Test-Path $installedVersionPath)
Stop-InstalledRuntimeProcesses -TargetInstallDir $InstallDir

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

$filesToCopy = @(
    'LockingGlass.exe',
    'run-live-desktop-probe.ps1',
    'resolve-virtual-desktop-helper.ps1',
    'VirtualDesktopAccessor.dll',
    'Start-LockingGlass.cmd',
    'README.txt',
    'VERSION.txt',
    'LICENSE.txt',
    'THIRD_PARTY_NOTICES.txt',
    'Install-LockingGlass.ps1'
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

$startMenuDir = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\LockingGlass'
New-Item -ItemType Directory -Force -Path $startMenuDir | Out-Null

$shell = New-Object -ComObject WScript.Shell
$launchShortcut = $shell.CreateShortcut((Join-Path $startMenuDir 'LockingGlass Tray.lnk'))
$launchShortcut.TargetPath = Join-Path $InstallDir 'Start-LockingGlass.cmd'
$launchShortcut.WorkingDirectory = $InstallDir
$launchShortcut.IconLocation = (Join-Path $InstallDir 'LockingGlass.exe') + ',0'
$launchShortcut.Description = 'Start the LockingGlass tray app that keeps selected monitors pinned while other monitors follow Windows desktop switches. It fails closed if the live controller cannot start.'
$launchShortcut.Save()

$readmeShortcut = $shell.CreateShortcut((Join-Path $startMenuDir 'LockingGlass README.lnk'))
$readmeShortcut.TargetPath = Join-Path $InstallDir 'README.txt'
$readmeShortcut.WorkingDirectory = $InstallDir
$readmeShortcut.Description = 'Open the LockingGlass Windows install notes and current limits.'
$readmeShortcut.Save()

$licenseShortcut = $shell.CreateShortcut((Join-Path $startMenuDir 'LockingGlass License.lnk'))
$licenseShortcut.TargetPath = Join-Path $InstallDir 'LICENSE.txt'
$licenseShortcut.WorkingDirectory = $InstallDir
$licenseShortcut.Description = 'Open the LockingGlass project license.'
$licenseShortcut.Save()

$thirdPartyShortcut = $shell.CreateShortcut((Join-Path $startMenuDir 'LockingGlass Third-Party Notices.lnk'))
$thirdPartyShortcut.TargetPath = Join-Path $InstallDir 'THIRD_PARTY_NOTICES.txt'
$thirdPartyShortcut.WorkingDirectory = $InstallDir
$thirdPartyShortcut.Description = 'Open bundled third-party license notices for LockingGlass.'
$thirdPartyShortcut.Save()

if ($EnableAutostart) {
    & $installedExe --install-autostart
    if ($LASTEXITCODE -ne 0) {
        throw "LockingGlass autostart registration failed from '$installedExe'."
    }
}

if ($LaunchAfterInstall) {
    Start-Process -FilePath $installedExe -ArgumentList '--background' -WorkingDirectory $InstallDir -WindowStyle Hidden | Out-Null
}

$installVerb = if ($wasInstalled) { 'Updated' } else { 'Installed' }
Write-Host ($installVerb + ' LockingGlass at: ' + $InstallDir)
if (-not [string]::IsNullOrWhiteSpace($incomingVersion)) {
    if ($wasInstalled -and -not [string]::IsNullOrWhiteSpace($existingVersion) -and $existingVersion -ne $incomingVersion) {
        Write-Host ('Version: ' + $existingVersion + ' -> ' + $incomingVersion)
    } elseif ($wasInstalled -and -not [string]::IsNullOrWhiteSpace($existingVersion)) {
        Write-Host ('Version: refreshed ' + $incomingVersion)
    } else {
        Write-Host ('Version: ' + $incomingVersion)
    }
}
Write-Host ('Launch shortcut: ' + (Join-Path $startMenuDir 'LockingGlass Tray.lnk'))
Write-Host ('README shortcut: ' + (Join-Path $startMenuDir 'LockingGlass README.lnk'))
Write-Host ('License shortcut: ' + (Join-Path $startMenuDir 'LockingGlass License.lnk'))
Write-Host ('Third-party notices shortcut: ' + (Join-Path $startMenuDir 'LockingGlass Third-Party Notices.lnk'))
Write-Host 'Updates: rerun a newer LockingGlass setup executable or Install-LockingGlass.ps1 over the existing install'
if ($EnableAutostart) {
    Write-Host 'Autostart: enabled for the current user'
} else {
    Write-Host 'Autostart: not changed'
}
if ($LaunchAfterInstall) {
    Write-Host 'Launch: started the installed background tray app'
} else {
    Write-Host 'Launch: not started by the installer script'
}
