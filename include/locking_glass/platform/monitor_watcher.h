#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "locking_glass/platform/monitor_gateway.h"

namespace locking_glass::platform {

struct MonitorWatchEvent {
  std::string trigger;
  std::vector<MonitorDescriptor> monitors;
};

using MonitorWatchCallback = std::function<bool(const MonitorWatchEvent&)>;

class MonitorWatcher {
 public:
  virtual ~MonitorWatcher() = default;
  virtual int Watch(const MonitorWatchCallback& callback) const = 0;
};

std::unique_ptr<MonitorWatcher> CreateMonitorWatcher();

}  // namespace locking_glass::platform
