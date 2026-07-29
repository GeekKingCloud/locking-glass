#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "locking_glass/integration/virtual_desktop_controller.h"
#include "locking_glass/platform/monitor_gateway.h"

namespace locking_glass::platform {

std::string BuildTrackedMonitorKey(const MonitorDescriptor& monitor);
std::string BuildTrackedMonitorKey(const core::DesktopWindow& window);

struct MonitorReturnState {
  std::vector<integration::TrackedWindowReturn> tracked_windows;
  std::optional<integration::DesktopIdentity> home_desktop;
  std::optional<integration::DesktopIdentity> current_desktop;
};

class WindowReturnTracker {
 public:
  void RecordSuccessfulMoves(
      const integration::DesktopSwitchReport& report,
      const std::function<bool(const core::DesktopWindow&)>& monitor_is_locked);
  void ClearMonitor(const std::string& monitor_key);
  void ClearMonitor(const MonitorDescriptor& monitor);
  void RestoreMonitor(
      const std::string& monitor_key,
      const std::vector<integration::TrackedWindowReturn>& tracked_windows);
  void RestoreMonitor(
      const MonitorDescriptor& monitor,
      const std::vector<integration::TrackedWindowReturn>& tracked_windows);
  void RestoreMonitorState(const std::string& monitor_key,
                           const MonitorReturnState& state);
  void RestoreMonitorState(const MonitorDescriptor& monitor,
                           const MonitorReturnState& state);
  std::vector<core::StagingRestoreHint> BuildStagingRestoreHints(
      const core::DesktopSwitchScenario& scenario) const;
  MonitorReturnState ConsumeMonitorState(const std::string& monitor_key);
  MonitorReturnState ConsumeMonitorState(const MonitorDescriptor& monitor);
  std::vector<integration::TrackedWindowReturn> ConsumeMonitor(
      const std::string& monitor_key);
  std::vector<integration::TrackedWindowReturn> ConsumeMonitor(
      const MonitorDescriptor& monitor);

 private:
  mutable std::mutex mutex_;
  std::map<std::string, integration::TrackedWindowReturn> tracked_windows_;
  std::map<std::string, integration::DesktopIdentity> monitor_home_desktops_;
  std::map<std::string, integration::DesktopIdentity> monitor_current_desktops_;
};

}  // namespace locking_glass::platform
