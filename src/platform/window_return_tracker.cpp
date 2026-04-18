#include "locking_glass/platform/window_return_tracker.h"

namespace locking_glass::platform {

namespace {

std::string BuildTrackedWindowKey(const std::string& monitor_key,
                                  const std::string& window_id) {
  return monitor_key + "|" + window_id;
}

bool MatchesTrackedMonitor(const std::string& tracked_key,
                           const std::string& monitor_key) {
  return tracked_key.rfind(monitor_key + "|", 0) == 0;
}

}  // namespace

std::string BuildTrackedMonitorKey(const MonitorDescriptor& monitor) {
  if (!monitor.stable_id.empty()) {
    return monitor.stable_id;
  }
  if (!monitor.device_path.empty()) {
    return monitor.device_path;
  }
  return monitor.label;
}

std::string BuildTrackedMonitorKey(const core::DesktopWindow& window) {
  if (!window.monitor_id.empty()) {
    return window.monitor_id;
  }
  if (!window.monitor_label.empty()) {
    return window.monitor_label;
  }
  return "<unknown-monitor>";
}

void WindowReturnTracker::RecordSuccessfulMoves(
    const integration::DesktopSwitchReport& report,
    const std::function<bool(const core::DesktopWindow&)>& monitor_is_locked) {
  std::lock_guard lock(mutex_);
  for (const auto& move_result : report.move_results) {
    if (!move_result.success || !monitor_is_locked(move_result.window)) {
      continue;
    }

    const std::string monitor_key = BuildTrackedMonitorKey(move_result.window);
    const std::string key =
        BuildTrackedWindowKey(monitor_key, move_result.window.window_id);
    const auto it = tracked_windows_.find(key);
    if (it == tracked_windows_.end()) {
      // The first successful follow-move defines the remembered home desktop.
      // Later successful moves refresh only the live window snapshot.
      tracked_windows_.emplace(
          key, integration::TrackedWindowReturn{
                   .window = move_result.window,
                   .home_desktop = move_result.from_desktop,
               });
      continue;
    }

    it->second.window = move_result.window;
  }
}

void WindowReturnTracker::ClearMonitor(const std::string& monitor_key) {
  std::lock_guard lock(mutex_);
  for (auto it = tracked_windows_.begin(); it != tracked_windows_.end();) {
    if (MatchesTrackedMonitor(it->first, monitor_key)) {
      it = tracked_windows_.erase(it);
    } else {
      ++it;
    }
  }
}

void WindowReturnTracker::ClearMonitor(const MonitorDescriptor& monitor) {
  ClearMonitor(BuildTrackedMonitorKey(monitor));
}

std::vector<integration::TrackedWindowReturn> WindowReturnTracker::ConsumeMonitor(
    const std::string& monitor_key) {
  std::lock_guard lock(mutex_);
  std::vector<integration::TrackedWindowReturn> tracked;
  for (auto it = tracked_windows_.begin(); it != tracked_windows_.end();) {
    if (MatchesTrackedMonitor(it->first, monitor_key)) {
      tracked.push_back(it->second);
      it = tracked_windows_.erase(it);
    } else {
      ++it;
    }
  }
  return tracked;
}

std::vector<integration::TrackedWindowReturn> WindowReturnTracker::ConsumeMonitor(
    const MonitorDescriptor& monitor) {
  return ConsumeMonitor(BuildTrackedMonitorKey(monitor));
}

}  // namespace locking_glass::platform
