$script:LockingGlassVirtualDesktopAccessorReleaseUrl = 'https://github.com/Ciantic/VirtualDesktopAccessor/releases/download/2024-12-16-windows11/VirtualDesktopAccessor.dll'
# This hash pin is the trust boundary for the downloaded helper DLL bundled
# into release packages.
$script:LockingGlassVirtualDesktopAccessorSha256 = '8740C572A1C000E3B87FFEB1E4C397EAE9AF3BD4A2ABDC3BCFFACAB4493F8FF5'

function Get-LockingGlassSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $hashBytes = $sha256.ComputeHash($stream)
        return ([System.BitConverter]::ToString($hashBytes) -replace '-', '').ToUpperInvariant()
    } finally {
        $stream.Dispose()
        $sha256.Dispose()
    }
}

function Resolve-LockingGlassVirtualDesktopHelper {
    param(
        [Parameter(Mandatory = $true)]
        [string]$HelperDllPath
    )

    $resolvedPath = [System.IO.Path]::GetFullPath($HelperDllPath)
    $downloadedThisRun = $false

    if (-not (Test-Path $resolvedPath)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedPath) | Out-Null
        Invoke-WebRequest -Uri $script:LockingGlassVirtualDesktopAccessorReleaseUrl -OutFile $resolvedPath
        $downloadedThisRun = $true
    }

    $actualHash = Get-LockingGlassSha256 -Path $resolvedPath
    if ($actualHash -ne $script:LockingGlassVirtualDesktopAccessorSha256) {
        if ($downloadedThisRun) {
            Remove-Item -Force $resolvedPath -ErrorAction SilentlyContinue
        }

        throw "VirtualDesktopAccessor.dll at '$resolvedPath' had SHA-256 '$actualHash', expected '$script:LockingGlassVirtualDesktopAccessorSha256'."
    }

    return $resolvedPath
}
