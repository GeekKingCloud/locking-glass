# Contributing

Thanks for helping improve LockingGlass.

## Before You Open A Pull Request

- Read [README.md](README.md) for the current build, test, packaging, and Windows runtime notes.
- Keep changes focused. Small, reviewable pull requests are easier to validate on the Windows proof path.
- Do not commit generated output from `build/`, `build-win/`, or `tools/*/bin` and `tools/*/obj`.

## Development Checks

- Host build and tests:
  `make`
  `make test`
- Windows cross-build:
  `make BUILD_DIR=build-win OBJ_DIR=build-win/obj BIN_DIR=build-win/bin OS=Windows_NT CXX=x86_64-w64-mingw32-g++ all`
- If you change packaging or installer behavior, also review:
  `scripts/stage-windows-install.ps1`
  `scripts/build-windows-installer.ps1`

## Windows Runtime Changes

- Changes to live desktop watching, background tray behavior, or installed packaging should keep the repo's fail-closed behavior intact.
- If you change the live Windows helper-backed path, update the relevant docs under `docs/` and rerun the proof scripts when possible.
- Replay-only coverage is useful, but it is not a substitute for the real Windows proof path.

## Pull Request Notes

- Describe the user-visible behavior change.
- Mention which checks you ran.
- Call out any proof scripts or manual Windows validation you did not run.
