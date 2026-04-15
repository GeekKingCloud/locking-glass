#include "locking_glass/platform/background_session.h"

#include <memory>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace locking_glass::platform {

namespace {

#if defined(_WIN32)
LRESULT CALLBACK BackgroundWindowProc(HWND window, UINT message, WPARAM w_param,
                                      LPARAM l_param) {
  switch (message) {
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

int RunHiddenMessageLoop() {
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

  HWND window =
      CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"LockingGlass Background",
                      WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) {
    return 1;
  }

  const HWND console = GetConsoleWindow();
  if (console != nullptr) {
    ShowWindow(console, SW_HIDE);
  }

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  return static_cast<int>(message.wParam);
}
#endif

class BackgroundSessionImpl final : public BackgroundSession {
 public:
  locking_glass::integration::CapabilityReport Probe() const override {
#if defined(_WIN32)
    return locking_glass::integration::CapabilityReport{
        .component = "background-session",
        .status = locking_glass::integration::CapabilityStatus::kReady,
        .detail =
            "Background startup enters a hidden Win32 message loop so the tray process can stay resident after logon.",
    };
#else
    return locking_glass::integration::CapabilityReport{
        .component = "background-session",
        .status = locking_glass::integration::CapabilityStatus::kStubbed,
        .detail = "Background launch mode is disabled on non-Windows hosts.",
    };
#endif
  }

  int Run() const override {
#if defined(_WIN32)
    return RunHiddenMessageLoop();
#else
    return 0;
#endif
  }
};

}  // namespace

std::unique_ptr<BackgroundSession> CreateBackgroundSession() {
  return std::make_unique<BackgroundSessionImpl>();
}

}  // namespace locking_glass::platform
