#pragma once

#include <string>
#include <vector>

#include "locking_glass/core/session_store.h"

namespace locking_glass::core {

struct DesktopWindow {
  std::string window_id;
  std::string title;
  std::string monitor_id;
  std::string monitor_label;
  std::string desktop_id;
  bool is_top_level = true;
  bool can_move = true;
};

struct DesktopSwitchScenario {
  std::string trigger;
  std::string source_desktop_id;
  std::string target_desktop_id;
  std::string staging_desktop_id;
  std::vector<platform::MonitorDescriptor> monitors;
  std::vector<DesktopWindow> windows;
};

struct MonitorLockingMove {
  DesktopWindow window;
  std::string from_desktop_id;
  std::string to_desktop_id;
};

struct MonitorLockingSkip {
  DesktopWindow window;
  std::string reason;
};

struct MonitorLockingPlan {
  std::string trigger;
  SessionRefreshResult session;
  std::string source_desktop_id;
  std::string target_desktop_id;
  std::string staging_desktop_id;
  std::vector<std::string> locked_monitors;
  std::vector<MonitorLockingMove> moves;
  std::vector<MonitorLockingSkip> skipped_windows;
};

MonitorLockingPlan BuildMonitorLockingPlan(
    const SessionStore& store, const DesktopSwitchScenario& scenario);
std::string FormatMonitorLockingPlan(const MonitorLockingPlan& plan);

}  // namespace locking_glass::core
