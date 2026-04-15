#pragma once

#include <memory>
#include <string>
#include <vector>

#include "locking_glass/integration/capability.h"
#include "locking_glass/platform/monitor_gateway.h"

namespace locking_glass::integration {

struct WindowsApiBoundary {
  std::string name;
  std::string purpose;
  CapabilityReport capability;
  std::vector<std::string> windows_apis;
  std::vector<std::string> in_scope;
  std::vector<std::string> out_of_scope;
  std::vector<std::string> expected_behavior;
};

struct WindowsApiPrototype {
  std::vector<WindowsApiBoundary> boundaries;
  std::vector<std::string> interaction_steps;
};

class WindowsApiProbe {
 public:
  virtual ~WindowsApiProbe() = default;
  virtual CapabilityReport Probe() const = 0;
  virtual WindowsApiPrototype BuildPrototype(
      const std::vector<platform::MonitorDescriptor>& monitors) const = 0;
};

std::string FormatWindowsApiPrototype(const WindowsApiPrototype& prototype);
std::unique_ptr<WindowsApiProbe> CreateWindowsApiProbe();

}  // namespace locking_glass::integration
