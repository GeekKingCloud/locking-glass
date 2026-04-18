#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "locking_glass/core/session_store.h"
#include "locking_glass/integration/virtual_desktop_controller.h"
#include "locking_glass/platform/monitor_gateway.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace locking_glass::integration::internal {

std::vector<std::string> SplitFields(const std::string& line);
bool ParseBoolField(const std::string& field, bool* value);
bool ParseIntField(const std::string& field, int* value);

std::string BuildDesktopDisplayId(int desktop_number,
                                  const std::string& desktop_guid,
                                  const std::string& desktop_name);
DesktopIdentity MakeDesktopIdentity(int desktop_number,
                                    std::string desktop_guid,
                                    std::string desktop_name);
DesktopIdentity MakeDisplayOnlyDesktopIdentity(std::string display_id);
bool DesktopIdentityEquals(const DesktopIdentity& left,
                           const DesktopIdentity& right);
const DesktopIdentity* FindMatchingDesktop(
    const std::vector<DesktopIdentity>& desktops,
    const DesktopIdentity& remembered_desktop);

std::string DescribeWindow(const core::DesktopWindow& window);
std::string DescribeMonitor(const core::DesktopWindow& window);
bool WindowMatchesMonitor(const core::DesktopWindow& window,
                          const platform::MonitorDescriptor& monitor);

#if defined(_WIN32)
using NativeWindowHandle = HWND;
#else
using NativeWindowHandle = void*;
#endif

struct CapturedWindow {
  core::DesktopWindow window;
  DesktopIdentity desktop;
  NativeWindowHandle handle = nullptr;
  std::string move_block_reason;
  std::string forced_failure_detail;
  std::string override_skip_reason;
  std::optional<core::MonitorLockingSkip> extra_skip;
};

struct DesktopReturnReplayState {
  std::vector<DesktopIdentity> desktops;
  std::vector<CapturedWindow> windows;
};

const CapturedWindow* FindCapturedWindow(
    const std::vector<CapturedWindow>& windows, const std::string& window_id);
core::DesktopWindow* FindReturnResultWindow(
    std::vector<core::DesktopWindow>* windows, const std::string& window_id);
core::DesktopWindow* FindResultWindow(
    std::vector<core::DesktopWindow>* windows,
    const core::MonitorLockingMove& move);

UnlockReturnReport BuildUnlockReturnReport(
    const UnlockReturnRequest& request,
    const std::vector<DesktopIdentity>& available_desktops,
    const std::vector<CapturedWindow>& current_windows,
    const std::function<WindowMoveResult(const CapturedWindow&,
                                         const DesktopIdentity&)>& attempt_move);

DesktopReturnReplayState LoadDesktopReturnScript(const std::string& script_path);
std::vector<core::DesktopSwitchScenario> LoadDesktopScript(
    const std::string& script_path);
DesktopSwitchReport BuildDesktopSwitchReport(
    const core::SessionStore& store,
    const core::DesktopSwitchScenario& scenario);
UnlockReturnReport BuildScriptedUnlockReturnReport(
    const UnlockReturnRequest& request,
    const DesktopReturnReplayState& replay_state);

#if defined(_WIN32)
CapabilityReport ProbeWindowsController();
UnlockReturnReport BuildWindowsUnlockReturnReport(
    const UnlockReturnRequest& request);
int WatchWindowsLiveSwitches(const core::SessionStore& store,
                             const DesktopSwitchCallback& callback,
                             const DesktopWatchOptions& options);
#endif

}  // namespace locking_glass::integration::internal
