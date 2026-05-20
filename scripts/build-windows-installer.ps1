param(
    [string]$StageDir,
    [string]$OutputDir,
    [string]$SetupExeName = 'Locking Glass Installer.exe',
    [string]$UninstallerExeName = 'Locking Glass Uninstaller.exe',
    [switch]$SkipStage
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$stageScript = Join-Path $PSScriptRoot 'stage-windows-install.ps1'
$bootstrapperProject = Join-Path $repoRoot 'tools\windows_installer_bootstrapper\LockingGlass.WindowsInstallerBootstrapper.csproj'

if ([string]::IsNullOrWhiteSpace($StageDir)) {
    $StageDir = Join-Path $repoRoot 'build\windows-install-stage\Locking Glass'
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
$installerPublishDir = Join-Path $OutputDir 'publish-installer'
$uninstallerPublishDir = Join-Path $OutputDir 'publish-uninstaller'
$targetExe = Join-Path $OutputDir $SetupExeName
$targetUninstallerExe = Join-Path $OutputDir $UninstallerExeName

Remove-Item -Recurse -Force $installerPublishDir, $uninstallerPublishDir -ErrorAction SilentlyContinue
Remove-Item -Force $payloadZip -ErrorAction SilentlyContinue
Remove-Item -Force $targetExe, $targetUninstallerExe -ErrorAction SilentlyContinue

# The payload is intentionally flat; the bootstrapper rejects nested or rooted
# entries before extraction.
Compress-Archive -Path $stagedFiles.FullName -DestinationPath $payloadZip -Force

function Publish-Bootstrapper([string]$Mode, [string]$PublishDir, [string]$TargetPath) {
    & $dotnet.Source publish $bootstrapperProject `
        -c Release `
        -f net8.0 `
        -r win-x64 `
        --self-contained true `
        -o $PublishDir `
        /p:PublishSingleFile=true `
        /p:EnableCompressionInSingleFile=true `
        /p:IncludeNativeLibrariesForSelfExtract=true `
        /p:PayloadZip=$payloadZip `
        /p:BootstrapperMode=$Mode

    if ($LASTEXITCODE -ne 0) {
        throw "dotnet publish failed for the Windows $Mode bootstrapper."
    }

    $publishedExe = Join-Path $PublishDir 'LockingGlass.WindowsInstallerBootstrapper.exe'
    if (-not (Test-Path $publishedExe)) {
        throw "Bootstrapper publish output did not create '$publishedExe'."
    }

    Copy-Item -Path $publishedExe -Destination $TargetPath -Force
}

Publish-Bootstrapper -Mode 'install' -PublishDir $installerPublishDir -TargetPath $targetExe
Publish-Bootstrapper -Mode 'uninstall' -PublishDir $uninstallerPublishDir -TargetPath $targetUninstallerExe

Write-Host ('Built Locking Glass installer: ' + $targetExe)
Write-Host ('Built Locking Glass uninstaller: ' + $targetUninstallerExe)
Write-Host ('Embedded payload zip: ' + $payloadZip)
