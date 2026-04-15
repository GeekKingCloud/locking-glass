#pragma once

#include <string>
#include <vector>

#include "locking_glass/core/session_store.h"

namespace locking_glass::core {

struct TrayMonitorState {
  platform::MonitorDescriptor monitor;
  bool locked = false;
  bool requires_confirmation = false;
};

struct TrayMenuModel {
  std::string trigger;
  std::vector<TrayMonitorState> monitors;
  std::size_t locked_monitors = 0;
  std::size_t review_monitors = 0;
};

TrayMenuModel BuildTrayMenuModel(const SessionRefreshResult& session,
                                 std::string trigger);
std::string BuildTrayMonitorLabel(const TrayMonitorState& monitor);
std::string FormatTrayMenuModel(const TrayMenuModel& model);
bool ToggleMonitorLock(const SessionStore& store, SessionSnapshot* snapshot,
                       const platform::MonitorDescriptor& monitor,
                       bool* locked_after = nullptr);

}  // namespace locking_glass::core
