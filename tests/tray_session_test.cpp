#include "test_helpers.h"

namespace locking_glass::tests {

bool RunTraySessionChecks() {
  int failures = 0;

  const auto temp_directory = MakeTempDirectory();
  const auto session_path = temp_directory / "tray-session-state.tsv";
  const auto script_path = temp_directory / "tray-script.tsv";
  WriteTextFile(
      script_path,
      "event\tstartup\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "monitor\tstable-right\tDISPLAY#RIGHT\tSERIAL-RIGHT\tDell U2720Q\tDisplay 2\t2560\t0\t5120\t1440\t0\n"
      "action\tclick\n"
      "action\thover\tDisplay 2\n"
      "action\thover-clear\n"
      "action\ttoggle\tDisplay 2\n"
      "action\thover\tDisplay 2\n"
      "event\tWM_DISPLAYCHANGE\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "action\tclick\n"
      "action\thover\tDisplay 1\n"
      "event\tWM_DISPLAYCHANGE\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "monitor\tstable-right\tDISPLAY#RIGHT\tSERIAL-RIGHT\tDell U2720Q\tDisplay 2\t2560\t0\t5120\t1440\t0\n"
      "monitor\tstable-new\tDISPLAY#NEW\tSERIAL-NEW\tLG UltraFine\tDisplay 3\t-1920\t0\t0\t1080\t0\n"
      "action\tclick\n"
      "action\thover\tDisplay 3\n"
      "action\texit\n");

  SetEnvironmentVariable("LOCKING_GLASS_SESSION_PATH", session_path.string());
  SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT", script_path.string());

  auto runtime = locking_glass::core::BuildRuntime();
  std::vector<locking_glass::platform::BackgroundSessionEvent> events;
  const int run_result = runtime.background_session->Run(
      [&](const locking_glass::platform::BackgroundSessionEvent& event) {
        events.push_back(event);
      });

  failures += !Expect(run_result == 0,
                      "scripted tray session should exit successfully");
  failures += !Expect(
      events.size() == 13U,
      "scripted tray session should emit startup, hover, hover-clear, immediate toggle refresh, disconnect, reconnect, and exit tray events");
  if (events.size() == 13U) {
    failures += !Expect(events[0].trigger == "startup",
                        "tray session should publish the startup snapshot first");
    failures += !Expect(!events[0].tray_menu_visible,
                        "startup snapshot should not mark the tray menu as visible");
    failures += !Expect(events[0].prompt.visible,
                        "startup snapshot should prompt for first-run monitor confirmation");
    failures += !Expect(events[0].tray_icon_variant == "review",
                        "unconfirmed monitors should place the tray icon in review mode");
    failures += !Expect(events[0].tray_icon_review_badge,
                        "unconfirmed monitors should add the tray review badge");
    failures += !Expect(events[1].trigger == "tray-click",
                        "first tray interaction should open the monitor menu");
    failures += !Expect(events[1].tray_menu_visible,
                        "tray click should mark the monitor UI as visible");
    failures += !Expect(events[2].trigger == "tray-hover",
                        "hovering a monitor should publish an identify event");
    failures += !Expect(events[2].tray_menu_visible,
                        "hover events should keep the tray menu visible");
    failures += !Expect(events[3].trigger == "tray-hover-clear",
                        "moving away from a hovered monitor should clear the identify overlay");
    failures += !Expect(events[3].tray_menu_visible,
                        "clearing the hover overlay should keep the tray menu visible");
    failures += !Expect(!events[3].highlight.visible,
                        "hover-clear events should hide the identify overlay");
    failures += !Expect(events[4].trigger == "tray-toggle",
                        "monitor toggle should publish an updated tray snapshot");
    failures += !Expect(events[4].tray_menu_visible,
                        "toggle result should keep the refreshed tray menu visible");
    failures += !Expect(events[5].trigger == "tray-hover",
                        "refreshed tray UI should stay interactive immediately after a toggle");
    failures += !Expect(events[5].tray_menu_visible,
                        "post-toggle hover should keep the refreshed tray menu visible");
    failures += !Expect(events[6].trigger == "WM_DISPLAYCHANGE",
                        "disconnect event should republish tray state");
    failures += !Expect(events[7].trigger == "tray-click",
                        "tray UI should still open after a monitor disconnects");
    failures += !Expect(events[8].trigger == "tray-hover",
                        "disconnect state should still support hover identification");
    failures += !Expect(events[9].trigger == "WM_DISPLAYCHANGE",
                        "reconnect event should republish tray state");
    failures += !Expect(events[10].trigger == "tray-click",
                        "topology changes should still leave the tray UI accessible");
    failures += !Expect(events[11].trigger == "tray-hover",
                        "newly added monitors should also support identify hover");
    failures += !Expect(events[12].trigger == "exit",
                        "scripted tray exit should publish a terminal event");

    const auto* opened_left = FindBackgroundMonitor(events[1], "Display 1");
    failures += !Expect(opened_left != nullptr,
                        "tray click should list the first monitor");
    if (opened_left != nullptr) {
      failures += !Expect(!opened_left->locked,
                          "first tray click should show Display 1 as initially unlocked");
      failures += !Expect(opened_left->requires_confirmation,
                          "new monitors should still require confirmation in the tray UI");
      failures += !Expect(opened_left->menu_label.find("2560x1440 @ 0,0, primary") !=
                              std::string::npos,
                          "primary tray monitors should expose layout metadata in the menu label");
      failures += !Expect(opened_left->padlock_variant == "unlocked",
                          "unlocked tray monitors should expose the unlocked padlock variant");
      failures += !Expect(opened_left->padlock_accent == "amber",
                          "confirmation-required tray monitors should use the amber padlock accent");
      failures += !Expect(!opened_left->padlock_filled,
                          "unlocked tray monitors should use an outline padlock");
      failures += !Expect(opened_left->padlock_review_badge,
                          "confirmation-required tray monitors should expose the badge");
    }
    const auto* opened_right = FindBackgroundMonitor(events[1], "Display 2");
    failures += !Expect(opened_right != nullptr,
                        "tray click should list the second monitor");
    if (opened_right != nullptr) {
      failures += !Expect(opened_right->menu_label.find("Display 2") !=
                              std::string::npos,
                          "tray click should expose monitor menu labels");
      failures += !Expect(opened_right->menu_label.find("2560x1440 @ 2560,0") !=
                              std::string::npos,
                          "tray click should expose per-monitor layout metadata in menu labels");
    }
    failures += !Expect(events[1].tray_icon_tooltip.find("0 of 2 locked") !=
                            std::string::npos,
                        "tray icon tooltip should summarize the visible lock state");
    failures += !Expect(HighlightTargets(events[2], "Display 2"),
                        "hovering Display 2 should target it in the highlight overlay");
    failures += !Expect(events[2].highlight.monitor.bounds.left == 2560 &&
                            events[2].highlight.monitor.bounds.right == 5120,
                        "hover overlay should map Display 2 back to the correct physical monitor bounds");
    failures += !Expect(events[2].highlight.title == "Display 2",
                        "hover overlay should use the monitor label as the title");
    failures += !Expect(events[2].highlight.message.find("Dell U2720Q") !=
                            std::string::npos,
                        "hover overlay should include the display name");
    failures += !Expect(events[2].highlight.message.find("top-left 2560,0") !=
                            std::string::npos,
                        "hover overlay should include monitor placement metadata");

    const auto* toggled_right = FindBackgroundMonitor(events[4], "Display 2");
    failures += !Expect(toggled_right != nullptr,
                        "toggle update should still include Display 2");
    if (toggled_right != nullptr) {
      failures += !Expect(toggled_right->locked,
                          "tray toggle should persist the new lock state");
      failures += !Expect(!toggled_right->requires_confirmation,
                          "toggling a monitor should confirm it and clear review state");
      failures += !Expect(toggled_right->status_label == "locked",
                          "tray toggle should expose a locked status label");
      failures += !Expect(toggled_right->padlock_variant == "locked",
                          "locked tray monitors should expose the locked padlock variant");
      failures += !Expect(toggled_right->padlock_accent == "emerald",
                          "locked tray monitors should use the locked padlock accent");
      failures += !Expect(toggled_right->padlock_filled,
                          "locked tray monitors should use a filled padlock icon");
      failures += !Expect(!toggled_right->padlock_review_badge,
                          "confirmed tray monitors should clear the review badge");
    }
    failures += !Expect(events[4].menu_status.empty(),
                        "normal toggle refresh should not render tray summary counts");
    failures += !Expect(events[4].tray_icon_tooltip.find("1 of 2 locked") !=
                            std::string::npos,
                        "toggle refresh should immediately update the tray icon tooltip");
    failures += !Expect(HighlightTargets(events[5], "Display 2"),
                        "post-toggle hover should still target the toggled monitor");
    failures += !Expect(events[5].highlight.message.find(" | locked") !=
                            std::string::npos,
                        "post-toggle hover should describe the updated locked state");
    failures += !Expect(events[5].highlight.message.find("confirm monitor") ==
                            std::string::npos,
                        "post-toggle hover should clear the confirmation-required text");

    const auto* disconnected_right = FindBackgroundMonitor(events[6], "Display 2");
    failures += !Expect(disconnected_right == nullptr,
                        "disconnect refresh should remove the missing monitor from the active tray model");
    failures += !Expect(!events[6].prompt.visible,
                        "disconnect refresh should not prompt when no new monitor was added");

    const auto* reopened_left = FindBackgroundMonitor(events[7], "Display 1");
    failures += !Expect(reopened_left != nullptr,
                        "tray menu should still include Display 1 after a disconnect");
    failures += !Expect(HighlightTargets(events[8], "Display 1"),
                        "hovering Display 1 after a disconnect should still identify it");
    failures += !Expect(events[8].highlight.message.find("unlocked, confirm monitor") !=
                            std::string::npos,
                        "hover overlays should describe both lock and confirmation state");

    const auto* restored_right = FindBackgroundMonitor(events[9], "Display 2");
    failures += !Expect(restored_right != nullptr,
                        "reconnect refresh should restore the returning monitor");
    if (restored_right != nullptr) {
      failures += !Expect(restored_right->locked,
                          "reconnected monitor should recover its saved lock state");
      failures += !Expect(restored_right->padlock_variant == "locked",
                          "reconnected locked monitors should keep the locked padlock variant");
    }

    failures += !Expect(events[9].prompt.visible,
                        "adding a new monitor should emit a confirmation prompt");
    failures += !Expect(events[9].prompt.title == "Confirm monitor",
                        "single-monitor additions should use the singular confirmation prompt");
    failures += !Expect(
        events[9].prompt.message.find("Display 3 - LG UltraFine") != std::string::npos,
        "confirmation prompt should name the newly added monitor");
    failures += !Expect(FindPromptMonitor(events[9].prompt, "Display 3") != nullptr,
                        "confirmation prompt should track the new monitor explicitly");

    const auto* added_monitor = FindBackgroundMonitor(events[9], "Display 3");
    failures += !Expect(added_monitor != nullptr,
                        "topology refresh should expose new monitors in the tray snapshot");
    if (added_monitor != nullptr) {
      failures += !Expect(!added_monitor->locked,
                          "added tray monitors should default to unlocked");
      failures += !Expect(added_monitor->requires_confirmation,
                          "added tray monitors should require confirmation");
      failures += !Expect(added_monitor->padlock_variant == "unlocked",
                          "added tray monitors should expose the unlocked padlock variant");
      failures += !Expect(added_monitor->padlock_accent == "amber",
                          "added tray monitors should use the confirmation padlock accent");
      failures += !Expect(!added_monitor->padlock_filled,
                          "added tray monitors should use an outline padlock icon");
      failures += !Expect(added_monitor->padlock_review_badge,
                          "added tray monitors should mark the padlock icon with the confirmation badge");
      failures += !Expect(added_monitor->identify_label.find("Display 3") !=
                              std::string::npos,
                          "tray menu items should describe the identify-hover behavior");
      failures += !Expect(added_monitor->menu_label.find("1920x1080 @ -1920,0") !=
                              std::string::npos,
                          "added tray monitors should expose layout metadata in the menu label");
    }

    const auto* reopened_right = FindBackgroundMonitor(events[10], "Display 2");
    failures += !Expect(reopened_right != nullptr,
                        "reopened tray UI should still include the reconnected monitor");
    if (reopened_right != nullptr) {
      failures += !Expect(reopened_right->locked,
                          "reopened tray UI should reflect the restored locked state");
    }
    failures += !Expect(!events[10].prompt.visible,
                        "reopening the tray UI should not re-emit the one-time add-monitor prompt");
    failures += !Expect(HighlightTargets(events[11], "Display 3"),
                        "hovering the new monitor should target it in the highlight overlay");
    failures += !Expect(events[11].highlight.message.find("LG UltraFine") !=
                            std::string::npos,
                        "highlight overlays should identify the new monitor hardware");
  }

  const auto left_monitor =
      MakeMonitor("stable-left", "DISPLAY#LEFT", "SERIAL-LEFT", "Dell U2720Q",
                  "Display 1", 0, 0, 2560, 1440, true);
  const auto right_monitor =
      MakeMonitor("stable-right", "DISPLAY#RIGHT", "SERIAL-RIGHT",
                  "Dell U2720Q", "Display 2", 2560, 0, 5120, 1440, false);
  const auto new_monitor =
      MakeMonitor("stable-new", "DISPLAY#NEW", "SERIAL-NEW", "LG UltraFine",
                  "Display 3", -1920, 0, 0, 1080, false);
  auto preview =
      locking_glass::core::SessionStore(session_path)
          .Preview({left_monitor, right_monitor, new_monitor});
  failures += !Expect(preview.restored_locked_monitors == 0U,
                      "tray exit should clear run-scoped monitor locks from the session store");
  const auto* persisted_right = FindMonitorState(preview.snapshot, right_monitor);
  failures += !Expect(persisted_right != nullptr,
                      "persisted session should still contain the reconnected monitor");
  if (persisted_right != nullptr) {
    failures += !Expect(persisted_right->is_present,
                        "persisted session should mark the reconnected monitor active");
    failures += !Expect(!persisted_right->locked,
                        "persisted session should clear the reconnected monitor lock on exit");
  }
  const auto formatted =
      locking_glass::core::FormatTrayMenuModel(
          locking_glass::core::BuildTrayMenuModel(preview, "verification"));
  failures += !Expect(formatted.find("Locking Glass tray menu") != std::string::npos,
                      "formatted tray menu output should include the tray heading");
  failures += !Expect(formatted.find("status: \n") !=
                          std::string::npos,
                      "formatted tray menu output should show an empty normal tray status");
  failures += !Expect(formatted.find("variant: review") != std::string::npos,
                      "formatted tray menu output should include the tray icon variant");
  failures += !Expect(
      formatted.find("Display 1 - Dell U2720Q (2560x1440 @ 0,0, primary)") !=
                          std::string::npos,
                      "formatted tray menu output should include monitor labels");
  failures += !Expect(
      formatted.find("Display 3 - LG UltraFine (1920x1080 @ -1920,0)") !=
                          std::string::npos,
                      "formatted tray menu output should include added monitor labels without a new status");
  failures += !Expect(
      formatted.find("padlock: unlocked, amber, outline, review badge") !=
          std::string::npos,
      "formatted tray menu output should describe confirmation-state padlock icons");
  failures += !Expect(
      formatted.find("Display 3 on screen (LG UltraFine, 1920x1080 @ -1920,0)") !=
          std::string::npos,
                      "formatted tray menu output should describe the identify-hover affordance");

  {
    const auto startup_temp_directory = MakeTempDirectory();
    const auto startup_session_path =
        startup_temp_directory / "tray-startup-session-state.tsv";
    const auto startup_script_path =
        startup_temp_directory / "tray-startup-script.tsv";
    auto saved_snapshot =
        locking_glass::core::SessionStore(startup_session_path)
            .Restore({left_monitor, right_monitor})
            .snapshot;
    failures += !Expect(
        locking_glass::core::SessionStore(startup_session_path)
            .SetLocked(&saved_snapshot, left_monitor, true),
        "startup-unlocked tray setup should be able to seed a saved lock");
    failures += !Expect(
        locking_glass::core::SessionStore(startup_session_path)
            .Save(saved_snapshot),
        "startup-unlocked tray setup should persist the seeded saved lock");
    WriteTextFile(
        startup_script_path,
        "event\tstartup\n"
        "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
        "monitor\tstable-right\tDISPLAY#RIGHT\tSERIAL-RIGHT\tDell U2720Q\tDisplay 2\t2560\t0\t5120\t1440\t0\n"
        "action\texit\n");

    SetEnvironmentVariable("LOCKING_GLASS_SESSION_PATH",
                           startup_session_path.string());
    SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT",
                           startup_script_path.string());
    auto startup_runtime = locking_glass::core::BuildRuntime();
    std::vector<locking_glass::platform::BackgroundSessionEvent>
        startup_events;
    const int startup_run_result = startup_runtime.background_session->Run(
        [&](const locking_glass::platform::BackgroundSessionEvent& event) {
          startup_events.push_back(event);
        });
    failures += !Expect(startup_run_result == 0,
                        "startup-unlocked tray script should exit successfully");
    failures += !Expect(startup_events.size() == 2U,
                        "startup-unlocked tray script should emit startup and exit events");
    if (!startup_events.empty()) {
      const auto* startup_left =
          FindBackgroundMonitor(startup_events.front(), "Display 1");
      failures += !Expect(startup_left != nullptr,
                          "startup event should include the seeded monitor");
      if (startup_left != nullptr) {
        failures += !Expect(!startup_left->locked,
                            "startup event should clear a saved monitor lock");
      }
    }
    std::filesystem::remove_all(startup_temp_directory);
  }

  SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT", "");
  std::filesystem::remove_all(temp_directory);
  return failures == 0;
}

}  // namespace locking_glass::tests
