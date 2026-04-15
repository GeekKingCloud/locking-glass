#include "locking_glass/platform/background_session.h"

#include <cstdlib>
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

BackgroundSessionEvent BuildSessionEvent(const core::TrayMenuModel& model,
                                         const bool tray_menu_visible,
                                         const core::MonitorReviewPrompt& prompt =
                                             core::MonitorReviewPrompt{}) {
  BackgroundSessionEvent event{
      .trigger = model.trigger,
      .tray_menu_visible = tray_menu_visible,
      .monitors = {},
      .prompt = BuildBackgroundPrompt(prompt),
  };
  for (const auto& monitor : model.monitors) {
    event.monitors.push_back(BackgroundSessionMenuItem{
        .monitor = monitor.monitor,
        .locked = monitor.locked,
        .requires_confirmation = monitor.requires_confirmation,
    });
  }
  return event;
}

void PublishEvent(const BackgroundSessionObserver& observer,
                  const core::TrayMenuModel& model,
                  const bool tray_menu_visible,
                  const core::MonitorReviewPrompt& prompt =
                      core::MonitorReviewPrompt{}) {
  if (observer) {
    observer(BuildSessionEvent(model, tray_menu_visible, prompt));
  }
}

#if defined(_WIN32)
constexpr UINT kTrayIconMessage = WM_APP + 1;
constexpr UINT kMenuCommandMonitorBase = 1000;
constexpr UINT kMenuCommandRefresh = 9000;
constexpr UINT kMenuCommandExit = 9001;

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
  core::SessionStore session_store;
  std::unique_ptr<MonitorGateway> monitor_gateway = CreateMonitorGateway();
  core::SessionRefreshResult session;
  BackgroundSessionObserver observer;
  NOTIFYICONDATAW tray_icon{};
  UINT taskbar_created_message = 0;
};

std::wstring BuildTrayTooltip(const core::TrayMenuModel& model) {
  std::wstring tooltip = L"LockingGlass";
  tooltip += L" - ";
  tooltip += std::to_wstring(model.locked_monitors);
  tooltip += L" locked";
  if (model.review_monitors > 0U) {
    tooltip += L", ";
    tooltip += std::to_wstring(model.review_monitors);
    tooltip += L" review";
  }
  return tooltip;
}

void UpdateTrayTooltip(HWND window, BackgroundSessionState* state,
                       const core::TrayMenuModel& model) {
  if (window == nullptr || state == nullptr) {
    return;
  }

  state->tray_icon.cbSize = sizeof(state->tray_icon);
  state->tray_icon.hWnd = window;
  state->tray_icon.uID = 1;
  state->tray_icon.uFlags = NIF_TIP;
  const auto tooltip = BuildTrayTooltip(model);
  wcsncpy_s(state->tray_icon.szTip, tooltip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &state->tray_icon);
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
  UpdateTrayTooltip(window, state, model);
  PublishEvent(state->observer, model, tray_menu_visible, prompt);
  ShowReviewNotification(window, state, prompt);
  return model;
}

void RepublishCurrentModel(HWND window, BackgroundSessionState* state,
                           std::string trigger,
                           const bool tray_menu_visible,
                           const core::MonitorReviewPrompt& prompt =
                               core::MonitorReviewPrompt{}) {
  auto model = core::BuildTrayMenuModel(state->session, std::move(trigger));
  UpdateTrayTooltip(window, state, model);
  PublishEvent(state->observer, model, tray_menu_visible, prompt);
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
  state->tray_icon.hIcon = static_cast<HICON>(
      LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON,
                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                 LR_SHARED));
  if (state->tray_icon.hIcon == nullptr) {
    return false;
  }

  const auto model = core::BuildTrayMenuModel(state->session, "startup");
  const auto tooltip = BuildTrayTooltip(model);
  wcsncpy_s(state->tray_icon.szTip, tooltip.c_str(), _TRUNCATE);

  if (!Shell_NotifyIconW(NIM_ADD, &state->tray_icon)) {
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

  if (state->tray_icon.hWnd != nullptr) {
    state->tray_icon.uFlags = 0;
    Shell_NotifyIconW(NIM_DELETE, &state->tray_icon);
  }
}

void ShowTrayMenu(HWND window, BackgroundSessionState* state) {
  if (window == nullptr || state == nullptr) {
    return;
  }

  const auto model = RefreshTrayModel(window, state, "tray-click", true, false);
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }

  std::vector<std::wstring> labels;
  labels.reserve(model.monitors.size() + 2U);
  if (model.monitors.empty()) {
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No monitors detected");
  } else {
    for (std::size_t index = 0; index < model.monitors.size(); ++index) {
      labels.push_back(Widen(core::BuildTrayMonitorLabel(model.monitors[index])));
      UINT flags = MF_STRING;
      if (model.monitors[index].locked) {
        flags |= MF_CHECKED;
      }
      AppendMenuW(menu, flags, kMenuCommandMonitorBase + static_cast<UINT>(index),
                  labels.back().c_str());
    }
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuCommandRefresh, L"Refresh monitors");
  AppendMenuW(menu, MF_STRING, kMenuCommandExit, L"Exit");

  POINT cursor{};
  GetCursorPos(&cursor);
  SetForegroundWindow(window);
  const UINT command = TrackPopupMenu(
      menu, TPM_NONOTIFY | TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x,
      cursor.y, 0, window, nullptr);
  DestroyMenu(menu);
  PostMessageW(window, WM_NULL, 0, 0);

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
      }
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, w_param, l_param);
  }
}

int RunWindowsTraySession(const BackgroundSessionObserver& observer) {
  HINSTANCE instance = GetModuleHandleW(nullptr);
  const wchar_t class_name[] = L"LockingGlassBackgroundWindow";

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = BackgroundWindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = class_name;

  const ATOM class_atom = RegisterClassW(&window_class);
  if (class_atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return 1;
  }

  auto state = std::make_unique<BackgroundSessionState>();
  state->observer = observer;
  state->taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
  state->session =
      state->session_store.Restore(state->monitor_gateway->Enumerate());

  HWND window =
      CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"LockingGlass Background",
                      WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance,
                      state.get());
  if (window == nullptr) {
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

enum class TrayScriptStepType { kEvent, kClick, kToggle, kRefresh, kExit };

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

  for (const auto& step : steps) {
    switch (step.type) {
      case TrayScriptStepType::kEvent:
        current_monitors = step.monitors;
        session = session_store.Restore(current_monitors);
        has_session = true;
        PublishEvent(observer,
                     core::BuildTrayMenuModel(session, step.trigger), false,
                     core::BuildMonitorReviewPrompt(session));
        break;
      case TrayScriptStepType::kClick:
        if (!has_session) {
          session = session_store.Restore(current_monitors);
          has_session = true;
        }
        PublishEvent(observer,
                     core::BuildTrayMenuModel(session, "tray-click"), true);
        break;
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
        PublishEvent(observer,
                     core::BuildTrayMenuModel(session, "tray-toggle"), false);
        break;
      }
      case TrayScriptStepType::kRefresh:
        session = session_store.Restore(current_monitors);
        has_session = true;
        PublishEvent(observer,
                     core::BuildTrayMenuModel(session, "tray-refresh"), false,
                     core::BuildMonitorReviewPrompt(session));
        break;
      case TrayScriptStepType::kExit:
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
            "Background startup enters a hidden Win32 message loop, registers a Shell_NotifyIcon tray entry, shows review prompts for newly added monitors, and exposes monitor lock toggles through a popup menu.",
    };
#else
    return locking_glass::integration::CapabilityReport{
        .component = "background-session",
        .status = locking_glass::integration::CapabilityStatus::kStubbed,
        .detail =
            "Tray session is stubbed on non-Windows hosts; LOCKING_GLASS_TRAY_SCRIPT can still replay tray clicks, monitor toggles, and new-monitor review prompts for local verification.",
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
