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
      .ffmpeg_probe = integration::CreateFfmpegProbe(),
      .windows_api_probe = integration::CreateWindowsApiProbe(),
  };
}

StartupDiagnostics CollectStartupDiagnostics(const Runtime& runtime) {
  StartupDiagnostics diagnostics;
  diagnostics.monitors = runtime.monitor_gateway->Enumerate();
  diagnostics.capabilities.push_back(runtime.windows_api_probe->Probe());
  diagnostics.capabilities.push_back(runtime.ffmpeg_probe->Probe());
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
