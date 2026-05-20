#include "background_session_internal.h"

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace locking_glass::platform::internal {

namespace {

std::vector<std::string> SplitFields(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t end = line.find('\t', start);
    if (end == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }

    fields.push_back(line.substr(start, end - start));
    start = end + 1;
  }
  return fields;
}

bool ParseBoolField(const std::string& field, bool* value) {
  if (field == "1" || field == "true") {
    *value = true;
    return true;
  }
  if (field == "0" || field == "false") {
    *value = false;
    return true;
  }
  return false;
}

bool ParseIntField(const std::string& field, int* value) {
  try {
    std::size_t consumed = 0;
    const int parsed = std::stoi(field, &consumed);
    if (consumed != field.size()) {
      return false;
    }
    *value = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

enum class TrayScriptStepType {
  kEvent,
  kClick,
  kHover,
  kHoverClear,
  kToggle,
  kDesktopWatch,
  kRefresh,
  kExit,
};

struct TrayScriptStep {
  TrayScriptStepType type = TrayScriptStepType::kEvent;
  std::string trigger;
  std::vector<MonitorDescriptor> monitors;
  std::string target;
};

std::vector<TrayScriptStep> LoadTrayScript(const std::string& script_path) {
  std::ifstream input(script_path);
  if (!input.is_open()) {
    return {};
  }

  std::vector<TrayScriptStep> steps;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const auto fields = SplitFields(line);
    if (fields.empty()) {
      continue;
    }

    if (fields[0] == "event" && fields.size() == 2U) {
      steps.push_back(TrayScriptStep{
          .type = TrayScriptStepType::kEvent,
          .trigger = fields[1],
          .monitors = {},
          .target = {},
      });
      continue;
    }

    if (fields[0] == "monitor" && fields.size() == 11U && !steps.empty() &&
        steps.back().type == TrayScriptStepType::kEvent) {
      bool is_primary = false;
      MonitorBounds bounds;
      if (!ParseIntField(fields[6], &bounds.left) ||
          !ParseIntField(fields[7], &bounds.top) ||
          !ParseIntField(fields[8], &bounds.right) ||
          !ParseIntField(fields[9], &bounds.bottom) ||
          !ParseBoolField(fields[10], &is_primary)) {
        return {};
      }

      steps.back().monitors.push_back(MonitorDescriptor{
          .stable_id = fields[1],
          .device_path = fields[2],
          .edid_serial = fields[3],
          .display_name = fields[4],
          .label = fields[5],
          .bounds = bounds,
          .is_primary = is_primary,
      });
      continue;
    }

    if (fields[0] == "action" && fields.size() >= 2U) {
      if (fields[1] == "click" && fields.size() == 2U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kClick,
            .trigger = {},
            .monitors = {},
            .target = {},
        });
        continue;
      }
      if (fields[1] == "hover" && fields.size() == 3U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kHover,
            .trigger = {},
            .monitors = {},
            .target = fields[2],
        });
        continue;
      }
      if (fields[1] == "hover-clear" && fields.size() == 2U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kHoverClear,
            .trigger = {},
            .monitors = {},
            .target = {},
        });
        continue;
      }
      if (fields[1] == "toggle" && fields.size() == 3U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kToggle,
            .trigger = {},
            .monitors = {},
            .target = fields[2],
        });
        continue;
      }
      if (fields[1] == "desktop-watch" && fields.size() == 2U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kDesktopWatch,
            .trigger = {},
            .monitors = {},
            .target = {},
        });
        continue;
      }
      if (fields[1] == "refresh" && fields.size() == 2U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kRefresh,
            .trigger = {},
            .monitors = {},
            .target = {},
        });
        continue;
      }
      if (fields[1] == "exit" && fields.size() == 2U) {
        steps.push_back(TrayScriptStep{
            .type = TrayScriptStepType::kExit,
            .trigger = {},
            .monitors = {},
            .target = {},
        });
        continue;
      }
    }

    return {};
  }

  return steps;
}

const platform::MonitorDescriptor* FindScriptMonitor(
    const core::TrayMenuModel& model, const std::string& target) {
  for (const auto& monitor : model.monitors) {
    if (monitor.monitor.label == target || monitor.monitor.stable_id == target ||
        monitor.monitor.device_path == target) {
      return &monitor.monitor;
    }
  }
  return nullptr;
}

core::TrayMenuModel BuildScriptTrayMenuModel(
    const core::SessionRefreshResult& session, std::string trigger,
    const locking_glass::integration::CapabilityReport&
        live_controller_capability,
    const bool live_controller_watcher_started) {
  auto model = core::BuildTrayMenuModel(session, std::move(trigger));
  ApplyLiveControllerStatus(live_controller_capability,
                            live_controller_watcher_started, &model);
  return model;
}

}  // namespace

int RunScriptedTraySession(const BackgroundSessionObserver& observer) {
  const char* script_path = std::getenv("LOCKING_GLASS_TRAY_SCRIPT");
  if (script_path == nullptr || script_path[0] == '\0') {
    return 0;
  }

  const auto steps = LoadTrayScript(script_path);
  if (steps.empty()) {
    return 1;
  }

  core::SessionStore session_store;
  core::SessionRefreshResult session;
  std::vector<MonitorDescriptor> current_monitors;
  bool has_session = false;
  bool started_unlocked = false;
  bool tray_menu_visible = false;
  auto unlock_return_controller =
      locking_glass::integration::CreateVirtualDesktopController();
  auto window_return_tracker = std::make_shared<WindowReturnTracker>();
  const auto live_controller_capability =
      ResolveBackgroundControllerCapabilityOverride().value_or(
          MakeReadyControllerCapability());
  const bool live_controller_watcher_started =
      IsLiveControllerAvailable(live_controller_capability);

  for (const auto& step : steps) {
    switch (step.type) {
      case TrayScriptStepType::kEvent:
        current_monitors = step.monitors;
        session = started_unlocked ? session_store.Restore(current_monitors)
                                   : session_store.StartUnlocked(current_monitors);
        started_unlocked = true;
        has_session = true;
        tray_menu_visible = false;
        PublishEvent(observer,
                     BuildScriptTrayMenuModel(
                         session, step.trigger, live_controller_capability,
                         live_controller_watcher_started),
                     live_controller_capability,
                     live_controller_watcher_started, false,
                     core::BuildMonitorReviewPrompt(session));
        break;
      case TrayScriptStepType::kClick:
        if (!has_session) {
          session = session_store.Restore(current_monitors);
          has_session = true;
        }
        tray_menu_visible = true;
        PublishEvent(observer,
                     BuildScriptTrayMenuModel(
                         session, "tray-click", live_controller_capability,
                         live_controller_watcher_started),
                     live_controller_capability,
                     live_controller_watcher_started, true);
        break;
      case TrayScriptStepType::kHover: {
        if (!has_session || !tray_menu_visible) {
          return 1;
        }
        const auto model = BuildScriptTrayMenuModel(
            session, "tray-hover", live_controller_capability,
            live_controller_watcher_started);
        const auto* monitor = FindScriptMonitor(model, step.target);
        if (monitor == nullptr) {
          return 1;
        }

        bool published = false;
        for (const auto& menu_monitor : model.monitors) {
          if (menu_monitor.monitor.label == monitor->label &&
              menu_monitor.monitor.stable_id == monitor->stable_id &&
              menu_monitor.monitor.device_path == monitor->device_path) {
            PublishEvent(observer, model, live_controller_capability,
                         live_controller_watcher_started, true,
                         core::MonitorReviewPrompt{},
                         core::BuildTrayIdentifyOverlay(menu_monitor));
            published = true;
            break;
          }
        }
        if (!published) {
          return 1;
        }
        break;
      }
      case TrayScriptStepType::kHoverClear:
        if (!has_session || !tray_menu_visible) {
          return 1;
        }
        PublishEvent(observer,
                     BuildScriptTrayMenuModel(
                         session, "tray-hover-clear", live_controller_capability,
                         live_controller_watcher_started),
                     live_controller_capability,
                     live_controller_watcher_started, true);
        break;
      case TrayScriptStepType::kToggle: {
        if (!has_session) {
          session = session_store.Restore(current_monitors);
          has_session = true;
        }
        const auto model = BuildScriptTrayMenuModel(
            session, "tray-click", live_controller_capability,
            live_controller_watcher_started);
        const auto* monitor = FindScriptMonitor(model, step.target);
        bool locked_after = false;
        if (monitor == nullptr ||
            !core::ToggleMonitorLock(session_store, &session.snapshot, *monitor,
                                     &locked_after)) {
          return 1;
        }
        if (window_return_tracker != nullptr && locked_after) {
          window_return_tracker->ClearMonitor(BuildTrackedMonitorKey(*monitor));
        }
        const auto unlock_return =
            !locked_after
                ? RunUnlockReturn(live_controller_capability,
                                  unlock_return_controller.get(),
                                  window_return_tracker, *monitor)
                : BackgroundSessionUnlockReturn{};
        session = session_store.Preview(current_monitors);
        tray_menu_visible = true;
        PublishEvent(observer,
                     BuildScriptTrayMenuModel(
                         session, "tray-toggle", live_controller_capability,
                         live_controller_watcher_started),
                     live_controller_capability,
                     live_controller_watcher_started, true,
                     core::MonitorReviewPrompt{}, core::TrayIdentifyOverlay{},
                     unlock_return);
        break;
      }
      case TrayScriptStepType::kDesktopWatch:
        if (unlock_return_controller == nullptr) {
          return 1;
        }
        if (unlock_return_controller->WatchSwitches(
                session_store,
                [session_store, window_return_tracker](
                    const locking_glass::integration::DesktopSwitchReport&
                        report) {
                  if (window_return_tracker != nullptr) {
                    window_return_tracker->RecordSuccessfulMoves(
                        report, [session_store](
                                    const locking_glass::core::DesktopWindow&
                                        window) {
                          return IsWindowMonitorCurrentlyLocked(session_store,
                                                                window);
                        });
                  }
                  return true;
                }) != 0) {
          return 1;
        }
        session = session_store.Restore(current_monitors);
        has_session = true;
        break;
      case TrayScriptStepType::kRefresh:
        session = session_store.Restore(current_monitors);
        has_session = true;
        tray_menu_visible = false;
        PublishEvent(observer,
                     BuildScriptTrayMenuModel(
                         session, "tray-refresh", live_controller_capability,
                         live_controller_watcher_started),
                     live_controller_capability,
                     live_controller_watcher_started, false,
                     core::BuildMonitorReviewPrompt(session));
        break;
      case TrayScriptStepType::kExit:
        tray_menu_visible = false;
        PublishEvent(observer,
                     BuildScriptTrayMenuModel(
                         session, "exit", live_controller_capability,
                         live_controller_watcher_started),
                     live_controller_capability,
                     live_controller_watcher_started, false);
        return 0;
    }
  }

  return 0;
}

}  // namespace locking_glass::platform::internal
