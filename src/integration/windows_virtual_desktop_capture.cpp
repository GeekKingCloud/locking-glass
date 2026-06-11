#include "windows_virtual_desktop_capture.h"

#if defined(_WIN32)

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace locking_glass::integration::internal {

namespace {

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

  // Window capture is intentionally conservative. Tool windows, owned windows,
  // child windows, and windows outside locked monitor bounds are reported as
  // skips so the move planner never has to guess about shell/UI internals.
  if (!monitor_match.extra_skip_reason.empty() &&
      monitor_match.touches_locked_monitor) {
    extra_skip = core::MonitorLockingSkip{
        .window = captured.window,
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

}  // namespace

std::vector<CapturedWindow> CaptureLiveWindows(
    const WindowsVirtualDesktopHelper& helper,
    const std::vector<platform::MonitorDescriptor>& monitors,
    const core::SessionRefreshResult& session,
    const std::function<DesktopIdentity(int)>& resolve_desktop) {
  std::vector<CapturedWindow> captures;
  CaptureWindowsContext context{
      .helper = &helper,
      .monitors = &monitors,
      .session = &session,
      .resolve_desktop = resolve_desktop,
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

}  // namespace locking_glass::integration::internal

#endif
