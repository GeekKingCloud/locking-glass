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

std::string BuildMonitorStatusLabel(const TrayMonitorState& monitor) {
  std::ostringstream builder;
  builder << (monitor.locked ? "locked" : "unlocked");
  if (monitor.requires_confirmation) {
    builder << ", review required";
  }
  return builder.str();
}

TrayPadlockIconState BuildMonitorPadlockIconState(
    const TrayMonitorState& monitor) {
  TrayPadlockIconState icon{
      .variant = monitor.locked ? "locked" : "unlocked",
      .accent = {},
      .filled = monitor.locked,
      .review_badge = monitor.requires_confirmation,
  };

  if (monitor.requires_confirmation) {
    icon.accent = "amber";
  } else if (monitor.locked) {
    icon.accent = "emerald";
  } else {
    icon.accent = "slate";
  }

  return icon;
}

std::string FormatMonitorResolution(const platform::MonitorDescriptor& monitor) {
  const int width = monitor.bounds.right - monitor.bounds.left;
  const int height = monitor.bounds.bottom - monitor.bounds.top;
  return std::to_string(width) + "x" + std::to_string(height);
}

std::string BuildMonitorMenuLabel(const TrayMonitorState& monitor) {
  std::ostringstream builder;
  builder << BuildMonitorDisplayLabel(monitor.monitor);
  builder << " (" << FormatMonitorResolution(monitor.monitor) << " @ "
          << monitor.monitor.bounds.left << "," << monitor.monitor.bounds.top;
  if (monitor.monitor.is_primary) {
    builder << ", primary";
  }
  builder << ")";
  if (monitor.requires_confirmation) {
    builder << " [review]";
  }
  return builder.str();
}

std::string BuildMonitorIdentifyLabel(const TrayMonitorState& monitor) {
  std::ostringstream builder;
  builder << "Hover highlights " << monitor.monitor.label << " on screen";
  if (!monitor.monitor.display_name.empty()) {
    builder << " (" << monitor.monitor.display_name << ", ";
  } else {
    builder << " (";
  }
  builder << FormatMonitorResolution(monitor.monitor) << " @ "
          << monitor.monitor.bounds.left << "," << monitor.monitor.bounds.top;
  if (monitor.monitor.is_primary) {
    builder << ", primary";
  }
  builder << ")";
  return builder.str();
}

TrayMenuHeader BuildTrayMenuHeader(const TrayMenuModel& model) {
  TrayMenuHeader header{
      .title = "Keep selected monitors pinned",
      .subtitle = {},
      .instruction =
          "Hover a monitor to identify it, then click to choose whether it "
          "stays pinned while other monitors follow Windows desktop "
          "switches.",
  };

  std::ostringstream subtitle;
  subtitle << model.monitors.size() << " visible";
  if (model.monitors.size() == 1U) {
    subtitle << " monitor";
  } else {
    subtitle << " monitors";
  }
  subtitle << " | " << model.locked_monitors << " locked";
  if (model.review_monitors > 0U) {
    subtitle << " | " << model.review_monitors << " need review";
  }
  header.subtitle = subtitle.str();
  return header;
}

TrayIconState BuildTrayIconState(const TrayMenuModel& model) {
  TrayIconState icon{
      .variant = "idle",
      .tooltip = "LockingGlass - No monitors detected",
      .accessibility_label = "No monitors detected",
      .review_badge = false,
  };

  if (model.monitors.empty()) {
    return icon;
  }

  if (model.review_monitors > 0U) {
    icon.variant = "review";
  } else if (model.locked_monitors == 0U) {
    icon.variant = "unlocked";
  } else if (model.locked_monitors == model.monitors.size()) {
    icon.variant = "locked";
  } else {
    icon.variant = "mixed";
  }

  std::ostringstream tooltip;
  tooltip << "LockingGlass - " << model.locked_monitors << " of "
          << model.monitors.size() << " locked";
  if (model.review_monitors > 0U) {
    tooltip << ", " << model.review_monitors << " need review";
  }
  icon.tooltip = tooltip.str();

  std::ostringstream accessibility_label;
  accessibility_label << model.locked_monitors << " locked, "
                      << (model.monitors.size() - model.locked_monitors)
                      << " unlocked";
  if (model.review_monitors > 0U) {
    accessibility_label << ", " << model.review_monitors
                        << " pending review";
    icon.review_badge = true;
  }
  icon.accessibility_label = accessibility_label.str();
  return icon;
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
      .header = {},
      .icon = {},
      .monitors = {},
      .locked_monitors = 0,
      .review_monitors = 0,
  };

  for (const auto& monitor_state : session.snapshot.monitors) {
    if (!monitor_state.is_present) {
      continue;
    }

    TrayMonitorState tray_monitor{
        .monitor = monitor_state.monitor,
        .locked = monitor_state.locked,
        .requires_confirmation = monitor_state.requires_confirmation,
        .padlock_icon = {},
        .status_label = {},
        .menu_label = {},
        .identify_label = {},
    };
    tray_monitor.status_label = BuildMonitorStatusLabel(tray_monitor);
    tray_monitor.padlock_icon = BuildMonitorPadlockIconState(tray_monitor);
    tray_monitor.menu_label = BuildMonitorMenuLabel(tray_monitor);
    tray_monitor.identify_label = BuildMonitorIdentifyLabel(tray_monitor);
    model.monitors.push_back(std::move(tray_monitor));
    if (monitor_state.locked) {
      ++model.locked_monitors;
    }
    if (monitor_state.requires_confirmation) {
      ++model.review_monitors;
    }
  }

  model.header = BuildTrayMenuHeader(model);
  model.icon = BuildTrayIconState(model);
  return model;
}

TrayIdentifyOverlay BuildTrayIdentifyOverlay(const TrayMonitorState& monitor) {
  TrayIdentifyOverlay overlay{
      .visible = true,
      .monitor = monitor.monitor,
      .title = monitor.monitor.label,
      .message = {},
  };

  std::ostringstream message;
  if (!monitor.monitor.display_name.empty()) {
    message << monitor.monitor.display_name << " | ";
  }
  message << FormatMonitorResolution(monitor.monitor) << " | "
          << monitor.status_label << " | top-left "
          << monitor.monitor.bounds.left << "," << monitor.monitor.bounds.top;
  if (monitor.monitor.is_primary) {
    message << " | primary";
  }
  overlay.message = message.str();
  return overlay;
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
  if (!monitor.menu_label.empty()) {
    return monitor.menu_label;
  }
  return BuildMonitorMenuLabel(monitor);
}

std::string FormatTrayMenuModel(const TrayMenuModel& model) {
  std::ostringstream builder;
  builder << "LockingGlass tray menu\n";
  builder << "Trigger:\n";
  builder << "  - source: " << model.trigger << '\n';
  builder << "Header:\n";
  builder << "  - title: " << model.header.title << '\n';
  builder << "  - subtitle: " << model.header.subtitle << '\n';
  builder << "  - instruction: " << model.header.instruction << '\n';
  builder << "Icon:\n";
  builder << "  - variant: " << model.icon.variant << '\n';
  builder << "  - tooltip: " << model.icon.tooltip << '\n';
  builder << "  - review badge: "
          << (model.icon.review_badge ? "yes" : "no") << '\n';
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
            << monitor.status_label << " | padlock: "
            << monitor.padlock_icon.variant << ", "
            << monitor.padlock_icon.accent << ", "
            << (monitor.padlock_icon.filled ? "filled" : "outline");
    if (monitor.padlock_icon.review_badge) {
      builder << ", review badge";
    }
    builder << '\n';
    builder << "    " << monitor.identify_label << '\n';
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
