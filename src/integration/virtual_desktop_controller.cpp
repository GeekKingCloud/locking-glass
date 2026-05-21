#include "virtual_desktop_controller_internal.h"

#include <cstdlib>
#include <cctype>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace locking_glass::integration::internal {

std::vector<std::string> SplitFields(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t end = line.find('\t', start);
    if (end == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }

    fields.push_back(line.substr(start, end - start));
    start = end + 1;
  }
  return fields;
}

bool ParseBoolField(const std::string& field, bool* value) {
  if (field == "1" || field == "true") {
    *value = true;
    return true;
  }
  if (field == "0" || field == "false") {
    *value = false;
    return true;
  }
  return false;
}

bool ParseIntField(const std::string& field, int* value) {
  try {
    std::size_t consumed = 0;
    const int parsed = std::stoi(field, &consumed);
    if (consumed != field.size()) {
      return false;
    }
    *value = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool GuidEquals(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto left_char = static_cast<unsigned char>(left[index]);
    const auto right_char = static_cast<unsigned char>(right[index]);
    if (std::tolower(left_char) != std::tolower(right_char)) {
      return false;
    }
  }
  return true;
}

std::string BuildDesktopDisplayId(int desktop_number,
                                  const std::string& desktop_guid,
                                  const std::string& desktop_name) {
  std::ostringstream builder;
  if (desktop_number >= 0) {
    builder << "Desktop " << (desktop_number + 1) << " [" << desktop_number
            << "]";
  } else {
    builder << "<unknown-desktop>";
  }

  if (!desktop_name.empty()) {
    builder << " \"" << desktop_name << "\"";
  }
  if (!desktop_guid.empty()) {
    builder << " {" << desktop_guid << "}";
  }
  return builder.str();
}

DesktopIdentity MakeDesktopIdentity(int desktop_number, std::string desktop_guid,
                                    std::string desktop_name) {
  DesktopIdentity desktop{
      .number = desktop_number,
      .guid = std::move(desktop_guid),
      .name = std::move(desktop_name),
      .display_id = {},
  };
  desktop.display_id =
      BuildDesktopDisplayId(desktop.number, desktop.guid, desktop.name);
  return desktop;
}

DesktopIdentity MakeDisplayOnlyDesktopIdentity(std::string display_id) {
  return DesktopIdentity{
      .number = -1,
      .guid = {},
      .name = {},
      .display_id = std::move(display_id),
  };
}

bool DesktopIdentityEquals(const DesktopIdentity& left,
                           const DesktopIdentity& right) {
  // Prefer the strongest available identity first so remembered desktops stay
  // stable even if display labels change across runs or helper surfaces.
  if (!left.guid.empty() && !right.guid.empty()) {
    return GuidEquals(left.guid, right.guid);
  }

  if (left.number >= 0 && right.number >= 0) {
    if (!left.name.empty() && !right.name.empty()) {
      return left.number == right.number && left.name == right.name;
    }
    return left.number == right.number;
  }

  if (!left.display_id.empty() && !right.display_id.empty()) {
    return left.display_id == right.display_id;
  }

  if (!left.name.empty() && !right.name.empty()) {
    return left.name == right.name;
  }

  return false;
}

const DesktopIdentity* FindMatchingDesktop(
    const std::vector<DesktopIdentity>& desktops,
    const DesktopIdentity& remembered_desktop) {
  for (const auto& desktop : desktops) {
    if (DesktopIdentityEquals(desktop, remembered_desktop)) {
      return &desktop;
    }
  }
  return nullptr;
}

std::string DescribeWindow(const core::DesktopWindow& window) {
  if (!window.title.empty()) {
    return window.title;
  }
  if (!window.window_id.empty()) {
    return window.window_id;
  }
  return "<unnamed>";
}

std::string DescribeMonitor(const core::DesktopWindow& window) {
  if (!window.monitor_label.empty()) {
    return window.monitor_label;
  }
  if (!window.monitor_id.empty()) {
    return window.monitor_id;
  }
  return "<unknown-monitor>";
}

bool WindowMatchesMonitor(const core::DesktopWindow& window,
                          const platform::MonitorDescriptor& monitor) {
  if (!window.monitor_id.empty() &&
      (window.monitor_id == monitor.stable_id ||
       window.monitor_id == monitor.device_path ||
       window.monitor_id == monitor.label)) {
    return true;
  }

  return !window.monitor_label.empty() && window.monitor_label == monitor.label;
}

const CapturedWindow* FindCapturedWindow(
    const std::vector<CapturedWindow>& windows, const std::string& window_id) {
  for (const auto& window : windows) {
    if (window.window.window_id == window_id) {
      return &window;
    }
  }
  return nullptr;
}

core::DesktopWindow* FindReturnResultWindow(
    std::vector<core::DesktopWindow>* windows, const std::string& window_id) {
  if (windows == nullptr) {
    return nullptr;
  }

  for (auto& window : *windows) {
    if (window.window_id == window_id) {
      return &window;
    }
  }
  return nullptr;
}

core::DesktopWindow* FindResultWindow(
    std::vector<core::DesktopWindow>* windows,
    const core::MonitorLockingMove& move) {
  for (auto& window : *windows) {
    if (window.window_id == move.window.window_id &&
        window.monitor_id == move.window.monitor_id &&
        window.monitor_label == move.window.monitor_label &&
        window.desktop_id == move.from_desktop_id) {
      return &window;
    }
  }
  return nullptr;
}

UnlockReturnReport BuildUnlockReturnReport(
    const UnlockReturnRequest& request,
    const std::vector<DesktopIdentity>& available_desktops,
    const std::vector<CapturedWindow>& current_windows,
    const std::function<WindowMoveResult(const CapturedWindow&,
                                         const DesktopIdentity&)>& attempt_move) {
  UnlockReturnReport report{
      .monitor = request.monitor,
      .move_results = {},
      .skipped_windows = {},
      .resulting_windows = {},
  };

  report.resulting_windows.reserve(current_windows.size());
  for (const auto& current_window : current_windows) {
    report.resulting_windows.push_back(current_window.window);
  }

  for (const auto& tracked_window : request.tracked_windows) {
    const auto* current_window =
        FindCapturedWindow(current_windows, tracked_window.window.window_id);
    if (current_window == nullptr) {
      report.skipped_windows.push_back(UnlockReturnSkip{
          .window = tracked_window.window,
          .current_desktop = {},
          .home_desktop = tracked_window.home_desktop,
          .reason = "window was missing from the live snapshot",
      });
      continue;
    }

    if (!WindowMatchesMonitor(current_window->window, request.monitor)) {
      report.skipped_windows.push_back(UnlockReturnSkip{
          .window = current_window->window,
          .current_desktop = current_window->desktop,
          .home_desktop = tracked_window.home_desktop,
          .reason = "window is no longer on the unlocked monitor",
      });
      continue;
    }

    if (current_window->window.desktop_id.empty()) {
      report.skipped_windows.push_back(UnlockReturnSkip{
          .window = current_window->window,
          .current_desktop = current_window->desktop,
          .home_desktop = tracked_window.home_desktop,
          .reason = "window desktop could not be resolved safely",
      });
      continue;
    }

    const auto* remembered_desktop =
        FindMatchingDesktop(available_desktops, tracked_window.home_desktop);
    if (remembered_desktop == nullptr) {
      report.skipped_windows.push_back(UnlockReturnSkip{
          .window = current_window->window,
          .current_desktop = current_window->desktop,
          .home_desktop = tracked_window.home_desktop,
          .reason = "remembered desktop no longer exists",
      });
      continue;
    }

    // Return moves are intentionally conservative: if the current desktop
    // state cannot be classified safely, skip and report instead of guessing.
    if (DesktopIdentityEquals(current_window->desktop, *remembered_desktop)) {
      report.skipped_windows.push_back(UnlockReturnSkip{
          .window = current_window->window,
          .current_desktop = current_window->desktop,
          .home_desktop = *remembered_desktop,
          .reason = "window is already on its remembered desktop",
      });
      continue;
    }

    if (!current_window->window.is_top_level) {
      report.skipped_windows.push_back(UnlockReturnSkip{
          .window = current_window->window,
          .current_desktop = current_window->desktop,
          .home_desktop = *remembered_desktop,
          .reason = "window is not top-level",
      });
      continue;
    }

    if (!current_window->window.can_move) {
      report.skipped_windows.push_back(UnlockReturnSkip{
          .window = current_window->window,
          .current_desktop = current_window->desktop,
          .home_desktop = *remembered_desktop,
          .reason = current_window->move_block_reason.empty()
                        ? "window cannot be moved"
                        : current_window->move_block_reason,
      });
      continue;
    }

    WindowMoveResult move_result =
        attempt_move(*current_window, *remembered_desktop);
    if (move_result.success) {
      if (auto* result_window = FindReturnResultWindow(
              &report.resulting_windows, current_window->window.window_id);
          result_window != nullptr) {
        result_window->desktop_id = move_result.to_desktop_id;
      }
    }
    report.move_results.push_back(std::move(move_result));
  }

  return report;
}

}  // namespace locking_glass::integration::internal

namespace locking_glass::integration {

namespace {

constexpr char kDesktopReturnScriptEnv[] =
    "LOCKING_GLASS_DESKTOP_RETURN_SCRIPT";

class VirtualDesktopControllerImpl final : public VirtualDesktopController {
 public:
  CapabilityReport Probe() const override {
#if defined(_WIN32)
    return internal::ProbeWindowsController();
#else
    return CapabilityReport{
        .component = "desktop-locking",
        .status = CapabilityStatus::kStubbed,
        .detail =
            "Desktop locking is stubbed on non-Windows hosts; LOCKING_GLASS_DESKTOP_SCRIPT remains a replay seam for policy checks only and does not prove the live Windows hook path.",
    };
#endif
  }

  bool CleanupStagingDesktop() const override {
#if defined(_WIN32)
    return internal::CleanupWindowsStagingDesktop();
#else
    return true;
#endif
  }

  UnlockReturnReport ReturnTrackedWindows(
      const UnlockReturnRequest& request) const override {
    const char* replay_path = std::getenv(kDesktopReturnScriptEnv);
    if (replay_path != nullptr && replay_path[0] != '\0') {
      const auto replay_state = internal::LoadDesktopReturnScript(replay_path);
      return internal::BuildScriptedUnlockReturnReport(request, replay_state);
    }

#if defined(_WIN32)
    return internal::BuildWindowsUnlockReturnReport(request);
#else
    UnlockReturnReport report{
        .monitor = request.monitor,
        .move_results = {},
        .skipped_windows = {},
        .resulting_windows = {},
    };
    for (const auto& tracked_window : request.tracked_windows) {
      report.skipped_windows.push_back(UnlockReturnSkip{
          .window = tracked_window.window,
          .current_desktop = {},
          .home_desktop = tracked_window.home_desktop,
          .reason =
              "desktop return replay is not configured on this non-Windows host",
      });
    }
    return report;
#endif
  }

  int WatchSwitches(const core::SessionStore& store,
                    const DesktopSwitchCallback& callback,
                    const DesktopWatchOptions options) const override {
    const char* script_path = std::getenv("LOCKING_GLASS_DESKTOP_SCRIPT");
    if (options.allow_script_replay && script_path != nullptr &&
        script_path[0] != '\0') {
      const auto scenarios = internal::LoadDesktopScript(script_path);
      if (scenarios.empty()) {
        return 1;
      }

      for (const auto& scenario : scenarios) {
        if (!callback(internal::BuildDesktopSwitchReport(store, scenario))) {
          break;
        }
      }

      return 0;
    }

#if defined(_WIN32)
    return internal::WatchWindowsLiveSwitches(store, callback, options);
#else
    return 1;
#endif
  }
};

}  // namespace

std::string FormatDesktopIdentity(const DesktopIdentity& desktop) {
  if (!desktop.display_id.empty()) {
    return desktop.display_id;
  }
  return internal::BuildDesktopDisplayId(desktop.number, desktop.guid,
                                         desktop.name);
}

std::string FormatDesktopSwitchReport(const DesktopSwitchReport& report) {
  std::ostringstream builder;
  builder << core::FormatMonitorLockingPlan(report.plan);

  builder << "Move results:\n";
  if (report.move_results.empty()) {
    builder << "  - none\n";
  } else {
    for (const auto& result : report.move_results) {
      builder << "  - " << internal::DescribeWindow(result.window) << " ["
              << internal::DescribeMonitor(result.window) << "] "
              << result.from_desktop_id << " -> " << result.to_desktop_id
              << " : " << (result.success ? "moved" : "failed")
              << " (" << result.detail << ")\n";
    }
  }

  builder << "Resulting windows:\n";
  if (report.resulting_windows.empty()) {
    builder << "  - none\n";
  } else {
    for (const auto& window : report.resulting_windows) {
      builder << "  - " << internal::DescribeWindow(window) << " ["
              << internal::DescribeMonitor(window) << "] on " << window.desktop_id
              << '\n';
    }
  }

  return builder.str();
}

std::string FormatUnlockReturnReport(const UnlockReturnReport& report) {
  std::ostringstream builder;
  builder << "Locking Glass unlock return\n";
  builder << "Monitor:\n";
  builder << "  - label: " << report.monitor.label << '\n';
  builder << "  - id: " << report.monitor.stable_id << '\n';
  builder << "Summary:\n";
  builder << "  - return moves: " << report.move_results.size() << '\n';
  builder << "  - skipped windows: " << report.skipped_windows.size() << '\n';

  builder << "Move results:\n";
  if (report.move_results.empty()) {
    builder << "  - none\n";
  } else {
    for (const auto& result : report.move_results) {
      builder << "  - " << internal::DescribeWindow(result.window) << " ["
              << internal::DescribeMonitor(result.window) << "] "
              << FormatDesktopIdentity(result.from_desktop) << " -> "
              << FormatDesktopIdentity(result.to_desktop) << " : "
              << (result.success ? "moved" : "failed") << " (" << result.detail
              << ")\n";
    }
  }

  builder << "Skipped:\n";
  if (report.skipped_windows.empty()) {
    builder << "  - none\n";
  } else {
    for (const auto& skipped : report.skipped_windows) {
      builder << "  - " << internal::DescribeWindow(skipped.window) << " ["
              << internal::DescribeMonitor(skipped.window) << "] on "
              << FormatDesktopIdentity(skipped.current_desktop) << " -> "
              << FormatDesktopIdentity(skipped.home_desktop) << " : "
              << skipped.reason << '\n';
    }
  }

  builder << "Resulting windows:\n";
  if (report.resulting_windows.empty()) {
    builder << "  - none\n";
  } else {
    for (const auto& window : report.resulting_windows) {
      builder << "  - " << internal::DescribeWindow(window) << " ["
              << internal::DescribeMonitor(window) << "] on " << window.desktop_id
              << '\n';
    }
  }

  return builder.str();
}

std::unique_ptr<VirtualDesktopController> CreateVirtualDesktopController() {
  return std::make_unique<VirtualDesktopControllerImpl>();
}

}  // namespace locking_glass::integration
