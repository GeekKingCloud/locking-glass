param(
    [string]$StageDir,
    [string]$OutputDir,
    [string]$SetupExeName = 'LockingGlass-setup-x64.exe',
    [switch]$SkipStage
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$stageScript = Join-Path $PSScriptRoot 'stage-windows-install.ps1'
$bootstrapperProject = Join-Path $repoRoot 'tools\windows_installer_bootstrapper\LockingGlass.WindowsInstallerBootstrapper.csproj'

if ([string]::IsNullOrWhiteSpace($StageDir)) {
    $StageDir = Join-Path $repoRoot 'build\windows-install-stage\LockingGlass'
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repoRoot 'build\windows-installer'
}

$StageDir = [System.IO.Path]::GetFullPath($StageDir)
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)

if (-not (Test-Path $bootstrapperProject)) {
    throw "Missing Windows installer bootstrapper project at '$bootstrapperProject'."
}

if (-not $SkipStage) {
    & $stageScript -OutputDir $StageDir
    if ($LASTEXITCODE -ne 0) {
        throw 'stage-windows-install.ps1 failed while preparing the installer payload.'
    }
}

if (-not (Test-Path $StageDir)) {
    throw "Missing staged package directory at '$StageDir'."
}

$stagedFiles = @(Get-ChildItem -Path $StageDir -File)
if ($stagedFiles.Count -eq 0) {
    throw "No staged files were found under '$StageDir'."
}

$dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
if ($null -eq $dotnet) {
    throw 'dotnet.exe is required to publish the Windows installer bootstrapper.'
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$payloadZip = Join-Path $OutputDir 'LockingGlass-stage-payload.zip'
$publishDir = Join-Path $OutputDir 'publish'
$targetExe = Join-Path $OutputDir $SetupExeName

Remove-Item -Recurse -Force $publishDir -ErrorAction SilentlyContinue
Remove-Item -Force $payloadZip -ErrorAction SilentlyContinue
Remove-Item -Force $targetExe -ErrorAction SilentlyContinue

Compress-Archive -Path (Join-Path $StageDir '*') -DestinationPath $payloadZip -Force

& $dotnet.Source publish $bootstrapperProject `
    -c Release `
    -f netcoreapp3.1 `
    -r win-x64 `
    --self-contained true `
    -o $publishDir `
    /p:PublishSingleFile=true `
    /p:EnableCompressionInSingleFile=true `
    /p:IncludeNativeLibrariesForSelfExtract=true `
    /p:PayloadZip=$payloadZip

if ($LASTEXITCODE -ne 0) {
    throw 'dotnet publish failed for the Windows installer bootstrapper.'
}

$publishedExe = Join-Path $publishDir 'LockingGlass.WindowsInstallerBootstrapper.exe'
if (-not (Test-Path $publishedExe)) {
    throw "Bootstrapper publish output did not create '$publishedExe'."
}

Copy-Item -Path $publishedExe -Destination $targetExe -Force

Write-Host ('Built LockingGlass installer: ' + $targetExe)
Write-Host ('Embedded payload zip: ' + $payloadZip)
