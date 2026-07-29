#include "test_helpers.h"

#if defined(_WIN32)
#include "../src/integration/virtual_desktop_controller_internal.h"
#include "../src/integration/windows_live_desktop_watch.h"
#endif

namespace locking_glass::tests {

bool RunWindowsLiveDesktopWatchChecks() {
  int failures = 0;

#if defined(_WIN32)
  using locking_glass::integration::internal::BuildLiveWatchCommandScript;
  using locking_glass::integration::internal::QuoteCommandArgument;
  using locking_glass::integration::internal::ResolvePlannedDesktopDestination;

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

  const auto source_staging = MakeDesktopIdentity(
      3, "29ccd282-483a-4b9f-9b93-abb515d2e82a", "Locking Glass");
  const auto helper_staging = MakeDesktopIdentity(
      3, "29CCD282-483A-4B9F-9B93-ABB515D2E82A", "Locking Glass");
  const auto target_desktop = MakeDesktopIdentity(
      2, "31AD6EAE-1DB1-43AB-A316-A961F445D190", "Work Buyflow");
  const auto restored_home = MakeDesktopIdentity(
      1, "2E57B057-B318-482B-87CA-CA7E088A1F1A", "Work L5");
  const auto resolved_restore = ResolvePlannedDesktopDestination(
      "Desktop 2 [1] \"Work L5\" {2e57b057-b318-482b-87ca-ca7e088a1f1a}",
      source_staging, target_desktop, helper_staging,
      {
          MakeDesktopIdentity(0, "51EBA7C7-03B3-4497-AAA7-C9C7155621BB",
                              "Geek"),
          restored_home,
          target_desktop,
          helper_staging,
      });
  failures += !Expect(
      resolved_restore.has_value() && resolved_restore->number == 1 &&
          resolved_restore->guid == restored_home.guid,
      "live destination resolution should support staging-to-home restore moves when leaving Locking Glass");
#endif

  return failures == 0;
}

}  // namespace locking_glass::tests
