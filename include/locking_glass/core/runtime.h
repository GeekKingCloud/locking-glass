#pragma once

#include <memory>
#include <string>
#include <vector>

#include "locking_glass/integration/autostart.h"
#include "locking_glass/integration/capability.h"
#include "locking_glass/integration/ffmpeg_probe.h"
#include "locking_glass/integration/windows_api_probe.h"
#include "locking_glass/core/session_store.h"
#include "locking_glass/platform/background_session.h"
#include "locking_glass/platform/monitor_gateway.h"
#include "locking_glass/platform/monitor_watcher.h"

namespace locking_glass::core {

struct Runtime {
  SessionStore session_store;
  std::unique_ptr<platform::MonitorGateway> monitor_gateway;
  std::unique_ptr<platform::MonitorWatcher> monitor_watcher;
  std::unique_ptr<platform::BackgroundSession> background_session;
  std::unique_ptr<integration::FfmpegProbe> ffmpeg_probe;
  std::unique_ptr<integration::WindowsApiProbe> windows_api_probe;
  std::unique_ptr<integration::AutostartManager> autostart_manager;
};

struct StartupDiagnostics {
  std::vector<platform::MonitorDescriptor> monitors;
  SessionRefreshResult session;
  std::vector<integration::CapabilityReport> capabilities;
  integration::AutostartPlan autostart;
};

struct MonitorRefreshReport {
  std::vector<platform::MonitorDescriptor> monitors;
  SessionRefreshResult session;
  std::string trigger;
  std::string topology_fingerprint;
  bool topology_changed = false;
};

Runtime BuildRuntime();
StartupDiagnostics CollectStartupDiagnostics(const Runtime& runtime,
                                            const std::string& executable_path);
std::string BuildMonitorTopologyFingerprint(
    const std::vector<platform::MonitorDescriptor>& monitors);
MonitorRefreshReport RefreshMonitorState(const Runtime& runtime,
                                         std::vector<platform::MonitorDescriptor> monitors,
                                         std::string trigger,
                                         std::string previous_fingerprint = {});
std::string FormatDiagnostics(const StartupDiagnostics& diagnostics);
std::string FormatMonitorRefreshReport(const MonitorRefreshReport& report);

}  // namespace locking_glass::core
