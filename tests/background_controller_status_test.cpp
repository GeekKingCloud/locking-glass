#include "test_helpers.h"

#include "../src/platform/background_session_internal.h"

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
  SetEnvironmentVariable("LOCKING_GLASS_BACKGROUND_CONTROLLER_STATUS", "ready");
  failures += !Expect(
      !locking_glass::platform::internal::
           ResolveBackgroundControllerCapabilityOverride()
               .has_value(),
      "background controller env override should not accept synthetic ready status");

  SetEnvironmentVariable("LOCKING_GLASS_BACKGROUND_CONTROLLER_STATUS",
                         "unavailable:VirtualDesktopAccessor.dll missing");

  auto runtime = locking_glass::core::BuildRuntime();
  std::vector<locking_glass::platform::BackgroundSessionEvent> events;
  const int run_result = runtime.background_session->Run(
      [&](const locking_glass::platform::BackgroundSessionEvent& event) {
        events.push_back(event);
      });

  failures += !Expect(run_result != 0,
                      "scripted tray session should fail closed when the live controller is unavailable");
  failures += !Expect(events.empty(),
                      "unavailable-controller tray script should not publish an interactive tray model");

  const auto left_monitor =
      MakeMonitor("stable-left", "DISPLAY#LEFT", "SERIAL-LEFT", "Dell U2720Q",
                  "Display 1", 0, 0, 2560, 1440, true);
  const auto preview =
      locking_glass::core::SessionStore(session_path).Preview({left_monitor});
  failures += !Expect(preview.restored_locked_monitors == 0U,
                      "controller-unavailable tray toggles should not persist a saved lock state");

  SetEnvironmentVariable("LOCKING_GLASS_BACKGROUND_CONTROLLER_STATUS", "");
  SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT", "");
  std::filesystem::remove_all(temp_directory);
  return failures == 0;
}

}  // namespace locking_glass::tests
