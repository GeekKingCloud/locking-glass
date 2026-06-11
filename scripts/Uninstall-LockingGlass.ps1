param(
    [string]$InstallDir,
    [switch]$RemoveUserData,
    [switch]$Quiet
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

    # Match the installer guard so uninstall cannot recursively delete a
    # profile, temp folder, or shared program directory from a bad argument.
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
    $installedPathPrefix = $normalizedInstallDir + [System.IO.Path]::DirectorySeparatorChar
    # Match the installed directory so uninstall does not kill a portable copy
    # the user launched for testing, while still clearing bundled helper
    # processes that can outlive the tray process.
    $processes = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            if ([string]::IsNullOrWhiteSpace($_.ExecutablePath)) {
                return $false
            }

            $candidatePath = [System.IO.Path]::GetFullPath($_.ExecutablePath)
            return ([string]::Equals(
                $candidatePath,
                $expectedExecutablePath,
                [System.StringComparison]::OrdinalIgnoreCase) -or
                $candidatePath.StartsWith(
                    $installedPathPrefix,
                    [System.StringComparison]::OrdinalIgnoreCase))
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
        Remove-ItemProperty -Path $runKey -Name 'Locking Glass.exe' -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $runKey -Name 'LockingGlass' -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $runKey -Name 'LockingGlass.exe' -ErrorAction SilentlyContinue
    }

    $startupApprovedRunKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run'
    if (Test-Path $startupApprovedRunKey) {
        Remove-ItemProperty -Path $startupApprovedRunKey -Name 'Locking Glass' -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $startupApprovedRunKey -Name 'Locking Glass.exe' -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $startupApprovedRunKey -Name 'LockingGlass' -ErrorAction SilentlyContinue
        Remove-ItemProperty -Path $startupApprovedRunKey -Name 'LockingGlass.exe' -ErrorAction SilentlyContinue
    }
}

function Get-UninstallRegistryKeyName {
    $override = $env:LOCKING_GLASS_UNINSTALL_REGISTRY_KEY_NAME
    if ([string]::IsNullOrWhiteSpace($override)) {
        return 'Locking Glass'
    }

    if ($override.IndexOfAny([char[]]@('\', '/')) -ge 0) {
        throw "Uninstall registry key name must not contain a path separator: '$override'."
    }

    return $override
}

function Remove-CurrentUserUninstallEntry {
    $registryPath = Join-Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall' (Get-UninstallRegistryKeyName)
    if (Test-Path $registryPath) {
        Remove-Item -Recurse -Force -Path $registryPath
    }
}

function Remove-StartMenuShortcuts {
    $startMenuDir = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Locking Glass'
    if (Test-Path $startMenuDir) {
        Remove-Item -Recurse -Force -Path $startMenuDir
    }

    $legacyStartMenuDir = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\LockingGlass'
    if (Test-Path $legacyStartMenuDir) {
        Remove-Item -Recurse -Force -Path $legacyStartMenuDir
    }
}

function Remove-InstallDirectory([string]$TargetInstallDir) {
    if (Test-Path $TargetInstallDir) {
        $manifestPath = Join-Path $TargetInstallDir 'LOCKING_GLASS_PAYLOAD_MANIFEST.txt'
        $executablePath = Join-Path $TargetInstallDir 'Locking Glass.exe'
        if (-not (Test-Path $manifestPath) -or -not (Test-Path $executablePath)) {
            throw "Refusing to remove '$TargetInstallDir' because it does not look like a Locking Glass install payload."
        }

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
Remove-CurrentUserUninstallEntry
Remove-StartMenuShortcuts
Remove-InstallDirectory -TargetInstallDir $InstallDir

# Session state is intentionally preserved by default so uninstall/reinstall is
# not a destructive troubleshooting step.
if ($RemoveUserData) {
    Remove-LockingGlassUserData
}

Write-Host ('Uninstalled Locking Glass from: ' + $InstallDir)
if ($RemoveUserData) {
    Write-Host 'User data: removed'
} else {
    Write-Host 'User data: preserved'
}
