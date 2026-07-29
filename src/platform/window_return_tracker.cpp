#include "locking_glass/platform/window_return_tracker.h"

#include <cctype>
#include <set>

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

struct MonitorBorrowState {
  bool locked = false;
  bool has_blocking_issue = false;
  std::optional<integration::DesktopIdentity> source_home;
  std::optional<integration::DesktopIdentity> current_desktop;
  std::set<std::string> pending_move_keys;
};

bool HasDesktopId(const std::string& desktop_id) {
  return !desktop_id.empty();
}

bool DesktopIdEquals(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto left_char = static_cast<unsigned char>(left[index]);
    const auto right_char = static_cast<unsigned char>(right[index]);
    if (std::tolower(left_char) != std::tolower(right_char)) {
      return false;
    }
  }
  return true;
}

bool IsSourceFollowMove(const integration::DesktopSwitchReport& report,
                        const integration::WindowMoveResult& move_result) {
  if (!HasDesktopId(report.plan.source_desktop_id) ||
      !HasDesktopId(report.plan.target_desktop_id)) {
    return true;
  }

  return DesktopIdEquals(move_result.from_desktop_id,
                         report.plan.source_desktop_id) &&
         DesktopIdEquals(move_result.to_desktop_id,
                         report.plan.target_desktop_id);
}

bool IsStagingMove(const integration::DesktopSwitchReport& report,
                   const integration::WindowMoveResult& move_result) {
  return HasDesktopId(report.plan.staging_desktop_id) &&
         DesktopIdEquals(move_result.to_desktop_id,
                         report.plan.staging_desktop_id);
}

bool IsTrackedStagingRestoreMove(
    const integration::DesktopSwitchReport& report,
    const integration::WindowMoveResult& move_result,
    const integration::TrackedWindowReturn& tracked_window) {
  if (!HasDesktopId(report.plan.staging_desktop_id) ||
      !tracked_window.staging_desktop.has_value()) {
    return false;
  }

  const std::string tracked_staging_id =
      integration::FormatDesktopIdentity(*tracked_window.staging_desktop);
  const std::string tracked_home_id =
      integration::FormatDesktopIdentity(tracked_window.home_desktop);
  return DesktopIdEquals(move_result.from_desktop_id,
                         report.plan.staging_desktop_id) &&
         DesktopIdEquals(tracked_staging_id, report.plan.staging_desktop_id) &&
         DesktopIdEquals(move_result.to_desktop_id, tracked_home_id);
}

bool IsBlockingTargetDesktopOccupantSkip(
    const integration::DesktopSwitchReport& report,
    const core::MonitorLockingSkip& skipped_window) {
  return HasDesktopId(report.plan.target_desktop_id) &&
         DesktopIdEquals(skipped_window.window.desktop_id,
                         report.plan.target_desktop_id) &&
         skipped_window.window.is_top_level && skipped_window.window.can_move;
}

integration::DesktopIdentity MakePlanDesktopIdentity(
    const std::string& desktop_id) {
  return integration::DesktopIdentity{
      .number = -1,
      .guid = {},
      .name = {},
      .display_id = desktop_id,
  };
}

std::string BuildMoveKey(const std::string& monitor_key,
                         const std::string& window_id,
                         const std::string& from_desktop_id,
                         const std::string& to_desktop_id) {
  return monitor_key + "|" + window_id + "|" + from_desktop_id + "|" +
         to_desktop_id;
}

std::string BuildMoveKey(const integration::WindowMoveResult& move_result) {
  return BuildMoveKey(BuildTrackedMonitorKey(move_result.window),
                      move_result.window.window_id,
                      move_result.from_desktop_id,
                      move_result.to_desktop_id);
}

std::string BuildMoveKey(const core::MonitorLockingMove& move) {
  return BuildMoveKey(BuildTrackedMonitorKey(move.window),
                      move.window.window_id,
                      move.from_desktop_id,
                      move.to_desktop_id);
}

std::map<std::string, MonitorBorrowState> BuildMonitorBorrowStates(
    const integration::DesktopSwitchReport& report,
    const std::function<bool(const core::DesktopWindow&)>& monitor_is_locked) {
  std::map<std::string, MonitorBorrowState> states;
  if (!HasDesktopId(report.plan.source_desktop_id)) {
    return states;
  }

  const integration::DesktopIdentity source_home =
      MakePlanDesktopIdentity(report.plan.source_desktop_id);
  const std::optional<integration::DesktopIdentity> current_desktop =
      HasDesktopId(report.plan.target_desktop_id)
          ? std::make_optional(
                MakePlanDesktopIdentity(report.plan.target_desktop_id))
          : std::nullopt;
  for (const auto& monitor_state : report.plan.session.snapshot.monitors) {
    if (!monitor_state.is_present || !monitor_state.locked) {
      continue;
    }

    states.try_emplace(
        BuildTrackedMonitorKey(monitor_state.monitor),
        MonitorBorrowState{
            .locked = true,
            .has_blocking_issue = false,
            .source_home = source_home,
            .current_desktop = current_desktop,
            .pending_move_keys = {},
        });
  }

  for (const auto& move : report.plan.moves) {
    if (!monitor_is_locked(move.window)) {
      continue;
    }

    auto& state = states[BuildTrackedMonitorKey(move.window)];
    state.locked = true;
    if (!state.source_home.has_value()) {
      state.source_home = source_home;
    }
    if (!state.current_desktop.has_value()) {
      state.current_desktop = current_desktop;
    }
    state.pending_move_keys.insert(BuildMoveKey(move));
  }

  for (const auto& skipped : report.plan.skipped_windows) {
    if (!monitor_is_locked(skipped.window)) {
      continue;
    }

    auto& state = states[BuildTrackedMonitorKey(skipped.window)];
    state.locked = true;
    if (!state.source_home.has_value()) {
      state.source_home = source_home;
    }
    if (!state.current_desktop.has_value()) {
      state.current_desktop = current_desktop;
    }
    // Browser, shell, and helper windows often cannot be mapped to a virtual
    // desktop. Those extra skips are not planned content moves, so they should
    // not erase the monitor home after the real locked-window moves succeed.
    // Actual planned move failures and unresolved planned moves still block the
    // monitor home below.
    if (IsBlockingTargetDesktopOccupantSkip(report, skipped)) {
      state.has_blocking_issue = true;
    }
  }

  return states;
}

void RememberMonitorHome(
    const std::string& monitor_key,
    const integration::DesktopIdentity& home_desktop,
    std::map<std::string, integration::DesktopIdentity>* monitor_home_desktops) {
  monitor_home_desktops->try_emplace(monitor_key, home_desktop);
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
  auto borrow_states = BuildMonitorBorrowStates(report, monitor_is_locked);
  for (const auto& move_result : report.move_results) {
    if (!monitor_is_locked(move_result.window)) {
      continue;
    }

    const std::string monitor_key = BuildTrackedMonitorKey(move_result.window);
    auto& borrow_state = borrow_states[monitor_key];
    borrow_state.locked = true;
    if (!borrow_state.source_home.has_value() &&
        HasDesktopId(report.plan.source_desktop_id)) {
      borrow_state.source_home =
          MakePlanDesktopIdentity(report.plan.source_desktop_id);
    }
    if (!borrow_state.current_desktop.has_value() &&
        HasDesktopId(report.plan.target_desktop_id)) {
      borrow_state.current_desktop =
          MakePlanDesktopIdentity(report.plan.target_desktop_id);
    }
    borrow_state.pending_move_keys.erase(BuildMoveKey(move_result));
    if (!move_result.success) {
      borrow_state.has_blocking_issue = true;
    }
    if (move_result.success && IsSourceFollowMove(report, move_result) &&
        !borrow_state.source_home.has_value()) {
      borrow_state.source_home = move_result.from_desktop;
    }
    if (move_result.success && IsSourceFollowMove(report, move_result) &&
        monitor_home_desktops_.find(monitor_key) == monitor_home_desktops_.end()) {
      borrow_state.source_home = move_result.from_desktop;
    }
    if (move_result.success && IsSourceFollowMove(report, move_result)) {
      borrow_state.current_desktop = move_result.to_desktop;
    }
    if (!move_result.success) {
      continue;
    }

    const std::string key =
        BuildTrackedWindowKey(monitor_key, move_result.window.window_id);
    if (const auto tracked_it = tracked_windows_.find(key);
        tracked_it != tracked_windows_.end() &&
        IsTrackedStagingRestoreMove(report, move_result, tracked_it->second)) {
      tracked_windows_.erase(key);
      continue;
    }

    integration::DesktopIdentity home_desktop = move_result.from_desktop;
    std::optional<integration::DesktopIdentity> staging_desktop = std::nullopt;
    if (IsSourceFollowMove(report, move_result)) {
      if (const auto home_it = monitor_home_desktops_.find(monitor_key);
          home_it != monitor_home_desktops_.end()) {
        home_desktop = home_it->second;
      }
    } else if (IsStagingMove(report, move_result)) {
      staging_desktop = move_result.to_desktop;
    }

    const auto it = tracked_windows_.find(key);
    if (it == tracked_windows_.end()) {
      // Source follow-moves use the monitor's original desktop. Displaced
      // target occupants keep their own source desktop so unlock never sends
      // another workspace's windows to the locked monitor's home.
      tracked_windows_.emplace(
          key, integration::TrackedWindowReturn{
                   .window = move_result.window,
                   .home_desktop = home_desktop,
                   .staging_desktop = staging_desktop,
               });
      continue;
    }

    it->second.window = move_result.window;
  }

  for (auto& [monitor_key, state] : borrow_states) {
    if (!state.pending_move_keys.empty()) {
      state.has_blocking_issue = true;
    }
    if (state.has_blocking_issue) {
      monitor_home_desktops_.erase(monitor_key);
      continue;
    }
    if (!state.source_home.has_value()) {
      continue;
    }
    RememberMonitorHome(monitor_key, *state.source_home,
                        &monitor_home_desktops_);
    if (state.current_desktop.has_value()) {
      monitor_current_desktops_[monitor_key] = *state.current_desktop;
    }
  }
}

void WindowReturnTracker::ClearMonitor(const std::string& monitor_key) {
  std::lock_guard lock(mutex_);
  monitor_home_desktops_.erase(monitor_key);
  monitor_current_desktops_.erase(monitor_key);
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

void WindowReturnTracker::RestoreMonitor(
    const std::string& monitor_key,
    const std::vector<integration::TrackedWindowReturn>& tracked_windows) {
  std::lock_guard lock(mutex_);
  for (const auto& tracked_window : tracked_windows) {
    const std::string key =
        BuildTrackedWindowKey(monitor_key, tracked_window.window.window_id);
    tracked_windows_.try_emplace(key, tracked_window);
  }
}

void WindowReturnTracker::RestoreMonitor(
    const MonitorDescriptor& monitor,
    const std::vector<integration::TrackedWindowReturn>& tracked_windows) {
  RestoreMonitor(BuildTrackedMonitorKey(monitor), tracked_windows);
}

void WindowReturnTracker::RestoreMonitorState(
    const std::string& monitor_key, const MonitorReturnState& state) {
  std::lock_guard lock(mutex_);
  if (state.home_desktop.has_value()) {
    monitor_home_desktops_.try_emplace(monitor_key, *state.home_desktop);
  }
  if (state.current_desktop.has_value()) {
    monitor_current_desktops_[monitor_key] = *state.current_desktop;
  }
  for (const auto& tracked_window : state.tracked_windows) {
    const std::string key =
        BuildTrackedWindowKey(monitor_key, tracked_window.window.window_id);
    tracked_windows_.try_emplace(key, tracked_window);
  }
}

void WindowReturnTracker::RestoreMonitorState(
    const MonitorDescriptor& monitor, const MonitorReturnState& state) {
  RestoreMonitorState(BuildTrackedMonitorKey(monitor), state);
}

std::vector<core::StagingRestoreHint>
WindowReturnTracker::BuildStagingRestoreHints(
    const core::DesktopSwitchScenario& scenario) const {
  std::lock_guard lock(mutex_);
  std::vector<core::StagingRestoreHint> hints;
  if (!HasDesktopId(scenario.staging_desktop_id)) {
    return hints;
  }

  for (const auto& window : scenario.windows) {
    if (!DesktopIdEquals(window.desktop_id, scenario.staging_desktop_id)) {
      continue;
    }

    const std::string key = BuildTrackedWindowKey(
        BuildTrackedMonitorKey(window), window.window_id);
    const auto tracked_it = tracked_windows_.find(key);
    if (tracked_it == tracked_windows_.end() ||
        !tracked_it->second.staging_desktop.has_value()) {
      continue;
    }

    const std::string tracked_staging_id =
        integration::FormatDesktopIdentity(*tracked_it->second.staging_desktop);
    if (!DesktopIdEquals(tracked_staging_id, scenario.staging_desktop_id)) {
      continue;
    }

    const std::string home_desktop_id =
        integration::FormatDesktopIdentity(tracked_it->second.home_desktop);
    if (!HasDesktopId(home_desktop_id)) {
      continue;
    }

    hints.push_back(core::StagingRestoreHint{
        .window_id = window.window_id,
        .monitor_id = window.monitor_id,
        .monitor_label = window.monitor_label,
        .home_desktop_id = home_desktop_id,
    });
  }
  return hints;
}

MonitorReturnState WindowReturnTracker::ConsumeMonitorState(
    const std::string& monitor_key) {
  std::lock_guard lock(mutex_);
  MonitorReturnState state{
      .tracked_windows = {},
      .home_desktop = std::nullopt,
      .current_desktop = std::nullopt,
  };
  if (const auto home_it = monitor_home_desktops_.find(monitor_key);
      home_it != monitor_home_desktops_.end()) {
    state.home_desktop = home_it->second;
    monitor_home_desktops_.erase(home_it);
  }
  if (const auto current_it = monitor_current_desktops_.find(monitor_key);
      current_it != monitor_current_desktops_.end()) {
    state.current_desktop = current_it->second;
    monitor_current_desktops_.erase(current_it);
  }

  for (auto it = tracked_windows_.begin(); it != tracked_windows_.end();) {
    if (MatchesTrackedMonitor(it->first, monitor_key)) {
      state.tracked_windows.push_back(it->second);
      it = tracked_windows_.erase(it);
    } else {
      ++it;
    }
  }
  return state;
}

MonitorReturnState WindowReturnTracker::ConsumeMonitorState(
    const MonitorDescriptor& monitor) {
  return ConsumeMonitorState(BuildTrackedMonitorKey(monitor));
}

std::vector<integration::TrackedWindowReturn> WindowReturnTracker::ConsumeMonitor(
    const std::string& monitor_key) {
  return ConsumeMonitorState(monitor_key).tracked_windows;
}

std::vector<integration::TrackedWindowReturn> WindowReturnTracker::ConsumeMonitor(
    const MonitorDescriptor& monitor) {
  return ConsumeMonitor(BuildTrackedMonitorKey(monitor));
}

}  // namespace locking_glass::platform
