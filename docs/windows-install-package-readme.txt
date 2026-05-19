LockingGlass for Windows

This package installs the LockingGlass background tray app.

Audit note
- This file describes the intended installed runtime contract.
- The proof scripts and proof documents in the repository are the verification path for live Windows behavior.

What it does
- Keeps selected monitors visually pinned while other monitors follow real Windows virtual desktop switches.
- When you unlock a monitor, it makes a best-effort attempt to return windows that LockingGlass moved for that lock back to their remembered original workspace if that workspace still exists.
- Uses the helper-backed live controller path. Run the proof scripts for current live Windows validation.
- Fails closed when the live controller cannot start instead of pretending replay or prototype behavior is live.

Requirements
- Windows only.
- At least two monitors.
- At least two Windows virtual desktops.
- No separate .NET runtime is required for normal installed use; the bundled live desktop probe is published as a self-contained Windows executable.

Files that must stay together
- LockingGlass.exe
- Install-LockingGlass.ps1
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
- Run LockingGlass.exe --install-autostart if you want current-user startup after sign-in.
- Re-run Install-LockingGlass.ps1 to refresh or upgrade an existing install. The installer script stops the current installed runtime before replacing files in place.
- The supported public upgrade path is to run a newer LockingGlass setup executable over the existing install.

Unlock return note
- Remembered original workspaces are tracked only in memory for the current app run.
- Only windows that LockingGlass itself moved successfully are eligible for automatic return on unlock.
- If the remembered workspace no longer exists, the window stays where it is.

Installer note
- Public releases may also ship as LockingGlass-<version>-setup-x64.exe.
- That setup executable is a wrapper around this same payload and still uses Install-LockingGlass.ps1 for the actual install logic.
- The setup executable and installer script are upgrade-safe by design: they keep the stable install path and do not move the external session-state file.
- Custom install directories must name an app-specific LockingGlass folder, not a shared parent directory.

What this package does not claim
- It is not a replay-only build.
- It is not a proof that every window can be moved safely; windows the live controller cannot classify are skipped and reported instead of guessed.
- It does not treat missing helper assets or unavailable live desktop hooks as success.
