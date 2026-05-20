#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "locking_glass/integration/capability.h"
#include "locking_glass/platform/monitor_gateway.h"

namespace locking_glass::platform {

struct BackgroundSessionMenuItem {
  MonitorDescriptor monitor;
  bool locked = false;
  bool requires_confirmation = false;
  std::string padlock_variant;
  std::string padlock_accent;
  bool padlock_filled = false;
  bool padlock_review_badge = false;
  std::string status_label;
  std::string menu_label;
  std::string identify_label;
};

struct BackgroundSessionPrompt {
  bool visible = false;
  std::string title;
  std::string message;
  std::vector<MonitorDescriptor> monitors;
};

struct BackgroundSessionHighlight {
  bool visible = false;
  MonitorDescriptor monitor;
  std::string title;
  std::string message;
};

struct BackgroundSessionUnlockReturn {
  bool attempted = false;
  std::size_t moved_windows = 0;
  std::size_t skipped_windows = 0;
  std::size_t failed_windows = 0;
  std::string detail;
};

struct BackgroundSessionEvent {
  std::string trigger;
  bool tray_menu_visible = false;
  std::string menu_title;
  std::string menu_subtitle;
  std::string menu_instruction;
  std::string tray_icon_variant;
  std::string tray_icon_tooltip;
  bool tray_icon_review_badge = false;
  bool live_controller_available = true;
  bool live_controller_watcher_started = false;
  std::string live_controller_detail;
  std::vector<BackgroundSessionMenuItem> monitors;
  BackgroundSessionPrompt prompt;
  BackgroundSessionHighlight highlight;
  BackgroundSessionUnlockReturn unlock_return;
};

using BackgroundSessionObserver =
    std::function<void(const BackgroundSessionEvent&)>;

class BackgroundSession {
 public:
  virtual ~BackgroundSession() = default;
  virtual locking_glass::integration::CapabilityReport Probe() const = 0;
  // Observation is for scripted tests and proof logs. Runtime behavior must not
  // depend on an observer being installed.
  virtual int Run(
      const BackgroundSessionObserver& observer = BackgroundSessionObserver{}) const = 0;
};

std::unique_ptr<BackgroundSession> CreateBackgroundSession();

}  // namespace locking_glass::platform
