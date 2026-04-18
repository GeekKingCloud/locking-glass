# Windows Live Desktop Hook Path

LockingGlass's proof path for real Windows virtual desktop switching is now explicitly separated from the replay seam.

## Chosen Live Boundary

- Live desktop notifications come from `VirtualDesktopAccessor.dll:RegisterPostMessageHook`, delivered to a hidden or tool-window message loop on the Windows runtime.
- Live window moves use `VirtualDesktopAccessor.dll:MoveWindowToDesktopNumber` on already-selected top-level windows after the core lock policy decides which windows must stay visually pinned.
- That same helper-backed move path is also used for the best-effort immediate unlock return when LockingGlass sends tracked windows back to their remembered original workspace.
- `IVirtualDesktopManager` remains part of the Windows capability probe and a useful verification seam, but it is not treated as sufficient proof of the live hook on its own because it does not provide the desktop-switch notification stream LockingGlass needs.
- The live move-path proof on the real Windows runtime showed `IVirtualDesktopManager.MoveWindowToDesktop` returning access denied for the disposable probe target, while the helper move export succeeded and the COM desktop-id query confirmed the result. That makes the helper move export the concrete supported move path for LockingGlass.

## Fail-Closed Contract

LockingGlass must mark live desktop locking as unavailable instead of guessing when any of these conditions apply:

- `VirtualDesktopAccessor.dll` is missing or cannot be loaded.
- The required helper exports are missing: `RegisterPostMessageHook`, `UnregisterPostMessageHook`, `GetCurrentDesktopNumber`, `GoToDesktopNumber`, `MoveWindowToDesktopNumber`, or `GetWindowDesktopNumber`.
- The Windows runtime reports fewer than two virtual desktops, so a real switch cannot be observed.
- The live hook registers but no real desktop-switch notifications arrive before timeout.
- The move-path exercise cannot confirm that a top-level probe window moved to the requested desktop and back.

## Replay Separation

- `LOCKING_GLASS_DESKTOP_SCRIPT` stays as a local replay seam for policy and formatting checks.
- Replay output is not valid completion evidence for the core feature, and downstream tickets must not treat it as proof of the live Windows path.

## Evidence Hierarchy

- The automated `wiring_test` harness proves host-side policy and scripted seam behavior, including unlock-return logic.
- GitHub Actions package smoke checks prove that the built setup executable can unpack the expected payload and launch the packaged app for diagnostic checks.
- The PowerShell proof scripts prove the live helper-backed Windows path on a real desktop shell.
- Dated proof notes under `docs/` are audit artifacts for specific runs. When runtime behavior changes, they must either be updated with a new proof run or clearly marked as predating the change.

GitHub-hosted CI does not run the live proof scripts because it lacks the interactive Windows shell, multi-monitor topology, virtual desktop state, and tray interaction required for meaningful evidence.

## Probe Command

Run the Windows proof probe from a Windows shell:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-live-desktop-probe.ps1
```

The wrapper downloads the current `VirtualDesktopAccessor.dll` Windows 11 release if it is absent, builds the probe with `dotnet`, captures a move-path exercise on a real top-level probe window, and records at least two live desktop-switch notifications to a log under `build\windows-live-desktop-probe\`.
