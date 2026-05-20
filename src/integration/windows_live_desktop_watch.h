#pragma once

#if defined(_WIN32)

#include "virtual_desktop_controller_internal.h"

#include <filesystem>
#include <string>

namespace locking_glass::integration::internal {

struct LiveDesktopSwitchEvent {
  int source_desktop_number = -1;
  std::string source_desktop_guid;
  std::string source_desktop_name;
  int target_desktop_number = -1;
  std::string target_desktop_guid;
  std::string target_desktop_name;
};

std::filesystem::path BuildLiveWatchLogPath();
std::filesystem::path FindLiveWatchAssetRoot();
std::string QuoteCommandArgument(const std::string& value);
std::filesystem::path BuildLiveWatchCommandScript(
    const std::filesystem::path& asset_root,
    const std::filesystem::path& log_path,
    const DesktopWatchOptions& options);
void TrimTrailingLineBreaks(std::string* value);
bool ParseLiveDesktopSwitchEvent(const std::string& line,
                                 LiveDesktopSwitchEvent* event);

}  // namespace locking_glass::integration::internal

#endif
