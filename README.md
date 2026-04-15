# LockingGlass

Initial scaffold for a Windows-resident tray utility that will keep selected monitors pinned while other displays continue following Windows virtual desktop changes.

## Layout

- `include/locking_glass/core`: runtime assembly and startup diagnostics.
- `include/locking_glass/core/session_store.h`: persisted monitor lock session model and topology reconciliation API.
- `include/locking_glass/platform`: monitor-facing abstractions and Win32 monitor enumeration seam.
- `include/locking_glass/integration`: FFmpeg, Windows API, and Windows autostart probes.
- `src`: application entrypoint and default adapter implementations.
- `tests`: smoke coverage plus a fake FFmpeg shared library used to verify the loader path.

## Build

```bash
make
make test
make smoke
make prototype
```

`make test` injects `build/lib/libfakeavutil.so` through `LOCKING_GLASS_FFMPEG_LIBRARY` so the FFmpeg seam is verified without requiring system FFmpeg packages.

`make test` also simulates a restart by saving monitor lock state to a temp session file, reloading it through a fresh `SessionStore`, and reconciling add/remove monitor topology changes.

`make prototype` runs `locking_glass --prototype-windows-apis`, which prints the Windows integration boundary contract and a simple interaction trace for virtual desktop control plus monitor enumeration.

## Autostart

- `locking_glass --install-autostart` writes a `LockingGlass` entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- The Run entry launches the binary with `--background`, which is wired to a hidden Win32 message loop on Windows so the process can stay resident after sign-in without foreground UI.
- `locking_glass --self-check` prints the exact autostart command that the Windows registration path will install, which keeps the contract host-verifiable from Linux workers.

## Session State

- Monitor lock state is stored by `core::SessionStore` in a local session file.
- The default path is `%LOCALAPPDATA%\\LockingGlass\\monitor-session-state.tsv` on Windows and `$XDG_STATE_HOME/locking-glass/monitor-session-state.tsv` or `$HOME/.local/state/locking-glass/monitor-session-state.tsv` on non-Windows hosts.
- Set `LOCKING_GLASS_SESSION_PATH` to override the storage file during tests or local inspection.
- The model keeps disconnected monitors in the saved session, restores their lock state when they return, and adds brand-new monitors as unlocked records that still require user confirmation.

## Windows Notes

- `src/platform/monitor_gateway.cpp` is where Win32 monitor enumeration is wired with `EnumDisplayMonitors` and `GetMonitorInfoW`.
- `src/integration/windows_api_probe.cpp` is where tray and monitor entry points are probed with `user32.dll`, `shell32.dll`, COM initialization, and the base `IVirtualDesktopManager` seam.
- `src/platform/background_session.cpp` is where background launches enter a hidden Win32 message loop.
- `src/integration/autostart.cpp` is where current-user Run-key registration is built and installed for sign-in autostart.
- `src/integration/ffmpeg_probe.cpp` uses runtime loading instead of static linkage so future Windows packaging can decide where FFmpeg DLLs live without changing the call site contract.
- `src/core/session_store.cpp` is where monitor lock state is serialized, restored, and reconciled against live monitor topology.

## Windows Integration Boundaries

- `virtual-desktop-control` owns COM initialization, supported `IVirtualDesktopManager` access, and the isolated helper seam used for desktop switch notifications or forced window moves.
- `virtual-desktop-control` does not choose which monitors or windows should move. Core policy decides that, then passes only eligible top-level windows into the boundary.
- `monitor-enumeration` owns `EnumDisplayMonitors`, `GetMonitorInfoW`, and `WM_DISPLAYCHANGE` handling. It reports live monitor geometry and identity fields without persisting state or deciding lock behavior.
- `monitor-enumeration` emits the identity envelope consumed by `SessionStore`: stable id, device path, EDID serial when available, display name, bounds, and primary flag. Ambiguity resolution stays in the core/session layer.
- `locking_glass --prototype-windows-apis` prints both boundaries plus the expected interaction flow: startup probe, live monitor scan, topology refresh on `WM_DISPLAYCHANGE`, desktop switch notification, and post-policy window moves.
