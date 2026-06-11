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

HICON CreateTrayStatusIcon(const core::TrayIconState& icon);
bool DrawTrayMenuStatusIcon(HDC dc, const RECT& rect, bool locked);
bool DrawOverlayStatusIcon(HDC dc, const RECT& rect, bool locked);

}  // namespace locking_glass::platform::internal

#endif
