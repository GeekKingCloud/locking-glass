#pragma once

#include <string>
#include <vector>

#include "locking_glass/core/session_store.h"

namespace locking_glass::core {

struct TrayMenuHeader {
  std::string title;
  std::string subtitle;
  std::string instruction;
};

struct TrayIconState {
  std::string variant;
  std::string tooltip;
  std::string accessibility_label;
  bool review_badge = false;
};

struct TrayPadlockIconState {
  std::string variant;
  std::string accent;
  bool filled = false;
  bool review_badge = false;
};

struct TrayMonitorState {
  platform::MonitorDescriptor monitor;
  bool locked = false;
  bool requires_confirmation = false;
  TrayPadlockIconState padlock_icon;
  std::string status_label;
  std::string menu_label;
  std::string identify_label;
};

struct TrayMenuModel {
  std::string trigger;
  TrayMenuHeader header;
  TrayIconState icon;
  std::vector<TrayMonitorState> monitors;
  std::size_t locked_monitors = 0;
  std::size_t review_monitors = 0;
};

struct TrayIdentifyOverlay {
  bool visible = false;
  platform::MonitorDescriptor monitor;
  std::string title;
  std::string message;
};

struct MonitorReviewPrompt {
  bool visible = false;
  std::string title;
  std::string message;
  std::vector<platform::MonitorDescriptor> monitors;
};

TrayMenuModel BuildTrayMenuModel(const SessionRefreshResult& session,
                                 std::string trigger);
TrayIdentifyOverlay BuildTrayIdentifyOverlay(const TrayMonitorState& monitor);
MonitorReviewPrompt BuildMonitorReviewPrompt(
    const SessionRefreshResult& session);
std::string BuildTrayMonitorLabel(const TrayMonitorState& monitor);
std::string FormatTrayMenuModel(const TrayMenuModel& model);
bool ToggleMonitorLock(const SessionStore& store, SessionSnapshot* snapshot,
                       const platform::MonitorDescriptor& monitor,
                       bool* locked_after = nullptr);

}  // namespace locking_glass::core
