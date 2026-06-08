#include "test_helpers.h"

#if defined(_WIN32)
#include "../src/integration/windows_live_desktop_watch.h"
#endif

namespace locking_glass::tests {

bool RunWindowsLiveDesktopWatchChecks() {
  int failures = 0;

#if defined(_WIN32)
  using locking_glass::integration::internal::BuildLiveWatchCommandScript;
  using locking_glass::integration::internal::QuoteCommandArgument;

  failures += !Expect(
      QuoteCommandArgument("C:\\Program Files\\Locking Glass\\probe.log") ==
          "\"C:\\Program Files\\Locking Glass\\probe.log\"",
      "live watch command arguments should quote paths with spaces");
  failures += !Expect(
      QuoteCommandArgument("C:\\Temp\\100%ready\\probe.log") ==
          "\"C:\\Temp\\100%%ready\\probe.log\"",
      "live watch command arguments should escape percent signs for cmd.exe");

  const auto temp_directory = MakeTempDirectory();
  const auto command_script = BuildLiveWatchCommandScript(
      temp_directory, temp_directory / "probe.log",
      locking_glass::integration::DesktopWatchOptions{});
  failures += !Expect(
      command_script.empty(),
      "live watch command generation should fail closed when helper assets are missing");
  std::filesystem::remove_all(temp_directory);
#endif

  return failures == 0;
}

}  // namespace locking_glass::tests
