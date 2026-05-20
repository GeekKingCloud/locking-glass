#include "test_helpers.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

namespace locking_glass::tests {

bool Expect(const bool condition, const std::string& message) {
  if (condition) {
    return true;
  }

  std::cerr << "Expectation failed: " << message << '\n';
  return false;
}

void RunCheckGroup(const std::string_view label, bool (*check)(), int* failures) {
  if (failures == nullptr) {
    return;
  }

  ClearLockingGlassTestEnvironment();
  std::cout << "locking_glass_tests: running " << label << " checks...\n";
  if (check()) {
    ClearLockingGlassTestEnvironment();
    return;
  }

  ClearLockingGlassTestEnvironment();
  ++(*failures);
  std::cerr << "locking_glass_tests: " << label << " checks failed\n";
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

void ClearLockingGlassTestEnvironment() {
  SetEnvironmentVariable("LOCKING_GLASS_BACKGROUND_CONTROLLER_STATUS", "");
  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_RETURN_SCRIPT", "");
  SetEnvironmentVariable("LOCKING_GLASS_DESKTOP_SCRIPT", "");
  SetEnvironmentVariable("LOCKING_GLASS_MONITOR_SCRIPT", "");
  SetEnvironmentVariable("LOCKING_GLASS_SESSION_PATH", "");
  SetEnvironmentVariable("LOCKING_GLASS_TRAY_SCRIPT", "");
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
    if (locking_glass::core::ExactMonitorIdentityEqual(monitor_state.monitor,
                                                       monitor)) {
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

const locking_glass::integration::WindowMoveResult* FindUnlockMoveResult(
    const locking_glass::integration::UnlockReturnReport& report,
    const std::string& window_id) {
  for (const auto& result : report.move_results) {
    if (result.window.window_id == window_id) {
      return &result;
    }
  }
  return nullptr;
}

const locking_glass::integration::UnlockReturnSkip* FindUnlockSkip(
    const locking_glass::integration::UnlockReturnReport& report,
    const std::string& window_id) {
  for (const auto& skipped : report.skipped_windows) {
    if (skipped.window.window_id == window_id) {
      return &skipped;
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

locking_glass::integration::DesktopIdentity MakeDesktopIdentity(
    const int number, std::string guid, std::string name) {
  locking_glass::integration::DesktopIdentity desktop{
      .number = number,
      .guid = std::move(guid),
      .name = std::move(name),
      .display_id = {},
  };
  desktop.display_id = locking_glass::integration::FormatDesktopIdentity(desktop);
  return desktop;
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

void ExpectWindowsAwareCapability(
    const locking_glass::core::StartupDiagnostics& diagnostics,
    const std::string& component, int* failures) {
  if (failures == nullptr) {
    return;
  }

  const auto* capability = FindCapability(diagnostics, component);
  *failures += !Expect(capability != nullptr, component + " capability should exist");
  if (capability == nullptr) {
    return;
  }

#if defined(_WIN32)
  *failures += !Expect(capability->status !=
                           locking_glass::integration::CapabilityStatus::kStubbed,
                       component + " should not be stubbed on Windows");
#else
  *failures += !Expect(capability->status ==
                           locking_glass::integration::CapabilityStatus::kStubbed,
                       component + " should be stubbed on non-Windows hosts");
#endif
  *failures += !Expect(!capability->detail.empty(),
                       component + " should provide a diagnostic detail");
}

}  // namespace locking_glass::tests
