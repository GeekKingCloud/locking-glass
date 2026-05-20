# Locking Glass

Locking Glass is a Windows tray app for pinning selected monitors across Windows virtual desktop switches.

Locked monitors keep their visible windows on the same desktop while unlocked monitors continue following normal Windows behavior. If the destination workspace already has windows on the locked monitor, Locking Glass moves those windows to a named `Locking Glass` staging desktop instead of pushing them onto another user workspace. When you unlock a monitor, Locking Glass makes a best-effort attempt to return the windows it moved for that lock back to their remembered original workspace if that workspace still exists. That remembered workspace is tracked only in memory for the current app run. The app is intentionally fail-closed: if the live desktop hook or staging desktop cannot start, Locking Glass keeps session state and tray controls available but does not pretend desktop locking is active.

## Behavior Contract

- Locking a monitor keeps the windows on that monitor visually pinned while other monitors continue following normal Windows desktop switches.
- Destination-workspace windows on a locked monitor are staged on a named `Locking Glass` virtual desktop so existing user workspaces are not used as overflow.
- Locking Glass only reuses a staging desktop identity it created during the same app run; it does not claim an existing user-created desktop by name.
- Unlocking a monitor triggers a best-effort immediate return for windows that Locking Glass itself moved successfully while that monitor was locked.
- The first successful follow-move becomes the remembered home workspace for that window during the current run.
- If the remembered workspace no longer exists, or the window can no longer be resolved safely, Locking Glass leaves the window where it is and reports the skip instead of guessing.
- Monitor identity and review state are persisted in the session file. Monitor locks start cleared whenever the app process starts; the tray is the only way to lock a monitor for the current run.
- Remembered home workspaces are not persisted and are forgotten when the app exits.
- If the live controller cannot prove the Windows desktop hook path, Locking Glass fails closed.

## Repository Map

- `include/locking_glass/core`: runtime assembly, session state, tray model, and desktop-locking policy.
- `include/locking_glass/platform`: monitor enumeration, monitor watching, and background tray session interfaces.
- `include/locking_glass/integration`: Windows probes, autostart, and live virtual desktop controller interfaces.
- `src/core`: session-store logic, diagnostics formatting, tray model projection, and monitor-locking policy.
- `src/platform`: Win32 monitor gateway, monitor watcher, and tray/background runtime.
- `src/integration`: Windows API probing, autostart registration, helper-backed live desktop control, and desktop watch bridge.
- `tests/*_test.cpp`: automated coverage for the app-level seams, linked into one test executable.
- `VERSION`: the single source of truth for the app, installer, and release version.
- `tools/windows_live_desktop_probe`: the Windows live-hook probe used by proof scripts and installed watch mode.
- `tools/windows_installer_bootstrapper`: the small self-contained bootstrapper used to produce `Locking Glass Installer.exe` and `Locking Glass Uninstaller.exe`.

## Build And Test

For the host build:

```bash
make
make test
make smoke
make prototype
```

For the Windows release build:

```powershell
./scripts/test-release.ps1 -Mode Build
```

For an optional non-release cross-build from a mingw-w64 environment:

```bash
make BUILD_DIR=build-win OBJ_DIR=build-win/obj BIN_DIR=build-win/bin OS=Windows_NT CXX=x86_64-w64-mingw32-g++ all
```

The automated C++ tests build into one executable named `locking_glass_tests`.

- `make test` builds and runs `build/bin/locking_glass_tests`
- the Windows test artifact is `build-win/bin/locking_glass_tests.exe`
- on success the runner prints progress for each check group and ends with `locking_glass_tests: ok`
- there is no active `tests/fakes/` subsystem; the harness uses environment-driven scripted seams instead of a reusable fake source tree

Current automated coverage includes:

- startup diagnostics and autostart planning
- session persistence, malformed-data recovery, and `.invalid` backup creation
- scripted monitor refresh handling
- scripted tray interaction flow, review prompts, and explicit `desktop-watch` replay after tray locks
- scripted desktop-locking policy
- unlock-return tracking, controller replay, and immediate tray-unlock return flow

Audit note: the automated tests prove policy, persistence, and scripted seam behavior. They do not claim to prove the real Windows desktop hook path by themselves.

Release verification is centralized in:

```powershell
./scripts/test-release.ps1 -Mode Hygiene
./scripts/test-release.ps1 -Mode Build
./scripts/test-release.ps1 -Mode Package
```

`-Mode All` runs the same checks end to end. The runner verifies repo hygiene, required helper source tracking, .NET SDK 8-or-newer helper builds, Windows native build/tests, installer extract smoke, installed-path smoke, uninstaller smoke, and `SHA256SUMS.txt`. The Windows native build expects `make`, `g++`, `windres`, and a Unix-like shell from MSYS2 or Git for Windows. The live desktop proof scripts remain manual because hosted CI cannot provide the required interactive Windows desktop, monitor, virtual desktop, or tray conditions.

## Running On Windows

- repo build: `build-win/bin/locking_glass.exe`
- release app: `build/release/Locking Glass.exe`
- installed build: `%LOCALAPPDATA%\Programs\Locking Glass\Locking Glass.exe`
- launching with no arguments starts the background tray app on Windows
- `--version` prints the current app version from the repo `VERSION` file
- `--self-check` prints startup diagnostics
- `--install-autostart` registers current-user startup with `--background`

Locking Glass stores monitor session state at `%LOCALAPPDATA%\Locking Glass\monitor-session-state.tsv` by default on Windows. Set `LOCKING_GLASS_SESSION_PATH` if you want to override that during testing.

Every app start begins with all present monitors unlocked. Saved monitor identity still helps recognize known displays, but Locking Glass does not automatically re-lock a monitor from a previous run.

Unlock return memory is separate from the session store. Locking Glass only remembers original workspaces for windows it moved successfully during the current run, and it forgets that information when the app exits.

## Live Windows Proof Scripts

- `scripts/run-live-desktop-probe.ps1`
  Downloads the pinned `VirtualDesktopAccessor.dll` release when needed, verifies its SHA-256, builds or launches the .NET 8 probe, and proves the live Windows hook path on a real desktop shell.
- `scripts/run-live-background-proof.ps1`
  Launches the real tray app, toggles a monitor lock through the live menu, switches desktops, and records the resulting desktop-switch reports.
- `scripts/run-live-pin-proof.ps1`
  Runs a lower-level live pinned-monitor proof with real Notepad windows. This is optional diagnostic coverage; `run-live-background-proof.ps1` is the release-facing tray workflow proof.
- `scripts/run-installed-background-proof.ps1`
  Re-runs the live background proof from the installed path instead of the repo build.
- `scripts/run-background-unavailable-proof.ps1`
  Proves the fail-closed behavior when the live controller cannot start.

Reference docs:

- [docs/windows-live-desktop-hook.md](docs/windows-live-desktop-hook.md)
- [docs/windows-monitor-pinning-acceptance-proof.md](docs/windows-monitor-pinning-acceptance-proof.md)
- [docs/windows-installed-background-proof.md](docs/windows-installed-background-proof.md)

## Evidence Model

- `make test` and `build-win/bin/locking_glass_tests.exe` are the source of truth for host-side policy, persistence, replay seams, and the scripted unlock-return flow.
- The PowerShell proof scripts are the source of truth for the live Windows helper-backed runtime path.
- Dated proof documents are evidence snapshots tied to the artifacts they cite. After behavior changes, re-run the proof scripts instead of treating older proof notes as evergreen claims.

## Packaging And Release

- `scripts/stage-windows-install.ps1`
  Stages the installable Windows payload under `build/windows-install-stage/Locking Glass/`.
- `scripts/install-staged-windows-build.ps1`
  Installs or updates the staged payload in `%LOCALAPPDATA%\Programs\Locking Glass`, creates Start Menu shortcuts, enables current-user autostart by default, and can optionally launch after install. Use `-NoAutostart` for package smoke tests or manual installs that should not write the Run key.
- `scripts/build-windows-installer.ps1`
  Wraps the staged payload into `Locking Glass Installer.exe` and `Locking Glass Uninstaller.exe` through the bootstrapper tool.
- `.github/workflows/windows-release.yml`
  Runs `scripts/test-release.ps1` on pull requests; tag builds also publish release assets after validating that the tag matches `VERSION`.

Public Windows release artifacts should include:

- `Locking Glass.exe`
- `Locking Glass Installer.exe`
- `Locking Glass Uninstaller.exe`
- `SHA256SUMS.txt`

`Locking Glass.exe` is the run-once portable app binary. It does not install itself. `Locking Glass Installer.exe` installs the current-user app, creates shortcuts, enables autostart by default, and launches after install by default. `Locking Glass Uninstaller.exe` removes the installed current-user app, autostart entry, and shortcuts while preserving session data unless `--remove-user-data` is supplied.

`SHA256SUMS.txt` contains SHA-256 hashes for the published executables so users can verify that the files they downloaded match the files that were released.

GitHub Actions intentionally stops at build, unit-test, packaging, extract-only installer smoke, and packaged `--self-check`. It does not run the live proof scripts because GitHub-hosted runners do not provide an interactive Windows desktop shell, multiple monitors, multiple virtual desktops, or tray interaction.

## How Updates Work

- The supported upgrade path is manual in-place reinstall through a newer `Locking Glass Installer.exe` or `Install-LockingGlass.ps1`.
- The installer stops the currently installed runtime, overwrites files in the stable install directory, preserves the external session-state file location, enables current-user autostart by default, and can relaunch the tray app.
- The uninstaller stops the installed runtime, removes the current-user autostart entry, removes Start Menu shortcuts, and removes installed app files. It preserves user data unless explicitly asked to remove it.
- Locking Glass does not include background update checks, release-feed polling, or self-applying updates.

## Windows Requirements

- Windows only
- at least two monitors
- at least two Windows virtual desktops
- `VirtualDesktopAccessor.dll` available beside the installed build or in the staged helper location, with the hook, move, and desktop lifecycle exports used by the Windows boundary

Release packages bundle the live desktop probe as a self-contained Windows executable, so normal installed use does not require a separate .NET runtime. Building from source still requires the .NET SDK 8 or newer for the helper projects and installer bootstrapper, plus MSYS2 or Git for Windows shell tools and a MinGW toolchain for the native Windows build.

If the helper DLL, required exports, or `Locking Glass` staging desktop lifecycle are unavailable, Locking Glass marks live desktop locking as unavailable and fails closed instead of replaying, guessing, or pushing windows onto another user workspace.

## License

Locking Glass is licensed under `GPL-3.0-only`. See [LICENSE](LICENSE).

Windows release packages currently bundle `VirtualDesktopAccessor.dll` under the MIT License. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for contributor workflow and validation notes.
