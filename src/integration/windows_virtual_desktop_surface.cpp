#include "windows_virtual_desktop_surface.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <objbase.h>
#if defined(__has_include)
#if __has_include(<shobjidl_core.h>)
#include <shobjidl_core.h>
#else
#include <shobjidl.h>
#endif
#else
#include <shobjidl_core.h>
#endif
#include <windows.h>
#endif

namespace locking_glass::integration::internal {

#if defined(_WIN32)
namespace {

HMODULE LoadVirtualDesktopHelper() {
  wchar_t helper_path[MAX_PATH];
  const DWORD length = GetEnvironmentVariableW(
      L"LOCKING_GLASS_VIRTUAL_DESKTOP_HELPER", helper_path, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return LoadLibraryW(helper_path);
  }
  return LoadLibraryW(L"VirtualDesktopAccessor.dll");
}

}  // namespace
#endif

WindowsVirtualDesktopSurfaceProbe ProbeWindowsVirtualDesktopSurface() {
  WindowsVirtualDesktopSurfaceProbe probe;

#if defined(_WIN32)
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  probe.com_ready = SUCCEEDED(com_result) || com_result == RPC_E_CHANGED_MODE;

  IVirtualDesktopManager* desktop_manager = nullptr;
  if (probe.com_ready) {
    const HRESULT desktop_result =
        CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&desktop_manager));
    probe.desktop_manager_ready =
        SUCCEEDED(desktop_result) && desktop_manager != nullptr;
  }

  HMODULE helper_library = LoadVirtualDesktopHelper();
  probe.helper_library_ready = helper_library != nullptr;
  probe.helper_watch_ready =
      helper_library != nullptr &&
      GetProcAddress(helper_library, "RegisterPostMessageHook") != nullptr &&
      GetProcAddress(helper_library, "UnregisterPostMessageHook") != nullptr &&
      GetProcAddress(helper_library, "GetCurrentDesktopNumber") != nullptr &&
      GetProcAddress(helper_library, "GoToDesktopNumber") != nullptr;
  probe.helper_move_ready =
      helper_library != nullptr &&
      GetProcAddress(helper_library, "MoveWindowToDesktopNumber") != nullptr &&
      GetProcAddress(helper_library, "GetWindowDesktopNumber") != nullptr;

  if (helper_library != nullptr) {
    FreeLibrary(helper_library);
  }
  if (desktop_manager != nullptr) {
    desktop_manager->Release();
  }
  if (com_result == S_OK || com_result == S_FALSE) {
    CoUninitialize();
  }
#endif

  return probe;
}

}  // namespace locking_glass::integration::internal
