#include "locking_glass/integration/windows_api_probe.h"

#include "windows_virtual_desktop_surface.h"

#include <memory>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#if defined(__has_include)
#if __has_include(<shobjidl_core.h>)
#include <shobjidl_core.h>
#else
#include <shobjidl.h>
#endif
#else
#include <shobjidl_core.h>
#endif
#include <windows.h>
#endif

namespace locking_glass::integration {

namespace {

struct WindowsSurfaceProbe {
  bool com_ready = false;
  bool tray_api_ready = false;
  bool monitor_api_ready = false;
  bool virtual_desktop_manager_ready = false;
  bool helper_library_ready = false;
  bool helper_watch_ready = false;
  bool helper_move_ready = false;
  bool helper_lifecycle_ready = false;
};

#if defined(_WIN32)
WindowsSurfaceProbe ProbeWindowsSurface() {
  WindowsSurfaceProbe probe;
  const auto desktop_probe = internal::ProbeWindowsVirtualDesktopSurface();
  probe.com_ready = desktop_probe.com_ready;
  probe.virtual_desktop_manager_ready = desktop_probe.desktop_manager_ready;
  probe.helper_library_ready = desktop_probe.helper_library_ready;
  probe.helper_watch_ready = desktop_probe.helper_watch_ready;
  probe.helper_move_ready = desktop_probe.helper_move_ready;
  probe.helper_lifecycle_ready = desktop_probe.helper_lifecycle_ready;

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
  if (user32 != nullptr) {
    FreeLibrary(user32);
  }
  if (shell32 != nullptr) {
    FreeLibrary(shell32);
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
  if (probe.com_ready && probe.virtual_desktop_manager_ready &&
      probe.helper_watch_ready && probe.helper_move_ready &&
      probe.helper_lifecycle_ready) {
    return CapabilityReport{
        .component = "virtual-desktop-control",
        .status = CapabilityStatus::kReady,
        .detail =
            "COM, IVirtualDesktopManager, and VirtualDesktopAccessor.dll are available; live desktop notifications, window moves, and the Locking Glass staging desktop are isolated behind the Windows boundary.",
    };
  }

  return CapabilityReport{
      .component = "virtual-desktop-control",
      .status = CapabilityStatus::kUnavailable,
      .detail =
          "Virtual desktop control fails closed until Locking Glass can resolve both IVirtualDesktopManager and VirtualDesktopAccessor.dll with RegisterPostMessageHook, UnregisterPostMessageHook, GetCurrentDesktopNumber, GoToDesktopNumber, GetDesktopCount, GetDesktopName, GetDesktopIdByNumber, MoveWindowToDesktopNumber, GetWindowDesktopNumber, CreateDesktop, SetDesktopName, and RemoveDesktop.",
  };
#else
  return CapabilityReport{
      .component = "virtual-desktop-control",
      .status = CapabilityStatus::kStubbed,
      .detail =
          "Win32 virtual desktop control is stubbed on non-Windows hosts; the boundary contract still documents COM plus the VirtualDesktopAccessor live hook, while replay stays explicitly separate from core feature proof.",
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
        probe.virtual_desktop_manager_ready && probe.helper_watch_ready &&
        probe.helper_move_ready && probe.helper_lifecycle_ready) {
      return CapabilityReport{
          .component = "windows-api",
          .status = CapabilityStatus::kReady,
          .detail =
              "Resolved tray, monitor, IVirtualDesktopManager, and VirtualDesktopAccessor entry points; live desktop notifications, move calls, and Locking Glass staging desktop lifecycle stay isolated behind the Windows boundary.",
      };
    }

    return CapabilityReport{
        .component = "windows-api",
        .status = CapabilityStatus::kUnavailable,
        .detail =
            "Windows API probe failed. Tray, monitor, IVirtualDesktopManager, or VirtualDesktopAccessor entry points were unavailable, so Locking Glass must fail closed on live desktop locking.",
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
            "Own COM readiness, the VirtualDesktopAccessor live hook, staging desktop lifecycle, and the isolated boundary that translates desktop switch notifications into explicit move calls.",
        .capability = ProbeVirtualDesktopBoundary(),
        .windows_apis =
            {
                "CoInitializeEx / CoUninitialize",
                "CoCreateInstance(CLSID_VirtualDesktopManager)",
                "IVirtualDesktopManager",
                "VirtualDesktopAccessor.dll:RegisterPostMessageHook",
                "VirtualDesktopAccessor.dll:UnregisterPostMessageHook",
                "VirtualDesktopAccessor.dll:GetCurrentDesktopNumber",
                "VirtualDesktopAccessor.dll:GoToDesktopNumber",
                "VirtualDesktopAccessor.dll:GetDesktopCount",
                "VirtualDesktopAccessor.dll:GetDesktopName",
                "VirtualDesktopAccessor.dll:GetDesktopIdByNumber",
                "VirtualDesktopAccessor.dll:MoveWindowToDesktopNumber",
                "VirtualDesktopAccessor.dll:GetWindowDesktopNumber",
                "VirtualDesktopAccessor.dll:CreateDesktop",
                "VirtualDesktopAccessor.dll:SetDesktopName",
                "VirtualDesktopAccessor.dll:RemoveDesktop",
            },
        .in_scope =
            {
                "Resolve the supported IVirtualDesktopManager and VirtualDesktopAccessor entry points before any monitor lock orchestration runs.",
                "Receive real desktop switch notifications through the helper post-message hook and translate them into app-level events with source and target desktop context.",
                "Create the named Locking Glass staging desktop, or reuse the same-run identity Locking Glass created itself, before moving target-desktop occupants out of a locked monitor's way.",
                "Move already-selected top-level windows between desktops through the isolated move call when the core policy decides they should follow a locked monitor.",
            },
        .out_of_scope =
            {
                "Choosing which monitors or windows should move on a desktop switch.",
                "Persisting monitor lock state, matching saved monitors, or mutating tray UI state.",
                "Treating the replay seam as evidence that the live Windows path is complete.",
            },
        .expected_behavior =
            {
                "Fail closed when IVirtualDesktopManager or the VirtualDesktopAccessor hook/move exports are unavailable instead of guessing at desktop state.",
                "Fail closed when the staging desktop cannot be resolved; do not fall back to pushing target-desktop windows onto another user desktop.",
                "Accept only top-level window handles from the core policy layer and report per-window move failures explicitly.",
                "Keep the helper-based live hook boundary isolated so replay remains an optional test seam rather than the product path.",
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
                "Re-run enumeration after display topology changes so the core can reconcile saved monitor session state.",
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
        "Desktop switch: receive the real switch event through VirtualDesktopAccessor RegisterPostMessageHook, pair it with current desktop identity, and hand the event to the core policy layer.");
    prototype.interaction_steps.push_back(
        "Window move: after the core selects eligible top-level windows on locked monitors, the virtual desktop boundary creates the named Locking Glass staging desktop or reuses its same-run identity, issues MoveWindowToDesktopNumber, and reports any failed HWND operations without touching persistence or tray state.");
    return prototype;
  }
};

std::string FormatWindowsApiPrototype(const WindowsApiPrototype& prototype) {
  std::ostringstream builder;
  builder << "Locking Glass Windows API integration prototype\n";
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
