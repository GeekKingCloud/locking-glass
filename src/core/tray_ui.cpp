#include "locking_glass/core/tray_ui.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace locking_glass::core {

namespace {

std::string BuildMonitorDisplayLabel(
    const platform::MonitorDescriptor& monitor) {
  std::ostringstream builder;
  builder << monitor.label;
  if (!monitor.display_name.empty()) {
    builder << " - " << monitor.display_name;
  }
  return builder.str();
}

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

std::vector<platform::MonitorDescriptor> CollectPromptMonitors(
    const SessionRefreshResult& session) {
  std::vector<platform::MonitorDescriptor> monitors;
  if (session.new_monitors == 0U || session.snapshot.monitors.empty()) {
    return monitors;
  }

  const std::size_t prompt_count =
      std::min(session.new_monitors, session.snapshot.monitors.size());
  const std::size_t start_index =
      session.snapshot.monitors.size() - prompt_count;
  monitors.reserve(prompt_count);

  for (std::size_t index = start_index; index < session.snapshot.monitors.size();
       ++index) {
    const auto& monitor_state = session.snapshot.monitors[index];
    if (!monitor_state.is_present || !monitor_state.requires_confirmation) {
      continue;
    }
    monitors.push_back(monitor_state.monitor);
  }

  return monitors;
}

std::string BuildPromptMonitorList(
    const std::vector<platform::MonitorDescriptor>& monitors) {
  constexpr std::size_t kMaxListedMonitors = 3U;

  std::ostringstream builder;
  const std::size_t listed_monitors =
      std::min(monitors.size(), kMaxListedMonitors);
  for (std::size_t index = 0; index < listed_monitors; ++index) {
    if (index > 0U) {
      builder << ", ";
    }
    builder << BuildMonitorDisplayLabel(monitors[index]);
  }

  if (monitors.size() > listed_monitors) {
    builder << ", +" << (monitors.size() - listed_monitors) << " more";
  }

  return builder.str();
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

MonitorReviewPrompt BuildMonitorReviewPrompt(
    const SessionRefreshResult& session) {
  auto monitors = CollectPromptMonitors(session);
  if (monitors.empty()) {
    return {};
  }

  MonitorReviewPrompt prompt{
      .visible = true,
      .title = {},
      .message = {},
      .monitors = std::move(monitors),
  };

  if (prompt.monitors.size() == 1U) {
    prompt.title = "Review new monitor lock state";
    prompt.message =
        BuildMonitorDisplayLabel(prompt.monitors.front()) +
        " was added unlocked. Open the LockingGlass tray icon to review its "
        "lock state.";
    return prompt;
  }

  prompt.title = "Review new monitor lock states";
  prompt.message = std::to_string(prompt.monitors.size()) +
                   " monitors were added unlocked: " +
                   BuildPromptMonitorList(prompt.monitors) +
                   ". Open the LockingGlass tray icon to review their lock "
                   "states.";
  return prompt;
}

std::string BuildTrayMonitorLabel(const TrayMonitorState& monitor) {
  std::ostringstream builder;
  builder << BuildMonitorDisplayLabel(monitor.monitor);
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
