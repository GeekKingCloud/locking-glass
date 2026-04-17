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

$requiredFiles = @(
    'LockingGlass.exe',
    'run-live-desktop-probe.ps1',
    'VirtualDesktopAccessor.dll',
    'Start-LockingGlass.cmd',
    'README.txt'
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
Stop-InstalledRuntimeProcesses -TargetInstallDir $InstallDir

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

$filesToCopy = @(
    'LockingGlass.exe',
    'run-live-desktop-probe.ps1',
    'VirtualDesktopAccessor.dll',
    'Start-LockingGlass.cmd',
    'README.txt',
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

if ($EnableAutostart) {
    & $installedExe --install-autostart
    if ($LASTEXITCODE -ne 0) {
        throw "LockingGlass autostart registration failed from '$installedExe'."
    }
}

if ($LaunchAfterInstall) {
    Start-Process -FilePath $installedExe -ArgumentList '--background' -WorkingDirectory $InstallDir -WindowStyle Hidden | Out-Null
}

Write-Host ('Installed LockingGlass to: ' + $InstallDir)
Write-Host ('Launch shortcut: ' + (Join-Path $startMenuDir 'LockingGlass Tray.lnk'))
Write-Host ('README shortcut: ' + (Join-Path $startMenuDir 'LockingGlass README.lnk'))
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
