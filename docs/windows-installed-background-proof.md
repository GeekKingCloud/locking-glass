# Windows Installed Background Proof

## Verdict

Installed-path proof passed on the real Windows runtime on 2026-04-16.

## Runtime

- Installed app path: `C:\Users\thete\AppData\Local\Programs\LockingGlass-OctopushProof\LockingGlass.exe --background`
- Staged package source: `build/windows-install-stage/LockingGlass/`
- Proof wrapper: `scripts/run-installed-background-proof.ps1`

## Artifacts

- `build/windows-installed-background-proof/proof-state.json`
- `build/windows-installed-background-proof/background.desktop-switch-reports.txt`
- `build/windows-installed-background-proof/background.stdout.txt`
- `build/windows-installed-background-proof/background.stderr.txt`

## What Was Proved

1. The staged install flow packaged the current proven Windows build, installed it under `%LOCALAPPDATA%\Programs\LockingGlass-OctopushProof`, and launched the installed executable instead of `build-win/bin/locking_glass.exe`.
2. `proof-state.json` records the installed-path watcher chain:
   - `background_executable`: `C:\Users\thete\AppData\Local\Programs\LockingGlass-OctopushProof\LockingGlass.exe`
   - `background_working_directory`: `C:\Users\thete\AppData\Local\Programs\LockingGlass-OctopushProof`
   - `installed_watch_script`: `C:\Users\thete\AppData\Local\Programs\LockingGlass-OctopushProof\run-live-desktop-probe.ps1`
   - `installed_probe_executable`: `C:\Users\thete\AppData\Local\Programs\LockingGlass-OctopushProof\LockingGlass.WindowsLiveDesktopProbe.exe`
   - `installed_helper_dll`: `C:\Users\thete\AppData\Local\Programs\LockingGlass-OctopushProof\VirtualDesktopAccessor.dll`
3. The watcher-process evidence inside `proof-state.json` shows the installed tray app spawning PowerShell with the bundled installed script:
   - `... -File "C:\Users\thete\AppData\Local\Programs\LockingGlass-OctopushProof\run-live-desktop-probe.ps1" -WatchStream ...`
4. The installed background app reached the same proven live controller path used during acceptance proof: `windows-live-post-message-hook` desktop events plus helper-backed move verification.
5. The installed-path desktop-switch report still shows the proved lock behavior:
   - After the tray lock: `restored locks: 1`, `locked monitors: 1`, `planned moves: 3`
   - Move results were verified immediately by the live helper move path.
   - The unlocked proof window `lg-bg-unlocked-target.txt - Notepad` remained on `Display 3` / `Desktop 2`.
   - After the tray unlock: `restored locks: 0`, `locked monitors: 0`, `planned moves: 0`

## Honest Limits

- The installed build still fails closed when the live helper assets cannot load.
- The proof runtime still reports many skipped Display 2 shell windows as `window desktop could not be resolved safely`, which is expected fail-closed behavior rather than a silent success claim.
