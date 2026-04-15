#include "locking_glass/core/runtime.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct ParsedArguments {
  bool self_check = false;
  bool prototype_windows_apis = false;
  bool install_autostart = false;
  bool background = false;
  bool help = false;
};

ParsedArguments ParseArguments(int argc, char** argv, bool* valid) {
  ParsedArguments parsed;
  *valid = true;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--self-check") {
      parsed.self_check = true;
    } else if (argument == "--prototype-windows-apis") {
      parsed.prototype_windows_apis = true;
    } else if (argument == "--install-autostart") {
      parsed.install_autostart = true;
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
  return std::filesystem::absolute(argv[0]).lexically_normal().string();
}

void PrintUsage() {
  std::cout << "Usage: locking_glass [--self-check] [--install-autostart] "
               "[--prototype-windows-apis] [--background]\n";
}

}  // namespace

int main(int argc, char** argv) {
  const auto runtime = locking_glass::core::BuildRuntime();
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

  if (arguments.background) {
    return runtime.background_session->Run();
  }

  std::cout << "LockingGlass scaffold bootstrap\n";
  std::cout << locking_glass::core::FormatDiagnostics(diagnostics);
  return 0;
}
