#include "locking_glass/core/runtime.h"

#include <sstream>
#include <utility>

namespace locking_glass::core {

namespace {

const SessionMonitorState* FindSessionState(
    const SessionRefreshResult& session,
    const platform::MonitorDescriptor& monitor) {
  for (const auto& monitor_state : session.snapshot.monitors) {
    if (!monitor_state.is_present) {
      continue;
    }
    if (monitor_state.monitor.stable_id == monitor.stable_id &&
        monitor_state.monitor.device_path == monitor.device_path &&
        monitor_state.monitor.edid_serial == monitor.edid_serial &&
        monitor_state.monitor.display_name == monitor.display_name &&
        monitor_state.monitor.bounds.left == monitor.bounds.left &&
        monitor_state.monitor.bounds.top == monitor.bounds.top &&
        monitor_state.monitor.bounds.right == monitor.bounds.right &&
        monitor_state.monitor.bounds.bottom == monitor.bounds.bottom &&
        monitor_state.monitor.is_primary == monitor.is_primary) {
      return &monitor_state;
    }
  }
  return nullptr;
}

std::string FormatMonitorLine(const platform::MonitorDescriptor& monitor,
                              const SessionRefreshResult& session) {
  std::ostringstream builder;
  builder << "  - " << monitor.label << " [" << monitor.stable_id << "] "
          << "(" << monitor.bounds.left << "," << monitor.bounds.top << ")-("
          << monitor.bounds.right << "," << monitor.bounds.bottom << ")";
  if (monitor.is_primary) {
    builder << " primary";
  }
  if (const auto* monitor_state = FindSessionState(session, monitor);
      monitor_state != nullptr) {
    builder << (monitor_state->locked ? " locked" : " unlocked");
    if (monitor_state->requires_confirmation) {
      builder << " review";
    }
  }
  return builder.str();
}

}  // namespace

Runtime BuildRuntime() {
  return Runtime{
      .session_store = SessionStore{},
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
  diagnostics.session = runtime.session_store.Preview(diagnostics.monitors);
  diagnostics.capabilities.push_back(runtime.background_session->Probe());
  diagnostics.capabilities.push_back(runtime.windows_api_probe->Probe());
  diagnostics.capabilities.push_back(runtime.autostart_manager->Probe());
  diagnostics.capabilities.push_back(runtime.ffmpeg_probe->Probe());
  diagnostics.capabilities.push_back(runtime.session_store.Probe());
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

  builder << "Session:\n";
  builder << "  - storage: " << diagnostics.session.storage_path.string() << '\n';
  builder << "  - loaded from disk: "
          << (diagnostics.session.loaded_from_disk ? "yes" : "no") << '\n';
  builder << "  - restored locks: "
          << diagnostics.session.restored_locked_monitors << '\n';
  builder << "  - disconnected monitors: "
          << diagnostics.session.disconnected_monitors << '\n';
  builder << "  - review required: " << diagnostics.session.review_monitors
          << '\n';

  builder << "Monitors:\n";
  if (diagnostics.monitors.empty()) {
    builder << "  - none detected in the active host build\n";
  } else {
    for (const auto& monitor : diagnostics.monitors) {
      builder << FormatMonitorLine(monitor, diagnostics.session) << '\n';
    }
  }

  return builder.str();
}

}  // namespace locking_glass::core
