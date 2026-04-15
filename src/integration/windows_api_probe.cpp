#include "locking_glass/integration/windows_api_probe.h"

#include <memory>
#include <sstream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <objbase.h>
#include <shobjidl_core.h>
#include <windows.h>
#endif

namespace locking_glass::integration {

namespace {

struct WindowsSurfaceProbe {
  bool com_ready = false;
  bool tray_api_ready = false;
  bool monitor_api_ready = false;
  bool virtual_desktop_manager_ready = false;
};

#if defined(_WIN32)
WindowsSurfaceProbe ProbeWindowsSurface() {
  WindowsSurfaceProbe probe;

  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  probe.com_ready = SUCCEEDED(com_result) || com_result == RPC_E_CHANGED_MODE;

  HMODULE user32 = LoadLibraryW(L"user32.dll");
  HMODULE shell32 = LoadLibraryW(L"shell32.dll");

  probe.monitor_api_ready =
      user32 != nullptr && GetProcAddress(user32, "EnumDisplayMonitors") != nullptr &&
      GetProcAddress(user32, "GetMonitorInfoW") != nullptr &&
      GetProcAddress(user32, "QueryDisplayConfig") != nullptr &&
      GetProcAddress(user32, "DisplayConfigGetDeviceInfo") != nullptr &&
      GetProcAddress(user32, "RegisterWindowMessageW") != nullptr;
  probe.tray_api_ready =
      shell32 != nullptr && GetProcAddress(shell32, "Shell_NotifyIconW") != nullptr;

  IVirtualDesktopManager* desktop_manager = nullptr;
  if (probe.com_ready) {
    const HRESULT desktop_result =
        CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&desktop_manager));
    probe.virtual_desktop_manager_ready =
        SUCCEEDED(desktop_result) && desktop_manager != nullptr;
  }

  if (desktop_manager != nullptr) {
    desktop_manager->Release();
  }
  if (user32 != nullptr) {
    FreeLibrary(user32);
  }
  if (shell32 != nullptr) {
    FreeLibrary(shell32);
  }
  if (com_result == S_OK || com_result == S_FALSE) {
    CoUninitialize();
  }

  return probe;
}
#endif

CapabilityReport ProbeMonitorBoundary() {
#if defined(_WIN32)
  const auto probe = ProbeWindowsSurface();
  if (probe.monitor_api_ready) {
    return CapabilityReport{
        .component = "monitor-enumeration",
        .status = CapabilityStatus::kReady,
        .detail =
            "Enumerates active monitors through EnumDisplayMonitors plus QueryDisplayConfig identity data and refreshes topology on WM_DISPLAYCHANGE.",
    };
  }

  return CapabilityReport{
      .component = "monitor-enumeration",
      .status = CapabilityStatus::kUnavailable,
      .detail =
          "Monitor enumeration boundary is unavailable because the required user32 entry points could not be resolved.",
  };
#else
  return CapabilityReport{
      .component = "monitor-enumeration",
      .status = CapabilityStatus::kStubbed,
      .detail =
          "Win32 monitor enumeration is stubbed on non-Windows hosts; the boundary contract still documents EnumDisplayMonitors, QueryDisplayConfig, DisplayConfigGetDeviceInfo, and WM_DISPLAYCHANGE usage.",
  };
#endif
}

CapabilityReport ProbeVirtualDesktopBoundary() {
#if defined(_WIN32)
  const auto probe = ProbeWindowsSurface();
  if (probe.com_ready && probe.virtual_desktop_manager_ready) {
    return CapabilityReport{
        .component = "virtual-desktop-control",
        .status = CapabilityStatus::kReady,
        .detail =
            "COM and IVirtualDesktopManager are available; helper-only desktop notifications and forced moves remain isolated behind this boundary.",
    };
  }

  return CapabilityReport{
      .component = "virtual-desktop-control",
      .status = CapabilityStatus::kUnavailable,
      .detail =
          "Virtual desktop control boundary is unavailable because COM startup or IVirtualDesktopManager resolution failed.",
  };
#else
  return CapabilityReport{
      .component = "virtual-desktop-control",
      .status = CapabilityStatus::kStubbed,
      .detail =
          "Win32 virtual desktop control is stubbed on non-Windows hosts; the boundary contract still documents COM, IVirtualDesktopManager, and helper-based notification flow.",
  };
#endif
}

std::string FormatStringList(const std::vector<std::string>& values,
                             const std::string& indent) {
  std::ostringstream builder;
  for (const auto& value : values) {
    builder << indent << "- " << value << '\n';
  }
  return builder.str();
}

}  // namespace

class WindowsApiProbeImpl final : public WindowsApiProbe {
 public:
  CapabilityReport Probe() const override {
#if defined(_WIN32)
    const auto probe = ProbeWindowsSurface();
    if (probe.com_ready && probe.monitor_api_ready && probe.tray_api_ready &&
        probe.virtual_desktop_manager_ready) {
      return CapabilityReport{
          .component = "windows-api",
          .status = CapabilityStatus::kReady,
          .detail =
              "Resolved tray, monitor, and base virtual-desktop entry points; helper-based desktop notifications and window moves stay isolated behind the Windows boundary.",
      };
    }

    return CapabilityReport{
        .component = "windows-api",
        .status = CapabilityStatus::kUnavailable,
        .detail =
            "Windows API probe failed. Tray, monitor, or virtual-desktop entry points were unavailable.",
    };
#else
    return CapabilityReport{
        .component = "windows-api",
        .status = CapabilityStatus::kStubbed,
        .detail =
            "Win32 tray, monitor, and virtual desktop probes are disabled on non-Windows hosts.",
    };
#endif
  }

  WindowsApiPrototype BuildPrototype(
      const std::vector<platform::MonitorDescriptor>& monitors) const override {
    WindowsApiPrototype prototype;
    prototype.boundaries.push_back(WindowsApiBoundary{
        .name = "virtual-desktop-control",
        .purpose =
            "Own COM startup, supported IVirtualDesktopManager calls, and the isolated helper seam used for desktop switch notifications and window moves.",
        .capability = ProbeVirtualDesktopBoundary(),
        .windows_apis =
            {
                "CoInitializeEx / CoUninitialize",
                "CoCreateInstance(CLSID_VirtualDesktopManager)",
                "IVirtualDesktopManager",
                "VirtualDesktopAccessor or equivalent helper",
            },
        .in_scope =
            {
                "Resolve the supported virtual desktop COM entry point before any monitor lock orchestration runs.",
                "Translate desktop switch notifications into app-level events with source and target desktop context.",
                "Move already-selected top-level windows between desktops when the core policy decides they should follow an unlocked monitor.",
            },
        .out_of_scope =
            {
                "Choosing which monitors or windows should move on a desktop switch.",
                "Persisting monitor lock state, matching saved monitors, or mutating tray UI state.",
                "Reaching into shell behavior outside the explicitly isolated helper seam.",
            },
        .expected_behavior =
            {
                "Fail closed when COM or IVirtualDesktopManager is unavailable instead of guessing at desktop state.",
                "Accept only top-level window handles from the core policy layer and report per-window move failures explicitly.",
                "Keep helper-based desktop notifications and forced moves replaceable behind this one boundary.",
            },
    });
    prototype.boundaries.push_back(WindowsApiBoundary{
        .name = "monitor-enumeration",
        .purpose =
            "Own live monitor discovery, identity capture, and topology refresh without deciding lock policy.",
        .capability = ProbeMonitorBoundary(),
        .windows_apis =
            {
                "EnumDisplayMonitors",
                "GetMonitorInfoW",
                "QueryDisplayConfig",
                "DisplayConfigGetDeviceInfo",
                "WM_DISPLAYCHANGE",
            },
        .in_scope =
            {
                "Enumerate active real monitors exposed by Windows and emit their bounds and primary flag.",
                "Fill the monitor identity envelope used by SessionStore: stable id, device path, EDID serial when available, display name, and bounds.",
                "Re-run enumeration after display topology changes so the core can reconcile saved lock state.",
            },
        .out_of_scope =
            {
                "Choosing default lock state for new or ambiguous monitors.",
                "Persisting monitor identity history or resolving user confirmation flows.",
                "Special-case support for mirrored, pseudo, or otherwise non-addressable displays in the MVP.",
            },
        .expected_behavior =
            {
                "Treat composite identity as data for the core layer, not as a policy decision inside the Win32 adapter.",
                "Return current live monitors only; disconnected displays stay in SessionStore rather than this boundary.",
                "Handle mixed DPI, orientation, and negative coordinates by passing through the OS-reported geometry unchanged.",
            },
    });

    prototype.interaction_steps.push_back(
        "Startup: probe tray, monitor, and virtual desktop entry points before enabling any monitor lock orchestration.");
    prototype.interaction_steps.push_back(
        "Monitor scan: enumerate monitors with EnumDisplayMonitors plus QueryDisplayConfig/DisplayConfigGetDeviceInfo and emit MonitorDescriptor values for SessionStore reconciliation.");
    prototype.interaction_steps.push_back(
        "Observed " + std::to_string(monitors.size()) +
        " active monitors in this host build; on Windows the same boundary drives numbering, identification, and review prompts.");
    prototype.interaction_steps.push_back(
        "Topology change: when the background window receives WM_DISPLAYCHANGE, rerun monitor enumeration and let the core decide whether any saved monitor state now needs user review.");
    prototype.interaction_steps.push_back(
        "Desktop switch: receive the supported desktop identity through IVirtualDesktopManager and helper-based notifications, then hand the event to the core policy layer.");
    prototype.interaction_steps.push_back(
        "Window move: after the core selects eligible top-level windows on unlocked monitors, the virtual desktop boundary issues the move and reports any failed HWND operations without touching persistence or tray state.");
    return prototype;
  }
};

std::string FormatWindowsApiPrototype(const WindowsApiPrototype& prototype) {
  std::ostringstream builder;
  builder << "LockingGlass Windows API integration prototype\n";
  builder << "Boundaries:\n";

  for (const auto& boundary : prototype.boundaries) {
    builder << "  - " << boundary.name << ": "
            << ToString(boundary.capability.status) << " ("
            << boundary.capability.detail << ")\n";
    builder << "    purpose: " << boundary.purpose << '\n';
    builder << "    windows apis:\n"
            << FormatStringList(boundary.windows_apis, "      ");
    builder << "    in scope:\n" << FormatStringList(boundary.in_scope, "      ");
    builder << "    out of scope:\n"
            << FormatStringList(boundary.out_of_scope, "      ");
    builder << "    expected behavior:\n"
            << FormatStringList(boundary.expected_behavior, "      ");
  }

  builder << "Interaction flow:\n";
  for (std::size_t index = 0; index < prototype.interaction_steps.size(); ++index) {
    builder << "  " << (index + 1) << ". " << prototype.interaction_steps[index] << '\n';
  }
  return builder.str();
}

std::unique_ptr<WindowsApiProbe> CreateWindowsApiProbe() {
  return std::make_unique<WindowsApiProbeImpl>();
}

}  // namespace locking_glass::integration
