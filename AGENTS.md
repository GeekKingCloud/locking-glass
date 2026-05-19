# LockingGlass Agent Instructions

Start with the global coding-agent instructions at `C:\Users\thete\AGENTS.md`.
Those instructions remain authoritative for shared workflow, skills, safety,
memory, and collaboration rules.

This file adds project-local rules for LockingGlass. Read `STYLE.md` before
editing source, scripts, tests, or docs in this repository.

## Project Boundary

LockingGlass is a Windows-specific tray tool. Keep release work focused on the
Windows build, Windows packaging, and Windows runtime proof path. Do not add
cross-platform runtime promises or broad portability layers.

The app's live monitor and virtual-desktop behavior is intentionally
Windows-only. Hosted CI can prove build, package, smoke, and checksum behavior,
but interactive tray, multi-monitor, and virtual-desktop proof remains manual on
a real Windows desktop.

## Release Source Of Truth

- `VERSION` is the single release version source.
- `scripts/test-release.ps1` is the release-verification entrypoint.
- `.github/workflows/windows-release.yml` should delegate release checks to
  `scripts/test-release.ps1` instead of duplicating build, test, package, or
  smoke-test logic in YAML. Minimal workflow metadata such as artifact names,
  tag matching, and GitHub release publication can stay in the workflow.
- `README.md`, `CONTRIBUTING.md`, and `docs/windows-install-package-readme.txt`
  must stay aligned with the release gate and packaging contract.
- `THIRD_PARTY_NOTICES.md` must describe every bundled runtime dependency.

## Generated Output

`build/`, `build-win/`, and `tools/**/bin/` / `tools/**/obj/` are generated
output. They may be deleted and regenerated during verification.

Source under `tools/windows_live_desktop_probe` and
`tools/windows_installer_bootstrapper` is release-critical and must remain
visible to Git.

## Safety Rules

- Do not weaken the fail-closed behavior when live desktop control is missing.
- Avoid changing complex Windows hook, tray, monitor, or virtual-desktop logic
  unless the current task requires it.
- Installer changes must be conservative. Validate install paths before stopping
  processes or replacing files.
- Downloaded helper artifacts must stay pinned by URL and SHA-256 hash.
- Manual live-proof scripts should not be described as CI coverage.

## Verification

Use the narrowest meaningful check after each change. Before calling the project
release-ready, run or explicitly account for:

- `./scripts/test-release.ps1 -Mode Hygiene`
- `./scripts/test-release.ps1 -Mode Build`
- `./scripts/test-release.ps1 -Mode Package`
- the relevant manual live-proof scripts when Windows desktop behavior changes

If local tooling is missing, report the missing prerequisite instead of claiming
verification from static inspection.
