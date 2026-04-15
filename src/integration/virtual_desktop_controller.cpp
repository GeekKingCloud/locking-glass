#include "locking_glass/integration/virtual_desktop_controller.h"

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <objbase.h>
#include <shobjidl_core.h>
#endif

namespace locking_glass::integration {

namespace {

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

#if defined(_WIN32)
CapabilityReport ProbeWindowsController() {
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool com_ready = SUCCEEDED(com_result) || com_result == RPC_E_CHANGED_MODE;

  IVirtualDesktopManager* desktop_manager = nullptr;
  bool desktop_manager_ready = false;
  if (com_ready) {
    const HRESULT desktop_result =
        CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&desktop_manager));
    desktop_manager_ready =
        SUCCEEDED(desktop_result) && desktop_manager != nullptr;
  }

  if (desktop_manager != nullptr) {
    desktop_manager->Release();
  }
  if (com_result == S_OK || com_result == S_FALSE) {
    CoUninitialize();
  }

  if (desktop_manager_ready) {
    return CapabilityReport{
        .component = "desktop-locking",
        .status = CapabilityStatus::kReady,
        .detail =
            "Monitor lock planning can target IVirtualDesktopManager moves; LOCKING_GLASS_DESKTOP_SCRIPT replays switch scenarios while live notifications stay behind the helper seam.",
    };
  }

  return CapabilityReport{
      .component = "desktop-locking",
      .status = CapabilityStatus::kUnavailable,
      .detail =
          "Desktop locking is unavailable because IVirtualDesktopManager could not be resolved through COM.",
  };
}
#endif

std::vector<core::DesktopSwitchScenario> LoadDesktopScript(
    const std::string& script_path) {
  std::ifstream input(script_path);
  if (!input.is_open()) {
    return {};
  }

  std::vector<core::DesktopSwitchScenario> scenarios;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const auto fields = SplitFields(line);
    if (fields.empty()) {
      continue;
    }

    if (fields[0] == "event" && fields.size() == 5U &&
        fields[1] == "desktop-switch") {
      scenarios.push_back(core::DesktopSwitchScenario{
          .trigger = fields[2],
          .source_desktop_id = fields[3],
          .target_desktop_id = fields[4],
          .monitors = {},
          .windows = {},
      });
      continue;
    }

    if (fields[0] == "monitor" && fields.size() == 11U && !scenarios.empty()) {
      bool is_primary = false;
      platform::MonitorBounds bounds;
      if (!ParseIntField(fields[6], &bounds.left) ||
          !ParseIntField(fields[7], &bounds.top) ||
          !ParseIntField(fields[8], &bounds.right) ||
          !ParseIntField(fields[9], &bounds.bottom) ||
          !ParseBoolField(fields[10], &is_primary)) {
        return {};
      }

      scenarios.back().monitors.push_back(platform::MonitorDescriptor{
          .stable_id = fields[1],
          .device_path = fields[2],
          .edid_serial = fields[3],
          .display_name = fields[4],
          .label = fields[5],
          .bounds = bounds,
          .is_primary = is_primary,
      });
      continue;
    }

    if (fields[0] == "window" && fields.size() == 8U && !scenarios.empty()) {
      bool is_top_level = false;
      bool can_move = false;
      if (!ParseBoolField(fields[6], &is_top_level) ||
          !ParseBoolField(fields[7], &can_move)) {
        return {};
      }

      scenarios.back().windows.push_back(core::DesktopWindow{
          .window_id = fields[1],
          .title = fields[2],
          .monitor_id = fields[3],
          .monitor_label = fields[4],
          .desktop_id = fields[5],
          .is_top_level = is_top_level,
          .can_move = can_move,
      });
      continue;
    }

    return {};
  }

  return scenarios;
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

DesktopSwitchReport BuildDesktopSwitchReport(
    const core::SessionStore& store,
    const core::DesktopSwitchScenario& scenario) {
  DesktopSwitchReport report{
      .plan = core::BuildMonitorLockingPlan(store, scenario),
      .move_results = {},
      .resulting_windows = scenario.windows,
  };

  for (const auto& move : report.plan.moves) {
    auto* result_window = FindResultWindow(&report.resulting_windows, move);
    if (result_window == nullptr) {
      report.move_results.push_back(WindowMoveResult{
          .window = move.window,
          .from_desktop_id = move.from_desktop_id,
          .to_desktop_id = move.to_desktop_id,
          .success = false,
          .detail = "window was missing from the replay snapshot",
      });
      continue;
    }

    result_window->desktop_id = move.to_desktop_id;
    report.move_results.push_back(WindowMoveResult{
        .window = move.window,
        .from_desktop_id = move.from_desktop_id,
        .to_desktop_id = move.to_desktop_id,
        .success = true,
        .detail = "scripted move applied",
    });
  }

  return report;
}

class VirtualDesktopControllerImpl final : public VirtualDesktopController {
 public:
  CapabilityReport Probe() const override {
#if defined(_WIN32)
    return ProbeWindowsController();
#else
    return CapabilityReport{
        .component = "desktop-locking",
        .status = CapabilityStatus::kStubbed,
        .detail =
            "Desktop locking is stubbed on non-Windows hosts; set LOCKING_GLASS_DESKTOP_SCRIPT to replay desktop-switch scenarios through the core policy.",
    };
#endif
  }

  int WatchSwitches(const core::SessionStore& store,
                    const DesktopSwitchCallback& callback) const override {
    const char* script_path = std::getenv("LOCKING_GLASS_DESKTOP_SCRIPT");
    if (script_path == nullptr || script_path[0] == '\0') {
      return 1;
    }

    const auto scenarios = LoadDesktopScript(script_path);
    if (scenarios.empty()) {
      return 1;
    }

    for (const auto& scenario : scenarios) {
      if (!callback(BuildDesktopSwitchReport(store, scenario))) {
        break;
      }
    }

    return 0;
  }
};

}  // namespace

std::string FormatDesktopSwitchReport(const DesktopSwitchReport& report) {
  std::ostringstream builder;
  builder << core::FormatMonitorLockingPlan(report.plan);

  builder << "Move results:\n";
  if (report.move_results.empty()) {
    builder << "  - none\n";
  } else {
    for (const auto& result : report.move_results) {
      builder << "  - " << DescribeWindow(result.window) << " ["
              << DescribeMonitor(result.window) << "] "
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
      builder << "  - " << DescribeWindow(window) << " ["
              << DescribeMonitor(window) << "] on " << window.desktop_id
              << '\n';
    }
  }

  return builder.str();
}

std::unique_ptr<VirtualDesktopController> CreateVirtualDesktopController() {
  return std::make_unique<VirtualDesktopControllerImpl>();
}

}  // namespace locking_glass::integration
