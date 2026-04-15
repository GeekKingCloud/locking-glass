#include "locking_glass/core/tray_ui.h"

#include <sstream>
#include <utility>

namespace locking_glass::core {

namespace {

SessionMonitorState* FindPresentMonitorState(
    SessionSnapshot* snapshot, const platform::MonitorDescriptor& monitor) {
  if (snapshot == nullptr) {
    return nullptr;
  }

  for (auto& monitor_state : snapshot->monitors) {
    if (!monitor_state.is_present) {
      continue;
    }
    if (monitor_state.monitor.stable_id == monitor.stable_id &&
        monitor_state.monitor.device_path == monitor.device_path &&
        monitor_state.monitor.edid_serial == monitor.edid_serial &&
        monitor_state.monitor.display_name == monitor.display_name &&
        monitor_state.monitor.label == monitor.label &&
        monitor_state.monitor.bounds.left == monitor.bounds.left &&
        monitor_state.monitor.bounds.top == monitor.bounds.top &&
        monitor_state.monitor.bounds.right == monitor.bounds.right &&
        monitor_state.monitor.bounds.bottom == monitor.bounds.bottom &&
        monitor_state.monitor.is_primary == monitor.is_primary) {
      return &monitor_state;
    }
  }

  for (auto& monitor_state : snapshot->monitors) {
    if (!monitor_state.is_present) {
      continue;
    }
    if (monitor_state.monitor.label == monitor.label ||
        (!monitor.device_path.empty() &&
         monitor_state.monitor.device_path == monitor.device_path) ||
        (!monitor.stable_id.empty() &&
         monitor_state.monitor.stable_id == monitor.stable_id)) {
      return &monitor_state;
    }
  }

  return nullptr;
}

}  // namespace

TrayMenuModel BuildTrayMenuModel(const SessionRefreshResult& session,
                                 std::string trigger) {
  TrayMenuModel model{
      .trigger = std::move(trigger),
      .monitors = {},
      .locked_monitors = 0,
      .review_monitors = 0,
  };

  for (const auto& monitor_state : session.snapshot.monitors) {
    if (!monitor_state.is_present) {
      continue;
    }

    model.monitors.push_back(TrayMonitorState{
        .monitor = monitor_state.monitor,
        .locked = monitor_state.locked,
        .requires_confirmation = monitor_state.requires_confirmation,
    });
    if (monitor_state.locked) {
      ++model.locked_monitors;
    }
    if (monitor_state.requires_confirmation) {
      ++model.review_monitors;
    }
  }

  return model;
}

std::string BuildTrayMonitorLabel(const TrayMonitorState& monitor) {
  std::ostringstream builder;
  builder << monitor.monitor.label;
  if (!monitor.monitor.display_name.empty()) {
    builder << " - " << monitor.monitor.display_name;
  }
  if (monitor.requires_confirmation) {
    builder << " [review]";
  }
  return builder.str();
}

std::string FormatTrayMenuModel(const TrayMenuModel& model) {
  std::ostringstream builder;
  builder << "LockingGlass tray menu\n";
  builder << "Trigger:\n";
  builder << "  - source: " << model.trigger << '\n';
  builder << "Summary:\n";
  builder << "  - visible monitors: " << model.monitors.size() << '\n';
  builder << "  - locked monitors: " << model.locked_monitors << '\n';
  builder << "  - review required: " << model.review_monitors << '\n';
  builder << "Monitors:\n";
  if (model.monitors.empty()) {
    builder << "  - none available\n";
    return builder.str();
  }

  for (const auto& monitor : model.monitors) {
    builder << "  - " << BuildTrayMonitorLabel(monitor) << " : "
            << (monitor.locked ? "locked" : "unlocked") << '\n';
  }
  return builder.str();
}

bool ToggleMonitorLock(const SessionStore& store, SessionSnapshot* snapshot,
                       const platform::MonitorDescriptor& monitor,
                       bool* locked_after) {
  auto* current_state = FindPresentMonitorState(snapshot, monitor);
  if (current_state == nullptr) {
    return false;
  }

  const bool next_locked = !current_state->locked;
  if (!store.SetLocked(snapshot, current_state->monitor, next_locked)) {
    return false;
  }
  if (!store.Save(*snapshot)) {
    return false;
  }
  if (locked_after != nullptr) {
    *locked_after = next_locked;
  }
  return true;
}

}  // namespace locking_glass::core
