#include "locking_glass/core/runtime.h"

#include <sstream>
#include <utility>

namespace locking_glass::core {

namespace {

std::string FormatMonitorLine(const platform::MonitorDescriptor& monitor) {
  std::ostringstream builder;
  builder << "  - " << monitor.label << " [" << monitor.stable_id << "] "
          << "(" << monitor.bounds.left << "," << monitor.bounds.top << ")-("
          << monitor.bounds.right << "," << monitor.bounds.bottom << ")";
  if (monitor.is_primary) {
    builder << " primary";
  }
  return builder.str();
}

}  // namespace

Runtime BuildRuntime() {
  return Runtime{
      .monitor_gateway = platform::CreateMonitorGateway(),
      .background_session = platform::CreateBackgroundSession(),
      .ffmpeg_probe = integration::CreateFfmpegProbe(),
      .windows_api_probe = integration::CreateWindowsApiProbe(),
      .autostart_manager = integration::CreateAutostartManager(),
  };
}

StartupDiagnostics CollectStartupDiagnostics(const Runtime& runtime,
                                            const std::string& executable_path) {
  StartupDiagnostics diagnostics;
  diagnostics.monitors = runtime.monitor_gateway->Enumerate();
  diagnostics.capabilities.push_back(runtime.background_session->Probe());
  diagnostics.capabilities.push_back(runtime.windows_api_probe->Probe());
  diagnostics.capabilities.push_back(runtime.autostart_manager->Probe());
  diagnostics.capabilities.push_back(runtime.ffmpeg_probe->Probe());
  diagnostics.autostart =
      runtime.autostart_manager->BuildPlan(executable_path);
  return diagnostics;
}

std::string FormatDiagnostics(const StartupDiagnostics& diagnostics) {
  std::ostringstream builder;
  builder << "LockingGlass startup diagnostics\n";
  builder << "Capabilities:\n";
  for (const auto& capability : diagnostics.capabilities) {
    builder << "  - " << capability.component << ": "
            << integration::ToString(capability.status) << " (" << capability.detail
            << ")\n";
  }

  builder << "Autostart:\n";
  builder << "  - scope: " << diagnostics.autostart.scope << '\n';
  builder << "  - location: " << diagnostics.autostart.location << '\n';
  builder << "  - entry: " << diagnostics.autostart.entry_name << '\n';
  builder << "  - launch mode: " << diagnostics.autostart.launch_mode << '\n';
  builder << "  - command: " << diagnostics.autostart.launch_command << '\n';

  builder << "Monitors:\n";
  if (diagnostics.monitors.empty()) {
    builder << "  - none detected in the active host build\n";
  } else {
    for (const auto& monitor : diagnostics.monitors) {
      builder << FormatMonitorLine(monitor) << '\n';
    }
  }

  return builder.str();
}

}  // namespace locking_glass::core
