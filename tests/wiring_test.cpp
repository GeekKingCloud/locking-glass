#include "locking_glass/core/runtime.h"

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
  const auto diagnostics = locking_glass::core::CollectStartupDiagnostics(runtime);
  const auto formatted = locking_glass::core::FormatDiagnostics(diagnostics);

  failures += !Expect(diagnostics.capabilities.size() == 2U,
                      "startup diagnostics should expose exactly two capability probes");
  failures += !Expect(formatted.find("windows-api") != std::string::npos,
                      "formatted diagnostics should mention the Windows API probe");
  failures += !Expect(formatted.find("ffmpeg") != std::string::npos,
                      "formatted diagnostics should mention the FFmpeg probe");

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

  if (failures == 0) {
    std::cout << "wiring_test: ok\n";
  }

  return failures == 0 ? 0 : 1;
}
