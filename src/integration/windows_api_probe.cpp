#include "locking_glass/integration/windows_api_probe.h"

#include <memory>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <objbase.h>
#include <windows.h>
#endif

namespace locking_glass::integration {

namespace {

class WindowsApiProbeImpl final : public WindowsApiProbe {
 public:
  CapabilityReport Probe() const override {
#if defined(_WIN32)
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_ready = SUCCEEDED(com_result) || com_result == RPC_E_CHANGED_MODE;

    HMODULE user32 = LoadLibraryW(L"user32.dll");
    HMODULE shell32 = LoadLibraryW(L"shell32.dll");

    const bool monitor_api_ready =
        user32 != nullptr && GetProcAddress(user32, "EnumDisplayMonitors") != nullptr &&
        GetProcAddress(user32, "GetMonitorInfoW") != nullptr &&
        GetProcAddress(user32, "RegisterWindowMessageW") != nullptr;
    const bool tray_api_ready =
        shell32 != nullptr && GetProcAddress(shell32, "Shell_NotifyIconW") != nullptr;

    if (user32 != nullptr) {
      FreeLibrary(user32);
    }
    if (shell32 != nullptr) {
      FreeLibrary(shell32);
    }
    if (com_result == S_OK || com_result == S_FALSE) {
      CoUninitialize();
    }

    if (com_ready && monitor_api_ready && tray_api_ready) {
      return CapabilityReport{
          .component = "windows-api",
          .status = CapabilityStatus::kReady,
          .detail =
              "Resolved Win32 tray and monitor entry points and confirmed COM startup.",
      };
    }

    return CapabilityReport{
        .component = "windows-api",
        .status = CapabilityStatus::kUnavailable,
        .detail =
            "Win32 API probe failed. COM, user32, or shell32 entry points were unavailable.",
    };
#else
    return CapabilityReport{
        .component = "windows-api",
        .status = CapabilityStatus::kStubbed,
        .detail = "Win32 tray and monitor probes are disabled on non-Windows hosts.",
    };
#endif
  }
};

}  // namespace

std::unique_ptr<WindowsApiProbe> CreateWindowsApiProbe() {
  return std::make_unique<WindowsApiProbeImpl>();
}

}  // namespace locking_glass::integration
