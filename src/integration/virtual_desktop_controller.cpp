#include "locking_glass/integration/virtual_desktop_controller.h"

#include "locking_glass/platform/monitor_gateway.h"
#include "windows_virtual_desktop_surface.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

DesktopSwitchReport BuildDesktopSwitchReport(
    const core::SessionStore& store,
    const core::DesktopSwitchScenario& scenario);

#if defined(_WIN32)
CapabilityReport ProbeWindowsController() {
  const auto probe = internal::ProbeWindowsVirtualDesktopSurface();
  if (probe.com_ready && probe.desktop_manager_ready && probe.helper_watch_ready &&
      probe.helper_move_ready) {
    return CapabilityReport{
        .component = "desktop-locking",
        .status = CapabilityStatus::kReady,
        .detail =
            "Live desktop locking is available through the VirtualDesktopAccessor post-message hook and move exports; replay through LOCKING_GLASS_DESKTOP_SCRIPT stays test-only and is not completion evidence for the core feature.",
    };
  }

  return CapabilityReport{
      .component = "desktop-locking",
      .status = CapabilityStatus::kUnavailable,
      .detail =
          "Desktop locking fails closed until both IVirtualDesktopManager and VirtualDesktopAccessor.dll (RegisterPostMessageHook, UnregisterPostMessageHook, GetCurrentDesktopNumber, GoToDesktopNumber, MoveWindowToDesktopNumber, GetWindowDesktopNumber) are available on the live Windows runtime.",
  };
}

std::filesystem::path BuildLiveWatchLogPath() {
  const DWORD process_id = GetCurrentProcessId();
  return std::filesystem::temp_directory_path() /
         ("locking-glass-live-desktop-watch-" + std::to_string(process_id) +
          ".log");
}

bool HasHelperWatchAssets(const std::filesystem::path& root) {
  return std::filesystem::exists(root / "scripts" / "run-live-desktop-probe.ps1") &&
         std::filesystem::exists(root / "tools" / "windows_live_desktop_probe" /
                                 "LockingGlass.WindowsLiveDesktopProbe.csproj");
}

std::filesystem::path FindRepositoryRoot(const std::filesystem::path& start) {
  if (start.empty()) {
    return {};
  }

  std::filesystem::path current = std::filesystem::absolute(start);
  while (!current.empty()) {
    if (HasHelperWatchAssets(current)) {
      return current;
    }

    const auto parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }

  return {};
}

std::filesystem::path FindRepositoryRoot() {
  if (const auto from_cwd = FindRepositoryRoot(std::filesystem::current_path());
      !from_cwd.empty()) {
    return from_cwd;
  }

  wchar_t module_path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }

  return FindRepositoryRoot(std::filesystem::path(module_path).parent_path());
}

std::string QuoteCommandArgument(const std::string& value) {
  return "\"" + value + "\"";
}

std::filesystem::path ResolveWindowsPowerShellPath() {
  wchar_t windir[MAX_PATH];
  const DWORD length =
      GetEnvironmentVariableW(L"WINDIR", windir, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return std::filesystem::path(windir) / "System32" / "WindowsPowerShell" /
           "v1.0" / "powershell.exe";
  }

  return std::filesystem::path("C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
}

std::filesystem::path BuildLiveWatchCommandScript(
    const std::filesystem::path& repository_root,
    const std::filesystem::path& log_path) {
  const auto script_path = repository_root / "scripts" / "run-live-desktop-probe.ps1";
  const auto powershell_path = ResolveWindowsPowerShellPath();
  const auto command_script_path =
      std::filesystem::temp_directory_path() /
      ("locking-glass-live-desktop-watch-" +
       std::to_string(GetCurrentProcessId()) + ".cmd");

  std::ofstream output(command_script_path, std::ios::trunc);
  output << "@echo off\r\n";
  output << QuoteCommandArgument(powershell_path.string())
         << " -NoProfile -ExecutionPolicy Bypass -File "
         << QuoteCommandArgument(script_path.string()) << " -WatchStream"
         << " -LogPath " << QuoteCommandArgument(log_path.string())
         << " -RequiredEvents 2"
         << " -TimeoutSeconds 0"
         << " -NoAutoCycle"
         << " -SkipMoveExercise\r\n";
  return command_script_path;
}

void TrimTrailingLineBreaks(std::string* value) {
  while (!value->empty() &&
         (value->back() == '\n' || value->back() == '\r')) {
    value->pop_back();
  }
}

struct LiveDesktopSwitchEvent {
  int source_desktop_number = -1;
  std::string source_desktop_guid;
  std::string source_desktop_name;
  int target_desktop_number = -1;
  std::string target_desktop_guid;
  std::string target_desktop_name;
};

bool ParseLiveDesktopSwitchEvent(const std::string& line,
                                 LiveDesktopSwitchEvent* event) {
  const auto fields = SplitFields(line);
  if (fields.size() != 7U || fields[0] != "watch-event") {
    return false;
  }

  if (!ParseIntField(fields[1], &event->source_desktop_number) ||
      !ParseIntField(fields[4], &event->target_desktop_number)) {
    return false;
  }

  event->source_desktop_guid = fields[2];
  event->source_desktop_name = fields[3];
  event->target_desktop_guid = fields[5];
  event->target_desktop_name = fields[6];
  return true;
}

std::string FormatDesktopContext(int desktop_number, const std::string& desktop_guid,
                                 const std::string& desktop_name) {
  std::ostringstream builder;
  if (desktop_number >= 0) {
    builder << "Desktop " << (desktop_number + 1) << " [" << desktop_number << "]";
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

int WatchWindowsLiveSwitches(const core::SessionStore& store,
                             const DesktopSwitchCallback& callback) {
  const auto repository_root = FindRepositoryRoot();
  if (repository_root.empty()) {
    std::cerr
        << "LockingGlass could not locate scripts/run-live-desktop-probe.ps1 or "
           "tools/windows_live_desktop_probe from the current checkout, so the "
           "live Windows desktop watch path cannot start.\n";
    return 1;
  }

  const auto log_path = BuildLiveWatchLogPath();
  const auto command_script_path =
      BuildLiveWatchCommandScript(repository_root, log_path);
  const std::string command = command_script_path.string() + " 2>&1";
  FILE* pipe = _popen(command.c_str(), "r");
  if (pipe == nullptr) {
    std::cerr << "LockingGlass could not launch the live Windows desktop watch helper.\n";
    return 1;
  }

  auto monitor_gateway = platform::CreateMonitorGateway();
  std::vector<std::string> helper_lines;
  std::size_t observed_events = 0;
  char buffer[4096];

  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    std::string line(buffer);
    TrimTrailingLineBreaks(&line);
    if (line.empty()) {
      continue;
    }

    LiveDesktopSwitchEvent event;
    if (ParseLiveDesktopSwitchEvent(line, &event)) {
      core::DesktopSwitchScenario scenario{
          .trigger = "windows-live-post-message-hook",
          .source_desktop_id =
              FormatDesktopContext(event.source_desktop_number,
                                   event.source_desktop_guid,
                                   event.source_desktop_name),
          .target_desktop_id =
              FormatDesktopContext(event.target_desktop_number,
                                   event.target_desktop_guid,
                                   event.target_desktop_name),
          .monitors = monitor_gateway->Enumerate(),
          .windows = {},
      };
      ++observed_events;
      if (!callback(BuildDesktopSwitchReport(store, scenario))) {
        break;
      }
      continue;
    }

    if (helper_lines.size() < 8U) {
      helper_lines.push_back(line);
    }
  }

  const int exit_code = _pclose(pipe);
  std::error_code remove_error;
  std::filesystem::remove(command_script_path, remove_error);
  if (exit_code == 0 && observed_events > 0U) {
    return 0;
  }

  std::cerr << "LockingGlass live Windows desktop watch failed";
  if (!log_path.empty()) {
    std::cerr << "; helper log: " << log_path.string();
  }
  std::cerr << '\n';
  for (const auto& helper_line : helper_lines) {
    std::cerr << helper_line << '\n';
  }
  return 1;
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
            "Desktop locking is stubbed on non-Windows hosts; LOCKING_GLASS_DESKTOP_SCRIPT remains a replay seam for policy checks only and does not prove the live Windows hook path.",
    };
#endif
  }

  int WatchSwitches(const core::SessionStore& store,
                    const DesktopSwitchCallback& callback) const override {
    const char* script_path = std::getenv("LOCKING_GLASS_DESKTOP_SCRIPT");
    if (script_path != nullptr && script_path[0] != '\0') {
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

#if defined(_WIN32)
    return WatchWindowsLiveSwitches(store, callback);
#else
    return 1;
#endif
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
