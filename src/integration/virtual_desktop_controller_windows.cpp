#include "virtual_desktop_controller_internal.h"

#if defined(_WIN32)

#include "windows_live_desktop_watch.h"
#include "windows_virtual_desktop_capture.h"
#include "windows_virtual_desktop_helper.h"
#include "windows_virtual_desktop_surface.h"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace locking_glass::integration::internal {

namespace {

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

std::wstring ResolveCommandProcessorPath() {
  wchar_t system_directory[MAX_PATH];
  const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return (std::filesystem::path(system_directory) / "cmd.exe").wstring();
  }

  return L"C:\\Windows\\System32\\cmd.exe";
}

struct HiddenWatchProcess {
  HANDLE output_read = nullptr;
  HANDLE process = nullptr;
  HANDLE thread = nullptr;
};

void CloseHandleIfPresent(HANDLE* handle) {
  if (handle != nullptr && *handle != nullptr &&
      *handle != INVALID_HANDLE_VALUE) {
    CloseHandle(*handle);
    *handle = nullptr;
  }
}

bool StartHiddenWatchProcess(const std::filesystem::path& command_script_path,
                             HiddenWatchProcess* watch_process) {
  if (watch_process == nullptr) {
    return false;
  }

  SECURITY_ATTRIBUTES security_attributes{};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;

  HANDLE output_read = nullptr;
  HANDLE output_write = nullptr;
  if (!CreatePipe(&output_read, &output_write, &security_attributes, 0)) {
    return false;
  }
  SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup_info.wShowWindow = SW_HIDE;
  startup_info.hStdOutput = output_write;
  startup_info.hStdError = output_write;
  startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process_info{};
  const std::wstring command_processor = ResolveCommandProcessorPath();
  std::wstring command_line = L"\"" + command_processor + L"\" /d /s /c \"\"" +
                              command_script_path.wstring() + L"\"\"";
  const BOOL started = CreateProcessW(
      nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
      nullptr, nullptr, &startup_info, &process_info);

  CloseHandleIfPresent(&output_write);
  if (!started) {
    CloseHandleIfPresent(&output_read);
    return false;
  }

  watch_process->output_read = output_read;
  watch_process->process = process_info.hProcess;
  watch_process->thread = process_info.hThread;
  return true;
}

bool ReadNextProcessLine(HANDLE output_read, std::string* pending,
                         std::string* line) {
  if (output_read == nullptr || pending == nullptr || line == nullptr) {
    return false;
  }

  while (true) {
    const std::size_t newline = pending->find('\n');
    if (newline != std::string::npos) {
      *line = pending->substr(0, newline + 1U);
      pending->erase(0, newline + 1U);
      return true;
    }

    char buffer[4096];
    DWORD bytes_read = 0;
    if (!ReadFile(output_read, buffer, sizeof(buffer), &bytes_read, nullptr) ||
        bytes_read == 0) {
      if (pending->empty()) {
        return false;
      }
      *line = *pending;
      pending->clear();
      return true;
    }
    pending->append(buffer, buffer + bytes_read);
  }
}

int FinishHiddenWatchProcess(HiddenWatchProcess* watch_process) {
  if (watch_process == nullptr) {
    return 1;
  }

  DWORD exit_code = 1;
  if (watch_process->process != nullptr) {
    WaitForSingleObject(watch_process->process, INFINITE);
    GetExitCodeProcess(watch_process->process, &exit_code);
  }

  CloseHandleIfPresent(&watch_process->output_read);
  CloseHandleIfPresent(&watch_process->thread);
  CloseHandleIfPresent(&watch_process->process);
  return static_cast<int>(exit_code);
}

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

bool CleanupWindowsStagingDesktop() {
  const auto asset_root = FindLiveWatchAssetRoot();
  std::string helper_library_detail;
  auto helper = WindowsVirtualDesktopHelper::Load(asset_root,
                                                  &helper_library_detail);
  if (helper == nullptr) {
    return false;
  }

  std::string cleanup_detail;
  return helper->RemoveEmptyStagingDesktopByName(&cleanup_detail);
}

int WatchWindowsLiveSwitches(const core::SessionStore& store,
                             const DesktopSwitchCallback& callback,
                             const DesktopWatchOptions& options) {
  const auto asset_root = FindLiveWatchAssetRoot();
  if (asset_root.empty()) {
    std::cerr
        << "Locking Glass could not locate bundled live desktop watch assets "
           "beside the executable or in the current repository checkout, "
           "so the live Windows desktop watch path cannot start.\n";
    return 1;
  }

  const auto log_path = BuildLiveWatchLogPath();
  const auto command_script_path =
      BuildLiveWatchCommandScript(asset_root, log_path, options);
  if (command_script_path.empty()) {
    std::cerr
        << "Locking Glass could not prepare the live Windows desktop watch "
           "helper command because the pinned helper DLL or probe script was "
           "unavailable.\n";
    return 1;
  }

  HiddenWatchProcess watch_process;
  if (!StartHiddenWatchProcess(command_script_path, &watch_process)) {
    std::cerr
        << "Locking Glass could not launch the live Windows desktop watch helper.\n";
    return 1;
  }

  auto monitor_gateway = platform::CreateMonitorGateway();
  std::unique_ptr<WindowsVirtualDesktopHelper> helper;
  std::string helper_library_detail;
  std::vector<std::string> helper_lines;
  std::size_t observed_events = 0;
  std::string pending_output;
  std::string line;

  while (ReadNextProcessLine(watch_process.output_read, &pending_output,
                             &line)) {
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

  const int exit_code = FinishHiddenWatchProcess(&watch_process);
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
  report = BuildUnlockReturnReport(
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
  for (const auto& move_result : report.move_results) {
    if (move_result.from_desktop.name == kStagingDesktopName) {
      // Unlock return is the point where the holding desktop should become
      // empty. Cleanup uses the exact desktop identity observed on a returned
      // window, never a name-only desktop lookup.
      helper->RemoveKnownStagingDesktopIfUnused(move_result.from_desktop,
                                                nullptr);
      break;
    }
  }
  for (const auto& tracked_window : request.tracked_windows) {
    if (tracked_window.staging_desktop.has_value() &&
        tracked_window.staging_desktop->name == kStagingDesktopName) {
      helper->RemoveKnownStagingDesktopIfUnused(
          *tracked_window.staging_desktop, nullptr);
      break;
    }
  }
  return report;
}

}  // namespace locking_glass::integration::internal

#endif
