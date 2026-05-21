#include "background_session_internal.h"
#include "tray_icon_drawing_win32.h"

#if defined(_WIN32)

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace locking_glass::platform::internal {

namespace {

using std::max;
using std::min;

constexpr char kBackgroundDesktopReportPathEnv[] =
    "LOCKING_GLASS_BACKGROUND_REPORT_PATH";
constexpr UINT kTrayIconMessage = WM_APP + 1;
constexpr UINT kLiveControllerWatchFailedMessage = WM_APP + 2;
constexpr UINT_PTR kMenuHoverTimer = 1;
constexpr UINT kMenuCommandMonitorBase = 1000;
constexpr UINT kMenuCommandRefresh = 9000;
constexpr UINT kMenuCommandExit = 9001;
constexpr wchar_t kBackgroundWindowClassName[] = L"LockingGlassBackgroundWindow";
constexpr wchar_t kIdentifyOverlayWindowClassName[] =
    L"LockingGlassIdentifyOverlayWindow";
constexpr int kIdentifyOverlayInset = 18;
constexpr int kIdentifyOverlayBorder = 6;
constexpr int kIdentifyOverlayCardWidth = 360;
constexpr int kIdentifyOverlayCardHeight = 108;
constexpr BYTE kIdentifyOverlayOpacity = 168;
constexpr int kTrayPopupIconGap = 10;
constexpr UINT kMenuHoverPollMilliseconds = 45;

enum class TrayMenuMode {
  kMonitorOnly,
  kManagement,
};

std::wstring Widen(const std::string& value) {
  if (value.empty()) {
    return L"";
  }

  const int size =
      MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (size <= 1) {
    return L"";
  }

  std::wstring widened(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, widened.data(), size);
  widened.pop_back();
  return widened;
}

struct BackgroundSessionState {
  // Owned by the hidden-window message thread. The window return tracker is the
  // only state intentionally shared with the desktop-watch worker.
  core::SessionStore session_store;
  std::unique_ptr<MonitorGateway> monitor_gateway = CreateMonitorGateway();
  std::shared_ptr<WindowReturnTracker> window_return_tracker =
      std::make_shared<WindowReturnTracker>();
  std::unique_ptr<locking_glass::integration::VirtualDesktopController>
      unlock_return_controller;
  core::SessionRefreshResult session;
  BackgroundSessionObserver observer;
  NOTIFYICONDATAW tray_icon{};
  HICON tray_icon_handle = nullptr;
  bool tray_icon_handle_owned = false;
  HWND identify_overlay_window = nullptr;
  std::wstring identify_overlay_title;
  std::wstring identify_overlay_message;
  core::TrayMenuModel active_menu_model;
  HMENU active_menu_handle = nullptr;
  std::string highlighted_monitor_key;
  bool tray_menu_open = false;
  UINT taskbar_created_message = 0;
  locking_glass::integration::CapabilityReport live_controller_capability =
      MakeReadyControllerCapability();
  bool live_controller_watcher_started = true;
};

void PollMenuHover(HWND window, BackgroundSessionState* state);

thread_local HWND g_menu_hook_window = nullptr;
thread_local BackgroundSessionState* g_menu_hook_state = nullptr;

LRESULT CALLBACK MenuMessageHookProc(int code, WPARAM w_param,
                                     LPARAM l_param) {
  if (code >= 0 && w_param == MSGF_MENU && l_param != 0 &&
      g_menu_hook_window != nullptr && g_menu_hook_state != nullptr) {
    const auto* message = reinterpret_cast<const MSG*>(l_param);
    if (message->message == WM_MOUSEMOVE) {
      PollMenuHover(g_menu_hook_window, g_menu_hook_state);
    }
  }
  return CallNextHookEx(nullptr, code, w_param, l_param);
}

LRESULT CALLBACK MenuMouseHookProc(int code, WPARAM w_param, LPARAM l_param) {
  (void)l_param;
  if (code >= 0 && (w_param == WM_MOUSEMOVE || w_param == WM_NCMOUSEMOVE) &&
      g_menu_hook_window != nullptr && g_menu_hook_state != nullptr) {
    PollMenuHover(g_menu_hook_window, g_menu_hook_state);
  }
  return CallNextHookEx(nullptr, code, w_param, l_param);
}

UINT BuildTrayPopupAlignmentFlags(const POINT menu_origin) {
  MONITORINFO monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  const HMONITOR monitor =
      MonitorFromPoint(menu_origin, MONITOR_DEFAULTTONEAREST);
  if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info)) {
    const LONG middle_y =
        monitor_info.rcWork.top +
        ((monitor_info.rcWork.bottom - monitor_info.rcWork.top) / 2);
    return menu_origin.y >= middle_y ? TPM_BOTTOMALIGN : TPM_TOPALIGN;
  }

  return TPM_BOTTOMALIGN;
}

POINT BuildTrayPopupOrigin(const POINT menu_origin,
                           const UINT alignment_flags) {
  POINT popup_origin = menu_origin;
  if ((alignment_flags & TPM_BOTTOMALIGN) != 0) {
    popup_origin.y -= kTrayPopupIconGap;
  } else if ((alignment_flags & TPM_TOPALIGN) != 0) {
    popup_origin.y += kTrayPopupIconGap;
  }
  return popup_origin;
}

std::string ReadBackgroundDesktopReportPathFromEnv() {
  const char* raw_value = std::getenv(kBackgroundDesktopReportPathEnv);
  if (raw_value == nullptr) {
    return {};
  }

  return raw_value;
}

void EmitBackgroundDesktopSwitchReport(
    const std::string& report_path,
    const locking_glass::integration::DesktopSwitchReport& report) {
  const std::string formatted =
      locking_glass::integration::FormatDesktopSwitchReport(report);
  std::cout << formatted << std::flush;

  if (report_path.empty()) {
    return;
  }

  std::ofstream output(report_path,
                       std::ios::out | std::ios::app | std::ios::binary);
  if (!output.is_open()) {
    std::cerr << "Locking Glass could not append the background desktop switch "
                 "report to "
              << report_path << '\n';
    return;
  }

  output << formatted;
  if (!formatted.empty() && formatted.back() != '\n') {
    output << '\n';
  }
  output.flush();
}

std::string BuildMonitorIdentityKey(const MonitorDescriptor& monitor) {
  if (!monitor.device_path.empty()) {
    return monitor.device_path;
  }
  if (!monitor.stable_id.empty()) {
    return monitor.stable_id;
  }
  return monitor.label;
}

void ReleaseTrayIconHandle(BackgroundSessionState* state) {
  if (state == nullptr || state->tray_icon_handle == nullptr) {
    return;
  }
  if (state->tray_icon_handle_owned) {
    DestroyIcon(state->tray_icon_handle);
  }
  state->tray_icon_handle = nullptr;
  state->tray_icon_handle_owned = false;
}

void UpdateTrayVisuals(HWND window, BackgroundSessionState* state,
                       const core::TrayMenuModel& model) {
  if (window == nullptr || state == nullptr) {
    return;
  }

  HICON next_icon = CreateTrayStatusIcon(model.icon);
  bool next_icon_owned = next_icon != nullptr;
  if (next_icon == nullptr) {
    next_icon = static_cast<HICON>(
        LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                   LR_SHARED));
  }
  if (next_icon == nullptr) {
    return;
  }

  ReleaseTrayIconHandle(state);
  state->tray_icon_handle = next_icon;
  state->tray_icon_handle_owned = next_icon_owned;

  state->tray_icon.cbSize = sizeof(state->tray_icon);
  state->tray_icon.hWnd = window;
  state->tray_icon.uID = 1;
  state->tray_icon.uFlags = NIF_ICON | NIF_TIP;
  state->tray_icon.hIcon = state->tray_icon_handle;
  const auto tooltip = Widen(model.icon.tooltip);
  wcsncpy_s(state->tray_icon.szTip, tooltip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &state->tray_icon);
}

LRESULT CALLBACK IdentifyOverlayWindowProc(HWND window, UINT message,
                                           WPARAM w_param, LPARAM l_param) {
  (void)w_param;
  if (message == WM_NCCREATE) {
    const auto* create_struct =
        reinterpret_cast<const CREATESTRUCTW*>(l_param);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create_struct->lpCreateParams));
    return TRUE;
  }

  auto* state = reinterpret_cast<BackgroundSessionState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT bounds{};
      GetClientRect(window, &bounds);
      const int width = bounds.right - bounds.left;
      const int height = bounds.bottom - bounds.top;

      HBRUSH background = CreateSolidBrush(RGB(8, 12, 18));
      FillRect(dc, &bounds, background);
      DeleteObject(background);

      HPEN border = CreatePen(PS_SOLID, kIdentifyOverlayBorder,
                              RGB(47, 143, 255));
      HGDIOBJ previous_pen = SelectObject(dc, border);
      HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, bounds.left + (kIdentifyOverlayBorder / 2),
                bounds.top + (kIdentifyOverlayBorder / 2),
                bounds.right - (kIdentifyOverlayBorder / 2),
                bounds.bottom - (kIdentifyOverlayBorder / 2));
      SelectObject(dc, previous_brush);
      SelectObject(dc, previous_pen);
      DeleteObject(border);

      RECT inner_bounds = bounds;
      InflateRect(&inner_bounds, -kIdentifyOverlayInset, -kIdentifyOverlayInset);
      HPEN inner_border = CreatePen(PS_SOLID, 2, RGB(130, 190, 255));
      previous_pen = SelectObject(dc, inner_border);
      previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, inner_bounds.left, inner_bounds.top, inner_bounds.right,
                inner_bounds.bottom);
      SelectObject(dc, previous_brush);
      SelectObject(dc, previous_pen);
      DeleteObject(inner_border);

      RECT card_rect{};
      const int card_width =
          min(kIdentifyOverlayCardWidth,
              max(220, width - (kIdentifyOverlayInset * 4)));
      const int card_height =
          min(kIdentifyOverlayCardHeight,
              max(86, height - (kIdentifyOverlayInset * 4)));
      card_rect.left = bounds.left + max(0, (width - card_width) / 2);
      card_rect.top = bounds.top + max(0, (height - card_height) / 2);
      card_rect.right = card_rect.left + card_width;
      card_rect.bottom = card_rect.top + card_height;

      HBRUSH card_fill = CreateSolidBrush(RGB(18, 25, 36));
      HPEN card_border = CreatePen(PS_SOLID, 2, RGB(130, 190, 255));
      previous_pen = SelectObject(dc, card_border);
      previous_brush = SelectObject(dc, card_fill);
      RoundRect(dc, card_rect.left, card_rect.top, card_rect.right,
                card_rect.bottom, 20, 20);
      SelectObject(dc, previous_brush);
      SelectObject(dc, previous_pen);
      DeleteObject(card_border);
      DeleteObject(card_fill);

      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(244, 247, 250));
      HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
      HGDIOBJ previous_font = SelectObject(dc, font);

      RECT title_rect{card_rect.left + 18, card_rect.top + 16,
                      card_rect.right - 18, card_rect.top + 44};
      RECT message_rect{card_rect.left + 18, card_rect.top + 46,
                        card_rect.right - 18, card_rect.bottom - 18};
      if (state != nullptr) {
        DrawTextW(dc, state->identify_overlay_title.c_str(), -1, &title_rect,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextW(dc, state->identify_overlay_message.c_str(), -1, &message_rect,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
      }

      SelectObject(dc, previous_font);
      EndPaint(window, &paint);
      return 0;
    }
    default:
      return DefWindowProcW(window, message, w_param, l_param);
  }
}

bool EnsureIdentifyOverlayWindow(HINSTANCE instance,
                                 BackgroundSessionState* state) {
  if (state == nullptr) {
    return false;
  }
  if (state->identify_overlay_window != nullptr) {
    return true;
  }

  // The identify overlay must stay out of Alt-Tab, avoid activation, and pass
  // clicks through to the desktop while still drawing above normal windows.
  state->identify_overlay_window = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE |
          WS_EX_TRANSPARENT,
      kIdentifyOverlayWindowClassName, L"Locking Glass Identify Overlay",
      WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, state);
  if (state->identify_overlay_window == nullptr) {
    return false;
  }

  SetLayeredWindowAttributes(state->identify_overlay_window, 0,
                             kIdentifyOverlayOpacity, LWA_ALPHA);
  return true;
}

bool HideIdentifyOverlay(BackgroundSessionState* state) {
  if (state == nullptr) {
    return false;
  }
  const bool was_visible =
      !state->highlighted_monitor_key.empty() &&
      state->identify_overlay_window != nullptr &&
      IsWindowVisible(state->identify_overlay_window);
  state->highlighted_monitor_key.clear();
  if (state->identify_overlay_window != nullptr) {
    ShowWindow(state->identify_overlay_window, SW_HIDE);
  }
  return was_visible;
}

void ShowIdentifyOverlay(BackgroundSessionState* state,
                         const core::TrayIdentifyOverlay& overlay) {
  if (state == nullptr || !overlay.visible ||
      state->identify_overlay_window == nullptr) {
    return;
  }

  state->identify_overlay_title = Widen(overlay.title);
  state->identify_overlay_message = Widen(overlay.message);
  state->highlighted_monitor_key = BuildMonitorIdentityKey(overlay.monitor);

  const int monitor_width =
      max(1, overlay.monitor.bounds.right - overlay.monitor.bounds.left);
  const int monitor_height =
      max(1, overlay.monitor.bounds.bottom - overlay.monitor.bounds.top);
  const int x = overlay.monitor.bounds.left;
  const int y = overlay.monitor.bounds.top;

  SetWindowPos(state->identify_overlay_window, HWND_TOPMOST, x, y,
               monitor_width, monitor_height,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(state->identify_overlay_window, nullptr, TRUE);
  UpdateWindow(state->identify_overlay_window);
}

core::TrayMenuModel BuildBackgroundTrayMenuModel(BackgroundSessionState* state,
                                                 std::string trigger) {
  return core::BuildTrayMenuModel(state->session, std::move(trigger));
}

void PublishHoverClearEvent(BackgroundSessionState* state) {
  if (state == nullptr) {
    return;
  }

  auto hover_model = state->active_menu_model;
  hover_model.trigger = "tray-hover-clear";
  PublishEvent(state->observer, hover_model, state->live_controller_capability,
               state->live_controller_watcher_started, true);
}

void ShowReviewNotification(HWND window, BackgroundSessionState* state,
                            const core::MonitorReviewPrompt& prompt) {
  if (window == nullptr || state == nullptr || !prompt.visible) {
    return;
  }

  auto notification = state->tray_icon;
  notification.cbSize = sizeof(notification);
  notification.hWnd = window;
  notification.uID = 1;
  notification.uFlags = NIF_INFO;
  notification.dwInfoFlags = NIIF_INFO;
  notification.uTimeout = 10000;

  const auto title = Widen(prompt.title);
  const auto message = Widen(prompt.message);
  wcsncpy_s(notification.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(notification.szInfo, message.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &notification);
}

void LogControllerUnavailable(
    const locking_glass::integration::CapabilityReport& capability) {
  if (IsLiveControllerAvailable(capability)) {
    return;
  }

  std::cerr << "Locking Glass background live desktop control unavailable: "
            << capability.detail << '\n';
}

void ShowControllerUnavailableError(
    HWND owner,
    const locking_glass::integration::CapabilityReport& capability) {
  const std::wstring message =
      Widen("Locking Glass is closing because live desktop control is "
            "unavailable.\n\n" +
            capability.detail);
  MessageBoxW(owner, message.c_str(), L"Locking Glass unavailable",
              MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

core::TrayMenuModel RefreshTrayModel(
    HWND window, BackgroundSessionState* state, std::string trigger,
    const bool tray_menu_visible, const bool emit_prompt,
    const BackgroundSessionUnlockReturn& unlock_return =
        BackgroundSessionUnlockReturn{}) {
  state->session =
      state->session_store.Restore(state->monitor_gateway->Enumerate());
  const auto model = BuildBackgroundTrayMenuModel(state, std::move(trigger));
  const auto prompt = emit_prompt
                          ? core::BuildMonitorReviewPrompt(state->session)
                          : core::MonitorReviewPrompt{};
  if (!tray_menu_visible) {
    HideIdentifyOverlay(state);
  }
  UpdateTrayVisuals(window, state, model);
  PublishEvent(state->observer, model, state->live_controller_capability,
               state->live_controller_watcher_started, tray_menu_visible,
               prompt, core::TrayIdentifyOverlay{}, unlock_return);
  if (IsLiveControllerAvailable(state->live_controller_capability) &&
      state->live_controller_watcher_started) {
    ShowReviewNotification(window, state, prompt);
  }
  return model;
}

void UnlockAndReturnAllOnExit(BackgroundSessionState* state) {
  if (state == nullptr) {
    return;
  }

  const auto loaded_session = state->session_store.Load();
  for (const auto& monitor_state : loaded_session.snapshot.monitors) {
    if (!monitor_state.locked) {
      continue;
    }

    const auto unlock_return =
        RunUnlockReturn(state->live_controller_capability,
                        state->unlock_return_controller.get(),
                        state->window_return_tracker, monitor_state.monitor);
    (void)unlock_return;
  }

  // Lock selections are current-run state. Even if Windows refuses to move a
  // window back during shutdown, the next process must start with no monitors
  // locked.
  state->session =
      state->session_store.StartUnlocked(state->monitor_gateway->Enumerate());
}

void RepublishCurrentModel(
    HWND window, BackgroundSessionState* state, std::string trigger,
    const bool tray_menu_visible,
    const core::MonitorReviewPrompt& prompt = core::MonitorReviewPrompt{},
    const core::TrayIdentifyOverlay& highlight = core::TrayIdentifyOverlay{},
    const BackgroundSessionUnlockReturn& unlock_return =
        BackgroundSessionUnlockReturn{}) {
  auto model = BuildBackgroundTrayMenuModel(state, std::move(trigger));
  if (!tray_menu_visible) {
    HideIdentifyOverlay(state);
  }
  UpdateTrayVisuals(window, state, model);
  PublishEvent(state->observer, model, state->live_controller_capability,
               state->live_controller_watcher_started, tray_menu_visible,
               prompt, highlight, unlock_return);
  if (IsLiveControllerAvailable(state->live_controller_capability) &&
      state->live_controller_watcher_started) {
    ShowReviewNotification(window, state, prompt);
  }
}

bool AddTrayIcon(HWND window, BackgroundSessionState* state) {
  if (window == nullptr || state == nullptr) {
    return false;
  }

  ZeroMemory(&state->tray_icon, sizeof(state->tray_icon));
  state->tray_icon.cbSize = sizeof(state->tray_icon);
  state->tray_icon.hWnd = window;
  state->tray_icon.uID = 1;
  state->tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  state->tray_icon.uCallbackMessage = kTrayIconMessage;

  const auto model = BuildBackgroundTrayMenuModel(state, "startup");
  state->tray_icon_handle = CreateTrayStatusIcon(model.icon);
  state->tray_icon_handle_owned = state->tray_icon_handle != nullptr;
  if (state->tray_icon_handle == nullptr) {
    state->tray_icon_handle = static_cast<HICON>(
        LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                   LR_SHARED));
    state->tray_icon_handle_owned = false;
  }
  if (state->tray_icon_handle == nullptr) {
    return false;
  }
  state->tray_icon.hIcon = state->tray_icon_handle;

  // Shell_NotifyIcon keeps its own icon reference; we still track ownership so
  // dynamically-created HICONs are destroyed when the tray icon is replaced.
  const auto tooltip = Widen(model.icon.tooltip);
  wcsncpy_s(state->tray_icon.szTip, tooltip.c_str(), _TRUNCATE);

  if (!Shell_NotifyIconW(NIM_ADD, &state->tray_icon)) {
    ReleaseTrayIconHandle(state);
    return false;
  }

  state->tray_icon.uVersion = NOTIFYICON_VERSION_4;
  // Version 4 gives the tray callback the compact lParam event code shape that
  // the window procedure decodes with LOWORD(l_param).
  Shell_NotifyIconW(NIM_SETVERSION, &state->tray_icon);
  return true;
}

void RemoveTrayIcon(BackgroundSessionState* state) {
  if (state == nullptr) {
    return;
  }

  HideIdentifyOverlay(state);
  if (state->tray_icon.hWnd != nullptr) {
    state->tray_icon.uFlags = 0;
    Shell_NotifyIconW(NIM_DELETE, &state->tray_icon);
  }
  ReleaseTrayIconHandle(state);
}

void ShowTrayMenu(HWND window, BackgroundSessionState* state,
                  const TrayMenuMode menu_mode) {
  if (window == nullptr || state == nullptr) {
    return;
  }

  std::string trigger = "tray-click";
  BackgroundSessionUnlockReturn unlock_return;
  POINT menu_origin{};
  GetCursorPos(&menu_origin);
  while (true) {
    const auto model = RefreshTrayModel(window, state, trigger, true, false,
                                        unlock_return);
    unlock_return = {};
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
      state->tray_menu_open = false;
      state->active_menu_handle = nullptr;
      state->active_menu_model = {};
      HideIdentifyOverlay(state);
      return;
    }

    state->active_menu_model =
        menu_mode == TrayMenuMode::kMonitorOnly ? model : core::TrayMenuModel{};
    state->active_menu_handle = menu;
    state->tray_menu_open = true;
    HideIdentifyOverlay(state);

    // Win32 menu items borrow string and bitmap storage while the menu is open,
    // so these backing containers must live until after TrackPopupMenu returns.
    std::vector<std::wstring> labels;
    std::vector<HBITMAP> menu_bitmaps;
    if (menu_mode == TrayMenuMode::kManagement) {
      labels.reserve(2U);
      AppendMenuW(menu, MF_STRING, kMenuCommandRefresh, L"Refresh monitor list");
      AppendMenuW(menu, MF_STRING, kMenuCommandExit, L"Exit Locking Glass");
    } else {
      labels.reserve(model.monitors.size() + 8U);
      menu_bitmaps.reserve(model.monitors.size());
      if (!model.menu_status.empty()) {
        labels.push_back(Widen(model.menu_status));
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, labels.back().c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      }

      if (model.monitors.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No monitors detected");
      } else {
        for (std::size_t index = 0; index < model.monitors.size(); ++index) {
          const UINT command =
              kMenuCommandMonitorBase + static_cast<UINT>(index);
          labels.push_back(Widen(model.monitors[index].menu_label));
          AppendMenuW(menu, MF_STRING, command, labels.back().c_str());

          HBITMAP bitmap =
              CreateMenuPadlockBitmap(model.monitors[index].padlock_icon);
          if (bitmap == nullptr) {
            continue;
          }

          MENUITEMINFOW item_info{};
          item_info.cbSize = sizeof(item_info);
          item_info.fMask = MIIM_BITMAP;
          item_info.hbmpItem = bitmap;
          if (!SetMenuItemInfoW(menu, command, FALSE, &item_info)) {
            DeleteObject(bitmap);
            continue;
          }
          menu_bitmaps.push_back(bitmap);
        }
      }

      if (!model.menu_instruction.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        labels.push_back(Widen(model.menu_instruction));
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, labels.back().c_str());
      }
    }

    SetForegroundWindow(window);
    // TrackPopupMenu's foreground-window dance and trailing WM_NULL are the
    // standard Win32 tray-menu pattern; without them the popup can linger after
    // focus moves away from the hidden message window.
    g_menu_hook_window = window;
    g_menu_hook_state = state;
    const HHOOK menu_hook =
        SetWindowsHookExW(WH_MSGFILTER, MenuMessageHookProc, nullptr,
                          GetCurrentThreadId());
    const HHOOK mouse_hook = SetWindowsHookExW(
        WH_MOUSE, MenuMouseHookProc, nullptr, GetCurrentThreadId());
    SetTimer(window, kMenuHoverTimer, kMenuHoverPollMilliseconds, nullptr);
    const UINT alignment_flags = BuildTrayPopupAlignmentFlags(menu_origin);
    const POINT popup_origin =
        BuildTrayPopupOrigin(menu_origin, alignment_flags);
    const UINT command =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | alignment_flags,
                       popup_origin.x, popup_origin.y, 0, window, nullptr);
    KillTimer(window, kMenuHoverTimer);
    if (mouse_hook != nullptr) {
      UnhookWindowsHookEx(mouse_hook);
    }
    if (menu_hook != nullptr) {
      UnhookWindowsHookEx(menu_hook);
    }
    g_menu_hook_window = nullptr;
    g_menu_hook_state = nullptr;
    DestroyMenu(menu);
    for (const HBITMAP bitmap : menu_bitmaps) {
      DeleteObject(bitmap);
    }
    PostMessageW(window, WM_NULL, 0, 0);
    state->tray_menu_open = false;
    state->active_menu_handle = nullptr;
    state->active_menu_model = {};
    HideIdentifyOverlay(state);

    if (menu_mode == TrayMenuMode::kMonitorOnly &&
        command >= kMenuCommandMonitorBase &&
        command < kMenuCommandMonitorBase + model.monitors.size()) {
      const auto monitor =
          model.monitors[command - kMenuCommandMonitorBase].monitor;
      bool locked_after = false;
      if (!core::ToggleMonitorLock(state->session_store, &state->session.snapshot,
                                   monitor, &locked_after)) {
        return;
      }
      if (state->window_return_tracker != nullptr && locked_after) {
        state->window_return_tracker->ClearMonitor(
            BuildTrackedMonitorKey(monitor));
      }
      unlock_return =
          !locked_after
              ? RunUnlockReturn(state->live_controller_capability,
                                state->unlock_return_controller.get(),
                                state->window_return_tracker, monitor)
              : BackgroundSessionUnlockReturn{};
      trigger = "tray-toggle";
      continue;
    }

    if (menu_mode == TrayMenuMode::kManagement &&
        command == kMenuCommandRefresh) {
      RefreshTrayModel(window, state, "tray-refresh", false, true);
      return;
    }

    if (menu_mode == TrayMenuMode::kManagement &&
        command == kMenuCommandExit) {
      DestroyWindow(window);
    }
    return;
  }
}

void UpdateHoverOverlay(BackgroundSessionState* state, const UINT command,
                        const UINT flags, const LPARAM menu_handle) {
  if (state == nullptr || !state->tray_menu_open) {
    return;
  }

  if (flags == 0xFFFF && menu_handle == 0) {
    if (HideIdentifyOverlay(state)) {
      PublishHoverClearEvent(state);
    }
    return;
  }

  if ((flags & (MF_POPUP | MF_SEPARATOR)) != 0 ||
      command < kMenuCommandMonitorBase ||
      command >=
          kMenuCommandMonitorBase + state->active_menu_model.monitors.size()) {
    if (HideIdentifyOverlay(state)) {
      PublishHoverClearEvent(state);
    }
    return;
  }

  const auto& hovered_monitor =
      state->active_menu_model.monitors[command - kMenuCommandMonitorBase];
  const auto key = BuildMonitorIdentityKey(hovered_monitor.monitor);
  if (key == state->highlighted_monitor_key &&
      state->identify_overlay_window != nullptr &&
      IsWindowVisible(state->identify_overlay_window)) {
    return;
  }

  const auto overlay = core::BuildTrayIdentifyOverlay(hovered_monitor);
  ShowIdentifyOverlay(state, overlay);
  auto hover_model = state->active_menu_model;
  hover_model.trigger = "tray-hover";
  PublishEvent(state->observer, hover_model, state->live_controller_capability,
               state->live_controller_watcher_started, true,
               core::MonitorReviewPrompt{}, overlay);
}

void PollMenuHover(HWND window, BackgroundSessionState* state) {
  if (window == nullptr || state == nullptr || !state->tray_menu_open ||
      state->active_menu_handle == nullptr) {
    return;
  }

  POINT cursor{};
  if (!GetCursorPos(&cursor)) {
    return;
  }

  const int item_count = GetMenuItemCount(state->active_menu_handle);
  for (int index = 0; index < item_count; ++index) {
    const UINT item_state = GetMenuState(
        state->active_menu_handle, static_cast<UINT>(index), MF_BYPOSITION);
    if (item_state != static_cast<UINT>(-1) &&
        (item_state & MF_HILITE) != 0) {
      const UINT command = GetMenuItemID(state->active_menu_handle, index);
      UpdateHoverOverlay(state, command, item_state,
                         reinterpret_cast<LPARAM>(state->active_menu_handle));
      return;
    }
  }

  for (int index = 0; index < item_count; ++index) {
    RECT item_rect{};
    if (!GetMenuItemRect(nullptr, state->active_menu_handle,
                         static_cast<UINT>(index), &item_rect) ||
        !PtInRect(&item_rect, cursor)) {
      continue;
    }

    const UINT command = GetMenuItemID(state->active_menu_handle, index);
    UpdateHoverOverlay(state, command, 0,
                       reinterpret_cast<LPARAM>(state->active_menu_handle));
    return;
  }

  if (HideIdentifyOverlay(state)) {
    PublishHoverClearEvent(state);
  }
}

bool StartLiveControllerWatcher(
    HWND window, BackgroundSessionState* state,
    std::unique_ptr<locking_glass::integration::VirtualDesktopController>
        live_controller) {
  if (window == nullptr || state == nullptr || live_controller == nullptr ||
      !IsLiveControllerAvailable(state->live_controller_capability)) {
    return false;
  }

  const auto session_store = state->session_store;
  const auto report_path = ReadBackgroundDesktopReportPathFromEnv();
  const auto window_return_tracker = state->window_return_tracker;
  try {
    // The watcher thread can outlive the hidden window. Capture only values
    // that remain valid independently and report failures back with PostMessage.
    std::thread(
        [window, session_store, report_path, window_return_tracker,
         live_controller = std::move(live_controller)]() mutable {
          (void)live_controller->WatchSwitches(
              session_store,
              [report_path, session_store,
               window_return_tracker](
                  const locking_glass::integration::DesktopSwitchReport&
                      report) {
                if (window_return_tracker != nullptr) {
                  window_return_tracker->RecordSuccessfulMoves(
                      report, [session_store](
                                  const locking_glass::core::DesktopWindow&
                                      window) {
                        return IsWindowMonitorCurrentlyLocked(session_store,
                                                              window);
                      });
                }
                EmitBackgroundDesktopSwitchReport(report_path, report);
                return true;
              },
              locking_glass::integration::DesktopWatchOptions{
                  .allow_script_replay = false,
                  .required_events = 0,
                  .timeout_seconds = 0,
              });
          PostMessageW(window, kLiveControllerWatchFailedMessage, 0, 0);
        })
        .detach();
    state->live_controller_watcher_started = true;
    return true;
  } catch (...) {
    state->live_controller_watcher_started = false;
    return false;
  }
}

LRESULT CALLBACK BackgroundWindowProc(HWND window, UINT message, WPARAM w_param,
                                      LPARAM l_param) {
  if (message == WM_NCCREATE) {
    const auto* create_struct =
        reinterpret_cast<const CREATESTRUCTW*>(l_param);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create_struct->lpCreateParams));
    return TRUE;
  }

  auto* state = reinterpret_cast<BackgroundSessionState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (state != nullptr && message == state->taskbar_created_message) {
    // Explorer broadcasts TaskbarCreated after shell restarts. Re-add the icon
    // from the current model so the background app survives explorer.exe resets.
    AddTrayIcon(window, state);
    RepublishCurrentModel(window, state, "taskbar-created", false);
    return 0;
  }

  switch (message) {
    case WM_DISPLAYCHANGE:
      if (state != nullptr) {
        RefreshTrayModel(window, state, "WM_DISPLAYCHANGE", false, true);
      }
      return 0;
    case WM_MENUSELECT:
      if (state != nullptr) {
        UpdateHoverOverlay(state, LOWORD(w_param), HIWORD(w_param), l_param);
      }
      return 0;
    case WM_TIMER:
      if (state != nullptr && w_param == kMenuHoverTimer) {
        PollMenuHover(window, state);
      }
      return 0;
    case kLiveControllerWatchFailedMessage:
      if (state != nullptr) {
        const auto unavailable = MakeUnavailableControllerCapability(
            "The live desktop watcher stopped. Locking Glass is closing "
            "instead of accepting lock changes it cannot enforce.");
        LogControllerUnavailable(unavailable);
        ShowControllerUnavailableError(window, unavailable);
        DestroyWindow(window);
      }
      return 0;
    case kTrayIconMessage:
      if (state != nullptr) {
        const UINT notification =
            LOWORD(static_cast<DWORD_PTR>(l_param));
        if (state->tray_menu_open) {
          return 0;
        }
        if (notification == WM_CONTEXTMENU || notification == WM_RBUTTONUP) {
          ShowTrayMenu(window, state, TrayMenuMode::kManagement);
        } else if (notification == WM_LBUTTONUP ||
                   notification == NIN_SELECT ||
                   notification == NIN_KEYSELECT) {
          ShowTrayMenu(window, state, TrayMenuMode::kMonitorOnly);
        }
      }
      return 0;
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      if (state != nullptr) {
        UnlockAndReturnAllOnExit(state);
        RepublishCurrentModel(window, state, "exit", false);
        RemoveTrayIcon(state);
        if (state->identify_overlay_window != nullptr) {
          DestroyWindow(state->identify_overlay_window);
          state->identify_overlay_window = nullptr;
        }
      }
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, w_param, l_param);
  }
}

}  // namespace

int RunWindowsTraySession(const BackgroundSessionObserver& observer) {
  HINSTANCE instance = GetModuleHandleW(nullptr);
  auto live_controller =
      locking_glass::integration::CreateVirtualDesktopController();

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = BackgroundWindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kBackgroundWindowClassName;

  const ATOM class_atom = RegisterClassW(&window_class);
  if (class_atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return 1;
  }

  WNDCLASSW overlay_class{};
  overlay_class.lpfnWndProc = IdentifyOverlayWindowProc;
  overlay_class.hInstance = instance;
  overlay_class.lpszClassName = kIdentifyOverlayWindowClassName;
  overlay_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);

  const ATOM overlay_atom = RegisterClassW(&overlay_class);
  if (overlay_atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return 1;
  }

  auto state = std::make_unique<BackgroundSessionState>();
  state->observer = observer;
  state->unlock_return_controller =
      locking_glass::integration::CreateVirtualDesktopController();
  state->taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
  if (const auto override = ResolveBackgroundControllerCapabilityOverride();
      override.has_value()) {
    state->live_controller_capability = *override;
  } else {
    state->live_controller_capability =
        live_controller != nullptr ? live_controller->Probe()
                                   : MakeUnavailableControllerCapability(
                                         "Locking Glass could not create the "
                                         "virtual desktop controller.");
  }
  state->live_controller_watcher_started = false;
  state->session =
      state->session_store.StartUnlocked(state->monitor_gateway->Enumerate());

  if (!IsLiveControllerAvailable(state->live_controller_capability)) {
    LogControllerUnavailable(state->live_controller_capability);
    ShowControllerUnavailableError(nullptr, state->live_controller_capability);
    return 1;
  }

  HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kBackgroundWindowClassName,
                                L"Locking Glass Background", WS_OVERLAPPED, 0, 0,
                                0, 0, nullptr, nullptr, instance, state.get());
  if (window == nullptr) {
    return 1;
  }

  if (!EnsureIdentifyOverlayWindow(instance, state.get())) {
    DestroyWindow(window);
    return 1;
  }

  if (IsLiveControllerAvailable(state->live_controller_capability) &&
      !StartLiveControllerWatcher(window, state.get(),
                                  std::move(live_controller))) {
    const auto unavailable = MakeUnavailableControllerCapability(
        "Locking Glass failed to start the live desktop watcher thread.");
    LogControllerUnavailable(unavailable);
    ShowControllerUnavailableError(window, unavailable);
    DestroyWindow(window);
    return 1;
  }

  if (!AddTrayIcon(window, state.get())) {
    DestroyWindow(window);
    return 1;
  }

  const HWND console = GetConsoleWindow();
  if (console != nullptr) {
    ShowWindow(console, SW_HIDE);
  }

  RepublishCurrentModel(window, state.get(), "startup", false,
                        core::BuildMonitorReviewPrompt(state->session));

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  state.reset();
  return static_cast<int>(message.wParam);
}

}  // namespace locking_glass::platform::internal

#endif
