param(
    [string]$ProofDir
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$watchExe = Join-Path $repoRoot 'build-win\bin\locking_glass.exe'

if (-not (Test-Path $watchExe)) {
    throw "Missing Windows build: $watchExe. Build it first with mingw."
}

if ([string]::IsNullOrWhiteSpace($ProofDir)) {
    $ProofDir = Join-Path $repoRoot 'build\windows-background-unavailable-proof'
}

New-Item -ItemType Directory -Force -Path $ProofDir | Out-Null

$sessionPath = Join-Path $ProofDir 'session.tsv'
$stdoutPath = Join-Path $ProofDir 'background.stdout.txt'
$stderrPath = Join-Path $ProofDir 'background.stderr.txt'
Remove-Item $sessionPath, $stdoutPath, $stderrPath -ErrorAction SilentlyContinue

$helperLocations = [System.Collections.Generic.List[string]]::new()
$current = [System.IO.Path]::GetFullPath((Split-Path -Parent $watchExe))
while (-not [string]::IsNullOrWhiteSpace($current)) {
    $helperLocations.Add((Join-Path $current 'build\windows-live-desktop-probe\VirtualDesktopAccessor.dll'))
    $helperLocations.Add((Join-Path $current 'VirtualDesktopAccessor.dll'))
    $parent = Split-Path -Parent $current
    if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $current) {
        break
    }
    $current = $parent
}
foreach ($helperLocation in $helperLocations) {
    if (Test-Path $helperLocation) {
        throw "This proof requires VirtualDesktopAccessor.dll to be absent from runtime helper search locations. Remove '$helperLocation' before running this proof."
    }
}

$env:LOCKING_GLASS_SESSION_PATH = $sessionPath

$process = Start-Process -FilePath $watchExe -ArgumentList '--background' -WorkingDirectory $repoRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
try {
    Start-Sleep -Seconds 5
}
finally {
    if ($process -and -not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit(5000)
    }

    $stderr = if (Test-Path $stderrPath) {
        [System.IO.File]::ReadAllText($stderrPath)
    }
    else {
        ''
    }

    Write-Host ('background stdout: ' + $stdoutPath)
    Write-Host ('background stderr: ' + $stderrPath)
    if ($stderr.Length -gt 0) {
        $stderr
    }

    Remove-Item Env:LOCKING_GLASS_SESSION_PATH -ErrorAction SilentlyContinue
}
