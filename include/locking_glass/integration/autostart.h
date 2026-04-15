#pragma once

#include <memory>
#include <string>

#include "locking_glass/integration/capability.h"

namespace locking_glass::integration {

struct AutostartPlan {
  std::string scope;
  std::string location;
  std::string entry_name;
  std::string launch_mode;
  std::string launch_command;
};

struct AutostartRegistrationResult {
  bool success = false;
  bool changed = false;
  std::string detail;
};

class AutostartManager {
 public:
  virtual ~AutostartManager() = default;
  virtual CapabilityReport Probe() const = 0;
  virtual AutostartPlan BuildPlan(const std::string& executable_path) const = 0;
  virtual AutostartRegistrationResult Enable(
      const std::string& executable_path) const = 0;
};

std::string QuoteWindowsCommandArg(const std::string& value);
std::unique_ptr<AutostartManager> CreateAutostartManager();

}  // namespace locking_glass::integration
