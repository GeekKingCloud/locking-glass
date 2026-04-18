# Windows Monitor Pinning Acceptance Proof

Date: 2026-04-16

Verdict: fully accomplished

## Scope

- This document is evidence for the monitor-pinning behavior that was exercised on 2026-04-16.
- It is not an evergreen proof for later behavior added after that run.
- The artifact-backed source of truth for this document is the proof output listed below, not recollection or replay-only tests.
- Automated host/package checks in CI are complementary only; they do not replace this kind of manual live Windows proof.

## Setup

- Runtime: real Windows desktop shell on a three-monitor setup.
- App under test: `build-win/bin/locking_glass.exe --background`.
- Locked monitor during proof: `Display 2`.
- Unlocked monitor during proof: `Display 3`.
- Desktop switching method: adjacent `Win+Ctrl+Left/Right` hotkeys driven by the proof harness and observed through the live Windows post-message hook.

## Artifacts

- `build/windows-live-background-proof/proof-state.json`
- `build/windows-live-background-proof/background.desktop-switch-reports.txt`
- `build/windows-live-background-proof/background.stdout.txt`
- `build/windows-live-background-proof/background.stderr.txt`

## Acceptance Flow Executed

1. Start the real background tray app with the live controller enabled.
2. Create proof windows on the locked and unlocked monitors across two real Windows desktops.
3. Open the real Win32 tray popup and lock `Display 2`.
4. Switch desktops with normal Windows hotkeys.
5. Confirm the locked monitor is restored while the unlocked monitor continues following the switch.
6. Unlock `Display 2` from the tray and repeat the switch to confirm the pinning policy disengages.

## Current Product Behavior

The current app also performs a best-effort immediate return on unlock for windows that LockingGlass itself moved while the monitor was locked.

- If the remembered original workspace still exists, LockingGlass tries to move those tracked windows back immediately when the monitor is unlocked.
- If the remembered workspace no longer exists, the window stays where it is.
- This remembered-workspace state lives only in memory for the current app run and is not written to the session file.

## Evidence

- `proof-state.json` confirms the tray path launched the live watcher chain from the background app itself:
  - `cmd.exe /c ... locking-glass-live-desktop-watch-568.cmd`
  - `powershell.exe ... run-live-desktop-probe.ps1 -WatchStream ... -RequiredEvents 0 -TimeoutSeconds 0 -NoAutoCycle -SkipMoveExercise`
- `background.desktop-switch-reports.txt` captured five live desktop-switch policy reports on the target runtime.
- After the tray lock was applied, the report for `Desktop 1 -> Desktop 2` shows:
  - `restored locks: 1`
  - `locked monitors: 1`
  - `planned moves: 2`
  - `Locked monitors: Display 2`
- The same locked-monitor report records real Display 2 windows being moved back to the pinned desktop:
  - a sample browser window on `Display 2`: `Desktop 2 -> Desktop 1 : moved`
  - a sample app window on `Display 2`: `Desktop 2 -> Desktop 1 : moved`
- That locked-monitor report also records an unlocked proof window staying on the switched desktop:
  - `lg-bg-unlocked-target.txt - Notepad [Display 3] on Desktop 2`
- The return switch with the lock still enabled shows the inverse move on `Display 2`, proving the pinned monitor stayed fixed again while the unlocked monitor remained on the switched desktop.
- After unlocking from the tray, the final `Desktop 1 -> Desktop 2` report returns to:
  - `restored locks: 0`
  - `locked monitors: 0`
  - `planned moves: 0`

## Remaining Gaps

- No release-blocking product gap was observed in this acceptance run for the pinning behavior that was exercised on 2026-04-16.
- Non-blocking proof caveat: the helper-only `switch_states.desktops` snapshot inside `proof-state.json` still collapses every proof window onto desktop `1` on this host. The authoritative acceptance evidence is `background.desktop-switch-reports.txt`, not that helper snapshot.
- This dated proof predates the later immediate unlock-return feature. Re-run the live background proof to capture direct Windows evidence for the remembered-workspace return path.
