#include "test_helpers.h"

namespace locking_glass::tests {

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
        events[0].menu_status.find("live controller unavailable") !=
            std::string::npos,
        "tray status should stop implying that live desktop control is active");
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

}  // namespace locking_glass::tests
