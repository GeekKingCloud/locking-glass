# LockingGlass Style Guide

LockingGlass favors boring, explicit code over clever abstractions. Keep changes
small and easy to audit.

## C++ Style

- Use C++20 and keep the existing two-space indentation.
- Keep public interfaces under `include/locking_glass/...` and implementations
  under the matching `src/...` area.
- Preserve the current namespace layout and `locking_glass::{core,platform,integration}`
  responsibility split.
- Keep domain policy in `src/core`, Win32 and shell integration in
  `src/platform`, and Windows virtual-desktop/helper integration in
  `src/integration`.
- Prefer explicit structs and named result types over boolean soup.
- Do not add comments that restate the next line of code. Add comments only for
  non-obvious Windows API behavior, release invariants, or fail-closed decisions.
- Keep user-facing strings truthful about replay, live proof, and fail-closed
  behavior.

## PowerShell Style

- Set `$ErrorActionPreference = 'Stop'` in scripts that perform release,
  install, or proof work.
- Use explicit parameters and validate dangerous filesystem or process actions
  before performing them.
- Prefer `Join-Path`, `Test-Path`, `Resolve-Path`, and typed .NET path helpers
  over string-built paths.
- Keep release outputs under `build/`; keep Windows binary outputs under
  `build-win/`.
- Do not hide machine-specific setup in repo config. Check for required tools
  and fail with a clear message.

## C# Helper Style

- Helper projects exist to support the Windows release path. Keep them small,
  self-contained, and explicit.
- Use nullable annotations and clear exception messages.
- Treat extraction, process execution, and filesystem writes as security-sensitive
  boundaries.
- Do not add background update behavior, network polling, or privileged install
  behavior without updating the docs and release gate.

## Tests And Proof

- Automated tests should verify behavior through the public seams where possible.
- Keep replay/scripted tests honest: they prove policy and wiring, not the live
  Windows desktop hook.
- Manual live-proof docs are evidence snapshots. Re-run proof scripts after
  behavior changes instead of treating old proof notes as current.

## Documentation

- Keep the project described as Windows-specific.
- Keep README, CONTRIBUTING, install package notes, and release workflow wording
  aligned when packaging behavior changes.
- Prefer direct release commands and concrete prerequisites over broad prose.
