Locking Glass for Windows

This package installs the Locking Glass background tray app.

Audit note
- This file describes the intended installed runtime contract.
- The proof scripts and proof documents in the repository are the verification path for live Windows behavior.

What it does
- Keeps selected monitors visually pinned while other monitors follow real Windows virtual desktop switches.
- Stages destination-workspace windows on a named Locking Glass virtual desktop so a locked monitor does not push them into another user workspace.
- When you unlock a monitor, it makes a best-effort attempt to return the current top-level windows on that borrowed monitor back to its remembered original workspace if that workspace still exists.
- Uses the helper-backed live controller path. Run the proof scripts for current live Windows validation.
- Fails closed when the live controller cannot start instead of pretending replay or prototype behavior is live.

Requirements
- Windows only.
- At least two monitors.
- At least two Windows virtual desktops.
- The pinned VirtualDesktopAccessor.dll with the expected SHA-256 plus the live hook, move, and desktop lifecycle exports bundled beside the app.
- No separate .NET runtime is required for normal installed use; the bundled live desktop probe is published as a self-contained Windows executable.

Files that must stay together
- Locking Glass.exe
- Install-LockingGlass.ps1
- Uninstall-LockingGlass.ps1
- Start-LockingGlass.cmd legacy launcher
- run-live-desktop-probe.ps1
- resolve-virtual-desktop-helper.ps1
- VirtualDesktopAccessor.dll
- VERSION.txt
- README.txt
- LICENSE.txt
- THIRD_PARTY_NOTICES.txt
- DOTNET_RUNTIME_LICENSE.txt
- DOTNET_RUNTIME_THIRD_PARTY_NOTICES.txt
- LOCKING_GLASS_PAYLOAD_MANIFEST.txt
- LockingGlass.WindowsLiveDesktopProbe publish output

How to use it
- Double-click Locking Glass.exe to start the background tray app.
- The Start Menu shortcut launches Locking Glass.exe directly with `--background`; it does not go through a command-window wrapper.
- Start-LockingGlass.cmd is retained only as a legacy folder launcher.
- Left-click the tray icon to open the monitor lock menu.
- Right-click the tray icon to open the management menu with refresh and exit commands.
- Run Locking Glass.exe --version to confirm which build you have.
- Run Locking Glass.exe --self-check to inspect startup diagnostics.
- The installer executable and installer script enable current-user startup after sign-in by default. Use the installer `--no-autostart` flag or installer-script `-NoAutostart` switch for installs that must not write the Run key.
- The installer launches Locking Glass after install by default. Use the installer `--no-launch-after-install` flag or installer-script `-NoLaunchAfterInstall` switch for verification runs that should not leave the tray app running.
- Run Locking Glass.exe --install-autostart to repair current-user startup manually.
- Run Locking Glass.exe --remove-autostart to remove current-user startup without uninstalling.
- Re-run Install-LockingGlass.ps1 to refresh or upgrade an existing install. The installer script stops the current installed runtime before replacing files in place.
- Use Windows Add or Remove Programs, run Uninstall-LockingGlass.ps1, or use the Start Menu uninstall shortcut to remove the installed app. User data is preserved unless `-RemoveUserData` is supplied.
- The supported public upgrade path is to run a newer public Locking Glass Installer.exe executable over the existing install.

Startup note
- Every app start begins with all present monitors unlocked.
- The left-click tray monitor menu is the only way to lock a monitor for the current app run.
- The session file still remembers monitor identity and review state, but it does not automatically re-lock monitors from a previous run.

Unlock return note
- Locking Glass may create a virtual desktop named Locking Glass while monitors are locked; it only reuses a staging desktop identity it created during the same app run.
- If you switch onto that Locking Glass desktop, parked workspace windows are restored to their own remembered desktops. The locked monitor content can continue following until you unlock it.
- Locking Glass removes that staging desktop when it is empty and still matches the identity it created.
- Remembered original workspaces are tracked only in memory for the current app run.
- Unlock return works at the top-level window level. It does not merge browser tabs or restore browser tab groups inside a process.
- Windows that were moved by Locking Glass and current movable top-level windows on the borrowed monitor are eligible for automatic return on unlock.
- Same-monitor windows on other virtual desktops are left alone instead of being stolen into the remembered workspace.
- If the remembered workspace no longer exists, the window stays where it is.

Installer note
- Public releases ship `Locking Glass Installer.exe` and `Locking Glass.exe`.
- The public installer embeds this whole payload, extracts it to a temporary setup directory, runs the bundled installer script, enables current-user startup by default, and launches the installed app by default.
- The public installer registers a current-user Add or Remove Programs entry for uninstall.
- The public one-time runner embeds this same payload, extracts it to a temporary directory, starts the app for the current session, and does not install startup.
- The public installer and installer script are upgrade-safe by design: they keep the stable install path and do not move the external session-state file.
- Package smoke tests should pass `--no-autostart` so verification never writes the real user's startup registry entry.
- Custom install directories must name an app-specific Locking Glass folder, not a shared parent directory.
- The uninstaller refuses to remove an install directory that is missing the Locking Glass payload manifest or app executable.

What this package does not claim
- It is not a replay-only build.
- It is not a proof that every window can be moved safely; windows the live controller cannot classify are skipped and reported instead of guessed.
- It does not treat missing helper assets or unavailable live desktop hooks as success.
