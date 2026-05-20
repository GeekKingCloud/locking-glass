#include "virtual_desktop_controller_internal.h"

#if defined(_WIN32)

#include "windows_virtual_desktop_capture.h"
#include "windows_virtual_desktop_helper.h"
#include "windows_virtual_desktop_surface.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace locking_glass::integration::internal {

namespace {

std::filesystem::path BuildLiveWatchLogPath() {
  const DWORD process_id = GetCurrentProcessId();
  return std::filesystem::temp_directory_path() /
         ("locking-glass-live-desktop-watch-" + std::to_string(process_id) +
          ".log");
}

bool HasHelperWatchAssets(const std::filesystem::path& root) {
  return (std::filesystem::exists(root / "scripts" /
                                  "run-live-desktop-probe.ps1") &&
          std::filesystem::exists(root / "tools" / "windows_live_desktop_probe" /
                                  "LockingGlass.WindowsLiveDesktopProbe.csproj")) ||
         (std::filesystem::exists(root / "run-live-desktop-probe.ps1") &&
          (std::filesystem::exists(
               root / "LockingGlass.WindowsLiveDesktopProbe.exe") ||
           std::filesystem::exists(
               root / "LockingGlass.WindowsLiveDesktopProbe.dll")));
}

std::filesystem::path ResolveLiveWatchScriptPath(
    const std::filesystem::path& root) {
  const auto bundled_script = root / "run-live-desktop-probe.ps1";
  if (std::filesystem::exists(bundled_script)) {
    return bundled_script;
  }

  const auto repository_script =
      root / "scripts" / "run-live-desktop-probe.ps1";
  if (std::filesystem::exists(repository_script)) {
    return repository_script;
  }

  return {};
}

std::filesystem::path FindLiveWatchAssetRoot(
    const std::filesystem::path& start) {
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

std::filesystem::path FindLiveWatchAssetRoot() {
  wchar_t module_path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }

  return FindLiveWatchAssetRoot(std::filesystem::path(module_path).parent_path());
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

  return std::filesystem::path(
      "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
}

std::filesystem::path BuildLiveWatchCommandScript(
    const std::filesystem::path& asset_root,
    const std::filesystem::path& log_path,
    const DesktopWatchOptions& options) {
  const auto script_path = ResolveLiveWatchScriptPath(asset_root);
  const auto powershell_path = ResolveWindowsPowerShellPath();
  const auto helper_dll_path = ResolvePreferredHelperDllPath(asset_root);
  const auto command_script_path =
      std::filesystem::temp_directory_path() /
      ("locking-glass-live-desktop-watch-" +
       std::to_string(GetCurrentProcessId()) + ".cmd");

  std::ofstream output(command_script_path, std::ios::trunc);
  output << "@echo off\r\n";
  output << QuoteCommandArgument(powershell_path.string())
         << " -NoProfile -ExecutionPolicy Bypass -File "
         << QuoteCommandArgument(script_path.string()) << " -WatchStream"
         << " -HelperDllPath "
         << QuoteCommandArgument(helper_dll_path.string())
         << " -LogPath " << QuoteCommandArgument(log_path.string())
         << " -RequiredEvents " << options.required_events
         << " -TimeoutSeconds " << options.timeout_seconds
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

std::string FormatDesktopContext(int desktop_number,
                                 const std::string& desktop_guid,
                                 const std::string& desktop_name) {
  return BuildDesktopDisplayId(desktop_number, desktop_guid, desktop_name);
}

struct WindowDesktopContextFormatter {
  LiveDesktopSwitchEvent event;

  DesktopIdentity Resolve(int desktop_number) const {
    if (desktop_number == event.source_desktop_number) {
      return MakeDesktopIdentity(event.source_desktop_number,
                                 event.source_desktop_guid,
                                 event.source_desktop_name);
    }
    if (desktop_number == event.target_desktop_number) {
      return MakeDesktopIdentity(event.target_desktop_number,
                                 event.target_desktop_guid,
                                 event.target_desktop_name);
    }
    return MakeDesktopIdentity(desktop_number, "", "");
  }
};

bool HasLockedPresentMonitor(const core::SessionRefreshResult& session) {
  for (const auto& monitor : session.snapshot.monitors) {
    if (monitor.is_present && monitor.locked) {
      return true;
    }
  }
  return false;
}

struct WindowDesktopVerificationResult {
  int desktop_number = -1;
  int polls = 0;
};

constexpr int kLiveMoveVerificationMaxPolls = 20;
constexpr DWORD kLiveMoveVerificationSleepMilliseconds = 25;

WindowDesktopVerificationResult WaitForWindowDesktopNumber(
    const WindowsVirtualDesktopHelper& helper, HWND window,
    int expected_desktop_number) {
  WindowDesktopVerificationResult result;
  for (int poll = 1; poll <= kLiveMoveVerificationMaxPolls; ++poll) {
    result.desktop_number = helper.GetWindowDesktopNumber(window);
    result.polls = poll;
    if (result.desktop_number == expected_desktop_number) {
      return result;
    }

    if (poll < kLiveMoveVerificationMaxPolls) {
      ::Sleep(kLiveMoveVerificationSleepMilliseconds);
    }
  }

  return result;
}

DesktopSwitchReport BuildBlockedDesktopSwitchReport(
    const core::SessionStore& store,
    const LiveDesktopSwitchEvent& event,
    const std::vector<platform::MonitorDescriptor>& monitors,
    const std::vector<CapturedWindow>& captured_windows,
    const std::string& block_detail) {
  core::DesktopSwitchScenario scenario{
      .trigger = "windows-live-post-message-hook",
      .source_desktop_id = FormatDesktopContext(event.source_desktop_number,
                                                event.source_desktop_guid,
                                                event.source_desktop_name),
      .target_desktop_id = FormatDesktopContext(event.target_desktop_number,
                                                event.target_desktop_guid,
                                                event.target_desktop_name),
      .staging_desktop_id = kStagingDesktopName,
      .monitors = monitors,
      .windows = {},
  };
  scenario.windows.reserve(captured_windows.size());
  std::map<std::string, DesktopIdentity> window_desktops;
  for (const auto& captured : captured_windows) {
    scenario.windows.push_back(captured.window);
    window_desktops.emplace(captured.window.window_id, captured.desktop);
  }

  DesktopSwitchReport report{
      .plan = core::BuildMonitorLockingPlan(store, scenario),
      .move_results = {},
      .resulting_windows = scenario.windows,
  };
  for (const auto& move : report.plan.moves) {
    const auto from_desktop_it = window_desktops.find(move.window.window_id);
    report.move_results.push_back(WindowMoveResult{
        .window = move.window,
        .from_desktop =
            from_desktop_it != window_desktops.end()
                ? from_desktop_it->second
                : MakeDisplayOnlyDesktopIdentity(move.from_desktop_id),
        .to_desktop = MakeDisplayOnlyDesktopIdentity(move.to_desktop_id),
        .from_desktop_id = move.from_desktop_id,
        .to_desktop_id = move.to_desktop_id,
        .success = false,
        .detail = block_detail,
    });
  }
  return report;
}

void OverrideSkippedWindowReason(
    DesktopSwitchReport* report,
    const std::map<std::string, std::string>& skip_reason_overrides) {
  for (auto& skipped : report->plan.skipped_windows) {
    const auto it = skip_reason_overrides.find(skipped.window.window_id);
    if (it != skip_reason_overrides.end()) {
      skipped.reason = it->second;
    }
  }
}

DesktopSwitchReport BuildWindowsLiveDesktopSwitchReport(
    const core::SessionStore& store,
    const LiveDesktopSwitchEvent& event,
    const std::vector<platform::MonitorDescriptor>& monitors,
    WindowsVirtualDesktopHelper& helper) {
  const core::SessionRefreshResult session = store.Restore(monitors);
  const WindowDesktopContextFormatter desktop_context{event};
  const auto captured_windows =
      CaptureLiveWindows(helper, monitors, session,
                         [&desktop_context](const int desktop_number) {
                           return desktop_context.Resolve(desktop_number);
                         });

  std::optional<DesktopIdentity> staging_desktop;
  if (HasLockedPresentMonitor(session)) {
    std::string staging_detail;
    staging_desktop = helper.EnsureStagingDesktop(&staging_detail);
    if (!staging_desktop.has_value()) {
      // Without a verified staging desktop, moving target occupants would push
      // them into some other user workspace. Block the switch plan instead.
      return BuildBlockedDesktopSwitchReport(
          store, event, monitors, captured_windows,
          "staging desktop unavailable: " + staging_detail);
    }
  }

  core::DesktopSwitchScenario scenario{
      .trigger = "windows-live-post-message-hook",
      .source_desktop_id = FormatDesktopContext(event.source_desktop_number,
                                                event.source_desktop_guid,
                                                event.source_desktop_name),
      .target_desktop_id = FormatDesktopContext(event.target_desktop_number,
                                                event.target_desktop_guid,
                                                event.target_desktop_name),
      .staging_desktop_id = staging_desktop.has_value()
                                ? FormatDesktopIdentity(*staging_desktop)
                                : std::string{},
      .monitors = monitors,
      .windows = {},
  };

  scenario.windows.reserve(captured_windows.size());
  std::map<std::string, HWND> window_handles;
  std::map<std::string, DesktopIdentity> window_desktops;
  std::map<std::string, std::string> skip_reason_overrides;
  std::vector<core::MonitorLockingSkip> extra_skips;
  for (const auto& captured : captured_windows) {
    scenario.windows.push_back(captured.window);
    window_handles.emplace(captured.window.window_id, captured.handle);
    window_desktops.emplace(captured.window.window_id, captured.desktop);
    if (!captured.override_skip_reason.empty()) {
      skip_reason_overrides.emplace(captured.window.window_id,
                                    captured.override_skip_reason);
    }
    if (captured.extra_skip.has_value()) {
      extra_skips.push_back(*captured.extra_skip);
    }
  }

  DesktopSwitchReport report{
      .plan = core::BuildMonitorLockingPlan(store, scenario),
      .move_results = {},
      .resulting_windows = scenario.windows,
  };
  OverrideSkippedWindowReason(&report, skip_reason_overrides);
  report.plan.skipped_windows.insert(report.plan.skipped_windows.end(),
                                     extra_skips.begin(), extra_skips.end());

  for (const auto& move : report.plan.moves) {
    auto* result_window = FindResultWindow(&report.resulting_windows, move);
    const auto handle_it = window_handles.find(move.window.window_id);
    const auto from_desktop_it = window_desktops.find(move.window.window_id);
    const DesktopIdentity from_desktop =
        from_desktop_it != window_desktops.end()
            ? from_desktop_it->second
            : MakeDisplayOnlyDesktopIdentity(move.from_desktop_id);
    if (result_window == nullptr || handle_it == window_handles.end()) {
      report.move_results.push_back(WindowMoveResult{
          .window = move.window,
          .from_desktop = from_desktop,
          .to_desktop = MakeDisplayOnlyDesktopIdentity(move.to_desktop_id),
          .from_desktop_id = move.from_desktop_id,
          .to_desktop_id = move.to_desktop_id,
          .success = false,
          .detail = "window handle was missing from the live snapshot",
      });
      continue;
    }

    const HWND window = handle_it->second;
    int destination_desktop_number = event.source_desktop_number;
    DesktopIdentity to_desktop = desktop_context.Resolve(destination_desktop_number);
    if (move.to_desktop_id == scenario.target_desktop_id) {
      destination_desktop_number = event.target_desktop_number;
      to_desktop = desktop_context.Resolve(destination_desktop_number);
    } else if (staging_desktop.has_value() &&
               move.to_desktop_id == FormatDesktopIdentity(*staging_desktop)) {
      // Switch events name only source and target desktops; staging has to be
      // resolved explicitly from the helper-owned desktop identity.
      destination_desktop_number = staging_desktop->number;
      to_desktop = *staging_desktop;
    }
    if (destination_desktop_number < 0) {
      report.move_results.push_back(WindowMoveResult{
          .window = move.window,
          .from_desktop = from_desktop,
          .to_desktop = to_desktop,
          .from_desktop_id = move.from_desktop_id,
          .to_desktop_id = move.to_desktop_id,
          .success = false,
          .detail = "destination desktop number was not available",
      });
      continue;
    }

    const int move_result =
        helper.MoveWindowToDesktopNumber(window, destination_desktop_number);
    if (move_result < 0) {
      report.move_results.push_back(WindowMoveResult{
          .window = move.window,
          .from_desktop = from_desktop,
          .to_desktop = to_desktop,
          .from_desktop_id = move.from_desktop_id,
          .to_desktop_id = move.to_desktop_id,
          .success = false,
          .detail = "MoveWindowToDesktopNumber returned a failure status",
      });
      continue;
    }

    const WindowDesktopVerificationResult verification =
        WaitForWindowDesktopNumber(helper, window, destination_desktop_number);
    const int actual_desktop_number = verification.desktop_number;
    DesktopIdentity actual_desktop =
        actual_desktop_number >= 0
            ? desktop_context.Resolve(actual_desktop_number)
            : MakeDisplayOnlyDesktopIdentity("<unknown-desktop>");
    if (staging_desktop.has_value() &&
        actual_desktop_number == staging_desktop->number) {
      actual_desktop = *staging_desktop;
    }
    const std::string actual_desktop_id = FormatDesktopIdentity(actual_desktop);
    if (actual_desktop_id != move.to_desktop_id) {
      report.move_results.push_back(WindowMoveResult{
          .window = move.window,
          .from_desktop = from_desktop,
          .to_desktop = to_desktop,
          .from_desktop_id = move.from_desktop_id,
          .to_desktop_id = move.to_desktop_id,
          .success = false,
          .detail = "post-move desktop verification returned " +
                    actual_desktop_id + " after " +
                    std::to_string(verification.polls) + " poll(s)",
      });
      continue;
    }

    result_window->desktop_id = move.to_desktop_id;
    report.move_results.push_back(WindowMoveResult{
        .window = move.window,
        .from_desktop = from_desktop,
        .to_desktop = to_desktop,
        .from_desktop_id = move.from_desktop_id,
        .to_desktop_id = move.to_desktop_id,
        .success = true,
        .detail = "live helper move verified against window desktop state " +
                  std::string(verification.polls > 1 ? "after settling"
                                                     : "immediately"),
    });
  }

  return report;
}

}  // namespace

CapabilityReport ProbeWindowsController() {
  const auto probe = ProbeWindowsVirtualDesktopSurface();
  if (probe.com_ready && probe.desktop_manager_ready &&
      probe.helper_watch_ready && probe.helper_move_ready &&
      probe.helper_lifecycle_ready) {
    return CapabilityReport{
        .component = "desktop-locking",
        .status = CapabilityStatus::kReady,
        .detail =
            "Live desktop locking is available through the VirtualDesktopAccessor post-message hook, move exports, and Locking Glass staging desktop lifecycle exports; replay through LOCKING_GLASS_DESKTOP_SCRIPT stays test-only and is not completion evidence for the core feature.",
    };
  }

  return CapabilityReport{
      .component = "desktop-locking",
      .status = CapabilityStatus::kUnavailable,
      .detail =
          "Desktop locking fails closed until both IVirtualDesktopManager and VirtualDesktopAccessor.dll (RegisterPostMessageHook, UnregisterPostMessageHook, GetCurrentDesktopNumber, GoToDesktopNumber, GetDesktopCount, GetDesktopName, GetDesktopIdByNumber, MoveWindowToDesktopNumber, GetWindowDesktopNumber, CreateDesktop, SetDesktopName, RemoveDesktop) are available on the live Windows runtime.",
  };
}

int WatchWindowsLiveSwitches(const core::SessionStore& store,
                             const DesktopSwitchCallback& callback,
                             const DesktopWatchOptions& options) {
  const auto asset_root = FindLiveWatchAssetRoot();
  if (asset_root.empty()) {
    std::cerr
        << "Locking Glass could not locate bundled live desktop watch assets "
           "beside the executable or in the executable's repository ancestors, "
           "so the live Windows desktop watch path cannot start.\n";
    return 1;
  }

  const auto log_path = BuildLiveWatchLogPath();
  const auto command_script_path =
      BuildLiveWatchCommandScript(asset_root, log_path, options);
  // _popen only gives a narrow command-string surface, so the generated .cmd
  // file keeps quoting centralized and lets the C++ side stream the helper log
  // without duplicating the PowerShell watch implementation.
  const std::string command =
      QuoteCommandArgument(command_script_path.string()) + " 2>&1";
  FILE* pipe = _popen(command.c_str(), "r");
  if (pipe == nullptr) {
    std::cerr
        << "Locking Glass could not launch the live Windows desktop watch helper.\n";
    return 1;
  }

  auto monitor_gateway = platform::CreateMonitorGateway();
  std::unique_ptr<WindowsVirtualDesktopHelper> helper;
  std::string helper_library_detail;
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
      if (helper == nullptr) {
        helper = WindowsVirtualDesktopHelper::Load(asset_root,
                                                   &helper_library_detail);
        if (helper == nullptr) {
          std::cerr << "Locking Glass could not load VirtualDesktopAccessor.dll "
                       "for live window moves: "
                    << helper_library_detail << '\n';
          break;
        }
      }

      const auto monitors = monitor_gateway->Enumerate();
      ++observed_events;
      if (!callback(BuildWindowsLiveDesktopSwitchReport(store, event, monitors,
                                                        *helper))) {
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

  std::cerr << "Locking Glass live Windows desktop watch failed";
  if (!log_path.empty()) {
    std::cerr << "; helper log: " << log_path.string();
  }
  std::cerr << '\n';
  for (const auto& helper_line : helper_lines) {
    std::cerr << helper_line << '\n';
  }
  return 1;
}

UnlockReturnReport BuildWindowsUnlockReturnReport(
    const UnlockReturnRequest& request) {
  UnlockReturnReport report{
      .monitor = request.monitor,
      .move_results = {},
      .skipped_windows = {},
      .resulting_windows = {},
  };
  if (request.tracked_windows.empty()) {
    return report;
  }

  const auto asset_root = FindLiveWatchAssetRoot();
  std::string helper_library_detail;
  auto helper = WindowsVirtualDesktopHelper::Load(asset_root,
                                                  &helper_library_detail);
  if (helper == nullptr) {
    for (const auto& tracked_window : request.tracked_windows) {
      report.move_results.push_back(WindowMoveResult{
          .window = tracked_window.window,
          .from_desktop = {},
          .to_desktop = tracked_window.home_desktop,
          .from_desktop_id = tracked_window.window.desktop_id,
          .to_desktop_id = FormatDesktopIdentity(tracked_window.home_desktop),
          .success = false,
          .detail = helper_library_detail,
      });
    }
    return report;
  }

  auto monitor_gateway = platform::CreateMonitorGateway();
  const auto monitors = monitor_gateway->Enumerate();
  const auto available_desktops = helper->ListDesktops();
  const auto current_windows = CaptureLiveWindowsForReturn(*helper, monitors);
  return BuildUnlockReturnReport(
      request, available_desktops, current_windows,
      [&helper](const CapturedWindow& current_window,
                const DesktopIdentity& remembered_desktop) {
        if (remembered_desktop.number < 0 || current_window.handle == nullptr) {
          return WindowMoveResult{
              .window = current_window.window,
              .from_desktop = current_window.desktop,
              .to_desktop = remembered_desktop,
              .from_desktop_id = current_window.window.desktop_id,
              .to_desktop_id = FormatDesktopIdentity(remembered_desktop),
              .success = false,
              .detail = "destination desktop number was not available",
          };
        }

        const int move_result = helper->MoveWindowToDesktopNumber(
            current_window.handle, remembered_desktop.number);
        if (move_result < 0) {
          return WindowMoveResult{
              .window = current_window.window,
              .from_desktop = current_window.desktop,
              .to_desktop = remembered_desktop,
              .from_desktop_id = current_window.window.desktop_id,
              .to_desktop_id = FormatDesktopIdentity(remembered_desktop),
              .success = false,
              .detail = "MoveWindowToDesktopNumber returned a failure status",
          };
        }

        const WindowDesktopVerificationResult verification =
            WaitForWindowDesktopNumber(*helper, current_window.handle,
                                       remembered_desktop.number);
        const DesktopIdentity actual_desktop =
            verification.desktop_number >= 0
                ? helper->GetDesktopIdentity(verification.desktop_number)
                : MakeDisplayOnlyDesktopIdentity("<unknown-desktop>");
        if (!DesktopIdentityEquals(actual_desktop, remembered_desktop)) {
          return WindowMoveResult{
              .window = current_window.window,
              .from_desktop = current_window.desktop,
              .to_desktop = remembered_desktop,
              .from_desktop_id = current_window.window.desktop_id,
              .to_desktop_id = FormatDesktopIdentity(remembered_desktop),
              .success = false,
              .detail = "post-return desktop verification returned " +
                        FormatDesktopIdentity(actual_desktop) + " after " +
                        std::to_string(verification.polls) + " poll(s)",
          };
        }

        return WindowMoveResult{
            .window = current_window.window,
            .from_desktop = current_window.desktop,
            .to_desktop = remembered_desktop,
            .from_desktop_id = current_window.window.desktop_id,
            .to_desktop_id = FormatDesktopIdentity(remembered_desktop),
            .success = true,
            .detail = "live helper return move verified against window desktop "
                      "state " +
                      std::string(verification.polls > 1 ? "after settling"
                                                         : "immediately"),
        };
      });
}

}  // namespace locking_glass::integration::internal

#endif
