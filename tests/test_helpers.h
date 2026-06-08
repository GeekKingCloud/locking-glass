#pragma once

#include "locking_glass/core/runtime.h"
#include "locking_glass/core/session_store.h"
#include "locking_glass/core/tray_ui.h"
#include "locking_glass/integration/autostart.h"
#include "locking_glass/platform/background_session.h"
#include "locking_glass/platform/window_return_tracker.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace locking_glass::tests {

bool Expect(bool condition, const std::string& message);
void RunCheckGroup(std::string_view label, bool (*check)(), int* failures);
void ClearLockingGlassTestEnvironment();

const integration::CapabilityReport* FindCapability(
    const core::StartupDiagnostics& diagnostics, const std::string& name);
void SetEnvironmentVariable(const std::string& name, const std::string& value);
std::filesystem::path MakeTempDirectory();
void WriteTextFile(const std::filesystem::path& path, const std::string& contents);
std::string ReadTextFile(const std::filesystem::path& path);

platform::MonitorDescriptor MakeMonitor(
    const std::string& stable_id, const std::string& device_path,
    const std::string& edid_serial, const std::string& display_name,
    const std::string& label, int left, int top, int right, int bottom,
    bool is_primary);
const core::SessionMonitorState* FindMonitorState(
    const core::SessionSnapshot& snapshot,
    const platform::MonitorDescriptor& monitor);
const platform::BackgroundSessionMenuItem* FindBackgroundMonitor(
    const platform::BackgroundSessionEvent& event, const std::string& label);
const integration::WindowMoveResult* FindMoveResult(
    const integration::DesktopSwitchReport& report, const std::string& window_id);
const integration::WindowMoveResult* FindUnlockMoveResult(
    const integration::UnlockReturnReport& report, const std::string& window_id);
const integration::UnlockReturnSkip* FindUnlockSkip(
    const integration::UnlockReturnReport& report, const std::string& window_id);
const core::DesktopWindow* FindDesktopWindow(
    const std::vector<core::DesktopWindow>& windows, const std::string& window_id);
integration::DesktopIdentity MakeDesktopIdentity(
    int number, std::string guid, std::string name);
const platform::MonitorDescriptor* FindPromptMonitor(
    const platform::BackgroundSessionPrompt& prompt, const std::string& label);
bool HighlightTargets(const platform::BackgroundSessionEvent& event,
                      const std::string& label);
void ExpectWindowsAwareCapability(const core::StartupDiagnostics& diagnostics,
                                  const std::string& component, int* failures);

bool RunSessionStoreChecks();
bool RunMonitorWatchChecks();
bool RunTraySessionChecks();
bool RunBackgroundControllerStatusChecks();
bool RunUnlockReturnChecks();
bool RunDesktopLockingChecks();
bool RunWindowsLiveDesktopWatchChecks();

}  // namespace locking_glass::tests
