#include "test_helpers.h"

#include <iostream>
#include <sstream>

namespace locking_glass::tests {

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
                },
            },
    };

    WriteTextFile(
        return_script_path,
        "desktop\t0\tguid-alpha\tAlpha\n"
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
      failures += !Expect(success_move->to_desktop_id == desktop_alpha.display_id,
                          "unlock return should target the remembered desktop");
      failures += !Expect(success_move->from_desktop.guid == "guid-beta",
                          "unlock return should preserve structured source desktop identity");
    }
    const auto* success_window =
        FindDesktopWindow(success_report.resulting_windows, "left-doc");
    failures += !Expect(success_window != nullptr,
                        "unlock return should report the resulting window snapshot");
    if (success_window != nullptr) {
      failures += !Expect(success_window->desktop_id == desktop_alpha.display_id,
                          "successful unlock returns should update the resulting window desktop");
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
      "window\tleft-static\tTimer\tstable-left\tDisplay 1\tdesktop-beta\t1\t0\n"
      "window\tright-editor\tEditor\tstable-right\tDisplay 2\tdesktop-beta\t1\t1\n");
  WriteTextFile(
      return_script_path,
      "desktop\t0\tguid-alpha\tAlpha\tdesktop-alpha\n"
      "desktop\t1\tguid-beta\tBeta\tdesktop-beta\n"
      "desktop\t2\tguid-gamma\tGamma\tdesktop-gamma\n"
      "desktop\t3\tguid-locking-glass\tLocking Glass\tdesktop-locking-glass\n"
      "window\tleft-doc\tDocs Renamed\tstable-left\tDisplay 1\t2\tguid-gamma\tGamma\tdesktop-gamma\t1\t1\n"
      "window\tleft-chat\tChat\tstable-left\tDisplay 1\t3\tguid-locking-glass\tLocking Glass\tdesktop-locking-glass\t1\t1\n"
      "window\tleft-static\tTimer\tstable-left\tDisplay 1\t2\tguid-gamma\tGamma\tdesktop-gamma\t1\t1\n");
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
                        "unlocking should return source and staged target windows");
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
      output.find("desktop-locking-glass -> desktop-beta") !=
          std::string::npos,
      "background unlock returns should send staged target-desktop windows back to their original desktop");
  failures += !Expect(
      output.find("left-static") == std::string::npos,
      "windows that were only skipped during switch replay should not be tracked for unlock return");

  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_RETURN_SCRIPT", "");
  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_SCRIPT", "");
  SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT", "");
  std::filesystem::remove_all(temp_directory);
  return failures == 0;
}

}  // namespace locking_glass::tests
