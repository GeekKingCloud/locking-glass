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
};

struct BackgroundSessionPrompt {
  bool visible = false;
  std::string title;
  std::string message;
  std::vector<MonitorDescriptor> monitors;
};

struct BackgroundSessionEvent {
  std::string trigger;
  bool tray_menu_visible = false;
  std::vector<BackgroundSessionMenuItem> monitors;
  BackgroundSessionPrompt prompt;
};

using BackgroundSessionObserver =
    std::function<void(const BackgroundSessionEvent&)>;

class BackgroundSession {
 public:
  virtual ~BackgroundSession() = default;
  virtual locking_glass::integration::CapabilityReport Probe() const = 0;
  virtual int Run(
      const BackgroundSessionObserver& observer = BackgroundSessionObserver{}) const = 0;
};

std::unique_ptr<BackgroundSession> CreateBackgroundSession();

}  // namespace locking_glass::platform
