#include "test_helpers.h"

#include <utility>

namespace locking_glass::tests {

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
                        "added monitor should require user confirmation");
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
        formatted.find("Confirm monitor") != std::string::npos,
        "formatted monitor refresh output should explain that the user needs to confirm the added monitor");
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

}  // namespace locking_glass::tests
