#pragma once

#include <memory>
#include <string>
#include <vector>

#include "locking_glass/integration/capability.h"
#include "locking_glass/integration/ffmpeg_probe.h"
#include "locking_glass/integration/windows_api_probe.h"
#include "locking_glass/platform/monitor_gateway.h"

namespace locking_glass::core {

struct Runtime {
  std::unique_ptr<platform::MonitorGateway> monitor_gateway;
  std::unique_ptr<integration::FfmpegProbe> ffmpeg_probe;
  std::unique_ptr<integration::WindowsApiProbe> windows_api_probe;
};

struct StartupDiagnostics {
  std::vector<platform::MonitorDescriptor> monitors;
  std::vector<integration::CapabilityReport> capabilities;
};

Runtime BuildRuntime();
StartupDiagnostics CollectStartupDiagnostics(const Runtime& runtime);
std::string FormatDiagnostics(const StartupDiagnostics& diagnostics);

}  // namespace locking_glass::core
