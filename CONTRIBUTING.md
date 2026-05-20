# Contributing

Thanks for helping improve LockingGlass.

## Before You Open A Pull Request

- Read [README.md](README.md) for the current build, test, packaging, and Windows runtime notes.
- Keep changes focused. Small, reviewable pull requests are easier to validate on the Windows proof path.
- Do not commit generated output from `build/`, `build-win/`, or `tools/*/bin` and `tools/*/obj`.
- Keep source files under `tools/windows_live_desktop_probe` and `tools/windows_installer_bootstrapper` tracked; the Windows release workflow depends on them.

## Development Checks

- Host build and tests:
  `make`
  `make test`
- Windows release build:
  `./scripts/test-release.ps1 -Mode Build`
- Optional non-release cross-build from a mingw-w64 environment:
  `make BUILD_DIR=build-win OBJ_DIR=build-win/obj BIN_DIR=build-win/bin OS=Windows_NT CXX=x86_64-w64-mingw32-g++ all`
- Release checks:
  `./scripts/test-release.ps1 -Mode Hygiene`
  `./scripts/test-release.ps1 -Mode Build`
  `./scripts/test-release.ps1 -Mode Package`
  These checks require the .NET SDK 8 or newer for helper project builds, plus `make`, `g++`, `windres`, and a Unix-like shell from MSYS2 or Git for Windows for the native Windows build.
- If you change packaging or installer behavior, run the release package mode and review:
  `scripts/stage-windows-install.ps1`
  `scripts/build-windows-installer.ps1`
  `scripts/install-staged-windows-build.ps1`

## Windows Runtime Changes

- Changes to live desktop watching, background tray behavior, or installed packaging should keep the repo's fail-closed behavior intact.
- If you change the live Windows helper-backed path, update the relevant docs under `docs/` and rerun the proof scripts when possible.
- Replay-only coverage is useful, but it is not a substitute for the real Windows proof path.
- Package smoke tests must use the installer autostart opt-out so verification never writes the real user's current-user Run key.

## Pull Request Notes

- Describe the user-visible behavior change.
- Mention which checks you ran.
- Call out any proof scripts or manual Windows validation you did not run.
