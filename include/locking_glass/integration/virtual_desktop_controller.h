#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "locking_glass/core/monitor_locking.h"
#include "locking_glass/integration/capability.h"

namespace locking_glass::integration {

struct DesktopIdentity {
  int number = -1;
  std::string guid;
  std::string name;
  std::string display_id;
};

struct WindowMoveResult {
  core::DesktopWindow window;
  DesktopIdentity from_desktop;
  DesktopIdentity to_desktop;
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
  // Replay is a test seam; live Windows proof still requires helper-backed
  // desktop-switch events from VirtualDesktopAccessor.
  bool allow_script_replay = true;
  int required_events = 2;
  int timeout_seconds = 0;
};

struct TrackedWindowReturn {
  core::DesktopWindow window;
  DesktopIdentity home_desktop;
  std::optional<DesktopIdentity> staging_desktop;
};

struct UnlockReturnRequest {
  platform::MonitorDescriptor monitor;
  std::vector<TrackedWindowReturn> tracked_windows;
};

struct UnlockReturnSkip {
  core::DesktopWindow window;
  DesktopIdentity current_desktop;
  DesktopIdentity home_desktop;
  std::string reason;
};

struct UnlockReturnReport {
  platform::MonitorDescriptor monitor;
  std::vector<WindowMoveResult> move_results;
  std::vector<UnlockReturnSkip> skipped_windows;
  std::vector<core::DesktopWindow> resulting_windows;
};

using DesktopSwitchCallback = std::function<bool(const DesktopSwitchReport&)>;

class VirtualDesktopController {
 public:
  virtual ~VirtualDesktopController() = default;
  virtual CapabilityReport Probe() const = 0;
  virtual UnlockReturnReport ReturnTrackedWindows(
      const UnlockReturnRequest& request) const = 0;
  virtual int WatchSwitches(const core::SessionStore& store,
                            const DesktopSwitchCallback& callback,
                            DesktopWatchOptions options =
                                DesktopWatchOptions{}) const = 0;
};

std::string FormatDesktopIdentity(const DesktopIdentity& desktop);
std::string FormatDesktopSwitchReport(const DesktopSwitchReport& report);
std::string FormatUnlockReturnReport(const UnlockReturnReport& report);
std::unique_ptr<VirtualDesktopController> CreateVirtualDesktopController();

}  // namespace locking_glass::integration
