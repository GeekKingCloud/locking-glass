#include "test_helpers.h"

#include "locking_glass/core/monitor_locking.h"

namespace locking_glass::tests {

bool RunDesktopLockingChecks() {
  // The replay-only staging desktop field tests production move ordering and
  // non-Windows fail-closed gating without claiming live helper coverage.
  int failures = 0;

  const auto temp_directory = MakeTempDirectory();
  const auto session_path = temp_directory / "desktop-session-state.tsv";
  const auto script_path = temp_directory / "desktop-script.tsv";
  WriteTextFile(
      script_path,
      "event\tdesktop-switch\tkeyboard\tdesktop-alpha\tdesktop-beta\tdesktop-locking-glass\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "monitor\tstable-right\tDISPLAY#RIGHT\tSERIAL-RIGHT\tDell U2720Q\tDisplay 2\t2560\t0\t5120\t1440\t0\n"
      "window\tleft-alpha\tDocs\tstable-left\tDisplay 1\tdesktop-alpha\t1\t1\n"
      "window\tleft-beta\tChat\tstable-left\tDisplay 1\tdesktop-beta\t1\t1\n"
      "window\tright-alpha\tEditor\tstable-right\tDisplay 2\tdesktop-alpha\t1\t1\n"
      "window\tleft-child\tChild palette\tstable-left\tDisplay 1\tdesktop-alpha\t0\t1\n"
      "window\tleft-static\tPinned timer\tstable-left\tDisplay 1\tdesktop-alpha\t1\t0\n"
      "event\tdesktop-switch\tkeyboard-after-unlock\tdesktop-alpha\tdesktop-beta\tdesktop-locking-glass\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "monitor\tstable-right\tDISPLAY#RIGHT\tSERIAL-RIGHT\tDell U2720Q\tDisplay 2\t2560\t0\t5120\t1440\t0\n"
      "window\tleft-alpha\tDocs\tstable-left\tDisplay 1\tdesktop-alpha\t1\t1\n"
      "window\tleft-beta\tChat\tstable-left\tDisplay 1\tdesktop-beta\t1\t1\n"
      "window\tright-alpha\tEditor\tstable-right\tDisplay 2\tdesktop-alpha\t1\t1\n");

  SetEnvironmentVariable("LOCKING_GLASS_SESSION_PATH", session_path.string());

#if !defined(_WIN32)
  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_SCRIPT", "");
  {
    auto runtime_without_script = locking_glass::core::BuildRuntime();
    const int no_script_result = runtime_without_script.virtual_desktop_controller->WatchSwitches(
        runtime_without_script.session_store,
        [&](const locking_glass::integration::DesktopSwitchReport&) { return true; });
    failures += !Expect(
        no_script_result == 1,
        "desktop watch should stay unavailable on non-Windows hosts until an explicit replay script is configured");
  }
#endif

  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_SCRIPT", script_path.string());

  const auto left_monitor =
      MakeMonitor("stable-left", "DISPLAY#LEFT", "SERIAL-LEFT", "Dell U2720Q",
                  "Display 1", 0, 0, 2560, 1440, true);
  const auto right_monitor =
      MakeMonitor("stable-right", "DISPLAY#RIGHT", "SERIAL-RIGHT", "Dell U2720Q",
                  "Display 2", 2560, 0, 5120, 1440, false);

  auto initial_snapshot =
      locking_glass::core::SessionStore(session_path)
          .Restore({left_monitor, right_monitor})
          .snapshot;
  failures += !Expect(
      locking_glass::core::SessionStore(session_path).SetLocked(
          &initial_snapshot, left_monitor, true),
      "desktop locking setup should be able to confirm and lock Display 1");
  failures += !Expect(
      locking_glass::core::SessionStore(session_path).Save(initial_snapshot),
      "desktop locking setup should persist the initial monitor lock");

  {
    const auto entering_staging_plan =
        locking_glass::core::BuildMonitorLockingPlan(
            locking_glass::core::SessionStore(session_path),
            locking_glass::core::DesktopSwitchScenario{
                .trigger = "enter-staging",
                .source_desktop_id = "desktop-beta",
                .target_desktop_id = "desktop-locking-glass",
                .staging_desktop_id = "desktop-locking-glass",
                .monitors = {left_monitor, right_monitor},
                .windows =
                    {
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-pinned",
                            .title = "Pinned source",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = "desktop-beta",
                            .is_top_level = true,
                            .can_move = true,
                        },
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-staged-beta",
                            .title = "Parked beta occupant",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = "desktop-locking-glass",
                            .is_top_level = true,
                            .can_move = true,
                        },
                    },
                .use_staging_restore_hints = true,
                .staging_restore_hints =
                    {
                        locking_glass::core::StagingRestoreHint{
                            .window_id = "left-staged-beta",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .home_desktop_id = "desktop-beta",
                        },
                    },
            });
    failures += !Expect(
        entering_staging_plan.moves.size() == 2U,
        "entering staging should restore parked occupants and move pinned source windows");
    if (entering_staging_plan.moves.size() == 2U) {
      failures += !Expect(
          entering_staging_plan.moves[0].window.window_id == "left-staged-beta",
          "entering staging should restore parked occupants before moving pinned source windows");
      failures += !Expect(
          entering_staging_plan.moves[0].from_desktop_id ==
              "desktop-locking-glass",
          "entering staging should move the parked occupant from staging");
      failures += !Expect(
          entering_staging_plan.moves[0].to_desktop_id == "desktop-beta",
          "entering staging should restore the parked occupant to the desktop being left");
      failures += !Expect(
          entering_staging_plan.moves[1].window.window_id == "left-pinned",
          "entering staging should then move the pinned source window");
      failures += !Expect(
          entering_staging_plan.moves[1].to_desktop_id ==
              "desktop-locking-glass",
          "entering staging should keep the pinned monitor visible on the staging desktop");
    }
  }

  {
    const auto unowned_staging_plan =
        locking_glass::core::BuildMonitorLockingPlan(
            locking_glass::core::SessionStore(session_path),
            locking_glass::core::DesktopSwitchScenario{
                .trigger = "enter-staging-unowned",
                .source_desktop_id = "desktop-beta",
                .target_desktop_id = "desktop-locking-glass",
                .staging_desktop_id = "desktop-locking-glass",
                .monitors = {left_monitor, right_monitor},
                .windows =
                    {
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-pinned",
                            .title = "Pinned source",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = "desktop-beta",
                            .is_top_level = true,
                            .can_move = true,
                        },
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-staging-user-window",
                            .title = "User staging window",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = "desktop-locking-glass",
                            .is_top_level = true,
                            .can_move = true,
                        },
                    },
                .use_staging_restore_hints = true,
                .staging_restore_hints = {},
            });
    failures += !Expect(
        unowned_staging_plan.moves.size() == 1U,
        "explicit staging restore mode should not restore untracked staging windows");
    if (unowned_staging_plan.moves.size() == 1U) {
      failures += !Expect(
          unowned_staging_plan.moves[0].window.window_id == "left-pinned",
          "untracked staging windows should stay on the holding desktop");
    }
  }

  {
    const std::string event_staging =
        "Desktop 4 [3] \"Locking Glass\" {29ccd282-483a-4b9f-9b93-abb515d2e82a}";
    const std::string helper_staging =
        "Desktop 4 [3] \"Locking Glass\" {29CCD282-483A-4B9F-9B93-ABB515D2E82A}";
    const auto cased_staging_plan =
        locking_glass::core::BuildMonitorLockingPlan(
            locking_glass::core::SessionStore(session_path),
            locking_glass::core::DesktopSwitchScenario{
                .trigger = "enter-staging-case-mismatch",
                .source_desktop_id = "desktop-beta",
                .target_desktop_id = event_staging,
                .staging_desktop_id = helper_staging,
                .monitors = {left_monitor, right_monitor},
                .windows =
                    {
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-pinned",
                            .title = "Pinned source",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = "desktop-beta",
                            .is_top_level = true,
                            .can_move = true,
                        },
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-staged-beta",
                            .title = "Parked beta occupant",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = event_staging,
                            .is_top_level = true,
                            .can_move = true,
                        },
                    },
                .use_staging_restore_hints = true,
                .staging_restore_hints =
                    {
                        locking_glass::core::StagingRestoreHint{
                            .window_id = "left-staged-beta",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .home_desktop_id = "desktop-beta",
                        },
                    },
            });
    failures += !Expect(
        cased_staging_plan.moves.size() == 2U,
        "staging desktop GUID casing differences should still restore parked occupants when entering staging");
    if (cased_staging_plan.moves.size() == 2U) {
      failures += !Expect(
          cased_staging_plan.moves[0].window.window_id == "left-staged-beta" &&
              cased_staging_plan.moves[0].to_desktop_id == "desktop-beta",
          "cased staging target occupants should restore to the desktop being left");
      failures += !Expect(
          cased_staging_plan.moves[1].window.window_id == "left-pinned" &&
              cased_staging_plan.moves[1].to_desktop_id == event_staging,
          "cased staging target should still receive the pinned source window");
    }
  }

  {
    const auto rotating_staging_plan =
        locking_glass::core::BuildMonitorLockingPlan(
            locking_glass::core::SessionStore(session_path),
            locking_glass::core::DesktopSwitchScenario{
                .trigger = "rotate-staging",
                .source_desktop_id = "desktop-beta",
                .target_desktop_id = "desktop-gamma",
                .staging_desktop_id = "desktop-locking-glass",
                .monitors = {left_monitor, right_monitor},
                .windows =
                    {
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-pinned",
                            .title = "Pinned source",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = "desktop-beta",
                            .is_top_level = true,
                            .can_move = true,
                        },
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-staged-beta",
                            .title = "Parked beta occupant",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = "desktop-locking-glass",
                            .is_top_level = true,
                            .can_move = true,
                        },
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-gamma",
                            .title = "Gamma occupant",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = "desktop-gamma",
                            .is_top_level = true,
                            .can_move = true,
                        },
                    },
                .use_staging_restore_hints = false,
                .staging_restore_hints = {},
            });
    failures += !Expect(
        rotating_staging_plan.moves.size() == 3U,
        "normal desktop switches should restore the old parked occupant, park the next occupant, and move pinned windows");
    if (rotating_staging_plan.moves.size() == 3U) {
      failures += !Expect(
          rotating_staging_plan.moves[0].window.window_id ==
              "left-staged-beta",
          "normal desktop switches should empty staging before parking the next desktop");
      failures += !Expect(
          rotating_staging_plan.moves[0].to_desktop_id == "desktop-beta",
          "normal desktop switches should restore staged windows to the desktop being left");
      failures += !Expect(
          rotating_staging_plan.moves[1].window.window_id == "left-gamma",
          "normal desktop switches should then stage the next target occupant");
      failures += !Expect(
          rotating_staging_plan.moves[1].to_desktop_id ==
              "desktop-locking-glass",
          "normal desktop switches should park target occupants on staging");
      failures += !Expect(
          rotating_staging_plan.moves[2].window.window_id == "left-pinned",
          "normal desktop switches should move pinned windows after staging is ready");
      failures += !Expect(
          rotating_staging_plan.moves[2].to_desktop_id == "desktop-gamma",
          "normal desktop switches should move pinned windows onto the next desktop");
    }
  }

  {
    const auto leaving_staging_plan =
        locking_glass::core::BuildMonitorLockingPlan(
            locking_glass::core::SessionStore(session_path),
            locking_glass::core::DesktopSwitchScenario{
                .trigger = "leave-staging",
                .source_desktop_id = "desktop-locking-glass",
                .target_desktop_id = "desktop-gamma",
                .staging_desktop_id = "desktop-locking-glass",
                .monitors = {left_monitor, right_monitor},
                .windows =
                    {
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-pinned",
                            .title = "Pinned source",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = "desktop-locking-glass",
                            .is_top_level = true,
                            .can_move = true,
                        },
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-parked-beta",
                            .title = "Parked beta occupant",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = "desktop-locking-glass",
                            .is_top_level = true,
                            .can_move = true,
                        },
                        locking_glass::core::DesktopWindow{
                            .window_id = "left-gamma",
                            .title = "Gamma occupant",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .desktop_id = "desktop-gamma",
                            .is_top_level = true,
                            .can_move = true,
                        },
                    },
                .use_staging_restore_hints = true,
                .staging_restore_hints =
                    {
                        locking_glass::core::StagingRestoreHint{
                            .window_id = "left-parked-beta",
                            .monitor_id = "stable-left",
                            .monitor_label = "Display 1",
                            .home_desktop_id = "desktop-beta",
                        },
                    },
            });
    failures += !Expect(
        leaving_staging_plan.moves.size() == 3U,
        "leaving staging should restore parked occupants, park the target desktop, and move pinned windows");
    if (leaving_staging_plan.moves.size() == 3U) {
      failures += !Expect(
          leaving_staging_plan.moves[0].window.window_id ==
              "left-parked-beta",
          "leaving staging should restore parked occupants before any follow move");
      failures += !Expect(
          leaving_staging_plan.moves[0].to_desktop_id == "desktop-beta",
          "leaving staging should restore parked occupants to their own desktop");
      failures += !Expect(
          leaving_staging_plan.moves[1].window.window_id == "left-gamma" &&
              leaving_staging_plan.moves[1].to_desktop_id ==
                  "desktop-locking-glass",
          "leaving staging should park target occupants on the holding desktop");
      failures += !Expect(
          leaving_staging_plan.moves[2].window.window_id == "left-pinned" &&
              leaving_staging_plan.moves[2].to_desktop_id == "desktop-gamma",
          "leaving staging should move the held monitor content onto the target desktop");
    }
  }

  auto runtime = locking_glass::core::BuildRuntime();
  std::vector<locking_glass::integration::DesktopSwitchReport> reports;
  const int watch_result = runtime.virtual_desktop_controller->WatchSwitches(
      runtime.session_store,
      [&](const locking_glass::integration::DesktopSwitchReport& report) {
        reports.push_back(report);
        if (reports.size() == 1U) {
          auto unlocked_snapshot = report.plan.session.snapshot;
          failures += !Expect(runtime.session_store.SetLocked(
                                  &unlocked_snapshot, left_monitor, false),
                              "desktop locking callback should be able to unlock Display 1");
          failures += !Expect(runtime.session_store.Save(unlocked_snapshot),
                              "desktop locking callback should persist the unlock");
        }
        return true;
      });

  failures += !Expect(watch_result == 0,
                      "scripted virtual desktop watch should exit successfully");
  failures += !Expect(reports.size() == 2U,
                      "scripted desktop switching should emit both replayed switch events");
  if (reports.size() == 2U) {
    const auto& locked_report = reports[0];
    failures += !Expect(locked_report.plan.trigger == "keyboard",
                        "first desktop switch should preserve the script trigger");
    failures += !Expect(locked_report.plan.session.restored_locked_monitors == 1U,
                        "first desktop switch should restore the confirmed locked monitor");
    failures += !Expect(locked_report.plan.locked_monitors.size() == 1U,
                        "first desktop switch should identify one locked monitor");
    if (locked_report.plan.locked_monitors.size() == 1U) {
      failures += !Expect(locked_report.plan.locked_monitors.front() == "Display 1",
                          "first desktop switch should lock Display 1");
    }
    failures += !Expect(locked_report.plan.moves.size() == 2U,
                        "locked monitor windows on both desktops should use target and staging desktops");
    failures += !Expect(locked_report.plan.skipped_windows.size() == 2U,
                        "non-top-level and immovable windows should be skipped");
    failures += !Expect(locked_report.move_results.size() == 2U,
                        "scripted replay should record both planned window moves");
    if (locked_report.move_results.size() == 2U) {
      failures += !Expect(
          locked_report.move_results[0].window.window_id == "left-beta",
          "target-desktop windows should move to staging before source windows move onto the target desktop");
    }

    const auto* left_alpha_move = FindMoveResult(locked_report, "left-alpha");
    failures += !Expect(left_alpha_move != nullptr,
                        "locked desktop switch should move the source desktop window");
    if (left_alpha_move != nullptr) {
      failures += !Expect(left_alpha_move->success,
                          "source desktop window move should succeed in the replay");
      failures += !Expect(left_alpha_move->to_desktop_id == "desktop-beta",
                          "source desktop window should move onto the target desktop");
    }

    const auto* left_beta_move = FindMoveResult(locked_report, "left-beta");
    failures += !Expect(left_beta_move != nullptr,
                        "locked desktop switch should move the target desktop window");
    if (left_beta_move != nullptr) {
      failures += !Expect(left_beta_move->success,
                          "target desktop window move should succeed in the replay");
      failures += !Expect(left_beta_move->to_desktop_id == "desktop-locking-glass",
                          "target desktop window should move onto the staging desktop");
    }

    const auto* right_alpha = FindDesktopWindow(locked_report.resulting_windows,
                                                "right-alpha");
    failures += !Expect(right_alpha != nullptr,
                        "desktop switch results should keep unlocked monitor windows");
    if (right_alpha != nullptr) {
      failures += !Expect(right_alpha->desktop_id == "desktop-alpha",
                          "unlocked monitor windows should remain on their original desktop");
    }

    const auto formatted_locked =
        locking_glass::integration::FormatDesktopSwitchReport(locked_report);
    failures += !Expect(
        formatted_locked.find("Locking Glass desktop switch policy") !=
            std::string::npos,
        "formatted desktop report should include the policy heading");
    failures += !Expect(
        formatted_locked.find("planned moves: 2") != std::string::npos,
        "formatted desktop report should summarize planned moves");
    failures += !Expect(
        formatted_locked.find("staging desktop: desktop-locking-glass") !=
            std::string::npos,
        "formatted desktop report should name the staging desktop when one is in use");
    failures += !Expect(
        formatted_locked.find("skipped windows: 2") != std::string::npos,
        "formatted desktop report should summarize skipped windows");
    failures += !Expect(
        formatted_locked.find("scripted move applied") != std::string::npos,
        "formatted desktop report should include move execution details");

    const auto& unlocked_report = reports[1];
    failures += !Expect(unlocked_report.plan.trigger == "keyboard-after-unlock",
                        "second desktop switch should preserve its trigger");
    failures += !Expect(unlocked_report.plan.locked_monitors.empty(),
                        "unlocking the monitor should clear the desktop lock plan");
    failures += !Expect(unlocked_report.plan.moves.empty(),
                        "unlocking the monitor should suppress further window moves");
    failures += !Expect(unlocked_report.move_results.empty(),
                        "unlocking the monitor should leave no move results to apply");
    failures += !Expect(unlocked_report.plan.session.restored_locked_monitors == 0U,
                        "second desktop switch should reflect the persisted unlock state");
  }

  const auto preview =
      locking_glass::core::SessionStore(session_path).Preview({left_monitor, right_monitor});
  failures += !Expect(preview.restored_locked_monitors == 0U,
                      "desktop watch unlock should persist into the session store");

  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_SCRIPT", "");
  std::filesystem::remove_all(temp_directory);
  return failures == 0;
}

}  // namespace locking_glass::tests
