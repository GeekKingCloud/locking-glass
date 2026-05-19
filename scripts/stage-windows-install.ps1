param(
    [string]$OutputDir,
    [string]$HelperDllPath
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$windowsBuild = Join-Path $repoRoot 'build-win\bin\locking_glass.exe'
$probeProject = Join-Path $repoRoot 'tools\windows_live_desktop_probe\LockingGlass.WindowsLiveDesktopProbe.csproj'
$probePublishDir = Join-Path $repoRoot 'build\windows-live-desktop-probe\publish'
$readmeSource = Join-Path $repoRoot 'docs\windows-install-package-readme.txt'
$versionSource = Join-Path $repoRoot 'VERSION'
$licenseSource = Join-Path $repoRoot 'LICENSE'
$thirdPartySource = Join-Path $repoRoot 'THIRD_PARTY_NOTICES.md'
$installerSource = Join-Path $repoRoot 'scripts\install-staged-windows-build.ps1'
$launcherSource = Join-Path $repoRoot 'scripts\Start-LockingGlass.cmd'
$probeScriptSource = Join-Path $repoRoot 'scripts\run-live-desktop-probe.ps1'
$helperResolverSource = Join-Path $repoRoot 'scripts\resolve-virtual-desktop-helper.ps1'

. $helperResolverSource

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repoRoot 'build\windows-install-stage\LockingGlass'
}

if (-not (Test-Path $windowsBuild)) {
    throw "Missing Windows build at '$windowsBuild'. Build the proven Windows binary before staging the install package."
}

if (-not (Test-Path $probeProject)) {
    throw "Missing Windows live desktop probe project at '$probeProject'."
}

if (-not (Test-Path $readmeSource)) {
    throw "Missing install README source at '$readmeSource'."
}

if (-not (Test-Path $versionSource)) {
    throw "Missing project version file at '$versionSource'."
}

if (-not (Test-Path $licenseSource)) {
    throw "Missing project license at '$licenseSource'."
}

if (-not (Test-Path $thirdPartySource)) {
    throw "Missing third-party notices at '$thirdPartySource'."
}

if ([string]::IsNullOrWhiteSpace($HelperDllPath)) {
    $HelperDllPath = Join-Path $repoRoot 'build\windows-live-desktop-probe\VirtualDesktopAccessor.dll'
}

$HelperDllPath = Resolve-LockingGlassVirtualDesktopHelper -HelperDllPath $HelperDllPath

$dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
if ($null -eq $dotnet) {
    throw 'dotnet.exe is required to publish the bundled Windows live desktop probe.'
}

New-Item -ItemType Directory -Force -Path $probePublishDir | Out-Null
& $dotnet.Source publish $probeProject `
    -c Release `
    -f net8.0 `
    -r win-x64 `
    --self-contained true `
    -o $probePublishDir `
    /p:PublishSingleFile=true `
    /p:EnableCompressionInSingleFile=true `
    /p:IncludeNativeLibrariesForSelfExtract=true
if ($LASTEXITCODE -ne 0) {
    throw 'dotnet publish failed for the Windows live desktop probe.'
}

Remove-Item -Recurse -Force $OutputDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

Copy-Item -Path $windowsBuild -Destination (Join-Path $OutputDir 'LockingGlass.exe') -Force
Copy-Item -Path $probeScriptSource -Destination (Join-Path $OutputDir 'run-live-desktop-probe.ps1') -Force
Copy-Item -Path $helperResolverSource -Destination (Join-Path $OutputDir 'resolve-virtual-desktop-helper.ps1') -Force
Copy-Item -Path $HelperDllPath -Destination (Join-Path $OutputDir 'VirtualDesktopAccessor.dll') -Force
Copy-Item -Path $readmeSource -Destination (Join-Path $OutputDir 'README.txt') -Force
Copy-Item -Path $versionSource -Destination (Join-Path $OutputDir 'VERSION.txt') -Force
Copy-Item -Path $licenseSource -Destination (Join-Path $OutputDir 'LICENSE.txt') -Force
Copy-Item -Path $thirdPartySource -Destination (Join-Path $OutputDir 'THIRD_PARTY_NOTICES.txt') -Force
Copy-Item -Path $installerSource -Destination (Join-Path $OutputDir 'Install-LockingGlass.ps1') -Force
Copy-Item -Path $launcherSource -Destination (Join-Path $OutputDir 'Start-LockingGlass.cmd') -Force
Copy-Item -Path (Join-Path $probePublishDir 'LockingGlass.WindowsLiveDesktopProbe*') -Destination $OutputDir -Force

Write-Host ('Staged LockingGlass Windows install package: ' + $OutputDir)
Write-Host ('Bundled app: ' + (Join-Path $OutputDir 'LockingGlass.exe'))
Write-Host ('Bundled version file: ' + (Join-Path $OutputDir 'VERSION.txt'))
Write-Host ('Bundled live watch script: ' + (Join-Path $OutputDir 'run-live-desktop-probe.ps1'))
Write-Host ('Bundled helper: ' + (Join-Path $OutputDir 'VirtualDesktopAccessor.dll'))
