param(
    [string]$InstallDir,
    [switch]$RemoveUserData
)

$ErrorActionPreference = 'Stop'

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
        throw "Refusing to uninstall from a filesystem root: '$normalizedInstallDir'."
    }

    $leafName = Split-Path -Path $normalizedInstallDir -Leaf
    if ($leafName -ne 'Locking Glass') {
        throw "InstallDir must end in 'Locking Glass' so uninstall cannot remove a broad shared directory: '$normalizedInstallDir'."
    }

    return $normalizedInstallDir
}

function Stop-InstalledRuntimeProcesses([string]$TargetInstallDir) {
    $normalizedInstallDir = Resolve-InstallDirectory -TargetInstallDir $TargetInstallDir
    $expectedExecutablePath = Join-Path $normalizedInstallDir 'Locking Glass.exe'
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

function Remove-CurrentUserAutostart {
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    if (Test-Path $runKey) {
        Remove-ItemProperty -Path $runKey -Name 'Locking Glass' -ErrorAction SilentlyContinue
    }
}

function Remove-StartMenuShortcuts {
    $startMenuDir = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Locking Glass'
    if (Test-Path $startMenuDir) {
        Remove-Item -Recurse -Force -Path $startMenuDir
    }
}

function Remove-InstallDirectory([string]$TargetInstallDir) {
    if (Test-Path $TargetInstallDir) {
        Remove-Item -Recurse -Force -Path $TargetInstallDir
    }
}

function Remove-LockingGlassUserData {
    $dataDir = Join-Path $env:LOCALAPPDATA 'Locking Glass'
    if (Test-Path $dataDir) {
        Remove-Item -Recurse -Force -Path $dataDir
    }
}

$InstallDir = Test-SafeInstallDirectory -TargetInstallDir $InstallDir

Stop-InstalledRuntimeProcesses -TargetInstallDir $InstallDir
Remove-CurrentUserAutostart
Remove-StartMenuShortcuts
Remove-InstallDirectory -TargetInstallDir $InstallDir

if ($RemoveUserData) {
    Remove-LockingGlassUserData
}

Write-Host ('Uninstalled Locking Glass from: ' + $InstallDir)
if ($RemoveUserData) {
    Write-Host 'User data: removed'
} else {
    Write-Host 'User data: preserved'
}
