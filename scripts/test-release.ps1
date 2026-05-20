param(
    [ValidateSet('All', 'Hygiene', 'Build', 'Package')]
    [string]$Mode = 'All',
    [string]$AppExeName,
    [string]$SetupExeName,
    [string]$UninstallerExeName
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$gitSafeDirectory = $repoRoot.Replace('\', '/')
$version = (Get-Content (Join-Path $repoRoot 'VERSION') -Raw).Trim()
$helperResolverPath = Join-Path $repoRoot 'scripts\resolve-virtual-desktop-helper.ps1'
. $helperResolverPath

if ([string]::IsNullOrWhiteSpace($AppExeName)) {
    $AppExeName = 'Locking Glass.exe'
}

if ([string]::IsNullOrWhiteSpace($SetupExeName)) {
    $SetupExeName = 'Locking Glass Installer.exe'
}

if ([string]::IsNullOrWhiteSpace($UninstallerExeName)) {
    $UninstallerExeName = 'Locking Glass Uninstaller.exe'
}

function Write-Step([string]$Message) {
    Write-Host ''
    Write-Host ('==> ' + $Message)
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = $repoRoot
    )

    Write-Host ('> ' + $FilePath + ' ' + ($Arguments -join ' '))
    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code $LASTEXITCODE`: $FilePath $($Arguments -join ' ')"
        }
    } finally {
        Pop-Location
    }
}

function Invoke-ExecutableOutput {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$Description = $FilePath
    )

    $outputDir = Join-Path $repoRoot 'build\release-command-output'
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
    $name = [System.IO.Path]::GetRandomFileName()
    $stdoutPath = Join-Path $outputDir ($name + '.stdout.txt')
    $stderrPath = Join-Path $outputDir ($name + '.stderr.txt')

    try {
        $process = Start-Process `
            -FilePath $FilePath `
            -ArgumentList $Arguments `
            -WorkingDirectory $repoRoot `
            -NoNewWindow `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath `
            -Wait `
            -PassThru
        $stdout = if (Test-Path $stdoutPath) {
            [System.IO.File]::ReadAllText($stdoutPath)
        } else {
            ''
        }
        $stderr = if (Test-Path $stderrPath) {
            [System.IO.File]::ReadAllText($stderrPath)
        } else {
            ''
        }
        if ($process.ExitCode -ne 0) {
            throw "$Description failed with exit code $($process.ExitCode).`n$stderr"
        }

        if (-not [string]::IsNullOrWhiteSpace($stderr)) {
            Write-Host $stderr.TrimEnd()
        }
        return $stdout
    } finally {
        Remove-Item -Force $stdoutPath, $stderrPath -ErrorAction SilentlyContinue
    }
}

function Invoke-Git {
    param([string[]]$Arguments)

    $gitArguments = @('-c', "safe.directory=$gitSafeDirectory") + $Arguments
    $output = & git @gitArguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
    }

    return @($output)
}

function Assert-LastCommandSucceeded([string]$Description) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

function Require-Command([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "Required command '$Name' was not found on PATH."
    }

    return $command.Source
}

function Require-DotNetCompatibleSdk {
    $dotnet = Require-Command 'dotnet'
    $sdks = @(& $dotnet --list-sdks)
    $compatibleSdk = $sdks | Where-Object {
        $_ -match '^(\d+)\.' -and [int]$Matches[1] -ge 8
    } | Select-Object -First 1
    if ($null -eq $compatibleSdk) {
        throw ".NET SDK 8 or newer is required. Installed SDKs:`n$($sdks -join [Environment]::NewLine)"
    }

    return $dotnet
}

function Add-PathPrefixIfPresent([string]$Candidate) {
    if ((Test-Path $Candidate) -and ($env:PATH -notlike ('*' + $Candidate + '*'))) {
        $env:PATH = $Candidate + [System.IO.Path]::PathSeparator + $env:PATH
    }
}

function Add-WindowsUnixToolsToPathIfPresent {
    $candidates = @(
        'C:\msys64\mingw64\bin',
        'C:\msys64\usr\bin',
        'C:\Program Files\Git\usr\bin'
    )

    foreach ($candidate in $candidates) {
        Add-PathPrefixIfPresent $candidate
    }
}

function Require-WindowsUnixBuildShell {
    $sh = Require-Command 'sh'
    $probe = 'command -v cat find sort mkdir rm >/dev/null'
    & $sh -lc $probe
    if ($LASTEXITCODE -ne 0) {
        throw "A working Unix-like shell with cat, find, sort, mkdir, and rm is required for the Makefile. Install MSYS2 or Git for Windows, then ensure its usr\bin directory is on PATH."
    }

    $requiredTools = @('make', 'g++', 'windres')
    foreach ($tool in $requiredTools) {
        try {
            $null = Require-Command $tool
        } catch {
            throw "Required Windows build tool '$tool' was not found on PATH. Install MSYS2 mingw-w64 GCC or an equivalent MinGW toolchain."
        }
    }
}

function Test-RequiredSourceTracked {
    $requiredTrackedFiles = @(
        '.github/workflows/windows-release.yml',
        'scripts/test-release.ps1',
        'scripts/Uninstall-LockingGlass.ps1',
        'tools/windows_live_desktop_probe/LockingGlass.WindowsLiveDesktopProbe.csproj',
        'tools/windows_live_desktop_probe/Program.cs',
        'tools/windows_installer_bootstrapper/LockingGlass.WindowsInstallerBootstrapper.csproj',
        'tools/windows_installer_bootstrapper/Program.cs'
    )

    $missingTrackedFiles = @()
    foreach ($path in $requiredTrackedFiles) {
        $null = & git -c "safe.directory=$gitSafeDirectory" ls-files --error-unmatch $path 2>$null
        if ($LASTEXITCODE -ne 0) {
            $missingTrackedFiles += $path
        }
    }

    if ($missingTrackedFiles.Count -gt 0) {
        throw "Required release source file(s) are not tracked by git. Add them before release verification can pass:`n$($missingTrackedFiles -join [Environment]::NewLine)"
    }
}

function Test-NoTrackedGeneratedOutput {
    $trackedGenerated = @()
    $trackedGenerated += Invoke-Git @('ls-files', '--', 'build', 'build-win')
    $trackedGenerated += Invoke-Git @('ls-files', '--', 'tools/**/bin/**', 'tools/**/obj/**')
    $trackedGenerated = @($trackedGenerated | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })

    if ($trackedGenerated.Count -gt 0) {
        throw "Generated output is tracked by git:`n$($trackedGenerated -join [Environment]::NewLine)"
    }
}

function Get-ReleaseTextFiles {
    $roots = @(
        '.github',
        'docs',
        'include',
        'scripts',
        'src',
        'tests',
        'tools'
    )
    $files = @(
        '.gitattributes',
        '.gitignore',
        'CONTRIBUTING.md',
        'Directory.Build.props',
        'Makefile',
        'README.md',
        'THIRD_PARTY_NOTICES.md',
        'VERSION'
    )

    foreach ($root in $roots) {
        $fullRoot = Join-Path $repoRoot $root
        if (-not (Test-Path $fullRoot)) {
            continue
        }

        $files += Get-ChildItem -Path $fullRoot -Recurse -File |
            Where-Object {
                $_.FullName -notmatch '\\(bin|obj)\\' -and
                $_.FullName -notmatch '\\build(-win)?\\'
            } |
            ForEach-Object { [System.IO.Path]::GetRelativePath($repoRoot, $_.FullName) }
    }

    return @($files | Sort-Object -Unique)
}

function Test-NoStaleRuntimeReferences {
    $stalePatterns = @(
        'netcoreapp3\.1',
        '\b3\.1\.x\b'
    )

    $hits = @()
    foreach ($file in Get-ReleaseTextFiles) {
        $fullPath = Join-Path $repoRoot $file
        if (-not (Test-Path $fullPath)) {
            continue
        }

        foreach ($pattern in $stalePatterns) {
            $matches = @(Select-String -Path $fullPath -Pattern $pattern)
            foreach ($match in $matches) {
                $hits += ('{0}:{1}:{2}' -f $file, $match.LineNumber, $match.Line.Trim())
            }
        }
    }

    if ($hits.Count -gt 0) {
        throw "Stale .NET 3.1 reference(s) found:`n$($hits -join [Environment]::NewLine)"
    }
}

function Test-ThirdPartyNoticeCoverage {
    $noticePath = Join-Path $repoRoot 'THIRD_PARTY_NOTICES.md'
    $noticeText = Get-Content -Path $noticePath -Raw
    $requiredNoticePatterns = @(
        'VirtualDesktopAccessor\.dll',
        'DOTNET_RUNTIME_LICENSE\.txt',
        'DOTNET_RUNTIME_THIRD_PARTY_NOTICES\.txt'
    )

    $missingPatterns = @()
    foreach ($pattern in $requiredNoticePatterns) {
        if ($noticeText -notmatch $pattern) {
            $missingPatterns += $pattern
        }
    }

    if ($missingPatterns.Count -gt 0) {
        throw "THIRD_PARTY_NOTICES.md is missing release notice coverage for:`n$($missingPatterns -join [Environment]::NewLine)"
    }
}

function Test-PowerShellSyntax {
    $scriptFiles = Get-ChildItem -Path (Join-Path $repoRoot 'scripts') -Filter '*.ps1' -File
    foreach ($scriptFile in $scriptFiles) {
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile(
            $scriptFile.FullName,
            [ref]$tokens,
            [ref]$errors) | Out-Null
        if ($errors.Count -gt 0) {
            $formatted = $errors | ForEach-Object {
                '{0}:{1}:{2}' -f $scriptFile.FullName, $_.Extent.StartLineNumber, $_.Message
            }
            throw "PowerShell parse error(s):`n$($formatted -join [Environment]::NewLine)"
        }
    }
}

function Test-Hygiene {
    Write-Step 'Checking release hygiene'
    if ($version -notmatch '^\d+\.\d+\.\d+$') {
        throw "VERSION must be SemVer X.Y.Z, got '$version'."
    }

    Test-RequiredSourceTracked
    Test-NoTrackedGeneratedOutput
    Test-NoStaleRuntimeReferences
    Test-ThirdPartyNoticeCoverage
    Test-PowerShellSyntax
}

function Test-Build {
    Write-Step 'Building .NET helper projects'
    $dotnet = Require-DotNetCompatibleSdk
    Invoke-Checked $dotnet -Arguments @('build', 'tools\windows_live_desktop_probe\LockingGlass.WindowsLiveDesktopProbe.csproj', '-c', 'Release')
    Invoke-Checked $dotnet -Arguments @('build', 'tools\windows_installer_bootstrapper\LockingGlass.WindowsInstallerBootstrapper.csproj', '-c', 'Release')

    Write-Step 'Building and testing Windows binaries'
    Add-WindowsUnixToolsToPathIfPresent
    Require-WindowsUnixBuildShell
    $make = Require-Command 'make'
    $makeArgs = @(
        'BUILD_DIR=build-win',
        'OBJ_DIR=build-win/obj',
        'BIN_DIR=build-win/bin',
        'OS=Windows_NT',
        'CXX=g++'
    )
    Invoke-Checked $make -Arguments ($makeArgs + @('all'))
    Invoke-Checked $make -Arguments ($makeArgs + @('test'))

    $app = Join-Path $repoRoot 'build-win\bin\locking_glass.exe'
    if (-not (Test-Path $app)) {
        throw "Expected Windows app was not built: $app"
    }

    $versionOutput = Invoke-ExecutableOutput $app -Arguments @('--version') -Description 'Built --version check'
    if ($versionOutput -notmatch [regex]::Escape($version)) {
        throw "Built --version output '$versionOutput' did not include expected version '$version'."
    }

    $null = Invoke-ExecutableOutput $app -Arguments @('--self-check') -Description 'Built --self-check'
}

function Get-RequiredPackageFiles {
    return @(
        'Locking Glass.exe',
        'Install-LockingGlass.ps1',
        'Uninstall-LockingGlass.ps1',
        'Start-LockingGlass.cmd',
        'run-live-desktop-probe.ps1',
        'resolve-virtual-desktop-helper.ps1',
        'VirtualDesktopAccessor.dll',
        'README.txt',
        'VERSION.txt',
        'LICENSE.txt',
        'THIRD_PARTY_NOTICES.txt',
        'DOTNET_RUNTIME_LICENSE.txt',
        'DOTNET_RUNTIME_THIRD_PARTY_NOTICES.txt',
        'LOCKING_GLASS_PAYLOAD_MANIFEST.txt'
    )
}

function Assert-PackagePayload([string]$Directory) {
    foreach ($requiredFile in Get-RequiredPackageFiles) {
        $candidate = Join-Path $Directory $requiredFile
        if (-not (Test-Path $candidate)) {
            throw "Missing packaged file '$requiredFile' under '$Directory'."
        }
    }

    $probeFiles = @(Get-ChildItem -Path $Directory -File |
        Where-Object { $_.Name -like 'LockingGlass.WindowsLiveDesktopProbe*' })
    if ($probeFiles.Count -eq 0) {
        throw "Missing bundled Windows live desktop probe output under '$Directory'."
    }
}

function Test-Sha256Sums([string]$SumPath, [string[]]$ExpectedPaths) {
    if (-not (Test-Path $SumPath)) {
        throw "Missing checksum file: $SumPath"
    }

    $lines = @(Get-Content -Path $SumPath)
    if ($lines.Count -ne $ExpectedPaths.Count) {
        throw "SHA256SUMS.txt should contain exactly $($ExpectedPaths.Count) lines, found $($lines.Count)."
    }

    $expectedFiles = $ExpectedPaths | ForEach-Object { Get-Item $_ }
    foreach ($expectedFile in $expectedFiles) {
        $expectedHash = (Get-FileHash -Algorithm SHA256 $expectedFile.FullName).Hash.ToLowerInvariant()
        $expectedName = $expectedFile.Name
        $expectedLine = "$expectedHash  $expectedName"
        $matching = @($lines | Where-Object { $_ -ceq $expectedLine })
        if ($matching.Count -ne 1) {
            throw "SHA256SUMS.txt does not contain the recomputed checksum line '$expectedLine'."
        }
    }
}

function Test-PinnedVirtualDesktopHelper([string]$Directory, [string]$Label) {
    $helperPath = Join-Path $Directory 'VirtualDesktopAccessor.dll'
    if (-not (Test-Path $helperPath)) {
        throw "$Label is missing VirtualDesktopAccessor.dll under '$Directory'."
    }

    # Re-check staged and extracted payloads against the resolver's pinned hash
    # so packaging cannot silently swap the helper DLL.
    $actualHash = (Get-FileHash -Algorithm SHA256 $helperPath).Hash.ToUpperInvariant()
    if ($actualHash -ne $script:LockingGlassVirtualDesktopAccessorSha256) {
        throw "$Label bundled VirtualDesktopAccessor.dll SHA-256 '$actualHash' did not match expected '$script:LockingGlassVirtualDesktopAccessorSha256'."
    }
}

function Test-PayloadManifest([string]$Directory, [string]$Label) {
    $manifestPath = Join-Path $Directory 'LOCKING_GLASS_PAYLOAD_MANIFEST.txt'
    if (-not (Test-Path $manifestPath)) {
        throw "$Label is missing LOCKING_GLASS_PAYLOAD_MANIFEST.txt under '$Directory'."
    }

    $expectedFiles = @(Get-ChildItem -Path $Directory -File |
        Where-Object { $_.Name -ne 'LOCKING_GLASS_PAYLOAD_MANIFEST.txt' } |
        Sort-Object -Property Name)
    $manifestLines = @(Get-Content -Path $manifestPath)
    if ($manifestLines.Count -ne $expectedFiles.Count) {
        throw "$Label manifest lists $($manifestLines.Count) file(s), expected $($expectedFiles.Count)."
    }

    foreach ($expectedFile in $expectedFiles) {
        $expectedHash = (Get-FileHash -Algorithm SHA256 $expectedFile.FullName).Hash.ToLowerInvariant()
        $expectedLine = "$expectedHash  $($expectedFile.Name)"
        $matching = @($manifestLines | Where-Object { $_ -ceq $expectedLine })
        if ($matching.Count -ne 1) {
            throw "$Label manifest does not contain the recomputed line '$expectedLine'."
        }
    }
}

function Test-ExtractedPackage([string]$Directory, [string]$Label) {
    Assert-PackagePayload $Directory
    Test-PayloadManifest -Directory $Directory -Label $Label
    Test-PinnedVirtualDesktopHelper -Directory $Directory -Label $Label

    $packagedExe = Join-Path $Directory 'Locking Glass.exe'
    $versionOutput = Invoke-ExecutableOutput $packagedExe -Arguments @('--version') -Description "$Label --version check"
    if ($versionOutput -notmatch [regex]::Escape($version)) {
        throw "$Label --version output '$versionOutput' did not include expected version '$version'."
    }

    $null = Invoke-ExecutableOutput $packagedExe -Arguments @('--self-check') -Description "$Label --self-check"
}

function Test-Package {
    Write-Step 'Staging and packaging Windows release artifacts'
    $stageDir = Join-Path $repoRoot 'build\windows-install-stage\Locking Glass'
    $installerDir = Join-Path $repoRoot 'build\windows-installer'
    $releaseDir = Join-Path $repoRoot 'build\release'
    $extractDir = Join-Path $repoRoot 'build\windows-installer-smoke'
    $installSmokeRoot = Join-Path $repoRoot 'build\windows-install-smoke'
    $installDir = Join-Path $installSmokeRoot 'Programs\Locking Glass'
    $setupPath = Join-Path $installerDir $SetupExeName
    $uninstallerPath = Join-Path $installerDir $UninstallerExeName
    $releaseAppPath = Join-Path $releaseDir $AppExeName
    $releaseSetupPath = Join-Path $releaseDir $SetupExeName
    $releaseUninstallerPath = Join-Path $releaseDir $UninstallerExeName
    $sumPath = Join-Path $releaseDir 'SHA256SUMS.txt'

    & (Join-Path $repoRoot 'scripts\stage-windows-install.ps1')
    Assert-LastCommandSucceeded 'stage-windows-install.ps1'
    Assert-PackagePayload $stageDir
    Test-PayloadManifest -Directory $stageDir -Label 'Staged package'
    Test-PinnedVirtualDesktopHelper -Directory $stageDir -Label 'Staged package'

    & (Join-Path $repoRoot 'scripts\build-windows-installer.ps1') `
        -StageDir $stageDir `
        -OutputDir $installerDir `
        -SetupExeName $SetupExeName `
        -UninstallerExeName $UninstallerExeName `
        -SkipStage
    Assert-LastCommandSucceeded 'build-windows-installer.ps1'

    New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
    Remove-Item -Force $releaseAppPath, $releaseSetupPath, $releaseUninstallerPath, $sumPath -ErrorAction SilentlyContinue
    Copy-Item -Path (Join-Path $stageDir 'Locking Glass.exe') -Destination $releaseAppPath -Force
    Copy-Item -Path $setupPath -Destination $releaseSetupPath -Force
    Copy-Item -Path $uninstallerPath -Destination $releaseUninstallerPath -Force

    $sumLines = @(
        "$((Get-FileHash -Algorithm SHA256 $releaseAppPath).Hash.ToLowerInvariant())  $AppExeName",
        "$((Get-FileHash -Algorithm SHA256 $releaseSetupPath).Hash.ToLowerInvariant())  $SetupExeName",
        "$((Get-FileHash -Algorithm SHA256 $releaseUninstallerPath).Hash.ToLowerInvariant())  $UninstallerExeName"
    )
    Set-Content -Path $sumPath -Value $sumLines -Encoding ascii
    Test-Sha256Sums -SumPath $sumPath -ExpectedPaths @($releaseAppPath, $releaseSetupPath, $releaseUninstallerPath)

    Remove-Item -Recurse -Force $extractDir -ErrorAction SilentlyContinue
    Invoke-Checked $releaseSetupPath -Arguments @('--extract-only', $extractDir)
    Test-ExtractedPackage -Directory $extractDir -Label 'Setup extract-only smoke'

    Remove-Item -Recurse -Force $installSmokeRoot -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $installSmokeRoot | Out-Null
    $previousAppData = $env:APPDATA
    $previousLocalAppData = $env:LOCALAPPDATA
    try {
        $env:APPDATA = Join-Path $installSmokeRoot 'Roaming'
        $env:LOCALAPPDATA = Join-Path $installSmokeRoot 'Local'
        New-Item -ItemType Directory -Force -Path $env:APPDATA, $env:LOCALAPPDATA | Out-Null
        Invoke-Checked $releaseSetupPath -Arguments @('--install-dir', $installDir, '--no-autostart', '--no-launch-after-install')
        Test-ExtractedPackage -Directory $installDir -Label 'Installed setup smoke'
        Invoke-Checked $releaseUninstallerPath -Arguments @('--install-dir', $installDir)
    } finally {
        $env:APPDATA = $previousAppData
        $env:LOCALAPPDATA = $previousLocalAppData
    }
    if (Test-Path $installDir) {
        throw "Uninstaller smoke did not remove install directory '$installDir'."
    }

    $startMenuDir = Join-Path $installSmokeRoot 'Roaming\Microsoft\Windows\Start Menu\Programs\Locking Glass'
    if (Test-Path $startMenuDir) {
        throw "Uninstaller smoke did not remove Start Menu directory '$startMenuDir'."
    }
}

if ($Mode -eq 'All' -or $Mode -eq 'Hygiene') {
    Test-Hygiene
}

if ($Mode -eq 'All' -or $Mode -eq 'Build') {
    Test-Build
}

if ($Mode -eq 'All' -or $Mode -eq 'Package') {
    Test-Package
}

Write-Step "Release test mode '$Mode' passed"
