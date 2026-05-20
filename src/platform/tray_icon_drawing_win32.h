#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "locking_glass/core/tray_ui.h"

namespace locking_glass::platform::internal {

HBITMAP CreateMenuPadlockBitmap(const core::TrayPadlockIconState& icon);
HICON CreateTrayStatusIcon(const core::TrayIconState& icon);

}  // namespace locking_glass::platform::internal

#endif
