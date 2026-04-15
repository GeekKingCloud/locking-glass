#include "locking_glass/core/runtime.h"
#include "locking_glass/integration/autostart.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

bool Expect(const bool condition, const std::string& message) {
  if (condition) {
    return true;
  }

  std::cerr << "Expectation failed: " << message << '\n';
  return false;
}

const locking_glass::integration::CapabilityReport* FindCapability(
    const locking_glass::core::StartupDiagnostics& diagnostics, const std::string& name) {
  for (const auto& capability : diagnostics.capabilities) {
    if (capability.component == name) {
      return &capability;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  int failures = 0;

  const auto runtime = locking_glass::core::BuildRuntime();
  const std::string test_install_path =
      "C:\\Program Files\\LockingGlass\\LockingGlass.exe";
  const auto diagnostics =
      locking_glass::core::CollectStartupDiagnostics(runtime, test_install_path);
  const auto formatted = locking_glass::core::FormatDiagnostics(diagnostics);

  failures += !Expect(diagnostics.capabilities.size() == 4U,
                      "startup diagnostics should expose exactly four capability probes");
  failures += !Expect(formatted.find("background-session") != std::string::npos,
                      "formatted diagnostics should mention the background session capability");
  failures += !Expect(formatted.find("windows-api") != std::string::npos,
                      "formatted diagnostics should mention the Windows API probe");
  failures += !Expect(formatted.find("autostart") != std::string::npos,
                      "formatted diagnostics should mention the autostart capability");
  failures += !Expect(formatted.find("ffmpeg") != std::string::npos,
                      "formatted diagnostics should mention the FFmpeg probe");
  failures += !Expect(
      formatted.find("\"C:\\Program Files\\LockingGlass\\LockingGlass.exe\" --background") !=
          std::string::npos,
      "formatted diagnostics should expose the quoted Windows autostart command");

  const auto* background_session =
      FindCapability(diagnostics, "background-session");
  failures +=
      !Expect(background_session != nullptr, "background-session capability should exist");
  if (background_session != nullptr) {
#if defined(_WIN32)
    failures += !Expect(background_session->status !=
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "background-session should not be stubbed on Windows");
#else
    failures += !Expect(background_session->status ==
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "background-session should be stubbed on non-Windows hosts");
#endif
    failures += !Expect(!background_session->detail.empty(),
                        "background-session capability should provide a diagnostic detail");
  }

  const auto* windows_api = FindCapability(diagnostics, "windows-api");
  failures += !Expect(windows_api != nullptr, "windows-api capability should exist");
  if (windows_api != nullptr) {
#if defined(_WIN32)
    failures += !Expect(windows_api->status !=
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "windows-api should not be stubbed on Windows");
#else
    failures += !Expect(windows_api->status ==
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "windows-api should be stubbed on non-Windows hosts");
#endif
    failures += !Expect(!windows_api->detail.empty(),
                        "windows-api capability should provide a diagnostic detail");
  }

  const auto* autostart = FindCapability(diagnostics, "autostart");
  failures += !Expect(autostart != nullptr, "autostart capability should exist");
  if (autostart != nullptr) {
#if defined(_WIN32)
    failures += !Expect(autostart->status !=
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "autostart should not be stubbed on Windows");
#else
    failures += !Expect(autostart->status ==
                            locking_glass::integration::CapabilityStatus::kStubbed,
                        "autostart should be stubbed on non-Windows hosts");
#endif
    failures += !Expect(!autostart->detail.empty(),
                        "autostart capability should provide a diagnostic detail");
  }

  const auto* ffmpeg = FindCapability(diagnostics, "ffmpeg");
  failures += !Expect(ffmpeg != nullptr, "ffmpeg capability should exist");
  if (ffmpeg != nullptr) {
    const char* injected_library = std::getenv("LOCKING_GLASS_FFMPEG_LIBRARY");
    failures += !Expect(injected_library != nullptr && injected_library[0] != '\0',
                        "LOCKING_GLASS_FFMPEG_LIBRARY should be injected by the build");
    failures += !Expect(ffmpeg->status == locking_glass::integration::CapabilityStatus::kReady,
                        "ffmpeg probe should load the injected fake avutil runtime");
    failures += !Expect(ffmpeg->detail.find("fake-ffmpeg-1.0") != std::string::npos,
                        "ffmpeg diagnostic should include the fake avutil version string");
  }

  const auto quoted_path =
      locking_glass::integration::QuoteWindowsCommandArg(test_install_path);
  failures += !Expect(
      quoted_path == "\"C:\\Program Files\\LockingGlass\\LockingGlass.exe\"",
      "QuoteWindowsCommandArg should quote Windows paths that contain spaces");
  failures += !Expect(diagnostics.autostart.entry_name == "LockingGlass",
                      "autostart diagnostics should use the LockingGlass Run entry name");
  failures += !Expect(
      diagnostics.autostart.launch_command ==
          "\"C:\\Program Files\\LockingGlass\\LockingGlass.exe\" --background",
      "autostart diagnostics should launch the executable in background mode");

  if (failures == 0) {
    std::cout << "wiring_test: ok\n";
  }

  return failures == 0 ? 0 : 1;
}
