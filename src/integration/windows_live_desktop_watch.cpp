#include "windows_live_desktop_watch.h"

#if defined(_WIN32)

#include "windows_virtual_desktop_helper.h"

#include <fstream>
#include <system_error>

namespace locking_glass::integration::internal {

namespace {

bool HasRepositoryLiveWatchAssets(const std::filesystem::path& root) {
  return std::filesystem::exists(root / "scripts" /
                                 "run-live-desktop-probe.ps1") &&
         std::filesystem::exists(root / "tools" / "windows_live_desktop_probe" /
                                 "LockingGlass.WindowsLiveDesktopProbe.csproj");
}

bool HasBundledLiveWatchAssets(const std::filesystem::path& root) {
  return std::filesystem::exists(root / "run-live-desktop-probe.ps1") &&
         (std::filesystem::exists(root /
                                  "LockingGlass.WindowsLiveDesktopProbe.exe") ||
          std::filesystem::exists(root /
                                  "LockingGlass.WindowsLiveDesktopProbe.dll"));
}

std::filesystem::path ResolveLiveWatchScriptPath(
    const std::filesystem::path& root) {
  const auto bundled_script = root / "run-live-desktop-probe.ps1";
  if (std::filesystem::exists(bundled_script)) {
    return bundled_script;
  }

  const auto repository_script =
      root / "scripts" / "run-live-desktop-probe.ps1";
  if (std::filesystem::exists(repository_script)) {
    return repository_script;
  }

  return {};
}

std::filesystem::path ResolveWindowsPowerShellPath() {
  wchar_t system_directory[MAX_PATH];
  const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return std::filesystem::path(system_directory) / "WindowsPowerShell" /
           "v1.0" / "powershell.exe";
  }

  return std::filesystem::path(
      "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
}

}  // namespace

std::filesystem::path BuildLiveWatchLogPath() {
  const DWORD process_id = GetCurrentProcessId();
  return std::filesystem::temp_directory_path() /
         ("locking-glass-live-desktop-watch-" + std::to_string(process_id) +
          ".log");
}

std::filesystem::path FindLiveWatchAssetRoot() {
  wchar_t module_path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }

  const auto module_directory = std::filesystem::path(module_path).parent_path();
  if (HasBundledLiveWatchAssets(module_directory)) {
    return module_directory;
  }

  std::error_code error;
  const auto current_directory = std::filesystem::current_path(error);
  if (!error && HasRepositoryLiveWatchAssets(current_directory)) {
    return current_directory;
  }

  return {};
}

std::string QuoteCommandArgument(const std::string& value) {
  std::string quoted;
  quoted.reserve(value.size() + 2U);
  quoted.push_back('"');
  for (const char ch : value) {
    if (ch == '%') {
      quoted.append("%%");
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('"');
  return quoted;
}

std::filesystem::path BuildLiveWatchCommandScript(
    const std::filesystem::path& asset_root,
    const std::filesystem::path& log_path,
    const DesktopWatchOptions& options) {
  const auto script_path = ResolveLiveWatchScriptPath(asset_root);
  const auto powershell_path = ResolveWindowsPowerShellPath();
  const auto helper_dll_path = ResolvePreferredHelperDllPath(asset_root);
  if (script_path.empty() || helper_dll_path.empty()) {
    return {};
  }

  const auto command_script_path =
      std::filesystem::temp_directory_path() /
      ("locking-glass-live-desktop-watch-" +
       std::to_string(GetCurrentProcessId()) + ".cmd");

  std::ofstream output(command_script_path, std::ios::trunc);
  if (!output) {
    return {};
  }

  output << "@echo off\r\n";
  output << QuoteCommandArgument(powershell_path.string())
         << " -NoProfile -ExecutionPolicy Bypass -File "
         << QuoteCommandArgument(script_path.string()) << " -WatchStream"
         << " -HelperDllPath "
         << QuoteCommandArgument(helper_dll_path.string())
         << " -LogPath " << QuoteCommandArgument(log_path.string())
         << " -RequiredEvents " << options.required_events
         << " -TimeoutSeconds " << options.timeout_seconds
         << " -NoAutoCycle"
         << " -SkipMoveExercise\r\n";
  return command_script_path;
}

void TrimTrailingLineBreaks(std::string* value) {
  while (!value->empty() &&
         (value->back() == '\n' || value->back() == '\r')) {
    value->pop_back();
  }
}

bool ParseLiveDesktopSwitchEvent(const std::string& line,
                                 LiveDesktopSwitchEvent* event) {
  const auto fields = SplitFields(line);
  if (fields.size() != 7U || fields[0] != "watch-event") {
    return false;
  }

  if (!ParseIntField(fields[1], &event->source_desktop_number) ||
      !ParseIntField(fields[4], &event->target_desktop_number)) {
    return false;
  }

  event->source_desktop_guid = fields[2];
  event->source_desktop_name = fields[3];
  event->target_desktop_guid = fields[5];
  event->target_desktop_name = fields[6];
  return true;
}

}  // namespace locking_glass::integration::internal

#endif
