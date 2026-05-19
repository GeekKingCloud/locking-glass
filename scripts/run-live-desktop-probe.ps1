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
        Write-Host "LockingGlass live desktop probe log: $LogPath"
        exit $exitCode
    }

    if ($ProbeExecutablePath.EndsWith('.dll', [System.StringComparison]::OrdinalIgnoreCase)) {
        $dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
        if ($null -eq $dotnet) {
            throw "dotnet.exe is required to launch the bundled probe DLL at '$ProbeExecutablePath'."
        }

        & $dotnet.Source $ProbeExecutablePath @probeArguments
        $exitCode = $LASTEXITCODE
        Write-Host "LockingGlass live desktop probe log: $LogPath"
        exit $exitCode
    }
}

if (-not $usesRepoProject) {
    throw 'LockingGlass could not find a bundled Windows live desktop probe executable beside the script, and this path is not running from a repo checkout with the source project available.'
}

$dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
if ($null -eq $dotnet) {
    throw 'dotnet.exe is required to build and run the Windows live desktop probe from source.'
}

$arguments = @(
    'run',
    '--project', $probeProject,
    '--configuration', 'Release',
    '--framework', $probeFramework,
    '--'
) + $probeArguments

& $dotnet.Source @arguments
$exitCode = $LASTEXITCODE
Write-Host "LockingGlass live desktop probe log: $LogPath"
exit $exitCode
