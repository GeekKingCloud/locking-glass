LockingGlass for Windows

This package installs the LockingGlass background tray app.

Audit note
- This file describes the intended installed runtime contract.
- The proof scripts and proof documents in the repository are the verification path for live Windows behavior.

What it does
- Keeps selected monitors visually pinned while other monitors follow real Windows virtual desktop switches.
- Stages destination-workspace windows on a named Locking-Glass virtual desktop so a locked monitor does not push them into another user workspace.
- When you unlock a monitor, it makes a best-effort attempt to return windows that LockingGlass moved for that lock back to their remembered original workspace if that workspace still exists.
- Uses the helper-backed live controller path. Run the proof scripts for current live Windows validation.
- Fails closed when the live controller cannot start instead of pretending replay or prototype behavior is live.

Requirements
- Windows only.
- At least two monitors.
- At least two Windows virtual desktops.
- VirtualDesktopAccessor.dll with the live hook, move, and desktop lifecycle exports bundled beside the app.
- No separate .NET runtime is required for normal installed use; the bundled live desktop probe is published as a self-contained Windows executable.

Files that must stay together
- LockingGlass.exe
- Install-LockingGlass.ps1
- Uninstall-LockingGlass.ps1
- Start-LockingGlass.cmd
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
- Double-click LockingGlass.exe to start the background tray app.
- Run Start-LockingGlass.cmd to start the tray app from this folder.
- Run LockingGlass.exe --version to confirm which build you have.
- Run LockingGlass.exe --self-check to inspect startup diagnostics.
- The installer executable and installer script enable current-user startup after sign-in by default. Use the installer `--no-autostart` flag or installer-script `-NoAutostart` switch for installs that must not write the Run key.
- Run LockingGlass.exe --install-autostart to repair current-user startup manually.
- Re-run Install-LockingGlass.ps1 to refresh or upgrade an existing install. The installer script stops the current installed runtime before replacing files in place.
- Run Uninstall-LockingGlass.ps1, the Start Menu uninstall shortcut, or the release `LockingGlass-Uninstaller.exe` to remove the installed app. User data is preserved unless `-RemoveUserData` or `--remove-user-data` is supplied.
- The supported public upgrade path is to run a newer LockingGlass installer executable over the existing install.

Startup note
- Every app start begins with all present monitors unlocked.
- The tray menu is the only way to lock a monitor for the current app run.
- The session file still remembers monitor identity and review state, but it does not automatically re-lock monitors from a previous run.

Unlock return note
- LockingGlass may create a virtual desktop named Locking-Glass while monitors are locked; it only reuses a staging desktop identity it created during the same app run.
- Remembered original workspaces are tracked only in memory for the current app run.
- Only windows that LockingGlass itself moved successfully are eligible for automatic return on unlock.
- If the remembered workspace no longer exists, the window stays where it is.

Installer note
- Public releases ship `LockingGlass.exe`, `LockingGlass-Installer.exe`, and `LockingGlass-Uninstaller.exe`.
- LockingGlass.exe is the run-once portable app binary and does not install itself.
- The installer and uninstaller executables are wrappers around this same payload and use the bundled PowerShell scripts for the actual install and uninstall logic.
- The installer executable and installer script are upgrade-safe by design: they keep the stable install path and do not move the external session-state file.
- Package smoke tests should pass `--no-autostart` so verification never writes the real user's startup registry entry.
- Custom install directories must name an app-specific LockingGlass folder, not a shared parent directory.

What this package does not claim
- It is not a replay-only build.
- It is not a proof that every window can be moved safely; windows the live controller cannot classify are skipped and reported instead of guessed.
- It does not treat missing helper assets or unavailable live desktop hooks as success.
