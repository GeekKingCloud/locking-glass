#pragma once

#include <memory>

#include "locking_glass/integration/capability.h"

namespace locking_glass::integration {

class WindowsApiProbe {
 public:
  virtual ~WindowsApiProbe() = default;
  virtual CapabilityReport Probe() const = 0;
};

std::unique_ptr<WindowsApiProbe> CreateWindowsApiProbe();

}  // namespace locking_glass::integration
