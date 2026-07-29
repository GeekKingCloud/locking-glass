#include "test_helpers.h"

#include "../src/platform/background_session_internal.h"

#include <iostream>
#include <memory>
#include <optional>
#include <sstream>

namespace locking_glass::tests {

namespace {

class SkippingReturnController
    : public locking_glass::integration::VirtualDesktopController {
 public:
  locking_glass::integration::CapabilityReport Probe() const override {
    return locking_glass::platform::internal::MakeReadyControllerCapability();
  }

  bool CleanupStagingDesktop() const override { return true; }

  locking_glass::integration::UnlockReturnReport ReturnTrackedWindows(
      const locking_glass::integration::UnlockReturnRequest& request)
      const override {
    locking_glass::integration::UnlockReturnReport report{
        .monitor = request.monitor,
        .monitor_home_desktop = request.monitor_home_desktop,
        .current_desktop = request.current_desktop,
        .borrowed_desktops = {},
        .return_candidates = request.tracked_windows,
        .move_results = {},
        .skipped_windows = {},
        .resulting_windows = {},
    };
    for (const auto& tracked_window : request.tracked_windows) {
      report.skipped_windows.push_back(
          locking_glass::integration::UnlockReturnSkip{
              .window = tracked_window.window,
              .current_desktop = tracked_window.staging_desktop.value_or(
                  tracked_window.home_desktop),
              .home_desktop = tracked_window.home_desktop,
              .reason = "test skip",
          });
    }
    return report;
  }

  int WatchSwitches(
      const locking_glass::core::SessionStore&,
      const locking_glass::integration::DesktopSwitchCallback&,
      locking_glass::integration::DesktopWatchOptions) const override {
    return 0;
  }
};

class PartialReturnController
    : public locking_glass::integration::VirtualDesktopController {
 public:
  locking_glass::integration::CapabilityReport Probe() const override {
    return locking_glass::platform::internal::MakeReadyControllerCapability();
  }

  bool CleanupStagingDesktop() const override { return true; }

  locking_glass::integration::UnlockReturnReport ReturnTrackedWindows(
      const locking_glass::integration::UnlockReturnRequest& request)
      const override {
    locking_glass::integration::UnlockReturnReport report{
        .monitor = request.monitor,
        .monitor_home_desktop = request.monitor_home_desktop,
        .current_desktop = request.current_desktop,
        .borrowed_desktops = {},
        .return_candidates = request.tracked_windows,
        .move_results = {},
        .skipped_windows = {},
        .resulting_windows = {},
    };
    for (std::size_t index = 0; index < request.tracked_windows.size();
         ++index) {
      const auto& tracked_window = request.tracked_windows[index];
      if (index == 0U) {
        report.move_results.push_back(
            locking_glass::integration::WindowMoveResult{
                .window = tracked_window.window,
                .from_desktop = tracked_window.staging_desktop.value_or(
                    tracked_window.home_desktop),
                .to_desktop = tracked_window.home_desktop,
                .from_desktop_id = tracked_window.window.desktop_id,
                .to_desktop_id = tracked_window.home_desktop.display_id,
                .success = true,
                .detail = "test move",
            });
        continue;
      }

      report.skipped_windows.push_back(
          locking_glass::integration::UnlockReturnSkip{
              .window = tracked_window.window,
              .current_desktop = tracked_window.staging_desktop.value_or(
                  tracked_window.home_desktop),
              .home_desktop = tracked_window.home_desktop,
              .reason = "test skip",
          });
    }
    return report;
  }

  int WatchSwitches(
      const locking_glass::core::SessionStore&,
      const locking_glass::integration::DesktopSwitchCallback&,
      locking_glass::integration::DesktopWatchOptions) const override {
    return 0;
  }
};

class CleanupCountingController
    : public locking_glass::integration::VirtualDesktopController {
 public:
  locking_glass::integration::CapabilityReport Probe() const override {
    return locking_glass::platform::internal::MakeReadyControllerCapability();
  }

  bool CleanupStagingDesktop() const override {
    ++cleanup_calls;
    return true;
  }

  locking_glass::integration::UnlockReturnReport ReturnTrackedWindows(
      const locking_glass::integration::UnlockReturnRequest& request)
      const override {
    return locking_glass::integration::UnlockReturnReport{
        .monitor = request.monitor,
        .monitor_home_desktop = request.monitor_home_desktop,
        .current_desktop = request.current_desktop,
        .borrowed_desktops = {},
        .return_candidates = request.tracked_windows,
        .move_results = {},
        .skipped_windows = {},
        .resulting_windows = {},
    };
  }

  int WatchSwitches(
      const locking_glass::core::SessionStore&,
      const locking_glass::integration::DesktopSwitchCallback&,
      locking_glass::integration::DesktopWatchOptions) const override {
    return 0;
  }

  mutable int cleanup_calls = 0;
};

}  // namespace

bool RunUnlockReturnChecks() {
  // Combines tracker memory, replay return scripts, and background tray unlock
  // flow so return-to-home state is proven as current-run behavior.
  int failures = 0;

  const auto temp_directory = MakeTempDirectory();
  const auto session_path = temp_directory / "unlock-return-session.tsv";
  const auto desktop_script_path = temp_directory / "unlock-return-desktop.tsv";
  const auto return_script_path = temp_directory / "unlock-return-state.tsv";
  const auto tray_script_path = temp_directory / "unlock-return-tray.tsv";

  const auto left_monitor =
      MakeMonitor("stable-left", "DISPLAY#LEFT", "SERIAL-LEFT", "Dell U2720Q",
                  "Display 1", 0, 0, 2560, 1440, true);
  const auto right_monitor =
      MakeMonitor("stable-right", "DISPLAY#RIGHT", "SERIAL-RIGHT", "Dell U2720Q",
                  "Display 2", 2560, 0, 5120, 1440, false);
  const auto desktop_alpha = MakeDesktopIdentity(0, "guid-alpha", "Alpha");
  const auto desktop_beta = MakeDesktopIdentity(1, "guid-beta", "Beta");
  const auto desktop_gamma = MakeDesktopIdentity(2, "guid-gamma", "Gamma");

  {
    locking_glass::core::SessionStore store(session_path);
    auto session = store.StartUnlocked({left_monitor, right_monitor}).snapshot;
    bool locked_after = false;
    failures += !Expect(
        locking_glass::core::ToggleMonitorLock(store, &session, left_monitor,
                                               &locked_after) &&
            locked_after,
        "test setup should lock the left monitor before recording a desktop switch");
    const auto locked_refresh = store.Restore({left_monitor, right_monitor});

    const auto left_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-race",
            .title = "Fast Unlock",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_alpha.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const locking_glass::integration::DesktopSwitchReport report{
        .plan =
            locking_glass::core::MonitorLockingPlan{
                .trigger = "windows-live-post-message-hook",
                .session = locked_refresh,
                .source_desktop_id = desktop_alpha.display_id,
                .target_desktop_id = desktop_beta.display_id,
                .staging_desktop_id = {},
                .locked_monitors = {left_monitor.label},
                .moves = {},
                .skipped_windows = {},
            },
        .move_results =
            {
                locking_glass::integration::WindowMoveResult{
                    .window = left_window,
                    .from_desktop = desktop_alpha,
                    .to_desktop = desktop_beta,
                    .from_desktop_id = desktop_alpha.display_id,
                    .to_desktop_id = desktop_beta.display_id,
                    .success = true,
                    .detail = "moved before unlock click",
                },
            },
        .resulting_windows = {},
    };

    store.StartUnlocked({left_monitor, right_monitor});
    locking_glass::platform::WindowReturnTracker tracker;
    tracker.RecordSuccessfulMoves(
        report,
        [&report](const locking_glass::core::DesktopWindow& window) {
          return locking_glass::platform::internal::IsWindowMonitorLockedInSession(
              report.plan.session, window);
        });
    failures += !Expect(
        tracker.ConsumeMonitor(left_monitor).size() == 1U,
        "desktop-switch tracking should use the switch snapshot, not a later unlocked session file");
  }

  {
    locking_glass::core::SessionStore store(session_path);
    auto session = store.StartUnlocked({left_monitor, right_monitor}).snapshot;
    bool locked_after = false;
    failures += !Expect(
        locking_glass::core::ToggleMonitorLock(store, &session, left_monitor,
                                               &locked_after) &&
            locked_after,
        "test setup should lock an empty monitor before recording its borrowed desktop home");
    const auto locked_refresh = store.Restore({left_monitor, right_monitor});

    locking_glass::platform::WindowReturnTracker tracker;
    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "windows-live-post-message-hook",
                    .session = locked_refresh,
                    .source_desktop_id = desktop_alpha.display_id,
                    .target_desktop_id = desktop_beta.display_id,
                    .staging_desktop_id = {},
                    .locked_monitors = {left_monitor.label},
                    .moves = {},
                    .skipped_windows = {},
                },
            .move_results = {},
            .resulting_windows = {},
        },
        [](const locking_glass::core::DesktopWindow&) { return true; });
    const auto empty_monitor_state = tracker.ConsumeMonitorState(left_monitor);
    failures += !Expect(
        empty_monitor_state.tracked_windows.empty() &&
            empty_monitor_state.home_desktop.has_value(),
        "an empty locked monitor should still remember its original desktop for unlock-time sweeps");
    if (empty_monitor_state.home_desktop.has_value()) {
      failures += !Expect(
          empty_monitor_state.home_desktop->display_id ==
              desktop_alpha.display_id,
          "empty locked monitor home should be the switch source desktop");
    }
    failures += !Expect(
        empty_monitor_state.current_desktop.has_value() &&
            empty_monitor_state.current_desktop->display_id ==
                desktop_beta.display_id,
        "an empty locked monitor should remember the borrowed current desktop from the switch target");
  }

  {
    locking_glass::platform::WindowReturnTracker tracker;
    const auto left_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-doc",
            .title = "Docs",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_alpha.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto renamed_left_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-doc",
            .title = "Docs Renamed",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_beta.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto failed_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-failed",
            .title = "Failed",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_alpha.display_id,
            .is_top_level = true,
            .can_move = true,
        };

    const auto monitor_locked =
        [&](const locking_glass::core::DesktopWindow& window) {
          return window.monitor_id == left_monitor.stable_id;
        };

    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan = {},
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = left_window,
                        .from_desktop = desktop_alpha,
                        .to_desktop = desktop_beta,
                        .from_desktop_id = desktop_alpha.display_id,
                        .to_desktop_id = desktop_beta.display_id,
                        .success = true,
                        .detail = "first move",
                    },
                },
            .resulting_windows = {},
        },
        monitor_locked);
    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan = {},
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = renamed_left_window,
                        .from_desktop = desktop_beta,
                        .to_desktop = desktop_gamma,
                        .from_desktop_id = desktop_beta.display_id,
                        .to_desktop_id = desktop_gamma.display_id,
                        .success = true,
                        .detail = "second move",
                    },
                    locking_glass::integration::WindowMoveResult{
                        .window = failed_window,
                        .from_desktop = desktop_alpha,
                        .to_desktop = desktop_beta,
                        .from_desktop_id = desktop_alpha.display_id,
                        .to_desktop_id = desktop_beta.display_id,
                        .success = false,
                        .detail = "failed move",
                    },
                },
            .resulting_windows = {},
        },
        monitor_locked);
    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan = {},
            .move_results = {},
            .resulting_windows = {},
        },
        monitor_locked);

    const auto consumed = tracker.ConsumeMonitor(left_monitor);
    failures += !Expect(
        consumed.size() == 1U,
        "return tracker should only keep successful moves on the locked monitor");
    if (consumed.size() == 1U) {
      failures += !Expect(
          consumed.front().home_desktop.display_id == desktop_alpha.display_id,
          "return tracker should preserve the first remembered home desktop");
      failures += !Expect(
          consumed.front().window.title == "Docs Renamed",
          "return tracker should refresh the tracked window metadata without overwriting its home desktop");
    }

    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan = {},
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = left_window,
                        .from_desktop = desktop_alpha,
                        .to_desktop = desktop_beta,
                        .from_desktop_id = desktop_alpha.display_id,
                        .to_desktop_id = desktop_beta.display_id,
                        .success = true,
                        .detail = "fresh move",
                    },
                },
            .resulting_windows = {},
        },
        monitor_locked);
    tracker.ClearMonitor(left_monitor);
    failures += !Expect(
        tracker.ConsumeMonitor(left_monitor).empty(),
        "clearing a monitor should reset the tracker so relocking starts fresh");

    tracker.RestoreMonitor(left_monitor, consumed);
    failures += !Expect(
        tracker.ConsumeMonitor(left_monitor).size() == 1U,
        "restoring a monitor should put failed unlock-return work back for retry");

    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan = {},
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = left_window,
                        .from_desktop = desktop_gamma,
                        .to_desktop = desktop_beta,
                        .from_desktop_id = desktop_gamma.display_id,
                        .to_desktop_id = desktop_beta.display_id,
                        .success = true,
                        .detail = "newer move",
                    },
                },
            .resulting_windows = {},
        },
        monitor_locked);
    tracker.RestoreMonitor(left_monitor, consumed);
    const auto merged_restore = tracker.ConsumeMonitor(left_monitor);
    failures += !Expect(
        merged_restore.size() == 1U &&
            merged_restore.front().home_desktop.display_id ==
                desktop_gamma.display_id,
        "restoring old unlock-return work should not overwrite newer tracked state");
  }

  {
    locking_glass::platform::WindowReturnTracker tracker;
    const auto original_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-original",
            .title = "Original",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_alpha.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto new_borrowed_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-new-borrowed",
            .title = "Spotify",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_beta.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto monitor_locked =
        [&](const locking_glass::core::DesktopWindow& window) {
          return window.monitor_id == left_monitor.stable_id;
        };

    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "first-switch",
                    .session = {},
                    .source_desktop_id = desktop_alpha.display_id,
                    .target_desktop_id = desktop_beta.display_id,
                    .staging_desktop_id = "desktop-locking-glass",
                    .locked_monitors = {left_monitor.label},
                    .moves = {},
                    .skipped_windows = {},
                },
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = original_window,
                        .from_desktop = desktop_alpha,
                        .to_desktop = desktop_beta,
                        .from_desktop_id = desktop_alpha.display_id,
                        .to_desktop_id = desktop_beta.display_id,
                        .success = true,
                        .detail = "first follow move",
                    },
                },
            .resulting_windows = {},
        },
        monitor_locked);
    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "second-switch",
                    .session = {},
                    .source_desktop_id = desktop_beta.display_id,
                    .target_desktop_id = desktop_gamma.display_id,
                    .staging_desktop_id = "desktop-locking-glass",
                    .locked_monitors = {left_monitor.label},
                    .moves = {},
                    .skipped_windows = {},
                },
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = new_borrowed_window,
                        .from_desktop = desktop_beta,
                        .to_desktop = desktop_gamma,
                        .from_desktop_id = desktop_beta.display_id,
                        .to_desktop_id = desktop_gamma.display_id,
                        .success = true,
                        .detail = "new borrowed window followed",
                    },
                },
            .resulting_windows = {},
        },
        monitor_locked);

    const auto borrowed_state = tracker.ConsumeMonitorState(left_monitor);
    failures += !Expect(
        borrowed_state.tracked_windows.size() == 2U,
        "return tracker should remember original and later-created borrowed windows");
    const locking_glass::integration::TrackedWindowReturn* borrowed = nullptr;
    for (const auto& tracked_window : borrowed_state.tracked_windows) {
      if (tracked_window.window.window_id == "left-new-borrowed") {
        borrowed = &tracked_window;
      }
    }
    failures += !Expect(
        borrowed != nullptr &&
            borrowed->home_desktop.display_id == desktop_alpha.display_id,
        "new windows that join a borrowed locked monitor should return to the monitor home, not the desktop where they were created");
  }

  {
    auto tracker =
        std::make_shared<locking_glass::platform::WindowReturnTracker>();
    CleanupCountingController cleanup_controller;
    const auto summary = locking_glass::platform::internal::RunUnlockReturn(
        locking_glass::platform::internal::MakeReadyControllerCapability(),
        &cleanup_controller, tracker, left_monitor);
    failures += !Expect(
        !summary.attempted && cleanup_controller.cleanup_calls == 1,
        "unlocking with no tracked windows should still sweep an empty holding desktop");
  }

  {
    locking_glass::platform::WindowReturnTracker tracker;
    const auto staging_desktop =
        MakeDesktopIdentity(3, "29CCD282-483A-4B9F-9B93-ABB515D2E82A",
                            "Locking Glass");
    const std::string event_staging_id =
        "Desktop 4 [3] \"Locking Glass\" {29ccd282-483a-4b9f-9b93-abb515d2e82a}";
    const std::string helper_staging_id = staging_desktop.display_id;
    const auto parked_beta_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-parked-beta",
            .title = "Parked Beta",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_beta.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto restored_beta_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-parked-beta",
            .title = "Parked Beta",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = event_staging_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto gamma_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-gamma",
            .title = "Gamma",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_gamma.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto monitor_locked =
        [&](const locking_glass::core::DesktopWindow& window) {
          return window.monitor_id == left_monitor.stable_id;
        };

    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "park-beta",
                    .session = {},
                    .source_desktop_id = desktop_alpha.display_id,
                    .target_desktop_id = desktop_beta.display_id,
                    .staging_desktop_id = helper_staging_id,
                    .locked_monitors = {left_monitor.label},
                    .moves = {},
                    .skipped_windows = {},
                },
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = parked_beta_window,
                        .from_desktop = desktop_beta,
                        .to_desktop = staging_desktop,
                        .from_desktop_id = desktop_beta.display_id,
                        .to_desktop_id = helper_staging_id,
                        .success = true,
                        .detail = "parked beta",
                    },
                },
            .resulting_windows = {},
        },
        monitor_locked);
    const auto restore_hints = tracker.BuildStagingRestoreHints(
        locking_glass::core::DesktopSwitchScenario{
            .trigger = "hint-restore",
            .source_desktop_id = desktop_beta.display_id,
            .target_desktop_id = desktop_gamma.display_id,
            .staging_desktop_id = helper_staging_id,
            .monitors = {left_monitor, right_monitor},
            .windows = {restored_beta_window},
            .use_staging_restore_hints = false,
            .staging_restore_hints = {},
        });
    failures += !Expect(
        restore_hints.size() == 1U &&
            restore_hints.front().window_id == "left-parked-beta" &&
            restore_hints.front().home_desktop_id == desktop_beta.display_id,
        "return tracker should expose staging restore hints for parked target occupants");
    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "rotate",
                    .session = {},
                    .source_desktop_id = desktop_beta.display_id,
                    .target_desktop_id = desktop_gamma.display_id,
                    .staging_desktop_id = helper_staging_id,
                    .locked_monitors = {left_monitor.label},
                    .moves = {},
                    .skipped_windows = {},
                },
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = restored_beta_window,
                        .from_desktop = staging_desktop,
                        .to_desktop = desktop_beta,
                        .from_desktop_id = event_staging_id,
                        .to_desktop_id = desktop_beta.display_id,
                        .success = true,
                        .detail = "restored beta",
                    },
                    locking_glass::integration::WindowMoveResult{
                        .window = gamma_window,
                        .from_desktop = desktop_gamma,
                        .to_desktop = staging_desktop,
                        .from_desktop_id = desktop_gamma.display_id,
                        .to_desktop_id = helper_staging_id,
                        .success = true,
                        .detail = "parked gamma",
                    },
                },
            .resulting_windows = {},
        },
        monitor_locked);

    const auto rotated_state = tracker.ConsumeMonitorState(left_monitor);
    bool restored_beta_is_still_tracked = false;
    bool gamma_is_tracked = false;
    for (const auto& tracked_window : rotated_state.tracked_windows) {
      if (tracked_window.window.window_id == "left-parked-beta") {
        restored_beta_is_still_tracked = true;
      }
      if (tracked_window.window.window_id == "left-gamma" &&
          tracked_window.home_desktop.display_id == desktop_gamma.display_id &&
          tracked_window.staging_desktop.has_value()) {
        gamma_is_tracked = true;
      }
    }
    failures += !Expect(
        !restored_beta_is_still_tracked,
        "return tracker should forget target occupants once staging restores them to their desktop");
    failures += !Expect(
        gamma_is_tracked,
        "return tracker should keep the next staged occupant queued for a later restore");
  }

  {
    locking_glass::platform::WindowReturnTracker tracker;
    const auto staging_desktop =
        locking_glass::integration::DesktopIdentity{
            .number = 3,
            .guid = "29CCD282-483A-4B9F-9B93-ABB515D2E82A",
            .name = "Locking Glass",
            .display_id =
                "Desktop 4 [3] \"Locking Glass\" {29CCD282-483A-4B9F-9B93-ABB515D2E82A}",
        };
    const std::string event_staging_id =
        "Desktop 4 [3] \"Locking Glass\" {29ccd282-483a-4b9f-9b93-abb515d2e82a}";
    const std::string helper_staging_id = staging_desktop.display_id;
    const auto parked_beta_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-parked-beta-from-staging",
            .title = "Parked Beta From Staging",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_beta.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto restored_beta_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-parked-beta-from-staging",
            .title = "Parked Beta From Staging",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = event_staging_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto monitor_locked =
        [&](const locking_glass::core::DesktopWindow& window) {
          return window.monitor_id == left_monitor.stable_id;
        };

    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "park-beta-before-staging-visit",
                    .session = {},
                    .source_desktop_id = desktop_alpha.display_id,
                    .target_desktop_id = desktop_beta.display_id,
                    .staging_desktop_id = helper_staging_id,
                    .locked_monitors = {left_monitor.label},
                    .moves = {},
                    .skipped_windows = {},
                },
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = parked_beta_window,
                        .from_desktop = desktop_beta,
                        .to_desktop = staging_desktop,
                        .from_desktop_id = desktop_beta.display_id,
                        .to_desktop_id = helper_staging_id,
                        .success = true,
                        .detail = "parked beta before staging visit",
                    },
                },
            .resulting_windows = {},
        },
        monitor_locked);
    tracker.RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "restore-while-leaving-staging",
                    .session = {},
                    .source_desktop_id = event_staging_id,
                    .target_desktop_id = desktop_gamma.display_id,
                    .staging_desktop_id = helper_staging_id,
                    .locked_monitors = {left_monitor.label},
                    .moves = {},
                    .skipped_windows = {},
                },
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = restored_beta_window,
                        .from_desktop = staging_desktop,
                        .to_desktop = desktop_beta,
                        .from_desktop_id = event_staging_id,
                        .to_desktop_id = desktop_beta.display_id,
                        .success = true,
                        .detail = "restored beta while leaving staging",
                    },
                },
            .resulting_windows = {},
        },
        monitor_locked);
    const auto staging_leave_state = tracker.ConsumeMonitorState(left_monitor);
    bool restored_beta_is_still_tracked = false;
    for (const auto& tracked_window : staging_leave_state.tracked_windows) {
      if (tracked_window.window.window_id == "left-parked-beta-from-staging") {
        restored_beta_is_still_tracked = true;
      }
    }
    failures += !Expect(
        !restored_beta_is_still_tracked,
        "return tracker should forget parked occupants restored while the source desktop is staging");
  }

  {
    auto tracker =
        std::make_shared<locking_glass::platform::WindowReturnTracker>();
    const auto left_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-doc",
            .title = "Docs",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_beta.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    tracker->RestoreMonitor(
        left_monitor,
        {
            locking_glass::integration::TrackedWindowReturn{
                .window = left_window,
                .home_desktop = desktop_alpha,
                .staging_desktop = std::nullopt,
            },
        });

    std::ostringstream unavailable_output;
    auto* original_stdout = std::cout.rdbuf(unavailable_output.rdbuf());
    const auto summary = locking_glass::platform::internal::RunUnlockReturn(
        locking_glass::platform::internal::MakeUnavailableControllerCapability(
            "test unavailable"),
        nullptr, tracker, left_monitor);
    std::cout.rdbuf(original_stdout);
    failures += !Expect(
        summary.failed_windows == 1U,
        "unlock return should report a failure when live desktop control is unavailable");
    failures += !Expect(
        tracker->ConsumeMonitor(left_monitor).size() == 1U,
        "failed unlock return should preserve tracked windows for a later retry");
  }

  {
    auto tracker =
        std::make_shared<locking_glass::platform::WindowReturnTracker>();
    const auto left_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-doc",
            .title = "Docs",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_beta.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    tracker->RestoreMonitor(
        left_monitor,
        {
            locking_glass::integration::TrackedWindowReturn{
                .window = left_window,
                .home_desktop = desktop_alpha,
                .staging_desktop = desktop_beta,
            },
        });

    SkippingReturnController skipping_controller;
    std::ostringstream skipped_output;
    auto* original_stdout = std::cout.rdbuf(skipped_output.rdbuf());
    const auto summary = locking_glass::platform::internal::RunUnlockReturn(
        locking_glass::platform::internal::MakeReadyControllerCapability(),
        &skipping_controller, tracker, left_monitor);
    std::cout.rdbuf(original_stdout);
    failures += !Expect(
        summary.skipped_windows == 1U,
        "unlock return should report skipped tracked windows");
    failures += !Expect(
        tracker->ConsumeMonitor(left_monitor).size() == 1U,
        "skipped unlock return should preserve tracked windows for a later retry");
  }

  {
    auto tracker =
        std::make_shared<locking_glass::platform::WindowReturnTracker>();
    const auto returned_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-returned",
            .title = "Returned",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_beta.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto skipped_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-skipped",
            .title = "Skipped",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_beta.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    tracker->RestoreMonitor(
        left_monitor,
        {
            locking_glass::integration::TrackedWindowReturn{
                .window = returned_window,
                .home_desktop = desktop_alpha,
                .staging_desktop = desktop_beta,
            },
            locking_glass::integration::TrackedWindowReturn{
                .window = skipped_window,
                .home_desktop = desktop_alpha,
                .staging_desktop = desktop_beta,
            },
        });

    PartialReturnController partial_controller;
    std::ostringstream partial_output;
    auto* original_stdout = std::cout.rdbuf(partial_output.rdbuf());
    const auto summary = locking_glass::platform::internal::RunUnlockReturn(
        locking_glass::platform::internal::MakeReadyControllerCapability(),
        &partial_controller, tracker, left_monitor);
    std::cout.rdbuf(original_stdout);
    const auto retryable = tracker->ConsumeMonitor(left_monitor);
    failures += !Expect(
        summary.moved_windows == 1U && summary.skipped_windows == 1U,
        "partial unlock return should report both returned and skipped windows");
    failures += !Expect(
        retryable.size() == 1U &&
            retryable.front().window.window_id == "left-skipped",
        "partial unlock return should retry only the unresolved tracked window");
  }

  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_SCRIPT", "");
  SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT", "");
  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_RETURN_SCRIPT",
                         return_script_path.string());

  {
    auto runtime = locking_glass::core::BuildRuntime();
    const locking_glass::integration::UnlockReturnRequest request{
        .monitor = left_monitor,
        .tracked_windows =
            {
                locking_glass::integration::TrackedWindowReturn{
                    .window =
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-doc",
                            .title = "Docs",
                            .monitor_id = left_monitor.stable_id,
                            .monitor_label = left_monitor.label,
                            .desktop_id = desktop_alpha.display_id,
                            .is_top_level = true,
                            .can_move = true,
                        },
                    .home_desktop = desktop_alpha,
                    .staging_desktop = std::nullopt,
                },
            },
        .allow_script_replay = true,
        .monitor_home_desktop = std::nullopt,
        .current_desktop = std::nullopt,
    };

    WriteTextFile(
        return_script_path,
        "desktop\t0\tGUID-ALPHA\tAlpha\n"
        "desktop\t1\tguid-beta\tBeta\n"
        "window\tleft-doc\tDocs\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n");
    const auto success_report =
        runtime.virtual_desktop_controller->ReturnTrackedWindows(request);
    const auto* success_move =
        FindUnlockMoveResult(success_report, "left-doc");
    failures += !Expect(success_move != nullptr,
                        "unlock return should report a replayed move when the remembered desktop still exists");
    if (success_move != nullptr) {
      failures += !Expect(success_move->success,
                          "unlock return should succeed in the replay when the target desktop exists");
      failures += !Expect(success_move->to_desktop.guid == "GUID-ALPHA",
                          "unlock return should target the remembered desktop even when helper GUID casing differs");
      failures += !Expect(success_move->from_desktop.guid == "guid-beta",
                          "unlock return should preserve structured source desktop identity");
    }
    const auto* success_window =
        FindDesktopWindow(success_report.resulting_windows, "left-doc");
    failures += !Expect(success_window != nullptr,
                        "unlock return should report the resulting window snapshot");
    if (success_window != nullptr) {
      failures += !Expect(success_window->desktop_id.find("GUID-ALPHA") !=
                              std::string::npos,
                          "successful unlock returns should update the resulting window desktop using the resolved helper identity");
    }

    WriteTextFile(
        return_script_path,
        "desktop\t1\tguid-beta\tBeta\n"
        "window\tleft-doc\tDocs\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n");
    const auto missing_report =
        runtime.virtual_desktop_controller->ReturnTrackedWindows(request);
    const auto* missing_skip = FindUnlockSkip(missing_report, "left-doc");
    failures += !Expect(missing_skip != nullptr,
                        "unlock return should skip windows whose remembered desktop no longer exists");
    if (missing_skip != nullptr) {
      failures += !Expect(
          missing_skip->reason.find("no longer exists") != std::string::npos,
          "missing remembered desktops should produce an explicit skip reason");
    }

    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\n"
        "desktop\t1\tguid-beta\tBeta\n"
        "window\tleft-doc\tDocs\tstable-left\tDisplay 1\t0\tguid-alpha\tAlpha\t1\t1\n");
    const auto already_home_report =
        runtime.virtual_desktop_controller->ReturnTrackedWindows(request);
    const auto* already_home_skip =
        FindUnlockSkip(already_home_report, "left-doc");
    failures += !Expect(already_home_skip != nullptr,
                        "unlock return should skip windows that are already back on their remembered desktop");
    if (already_home_skip != nullptr) {
      failures += !Expect(
          already_home_skip->reason.find("already on its remembered desktop") !=
              std::string::npos,
          "already-home windows should keep their explicit skip reason");
    }

    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\n"
        "desktop\t1\tguid-beta\tBeta\n"
        "window\tleft-doc\tDocs\tstable-right\tDisplay 2\t1\tguid-beta\tBeta\t1\t1\n");
    const auto wrong_monitor_report =
        runtime.virtual_desktop_controller->ReturnTrackedWindows(request);
    const auto* wrong_monitor_skip =
        FindUnlockSkip(wrong_monitor_report, "left-doc");
    failures += !Expect(wrong_monitor_skip != nullptr,
                        "unlock return should skip windows that no longer live on the unlocked monitor");
    if (wrong_monitor_skip != nullptr) {
      failures += !Expect(
          wrong_monitor_skip->reason.find("no longer on the unlocked monitor") !=
              std::string::npos,
          "monitor drift should produce an explicit unlock-return skip reason");
    }

    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\n"
        "desktop\t1\tguid-beta\tBeta\n"
        "window\tleft-doc\tDocs\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n"
        "failure\tleft-doc\tforced replay failure\n");
    const auto failed_report =
        runtime.virtual_desktop_controller->ReturnTrackedWindows(request);
    const auto* failed_move = FindUnlockMoveResult(failed_report, "left-doc");
    failures += !Expect(failed_move != nullptr,
                        "unlock return should report failed replay moves");
    if (failed_move != nullptr) {
      failures += !Expect(!failed_move->success,
                          "forced replay failures should surface as failed return moves");
      failures += !Expect(failed_move->detail == "forced replay failure",
                          "failed replay moves should preserve their failure detail");
    }

    const locking_glass::integration::UnlockReturnRequest no_replay_request{
        .monitor = request.monitor,
        .tracked_windows = request.tracked_windows,
        .allow_script_replay = false,
        .monitor_home_desktop = request.monitor_home_desktop,
        .current_desktop = request.current_desktop,
    };
    const auto no_replay_report =
        runtime.virtual_desktop_controller->ReturnTrackedWindows(
            no_replay_request);
    const auto* no_replay_move =
        FindUnlockMoveResult(no_replay_report, "left-doc");
    failures += !Expect(
        no_replay_move == nullptr ||
            no_replay_move->detail != "forced replay failure",
        "unlock return should ignore replay scripts when replay is disabled");

    const locking_glass::integration::UnlockReturnRequest sweep_request{
        .monitor = left_monitor,
        .tracked_windows =
            {
                locking_glass::integration::TrackedWindowReturn{
                    .window =
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-doc",
                            .title = "Docs",
                            .monitor_id = left_monitor.stable_id,
                            .monitor_label = left_monitor.label,
                            .desktop_id = desktop_gamma.display_id,
                            .is_top_level = true,
                            .can_move = true,
                        },
                    .home_desktop = desktop_alpha,
                    .staging_desktop = std::nullopt,
                },
                locking_glass::integration::TrackedWindowReturn{
                    .window =
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-chat",
                            .title = "Chat",
                            .monitor_id = left_monitor.stable_id,
                            .monitor_label = left_monitor.label,
                            .desktop_id = "desktop-locking-glass",
                            .is_top_level = true,
                            .can_move = true,
                        },
                    .home_desktop = desktop_beta,
                    .staging_desktop =
                        locking_glass::integration::DesktopIdentity{
                            .number = 3,
                            .guid = "guid-locking-glass",
                            .name = "Locking Glass",
                            .display_id = "desktop-locking-glass",
                        },
                },
            },
        .allow_script_replay = true,
        .monitor_home_desktop = desktop_alpha,
        .current_desktop = desktop_gamma,
    };
    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\tdesktop-alpha\n"
        "desktop\t1\tguid-beta\tBeta\tdesktop-beta\n"
        "desktop\t2\tguid-gamma\tGamma\tdesktop-gamma\n"
        "desktop\t3\tguid-locking-glass\tLocking Glass\tdesktop-locking-glass\n"
        "current\t2\tguid-gamma\tGamma\tdesktop-gamma\n"
        "window\tleft-doc\tDocs\tstable-left\tDisplay 1\t2\tguid-gamma\tGamma\tdesktop-gamma\t1\t1\n"
        "window\tleft-chat\tChat\tstable-left\tDisplay 1\t3\tguid-locking-glass\tLocking Glass\tdesktop-locking-glass\t1\t1\n"
        "window\tleft-new\tSpotify\tstable-left\tDisplay 1\t2\tguid-gamma\tGamma\tdesktop-gamma\t1\t1\n"
        "window\tleft-child\tChild\tstable-left\tDisplay 1\t2\tguid-gamma\tGamma\tdesktop-gamma\t0\t1\n"
        "window\tleft-blocked\tBlocked\tstable-left\tDisplay 1\t2\tguid-gamma\tGamma\tdesktop-gamma\t1\t0\n"
        "window\tleft-off-current\tMail\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\tdesktop-beta\t1\t1\n");
    const auto sweep_report =
        runtime.virtual_desktop_controller->ReturnTrackedWindows(sweep_request);
    failures += !Expect(
        FindUnlockMoveResult(sweep_report, "left-doc") != nullptr,
        "unlock return should keep returning tracked borrowed windows");
    const auto* sweep_staged =
        FindUnlockMoveResult(sweep_report, "left-chat");
    failures += !Expect(
        sweep_staged != nullptr &&
            sweep_staged->to_desktop.guid == desktop_beta.guid,
        "unlock return should keep staged target occupants pointed at their own home desktop");
    const auto* sweep_new = FindUnlockMoveResult(sweep_report, "left-new");
    failures += !Expect(
        sweep_new != nullptr &&
            sweep_new->to_desktop.guid == desktop_alpha.guid,
        "unlock return should synthesize current-monitor windows on the borrowed desktop and send them to monitor home");
    failures += !Expect(
        FindUnlockMoveResult(sweep_report, "left-off-current") == nullptr &&
            FindUnlockSkip(sweep_report, "left-off-current") == nullptr,
        "unlock return should not steal same-monitor windows from non-current virtual desktops");
    failures += !Expect(
        FindUnlockMoveResult(sweep_report, "left-child") == nullptr &&
            FindUnlockSkip(sweep_report, "left-child") == nullptr,
        "unlock return should not synthesize non-top-level current monitor windows");
    failures += !Expect(
        FindUnlockMoveResult(sweep_report, "left-blocked") == nullptr &&
            FindUnlockSkip(sweep_report, "left-blocked") == nullptr,
        "unlock return should not synthesize immovable current monitor windows");
    failures += !Expect(
        sweep_report.return_candidates.size() == 3U,
        "unlock return should expose tracked plus eligible synthesized candidates for retry decisions");

    const locking_glass::integration::UnlockReturnRequest stale_current_request{
        .monitor = left_monitor,
        .tracked_windows =
            {
                locking_glass::integration::TrackedWindowReturn{
                    .window =
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-doc-stale-current",
                            .title = "Docs With Stale Current",
                            .monitor_id = left_monitor.stable_id,
                            .monitor_label = left_monitor.label,
                            .desktop_id = desktop_alpha.display_id,
                            .is_top_level = true,
                            .can_move = true,
                        },
                    .home_desktop = desktop_alpha,
                    .staging_desktop = std::nullopt,
                },
            },
        .allow_script_replay = true,
        .monitor_home_desktop = desktop_alpha,
        .current_desktop = desktop_beta,
    };
    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\tdesktop-alpha\n"
        "desktop\t1\tguid-beta\tBeta\tdesktop-beta\n"
        "desktop\t2\tguid-gamma\tGamma\tdesktop-gamma\n"
        "window\tleft-doc-stale-current\tDocs With Stale Current\tstable-left\tDisplay 1\t2\tguid-gamma\tGamma\tdesktop-gamma\t1\t1\n"
        "window\tleft-new-stale-current\tSpotify With Stale Current\tstable-left\tDisplay 1\t2\tguid-gamma\tGamma\tdesktop-gamma\t1\t1\n"
        "window\tleft-off-stale-current\tMail With Stale Current\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\tdesktop-beta\t1\t1\n");
    const auto stale_current_report =
        runtime.virtual_desktop_controller->ReturnTrackedWindows(
            stale_current_request);
    const auto* stale_current_new =
        FindUnlockMoveResult(stale_current_report, "left-new-stale-current");
    failures += !Expect(
        stale_current_new != nullptr &&
            stale_current_new->to_desktop.guid == desktop_alpha.guid,
        "unlock return should infer the borrowed desktop from tracked home windows when stored current desktop is stale");
    failures += !Expect(
        FindUnlockMoveResult(stale_current_report,
                             "left-off-stale-current") == nullptr &&
            FindUnlockSkip(stale_current_report,
                           "left-off-stale-current") == nullptr,
        "unlock return should not sweep the stale stored current desktop when tracked home windows identify the borrowed desktop");
  }

  {
    const auto desktop_beta_lower_display_only =
        locking_glass::integration::DesktopIdentity{
            .number = -1,
            .guid = {},
            .name = {},
            .display_id =
                "Desktop 2 [1] \"Beta\" {01234567-89ab-cdef-0123-456789abcdef}",
        };
    const locking_glass::integration::UnlockReturnRequest case_request{
        .monitor = left_monitor,
        .tracked_windows =
            {
                locking_glass::integration::TrackedWindowReturn{
                    .window =
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-original-live-case",
                            .title = "Original Live Case",
                            .monitor_id = left_monitor.stable_id,
                            .monitor_label = left_monitor.label,
                            .desktop_id = desktop_alpha.display_id,
                            .is_top_level = true,
                            .can_move = true,
                        },
                    .home_desktop = desktop_alpha,
                    .staging_desktop = std::nullopt,
                },
            },
        .allow_script_replay = true,
        .monitor_home_desktop = desktop_alpha,
        .current_desktop = desktop_beta_lower_display_only,
    };
    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\tdesktop-alpha\n"
        "desktop\t1\t01234567-89AB-CDEF-0123-456789ABCDEF\tBeta\n"
        "window\tleft-original-live-case\tOriginal Live Case\tstable-left\tDisplay 1\t1\t01234567-89AB-CDEF-0123-456789ABCDEF\tBeta\t1\t1\n"
        "window\tleft-detached-live-case\tDetached Live Case\tstable-left\tDisplay 1\t1\t01234567-89AB-CDEF-0123-456789ABCDEF\tBeta\t1\t1\n");
    auto runtime = locking_glass::core::BuildRuntime();
    const auto case_report =
        runtime.virtual_desktop_controller->ReturnTrackedWindows(case_request);
    const auto* case_new =
        FindUnlockMoveResult(case_report, "left-detached-live-case");
    failures += !Expect(
        case_new != nullptr &&
            case_new->to_desktop.guid == desktop_alpha.guid,
        "unlock return should match borrowed desktops when live GUID case differs from the switch-plan display id");
    failures += !Expect(
        case_report.return_candidates.size() == 2U,
        "live GUID case differences should not hide same-monitor synthesized candidates");
  }

  {
    locking_glass::core::SessionStore store(session_path);
    auto session = store.StartUnlocked({left_monitor, right_monitor}).snapshot;
    bool locked_after = false;
    failures += !Expect(
        locking_glass::core::ToggleMonitorLock(store, &session, left_monitor,
                                               &locked_after) &&
            locked_after,
        "test setup should lock the left monitor before source-side skip coverage");
    const auto locked_refresh = store.Restore({left_monitor, right_monitor});
    auto tracker =
        std::make_shared<locking_glass::platform::WindowReturnTracker>();
    const auto source_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-brave-original",
            .title = "Brave Original",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_alpha.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto source_helper_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-brave-helper",
            .title = "Brave Helper",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_alpha.display_id,
            .is_top_level = false,
            .can_move = false,
        };
    tracker->RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "windows-live-post-message-hook",
                    .session = locked_refresh,
                    .source_desktop_id = desktop_alpha.display_id,
                    .target_desktop_id = desktop_beta.display_id,
                    .staging_desktop_id = "desktop-locking-glass",
                    .locked_monitors = {left_monitor.label},
                    .moves =
                        {
                            locking_glass::core::MonitorLockingMove{
                                .window = source_window,
                                .from_desktop_id = desktop_alpha.display_id,
                                .to_desktop_id = desktop_beta.display_id,
                            },
                        },
                    .skipped_windows =
                        {
                            locking_glass::core::MonitorLockingSkip{
                                .window = source_helper_window,
                                .reason = "window is not top-level",
                            },
                        },
                },
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = source_window,
                        .from_desktop = desktop_alpha,
                        .to_desktop = desktop_beta,
                        .from_desktop_id = desktop_alpha.display_id,
                        .to_desktop_id = desktop_beta.display_id,
                        .success = true,
                        .detail = "source moved",
                    },
                },
            .resulting_windows = {},
        },
        [](const locking_glass::core::DesktopWindow&) { return true; });

    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\n"
        "desktop\t1\tguid-beta\tBeta\n"
        "desktop\t2\tguid-gamma\tGamma\n"
        "current\t2\tguid-gamma\tGamma\n"
        "window\tleft-brave-original\tBrave Original\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n"
        "window\tleft-brave-detached\tBrave Detached Tab\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n");
    auto runtime = locking_glass::core::BuildRuntime();
    std::ostringstream source_skip_output;
    auto* original_stdout = std::cout.rdbuf(source_skip_output.rdbuf());
    const auto summary = locking_glass::platform::internal::RunUnlockReturn(
        locking_glass::platform::internal::MakeReadyControllerCapability(),
        runtime.virtual_desktop_controller.get(), tracker, left_monitor);
    std::cout.rdbuf(original_stdout);
    failures += !Expect(
        summary.moved_windows == 2U && summary.failed_windows == 0U &&
            summary.skipped_windows == 0U,
        "source-side skipped helper windows should not suppress borrowed-monitor unlock sweeps");
    failures += !Expect(
        tracker->ConsumeMonitorState(left_monitor).tracked_windows.empty(),
        "successful borrowed-monitor sweep after source-side skips should not leave retry state");
  }

  {
    locking_glass::core::SessionStore store(session_path);
    auto session = store.StartUnlocked({left_monitor, right_monitor}).snapshot;
    bool locked_after = false;
    failures += !Expect(
        locking_glass::core::ToggleMonitorLock(store, &session, left_monitor,
                                               &locked_after) &&
            locked_after,
        "test setup should lock the left monitor before unresolved helper skip coverage");
    const auto locked_refresh = store.Restore({left_monitor, right_monitor});
    auto tracker =
        std::make_shared<locking_glass::platform::WindowReturnTracker>();
    const auto source_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-source-with-unresolved-helper",
            .title = "Source With Unresolved Helper",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_alpha.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto unresolved_helper_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-unresolved-helper",
            .title = "LockingGlassIdentifyOverlayWindow",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = {},
            .is_top_level = true,
            .can_move = true,
        };
    tracker->RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "windows-live-post-message-hook",
                    .session = locked_refresh,
                    .source_desktop_id = desktop_alpha.display_id,
                    .target_desktop_id = desktop_beta.display_id,
                    .staging_desktop_id = "desktop-locking-glass",
                    .locked_monitors = {left_monitor.label},
                    .moves =
                        {
                            locking_glass::core::MonitorLockingMove{
                                .window = source_window,
                                .from_desktop_id = desktop_alpha.display_id,
                                .to_desktop_id = desktop_beta.display_id,
                            },
                        },
                    .skipped_windows =
                        {
                            locking_glass::core::MonitorLockingSkip{
                                .window = unresolved_helper_window,
                                .reason =
                                    "window desktop could not be resolved safely",
                            },
                        },
                },
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = source_window,
                        .from_desktop = desktop_alpha,
                        .to_desktop = desktop_beta,
                        .from_desktop_id = desktop_alpha.display_id,
                        .to_desktop_id = desktop_beta.display_id,
                        .success = true,
                        .detail = "source moved",
                    },
                },
            .resulting_windows = {},
        },
        [](const locking_glass::core::DesktopWindow&) { return true; });

    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\n"
        "desktop\t1\tguid-beta\tBeta\n"
        "current\t1\tguid-beta\tBeta\n"
        "window\tleft-source-with-unresolved-helper\tSource With Unresolved Helper\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n"
        "window\tleft-new-after-unresolved-helper\tNew After Unresolved Helper\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n");
    auto runtime = locking_glass::core::BuildRuntime();
    std::ostringstream unresolved_helper_output;
    auto* original_stdout = std::cout.rdbuf(
        unresolved_helper_output.rdbuf());
    const auto summary = locking_glass::platform::internal::RunUnlockReturn(
        locking_glass::platform::internal::MakeReadyControllerCapability(),
        runtime.virtual_desktop_controller.get(), tracker, left_monitor);
    std::cout.rdbuf(original_stdout);
    failures += !Expect(
        summary.moved_windows == 2U && summary.failed_windows == 0U &&
            summary.skipped_windows == 0U,
        "unresolved helper skips should not erase monitor home or suppress borrowed-monitor sweeps");
  }

  {
    locking_glass::core::SessionStore store(session_path);
    auto session = store.StartUnlocked({left_monitor, right_monitor}).snapshot;
    bool locked_after = false;
    failures += !Expect(
        locking_glass::core::ToggleMonitorLock(store, &session, left_monitor,
                                               &locked_after) &&
            locked_after,
        "test setup should lock the left monitor before target-side helper skip coverage");
    const auto locked_refresh = store.Restore({left_monitor, right_monitor});
    auto tracker =
        std::make_shared<locking_glass::platform::WindowReturnTracker>();
    const auto source_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-source-with-target-helper",
            .title = "Source With Target Helper",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_alpha.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto target_helper_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-target-helper",
            .title = "Target Helper",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_beta.display_id,
            .is_top_level = false,
            .can_move = false,
        };
    tracker->RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "windows-live-post-message-hook",
                    .session = locked_refresh,
                    .source_desktop_id = desktop_alpha.display_id,
                    .target_desktop_id = desktop_beta.display_id,
                    .staging_desktop_id = "desktop-locking-glass",
                    .locked_monitors = {left_monitor.label},
                    .moves =
                        {
                            locking_glass::core::MonitorLockingMove{
                                .window = source_window,
                                .from_desktop_id = desktop_alpha.display_id,
                                .to_desktop_id = desktop_beta.display_id,
                            },
                        },
                    .skipped_windows =
                        {
                            locking_glass::core::MonitorLockingSkip{
                                .window = target_helper_window,
                                .reason = "window is not top-level",
                            },
                        },
                },
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = source_window,
                        .from_desktop = desktop_alpha,
                        .to_desktop = desktop_beta,
                        .from_desktop_id = desktop_alpha.display_id,
                        .to_desktop_id = desktop_beta.display_id,
                        .success = true,
                        .detail = "source moved",
                    },
                },
            .resulting_windows = {},
        },
        [](const locking_glass::core::DesktopWindow&) { return true; });

    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\n"
        "desktop\t1\tguid-beta\tBeta\n"
        "desktop\t2\tguid-gamma\tGamma\n"
        "current\t2\tguid-gamma\tGamma\n"
        "window\tleft-source-with-target-helper\tSource With Target Helper\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n"
        "window\tleft-new-after-target-helper\tNew After Target Helper\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n");
    auto runtime = locking_glass::core::BuildRuntime();
    std::ostringstream target_helper_output;
    auto* original_stdout = std::cout.rdbuf(target_helper_output.rdbuf());
    const auto summary = locking_glass::platform::internal::RunUnlockReturn(
        locking_glass::platform::internal::MakeReadyControllerCapability(),
        runtime.virtual_desktop_controller.get(), tracker, left_monitor);
    std::cout.rdbuf(original_stdout);
    failures += !Expect(
        summary.moved_windows == 2U && summary.failed_windows == 0U &&
            summary.skipped_windows == 0U,
        "target-side helper skips should not suppress borrowed-monitor unlock sweeps");
  }

  {
    locking_glass::core::SessionStore store(session_path);
    auto session = store.StartUnlocked({left_monitor, right_monitor}).snapshot;
    bool locked_after = false;
    failures += !Expect(
        locking_glass::core::ToggleMonitorLock(store, &session, left_monitor,
                                               &locked_after) &&
            locked_after,
        "test setup should lock the left monitor before synthetic retry coverage");
    const auto locked_refresh = store.Restore({left_monitor, right_monitor});
    auto tracker =
        std::make_shared<locking_glass::platform::WindowReturnTracker>();
    tracker->RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "windows-live-post-message-hook",
                    .session = locked_refresh,
                    .source_desktop_id = desktop_alpha.display_id,
                    .target_desktop_id = desktop_beta.display_id,
                    .staging_desktop_id = "desktop-locking-glass",
                    .locked_monitors = {left_monitor.label},
                    .moves = {},
                    .skipped_windows = {},
                },
            .move_results = {},
            .resulting_windows = {},
        },
        [](const locking_glass::core::DesktopWindow&) { return true; });

    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\n"
        "desktop\t1\tguid-beta\tBeta\n"
        "current\t1\tguid-beta\tBeta\n"
        "window\tleft-new-fails\tSpotify\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n"
        "failure\tleft-new-fails\tforced synthetic failure\n");
    auto runtime = locking_glass::core::BuildRuntime();
    std::ostringstream synthetic_output;
    auto* original_stdout = std::cout.rdbuf(synthetic_output.rdbuf());
    const auto summary = locking_glass::platform::internal::RunUnlockReturn(
        locking_glass::platform::internal::MakeReadyControllerCapability(),
        runtime.virtual_desktop_controller.get(), tracker, left_monitor);
    std::cout.rdbuf(original_stdout);
    const auto retry_state = tracker->ConsumeMonitorState(left_monitor);
    failures += !Expect(
        summary.failed_windows == 1U && summary.retryable_windows == 1U,
        "failed synthetic unlock return should be reported as retryable");
    failures += !Expect(
        retry_state.tracked_windows.size() == 1U &&
            retry_state.tracked_windows.front().window.window_id ==
                "left-new-fails" &&
            retry_state.home_desktop.has_value(),
        "failed synthetic unlock return should restore the synthesized window and monitor home for retry");
  }

  {
    locking_glass::core::SessionStore store(session_path);
    auto session = store.StartUnlocked({left_monitor, right_monitor}).snapshot;
    bool locked_after = false;
    failures += !Expect(
        locking_glass::core::ToggleMonitorLock(store, &session, left_monitor,
                                               &locked_after) &&
            locked_after,
        "test setup should lock the left monitor before partial failure coverage");
    const auto locked_refresh = store.Restore({left_monitor, right_monitor});
    auto tracker =
        std::make_shared<locking_glass::platform::WindowReturnTracker>();
    const auto source_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-source-partial",
            .title = "Source Partial",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_alpha.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    const auto target_window =
        locking_glass::core::DesktopWindow{
            .window_id = "left-target-failed",
            .title = "Target Failed",
            .monitor_id = left_monitor.stable_id,
            .monitor_label = left_monitor.label,
            .desktop_id = desktop_beta.display_id,
            .is_top_level = true,
            .can_move = true,
        };
    tracker->RecordSuccessfulMoves(
        locking_glass::integration::DesktopSwitchReport{
            .plan =
                locking_glass::core::MonitorLockingPlan{
                    .trigger = "windows-live-post-message-hook",
                    .session = locked_refresh,
                    .source_desktop_id = desktop_alpha.display_id,
                    .target_desktop_id = desktop_beta.display_id,
                    .staging_desktop_id = "desktop-locking-glass",
                    .locked_monitors = {left_monitor.label},
                    .moves =
                        {
                            locking_glass::core::MonitorLockingMove{
                                .window = source_window,
                                .from_desktop_id = desktop_alpha.display_id,
                                .to_desktop_id = desktop_beta.display_id,
                            },
                            locking_glass::core::MonitorLockingMove{
                                .window = target_window,
                                .from_desktop_id = desktop_beta.display_id,
                                .to_desktop_id = "desktop-locking-glass",
                            },
                        },
                    .skipped_windows = {},
                },
            .move_results =
                {
                    locking_glass::integration::WindowMoveResult{
                        .window = source_window,
                        .from_desktop = desktop_alpha,
                        .to_desktop = desktop_beta,
                        .from_desktop_id = desktop_alpha.display_id,
                        .to_desktop_id = desktop_beta.display_id,
                        .success = true,
                        .detail = "source moved",
                    },
                    locking_glass::integration::WindowMoveResult{
                        .window = target_window,
                        .from_desktop = desktop_beta,
                        .to_desktop =
                            locking_glass::integration::DesktopIdentity{
                                .number = 3,
                                .guid = "guid-locking-glass",
                                .name = "Locking Glass",
                                .display_id = "desktop-locking-glass",
                            },
                        .from_desktop_id = desktop_beta.display_id,
                        .to_desktop_id = "desktop-locking-glass",
                        .success = false,
                        .detail = "staging failed",
                    },
                },
            .resulting_windows = {},
        },
        [](const locking_glass::core::DesktopWindow&) { return true; });

    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\n"
        "desktop\t1\tguid-beta\tBeta\n"
        "current\t1\tguid-beta\tBeta\n"
        "window\tleft-source-partial\tSource Partial\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n"
        "window\tleft-new-after-partial\tNew After Partial\tstable-left\tDisplay 1\t1\tguid-beta\tBeta\t1\t1\n");
    auto runtime = locking_glass::core::BuildRuntime();
    std::ostringstream partial_failure_output;
    auto* original_stdout = std::cout.rdbuf(partial_failure_output.rdbuf());
    const auto summary = locking_glass::platform::internal::RunUnlockReturn(
        locking_glass::platform::internal::MakeReadyControllerCapability(),
        runtime.virtual_desktop_controller.get(), tracker, left_monitor);
    std::cout.rdbuf(original_stdout);
    failures += !Expect(
        summary.moved_windows == 1U && summary.failed_windows == 0U &&
            summary.skipped_windows == 0U,
        "partial lock failures should return tracked successful windows without synthesizing a borrowed-monitor sweep");
  }

  WriteTextFile(
      desktop_script_path,
      "event\tdesktop-switch\tfirst\tdesktop-alpha\tdesktop-beta\tdesktop-locking-glass\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "monitor\tstable-right\tDISPLAY#RIGHT\tSERIAL-RIGHT\tDell U2720Q\tDisplay 2\t2560\t0\t5120\t1440\t0\n"
      "window\tleft-doc\tDocs\tstable-left\tDisplay 1\tdesktop-alpha\t1\t1\n"
      "window\tleft-chat\tChat\tstable-left\tDisplay 1\tdesktop-beta\t1\t1\n"
      "window\tright-editor\tEditor\tstable-right\tDisplay 2\tdesktop-alpha\t1\t1\n"
      "event\tdesktop-switch\tsecond\tdesktop-beta\tdesktop-gamma\tdesktop-locking-glass\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "monitor\tstable-right\tDISPLAY#RIGHT\tSERIAL-RIGHT\tDell U2720Q\tDisplay 2\t2560\t0\t5120\t1440\t0\n"
      "window\tleft-doc\tDocs Renamed\tstable-left\tDisplay 1\tdesktop-beta\t1\t1\n"
      "window\tleft-chat\tChat\tstable-left\tDisplay 1\tdesktop-locking-glass\t1\t1\n"
      "window\tright-editor\tEditor\tstable-right\tDisplay 2\tdesktop-beta\t1\t1\n");
  WriteTextFile(
      return_script_path,
      "desktop\t0\tguid-alpha\tAlpha\tdesktop-alpha\n"
      "desktop\t1\tguid-beta\tBeta\tdesktop-beta\n"
      "desktop\t2\tguid-gamma\tGamma\tdesktop-gamma\n"
      "desktop\t3\tguid-locking-glass\tLocking Glass\tdesktop-locking-glass\n"
      "current\t2\tguid-gamma\tGamma\tdesktop-gamma\n"
      "window\tleft-doc\tDocs Renamed\tstable-left\tDisplay 1\t2\tguid-gamma\tGamma\tdesktop-gamma\t1\t1\n"
      "window\tleft-chat\tChat\tstable-left\tDisplay 1\t3\tguid-locking-glass\tLocking Glass\tdesktop-locking-glass\t1\t1\n"
      "window\tleft-new\tSpotify\tstable-left\tDisplay 1\t2\tguid-gamma\tGamma\tdesktop-gamma\t1\t1\n");
  WriteTextFile(
      tray_script_path,
      "event\tstartup\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "monitor\tstable-right\tDISPLAY#RIGHT\tSERIAL-RIGHT\tDell U2720Q\tDisplay 2\t2560\t0\t5120\t1440\t0\n"
      "action\tclick\n"
      "action\ttoggle\tDisplay 1\n"
      "action\tdesktop-watch\n"
      "action\tclick\n"
      "action\ttoggle\tDisplay 1\n"
      "action\texit\n");

  SetEnvironmentVariable("LOCKING_GLASS_SESSION_PATH", session_path.string());
  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_SCRIPT",
                         desktop_script_path.string());
  SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT", tray_script_path.string());
  auto runtime = locking_glass::core::BuildRuntime();
  std::vector<locking_glass::platform::BackgroundSessionEvent> events;
  std::ostringstream captured_output;
  auto* original_stdout = std::cout.rdbuf(captured_output.rdbuf());
  const int background_run_result = runtime.background_session->Run(
      [&](const locking_glass::platform::BackgroundSessionEvent& event) {
        events.push_back(event);
      });
  std::cout.rdbuf(original_stdout);

  failures += !Expect(background_run_result == 0,
                      "scripted background unlock flow should exit successfully");
  failures += !Expect(events.size() == 6U,
                      "scripted background unlock flow should emit startup, lock, second click, unlock, and exit events");
  if (events.size() == 6U) {
    failures += !Expect(events[4].trigger == "tray-toggle",
                        "unlock flow should publish the unlock result on the tray-toggle event");
    failures += !Expect(events[4].unlock_return.attempted,
                        "unlocking a monitor with tracked windows should attempt an immediate return");
    failures += !Expect(events[4].unlock_return.moved_windows == 2U,
                        "unlocking should return the locked monitor contents that remain borrowed");
    failures += !Expect(events[4].unlock_return.skipped_windows == 0U,
                        "unlocking should not report skipped windows when the remembered desktop still exists");
    failures += !Expect(events[4].unlock_return.failed_windows == 0U,
                        "unlocking should not report failed windows in the happy path");
  }

  const auto output = captured_output.str();
  failures += !Expect(
      output.find("Locking Glass unlock return") != std::string::npos,
      "background unlocks should log a formatted unlock-return report");
  failures += !Expect(
      output.find("desktop-gamma -> desktop-alpha") != std::string::npos,
      "background unlock returns should preserve the first remembered home desktop across multiple follow moves");
  failures += !Expect(
      output.find("Spotify") != std::string::npos,
      "windows that appear on the borrowed monitor after lock should return home on unlock");

  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_RETURN_SCRIPT", "");
  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_SCRIPT", "");
  SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT", "");
  std::filesystem::remove_all(temp_directory);
  return failures == 0;
}

}  // namespace locking_glass::tests
