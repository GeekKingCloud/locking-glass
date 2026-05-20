#include "background_session_internal.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace locking_glass::platform::internal {

namespace {

constexpr char kBackgroundControllerStatusOverrideEnv[] =
    "LOCKING_GLASS_BACKGROUND_CONTROLLER_STATUS";

BackgroundSessionPrompt BuildBackgroundPrompt(
    const core::MonitorReviewPrompt& prompt) {
  return BackgroundSessionPrompt{
      .visible = prompt.visible,
      .title = prompt.title,
      .message = prompt.message,
      .monitors = prompt.monitors,
  };
}

BackgroundSessionHighlight BuildBackgroundHighlight(
    const core::TrayIdentifyOverlay& overlay) {
  return BackgroundSessionHighlight{
      .visible = overlay.visible,
      .monitor = overlay.monitor,
      .title = overlay.title,
      .message = overlay.message,
  };
}

std::size_t CountUnlockReturnFailures(
    const locking_glass::integration::UnlockReturnReport& report) {
  std::size_t failures = 0;
  for (const auto& move_result : report.move_results) {
    if (!move_result.success) {
      ++failures;
    }
  }
  return failures;
}

BackgroundSessionUnlockReturn BuildUnlockReturnSummary(
    const locking_glass::integration::UnlockReturnReport& report) {
  const std::size_t failed_windows = CountUnlockReturnFailures(report);
  if (report.move_results.empty() && report.skipped_windows.empty()) {
    return {};
  }

  BackgroundSessionUnlockReturn summary{
      .attempted = true,
      .moved_windows = report.move_results.size() - failed_windows,
      .skipped_windows = report.skipped_windows.size(),
      .failed_windows = failed_windows,
      .detail = {},
  };

  std::ostringstream detail;
  detail << "Returned " << summary.moved_windows << " window";
  if (summary.moved_windows != 1U) {
    detail << 's';
  }
  if (summary.skipped_windows > 0U || summary.failed_windows > 0U) {
    detail << "; " << summary.skipped_windows << " skipped, "
           << summary.failed_windows << " failed";
  }
  detail << '.';
  summary.detail = detail.str();
  return summary;
}

bool ShouldRetrySkippedUnlockReturn(
    const locking_glass::integration::UnlockReturnSkip& skipped_window) {
  return skipped_window.reason !=
         "window is already on its remembered desktop";
}

std::vector<locking_glass::integration::TrackedWindowReturn>
SelectRetryableTrackedWindows(
    const locking_glass::integration::UnlockReturnReport& report,
    const std::vector<locking_glass::integration::TrackedWindowReturn>&
        tracked_windows) {
  std::vector<locking_glass::integration::TrackedWindowReturn> retryable;
  for (const auto& tracked_window : tracked_windows) {
    const std::string& window_id = tracked_window.window.window_id;
    bool should_retry = false;
    for (const auto& move_result : report.move_results) {
      if (move_result.window.window_id == window_id && !move_result.success) {
        should_retry = true;
        break;
      }
    }
    for (const auto& skipped_window : report.skipped_windows) {
      if (skipped_window.window.window_id == window_id &&
          ShouldRetrySkippedUnlockReturn(skipped_window)) {
        should_retry = true;
        break;
      }
    }
    if (should_retry) {
      retryable.push_back(tracked_window);
    }
  }
  return retryable;
}

}  // namespace

locking_glass::integration::CapabilityReport MakeReadyControllerCapability() {
  return locking_glass::integration::CapabilityReport{
      .component = "desktop-locking",
      .status = locking_glass::integration::CapabilityStatus::kReady,
      .detail =
          "The live desktop controller is active for the background tray session.",
  };
}

locking_glass::integration::CapabilityReport MakeUnavailableControllerCapability(
    std::string detail) {
  if (detail.empty()) {
    detail =
        "The live desktop controller is unavailable for the background tray "
        "session.";
  }

  return locking_glass::integration::CapabilityReport{
      .component = "desktop-locking",
      .status = locking_glass::integration::CapabilityStatus::kUnavailable,
      .detail = std::move(detail),
  };
}

std::optional<locking_glass::integration::CapabilityReport>
ResolveBackgroundControllerCapabilityOverride() {
  const char* raw_value = std::getenv(kBackgroundControllerStatusOverrideEnv);
  if (raw_value == nullptr || raw_value[0] == '\0') {
    return std::nullopt;
  }

  const std::string value(raw_value);
  if (value == "ready") {
    return MakeReadyControllerCapability();
  }

  constexpr char kUnavailablePrefix[] = "unavailable:";
  if (value == "unavailable") {
    return MakeUnavailableControllerCapability({});
  }
  if (value.rfind(kUnavailablePrefix, 0) == 0) {
    return MakeUnavailableControllerCapability(
        value.substr(sizeof(kUnavailablePrefix) - 1U));
  }

  return MakeUnavailableControllerCapability(
      "Background-session controller override: " + value);
}

bool IsLiveControllerAvailable(
    const locking_glass::integration::CapabilityReport& capability) {
  return capability.status ==
         locking_glass::integration::CapabilityStatus::kReady;
}

bool SessionStateMatchesWindowMonitor(
    const locking_glass::core::SessionMonitorState& monitor_state,
    const locking_glass::core::DesktopWindow& window) {
  if (!window.monitor_id.empty() &&
      (window.monitor_id == monitor_state.monitor.stable_id ||
       window.monitor_id == monitor_state.monitor.device_path ||
       window.monitor_id == monitor_state.monitor.label)) {
    return true;
  }

  return !window.monitor_label.empty() &&
         window.monitor_label == monitor_state.monitor.label;
}

bool IsWindowMonitorCurrentlyLocked(
    const core::SessionStore& session_store,
    const locking_glass::core::DesktopWindow& window) {
  const auto loaded = session_store.Load();
  for (const auto& monitor_state : loaded.snapshot.monitors) {
    if (!monitor_state.locked) {
      continue;
    }
    if (SessionStateMatchesWindowMonitor(monitor_state, window)) {
      return true;
    }
  }
  return false;
}

BackgroundSessionUnlockReturn RunUnlockReturn(
    const locking_glass::integration::CapabilityReport&
        live_controller_capability,
    locking_glass::integration::VirtualDesktopController*
        unlock_return_controller,
    const std::shared_ptr<WindowReturnTracker>& window_return_tracker,
    const MonitorDescriptor& monitor) {
  if (window_return_tracker == nullptr) {
    return {};
  }

  const std::string monitor_key = BuildTrackedMonitorKey(monitor);
  const auto tracked_windows = window_return_tracker->ConsumeMonitor(monitor_key);
  if (tracked_windows.empty()) {
    return {};
  }

  locking_glass::integration::UnlockReturnReport report{
      .monitor = monitor,
      .move_results = {},
      .skipped_windows = {},
      .resulting_windows = {},
  };
  if (!IsLiveControllerAvailable(live_controller_capability) ||
      unlock_return_controller == nullptr) {
    for (const auto& tracked_window : tracked_windows) {
      report.move_results.push_back(locking_glass::integration::WindowMoveResult{
          .window = tracked_window.window,
          .from_desktop = {},
          .to_desktop = tracked_window.home_desktop,
          .from_desktop_id = tracked_window.window.desktop_id,
          .to_desktop_id =
              locking_glass::integration::FormatDesktopIdentity(
                  tracked_window.home_desktop),
          .success = false,
          .detail = "live desktop control unavailable during unlock return",
      });
    }
  } else {
    report = unlock_return_controller->ReturnTrackedWindows(
        locking_glass::integration::UnlockReturnRequest{
            .monitor = monitor,
            .tracked_windows = tracked_windows,
        });
  }

  const auto summary = BuildUnlockReturnSummary(report);
  const auto retryable_tracked_windows =
      SelectRetryableTrackedWindows(report, tracked_windows);
  if (summary.failed_windows > 0U || summary.skipped_windows > 0U) {
    window_return_tracker->RestoreMonitor(monitor_key,
                                          retryable_tracked_windows);
  }
  auto unlock_return = summary;
  unlock_return.retryable_windows = retryable_tracked_windows.size();
  if (summary.attempted) {
    std::cout << locking_glass::integration::FormatUnlockReturnReport(report)
              << std::flush;
  }
  return unlock_return;
}

BackgroundSessionEvent BuildSessionEvent(
    const core::TrayMenuModel& model,
    const locking_glass::integration::CapabilityReport&
        live_controller_capability,
    const bool live_controller_watcher_started,
    const bool tray_menu_visible,
    const core::MonitorReviewPrompt& prompt,
    const core::TrayIdentifyOverlay& highlight,
    const BackgroundSessionUnlockReturn& unlock_return) {
  BackgroundSessionEvent event{
      .trigger = model.trigger,
      .tray_menu_visible = tray_menu_visible,
      .menu_status = model.menu_status,
      .menu_instruction = model.menu_instruction,
      .tray_icon_variant = model.icon.variant,
      .tray_icon_tooltip = model.icon.tooltip,
      .tray_icon_review_badge = model.icon.review_badge,
      .live_controller_available =
          IsLiveControllerAvailable(live_controller_capability),
      .live_controller_watcher_started = live_controller_watcher_started,
      .live_controller_detail = live_controller_capability.detail,
      .monitors = {},
      .prompt = BuildBackgroundPrompt(prompt),
      .highlight = BuildBackgroundHighlight(highlight),
      .unlock_return = unlock_return,
  };
  for (const auto& monitor : model.monitors) {
    event.monitors.push_back(BackgroundSessionMenuItem{
        .monitor = monitor.monitor,
        .locked = monitor.locked,
        .requires_confirmation = monitor.requires_confirmation,
        .padlock_variant = monitor.padlock_icon.variant,
        .padlock_accent = monitor.padlock_icon.accent,
        .padlock_filled = monitor.padlock_icon.filled,
        .padlock_review_badge = monitor.padlock_icon.review_badge,
        .status_label = monitor.status_label,
        .menu_label = monitor.menu_label,
        .identify_label = monitor.identify_label,
    });
  }
  return event;
}

void PublishEvent(
    const BackgroundSessionObserver& observer,
    const core::TrayMenuModel& model,
    const locking_glass::integration::CapabilityReport&
        live_controller_capability,
    const bool live_controller_watcher_started,
    const bool tray_menu_visible,
    const core::MonitorReviewPrompt& prompt,
    const core::TrayIdentifyOverlay& highlight,
    const BackgroundSessionUnlockReturn& unlock_return) {
  if (observer) {
    observer(BuildSessionEvent(model, live_controller_capability,
                               live_controller_watcher_started,
                               tray_menu_visible, prompt, highlight,
                               unlock_return));
  }
}

}  // namespace locking_glass::platform::internal

namespace locking_glass::platform {

namespace {

class BackgroundSessionImpl final : public BackgroundSession {
 public:
  locking_glass::integration::CapabilityReport Probe() const override {
#if defined(_WIN32)
    return locking_glass::integration::CapabilityReport{
        .component = "background-session",
        .status = locking_glass::integration::CapabilityStatus::kReady,
        .detail =
            "Background startup enters a hidden Win32 message loop, renders a status-aware Shell_NotifyIcon tray icon, starts the live desktop watcher, and exits instead of accepting lock changes when live desktop control is unavailable.",
    };
#else
    return locking_glass::integration::CapabilityReport{
        .component = "background-session",
        .status = locking_glass::integration::CapabilityStatus::kStubbed,
        .detail =
            "Tray session is stubbed on non-Windows hosts; LOCKING_GLASS_TRAY_SCRIPT can still replay tray clicks, hover-identify and hover-clear overlay events, monitor toggles, and new-monitor confirmation prompts for local verification.",
    };
#endif
  }

  int Run(const BackgroundSessionObserver& observer) const override {
    if (const char* script_path = std::getenv("LOCKING_GLASS_TRAY_SCRIPT");
        script_path != nullptr && script_path[0] != '\0') {
      return internal::RunScriptedTraySession(observer);
    }

#if defined(_WIN32)
    return internal::RunWindowsTraySession(observer);
#else
    return internal::RunScriptedTraySession(observer);
#endif
  }
};

}  // namespace

std::unique_ptr<BackgroundSession> CreateBackgroundSession() {
  return std::make_unique<BackgroundSessionImpl>();
}

}  // namespace locking_glass::platform
