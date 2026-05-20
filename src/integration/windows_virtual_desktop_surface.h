#pragma once

namespace locking_glass::integration::internal {

struct WindowsVirtualDesktopSurfaceProbe {
  bool com_ready = false;
  bool desktop_manager_ready = false;
  bool helper_library_ready = false;
  bool helper_watch_ready = false;
  bool helper_move_ready = false;
  bool helper_lifecycle_ready = false;
};

WindowsVirtualDesktopSurfaceProbe ProbeWindowsVirtualDesktopSurface();

}  // namespace locking_glass::integration::internal
