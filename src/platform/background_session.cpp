#include "locking_glass/platform/background_session.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "locking_glass/core/session_store.h"
#include "locking_glass/core/tray_ui.h"
#include "locking_glass/platform/monitor_gateway.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <shellapi.h>
#include <windows.h>
#endif

namespace locking_glass::platform {

namespace {

BackgroundSessionPrompt BuildBackgroundPrompt(
    const core::MonitorReviewPrompt& prompt) {
  return BackgroundSessionPrompt{
      .visible = prompt.visible,
      .title = prompt.title,
      .message = prompt.message,
      .monitors = prompt.monitors,
  };
}

BackgroundSessionHighlight BuildBackgroundHighlight(
    const core::TrayIdentifyOverlay& overlay) {
  return BackgroundSessionHighlight{
      .visible = overlay.visible,
      .monitor = overlay.monitor,
      .title = overlay.title,
      .message = overlay.message,
  };
}

BackgroundSessionEvent BuildSessionEvent(const core::TrayMenuModel& model,
                                         const bool tray_menu_visible,
                                         const core::MonitorReviewPrompt& prompt =
                                             core::MonitorReviewPrompt{},
                                         const core::TrayIdentifyOverlay& highlight =
                                             core::TrayIdentifyOverlay{}) {
  BackgroundSessionEvent event{
      .trigger = model.trigger,
      .tray_menu_visible = tray_menu_visible,
      .menu_title = model.header.title,
      .menu_subtitle = model.header.subtitle,
      .menu_instruction = model.header.instruction,
      .tray_icon_variant = model.icon.variant,
      .tray_icon_tooltip = model.icon.tooltip,
      .tray_icon_review_badge = model.icon.review_badge,
      .monitors = {},
      .prompt = BuildBackgroundPrompt(prompt),
      .highlight = BuildBackgroundHighlight(highlight),
  };
  for (const auto& monitor : model.monitors) {
    event.monitors.push_back(BackgroundSessionMenuItem{
        .monitor = monitor.monitor,
        .locked = monitor.locked,
        .requires_confirmation = monitor.requires_confirmation,
        .status_label = monitor.status_label,
        .menu_label = monitor.menu_label,
        .identify_label = monitor.identify_label,
    });
  }
  return event;
}

void PublishEvent(const BackgroundSessionObserver& observer,
                  const core::TrayMenuModel& model,
                  const bool tray_menu_visible,
                  const core::MonitorReviewPrompt& prompt =
                      core::MonitorReviewPrompt{},
                  const core::TrayIdentifyOverlay& highlight =
                      core::TrayIdentifyOverlay{}) {
  if (observer) {
    observer(BuildSessionEvent(model, tray_menu_visible, prompt, highlight));
  }
}

#if defined(_WIN32)
constexpr UINT kTrayIconMessage = WM_APP + 1;
constexpr UINT kMenuCommandMonitorBase = 1000;
constexpr UINT kMenuCommandRefresh = 9000;
constexpr UINT kMenuCommandExit = 9001;
constexpr wchar_t kBackgroundWindowClassName[] = L"LockingGlassBackgroundWindow";
constexpr wchar_t kIdentifyOverlayWindowClassName[] =
    L"LockingGlassIdentifyOverlayWindow";
constexpr int kTrayIconPixels = 16;
constexpr int kIdentifyOverlayWidth = 320;
constexpr int kIdentifyOverlayHeight = 96;

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

struct IconColor {
  BYTE red = 0;
  BYTE green = 0;
  BYTE blue = 0;
  BYTE alpha = 255;
};

struct BackgroundSessionState {
  core::SessionStore session_store;
  std::unique_ptr<MonitorGateway> monitor_gateway = CreateMonitorGateway();
  core::SessionRefreshResult session;
  BackgroundSessionObserver observer;
  NOTIFYICONDATAW tray_icon{};
  HICON tray_icon_handle = nullptr;
  bool tray_icon_handle_owned = false;
  HWND identify_overlay_window = nullptr;
  std::wstring identify_overlay_title;
  std::wstring identify_overlay_message;
  core::TrayMenuModel active_menu_model;
  std::string highlighted_monitor_key;
  bool tray_menu_open = false;
  UINT taskbar_created_message = 0;
};

std::string BuildMonitorIdentityKey(const MonitorDescriptor& monitor) {
  if (!monitor.device_path.empty()) {
    return monitor.device_path;
  }
  if (!monitor.stable_id.empty()) {
    return monitor.stable_id;
  }
  return monitor.label;
}

void SetPixel(std::vector<DWORD>* pixels, const int x, const int y,
              const IconColor color) {
  if (pixels == nullptr || x < 0 || y < 0 || x >= kTrayIconPixels ||
      y >= kTrayIconPixels) {
    return;
  }

  (*pixels)[static_cast<std::size_t>(y) * kTrayIconPixels +
            static_cast<std::size_t>(x)] =
      (static_cast<DWORD>(color.alpha) << 24) |
      (static_cast<DWORD>(color.red) << 16) |
      (static_cast<DWORD>(color.green) << 8) |
      static_cast<DWORD>(color.blue);
}

void FillRect(std::vector<DWORD>* pixels, const int left, const int top,
              const int right, const int bottom, const IconColor color) {
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      SetPixel(pixels, x, y, color);
    }
  }
}

void FillCircle(std::vector<DWORD>* pixels, const int center_x,
                const int center_y, const int radius, const IconColor color) {
  const int radius_squared = radius * radius;
  for (int y = center_y - radius; y <= center_y + radius; ++y) {
    for (int x = center_x - radius; x <= center_x + radius; ++x) {
      const int dx = x - center_x;
      const int dy = y - center_y;
      if ((dx * dx) + (dy * dy) <= radius_squared) {
        SetPixel(pixels, x, y, color);
      }
    }
  }
}

void DrawLockBadge(std::vector<DWORD>* pixels, const IconColor accent,
                   const bool open_shackle) {
  const IconColor white{255, 255, 255, 255};
  FillRect(pixels, 8, 7, 14, 13, accent);
  FillRect(pixels, 10, 9, 12, 11, white);

  SetPixel(pixels, 9, 7, white);
  SetPixel(pixels, 10, 6, white);
  SetPixel(pixels, 11, 6, white);
  SetPixel(pixels, 12, 6, white);
  SetPixel(pixels, 13, 7, white);
  SetPixel(pixels, 9, 8, white);
  if (!open_shackle) {
    SetPixel(pixels, 13, 8, white);
  }
  SetPixel(pixels, 9, 9, white);
  if (!open_shackle) {
    SetPixel(pixels, 13, 9, white);
  }
  if (open_shackle) {
    SetPixel(pixels, 13, 5, white);
    SetPixel(pixels, 14, 6, white);
    SetPixel(pixels, 14, 7, white);
  }
}

HICON CreateTrayStatusIcon(const core::TrayIconState& icon) {
  IconColor accent{115, 134, 148, 255};
  if (icon.variant == "review") {
    accent = IconColor{217, 148, 31, 255};
  } else if (icon.variant == "locked") {
    accent = IconColor{47, 157, 90, 255};
  } else if (icon.variant == "mixed") {
    accent = IconColor{47, 143, 255, 255};
  }

  const IconColor screen_fill{18, 25, 36, 255};
  std::vector<DWORD> pixels(static_cast<std::size_t>(kTrayIconPixels) *
                            static_cast<std::size_t>(kTrayIconPixels), 0);

  FillRect(&pixels, 1, 2, 13, 10, accent);
  FillRect(&pixels, 2, 3, 12, 9, screen_fill);
  FillRect(&pixels, 5, 10, 9, 12, accent);
  FillRect(&pixels, 3, 12, 11, 14, accent);

  DrawLockBadge(&pixels, accent,
                icon.variant == "unlocked" || icon.variant == "idle");
  if (icon.review_badge) {
    FillCircle(&pixels, 13, 3, 2, IconColor{214, 59, 72, 255});
  }

  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = kTrayIconPixels;
  header.bV5Height = -kTrayIconPixels;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;

  void* bitmap_bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color_bitmap =
      CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                       DIB_RGB_COLORS, &bitmap_bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (color_bitmap == nullptr || bitmap_bits == nullptr) {
    if (color_bitmap != nullptr) {
      DeleteObject(color_bitmap);
    }
    return nullptr;
  }

  std::memcpy(bitmap_bits, pixels.data(), pixels.size() * sizeof(DWORD));
  HBITMAP mask_bitmap =
      CreateBitmap(kTrayIconPixels, kTrayIconPixels, 1, 1, nullptr);
  if (mask_bitmap == nullptr) {
    DeleteObject(color_bitmap);
    return nullptr;
  }

  ICONINFO icon_info{};
  icon_info.fIcon = TRUE;
  icon_info.hbmColor = color_bitmap;
  icon_info.hbmMask = mask_bitmap;
  HICON icon_handle = CreateIconIndirect(&icon_info);

  DeleteObject(color_bitmap);
  DeleteObject(mask_bitmap);
  return icon_handle;
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

      HBRUSH background = CreateSolidBrush(RGB(18, 25, 36));
      FillRect(dc, &bounds, background);
      DeleteObject(background);

      HPEN border = CreatePen(PS_SOLID, 2, RGB(47, 143, 255));
      HGDIOBJ previous_pen = SelectObject(dc, border);
      HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      RoundRect(dc, bounds.left + 1, bounds.top + 1, bounds.right - 1,
                bounds.bottom - 1, 16, 16);
      SelectObject(dc, previous_brush);
      SelectObject(dc, previous_pen);
      DeleteObject(border);

      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(244, 247, 250));
      HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
      HGDIOBJ previous_font = SelectObject(dc, font);

      RECT title_rect{16, 16, bounds.right - 16, 44};
      RECT message_rect{16, 46, bounds.right - 16, bounds.bottom - 16};
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

  state->identify_overlay_window = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE |
          WS_EX_TRANSPARENT,
      kIdentifyOverlayWindowClassName, L"LockingGlass Identify Overlay", WS_POPUP,
      0, 0, kIdentifyOverlayWidth, kIdentifyOverlayHeight, nullptr, nullptr,
      instance, state);
  if (state->identify_overlay_window == nullptr) {
    return false;
  }

  SetLayeredWindowAttributes(state->identify_overlay_window, 0, 235, LWA_ALPHA);
  return true;
}

void HideIdentifyOverlay(BackgroundSessionState* state) {
  if (state == nullptr) {
    return;
  }
  state->highlighted_monitor_key.clear();
  if (state->identify_overlay_window != nullptr) {
    ShowWindow(state->identify_overlay_window, SW_HIDE);
  }
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
      overlay.monitor.bounds.right - overlay.monitor.bounds.left;
  const int monitor_height =
      overlay.monitor.bounds.bottom - overlay.monitor.bounds.top;
  const int x = overlay.monitor.bounds.left +
                (monitor_width - kIdentifyOverlayWidth) / 2;
  const int y = overlay.monitor.bounds.top +
                (monitor_height - kIdentifyOverlayHeight) / 2;

  SetWindowPos(state->identify_overlay_window, HWND_TOPMOST, x, y,
               kIdentifyOverlayWidth, kIdentifyOverlayHeight,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(state->identify_overlay_window, nullptr, TRUE);
  UpdateWindow(state->identify_overlay_window);
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

core::TrayMenuModel RefreshTrayModel(HWND window, BackgroundSessionState* state,
                                     std::string trigger,
                                     const bool tray_menu_visible,
                                     const bool emit_prompt) {
  state->session =
      state->session_store.Restore(state->monitor_gateway->Enumerate());
  const auto model =
      core::BuildTrayMenuModel(state->session, std::move(trigger));
  const auto prompt = emit_prompt
                          ? core::BuildMonitorReviewPrompt(state->session)
                          : core::MonitorReviewPrompt{};
  if (!tray_menu_visible) {
    HideIdentifyOverlay(state);
  }
  UpdateTrayVisuals(window, state, model);
  PublishEvent(state->observer, model, tray_menu_visible, prompt);
  ShowReviewNotification(window, state, prompt);
  return model;
}

void RepublishCurrentModel(HWND window, BackgroundSessionState* state,
                           std::string trigger,
                           const bool tray_menu_visible,
                           const core::MonitorReviewPrompt& prompt =
                               core::MonitorReviewPrompt{},
                           const core::TrayIdentifyOverlay& highlight =
                               core::TrayIdentifyOverlay{}) {
  auto model = core::BuildTrayMenuModel(state->session, std::move(trigger));
  if (!tray_menu_visible) {
    HideIdentifyOverlay(state);
  }
  UpdateTrayVisuals(window, state, model);
  PublishEvent(state->observer, model, tray_menu_visible, prompt, highlight);
  ShowReviewNotification(window, state, prompt);
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

  const auto model = core::BuildTrayMenuModel(state->session, "startup");
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

  const auto tooltip = Widen(model.icon.tooltip);
  wcsncpy_s(state->tray_icon.szTip, tooltip.c_str(), _TRUNCATE);

  if (!Shell_NotifyIconW(NIM_ADD, &state->tray_icon)) {
    ReleaseTrayIconHandle(state);
    return false;
  }

  state->tray_icon.uVersion = NOTIFYICON_VERSION_4;
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

void ShowTrayMenu(HWND window, BackgroundSessionState* state) {
  if (window == nullptr || state == nullptr) {
    return;
  }

  auto model = RefreshTrayModel(window, state, "tray-click", true, false);
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }

  state->active_menu_model = model;
  state->tray_menu_open = true;
  HideIdentifyOverlay(state);

  std::vector<std::wstring> labels;
  labels.reserve(model.monitors.size() + 8U);
  labels.push_back(Widen(model.header.title));
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, labels.back().c_str());
  labels.push_back(Widen(model.header.subtitle));
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, labels.back().c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  if (model.monitors.empty()) {
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No monitors detected");
  } else {
    for (std::size_t index = 0; index < model.monitors.size(); ++index) {
      labels.push_back(Widen(model.monitors[index].menu_label));
      AppendMenuW(menu, MF_STRING,
                  kMenuCommandMonitorBase + static_cast<UINT>(index),
                  labels.back().c_str());
    }
  }

  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  labels.push_back(Widen(model.header.instruction));
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, labels.back().c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuCommandRefresh, L"Refresh monitor list");
  AppendMenuW(menu, MF_STRING, kMenuCommandExit, L"Exit LockingGlass");

  POINT cursor{};
  GetCursorPos(&cursor);
  SetForegroundWindow(window);
  const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      cursor.x, cursor.y, 0, window, nullptr);
  DestroyMenu(menu);
  PostMessageW(window, WM_NULL, 0, 0);
  state->tray_menu_open = false;
  state->active_menu_model = {};
  HideIdentifyOverlay(state);

  if (command >= kMenuCommandMonitorBase &&
      command < kMenuCommandMonitorBase + model.monitors.size()) {
    const auto monitor = model.monitors[command - kMenuCommandMonitorBase].monitor;
    if (core::ToggleMonitorLock(state->session_store, &state->session.snapshot,
                                monitor)) {
      state->session =
          state->session_store.Preview(state->monitor_gateway->Enumerate());
      RepublishCurrentModel(window, state, "tray-toggle", false);
    }
    return;
  }

  if (command == kMenuCommandRefresh) {
    RefreshTrayModel(window, state, "tray-refresh", false, true);
    return;
  }

  if (command == kMenuCommandExit) {
    DestroyWindow(window);
  }
}

void UpdateHoverOverlay(BackgroundSessionState* state, const UINT command,
                        const UINT flags, const LPARAM menu_handle) {
  if (state == nullptr || !state->tray_menu_open) {
    return;
  }

  if (flags == 0xFFFF && menu_handle == 0) {
    HideIdentifyOverlay(state);
    return;
  }

  if ((flags & (MF_POPUP | MF_SEPARATOR)) != 0 ||
      command < kMenuCommandMonitorBase ||
      command >=
          kMenuCommandMonitorBase + state->active_menu_model.monitors.size()) {
    HideIdentifyOverlay(state);
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
  PublishEvent(state->observer, hover_model, true, core::MonitorReviewPrompt{},
               overlay);
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
    case kTrayIconMessage:
      if (state != nullptr &&
          (l_param == WM_CONTEXTMENU || l_param == WM_RBUTTONUP ||
           l_param == WM_LBUTTONUP || l_param == NIN_SELECT ||
           l_param == NIN_KEYSELECT)) {
        ShowTrayMenu(window, state);
      }
      return 0;
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      if (state != nullptr) {
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

int RunWindowsTraySession(const BackgroundSessionObserver& observer) {
  HINSTANCE instance = GetModuleHandleW(nullptr);

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
  state->taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
  state->session =
      state->session_store.Restore(state->monitor_gateway->Enumerate());

  HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kBackgroundWindowClassName,
                                L"LockingGlass Background", WS_OVERLAPPED, 0, 0,
                                0, 0, nullptr, nullptr, instance, state.get());
  if (window == nullptr) {
    return 1;
  }

  if (!EnsureIdentifyOverlayWindow(instance, state.get())) {
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
#endif

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

enum class TrayScriptStepType {
  kEvent,
  kClick,
  kHover,
  kToggle,
  kRefresh,
  kExit,
};

struct TrayScriptStep {
  TrayScriptStepType type = TrayScriptStepType::kEvent;
  std::string trigger;
  std::vector<MonitorDescriptor> monitors;
  std::string target;
};

std::vector<TrayScriptStep> LoadTrayScript(const std::string& script_path) {
  std::ifstream input(script_path);
  if (!input.is_open()) {
    return {};
  }

  std::vector<TrayScriptStep> steps;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const auto fields = SplitFields(line);
    if (fields.empty()) {
      continue;
    }

    if (fields[0] == "event" && fields.size() == 2U) {
      steps.push_back(TrayScriptStep{
          .type = TrayScriptStepType::kEvent,
          .trigger = fields[1],
          .monitors = {},
          .target = {},
      });
      continue;
    }

    if (fields[0] == "monitor" && fields.size() == 11U && !steps.empty() &&
        steps.back().type == TrayScriptStepType::kEvent) {
      bool is_primary = false;
      MonitorBounds bounds;
      if (!ParseIntField(fields[6], &bounds.left) ||
          !ParseIntField(fields[7], &bounds.top) ||
          !ParseIntField(fields[8], &bounds.right) ||
          !ParseIntField(fields[9], &bounds.bottom) ||
          !ParseBoolField(fields[10], &is_primary)) {
        return {};
      }

      steps.back().monitors.push_back(MonitorDescriptor{
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

    if (fields[0] == "action" && fields.size() >= 2U) {
      if (fields[1] == "click" && fields.size() == 2U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kClick,
            .trigger = {},
            .monitors = {},
            .target = {},
        });
        continue;
      }
      if (fields[1] == "hover" && fields.size() == 3U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kHover,
            .trigger = {},
            .monitors = {},
            .target = fields[2],
        });
        continue;
      }
      if (fields[1] == "toggle" && fields.size() == 3U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kToggle,
            .trigger = {},
            .monitors = {},
            .target = fields[2],
        });
        continue;
      }
      if (fields[1] == "refresh" && fields.size() == 2U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kRefresh,
            .trigger = {},
            .monitors = {},
            .target = {},
        });
        continue;
      }
      if (fields[1] == "exit" && fields.size() == 2U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kExit,
            .trigger = {},
            .monitors = {},
            .target = {},
        });
        continue;
      }
    }

    return {};
  }

  return steps;
}

const platform::MonitorDescriptor* FindScriptMonitor(
    const core::TrayMenuModel& model, const std::string& target) {
  for (const auto& monitor : model.monitors) {
    if (monitor.monitor.label == target || monitor.monitor.stable_id == target ||
        monitor.monitor.device_path == target) {
      return &monitor.monitor;
    }
  }
  return nullptr;
}

int RunScriptedTraySession(const BackgroundSessionObserver& observer) {
  const char* script_path = std::getenv("LOCKING_GLASS_TRAY_SCRIPT");
  if (script_path == nullptr || script_path[0] == '\0') {
    return 0;
  }

  const auto steps = LoadTrayScript(script_path);
  if (steps.empty()) {
    return 1;
  }

  core::SessionStore session_store;
  core::SessionRefreshResult session;
  std::vector<MonitorDescriptor> current_monitors;
  bool has_session = false;
  bool tray_menu_visible = false;

  for (const auto& step : steps) {
    switch (step.type) {
      case TrayScriptStepType::kEvent:
        current_monitors = step.monitors;
        session = session_store.Restore(current_monitors);
        has_session = true;
        tray_menu_visible = false;
        PublishEvent(observer,
                     core::BuildTrayMenuModel(session, step.trigger), false,
                     core::BuildMonitorReviewPrompt(session));
        break;
      case TrayScriptStepType::kClick:
        if (!has_session) {
          session = session_store.Restore(current_monitors);
          has_session = true;
        }
        tray_menu_visible = true;
        PublishEvent(observer,
                     core::BuildTrayMenuModel(session, "tray-click"), true);
        break;
      case TrayScriptStepType::kHover: {
        if (!has_session || !tray_menu_visible) {
          return 1;
        }
        const auto model = core::BuildTrayMenuModel(session, "tray-hover");
        const auto* monitor = FindScriptMonitor(model, step.target);
        if (monitor == nullptr) {
          return 1;
        }

        bool published = false;
        for (const auto& menu_monitor : model.monitors) {
          if (menu_monitor.monitor.label == monitor->label &&
              menu_monitor.monitor.stable_id == monitor->stable_id &&
              menu_monitor.monitor.device_path == monitor->device_path) {
            PublishEvent(observer, model, true, core::MonitorReviewPrompt{},
                         core::BuildTrayIdentifyOverlay(menu_monitor));
            published = true;
            break;
          }
        }
        if (!published) {
          return 1;
        }
        break;
      }
      case TrayScriptStepType::kToggle: {
        if (!has_session) {
          session = session_store.Restore(current_monitors);
          has_session = true;
        }
        const auto model = core::BuildTrayMenuModel(session, "tray-click");
        const auto* monitor = FindScriptMonitor(model, step.target);
        if (monitor == nullptr ||
            !core::ToggleMonitorLock(session_store, &session.snapshot, *monitor)) {
          return 1;
        }
        session = session_store.Preview(current_monitors);
        tray_menu_visible = false;
        PublishEvent(observer,
                     core::BuildTrayMenuModel(session, "tray-toggle"), false);
        break;
      }
      case TrayScriptStepType::kRefresh:
        session = session_store.Restore(current_monitors);
        has_session = true;
        tray_menu_visible = false;
        PublishEvent(observer,
                     core::BuildTrayMenuModel(session, "tray-refresh"), false,
                     core::BuildMonitorReviewPrompt(session));
        break;
      case TrayScriptStepType::kExit:
        tray_menu_visible = false;
        PublishEvent(observer,
                     core::BuildTrayMenuModel(session, "exit"), false);
        return 0;
    }
  }

  return 0;
}

class BackgroundSessionImpl final : public BackgroundSession {
 public:
  locking_glass::integration::CapabilityReport Probe() const override {
#if defined(_WIN32)
    return locking_glass::integration::CapabilityReport{
        .component = "background-session",
        .status = locking_glass::integration::CapabilityStatus::kReady,
        .detail =
            "Background startup enters a hidden Win32 message loop, renders a status-aware Shell_NotifyIcon tray icon, shows hover identify overlays for monitors, and exposes per-monitor padlock toggles through a structured popup menu.",
    };
#else
    return locking_glass::integration::CapabilityReport{
        .component = "background-session",
        .status = locking_glass::integration::CapabilityStatus::kStubbed,
        .detail =
            "Tray session is stubbed on non-Windows hosts; LOCKING_GLASS_TRAY_SCRIPT can still replay tray clicks, hover-identify overlays, monitor toggles, and new-monitor review prompts for local verification.",
    };
#endif
  }

  int Run(const BackgroundSessionObserver& observer) const override {
#if defined(_WIN32)
    return RunWindowsTraySession(observer);
#else
    return RunScriptedTraySession(observer);
#endif
  }
};

}  // namespace

std::unique_ptr<BackgroundSession> CreateBackgroundSession() {
  return std::make_unique<BackgroundSessionImpl>();
}

}  // namespace locking_glass::platform
