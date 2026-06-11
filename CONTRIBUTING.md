# Contributing

Thanks for helping improve Locking Glass. This file is for contributor setup,
release checks, packaging expectations, and proof boundaries. The README is
kept short for people who only want to install and use the app.

## Before You Open A Pull Request

- Read [AGENTS.md](AGENTS.md) and [STYLE.md](STYLE.md) before editing source,
  scripts, tests, or docs.
- Keep changes focused. Small, reviewable pull requests are easier to validate
  on the Windows proof path.
- Do not commit generated output from `build/`, `build-win/`,
  `tools/**/bin/`, or `tools/**/obj/`.
- Keep release-critical helper source tracked under
  `tools/windows_live_desktop_probe` and `tools/windows_installer_bootstrapper`.
- Keep repository branding source under `.branding/`, app/tray icon source under
  `assets/`, and update docs when release asset names or behavior change.

## Repository Map

- `include/locking_glass/core` and `src/core`: runtime assembly, session state,
  tray model projection, and desktop-locking policy.
- `include/locking_glass/platform` and `src/platform`: monitor enumeration,
  monitor watching, tray behavior, and background runtime.
- `include/locking_glass/integration` and `src/integration`: Windows API probes,
  autostart, live virtual desktop control, and desktop watch integration.
- `tests/*_test.cpp`: automated coverage linked into one test executable.
- `assets/locking-glass.ico`: canonical Windows app icon embedded in the app.
- `assets/icons`: ICO source frames plus tray and overlay PNGs checked by
  release hygiene.
- `.branding`: README logo, brandmark, and social preview assets.
- `tools/windows_live_desktop_probe`: self-contained Windows live-hook probe.
- `tools/windows_installer_bootstrapper`: embedded-payload bootstrapper used to
  build the public installer, public one-time runner, and internal uninstaller.

## Prerequisites

- Windows with PowerShell.
- .NET SDK 8 or newer for helper projects and bootstrapper publish.
- `make`, `g++`, `windres`, and a Unix-like shell from MSYS2 or Git for Windows
  for the native Windows build.

Stay on the native Windows toolchain unless a maintainer explicitly asks for
WSL.

## Development Checks

For the normal local build:

```powershell
make
make test
```

For the Windows release build and tests:

```powershell
./scripts/test-release.ps1 -Mode Build
```

For full release validation:

```powershell
./scripts/test-release.ps1 -Mode Hygiene
./scripts/test-release.ps1 -Mode Build
./scripts/test-release.ps1 -Mode Package
```

`-Mode Hygiene` checks release source tracking, generated-output hygiene, stale
runtime references, notice coverage, helper hash pins, icon dimensions, and
PowerShell syntax. `-Mode Build` builds helper projects, builds native Windows
binaries, runs C++ tests, and checks `--version` plus `--self-check`. `-Mode
Package` stages the install payload, builds the public executables, verifies
extract-only payloads, and smokes install/uninstall behavior.

## Packaging Contract

`VERSION` is the single version source. Public Windows releases should contain
exactly these user-facing downloads:

- `Locking Glass Installer.exe`: installs or updates the current-user app,
  enables startup by default, and launches after install by default.
- `Locking Glass.exe`: runs once by extracting the same validated payload to a
  temporary directory and starting the app without installing startup.

Do not reintroduce a portable zip or publish `SHA256SUMS.txt` as a release
asset. The local checksum file generated under `build/release` is package-gate
evidence only. The internal `Locking Glass Uninstaller.exe` is built for install
smoke and installed cleanup paths, not as a public release download.

Relevant packaging scripts:

- `scripts/stage-windows-install.ps1`
- `scripts/build-windows-installer.ps1`
- `scripts/install-staged-windows-build.ps1`
- `scripts/Uninstall-LockingGlass.ps1`
- `scripts/test-release.ps1`

GitHub Actions should delegate release checks to `scripts/test-release.ps1`.
Pushes to `dev` run hygiene and Windows build tests. Pushes to `main` also
package and publish the release assets for `v<VERSION>`.

## Windows Runtime Proof

Keep the fail-closed behavior intact. If the live desktop hook, helper DLL,
required exports, or staging desktop lifecycle cannot be proven available,
Locking Glass must not accept monitor-lock changes as if live control works.

Automated tests prove policy, persistence, replay/scripted seams, packaging,
and smoke behavior. They do not prove real tray interaction, real multiple
monitor behavior, or real Windows virtual desktop movement.

Manual proof scripts live under `scripts/`:

- `scripts/run-live-desktop-probe.ps1`
- `scripts/run-live-background-proof.ps1`
- `scripts/run-live-pin-proof.ps1`
- `scripts/run-installed-background-proof.ps1`
- `scripts/run-background-unavailable-proof.ps1`

Update the relevant docs under `docs/` and rerun the appropriate proof scripts
when changing live desktop watching, background tray behavior, installed
packaging, or fail-closed behavior. If you cannot run a live proof, say so in
the PR.

## Pull Request Notes

- Describe the user-visible behavior change.
- Mention which checks you ran.
- Call out proof scripts or manual Windows validation you did not run.
- Note release asset or packaging changes explicitly.
