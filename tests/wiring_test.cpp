#include "locking_glass/core/runtime.h"
#include "locking_glass/core/session_store.h"
#include "locking_glass/integration/autostart.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

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

  failures += !Expect(diagnostics.capabilities.size() == 5U,
                      "startup diagnostics should expose exactly five capability probes");
  failures += !Expect(windows_api_prototype.boundaries.size() == 2U,
                      "windows API prototype should define the virtual desktop and monitor boundaries");
  failures += !Expect(formatted.find("background-session") != std::string::npos,
                      "formatted diagnostics should mention the background session capability");
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

  std::filesystem::remove_all(temp_directory);

  if (failures == 0) {
    std::cout << "wiring_test: ok\n";
  }

  return failures == 0 ? 0 : 1;
}
