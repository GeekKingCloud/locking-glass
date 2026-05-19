#include "virtual_desktop_controller_internal.h"

#if defined(_WIN32)

#include "windows_virtual_desktop_surface.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace locking_glass::integration::internal {

namespace {

bool GuidIsZero(const GUID& guid) {
  return guid.Data1 == 0 && guid.Data2 == 0 && guid.Data3 == 0 &&
         guid.Data4[0] == 0 && guid.Data4[1] == 0 && guid.Data4[2] == 0 &&
         guid.Data4[3] == 0 && guid.Data4[4] == 0 && guid.Data4[5] == 0 &&
         guid.Data4[6] == 0 && guid.Data4[7] == 0;
}

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

std::filesystem::path ResolvePreferredHelperDllPath(
    const std::filesystem::path& asset_root) {
  wchar_t helper_path[MAX_PATH];
  const DWORD length = GetEnvironmentVariableW(
      L"LOCKING_GLASS_VIRTUAL_DESKTOP_HELPER", helper_path, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return std::filesystem::path(helper_path);
  }

  if (!asset_root.empty()) {
    const auto repository_helper =
        asset_root / "build" / "windows-live-desktop-probe" /
        "VirtualDesktopAccessor.dll";
    if (std::filesystem::exists(repository_helper)) {
      return repository_helper;
    }

    const auto bundled_helper = asset_root / "VirtualDesktopAccessor.dll";
    if (std::filesystem::exists(bundled_helper)) {
      return bundled_helper;
    }
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

class WindowsVirtualDesktopHelper {
 public:
  using GetDesktopCountFn = int(WINAPI*)();
  using GetDesktopNameFn = int(WINAPI*)(int, char*, std::size_t);
  using GetDesktopIdByNumberFn = GUID(WINAPI*)(int);
  using GetWindowDesktopNumberFn = int(WINAPI*)(HWND);
  using MoveWindowToDesktopNumberFn = int(WINAPI*)(HWND, int);

  WindowsVirtualDesktopHelper(HMODULE library,
                              GetDesktopCountFn get_desktop_count,
                              GetDesktopNameFn get_desktop_name,
                              GetDesktopIdByNumberFn get_desktop_id_by_number,
                              GetWindowDesktopNumberFn get_window_desktop_number,
                              MoveWindowToDesktopNumberFn move_window_to_desktop_number)
      : library_(library),
        get_desktop_count_(get_desktop_count),
        get_desktop_name_(get_desktop_name),
        get_desktop_id_by_number_(get_desktop_id_by_number),
        get_window_desktop_number_(get_window_desktop_number),
        move_window_to_desktop_number_(move_window_to_desktop_number) {}

  ~WindowsVirtualDesktopHelper() {
    if (library_ != nullptr) {
      FreeLibrary(library_);
    }
  }

  WindowsVirtualDesktopHelper(const WindowsVirtualDesktopHelper&) = delete;
  WindowsVirtualDesktopHelper& operator=(const WindowsVirtualDesktopHelper&) =
      delete;

  int GetDesktopCount() const {
    return get_desktop_count_ != nullptr ? get_desktop_count_() : 0;
  }

  DesktopIdentity GetDesktopIdentity(int desktop_number) const {
    std::string desktop_name;
    if (get_desktop_name_ != nullptr) {
      char buffer[1024] = {};
      if (get_desktop_name_(desktop_number, buffer, sizeof(buffer)) >= 0) {
        desktop_name = buffer;
      }
    }

    std::string desktop_guid;
    if (get_desktop_id_by_number_ != nullptr) {
      const GUID guid = get_desktop_id_by_number_(desktop_number);
      if (!GuidIsZero(guid)) {
        char buffer[64];
        std::snprintf(
            buffer, sizeof(buffer),
            "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
            guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
            guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
            guid.Data4[6], guid.Data4[7]);
        desktop_guid = buffer;
      }
    }

    return MakeDesktopIdentity(desktop_number, desktop_guid, desktop_name);
  }

  std::vector<DesktopIdentity> ListDesktops() const {
    std::vector<DesktopIdentity> desktops;
    const int desktop_count = GetDesktopCount();
    if (desktop_count <= 0) {
      return desktops;
    }

    desktops.reserve(static_cast<std::size_t>(desktop_count));
    for (int desktop_number = 0; desktop_number < desktop_count;
         ++desktop_number) {
      desktops.push_back(GetDesktopIdentity(desktop_number));
    }
    return desktops;
  }

  int GetWindowDesktopNumber(HWND window) const {
    return get_window_desktop_number_(window);
  }

  int MoveWindowToDesktopNumber(HWND window, int desktop_number) const {
    return move_window_to_desktop_number_(window, desktop_number);
  }

  static std::unique_ptr<WindowsVirtualDesktopHelper> Load(
      const std::filesystem::path& repository_root, std::string* detail) {
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

      const FARPROC get_desktop_count_symbol =
          GetProcAddress(library, "GetDesktopCount");
      const FARPROC get_desktop_name_symbol =
          GetProcAddress(library, "GetDesktopName");
      const FARPROC get_desktop_id_by_number_symbol =
          GetProcAddress(library, "GetDesktopIdByNumber");
      const FARPROC get_window_desktop_number_symbol =
          GetProcAddress(library, "GetWindowDesktopNumber");
      const FARPROC move_window_to_desktop_number_symbol =
          GetProcAddress(library, "MoveWindowToDesktopNumber");
      const auto get_desktop_count =
          get_desktop_count_symbol != nullptr
              ? std::bit_cast<GetDesktopCountFn>(get_desktop_count_symbol)
              : nullptr;
      const auto get_desktop_name =
          get_desktop_name_symbol != nullptr
              ? std::bit_cast<GetDesktopNameFn>(get_desktop_name_symbol)
              : nullptr;
      const auto get_desktop_id_by_number =
          get_desktop_id_by_number_symbol != nullptr
              ? std::bit_cast<GetDesktopIdByNumberFn>(
                    get_desktop_id_by_number_symbol)
              : nullptr;
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
      if (get_desktop_count == nullptr || get_window_desktop_number == nullptr ||
          move_window_to_desktop_number == nullptr) {
        last_error =
            "VirtualDesktopAccessor.dll at " + candidate.string() +
            " was missing GetDesktopCount, GetWindowDesktopNumber, or "
            "MoveWindowToDesktopNumber.";
        FreeLibrary(library);
        continue;
      }

      if (detail != nullptr) {
        *detail = candidate.string();
      }
      return std::make_unique<WindowsVirtualDesktopHelper>(
          library, get_desktop_count, get_desktop_name,
          get_desktop_id_by_number, get_window_desktop_number,
          move_window_to_desktop_number);
    }

    if (detail != nullptr) {
      *detail = last_error;
    }
    return nullptr;
  }

 private:
  HMODULE library_ = nullptr;
  GetDesktopCountFn get_desktop_count_ = nullptr;
  GetDesktopNameFn get_desktop_name_ = nullptr;
  GetDesktopIdByNumberFn get_desktop_id_by_number_ = nullptr;
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
  std::function<DesktopIdentity(int)> resolve_desktop;
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
        .desktop = {},
        .handle = window,
        .move_block_reason = {},
        .forced_failure_detail = {},
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
    DesktopIdentity desktop = context->resolve_desktop(desktop_number);
    captured_window.desktop_id = FormatDesktopIdentity(desktop);
    context->captures->push_back(CapturedWindow{
        .window = std::move(captured_window),
        .desktop = std::move(desktop),
        .handle = window,
        .move_block_reason = {},
        .forced_failure_detail = {},
        .override_skip_reason = {},
        .extra_skip = std::nullopt,
    });
  } else {
    context->captures->push_back(CapturedWindow{
        .window = std::move(captured_window),
        .desktop = {},
        .handle = window,
        .move_block_reason = {},
        .forced_failure_detail = {},
        .override_skip_reason = {},
        .extra_skip = std::nullopt,
    });
  }

  auto& captured = context->captures->back();
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
        .window = captured.window,
        .reason = "window desktop could not be resolved safely",
    };
  } else if (!unsafe_reason.empty() && monitor_match.monitor != nullptr &&
             !captured.window.desktop_id.empty()) {
    captured.window.can_move = false;
    captured.move_block_reason = unsafe_reason;
    if (IsLockedPresentMonitor(*context->session, *monitor_match.monitor)) {
      override_skip_reason = unsafe_reason;
    }
  }

  captured.override_skip_reason = std::move(override_skip_reason);
  captured.extra_skip = std::move(extra_skip);
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
      .resolve_desktop = [&desktop_context](const int desktop_number) {
        return desktop_context.Resolve(desktop_number);
      },
      .captures = &captures,
  };

  EnumWindows(CaptureLiveWindow, reinterpret_cast<LPARAM>(&context));
  return captures;
}

std::vector<CapturedWindow> CaptureLiveWindowsForReturn(
    const WindowsVirtualDesktopHelper& helper,
    const std::vector<platform::MonitorDescriptor>& monitors) {
  core::SessionRefreshResult session{
      .snapshot = {},
      .storage_path = {},
      .loaded_from_disk = false,
      .restored_locked_monitors = 0,
      .disconnected_monitors = 0,
      .new_monitors = 0,
      .review_monitors = 0,
      .storage_issue = core::SessionStorageIssue::kNone,
      .recovered_invalid_data = false,
      .invalid_storage_backup_path = {},
      .storage_detail = {},
  };
  session.snapshot.monitors.reserve(monitors.size());
  for (const auto& monitor : monitors) {
    session.snapshot.monitors.push_back(core::SessionMonitorState{
        .monitor = monitor,
        .locked = false,
        .is_present = true,
        .requires_confirmation = false,
    });
  }

  const auto desktops = helper.ListDesktops();
  CaptureWindowsContext context{
      .helper = &helper,
      .monitors = &monitors,
      .session = &session,
      .resolve_desktop = [&desktops](const int desktop_number) {
        if (const auto* desktop = FindMatchingDesktop(
                desktops, MakeDesktopIdentity(desktop_number, "", ""));
            desktop != nullptr) {
          return *desktop;
        }
        return MakeDesktopIdentity(desktop_number, "", "");
      },
      .captures = nullptr,
  };

  std::vector<CapturedWindow> captures;
  context.captures = &captures;
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

  const WindowDesktopContextFormatter desktop_context{event};
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
    const int destination_desktop_number =
        move.to_desktop_id == scenario.target_desktop_id
            ? event.target_desktop_number
            : event.source_desktop_number;
    const DesktopIdentity to_desktop =
        desktop_context.Resolve(destination_desktop_number);
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
    const DesktopIdentity actual_desktop =
        actual_desktop_number >= 0
            ? desktop_context.Resolve(actual_desktop_number)
            : MakeDisplayOnlyDesktopIdentity("<unknown-desktop>");
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
      probe.helper_watch_ready && probe.helper_move_ready) {
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

int WatchWindowsLiveSwitches(const core::SessionStore& store,
                             const DesktopSwitchCallback& callback,
                             const DesktopWatchOptions& options) {
  const auto asset_root = FindLiveWatchAssetRoot();
  if (asset_root.empty()) {
    std::cerr
        << "LockingGlass could not locate bundled live desktop watch assets "
           "beside the executable or in the executable's repository ancestors, "
           "so the live Windows desktop watch path cannot start.\n";
    return 1;
  }

  const auto log_path = BuildLiveWatchLogPath();
  const auto command_script_path =
      BuildLiveWatchCommandScript(asset_root, log_path, options);
  const std::string command =
      QuoteCommandArgument(command_script_path.string()) + " 2>&1";
  FILE* pipe = _popen(command.c_str(), "r");
  if (pipe == nullptr) {
    std::cerr
        << "LockingGlass could not launch the live Windows desktop watch helper.\n";
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
