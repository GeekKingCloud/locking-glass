param(
    [string]$HelperDllPath,
    [string]$LogPath,
    [int]$TimeoutSeconds = 20,
    [int]$RequiredEvents = 2,
    [switch]$WatchStream,
    [switch]$NoAutoCycle,
    [switch]$SkipMoveExercise
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$probeProject = Join-Path $repoRoot 'tools\windows_live_desktop_probe\LockingGlass.WindowsLiveDesktopProbe.csproj'
$probeBuildDir = Join-Path $repoRoot 'build\windows-live-desktop-probe'
$helperReleaseUrl = 'https://github.com/Ciantic/VirtualDesktopAccessor/releases/download/2024-12-16-windows11/VirtualDesktopAccessor.dll'

New-Item -ItemType Directory -Force -Path $probeBuildDir | Out-Null

if ([string]::IsNullOrWhiteSpace($HelperDllPath)) {
    $HelperDllPath = Join-Path $probeBuildDir 'VirtualDesktopAccessor.dll'
}

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $logPrefix = if ($WatchStream) { 'live-desktop-watch-' } else { 'live-desktop-proof-' }
    $LogPath = Join-Path $probeBuildDir ($logPrefix + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
}

if (-not (Test-Path $HelperDllPath)) {
    Write-Host "Downloading VirtualDesktopAccessor.dll from $helperReleaseUrl"
    Invoke-WebRequest -Uri $helperReleaseUrl -OutFile $HelperDllPath
}

$dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
if ($null -eq $dotnet) {
    throw 'dotnet.exe is required to run the Windows live desktop probe.'
}

$arguments = @(
    'run',
    '--project', $probeProject,
    '--configuration', 'Release',
    '--framework', 'netcoreapp3.1',
    '--',
    '--helper-dll', $HelperDllPath,
    '--log', $LogPath
)

if ($RequiredEvents -ge 0) {
    $arguments += '--required-events'
    $arguments += $RequiredEvents
}

if ($TimeoutSeconds -ge 0) {
    $arguments += '--timeout-seconds'
    $arguments += $TimeoutSeconds
}

if ($WatchStream) {
    $arguments += '--watch-stream'
}

if (-not $NoAutoCycle) {
    $arguments += '--auto-cycle'
}

if (-not $SkipMoveExercise) {
    $arguments += '--exercise-move'
}

& $dotnet.Source @arguments
$exitCode = $LASTEXITCODE
Write-Host "LockingGlass live desktop probe log: $LogPath"
exit $exitCode
