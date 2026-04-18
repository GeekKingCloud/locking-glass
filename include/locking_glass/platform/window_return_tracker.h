#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "locking_glass/integration/virtual_desktop_controller.h"
#include "locking_glass/platform/monitor_gateway.h"

namespace locking_glass::platform {

std::string BuildTrackedMonitorKey(const MonitorDescriptor& monitor);
std::string BuildTrackedMonitorKey(const core::DesktopWindow& window);

class WindowReturnTracker {
 public:
  void RecordSuccessfulMoves(
      const integration::DesktopSwitchReport& report,
      const std::function<bool(const core::DesktopWindow&)>& monitor_is_locked);
  void ClearMonitor(const std::string& monitor_key);
  void ClearMonitor(const MonitorDescriptor& monitor);
  std::vector<integration::TrackedWindowReturn> ConsumeMonitor(
      const std::string& monitor_key);
  std::vector<integration::TrackedWindowReturn> ConsumeMonitor(
      const MonitorDescriptor& monitor);

 private:
  std::mutex mutex_;
  std::map<std::string, integration::TrackedWindowReturn> tracked_windows_;
};

}  // namespace locking_glass::platform
