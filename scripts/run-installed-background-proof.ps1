param(
    [string]$InstallDir,
    [string]$ProofDir
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$stageDir = Join-Path $repoRoot 'build\windows-install-stage\LockingGlass'

if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    $InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\LockingGlassInstalledProof'
}

if ([string]::IsNullOrWhiteSpace($ProofDir)) {
    $ProofDir = Join-Path $repoRoot 'build\windows-installed-background-proof'
}

& (Join-Path $repoRoot 'scripts\stage-windows-install.ps1') -OutputDir $stageDir
if ($LASTEXITCODE -ne 0) {
    throw 'Failed to stage the Windows install package.'
}

& (Join-Path $stageDir 'Install-LockingGlass.ps1') -InstallDir $InstallDir
if ($LASTEXITCODE -ne 0) {
    throw 'Failed to install the staged Windows package.'
}

$installedExe = Join-Path $InstallDir 'LockingGlass.exe'
$installedHelper = Join-Path $InstallDir 'VirtualDesktopAccessor.dll'
$installedWatchScript = Join-Path $InstallDir 'run-live-desktop-probe.ps1'
$installedProbe = Join-Path $InstallDir 'LockingGlass.WindowsLiveDesktopProbe.exe'

& (Join-Path $repoRoot 'scripts\run-live-background-proof.ps1') `
    -WatchExe $installedExe `
    -WorkingDirectory $InstallDir `
    -HelperDllPath $installedHelper `
    -ProofDir $ProofDir
if ($LASTEXITCODE -ne 0) {
    throw 'Installed background proof failed.'
}

$stateJson = Join-Path $ProofDir 'proof-state.json'
if (Test-Path $stateJson) {
    $proofState = Get-Content $stateJson -Raw | ConvertFrom-Json
    $proofState | Add-Member -NotePropertyName install_dir -NotePropertyValue $InstallDir -Force
    $proofState | Add-Member -NotePropertyName staged_package_dir -NotePropertyValue $stageDir -Force
    $proofState | Add-Member -NotePropertyName installed_watch_script -NotePropertyValue $installedWatchScript -Force
    $proofState | Add-Member -NotePropertyName installed_probe_executable -NotePropertyValue $installedProbe -Force
    $proofState | Add-Member -NotePropertyName installed_helper_dll -NotePropertyValue $installedHelper -Force
    $proofState | ConvertTo-Json -Depth 8 | Set-Content -Path $stateJson -Encoding UTF8
}

Write-Host ('installed app: ' + $installedExe)
Write-Host ('installed live watch script: ' + $installedWatchScript)
Write-Host ('installed proof state: ' + $stateJson)
