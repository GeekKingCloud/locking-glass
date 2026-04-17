LockingGlass for Windows

This package installs the proven background tray build from the live-controller branch.

What this build does
- Keeps selected monitors visually pinned while other monitors follow real Windows virtual desktop switches.
- Launches the same helper-backed live controller path that passed the Windows acceptance proof.
- Fails closed when the live controller cannot start instead of pretending replay or prototype behavior is live.

Current limits
- Windows only.
- Requires at least two monitors and at least two Windows virtual desktops to exercise the pinned-monitor behavior.
- The bundled files in this folder must stay together: LockingGlass.exe, run-live-desktop-probe.ps1, VirtualDesktopAccessor.dll, and the LockingGlass.WindowsLiveDesktopProbe publish output.

How to launch
- Run Start-LockingGlass.cmd to start the background tray app from the installed folder.
- Or run LockingGlass.exe --background directly from this folder.
- Run LockingGlass.exe --install-autostart if you want current-user startup after sign-in.
- Re-running Install-LockingGlass.ps1 refreshes this install by stopping the existing installed LockingGlass runtime and bundled live probe before replacing files.

What this package does not claim
- It is not a replay-only build.
- It is not a proof that every window can be moved safely; windows the live controller cannot classify are skipped and reported instead of guessed.
- It does not treat missing helper assets or unavailable live desktop hooks as success.
