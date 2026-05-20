#pragma once

#if defined(_WIN32)

#include "windows_virtual_desktop_helper.h"

#include <functional>
#include <vector>

namespace locking_glass::integration::internal {

std::vector<CapturedWindow> CaptureLiveWindows(
    const WindowsVirtualDesktopHelper& helper,
    const std::vector<platform::MonitorDescriptor>& monitors,
    const core::SessionRefreshResult& session,
    const std::function<DesktopIdentity(int)>& resolve_desktop);
std::vector<CapturedWindow> CaptureLiveWindowsForReturn(
    const WindowsVirtualDesktopHelper& helper,
    const std::vector<platform::MonitorDescriptor>& monitors);

}  // namespace locking_glass::integration::internal

#endif
