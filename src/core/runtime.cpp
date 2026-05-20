#include "locking_glass/core/runtime.h"

#include <algorithm>
#include <sstream>
#include <utility>
#include <vector>

#include "locking_glass/core/tray_ui.h"

namespace locking_glass::core {

namespace {

const SessionMonitorState* FindSessionState(
    const SessionRefreshResult& session,
    const platform::MonitorDescriptor& monitor) {
  for (const auto& monitor_state : session.snapshot.monitors) {
    if (!monitor_state.is_present) {
      continue;
    }
    if (ExactMonitorIdentityEqual(monitor_state.monitor, monitor)) {
      return &monitor_state;
    }
  }
  return nullptr;
}

std::string FormatMonitorLine(const platform::MonitorDescriptor& monitor,
                              const SessionRefreshResult& session) {
  std::ostringstream builder;
  builder << "  - " << monitor.label;
  if (!monitor.display_name.empty()) {
    builder << " \"" << monitor.display_name << "\"";
  }
  builder << " [id=" << monitor.stable_id << "] "
          << "(" << monitor.bounds.left << "," << monitor.bounds.top << ")-("
          << monitor.bounds.right << "," << monitor.bounds.bottom << ")";
  if (monitor.is_primary) {
    builder << " primary";
  }
  if (!monitor.device_path.empty() && monitor.device_path != monitor.stable_id) {
    builder << " path=" << monitor.device_path;
  }
  if (!monitor.edid_serial.empty()) {
    builder << " serial=" << monitor.edid_serial;
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
      .monitor_watcher = platform::CreateMonitorWatcher(),
      .background_session = platform::CreateBackgroundSession(),
      .virtual_desktop_controller = integration::CreateVirtualDesktopController(),
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
  diagnostics.capabilities.push_back(runtime.virtual_desktop_controller->Probe());
  diagnostics.capabilities.push_back(runtime.windows_api_probe->Probe());
  diagnostics.capabilities.push_back(runtime.autostart_manager->Probe());
  diagnostics.capabilities.push_back(runtime.session_store.Probe());
  diagnostics.autostart =
      runtime.autostart_manager->BuildPlan(executable_path);
  return diagnostics;
}

std::string BuildMonitorTopologyFingerprint(
    const std::vector<platform::MonitorDescriptor>& monitors) {
  std::vector<std::string> entries;
  entries.reserve(monitors.size());

  for (const auto& monitor : monitors) {
    std::ostringstream entry;
    entry << monitor.stable_id << '|'
          << monitor.device_path << '|'
          << monitor.edid_serial << '|'
          << monitor.bounds.left << ',' << monitor.bounds.top << ','
          << monitor.bounds.right << ',' << monitor.bounds.bottom << '|'
          << (monitor.is_primary ? '1' : '0');
    entries.push_back(entry.str());
  }

  std::sort(entries.begin(), entries.end());

  std::ostringstream fingerprint;
  for (const auto& entry : entries) {
    fingerprint << entry << '\n';
  }
  return fingerprint.str();
}

MonitorRefreshReport RefreshMonitorState(const Runtime& runtime,
                                         std::vector<platform::MonitorDescriptor> monitors,
                                         std::string trigger,
                                         std::string previous_fingerprint) {
  MonitorRefreshReport report{
      .monitors = std::move(monitors),
      .session = SessionRefreshResult{},
      .trigger = std::move(trigger),
      .topology_fingerprint = {},
      .topology_changed = false,
  };
  report.topology_fingerprint = BuildMonitorTopologyFingerprint(report.monitors);
  report.topology_changed = previous_fingerprint.empty() ||
                            previous_fingerprint != report.topology_fingerprint;
  report.session = runtime.session_store.Restore(report.monitors);
  return report;
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
  builder << "  - storage issue: "
          << ToString(diagnostics.session.storage_issue) << '\n';
  builder << "  - storage detail: " << diagnostics.session.storage_detail << '\n';
  builder << "  - restored locks: "
          << diagnostics.session.restored_locked_monitors << '\n';
  builder << "  - disconnected monitors: "
          << diagnostics.session.disconnected_monitors << '\n';
  builder << "  - confirmation required: "
          << diagnostics.session.review_monitors << '\n';
  if (!diagnostics.session.invalid_storage_backup_path.empty()) {
    builder << "  - rejected backup: "
            << diagnostics.session.invalid_storage_backup_path.string() << '\n';
  }

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

std::string FormatMonitorRefreshReport(const MonitorRefreshReport& report) {
  std::ostringstream builder;
  builder << "LockingGlass monitor refresh\n";
  builder << "Trigger:\n";
  builder << "  - source: " << report.trigger << '\n';
  builder << "  - topology changed: "
          << (report.topology_changed ? "yes" : "no") << '\n';

  builder << "Session:\n";
  builder << "  - active monitors: " << report.monitors.size() << '\n';
  builder << "  - storage issue: " << ToString(report.session.storage_issue)
          << '\n';
  builder << "  - storage detail: " << report.session.storage_detail << '\n';
  builder << "  - restored locks: "
          << report.session.restored_locked_monitors << '\n';
  builder << "  - disconnected monitors: "
          << report.session.disconnected_monitors << '\n';
  builder << "  - new monitors: " << report.session.new_monitors << '\n';
  builder << "  - confirmation required: " << report.session.review_monitors
          << '\n';
  if (report.session.recovered_invalid_data) {
    builder << "  - invalid data recovered: yes\n";
  }
  if (!report.session.invalid_storage_backup_path.empty()) {
    builder << "  - rejected backup: "
            << report.session.invalid_storage_backup_path.string() << '\n';
  }

  const auto prompt = BuildMonitorReviewPrompt(report.session);
  if (prompt.visible) {
    builder << "Prompt:\n";
    builder << "  - title: " << prompt.title << '\n';
    builder << "  - message: " << prompt.message << '\n';
  }

  builder << "Monitors:\n";
  if (report.monitors.empty()) {
    builder << "  - none detected in the active host build\n";
  } else {
    for (const auto& monitor : report.monitors) {
      builder << FormatMonitorLine(monitor, report.session) << '\n';
    }
  }

  return builder.str();
}

}  // namespace locking_glass::core
