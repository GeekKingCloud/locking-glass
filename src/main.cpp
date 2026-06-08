#include "locking_glass/core/runtime.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef LOCKING_GLASS_VERSION_STR
#define LOCKING_GLASS_VERSION_STR "0.0.0"
#endif

namespace {

struct ParsedArguments {
  bool version = false;
  bool self_check = false;
  bool prototype_windows_apis = false;
  bool install_autostart = false;
  bool watch_monitors = false;
  bool watch_virtual_desktops = false;
  bool background = false;
  bool help = false;
};

ParsedArguments ParseArguments(int argc, char** argv, bool* valid) {
  ParsedArguments parsed;
  *valid = true;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--version") {
      parsed.version = true;
    } else if (argument == "--self-check") {
      parsed.self_check = true;
    } else if (argument == "--prototype-windows-apis") {
      parsed.prototype_windows_apis = true;
    } else if (argument == "--install-autostart") {
      parsed.install_autostart = true;
    } else if (argument == "--watch-monitors") {
      parsed.watch_monitors = true;
    } else if (argument == "--watch-virtual-desktops") {
      parsed.watch_virtual_desktops = true;
    } else if (argument == "--background") {
      parsed.background = true;
    } else if (argument == "--help") {
      parsed.help = true;
    } else {
      std::cerr << "Unknown argument: " << argument << '\n';
      *valid = false;
      break;
    }
  }

  return parsed;
}

std::string ResolveExecutablePath(char** argv) {
#if defined(_WIN32)
  wchar_t module_path[MAX_PATH];
  const DWORD module_length =
      GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  if (module_length > 0 && module_length < MAX_PATH) {
    return std::filesystem::path(module_path).lexically_normal().string();
  }
#endif

  return std::filesystem::absolute(argv[0]).lexically_normal().string();
}

void PrintUsage() {
  std::cout << "Usage: locking_glass [--version] [--self-check] "
               "[--install-autostart] [--prototype-windows-apis] [--watch-monitors] "
               "[--watch-virtual-desktops] [--background]\n";
#if defined(_WIN32)
  std::cout << "On Windows, launching Locking Glass with no arguments starts the "
               "background tray app.\n";
#endif
}

bool RunsTrayMode(const ParsedArguments& arguments) {
  return arguments.background ||
         (!arguments.version && !arguments.self_check &&
          !arguments.prototype_windows_apis && !arguments.install_autostart &&
          !arguments.watch_monitors && !arguments.watch_virtual_desktops &&
          !arguments.help);
}

#if defined(_WIN32)
void ReleaseConsoleForTrayMode(const ParsedArguments& arguments) {
  if (RunsTrayMode(arguments)) {
    FreeConsole();
  }
}
#else
void ReleaseConsoleForTrayMode(const ParsedArguments&) {}
#endif

}  // namespace

int main(int argc, char** argv) {
  bool valid_arguments = true;
  const ParsedArguments arguments = ParseArguments(argc, argv, &valid_arguments);
  if (!valid_arguments) {
    PrintUsage();
    return 64;
  }

  if (arguments.help) {
    PrintUsage();
    return 0;
  }

  if (arguments.version) {
    std::cout << "Locking Glass " << LOCKING_GLASS_VERSION_STR << '\n';
    return 0;
  }

  ReleaseConsoleForTrayMode(arguments);

  const auto runtime = locking_glass::core::BuildRuntime();
  const std::string executable_path = ResolveExecutablePath(argv);
  const auto diagnostics =
      locking_glass::core::CollectStartupDiagnostics(runtime, executable_path);

  if (arguments.install_autostart) {
    const auto result = runtime.autostart_manager->Enable(executable_path);
    std::cout << result.detail << '\n';
    return result.success ? 0 : 1;
  }

  if (arguments.self_check) {
    std::cout << locking_glass::core::FormatDiagnostics(diagnostics);
    return 0;
  }

  if (arguments.prototype_windows_apis) {
    const auto prototype =
        runtime.windows_api_probe->BuildPrototype(diagnostics.monitors);
    std::cout << locking_glass::integration::FormatWindowsApiPrototype(prototype);
    return 0;
  }

  if (arguments.watch_monitors) {
    std::string previous_fingerprint;
    return runtime.monitor_watcher->Watch(
        [&](const locking_glass::platform::MonitorWatchEvent& event) {
          const auto report = locking_glass::core::RefreshMonitorState(
              runtime, event.monitors, event.trigger, previous_fingerprint);
          previous_fingerprint = report.topology_fingerprint;
          std::cout << locking_glass::core::FormatMonitorRefreshReport(report);
          return true;
        });
  }

  if (arguments.watch_virtual_desktops) {
    return runtime.virtual_desktop_controller->WatchSwitches(
        runtime.session_store,
        [&](const locking_glass::integration::DesktopSwitchReport& report) {
          std::cout
              << locking_glass::integration::FormatDesktopSwitchReport(report);
          return true;
        });
  }

  if (arguments.background) {
    return runtime.background_session->Run();
  }

  // No-argument Windows launches are the tray app. Non-Windows keeps a
  // diagnostic path for CI/smoke output without promising runtime support.
#if defined(_WIN32)
  return runtime.background_session->Run();
#else
  std::cout << "Locking Glass diagnostics\n";
  std::cout << locking_glass::core::FormatDiagnostics(diagnostics);
  return 0;
#endif
}
