#include "locking_glass/integration/virtual_desktop_controller.h"

#include "locking_glass/platform/monitor_gateway.h"
#include "windows_virtual_desktop_surface.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
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
core::DesktopWindow* FindResultWindow(
    std::vector<core::DesktopWindow>* windows,
    const core::MonitorLockingMove& move);

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

std::filesystem::path ResolvePreferredHelperDllPath(
    const std::filesystem::path& repository_root) {
  wchar_t helper_path[MAX_PATH];
  const DWORD length = GetEnvironmentVariableW(
      L"LOCKING_GLASS_VIRTUAL_DESKTOP_HELPER", helper_path, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return std::filesystem::path(helper_path);
  }

  if (!repository_root.empty()) {
    return repository_root / "build" / "windows-live-desktop-probe" /
           "VirtualDesktopAccessor.dll";
  }

  return std::filesystem::path("VirtualDesktopAccessor.dll");
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
    const std::filesystem::path& log_path,
    const DesktopWatchOptions& options) {
  const auto script_path = repository_root / "scripts" / "run-live-desktop-probe.ps1";
  const auto powershell_path = ResolveWindowsPowerShellPath();
  const auto helper_dll_path = ResolvePreferredHelperDllPath(repository_root);
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

std::string Narrow(const std::wstring& value) {
  if (value.empty()) {
    return "";
  }

  const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr,
                                       0, nullptr, nullptr);
  if (size <= 1) {
    return "";
  }

  std::string buffer(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, buffer.data(), size,
                      nullptr, nullptr);
  buffer.pop_back();
  return buffer;
}

std::string ReadWindowTitle(HWND window) {
  const int length = GetWindowTextLengthW(window);
  if (length <= 0) {
    return "";
  }

  std::wstring title(static_cast<std::size_t>(length) + 1U, L'\0');
  const int written = GetWindowTextW(window, title.data(), length + 1);
  if (written <= 0) {
    return "";
  }

  title.resize(static_cast<std::size_t>(written));
  return Narrow(title);
}

std::string ReadWindowClassName(HWND window) {
  wchar_t class_name[256];
  const int length = GetClassNameW(window, class_name,
                                   static_cast<int>(std::size(class_name)));
  if (length <= 0) {
    return "";
  }
  return Narrow(std::wstring(class_name, class_name + length));
}

std::string FormatWindowId(HWND window) {
  std::ostringstream builder;
  builder << "0x" << std::hex << std::uppercase
          << reinterpret_cast<std::uintptr_t>(window);
  return builder.str();
}

bool MonitorMatchesSessionState(const core::SessionMonitorState& state,
                                const platform::MonitorDescriptor& monitor) {
  return state.monitor.stable_id == monitor.stable_id &&
         state.monitor.device_path == monitor.device_path &&
         state.monitor.bounds.left == monitor.bounds.left &&
         state.monitor.bounds.top == monitor.bounds.top &&
         state.monitor.bounds.right == monitor.bounds.right &&
         state.monitor.bounds.bottom == monitor.bounds.bottom;
}

bool IsLockedPresentMonitor(const core::SessionRefreshResult& session,
                            const platform::MonitorDescriptor& monitor) {
  for (const auto& state : session.snapshot.monitors) {
    if (!state.is_present || !state.locked) {
      continue;
    }

    if (MonitorMatchesSessionState(state, monitor)) {
      return true;
    }
  }
  return false;
}

struct WindowMonitorMatch {
  const platform::MonitorDescriptor* monitor = nullptr;
  bool touches_locked_monitor = false;
  std::string extra_skip_reason;
};

long long IntersectionArea(const RECT& window_rect,
                           const platform::MonitorBounds& bounds) {
  const LONG left =
      std::max(window_rect.left, static_cast<LONG>(bounds.left));
  const LONG top = std::max(window_rect.top, static_cast<LONG>(bounds.top));
  const LONG right =
      std::min(window_rect.right, static_cast<LONG>(bounds.right));
  const LONG bottom =
      std::min(window_rect.bottom, static_cast<LONG>(bounds.bottom));
  if (left >= right || top >= bottom) {
    return 0;
  }

  return static_cast<long long>(right - left) *
         static_cast<long long>(bottom - top);
}

WindowMonitorMatch MatchWindowToMonitor(
    const RECT& window_rect,
    const std::vector<platform::MonitorDescriptor>& monitors,
    const core::SessionRefreshResult& session) {
  WindowMonitorMatch match;
  const platform::MonitorDescriptor* resolved_monitor = nullptr;
  std::size_t intersection_count = 0;

  for (const auto& monitor : monitors) {
    if (IntersectionArea(window_rect, monitor.bounds) == 0) {
      continue;
    }

    ++intersection_count;
    if (resolved_monitor == nullptr) {
      resolved_monitor = &monitor;
    }
    if (IsLockedPresentMonitor(session, monitor)) {
      match.touches_locked_monitor = true;
    }
  }

  if (intersection_count == 1U) {
    match.monitor = resolved_monitor;
    return match;
  }

  if (intersection_count == 0U) {
    match.extra_skip_reason = "window does not intersect any live monitor";
    return match;
  }

  match.extra_skip_reason = "window spans multiple monitors";
  return match;
}

struct WindowDesktopContextFormatter {
  LiveDesktopSwitchEvent event;

  std::string Format(int desktop_number) const {
    if (desktop_number == event.source_desktop_number) {
      return FormatDesktopContext(event.source_desktop_number,
                                  event.source_desktop_guid,
                                  event.source_desktop_name);
    }
    if (desktop_number == event.target_desktop_number) {
      return FormatDesktopContext(event.target_desktop_number,
                                  event.target_desktop_guid,
                                  event.target_desktop_name);
    }
    return FormatDesktopContext(desktop_number, "", "");
  }
};

class WindowsVirtualDesktopHelper {
 public:
  using GetWindowDesktopNumberFn = int(WINAPI*)(HWND);
  using MoveWindowToDesktopNumberFn = int(WINAPI*)(HWND, int);

  WindowsVirtualDesktopHelper(HMODULE library,
                              GetWindowDesktopNumberFn get_window_desktop_number,
                              MoveWindowToDesktopNumberFn move_window_to_desktop_number)
      : library_(library),
        get_window_desktop_number_(get_window_desktop_number),
        move_window_to_desktop_number_(move_window_to_desktop_number) {}

  ~WindowsVirtualDesktopHelper() {
    if (library_ != nullptr) {
      FreeLibrary(library_);
    }
  }

  WindowsVirtualDesktopHelper(const WindowsVirtualDesktopHelper&) = delete;
  WindowsVirtualDesktopHelper& operator=(const WindowsVirtualDesktopHelper&) = delete;

  int GetWindowDesktopNumber(HWND window) const {
    return get_window_desktop_number_(window);
  }

  int MoveWindowToDesktopNumber(HWND window, int desktop_number) const {
    return move_window_to_desktop_number_(window, desktop_number);
  }

  static std::unique_ptr<WindowsVirtualDesktopHelper> Load(
      const std::filesystem::path& repository_root,
      std::string* detail) {
    std::vector<std::filesystem::path> candidates;
    const auto preferred_path = ResolvePreferredHelperDllPath(repository_root);
    if (!preferred_path.empty()) {
      candidates.push_back(preferred_path);
    }

    wchar_t module_path[MAX_PATH];
    const DWORD module_length =
        GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (module_length > 0 && module_length < MAX_PATH) {
      candidates.push_back(
          std::filesystem::path(module_path).parent_path() /
          "VirtualDesktopAccessor.dll");
    }

    candidates.push_back(std::filesystem::current_path() /
                         "VirtualDesktopAccessor.dll");
    candidates.push_back(std::filesystem::path("VirtualDesktopAccessor.dll"));

    std::string last_error =
        "VirtualDesktopAccessor.dll could not be loaded for live window moves.";
    for (const auto& candidate : candidates) {
      if (candidate.has_parent_path()) {
        std::error_code exists_error;
        if (!std::filesystem::exists(candidate, exists_error) || exists_error) {
          continue;
        }
      }

      HMODULE library = LoadLibraryW(candidate.c_str());
      if (library == nullptr) {
        last_error =
            "LoadLibraryW failed for " + candidate.string() + " (Win32 error " +
            std::to_string(GetLastError()) + ").";
        continue;
      }

      const FARPROC get_window_desktop_number_symbol =
          GetProcAddress(library, "GetWindowDesktopNumber");
      const FARPROC move_window_to_desktop_number_symbol =
          GetProcAddress(library, "MoveWindowToDesktopNumber");
      const auto get_window_desktop_number =
          get_window_desktop_number_symbol != nullptr
              ? std::bit_cast<GetWindowDesktopNumberFn>(
                    get_window_desktop_number_symbol)
              : nullptr;
      const auto move_window_to_desktop_number =
          move_window_to_desktop_number_symbol != nullptr
              ? std::bit_cast<MoveWindowToDesktopNumberFn>(
                    move_window_to_desktop_number_symbol)
              : nullptr;
      if (get_window_desktop_number == nullptr ||
          move_window_to_desktop_number == nullptr) {
        last_error =
            "VirtualDesktopAccessor.dll at " + candidate.string() +
            " was missing GetWindowDesktopNumber or MoveWindowToDesktopNumber.";
        FreeLibrary(library);
        continue;
      }

      if (detail != nullptr) {
        *detail = candidate.string();
      }
      return std::make_unique<WindowsVirtualDesktopHelper>(
          library, get_window_desktop_number, move_window_to_desktop_number);
    }

    if (detail != nullptr) {
      *detail = last_error;
    }
    return nullptr;
  }

 private:
  HMODULE library_ = nullptr;
  GetWindowDesktopNumberFn get_window_desktop_number_ = nullptr;
  MoveWindowToDesktopNumberFn move_window_to_desktop_number_ = nullptr;
};

struct WindowDesktopVerificationResult {
  int desktop_number = -1;
  int polls = 0;
};

constexpr int kLiveMoveVerificationMaxPolls = 20;
constexpr DWORD kLiveMoveVerificationSleepMilliseconds = 25;

WindowDesktopVerificationResult WaitForWindowDesktopNumber(
    const WindowsVirtualDesktopHelper& helper,
    HWND window,
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

struct CapturedWindow {
  core::DesktopWindow window;
  HWND handle = nullptr;
  std::string override_skip_reason;
  std::optional<core::MonitorLockingSkip> extra_skip;
};

std::string DescribeUnsafeWindowReason(HWND window) {
  if (window == GetShellWindow()) {
    return "window is the shell host";
  }

  if (GetAncestor(window, GA_ROOT) != window) {
    return "window is not a root top-level window";
  }

  const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
  if ((style & WS_CHILD) != 0) {
    return "window is a child window";
  }

  if (GetWindow(window, GW_OWNER) != nullptr) {
    return "window is owned by another top-level window";
  }

  const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if ((ex_style & WS_EX_TOOLWINDOW) != 0) {
    return "window is a tool window";
  }

  return "";
}

struct CaptureWindowsContext {
  const WindowsVirtualDesktopHelper* helper = nullptr;
  const std::vector<platform::MonitorDescriptor>* monitors = nullptr;
  const core::SessionRefreshResult* session = nullptr;
  const WindowDesktopContextFormatter* desktop_context = nullptr;
  std::vector<CapturedWindow>* captures = nullptr;
};

BOOL CALLBACK CaptureLiveWindow(HWND window, LPARAM raw_context) {
  auto* context = reinterpret_cast<CaptureWindowsContext*>(raw_context);

  core::DesktopWindow captured_window{
      .window_id = FormatWindowId(window),
      .title = ReadWindowTitle(window),
      .monitor_id = "",
      .monitor_label = "",
      .desktop_id = "",
      .is_top_level = true,
      .can_move = true,
  };
  const std::string class_name = ReadWindowClassName(window);
  if (captured_window.title.empty()) {
    captured_window.title = class_name;
  }

  RECT window_rect{};
  if (!GetWindowRect(window, &window_rect)) {
    context->captures->push_back(CapturedWindow{
        .window = captured_window,
        .handle = window,
        .override_skip_reason = "",
        .extra_skip = core::MonitorLockingSkip{
            .window = captured_window,
            .reason = "could not read window bounds",
        },
    });
    return TRUE;
  }

  const auto monitor_match = MatchWindowToMonitor(window_rect, *context->monitors,
                                                  *context->session);
  if (monitor_match.monitor != nullptr) {
    captured_window.monitor_id = monitor_match.monitor->stable_id;
    captured_window.monitor_label = monitor_match.monitor->label;
  }

  const int desktop_number = context->helper->GetWindowDesktopNumber(window);
  if (desktop_number >= 0) {
    captured_window.desktop_id = context->desktop_context->Format(desktop_number);
  }

  const std::string unsafe_reason = DescribeUnsafeWindowReason(window);
  std::optional<core::MonitorLockingSkip> extra_skip;
  std::string override_skip_reason;

  if (!monitor_match.extra_skip_reason.empty() &&
      monitor_match.touches_locked_monitor) {
    extra_skip = core::MonitorLockingSkip{
        .window = captured_window,
        .reason = monitor_match.extra_skip_reason,
    };
  } else if (desktop_number < 0 &&
             monitor_match.monitor != nullptr &&
             IsLockedPresentMonitor(*context->session, *monitor_match.monitor)) {
    extra_skip = core::MonitorLockingSkip{
        .window = captured_window,
        .reason = "window desktop could not be resolved safely",
    };
  } else if (!unsafe_reason.empty() &&
             monitor_match.monitor != nullptr &&
             IsLockedPresentMonitor(*context->session, *monitor_match.monitor) &&
             !captured_window.desktop_id.empty()) {
    captured_window.can_move = false;
    override_skip_reason = unsafe_reason;
  }

  context->captures->push_back(CapturedWindow{
      .window = std::move(captured_window),
      .handle = window,
      .override_skip_reason = std::move(override_skip_reason),
      .extra_skip = std::move(extra_skip),
  });
  return TRUE;
}

std::vector<CapturedWindow> CaptureLiveWindows(
    const WindowsVirtualDesktopHelper& helper,
    const std::vector<platform::MonitorDescriptor>& monitors,
    const core::SessionRefreshResult& session,
    const LiveDesktopSwitchEvent& event) {
  std::vector<CapturedWindow> captures;
  const WindowDesktopContextFormatter desktop_context{event};
  CaptureWindowsContext context{
      .helper = &helper,
      .monitors = &monitors,
      .session = &session,
      .desktop_context = &desktop_context,
      .captures = &captures,
  };

  EnumWindows(CaptureLiveWindow, reinterpret_cast<LPARAM>(&context));
  return captures;
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
    const WindowsVirtualDesktopHelper& helper) {
  const core::SessionRefreshResult session = store.Restore(monitors);
  const auto captured_windows =
      CaptureLiveWindows(helper, monitors, session, event);

  core::DesktopSwitchScenario scenario{
      .trigger = "windows-live-post-message-hook",
      .source_desktop_id = FormatDesktopContext(event.source_desktop_number,
                                                event.source_desktop_guid,
                                                event.source_desktop_name),
      .target_desktop_id = FormatDesktopContext(event.target_desktop_number,
                                                event.target_desktop_guid,
                                                event.target_desktop_name),
      .monitors = monitors,
      .windows = {},
  };

  scenario.windows.reserve(captured_windows.size());
  std::map<std::string, HWND> window_handles;
  std::map<std::string, std::string> skip_reason_overrides;
  std::vector<core::MonitorLockingSkip> extra_skips;
  for (const auto& captured : captured_windows) {
    scenario.windows.push_back(captured.window);
    window_handles.emplace(captured.window.window_id, captured.handle);
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

  const WindowDesktopContextFormatter desktop_context{event};
  for (const auto& move : report.plan.moves) {
    auto* result_window = FindResultWindow(&report.resulting_windows, move);
    const auto handle_it = window_handles.find(move.window.window_id);
    if (result_window == nullptr || handle_it == window_handles.end()) {
      report.move_results.push_back(WindowMoveResult{
          .window = move.window,
          .from_desktop_id = move.from_desktop_id,
          .to_desktop_id = move.to_desktop_id,
          .success = false,
          .detail = "window handle was missing from the live snapshot",
      });
      continue;
    }

    const HWND window = handle_it->second;
    const int destination_desktop_number =
        move.to_desktop_id == scenario.target_desktop_id
            ? event.target_desktop_number
            : event.source_desktop_number;
    if (destination_desktop_number < 0) {
      report.move_results.push_back(WindowMoveResult{
          .window = move.window,
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
    const std::string actual_desktop_id =
        actual_desktop_number >= 0 ? desktop_context.Format(actual_desktop_number)
                                   : "<unknown-desktop>";
    if (actual_desktop_id != move.to_desktop_id) {
      report.move_results.push_back(WindowMoveResult{
          .window = move.window,
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

int WatchWindowsLiveSwitches(const core::SessionStore& store,
                             const DesktopSwitchCallback& callback,
                             const DesktopWatchOptions& options) {
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
      BuildLiveWatchCommandScript(repository_root, log_path, options);
  const std::string command = command_script_path.string() + " 2>&1";
  FILE* pipe = _popen(command.c_str(), "r");
  if (pipe == nullptr) {
    std::cerr << "LockingGlass could not launch the live Windows desktop watch helper.\n";
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
        helper = WindowsVirtualDesktopHelper::Load(repository_root,
                                                   &helper_library_detail);
        if (helper == nullptr) {
          std::cerr << "LockingGlass could not load VirtualDesktopAccessor.dll "
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
                    const DesktopSwitchCallback& callback,
                    const DesktopWatchOptions options) const override {
    const char* script_path = std::getenv("LOCKING_GLASS_DESKTOP_SCRIPT");
    if (options.allow_script_replay && script_path != nullptr &&
        script_path[0] != '\0') {
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
    return WatchWindowsLiveSwitches(store, callback, options);
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
