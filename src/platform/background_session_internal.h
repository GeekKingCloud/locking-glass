#pragma once

#include <memory>
#include <optional>
#include <string>

#include "locking_glass/core/session_store.h"
#include "locking_glass/core/tray_ui.h"
#include "locking_glass/integration/virtual_desktop_controller.h"
#include "locking_glass/platform/background_session.h"
#include "locking_glass/platform/window_return_tracker.h"

namespace locking_glass::platform::internal {

locking_glass::integration::CapabilityReport MakeReadyControllerCapability();
locking_glass::integration::CapabilityReport MakeUnavailableControllerCapability(
    std::string detail);
std::optional<locking_glass::integration::CapabilityReport>
ResolveBackgroundControllerCapabilityOverride();
bool IsLiveControllerAvailable(
    const locking_glass::integration::CapabilityReport& capability);

bool SessionStateMatchesWindowMonitor(
    const locking_glass::core::SessionMonitorState& monitor_state,
    const locking_glass::core::DesktopWindow& window);
bool IsWindowMonitorLockedInSession(
    const locking_glass::core::SessionRefreshResult& session,
    const locking_glass::core::DesktopWindow& window);

BackgroundSessionUnlockReturn RunUnlockReturn(
    const locking_glass::integration::CapabilityReport&
        live_controller_capability,
    locking_glass::integration::VirtualDesktopController*
        unlock_return_controller,
    const std::shared_ptr<WindowReturnTracker>& window_return_tracker,
    const MonitorDescriptor& monitor,
    bool allow_script_replay = true);
BackgroundSessionEvent BuildSessionEvent(
    const core::TrayMenuModel& model,
    const locking_glass::integration::CapabilityReport&
        live_controller_capability,
    bool live_controller_watcher_started, bool tray_menu_visible,
    const core::MonitorReviewPrompt& prompt = core::MonitorReviewPrompt{},
    const core::TrayIdentifyOverlay& highlight = core::TrayIdentifyOverlay{},
    const BackgroundSessionUnlockReturn& unlock_return =
        BackgroundSessionUnlockReturn{});
void PublishEvent(
    const BackgroundSessionObserver& observer,
    const core::TrayMenuModel& model,
    const locking_glass::integration::CapabilityReport&
        live_controller_capability,
    bool live_controller_watcher_started, bool tray_menu_visible,
    const core::MonitorReviewPrompt& prompt = core::MonitorReviewPrompt{},
    const core::TrayIdentifyOverlay& highlight = core::TrayIdentifyOverlay{},
    const BackgroundSessionUnlockReturn& unlock_return =
        BackgroundSessionUnlockReturn{});

int RunWindowsTraySession(const BackgroundSessionObserver& observer);
int RunScriptedTraySession(const BackgroundSessionObserver& observer);

}  // namespace locking_glass::platform::internal
