#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "locking_glass/core/monitor_locking.h"
#include "locking_glass/integration/capability.h"

namespace locking_glass::integration {

struct WindowMoveResult {
  core::DesktopWindow window;
  std::string from_desktop_id;
  std::string to_desktop_id;
  bool success = false;
  std::string detail;
};

struct DesktopSwitchReport {
  core::MonitorLockingPlan plan;
  std::vector<WindowMoveResult> move_results;
  std::vector<core::DesktopWindow> resulting_windows;
};

struct DesktopWatchOptions {
  bool allow_script_replay = true;
  int required_events = 2;
  int timeout_seconds = 0;
};

using DesktopSwitchCallback = std::function<bool(const DesktopSwitchReport&)>;

class VirtualDesktopController {
 public:
  virtual ~VirtualDesktopController() = default;
  virtual CapabilityReport Probe() const = 0;
  virtual int WatchSwitches(const core::SessionStore& store,
                            const DesktopSwitchCallback& callback,
                            DesktopWatchOptions options =
                                DesktopWatchOptions{}) const = 0;
};

std::string FormatDesktopSwitchReport(const DesktopSwitchReport& report);
std::unique_ptr<VirtualDesktopController> CreateVirtualDesktopController();

}  // namespace locking_glass::integration
