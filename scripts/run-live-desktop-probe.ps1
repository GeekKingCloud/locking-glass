param(
    [string]$HelperDllPath,
    [string]$LogPath,
    [string]$ProbeExecutablePath,
    [int]$TimeoutSeconds = 20,
    [int]$RequiredEvents = 2,
    [switch]$WatchStream,
    [switch]$NoAutoCycle,
    [switch]$SkipMoveExercise
)

$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSCommandPath
$repoRoot = Split-Path -Parent $scriptRoot
$probeProject = Join-Path $repoRoot 'tools\windows_live_desktop_probe\LockingGlass.WindowsLiveDesktopProbe.csproj'
$probeBuildDir = Join-Path $repoRoot 'build\windows-live-desktop-probe'
$helperResolverPath = Join-Path $scriptRoot 'resolve-virtual-desktop-helper.ps1'
$probeFramework = 'net8.0'

. $helperResolverPath

function Require-Command([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "Required command '$Name' was not found on PATH."
    }

    return $command.Source
}

function Require-DotNetCompatibleSdk {
    $dotnet = Require-Command 'dotnet'
    $sdks = @(& $dotnet --list-sdks)
    $compatibleSdk = $sdks | Where-Object {
        $_ -match '^(\d+)\.' -and [int]$Matches[1] -ge 8
    } | Select-Object -First 1
    if ($null -eq $compatibleSdk) {
        throw ".NET SDK 8 or newer is required. Installed SDKs:`n$($sdks -join [Environment]::NewLine)"
    }

    return $dotnet
}

function Invoke-DotNet {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DotNetPath,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $previousDotNetRoot = $env:DOTNET_ROOT
    $previousMSBuildSDKsPath = $env:MSBuildSDKsPath
    try {
        $env:DOTNET_ROOT = Split-Path -Parent $DotNetPath
        Remove-Item Env:MSBuildSDKsPath -ErrorAction SilentlyContinue
        & $DotNetPath @Arguments
        $script:LockingGlassLastDotNetExitCode = $LASTEXITCODE
    } finally {
        if ($null -eq $previousDotNetRoot) {
            Remove-Item Env:DOTNET_ROOT -ErrorAction SilentlyContinue
        } else {
            $env:DOTNET_ROOT = $previousDotNetRoot
        }
        if ($null -eq $previousMSBuildSDKsPath) {
            Remove-Item Env:MSBuildSDKsPath -ErrorAction SilentlyContinue
        } else {
            $env:MSBuildSDKsPath = $previousMSBuildSDKsPath
        }
    }
}

$usesRepoProject = Test-Path $probeProject
$artifactRoot = if ($usesRepoProject) { $probeBuildDir } else { $scriptRoot }
New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

if ([string]::IsNullOrWhiteSpace($HelperDllPath)) {
    $HelperDllPath = Join-Path $artifactRoot 'VirtualDesktopAccessor.dll'
}

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $logPrefix = if ($WatchStream) { 'live-desktop-watch-' } else { 'live-desktop-proof-' }
    $LogPath = Join-Path $artifactRoot ($logPrefix + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
}

$HelperDllPath = Resolve-LockingGlassVirtualDesktopHelper -HelperDllPath $HelperDllPath

if ([string]::IsNullOrWhiteSpace($ProbeExecutablePath)) {
    $candidateProbePaths = @(
        (Join-Path $scriptRoot 'LockingGlass.WindowsLiveDesktopProbe.exe'),
        (Join-Path $scriptRoot 'LockingGlass.WindowsLiveDesktopProbe.dll'),
        (Join-Path $probeBuildDir 'publish\LockingGlass.WindowsLiveDesktopProbe.exe'),
        (Join-Path $probeBuildDir 'publish\LockingGlass.WindowsLiveDesktopProbe.dll')
    )
    $ProbeExecutablePath = $candidateProbePaths |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
}

$probeArguments = @(
    '--helper-dll', $HelperDllPath,
    '--log', $LogPath
)

if ($RequiredEvents -ge 0) {
    $probeArguments += '--required-events'
    $probeArguments += $RequiredEvents
}

if ($TimeoutSeconds -ge 0) {
    $probeArguments += '--timeout-seconds'
    $probeArguments += $TimeoutSeconds
}

if ($WatchStream) {
    $probeArguments += '--watch-stream'
}

if (-not $NoAutoCycle) {
    $probeArguments += '--auto-cycle'
}

if (-not $SkipMoveExercise) {
    $probeArguments += '--exercise-move'
}

if (-not [string]::IsNullOrWhiteSpace($ProbeExecutablePath)) {
    if ($ProbeExecutablePath.EndsWith('.exe', [System.StringComparison]::OrdinalIgnoreCase)) {
        & $ProbeExecutablePath @probeArguments
        $exitCode = $LASTEXITCODE
        Write-Host "Locking Glass live desktop probe log: $LogPath"
        exit $exitCode
    }

    if ($ProbeExecutablePath.EndsWith('.dll', [System.StringComparison]::OrdinalIgnoreCase)) {
        $dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
        if ($null -eq $dotnet) {
            throw "dotnet.exe is required to launch the bundled probe DLL at '$ProbeExecutablePath'."
        }

        Invoke-DotNet -DotNetPath $dotnet.Source -Arguments (
            @($ProbeExecutablePath) + $probeArguments)
        $exitCode = $script:LockingGlassLastDotNetExitCode
        Write-Host "Locking Glass live desktop probe log: $LogPath"
        exit $exitCode
    }
}

if (-not $usesRepoProject) {
    throw 'Locking Glass could not find a bundled Windows live desktop probe executable beside the script, and this path is not running from a repo checkout with the source project available.'
}

$dotnet = Require-DotNetCompatibleSdk

$arguments = @(
    'run',
    '--project', $probeProject,
    '--configuration', 'Release',
    '--framework', $probeFramework,
    '--'
) + $probeArguments

Invoke-DotNet -DotNetPath $dotnet -Arguments $arguments
$exitCode = $script:LockingGlassLastDotNetExitCode
Write-Host "Locking Glass live desktop probe log: $LogPath"
exit $exitCode
