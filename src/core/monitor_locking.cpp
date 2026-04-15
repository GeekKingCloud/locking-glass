#include "locking_glass/core/monitor_locking.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace locking_glass::core {

namespace {

bool WindowMatchesMonitor(const DesktopWindow& window,
                          const platform::MonitorDescriptor& monitor) {
  if (!window.monitor_id.empty() &&
      (window.monitor_id == monitor.stable_id ||
       window.monitor_id == monitor.device_path ||
       window.monitor_id == monitor.label)) {
    return true;
  }

  return !window.monitor_label.empty() && window.monitor_label == monitor.label;
}

const SessionMonitorState* FindLockedPresentMonitor(
    const SessionRefreshResult& session, const DesktopWindow& window) {
  for (const auto& monitor_state : session.snapshot.monitors) {
    if (!monitor_state.is_present || !monitor_state.locked) {
      continue;
    }
    if (WindowMatchesMonitor(window, monitor_state.monitor)) {
      return &monitor_state;
    }
  }
  return nullptr;
}

std::string DescribeWindow(const DesktopWindow& window) {
  if (!window.title.empty()) {
    return window.title;
  }
  if (!window.window_id.empty()) {
    return window.window_id;
  }
  return "<unnamed>";
}

std::string DescribeMonitor(const platform::MonitorDescriptor& monitor) {
  if (!monitor.label.empty()) {
    return monitor.label;
  }
  if (!monitor.display_name.empty()) {
    return monitor.display_name;
  }
  if (!monitor.stable_id.empty()) {
    return monitor.stable_id;
  }
  return "<unknown-monitor>";
}

void AppendLockedMonitor(std::vector<std::string>* locked_monitors,
                         const platform::MonitorDescriptor& monitor) {
  const std::string description = DescribeMonitor(monitor);
  if (std::find(locked_monitors->begin(), locked_monitors->end(), description) ==
      locked_monitors->end()) {
    locked_monitors->push_back(description);
  }
}

std::size_t CountPresentMonitors(const SessionRefreshResult& session) {
  std::size_t count = 0;
  for (const auto& monitor_state : session.snapshot.monitors) {
    if (monitor_state.is_present) {
      ++count;
    }
  }
  return count;
}

}  // namespace

MonitorLockingPlan BuildMonitorLockingPlan(
    const SessionStore& store, const DesktopSwitchScenario& scenario) {
  MonitorLockingPlan plan{
      .trigger = scenario.trigger,
      .session = store.Restore(scenario.monitors),
      .source_desktop_id = scenario.source_desktop_id,
      .target_desktop_id = scenario.target_desktop_id,
      .locked_monitors = {},
      .moves = {},
      .skipped_windows = {},
  };

  for (const auto& monitor_state : plan.session.snapshot.monitors) {
    if (!monitor_state.is_present || !monitor_state.locked) {
      continue;
    }
    AppendLockedMonitor(&plan.locked_monitors, monitor_state.monitor);
  }

  if (plan.source_desktop_id.empty() || plan.target_desktop_id.empty() ||
      plan.source_desktop_id == plan.target_desktop_id) {
    return plan;
  }

  for (const auto& window : scenario.windows) {
    const auto* monitor = FindLockedPresentMonitor(plan.session, window);
    if (monitor == nullptr) {
      continue;
    }

    if (window.desktop_id != plan.source_desktop_id &&
        window.desktop_id != plan.target_desktop_id) {
      continue;
    }

    if (!window.is_top_level) {
      plan.skipped_windows.push_back(MonitorLockingSkip{
          .window = window,
          .reason = "window is not top-level",
      });
      continue;
    }

    if (!window.can_move) {
      plan.skipped_windows.push_back(MonitorLockingSkip{
          .window = window,
          .reason = "window cannot be moved",
      });
      continue;
    }

    const std::string destination =
        window.desktop_id == plan.source_desktop_id ? plan.target_desktop_id
                                                    : plan.source_desktop_id;
    plan.moves.push_back(MonitorLockingMove{
        .window = window,
        .from_desktop_id = window.desktop_id,
        .to_desktop_id = std::move(destination),
    });
  }

  return plan;
}

std::string FormatMonitorLockingPlan(const MonitorLockingPlan& plan) {
  std::ostringstream builder;
  builder << "LockingGlass desktop switch policy\n";
  builder << "Trigger:\n";
  builder << "  - source: " << plan.trigger << '\n';
  builder << "  - from desktop: " << plan.source_desktop_id << '\n';
  builder << "  - to desktop: " << plan.target_desktop_id << '\n';

  builder << "Session:\n";
  builder << "  - active monitors: " << CountPresentMonitors(plan.session)
          << '\n';
  builder << "  - restored locks: " << plan.session.restored_locked_monitors
          << '\n';
  builder << "  - review required: " << plan.session.review_monitors << '\n';

  builder << "Policy:\n";
  builder << "  - locked monitors: " << plan.locked_monitors.size() << '\n';
  builder << "  - planned moves: " << plan.moves.size() << '\n';
  builder << "  - skipped windows: " << plan.skipped_windows.size() << '\n';

  builder << "Locked monitors:\n";
  if (plan.locked_monitors.empty()) {
    builder << "  - none\n";
  } else {
    for (const auto& monitor : plan.locked_monitors) {
      builder << "  - " << monitor << '\n';
    }
  }

  builder << "Moves:\n";
  if (plan.moves.empty()) {
    builder << "  - none\n";
  } else {
    for (const auto& move : plan.moves) {
      builder << "  - " << DescribeWindow(move.window) << " ["
              << (move.window.monitor_label.empty() ? move.window.monitor_id
                                                    : move.window.monitor_label)
              << "] " << move.from_desktop_id << " -> " << move.to_desktop_id
              << '\n';
    }
  }

  builder << "Skipped:\n";
  if (plan.skipped_windows.empty()) {
    builder << "  - none\n";
  } else {
    for (const auto& skipped : plan.skipped_windows) {
      builder << "  - " << DescribeWindow(skipped.window) << " ["
              << (skipped.window.monitor_label.empty()
                      ? skipped.window.monitor_id
                      : skipped.window.monitor_label)
              << "] : " << skipped.reason << '\n';
    }
  }

  return builder.str();
}

}  // namespace locking_glass::core
