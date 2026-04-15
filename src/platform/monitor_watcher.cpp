#include "locking_glass/platform/monitor_watcher.h"

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace locking_glass::platform {

namespace {

#if !defined(_WIN32)
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

std::vector<MonitorWatchEvent> LoadScriptedEvents(
    const std::string& script_path) {
  std::ifstream input(script_path);
  if (!input.is_open()) {
    return {};
  }

  std::vector<MonitorWatchEvent> events;
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
      events.push_back(MonitorWatchEvent{
          .trigger = fields[1],
          .monitors = {},
      });
      continue;
    }

    if (fields[0] != "monitor" || fields.size() != 11U || events.empty()) {
      return {};
    }

    bool is_primary = false;
    MonitorBounds bounds;
    if (!ParseIntField(fields[6], &bounds.left) ||
        !ParseIntField(fields[7], &bounds.top) ||
        !ParseIntField(fields[8], &bounds.right) ||
        !ParseIntField(fields[9], &bounds.bottom) ||
        !ParseBoolField(fields[10], &is_primary)) {
      return {};
    }

    events.back().monitors.push_back(MonitorDescriptor{
        .stable_id = fields[1],
        .device_path = fields[2],
        .edid_serial = fields[3],
        .display_name = fields[4],
        .label = fields[5],
        .bounds = bounds,
        .is_primary = is_primary,
    });
  }

  return events;
}
#endif

#if defined(_WIN32)
struct MonitorWatchContext {
  const MonitorWatchCallback* callback = nullptr;
};

bool DispatchMonitorUpdate(const MonitorWatchContext& context,
                           const std::string& trigger) {
  auto gateway = CreateMonitorGateway();
  return (*context.callback)(MonitorWatchEvent{
      .trigger = trigger,
      .monitors = gateway->Enumerate(),
  });
}

LRESULT CALLBACK MonitorWatchWindowProc(HWND window, UINT message,
                                        WPARAM w_param, LPARAM l_param) {
  if (message == WM_NCCREATE) {
    const auto* create_struct =
        reinterpret_cast<const CREATESTRUCTW*>(l_param);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create_struct->lpCreateParams));
    return TRUE;
  }

  const auto* context = reinterpret_cast<const MonitorWatchContext*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_DISPLAYCHANGE:
      if (context != nullptr &&
          !DispatchMonitorUpdate(*context, "WM_DISPLAYCHANGE")) {
        DestroyWindow(window);
      }
      return 0;
    case WM_CLOSE:
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, w_param, l_param);
  }
}

int RunWindowsMonitorWatch(const MonitorWatchCallback& callback) {
  HINSTANCE instance = GetModuleHandleW(nullptr);
  const wchar_t class_name[] = L"LockingGlassMonitorWatchWindow";

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = MonitorWatchWindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = class_name;

  const ATOM class_atom = RegisterClassW(&window_class);
  if (class_atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return 1;
  }

  MonitorWatchContext context{
      .callback = &callback,
  };

  HWND window =
      CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"LockingGlass Monitor Watch",
                      WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance,
                      &context);
  if (window == nullptr) {
    return 1;
  }

  if (!DispatchMonitorUpdate(context, "startup")) {
    DestroyWindow(window);
    return 0;
  }

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  return static_cast<int>(message.wParam);
}
#endif

class MonitorWatcherImpl final : public MonitorWatcher {
 public:
  int Watch(const MonitorWatchCallback& callback) const override {
#if defined(_WIN32)
    return RunWindowsMonitorWatch(callback);
#else
    if (const char* script_path = std::getenv("LOCKING_GLASS_MONITOR_SCRIPT");
        script_path != nullptr && script_path[0] != '\0') {
      const auto events = LoadScriptedEvents(script_path);
      if (events.empty()) {
        return 1;
      }

      for (const auto& event : events) {
        if (!callback(event)) {
          break;
        }
      }
      return 0;
    }

    auto gateway = CreateMonitorGateway();
    callback(MonitorWatchEvent{
        .trigger = "startup",
        .monitors = gateway->Enumerate(),
    });
    return 0;
#endif
  }
};

}  // namespace

std::unique_ptr<MonitorWatcher> CreateMonitorWatcher() {
  return std::make_unique<MonitorWatcherImpl>();
}

}  // namespace locking_glass::platform
