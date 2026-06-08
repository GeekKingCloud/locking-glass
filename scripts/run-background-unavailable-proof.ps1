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
$helperLocations.Add((Join-Path (Split-Path -Parent $watchExe) 'VirtualDesktopAccessor.dll'))
$helperLocations.Add((Join-Path $repoRoot 'build\windows-live-desktop-probe\VirtualDesktopAccessor.dll'))
$helperLocations.Add((Join-Path $repoRoot 'VirtualDesktopAccessor.dll'))
foreach ($helperLocation in $helperLocations) {
    if (Test-Path $helperLocation) {
        throw "This proof requires VirtualDesktopAccessor.dll to be absent from runtime helper search locations. Remove '$helperLocation' before running this proof."
    }
}

$env:LOCKING_GLASS_SESSION_PATH = $sessionPath

$process = Start-Process -FilePath $watchExe -ArgumentList '--background' -WorkingDirectory $repoRoot -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
try {
    if (-not $process.WaitForExit(15000)) {
        throw 'Locking Glass stayed running even though the live desktop controller was unavailable.'
    }
    if ($process.ExitCode -eq 0) {
        throw 'Locking Glass exited successfully even though the live desktop controller was unavailable.'
    }
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
    Write-Host ('background exit code: ' + $process.ExitCode)

    Remove-Item Env:LOCKING_GLASS_SESSION_PATH -ErrorAction SilentlyContinue
}
