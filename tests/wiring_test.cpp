#include "locking_glass/core/runtime.h"
#include "locking_glass/core/session_store.h"
#include "locking_glass/core/tray_ui.h"
#include "locking_glass/integration/autostart.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool Expect(const bool condition, const std::string& message) {
  if (condition) {
    return true;
  }

  std::cerr << "Expectation failed: " << message << '\n';
  return false;
}

const locking_glass::integration::CapabilityReport* FindCapability(
    const locking_glass::core::StartupDiagnostics& diagnostics, const std::string& name) {
  for (const auto& capability : diagnostics.capabilities) {
    if (capability.component == name) {
      return &capability;
    }
  }
  return nullptr;
}

void SetEnvironmentVariable(const std::string& name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name.c_str(), value.c_str());
#else
  setenv(name.c_str(), value.c_str(), 1);
#endif
}

std::filesystem::path MakeTempDirectory() {
  const auto unique_value =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("locking_glass_session_test_" + std::to_string(unique_value));
  std::filesystem::create_directories(path);
  return path;
}

void WriteTextFile(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << contents;
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

locking_glass::platform::MonitorDescriptor MakeMonitor(
    const std::string& stable_id, const std::string& device_path,
    const std::string& edid_serial, const std::string& display_name,
    const std::string& label, const int left, const int top, const int right,
    const int bottom, const bool is_primary) {
  return locking_glass::platform::MonitorDescriptor{
      .stable_id = stable_id,
      .device_path = device_path,
      .edid_serial = edid_serial,
      .display_name = display_name,
      .label = label,
      .bounds =
          locking_glass::platform::MonitorBounds{
              .left = left,
              .top = top,
              .right = right,
              .bottom = bottom,
          },
      .is_primary = is_primary,
  };
}

const locking_glass::core::SessionMonitorState* FindMonitorState(
    const locking_glass::core::SessionSnapshot& snapshot,
    const locking_glass::platform::MonitorDescriptor& monitor) {
  for (const auto& monitor_state : snapshot.monitors) {
    if (monitor_state.monitor.stable_id == monitor.stable_id &&
        monitor_state.monitor.device_path == monitor.device_path &&
        monitor_state.monitor.edid_serial == monitor.edid_serial &&
        monitor_state.monitor.display_name == monitor.display_name &&
        monitor_state.monitor.bounds.left == monitor.bounds.left &&
        monitor_state.monitor.bounds.top == monitor.bounds.top &&
        monitor_state.monitor.bounds.right == monitor.bounds.right &&
        monitor_state.monitor.bounds.bottom == monitor.bounds.bottom &&
        monitor_state.monitor.is_primary == monitor.is_primary) {
      return &monitor_state;
    }
  }
  return nullptr;
}

const locking_glass::platform::BackgroundSessionMenuItem* FindBackgroundMonitor(
    const locking_glass::platform::BackgroundSessionEvent& event,
    const std::string& label) {
  for (const auto& monitor : event.monitors) {
    if (monitor.monitor.label == label) {
      return &monitor;
    }
  }
  return nullptr;
}

const locking_glass::integration::WindowMoveResult* FindMoveResult(
    const locking_glass::integration::DesktopSwitchReport& report,
    const std::string& window_id) {
  for (const auto& result : report.move_results) {
    if (result.window.window_id == window_id) {
      return &result;
    }
  }
  return nullptr;
}

const locking_glass::core::DesktopWindow* FindDesktopWindow(
    const std::vector<locking_glass::core::DesktopWindow>& windows,
    const std::string& window_id) {
  for (const auto& window : windows) {
    if (window.window_id == window_id) {
      return &window;
    }
  }
  return nullptr;
}

const locking_glass::platform::MonitorDescriptor* FindPromptMonitor(
    const locking_glass::platform::BackgroundSessionPrompt& prompt,
    const std::string& label) {
  for (const auto& monitor : prompt.monitors) {
    if (monitor.label == label) {
      return &monitor;
    }
  }
  return nullptr;
}

bool HighlightTargets(
    const locking_glass::platform::BackgroundSessionEvent& event,
    const std::string& label) {
  return event.highlight.visible && event.highlight.monitor.label == label;
}

bool RunSessionStoreChecks() {
  int failures = 0;

  const auto temp_directory = MakeTempDirectory();
  const auto session_path = temp_directory / "monitor-session-state.tsv";
  const locking_glass::core::SessionStore session_store(session_path);

  const auto left_monitor =
      MakeMonitor("\\\\.\\DISPLAY1", "DISPLAY#LEFT", "SERIAL-LEFT",
                  "Dell U2720Q", "Display 1", 0, 0, 2560, 1440, true);
  const auto right_monitor =
      MakeMonitor("\\\\.\\DISPLAY2", "DISPLAY#RIGHT", "SERIAL-RIGHT",
                  "Dell U2720Q", "Display 2", 2560, 0, 5120, 1440, false);
  const auto new_monitor =
      MakeMonitor("\\\\.\\DISPLAY3", "DISPLAY#NEW", "SERIAL-NEW",
                  "LG UltraFine", "Display 3", -1920, 0, 0, 1080, false);

  auto initial = session_store.Restore({left_monitor, right_monitor});
  failures += !Expect(!initial.loaded_from_disk,
                      "initial restore should bootstrap from an empty session file");
  failures += !Expect(initial.new_monitors == 2U,
                      "initial restore should register each live monitor as new");
  failures += !Expect(initial.review_monitors == 2U,
                      "new monitors should require confirmation by default");
  failures += !Expect(session_store.SetLocked(&initial.snapshot, left_monitor, true),
                      "left monitor should be lockable in the session snapshot");
  failures += !Expect(session_store.SetLocked(&initial.snapshot, right_monitor, true),
                      "right monitor should be lockable in the session snapshot");
  failures += !Expect(session_store.Save(initial.snapshot),
                      "session snapshot should persist to disk");
  failures += !Expect(std::filesystem::exists(session_path),
                      "saving the session should create the session file");

  auto restarted =
      locking_glass::core::SessionStore(session_path).Restore({left_monitor, right_monitor});
  failures += !Expect(restarted.loaded_from_disk,
                      "restart simulation should reload the persisted session file");
  failures += !Expect(restarted.restored_locked_monitors == 2U,
                      "restart simulation should restore both confirmed locked monitors");
  failures += !Expect(restarted.review_monitors == 0U,
                      "confirmed monitor locks should not require review on restart");

  const auto* restarted_left = FindMonitorState(restarted.snapshot, left_monitor);
  failures += !Expect(restarted_left != nullptr,
                      "restarted snapshot should contain the left monitor");
  if (restarted_left != nullptr) {
    failures += !Expect(restarted_left->is_present,
                        "left monitor should be active after restart");
    failures += !Expect(restarted_left->locked,
                        "left monitor lock should survive restart");
    failures += !Expect(!restarted_left->requires_confirmation,
                        "confirmed left monitor should not require review");
  }

  auto topology_change =
      locking_glass::core::SessionStore(session_path).Restore({left_monitor, new_monitor});
  failures += !Expect(topology_change.disconnected_monitors == 1U,
                      "removing a monitor should keep one saved monitor inactive");
  failures += !Expect(topology_change.new_monitors == 1U,
                      "adding a monitor should create one new session record");
  failures += !Expect(topology_change.review_monitors == 1U,
                      "new monitors should remain unlocked until reviewed");
  failures += !Expect(topology_change.restored_locked_monitors == 1U,
                      "only active confirmed monitors should count as restored locks");

  const auto* disconnected_right = FindMonitorState(topology_change.snapshot, right_monitor);
  failures += !Expect(disconnected_right != nullptr,
                      "removed monitor should remain in the session snapshot");
  if (disconnected_right != nullptr) {
    failures += !Expect(!disconnected_right->is_present,
                        "removed monitor should be marked inactive");
    failures += !Expect(disconnected_right->locked,
                        "removed monitor should keep its saved lock state");
  }

  const auto* added_monitor = FindMonitorState(topology_change.snapshot, new_monitor);
  failures += !Expect(added_monitor != nullptr,
                      "added monitor should be tracked in the session snapshot");
  if (added_monitor != nullptr) {
    failures += !Expect(added_monitor->is_present,
                        "added monitor should be active immediately");
    failures += !Expect(!added_monitor->locked,
                        "added monitor should default to unlocked");
    failures += !Expect(added_monitor->requires_confirmation,
                        "added monitor should require user confirmation");
  }

  auto restored_topology = locking_glass::core::SessionStore(session_path)
                               .Restore({left_monitor, right_monitor, new_monitor});
  failures += !Expect(restored_topology.restored_locked_monitors == 2U,
                      "reconnecting a saved monitor should reapply its lock state");

  const auto* reconnected_right =
      FindMonitorState(restored_topology.snapshot, right_monitor);
  failures += !Expect(reconnected_right != nullptr,
                      "reconnected monitor should still exist in the session snapshot");
  if (reconnected_right != nullptr) {
    failures += !Expect(reconnected_right->is_present,
                        "reconnected monitor should be active again");
    failures += !Expect(reconnected_right->locked,
                        "reconnected monitor should recover its saved lock state");
  }

  const auto invalid_backup_path =
      std::filesystem::path(session_path.string() + ".invalid");
  const std::string malformed_contents =
      "version\t1\n"
      "monitor\tbad-stable\tbad-device\tbad-serial\tDell U2720Q\tDisplay 9\t0\t0\t1920\t1080\t1\tnot-a-bool\t0\n";
  WriteTextFile(session_path, malformed_contents);

  auto malformed_restore =
      locking_glass::core::SessionStore(session_path).Restore({left_monitor});
  failures += !Expect(malformed_restore.loaded_from_disk,
                      "malformed session recovery should detect the edited session file");
  failures += !Expect(
      malformed_restore.storage_issue ==
          locking_glass::core::SessionStorageIssue::kMalformedRecord,
      "malformed session recovery should classify invalid monitor rows");
  failures += !Expect(malformed_restore.recovered_invalid_data,
                      "malformed session recovery should rebuild the active session file");
  failures += !Expect(
      malformed_restore.invalid_storage_backup_path == invalid_backup_path,
      "malformed session recovery should preserve the rejected file at a deterministic path");
  failures += !Expect(std::filesystem::exists(invalid_backup_path),
                      "malformed session recovery should emit the rejected-data backup");
  failures += !Expect(ReadTextFile(invalid_backup_path) == malformed_contents,
                      "rejected malformed session data should remain inspectable");
  failures += !Expect(malformed_restore.restored_locked_monitors == 0U,
                      "malformed session recovery should not trust rejected lock data");
  failures += !Expect(malformed_restore.new_monitors == 1U,
                      "malformed session recovery should rebuild from live monitors");
  failures += !Expect(malformed_restore.review_monitors == 1U,
                      "malformed session recovery should require user review");
  failures += !Expect(
      malformed_restore.storage_detail.find("malformed monitor record") !=
          std::string::npos,
      "malformed session recovery should explain why the stored data was rejected");

  const auto* malformed_left =
      FindMonitorState(malformed_restore.snapshot, left_monitor);
  failures += !Expect(malformed_left != nullptr,
                      "malformed session recovery should still return the live monitor");
  if (malformed_left != nullptr) {
    failures += !Expect(!malformed_left->locked,
                        "malformed session recovery should reset the live monitor to unlocked");
    failures += !Expect(malformed_left->requires_confirmation,
                        "malformed session recovery should require the user to reconfirm");
  }

  const std::string unsupported_contents = "version\t99\n";
  WriteTextFile(session_path, unsupported_contents);

  auto unsupported_preview =
      locking_glass::core::SessionStore(session_path).Preview({left_monitor});
  failures += !Expect(
      unsupported_preview.storage_issue ==
          locking_glass::core::SessionStorageIssue::kUnsupportedVersion,
      "preview should flag unsupported session format versions");
  failures += !Expect(!unsupported_preview.recovered_invalid_data,
                      "preview should inspect unsupported data without mutating it");
  failures += !Expect(ReadTextFile(session_path) == unsupported_contents,
                      "preview should leave unsupported session files untouched");

  auto unsupported_restore =
      locking_glass::core::SessionStore(session_path).Restore({left_monitor});
  failures += !Expect(
      unsupported_restore.storage_issue ==
          locking_glass::core::SessionStorageIssue::kUnsupportedVersion,
      "restore should classify unsupported session versions");
  failures += !Expect(unsupported_restore.recovered_invalid_data,
                      "restore should rebuild the session after an unsupported format");
  failures += !Expect(
      unsupported_restore.invalid_storage_backup_path == invalid_backup_path,
      "unsupported session recovery should reuse the deterministic rejected-data path");
  failures += !Expect(ReadTextFile(invalid_backup_path) == unsupported_contents,
                      "unsupported session recovery should preserve the rejected file contents");

  const auto repaired_preview =
      locking_glass::core::SessionStore(session_path).Preview({left_monitor});
  failures += !Expect(
      repaired_preview.storage_issue == locking_glass::core::SessionStorageIssue::kNone,
      "rebuilt session storage should deserialize cleanly after recovery");
  failures += !Expect(repaired_preview.review_monitors == 1U,
                      "rebuilt session storage should keep the live monitor pending review");

  std::filesystem::remove_all(temp_directory);
  return failures == 0;
}

bool RunMonitorWatchChecks() {
  int failures = 0;

  const auto temp_directory = MakeTempDirectory();
  const auto session_path = temp_directory / "watch-session-state.tsv";
  const auto script_path = temp_directory / "watch-script.tsv";
  WriteTextFile(
      script_path,
      "event\tstartup\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "monitor\tstable-right\tDISPLAY#RIGHT\tSERIAL-RIGHT\tDell U2720Q\tDisplay 2\t2560\t0\t5120\t1440\t0\n"
      "event\tWM_DISPLAYCHANGE\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "monitor\tstable-new\tDISPLAY#NEW\tSERIAL-NEW\tLG UltraFine\tDisplay 3\t-1920\t0\t0\t1080\t0\n");

  SetEnvironmentVariable("LOCKING_GLASS_SESSION_PATH", session_path.string());
  SetEnvironmentVariable("LOCKING_GLASS_MONITOR_SCRIPT", script_path.string());

  auto runtime = locking_glass::core::BuildRuntime();
  std::vector<locking_glass::core::MonitorRefreshReport> reports;
  std::string previous_fingerprint;

  const int watch_result = runtime.monitor_watcher->Watch(
      [&](const locking_glass::platform::MonitorWatchEvent& event) {
        auto report = locking_glass::core::RefreshMonitorState(
            runtime, event.monitors, event.trigger, previous_fingerprint);
        previous_fingerprint = report.topology_fingerprint;

        if (event.trigger == "startup") {
          auto snapshot = report.session.snapshot;
          const bool locked =
              runtime.session_store.SetLocked(&snapshot, event.monitors.front(), true);
          failures += !Expect(
              locked,
              "monitor watch startup event should be able to confirm and lock a monitor");
          failures += !Expect(runtime.session_store.Save(snapshot),
                              "monitor watch startup event should persist lock edits");
        }

        reports.push_back(std::move(report));
        return true;
      });

  failures += !Expect(watch_result == 0,
                      "scripted monitor watch should exit successfully");
  failures += !Expect(reports.size() == 2U,
                      "scripted monitor watch should emit both scripted events");
  if (reports.size() == 2U) {
    failures += !Expect(reports[0].trigger == "startup",
                        "first monitor watch event should be the startup snapshot");
    failures += !Expect(reports[0].topology_changed,
                        "startup snapshot should count as a topology change");
    failures += !Expect(reports[0].monitors.size() == 2U,
                        "startup snapshot should include the initial monitors");
    failures += !Expect(
        reports[0].session.new_monitors == 2U,
        "startup snapshot should treat both initial monitors as new session entries");
    failures += !Expect(reports[1].trigger == "WM_DISPLAYCHANGE",
                        "second monitor watch event should simulate WM_DISPLAYCHANGE");
    failures += !Expect(reports[1].topology_changed,
                        "topology change event should report a changed fingerprint");
    failures += !Expect(reports[1].session.restored_locked_monitors == 1U,
                        "topology change should restore the confirmed monitor lock");
    failures += !Expect(reports[1].session.disconnected_monitors == 1U,
                        "topology change should keep the removed monitor in session history");
    failures += !Expect(reports[1].session.new_monitors == 1U,
                        "topology change should record the added monitor");
    failures += !Expect(reports[1].session.review_monitors == 1U,
                        "added monitor should require user review");
    const auto formatted =
        locking_glass::core::FormatMonitorRefreshReport(reports[1]);
    failures += !Expect(
        formatted.find("Trigger:") != std::string::npos,
        "formatted monitor refresh output should include the trigger section");
    failures += !Expect(
        formatted.find("source: WM_DISPLAYCHANGE") != std::string::npos,
        "formatted monitor refresh output should include the event source");
    failures += !Expect(
        formatted.find("new monitors: 1") != std::string::npos,
        "formatted monitor refresh output should summarize added monitors");
    failures += !Expect(
        formatted.find("Prompt:") != std::string::npos,
        "formatted monitor refresh output should include a prompt section for new monitors");
    failures += !Expect(
        formatted.find("Review new monitor lock state") != std::string::npos,
        "formatted monitor refresh output should explain that the user needs to review the new monitor");
    failures += !Expect(
        formatted.find("Display 3") != std::string::npos,
        "formatted monitor refresh output should include the added monitor label");
  }

  if (!reports.empty() && reports.front().monitors.size() == 2U) {
    const auto reordered_fingerprint =
        locking_glass::core::BuildMonitorTopologyFingerprint(
            {reports.front().monitors.back(), reports.front().monitors.front()});
    failures += !Expect(
        reordered_fingerprint == reports.front().topology_fingerprint,
        "monitor topology fingerprint should be order-independent");
  }

  SetEnvironmentVariable("LOCKING_GLASS_MONITOR_SCRIPT", "");
  std::filesystem::remove_all(temp_directory);
  return failures == 0;
}

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
                        "startup snapshot should prompt for first-run monitor review");
    failures += !Expect(events[0].menu_title == "Keep selected monitors pinned",
                        "tray model should expose the tray menu title");
    failures += !Expect(events[0].tray_icon_variant == "review",
                        "new monitors should place the tray icon in review mode");
    failures += !Expect(events[0].tray_icon_review_badge,
                        "new monitors should add the tray review badge");
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
                          "new monitors should still be marked for review in the tray UI");
      failures += !Expect(opened_left->menu_label.find("2560x1440 @ 0,0, primary") !=
                              std::string::npos,
                          "primary tray monitors should expose layout metadata in the menu label");
      failures += !Expect(opened_left->padlock_variant == "unlocked",
                          "unlocked tray monitors should expose the unlocked padlock variant");
      failures += !Expect(opened_left->padlock_accent == "amber",
                          "review-required tray monitors should use the review padlock accent");
      failures += !Expect(!opened_left->padlock_filled,
                          "unlocked tray monitors should use an outline padlock");
      failures += !Expect(opened_left->padlock_review_badge,
                          "review-required tray monitors should expose the review badge");
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
    failures += !Expect(events[4].menu_subtitle.find("1 locked") !=
                            std::string::npos,
                        "toggle refresh should immediately update the tray summary counts");
    failures += !Expect(events[4].tray_icon_tooltip.find("1 of 2 locked") !=
                            std::string::npos,
                        "toggle refresh should immediately update the tray icon tooltip");
    failures += !Expect(HighlightTargets(events[5], "Display 2"),
                        "post-toggle hover should still target the toggled monitor");
    failures += !Expect(events[5].highlight.message.find(" | locked") !=
                            std::string::npos,
                        "post-toggle hover should describe the updated locked state");
    failures += !Expect(events[5].highlight.message.find("review required") ==
                            std::string::npos,
                        "post-toggle hover should clear the review-required text");

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
    failures += !Expect(events[8].highlight.message.find("unlocked, review required") !=
                            std::string::npos,
                        "hover overlays should describe both lock and review state");

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
                        "adding a new monitor should emit a review prompt");
    failures += !Expect(events[9].prompt.title == "Review new monitor lock state",
                        "single-monitor additions should use the singular review prompt");
    failures += !Expect(
        events[9].prompt.message.find("Display 3 - LG UltraFine") != std::string::npos,
        "review prompt should name the newly added monitor");
    failures += !Expect(FindPromptMonitor(events[9].prompt, "Display 3") != nullptr,
                        "review prompt should track the new monitor explicitly");

    const auto* added_monitor = FindBackgroundMonitor(events[9], "Display 3");
    failures += !Expect(added_monitor != nullptr,
                        "topology refresh should expose new monitors in the tray snapshot");
    if (added_monitor != nullptr) {
      failures += !Expect(!added_monitor->locked,
                          "new tray monitors should default to unlocked");
      failures += !Expect(added_monitor->requires_confirmation,
                          "new tray monitors should require review");
      failures += !Expect(added_monitor->padlock_variant == "unlocked",
                          "new tray monitors should expose the unlocked padlock variant");
      failures += !Expect(added_monitor->padlock_accent == "amber",
                          "new tray monitors should use the review padlock accent");
      failures += !Expect(!added_monitor->padlock_filled,
                          "new tray monitors should use an outline padlock icon");
      failures += !Expect(added_monitor->padlock_review_badge,
                          "new tray monitors should mark the padlock icon with the review badge");
      failures += !Expect(added_monitor->identify_label.find("Display 3") !=
                              std::string::npos,
                          "tray menu items should describe the identify-hover behavior");
      failures += !Expect(added_monitor->menu_label.find("1920x1080 @ -1920,0") !=
                              std::string::npos,
                          "new tray monitors should expose layout metadata in the menu label");
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
  failures += !Expect(preview.restored_locked_monitors == 1U,
                      "tray toggles should restore the reconnected monitor lock from the session store");
  const auto* persisted_right = FindMonitorState(preview.snapshot, right_monitor);
  failures += !Expect(persisted_right != nullptr,
                      "persisted session should still contain the reconnected monitor");
  if (persisted_right != nullptr) {
    failures += !Expect(persisted_right->is_present,
                        "persisted session should mark the reconnected monitor active");
    failures += !Expect(persisted_right->locked,
                        "persisted session should keep the reconnected monitor locked");
  }
  const auto formatted =
      locking_glass::core::FormatTrayMenuModel(
          locking_glass::core::BuildTrayMenuModel(preview, "verification"));
  failures += !Expect(formatted.find("LockingGlass tray menu") != std::string::npos,
                      "formatted tray menu output should include the tray heading");
  failures += !Expect(formatted.find("title: Keep selected monitors pinned") !=
                          std::string::npos,
                      "formatted tray menu output should include the tray title");
  failures += !Expect(formatted.find("variant: review") != std::string::npos,
                      "formatted tray menu output should include the tray icon variant");
  failures += !Expect(
      formatted.find("Display 1 - Dell U2720Q (2560x1440 @ 0,0, primary)") !=
                          std::string::npos,
                      "formatted tray menu output should include monitor labels");
  failures += !Expect(
      formatted.find("Display 3 - LG UltraFine (1920x1080 @ -1920,0) [review]") !=
                          std::string::npos,
                      "formatted tray menu output should flag review-required monitors");
  failures += !Expect(formatted.find("padlock: locked, emerald, filled") !=
                          std::string::npos,
                      "formatted tray menu output should describe locked padlock icons");
  failures += !Expect(
      formatted.find("padlock: unlocked, amber, outline, review badge") !=
          std::string::npos,
      "formatted tray menu output should describe review-state padlock icons");
  failures += !Expect(
      formatted.find("Hover highlights Display 3 on screen (LG UltraFine, 1920x1080 @ -1920,0)") !=
          std::string::npos,
                      "formatted tray menu output should describe the identify-hover affordance");

  SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT", "");
  std::filesystem::remove_all(temp_directory);
  return failures == 0;
}

bool RunBackgroundControllerStatusChecks() {
  int failures = 0;

  const auto temp_directory = MakeTempDirectory();
  const auto session_path = temp_directory / "background-controller-session.tsv";
  const auto script_path = temp_directory / "background-controller-tray.tsv";
  WriteTextFile(
      script_path,
      "event\tstartup\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "action\tclick\n"
      "action\ttoggle\tDisplay 1\n"
      "action\texit\n");

  SetEnvironmentVariable("LOCKING_GLASS_SESSION_PATH", session_path.string());
  SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT", script_path.string());
  SetEnvironmentVariable("LOCKING_GLASS_BACKGROUND_CONTROLLER_STATUS",
                         "unavailable:VirtualDesktopAccessor.dll missing");

  auto runtime = locking_glass::core::BuildRuntime();
  std::vector<locking_glass::platform::BackgroundSessionEvent> events;
  const int run_result = runtime.background_session->Run(
      [&](const locking_glass::platform::BackgroundSessionEvent& event) {
        events.push_back(event);
      });

  failures += !Expect(run_result == 0,
                      "scripted tray session should still exit successfully when the live controller is unavailable");
  failures += !Expect(events.size() == 4U,
                      "unavailable-controller tray script should emit startup, click, toggle, and exit events");
  if (events.size() == 4U) {
    failures += !Expect(!events[0].live_controller_available,
                        "startup tray state should admit that the live controller is unavailable");
    failures += !Expect(!events[0].live_controller_watcher_started,
                        "unavailable-controller startup should not claim the watcher is running");
    failures += !Expect(
        events[0].live_controller_detail.find("VirtualDesktopAccessor.dll missing") !=
            std::string::npos,
        "unavailable-controller tray state should surface the underlying controller detail");
    failures += !Expect(
        events[0].menu_subtitle.find("live controller unavailable") !=
            std::string::npos,
        "tray subtitle should stop implying that live desktop control is active");
    failures += !Expect(
        events[0].menu_instruction.find("Live desktop control unavailable") !=
            std::string::npos,
        "tray instruction should explain that live desktop switching will not follow toggles");
    failures += !Expect(
        events[0].tray_icon_tooltip.find("live desktop control unavailable") !=
            std::string::npos,
        "tray tooltip should expose the unavailable live-controller state");

    const auto* toggled_monitor = FindBackgroundMonitor(events[2], "Display 1");
    failures += !Expect(toggled_monitor != nullptr,
                        "tray toggle should still expose the monitor while the controller is unavailable");
    if (toggled_monitor != nullptr) {
      failures += !Expect(toggled_monitor->locked,
                          "tray toggle should still persist the requested lock state");
    }
    failures += !Expect(!events[2].live_controller_available,
                        "tray toggles should keep reporting the unavailable live-controller state");
  }

  const auto left_monitor =
      MakeMonitor("stable-left", "DISPLAY#LEFT", "SERIAL-LEFT", "Dell U2720Q",
                  "Display 1", 0, 0, 2560, 1440, true);
  const auto preview =
      locking_glass::core::SessionStore(session_path).Preview({left_monitor});
  failures += !Expect(preview.restored_locked_monitors == 1U,
                      "controller-unavailable tray toggles should still persist the saved lock state");

  SetEnvironmentVariable("LOCKING_GLASS_BACKGROUND_CONTROLLER_STATUS", "");
  SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT", "");
  std::filesystem::remove_all(temp_directory);
  return failures == 0;
}

bool RunDesktopLockingChecks() {
  int failures = 0;

  const auto temp_directory = MakeTempDirectory();
  const auto session_path = temp_directory / "desktop-session-state.tsv";
  const auto script_path = temp_directory / "desktop-script.tsv";
  WriteTextFile(
      script_path,
      "event\tdesktop-switch\tkeyboard\tdesktop-alpha\tdesktop-beta\n"
      "monitor\tstable-left\tDISPLAY#LEFT\tSERIAL-LEFT\tDell U2720Q\tDisplay 1\t0\t0\t2560\t1440\t1\n"
      "monitor\tstable-right\tDISPLAY#RIGHT\tSERIAL-RIGHT\tDell U2720Q\tDisplay 2\t2560\t0\t5120\t1440\t0\n"
      "window\tleft-alpha\tDocs\tstable-left\tDisplay 1\tdesktop-alpha\t1\t1\n"
      "window\tleft-beta\tChat\tstable-left\tDisplay 1\tdesktop-beta\t1\t1\n"
      "window\tright-alpha\tEditor\tstable-right\tDisplay 2\tdesktop-alpha\t1\t1\n"
      "window\tleft-child\tChild palette\tstable-left\tDisplay 1\tdesktop-alpha\t0\t1\n"
      "window\tleft-static\tPinned timer\tstable-left\tDisplay 1\tdesktop-alpha\t1\t0\n"
      "event\tdesktop-switch\tkeyboard-after-unlock\tdesktop-alpha\tdesktop-beta\n"
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
                        "locked monitor windows on both desktops should be swapped");
    failures += !Expect(locked_report.plan.skipped_windows.size() == 2U,
                        "non-top-level and immovable windows should be skipped");
    failures += !Expect(locked_report.move_results.size() == 2U,
                        "scripted replay should record both planned window moves");

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
      failures += !Expect(left_beta_move->to_desktop_id == "desktop-alpha",
                          "target desktop window should move back onto the source desktop");
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
        formatted_locked.find("LockingGlass desktop switch policy") !=
            std::string::npos,
        "formatted desktop report should include the policy heading");
    failures += !Expect(
        formatted_locked.find("planned moves: 2") != std::string::npos,
        "formatted desktop report should summarize planned moves");
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

}  // namespace

int main() {
  int failures = 0;

  const auto temp_directory = MakeTempDirectory();
  const auto diagnostics_session_path = temp_directory / "diagnostics-session-state.tsv";
  SetEnvironmentVariable("LOCKING_GLASS_SESSION_PATH",
                         diagnostics_session_path.string());

  const auto runtime = locking_glass::core::BuildRuntime();
  const std::string test_install_path =
      "C:\\Program Files\\LockingGlass\\LockingGlass.exe";
  const auto diagnostics =
      locking_glass::core::CollectStartupDiagnostics(runtime, test_install_path);
  const auto formatted = locking_glass::core::FormatDiagnostics(diagnostics);
  const auto prototype_monitors = std::vector<locking_glass::platform::MonitorDescriptor>{
      MakeMonitor("\\\\.\\DISPLAY1", "DISPLAY#LEFT", "SERIAL-LEFT",
                  "Dell U2720Q", "Display 1", 0, 0, 2560, 1440, true),
      MakeMonitor("\\\\.\\DISPLAY2", "DISPLAY#RIGHT", "SERIAL-RIGHT",
                  "Dell U2720Q", "Display 2", 2560, 0, 5120, 1440, false),
  };
  const auto windows_api_prototype =
      runtime.windows_api_probe->BuildPrototype(prototype_monitors);
  const auto formatted_prototype =
      locking_glass::integration::FormatWindowsApiPrototype(
          windows_api_prototype);

  failures += !Expect(diagnostics.capabilities.size() == 6U,
                      "startup diagnostics should expose exactly six capability probes");
  failures += !Expect(windows_api_prototype.boundaries.size() == 2U,
                      "windows API prototype should define the virtual desktop and monitor boundaries");
  failures += !Expect(formatted.find("background-session") != std::string::npos,
                      "formatted diagnostics should mention the background session capability");
  failures += !Expect(formatted.find("desktop-locking") != std::string::npos,
                      "formatted diagnostics should mention the desktop locking capability");
  failures += !Expect(formatted.find("windows-api") != std::string::npos,
                      "formatted diagnostics should mention the Windows API probe");
  failures += !Expect(formatted.find("autostart") != std::string::npos,
                      "formatted diagnostics should mention the autostart capability");
  failures += !Expect(formatted.find("ffmpeg") != std::string::npos,
                      "formatted diagnostics should mention the FFmpeg probe");
  failures += !Expect(formatted.find("session-store") != std::string::npos,
                      "formatted diagnostics should mention the session store capability");
  failures += !Expect(formatted.find("Session:") != std::string::npos,
                      "formatted diagnostics should include a session summary section");
  failures += !Expect(
      formatted.find("\"C:\\Program Files\\LockingGlass\\LockingGlass.exe\" --background") !=
          std::string::npos,
      "formatted diagnostics should expose the quoted Windows autostart command");
  failures += !Expect(
      formatted_prototype.find("virtual-desktop-control") != std::string::npos,
      "formatted prototype should name the virtual desktop boundary");
  failures += !Expect(
      formatted_prototype.find("monitor-enumeration") != std::string::npos,
      "formatted prototype should name the monitor enumeration boundary");
  failures += !Expect(formatted_prototype.find("IVirtualDesktopManager") !=
                          std::string::npos,
                      "formatted prototype should document the supported virtual desktop COM API");
  failures += !Expect(formatted_prototype.find("VirtualDesktopAccessor") !=
                          std::string::npos,
                      "formatted prototype should document the helper seam for missing desktop controls");
  failures += !Expect(
      formatted_prototype.find("EnumDisplayMonitors") != std::string::npos,
      "formatted prototype should document Win32 monitor enumeration");
  failures += !Expect(
      formatted_prototype.find("QueryDisplayConfig") != std::string::npos,
      "formatted prototype should document persistent monitor identity lookup");
  failures += !Expect(formatted_prototype.find("WM_DISPLAYCHANGE") !=
                          std::string::npos,
                      "formatted prototype should document topology refresh behavior");
  failures += !Expect(
      formatted_prototype.find("Observed 2 active monitors") != std::string::npos,
      "formatted prototype should include the observed monitor count");
  failures += !Expect(diagnostics.session.storage_path == diagnostics_session_path,
                      "startup diagnostics should use the configured session storage path");
  failures += !Expect(!diagnostics.session.loaded_from_disk,
                      "startup diagnostics should report an empty temp session on first load");
  failures += !Expect(
      diagnostics.session.storage_issue ==
          locking_glass::core::SessionStorageIssue::kNone,
      "startup diagnostics should not report a storage issue for a missing session file");
  failures += !Expect(
      diagnostics.session.storage_detail.find("No saved session file found") !=
          std::string::npos,
      "startup diagnostics should explain when the session store is bootstrapping");
  failures += !Expect(formatted.find("storage issue: none") != std::string::npos,
                      "formatted diagnostics should show the persistence issue state");

  const auto* background_session =
      FindCapability(diagnostics, "background-session");
  failures +=
      !Expect(background_session != nullptr, "background-session capability should exist");
  if (background_session != nullptr) {
#if defined(_WIN32)
    failures += !Expect(background_session->status !=
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "background-session should not be stubbed on Windows");
#else
    failures += !Expect(background_session->status ==
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "background-session should be stubbed on non-Windows hosts");
#endif
    failures += !Expect(!background_session->detail.empty(),
                        "background-session capability should provide a diagnostic detail");
  }

  const auto* desktop_locking = FindCapability(diagnostics, "desktop-locking");
  failures += !Expect(desktop_locking != nullptr,
                      "desktop-locking capability should exist");
  if (desktop_locking != nullptr) {
#if defined(_WIN32)
    failures += !Expect(desktop_locking->status !=
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "desktop-locking should not be stubbed on Windows");
#else
    failures += !Expect(desktop_locking->status ==
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "desktop-locking should be stubbed on non-Windows hosts");
#endif
    failures += !Expect(!desktop_locking->detail.empty(),
                        "desktop-locking capability should provide a diagnostic detail");
  }

  const auto* windows_api = FindCapability(diagnostics, "windows-api");
  failures += !Expect(windows_api != nullptr, "windows-api capability should exist");
  if (windows_api != nullptr) {
#if defined(_WIN32)
    failures += !Expect(windows_api->status !=
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "windows-api should not be stubbed on Windows");
#else
    failures += !Expect(windows_api->status ==
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "windows-api should be stubbed on non-Windows hosts");
#endif
    failures += !Expect(!windows_api->detail.empty(),
                        "windows-api capability should provide a diagnostic detail");
  }

  const auto* autostart = FindCapability(diagnostics, "autostart");
  failures += !Expect(autostart != nullptr, "autostart capability should exist");
  if (autostart != nullptr) {
#if defined(_WIN32)
    failures += !Expect(autostart->status !=
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "autostart should not be stubbed on Windows");
#else
    failures += !Expect(autostart->status ==
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "autostart should be stubbed on non-Windows hosts");
#endif
    failures += !Expect(!autostart->detail.empty(),
                        "autostart capability should provide a diagnostic detail");
  }

  const auto* ffmpeg = FindCapability(diagnostics, "ffmpeg");
  failures += !Expect(ffmpeg != nullptr, "ffmpeg capability should exist");
  if (ffmpeg != nullptr) {
    const char* injected_library = std::getenv("LOCKING_GLASS_FFMPEG_LIBRARY");
    failures += !Expect(injected_library != nullptr && injected_library[0] != '\0',
                        "LOCKING_GLASS_FFMPEG_LIBRARY should be injected by the build");
    failures += !Expect(ffmpeg->status == locking_glass::integration::CapabilityStatus::kReady,
                        "ffmpeg probe should load the injected fake avutil runtime");
    failures += !Expect(ffmpeg->detail.find("fake-ffmpeg-1.0") != std::string::npos,
                        "ffmpeg diagnostic should include the fake avutil version string");
  }

  const auto* session_store = FindCapability(diagnostics, "session-store");
  failures += !Expect(session_store != nullptr, "session-store capability should exist");
  if (session_store != nullptr) {
    failures += !Expect(session_store->status ==
                            locking_glass::integration::CapabilityStatus::kReady,
                        "session-store should be ready on the host build");
    failures += !Expect(session_store->detail.find(
                            diagnostics_session_path.string()) != std::string::npos,
                        "session-store capability should report the session file path");
  }

  const auto quoted_path =
      locking_glass::integration::QuoteWindowsCommandArg(test_install_path);
  failures += !Expect(
      quoted_path == "\"C:\\Program Files\\LockingGlass\\LockingGlass.exe\"",
      "QuoteWindowsCommandArg should quote Windows paths that contain spaces");
  failures += !Expect(diagnostics.autostart.entry_name == "LockingGlass",
                      "autostart diagnostics should use the LockingGlass Run entry name");
  failures += !Expect(
      diagnostics.autostart.launch_command ==
          "\"C:\\Program Files\\LockingGlass\\LockingGlass.exe\" --background",
      "autostart diagnostics should launch the executable in background mode");
  failures += !Expect(RunSessionStoreChecks(),
                      "session store should persist locks across restart and topology changes");
  failures += !Expect(RunMonitorWatchChecks(),
                      "monitor watch should reconcile scripted topology changes");
  failures += !Expect(RunTraySessionChecks(),
                      "background session should expose tray clicks and lock toggles");
  failures += !Expect(
      RunBackgroundControllerStatusChecks(),
      "background session should report unavailable live-controller states honestly");
  failures += !Expect(
      RunDesktopLockingChecks(),
      "virtual desktop watch should preserve locked monitors and honor unlocks");

  std::filesystem::remove_all(temp_directory);

  if (failures == 0) {
    std::cout << "wiring_test: ok\n";
  }

  return failures == 0 ? 0 : 1;
}
