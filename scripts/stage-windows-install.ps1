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
$uninstallerSource = Join-Path $repoRoot 'scripts\Uninstall-LockingGlass.ps1'
$launcherSource = Join-Path $repoRoot 'scripts\Start-LockingGlass.cmd'
$probeScriptSource = Join-Path $repoRoot 'scripts\run-live-desktop-probe.ps1'
$helperResolverSource = Join-Path $repoRoot 'scripts\resolve-virtual-desktop-helper.ps1'
$payloadManifestName = 'LOCKING_GLASS_PAYLOAD_MANIFEST.txt'

. $helperResolverSource

function Resolve-StageOutputDirectory([string]$TargetOutputDir) {
    if ([string]::IsNullOrWhiteSpace($TargetOutputDir)) {
        throw 'OutputDir must not be empty.'
    }

    $resolvedOutputDir = [System.IO.Path]::GetFullPath($TargetOutputDir).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)
    $allowedRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $repoRoot 'build\windows-install-stage')).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar)

    if (-not (
            [string]::Equals(
                $resolvedOutputDir,
                $allowedRoot,
                [System.StringComparison]::OrdinalIgnoreCase) -or
            $resolvedOutputDir.StartsWith(
                $allowedRoot + [System.IO.Path]::DirectorySeparatorChar,
                [System.StringComparison]::OrdinalIgnoreCase))) {
        throw "OutputDir must be under the repo-local build\\windows-install-stage directory: '$resolvedOutputDir'."
    }

    $leafName = Split-Path -Path $resolvedOutputDir -Leaf
    if ([string]::IsNullOrWhiteSpace($leafName)) {
        throw "OutputDir must name a concrete staged package directory: '$resolvedOutputDir'."
    }

    return $resolvedOutputDir
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repoRoot 'build\windows-install-stage\Locking Glass'
}
$OutputDir = Resolve-StageOutputDirectory -TargetOutputDir $OutputDir

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

$dotnetRoot = Split-Path -Parent $dotnet.Source
$dotnetLicenseSource = Join-Path $dotnetRoot 'LICENSE.txt'
$dotnetThirdPartySource = Join-Path $dotnetRoot 'ThirdPartyNotices.txt'
if (-not (Test-Path $dotnetLicenseSource)) {
    throw "Missing .NET runtime license notice at '$dotnetLicenseSource'."
}
if (-not (Test-Path $dotnetThirdPartySource)) {
    throw "Missing .NET runtime third-party notice file at '$dotnetThirdPartySource'."
}

Remove-Item -Recurse -Force $probePublishDir -ErrorAction SilentlyContinue
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

Copy-Item -Path $windowsBuild -Destination (Join-Path $OutputDir 'Locking Glass.exe') -Force
Copy-Item -Path $probeScriptSource -Destination (Join-Path $OutputDir 'run-live-desktop-probe.ps1') -Force
Copy-Item -Path $helperResolverSource -Destination (Join-Path $OutputDir 'resolve-virtual-desktop-helper.ps1') -Force
Copy-Item -Path $HelperDllPath -Destination (Join-Path $OutputDir 'VirtualDesktopAccessor.dll') -Force
Copy-Item -Path $readmeSource -Destination (Join-Path $OutputDir 'README.txt') -Force
Copy-Item -Path $versionSource -Destination (Join-Path $OutputDir 'VERSION.txt') -Force
Copy-Item -Path $licenseSource -Destination (Join-Path $OutputDir 'LICENSE.txt') -Force
Copy-Item -Path $thirdPartySource -Destination (Join-Path $OutputDir 'THIRD_PARTY_NOTICES.txt') -Force
Copy-Item -Path $dotnetLicenseSource -Destination (Join-Path $OutputDir 'DOTNET_RUNTIME_LICENSE.txt') -Force
Copy-Item -Path $dotnetThirdPartySource -Destination (Join-Path $OutputDir 'DOTNET_RUNTIME_THIRD_PARTY_NOTICES.txt') -Force
Copy-Item -Path $installerSource -Destination (Join-Path $OutputDir 'Install-LockingGlass.ps1') -Force
Copy-Item -Path $uninstallerSource -Destination (Join-Path $OutputDir 'Uninstall-LockingGlass.ps1') -Force
Copy-Item -Path $launcherSource -Destination (Join-Path $OutputDir 'Start-LockingGlass.cmd') -Force
Copy-Item -Path (Join-Path $probePublishDir 'LockingGlass.WindowsLiveDesktopProbe*') -Destination $OutputDir -Force

$manifestPath = Join-Path $OutputDir $payloadManifestName
# This manifest is the bootstrapper's allow-list after extraction, not just a
# release checksum convenience.
$manifestLines = Get-ChildItem -Path $OutputDir -File |
    Where-Object { $_.Name -ne $payloadManifestName } |
    Sort-Object -Property Name |
    ForEach-Object {
        "$((Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant())  $($_.Name)"
    }
Set-Content -Path $manifestPath -Value $manifestLines -Encoding ascii

Write-Host ('Staged Locking Glass Windows install package: ' + $OutputDir)
Write-Host ('Bundled app: ' + (Join-Path $OutputDir 'Locking Glass.exe'))
Write-Host ('Bundled version file: ' + (Join-Path $OutputDir 'VERSION.txt'))
Write-Host ('Bundled live watch script: ' + (Join-Path $OutputDir 'run-live-desktop-probe.ps1'))
Write-Host ('Bundled helper: ' + (Join-Path $OutputDir 'VirtualDesktopAccessor.dll'))
