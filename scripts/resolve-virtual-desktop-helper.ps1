$script:LockingGlassVirtualDesktopAccessorReleaseUrl = 'https://github.com/Ciantic/VirtualDesktopAccessor/releases/download/2024-12-16-windows11/VirtualDesktopAccessor.dll'
$script:LockingGlassVirtualDesktopAccessorSha256 = '8740C572A1C000E3B87FFEB1E4C397EAE9AF3BD4A2ABDC3BCFFACAB4493F8FF5'

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

    $actualHash = (Get-FileHash -Algorithm SHA256 $resolvedPath).Hash.ToUpperInvariant()
    if ($actualHash -ne $script:LockingGlassVirtualDesktopAccessorSha256) {
        if ($downloadedThisRun) {
            Remove-Item -Force $resolvedPath -ErrorAction SilentlyContinue
        }

        throw "VirtualDesktopAccessor.dll at '$resolvedPath' had SHA-256 '$actualHash', expected '$script:LockingGlassVirtualDesktopAccessorSha256'."
    }

    return $resolvedPath
}
