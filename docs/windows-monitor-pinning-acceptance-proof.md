# Windows Monitor Pinning Acceptance Proof

Date: 2026-04-16

Verdict: fully accomplished

## Setup

- Runtime: real Windows desktop shell on a three-monitor setup.
- App under test: `build-win/bin/locking_glass.exe --background`.
- Locked monitor during proof: `Display 2` (`LS28AG700N`).
- Unlocked monitor during proof: `Display 3` (`ROG PG278QR`).
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
  - `level5-development-chat ... [Display 2] Desktop 2 -> Desktop 1 : moved`
  - `Unauthorized - Brave [Display 2] Desktop 2 -> Desktop 1 : moved`
- That locked-monitor report also records an unlocked proof window staying on the switched desktop:
  - `lg-bg-unlocked-target.txt - Notepad [Display 3] on Desktop 2`
- The return switch with the lock still enabled shows the inverse move on `Display 2`, proving the pinned monitor stayed fixed again while the unlocked monitor remained on the switched desktop.
- After unlocking from the tray, the final `Desktop 1 -> Desktop 2` report returns to:
  - `restored locks: 0`
  - `locked monitors: 0`
  - `planned moves: 0`

## Remaining Gaps

- No release-blocking product gap was observed in this acceptance run.
- Non-blocking proof caveat: the helper-only `switch_states.desktops` snapshot inside `proof-state.json` still collapses every proof window onto desktop `1` on this host. The authoritative acceptance evidence is `background.desktop-switch-reports.txt`, not that helper snapshot.
