#include "test_helpers.h"

// Single-executable runner for core policy plus scripted platform seams.
// Scripted replay proves wiring and invariants, not live Windows desktop hooks.

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace locking_glass::tests;

int main() {
  int failures = 0;
  ClearLockingGlassTestEnvironment();

  const auto temp_directory = MakeTempDirectory();
  const auto diagnostics_session_path = temp_directory / "diagnostics-session-state.tsv";
  SetEnvironmentVariable("LOCKING_GLASS_SESSION_PATH",
                         diagnostics_session_path.string());

  const auto runtime = locking_glass::core::BuildRuntime();
  const std::string test_install_path =
      "C:\\Program Files\\Locking Glass\\Locking Glass.exe";
  const auto diagnostics =
      locking_glass::core::CollectStartupDiagnostics(runtime, test_install_path);
  const auto formatted = locking_glass::core::FormatDiagnostics(diagnostics);
  const auto prototype_monitors = std::vector<locking_glass::platform::MonitorDescriptor>{
      MakeMonitor("\\\\.\\DISPLAY1", "DISPLAY#LEFT", "SERIAL-LEFT",
                  "Dell U2720Q", "Display 1", 0, 0, 2560, 1440, true),
      MakeMonitor("\\\\.\\DISPLAY2", "DISPLAY#RIGHT", "SERIAL-RIGHT",
                  "Dell U2720Q", "Display 2", 2560, 0, 5120, 1440, false),
  };
  const auto windows_api_prototype =
      runtime.windows_api_probe->BuildPrototype(prototype_monitors);
  const auto formatted_prototype =
      locking_glass::integration::FormatWindowsApiPrototype(
          windows_api_prototype);

  failures += !Expect(diagnostics.capabilities.size() == 5U,
                      "startup diagnostics should expose exactly five capability probes");
  failures += !Expect(windows_api_prototype.boundaries.size() == 2U,
                      "windows API prototype should define the virtual desktop and monitor boundaries");
  failures += !Expect(formatted.find("background-session") != std::string::npos,
                      "formatted diagnostics should mention the background session capability");
  failures += !Expect(formatted.find("desktop-locking") != std::string::npos,
                      "formatted diagnostics should mention the desktop locking capability");
  failures += !Expect(formatted.find("windows-api") != std::string::npos,
                      "formatted diagnostics should mention the Windows API probe");
  failures += !Expect(formatted.find("autostart") != std::string::npos,
                      "formatted diagnostics should mention the autostart capability");
  failures += !Expect(formatted.find("session-store") != std::string::npos,
                      "formatted diagnostics should mention the session store capability");
  failures += !Expect(formatted.find("Session:") != std::string::npos,
                      "formatted diagnostics should include a session summary section");
  failures += !Expect(
      formatted.find("\"C:\\Program Files\\Locking Glass\\Locking Glass.exe\" --background") !=
          std::string::npos,
      "formatted diagnostics should expose the quoted Windows autostart command");
  failures += !Expect(
      formatted_prototype.find("virtual-desktop-control") != std::string::npos,
      "formatted prototype should name the virtual desktop boundary");
  failures += !Expect(
      formatted_prototype.find("monitor-enumeration") != std::string::npos,
      "formatted prototype should name the monitor enumeration boundary");
  failures += !Expect(formatted_prototype.find("IVirtualDesktopManager") !=
                          std::string::npos,
                      "formatted prototype should document the supported virtual desktop COM API");
  failures += !Expect(formatted_prototype.find("VirtualDesktopAccessor") !=
                          std::string::npos,
                      "formatted prototype should document the helper seam for missing desktop controls");
  failures += !Expect(
      formatted_prototype.find("EnumDisplayMonitors") != std::string::npos,
      "formatted prototype should document Win32 monitor enumeration");
  failures += !Expect(
      formatted_prototype.find("QueryDisplayConfig") != std::string::npos,
      "formatted prototype should document persistent monitor identity lookup");
  failures += !Expect(formatted_prototype.find("WM_DISPLAYCHANGE") !=
                          std::string::npos,
                      "formatted prototype should document topology refresh behavior");
  failures += !Expect(
      formatted_prototype.find("Observed 2 active monitors") != std::string::npos,
      "formatted prototype should include the observed monitor count");
  failures += !Expect(diagnostics.session.storage_path == diagnostics_session_path,
                      "startup diagnostics should use the configured session storage path");
  failures += !Expect(!diagnostics.session.loaded_from_disk,
                      "startup diagnostics should report an empty temp session on first load");
  failures += !Expect(
      diagnostics.session.storage_issue ==
          locking_glass::core::SessionStorageIssue::kNone,
      "startup diagnostics should not report a storage issue for a missing session file");
  failures += !Expect(
      diagnostics.session.storage_detail.find("No saved session file found") !=
          std::string::npos,
      "startup diagnostics should explain when the session store is bootstrapping");
  failures += !Expect(formatted.find("storage issue: none") != std::string::npos,
                      "formatted diagnostics should show the persistence issue state");

  ExpectWindowsAwareCapability(diagnostics, "background-session", &failures);
  ExpectWindowsAwareCapability(diagnostics, "desktop-locking", &failures);
  ExpectWindowsAwareCapability(diagnostics, "windows-api", &failures);
  ExpectWindowsAwareCapability(diagnostics, "autostart", &failures);

  const auto* session_store = FindCapability(diagnostics, "session-store");
  failures += !Expect(session_store != nullptr, "session-store capability should exist");
  if (session_store != nullptr) {
    failures += !Expect(session_store->status ==
                            locking_glass::integration::CapabilityStatus::kReady,
                        "session-store should be ready on the host build");
    failures += !Expect(session_store->detail.find(
                            diagnostics_session_path.string()) != std::string::npos,
                        "session-store capability should report the session file path");
  }

  const auto quoted_path =
      locking_glass::integration::QuoteWindowsCommandArg(test_install_path);
  failures += !Expect(
      quoted_path == "\"C:\\Program Files\\Locking Glass\\Locking Glass.exe\"",
      "QuoteWindowsCommandArg should quote Windows paths that contain spaces");
  failures += !Expect(diagnostics.autostart.entry_name == "Locking Glass",
                      "autostart diagnostics should use the Locking Glass Run entry name");
  failures += !Expect(
      diagnostics.autostart.launch_command ==
          "\"C:\\Program Files\\Locking Glass\\Locking Glass.exe\" --background",
      "autostart diagnostics should launch the executable in background mode");
  RunCheckGroup("session store", RunSessionStoreChecks, &failures);
  RunCheckGroup("monitor watch", RunMonitorWatchChecks, &failures);
  RunCheckGroup("tray session", RunTraySessionChecks, &failures);
  RunCheckGroup("background controller status",
                RunBackgroundControllerStatusChecks, &failures);
  RunCheckGroup("unlock return", RunUnlockReturnChecks, &failures);
  RunCheckGroup("desktop locking", RunDesktopLockingChecks, &failures);

  std::filesystem::remove_all(temp_directory);

  if (failures == 0) {
    std::cout << "locking_glass_tests: ok\n";
  } else {
    std::cerr << "locking_glass_tests: failed (" << failures
              << " check group(s) or expectation block(s))\n";
  }

  ClearLockingGlassTestEnvironment();
  return failures == 0 ? 0 : 1;
}
