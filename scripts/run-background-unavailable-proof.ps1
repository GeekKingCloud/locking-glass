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

$env:LOCKING_GLASS_SESSION_PATH = $sessionPath
$env:LOCKING_GLASS_VIRTUAL_DESKTOP_HELPER = 'C:\definitely-missing\VirtualDesktopAccessor.dll'

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
    Remove-Item Env:LOCKING_GLASS_VIRTUAL_DESKTOP_HELPER -ErrorAction SilentlyContinue
}
