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

`make test` now also replays a scripted tray session: it opens the tray UI, hovers monitors to trigger the identify overlay, clears that hover while the menu stays open, locks a monitor, simulates that monitor disconnecting, simulates it reconnecting beside a brand-new monitor, and verifies that the saved lock is restored while only the new hardware emits a review prompt.

`make test` also replays scripted virtual desktop switches through `LOCKING_GLASS_DESKTOP_SCRIPT`, proving that locked-monitor windows are swapped across desktops while unlocked monitors keep following the normal desktop change.

`build/bin/locking_glass --watch-monitors` prints monitor refresh events. On Windows it waits for live `WM_DISPLAYCHANGE` updates; on non-Windows hosts set `LOCKING_GLASS_MONITOR_SCRIPT` to a scripted event file so the same reporting path can be verified locally. When a refresh introduces brand-new monitors, the report now includes the review prompt text that the tray flow will surface.

`build/bin/locking_glass --watch-virtual-desktops` now has two explicit modes. On Windows, if `LOCKING_GLASS_DESKTOP_SCRIPT` is unset, it launches the live helper-backed watch path, waits for two real desktop-switch notifications, and prints the controller report with source and target desktop context from the live shell event stream. On non-Windows hosts, or whenever you intentionally set `LOCKING_GLASS_DESKTOP_SCRIPT`, it replays the scripted switch file for deterministic policy verification.

`make prototype` runs `locking_glass --prototype-windows-apis`, which prints the Windows integration boundary contract and a simple interaction trace for virtual desktop control plus monitor enumeration.

For the real Windows hook proof, run `scripts/run-live-desktop-probe.ps1` from a Windows shell. That wrapper downloads the current `VirtualDesktopAccessor.dll` release when needed, builds `tools/windows_live_desktop_probe`, exercises the live move path on a real top-level probe window, and records live desktop-switch notifications without using `LOCKING_GLASS_DESKTOP_SCRIPT`.

## Autostart

- `locking_glass --install-autostart` writes a `LockingGlass` entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- The Run entry launches the binary with `--background`, which is wired to a hidden Win32 message loop on Windows so the process can stay resident after sign-in without foreground UI.
- On Windows, `--background` now registers a status-aware `Shell_NotifyIconW` tray icon, refreshes monitor state on `WM_DISPLAYCHANGE`, and opens a popup monitor-toggle menu when the tray icon is clicked.
- `locking_glass --self-check` prints the exact autostart command that the Windows registration path will install, which keeps the contract host-verifiable from Linux workers.

## Tray Session

- The Win32 background session keeps a hidden tool window alive, renders a custom tray icon that changes between unlocked, mixed, locked, and review-needed states, and uses the same model to drive the menu header and tooltip copy.
- Clicking the tray icon rebuilds the current monitor list from `MonitorGateway`, reconciles it through `SessionStore`, and shows a structured popup menu with a title, live summary line, hover guidance, one rendered padlock icon per active monitor entry, and layout metadata in each monitor label (`resolution @ x,y`, plus `primary` when applicable) so identical panels stay distinguishable before hover.
- Monitor entries now use a filled emerald padlock for locked displays, a hollow slate padlock for unlocked displays, and an amber review-badged padlock for newly added monitors that still need confirmation.
- Hovering a monitor entry now paints a full-monitor topmost highlight overlay with a centered identify card, making it clear which physical screen is being referenced before toggling; the overlay text also echoes the monitor's placement metadata for easier verification in scripted runs.
- Selecting a monitor entry toggles its persisted lock state immediately, clears any outstanding review requirement for that monitor, and reopens the tray menu with refreshed lock counts and padlock indicators so the change is visible without another click.
- Startup, `WM_DISPLAYCHANGE`, and manual tray refreshes now emit a lightweight review prompt when brand-new monitors appear, while disconnected monitors stay silent and simply retain their saved lock state until they return.
- The tray tooltip summarizes the current lock count and pending-review count so topology changes are visible even before the menu is opened, and the scripted event model exposes the same tray/menu and per-monitor padlock state for host verification.
- Win32 padlock menu bitmaps are recreated from the current `SM_CXMENUCHECK` and `SM_CYMENUCHECK` metrics on each refresh so the indicators stay aligned with system menu sizing across DPI scales and theme variants.
- On non-Windows hosts, set `LOCKING_GLASS_TRAY_SCRIPT` to a scripted event file if you want to replay tray clicks, hover-identify steps, explicit `hover-clear` transitions, disconnect/reconnect cycles, and new-monitor review prompts through the same `--background` code path for local verification.

## Session State

- Monitor lock state is stored by `core::SessionStore` in a local session file.
- The default path is `%LOCALAPPDATA%\\LockingGlass\\monitor-session-state.tsv` on Windows and `$XDG_STATE_HOME/locking-glass/monitor-session-state.tsv` or `$HOME/.local/state/locking-glass/monitor-session-state.tsv` on non-Windows hosts.
- Set `LOCKING_GLASS_SESSION_PATH` to override the storage file during tests or local inspection.
- The on-disk format is versioned: the first row is `version<TAB>1`, followed by one `monitor` row per saved monitor record.
- The model keeps disconnected monitors in the saved session, restores their lock state when they return, and adds brand-new monitors as unlocked records that still require user confirmation.
- Review prompts are tied to the refresh that first discovers a brand-new monitor, so reopening the tray menu later does not keep re-emitting the same add-monitor notification.
- If the app finds malformed or unsupported session data on startup, it treats the file as rejected input, copies the original contents to `<session-path>.invalid`, and writes a clean snapshot based on the currently visible monitors.

## Desktop Locking

- `src/core/monitor_locking.cpp` builds the per-switch policy: it restores persisted monitor locks, identifies which live monitors are locked, and plans only the top-level movable windows that need to swap desktops to keep a locked monitor visually fixed.
- `src/integration/virtual_desktop_controller.cpp` owns both the explicit replay seam and the Windows live-watch bridge. On Windows it prefers the real helper-backed event stream when `LOCKING_GLASS_DESKTOP_SCRIPT` is unset, and only uses replay when that env var is configured on purpose.
- `docs/windows-live-desktop-hook.md` records the chosen live boundary: `VirtualDesktopAccessor.dll:RegisterPostMessageHook` for real desktop-switch notifications and `VirtualDesktopAccessor.dll:MoveWindowToDesktopNumber` for isolated top-level window moves.
- The Windows proof probe for ticket `#15` also showed that direct `IVirtualDesktopManager.MoveWindowToDesktop` is not the supported move path on this runtime; the helper move export succeeded while COM remained useful for readiness and desktop-id verification.
- In the replay format, each `event	desktop-switch	<trigger>	<from>	<to>` block can include `monitor` rows plus `window` rows (`window_id`, `title`, `monitor_id`, `monitor_label`, `desktop_id`, `is_top_level`, `can_move`).
- `LOCKING_GLASS_DESKTOP_SCRIPT` remains a replay seam for local policy verification only. It is not valid completion evidence for the live Windows desktop hook path.
- `scripts/run-live-desktop-probe.ps1` and `tools/windows_live_desktop_probe/Program.cs` now serve two roles on Windows: the existing proof probe for ticket `#15`, and the live watch backend that `locking_glass --watch-virtual-desktops` launches when replay is not explicitly requested.

## Monitor Enumeration

- `src/platform/monitor_gateway.cpp` enumerates live monitors with `EnumDisplayMonitors` and `GetMonitorInfoW`, then enriches each screen with `QueryDisplayConfig` and `DisplayConfigGetDeviceInfo` data when Windows exposes a persistent device path.
- The exported monitor identity envelope is `stable_id`, `device_path`, `edid_serial` when available, `display_name`, bounds, and primary flag. The current implementation prefers the Windows device path as the persistent id and falls back to an adapter-target signature when Windows does not provide one.
- Monitor labels are assigned from the live screen layout after sorting by top-left position, so tray-facing numbering stays tied to the current topology rather than callback order.
- `src/platform/monitor_watcher.cpp` drives startup and topology refresh events. On Windows it listens for `WM_DISPLAYCHANGE`; on non-Windows hosts it can replay scripted events through `LOCKING_GLASS_MONITOR_SCRIPT`.

## Windows Notes

- `src/platform/monitor_gateway.cpp` is where Win32 monitor enumeration is wired with `EnumDisplayMonitors`, `GetMonitorInfoW`, `QueryDisplayConfig`, and `DisplayConfigGetDeviceInfo`.
- `src/platform/monitor_watcher.cpp` is where foreground monitor-watch sessions react to startup and `WM_DISPLAYCHANGE` refresh events.
- `src/integration/windows_api_probe.cpp` is where tray and monitor entry points are probed with `user32.dll`, `shell32.dll`, COM initialization, and the base `IVirtualDesktopManager` seam.
- `src/integration/virtual_desktop_controller.cpp` is where desktop switch replay, live-helper availability reporting, and fail-closed desktop-locking diagnostics are isolated.
- `src/integration/windows_virtual_desktop_surface.cpp` centralizes the Windows-only readiness probe for `IVirtualDesktopManager` plus the `VirtualDesktopAccessor.dll` exports LockingGlass requires before it can claim the live hook path is available.
- `src/platform/background_session.cpp` is where background launches enter the hidden Win32 tray session, handle tray clicks, and expose the scripted non-Windows verification path.
- `src/integration/autostart.cpp` is where current-user Run-key registration is built and installed for sign-in autostart.
- `src/integration/ffmpeg_probe.cpp` uses runtime loading instead of static linkage so future Windows packaging can decide where FFmpeg DLLs live without changing the call site contract.
- `src/core/session_store.cpp` is where monitor lock state is serialized, restored, and reconciled against live monitor topology.
- `src/core/tray_ui.cpp` is where active monitor session state is projected into the tray menu model, including the per-monitor padlock icon state, and where tray-driven lock toggles are persisted.
- `scripts/run-live-desktop-probe.ps1` is the Windows-side wrapper that downloads the maintained helper DLL release, runs the .NET probe, and writes proof logs under `build/windows-live-desktop-probe/`.

## Windows Integration Boundaries

- `virtual-desktop-control` owns COM readiness, supported `IVirtualDesktopManager` access, and the isolated `VirtualDesktopAccessor.dll` seam used for live desktop switch notifications and top-level window moves.
- `virtual-desktop-control` does not choose which monitors or windows should move. Core policy decides that, then passes only eligible top-level windows into the boundary.
- If `VirtualDesktopAccessor.dll` or its required exports are missing, the desktop-control boundary must report `unavailable` and LockingGlass must fail closed instead of falling back to replay or guessed shell state.
- `monitor-enumeration` owns `EnumDisplayMonitors`, `GetMonitorInfoW`, `QueryDisplayConfig`, `DisplayConfigGetDeviceInfo`, and `WM_DISPLAYCHANGE` handling. It reports live monitor geometry and identity fields without persisting state or deciding lock behavior.
- `monitor-enumeration` emits the identity envelope consumed by `SessionStore`: stable id, device path, EDID serial when available, display name, bounds, and primary flag. Ambiguity resolution stays in the core/session layer.
- `locking_glass --prototype-windows-apis` prints both boundaries plus the expected interaction flow: startup probe, live monitor scan, topology refresh on `WM_DISPLAYCHANGE`, desktop switch notification, and post-policy window moves.
