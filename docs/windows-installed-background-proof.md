# Windows Installed Background Proof

## Verdict

Installed-path proof passed on the real Windows runtime on 2026-04-16.

## Scope

- This document is evidence for the installed-path behavior exercised on 2026-04-16.
- It proves that the installed bundle reached the same live helper-backed controller path as the repo build for that run.
- It does not automatically prove later runtime behavior added after that run.
- Automated packaging smoke checks are helpful release gates, but they do not replace this manual installed-path proof on a real desktop shell.

## Runtime

- Installed app path: `%LOCALAPPDATA%\Programs\Locking Glass\Locking Glass.exe --background`
- Staged package source: `build/windows-install-stage/Locking Glass/`
- Proof wrapper: `scripts/run-installed-background-proof.ps1`

## Artifacts

- `build/windows-installed-background-proof/proof-state.json`
- `build/windows-installed-background-proof/background.desktop-switch-reports.txt`
- `build/windows-installed-background-proof/background.stdout.txt`
- `build/windows-installed-background-proof/background.stderr.txt`

## What Was Proved

1. The staged install flow packaged the current proven Windows build, installed it under `%LOCALAPPDATA%\Programs\Locking Glass`, and launched the installed executable instead of `build-win/bin/locking_glass.exe`.
2. `proof-state.json` records the installed-path watcher chain:
   - `background_executable`: `%LOCALAPPDATA%\Programs\Locking Glass\Locking Glass.exe`
   - `background_working_directory`: `%LOCALAPPDATA%\Programs\Locking Glass`
   - `installed_watch_script`: `%LOCALAPPDATA%\Programs\Locking Glass\run-live-desktop-probe.ps1`
   - `installed_probe_executable`: `%LOCALAPPDATA%\Programs\Locking Glass\LockingGlass.WindowsLiveDesktopProbe.exe`
   - `installed_helper_dll`: `%LOCALAPPDATA%\Programs\Locking Glass\VirtualDesktopAccessor.dll`
3. The watcher-process evidence inside `proof-state.json` shows the installed tray app spawning PowerShell with the bundled installed script:
   - `... -File "%LOCALAPPDATA%\Programs\Locking Glass\run-live-desktop-probe.ps1" -WatchStream ...`
4. The installed background app reached the same proven live controller path used during acceptance proof: `windows-live-post-message-hook` desktop events plus helper-backed move verification.
5. The installed-path desktop-switch report still shows the proved lock behavior:
   - After the tray lock: `restored locks: 1`, `locked monitors: 1`, `planned moves: 3`
   - Move results were verified immediately by the live helper move path.
   - The unlocked proof window `lg-bg-unlocked-target.txt - Notepad` remained on `Display 3` / `Desktop 2`.
   - After the tray unlock: `restored locks: 0`, `locked monitors: 0`, `planned moves: 0`

## Current Product Behavior

The current installed build also performs a best-effort immediate return on unlock for windows that Locking Glass itself moved while the monitor was locked.

- If the remembered original workspace still exists, Locking Glass tries to move those tracked windows back immediately on unlock.
- If the remembered workspace is gone, the window stays where it is.
- This remembered-workspace state is current-run only and is not persisted into the monitor session file.

## Honest Limits

- The installed build still fails closed when the live helper assets cannot load.
- The proof runtime still reports many skipped Display 2 shell windows as `window desktop could not be resolved safely`, which is expected fail-closed behavior rather than a silent success claim.
- This installed proof was recorded before the immediate unlock-return feature was added, so it does not yet serve as direct installed-path evidence for the remembered-workspace return behavior.
