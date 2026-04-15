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

`make test` also manually edits the stored session data to verify that malformed monitor rows and unsupported format versions are rejected, backed up to a `.invalid` file, and rebuilt from live monitor state without crashing the app.

`make test` now also replays a scripted tray session: it opens the tray UI, hovers monitors to trigger the identify overlay, locks a monitor, simulates that monitor disconnecting, simulates it reconnecting beside a brand-new monitor, and verifies that the saved lock is restored while only the new hardware emits a review prompt.

`build/bin/locking_glass --watch-monitors` prints monitor refresh events. On Windows it waits for live `WM_DISPLAYCHANGE` updates; on non-Windows hosts set `LOCKING_GLASS_MONITOR_SCRIPT` to a scripted event file so the same reporting path can be verified locally. When a refresh introduces brand-new monitors, the report now includes the review prompt text that the tray flow will surface.

`make prototype` runs `locking_glass --prototype-windows-apis`, which prints the Windows integration boundary contract and a simple interaction trace for virtual desktop control plus monitor enumeration.

## Autostart

- `locking_glass --install-autostart` writes a `LockingGlass` entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- The Run entry launches the binary with `--background`, which is wired to a hidden Win32 message loop on Windows so the process can stay resident after sign-in without foreground UI.
- On Windows, `--background` now registers a status-aware `Shell_NotifyIconW` tray icon, refreshes monitor state on `WM_DISPLAYCHANGE`, and opens a popup monitor-toggle menu when the tray icon is clicked.
- `locking_glass --self-check` prints the exact autostart command that the Windows registration path will install, which keeps the contract host-verifiable from Linux workers.

## Tray Session

- The Win32 background session keeps a hidden tool window alive, renders a custom tray icon that changes between unlocked, mixed, locked, and review-needed states, and uses the same model to drive the menu header and tooltip copy.
- Clicking the tray icon rebuilds the current monitor list from `MonitorGateway`, reconciles it through `SessionStore`, and shows a structured popup menu with a title, live summary line, hover guidance, and one padlock-prefixed entry per active monitor.
- Hovering a monitor entry shows a small topmost overlay label on that physical display so the current menu selection can be matched to the real monitor before toggling.
- Selecting a monitor entry toggles its persisted lock state immediately and clears any outstanding review requirement for that monitor.
- Startup, `WM_DISPLAYCHANGE`, and manual tray refreshes now emit a lightweight review prompt when brand-new monitors appear, while disconnected monitors stay silent and simply retain their saved lock state until they return.
- The tray tooltip summarizes the current lock count and pending-review count so topology changes are visible even before the menu is opened, and the scripted event model exposes the same icon/menu state for host verification.
- On non-Windows hosts, set `LOCKING_GLASS_TRAY_SCRIPT` to a scripted event file if you want to replay tray clicks, hover-identify steps, disconnect/reconnect cycles, and new-monitor review prompts through the same `--background` code path for local verification.

## Session State

- Monitor lock state is stored by `core::SessionStore` in a local session file.
- The default path is `%LOCALAPPDATA%\\LockingGlass\\monitor-session-state.tsv` on Windows and `$XDG_STATE_HOME/locking-glass/monitor-session-state.tsv` or `$HOME/.local/state/locking-glass/monitor-session-state.tsv` on non-Windows hosts.
- Set `LOCKING_GLASS_SESSION_PATH` to override the storage file during tests or local inspection.
- The on-disk format is versioned: the first row is `version<TAB>1`, followed by one `monitor` row per saved monitor record.
- The model keeps disconnected monitors in the saved session, restores their lock state when they return, and adds brand-new monitors as unlocked records that still require user confirmation.
- Review prompts are tied to the refresh that first discovers a brand-new monitor, so reopening the tray menu later does not keep re-emitting the same add-monitor notification.
- If the app finds malformed or unsupported session data on startup, it treats the file as rejected input, copies the original contents to `<session-path>.invalid`, and writes a clean snapshot based on the currently visible monitors.

## Monitor Enumeration

- `src/platform/monitor_gateway.cpp` enumerates live monitors with `EnumDisplayMonitors` and `GetMonitorInfoW`, then enriches each screen with `QueryDisplayConfig` and `DisplayConfigGetDeviceInfo` data when Windows exposes a persistent device path.
- The exported monitor identity envelope is `stable_id`, `device_path`, `edid_serial` when available, `display_name`, bounds, and primary flag. The current implementation prefers the Windows device path as the persistent id and falls back to an adapter-target signature when Windows does not provide one.
- Monitor labels are assigned from the live screen layout after sorting by top-left position, so tray-facing numbering stays tied to the current topology rather than callback order.
- `src/platform/monitor_watcher.cpp` drives startup and topology refresh events. On Windows it listens for `WM_DISPLAYCHANGE`; on non-Windows hosts it can replay scripted events through `LOCKING_GLASS_MONITOR_SCRIPT`.

## Windows Notes

- `src/platform/monitor_gateway.cpp` is where Win32 monitor enumeration is wired with `EnumDisplayMonitors`, `GetMonitorInfoW`, `QueryDisplayConfig`, and `DisplayConfigGetDeviceInfo`.
- `src/platform/monitor_watcher.cpp` is where foreground monitor-watch sessions react to startup and `WM_DISPLAYCHANGE` refresh events.
- `src/integration/windows_api_probe.cpp` is where tray and monitor entry points are probed with `user32.dll`, `shell32.dll`, COM initialization, and the base `IVirtualDesktopManager` seam.
- `src/platform/background_session.cpp` is where background launches enter the hidden Win32 tray session, handle tray clicks, and expose the scripted non-Windows verification path.
- `src/integration/autostart.cpp` is where current-user Run-key registration is built and installed for sign-in autostart.
- `src/integration/ffmpeg_probe.cpp` uses runtime loading instead of static linkage so future Windows packaging can decide where FFmpeg DLLs live without changing the call site contract.
- `src/core/session_store.cpp` is where monitor lock state is serialized, restored, and reconciled against live monitor topology.
- `src/core/tray_ui.cpp` is where active monitor session state is projected into the tray menu model and where tray-driven lock toggles are persisted.

## Windows Integration Boundaries

- `virtual-desktop-control` owns COM initialization, supported `IVirtualDesktopManager` access, and the isolated helper seam used for desktop switch notifications or forced window moves.
- `virtual-desktop-control` does not choose which monitors or windows should move. Core policy decides that, then passes only eligible top-level windows into the boundary.
- `monitor-enumeration` owns `EnumDisplayMonitors`, `GetMonitorInfoW`, `QueryDisplayConfig`, `DisplayConfigGetDeviceInfo`, and `WM_DISPLAYCHANGE` handling. It reports live monitor geometry and identity fields without persisting state or deciding lock behavior.
- `monitor-enumeration` emits the identity envelope consumed by `SessionStore`: stable id, device path, EDID serial when available, display name, bounds, and primary flag. Ambiguity resolution stays in the core/session layer.
- `locking_glass --prototype-windows-apis` prints both boundaries plus the expected interaction flow: startup probe, live monitor scan, topology refresh on `WM_DISPLAYCHANGE`, desktop switch notification, and post-policy window moves.
