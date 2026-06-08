#include "windows_virtual_desktop_surface.h"

#include "windows_live_desktop_watch.h"
#include "windows_virtual_desktop_helper.h"

#include <filesystem>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

std::vector<std::filesystem::path> BuildHelperDllCandidates() {
  std::vector<std::filesystem::path> candidates;
  const auto add_candidate = [&](const std::filesystem::path& path) {
    if (path.empty()) {
      return;
    }
    for (const auto& candidate : candidates) {
      if (candidate == path) {
        return;
      }
    }
    candidates.push_back(path);
  };

  wchar_t module_path[MAX_PATH];
  const DWORD module_length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  if (module_length > 0 && module_length < MAX_PATH) {
    add_candidate(std::filesystem::path(module_path).parent_path() /
                  "VirtualDesktopAccessor.dll");
  }

  const auto asset_root = FindLiveWatchAssetRoot();
  add_candidate(ResolvePreferredHelperDllPath(asset_root));
  return candidates;
}

HMODULE LoadVirtualDesktopHelper() {
  for (const auto& candidate : BuildHelperDllCandidates()) {
    std::error_code exists_error;
    if (!std::filesystem::exists(candidate, exists_error) || exists_error) {
      continue;
    }

    std::string hash_detail;
    if (!VerifyVirtualDesktopAccessorSha256(candidate, &hash_detail)) {
      continue;
    }

    if (HMODULE library =
            LoadLibraryExW(candidate.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                               LOAD_LIBRARY_SEARCH_SYSTEM32);
        library != nullptr) {
      return library;
    }
  }

  return nullptr;
}

}  // namespace
#endif

WindowsVirtualDesktopSurfaceProbe ProbeWindowsVirtualDesktopSurface() {
  WindowsVirtualDesktopSurfaceProbe probe;

#if defined(_WIN32)
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  probe.com_ready = SUCCEEDED(com_result) || com_result == RPC_E_CHANGED_MODE;

  IUnknown* desktop_manager = nullptr;
  if (probe.com_ready) {
    const HRESULT desktop_result =
        CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL,
                         IID_IUnknown,
                         reinterpret_cast<void**>(&desktop_manager));
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
  // Lifecycle exports are required for the owned staging desktop; without them
  // live locking fails closed instead of using a user's workspace as overflow.
  probe.helper_lifecycle_ready =
      helper_library != nullptr &&
      GetProcAddress(helper_library, "GetDesktopCount") != nullptr &&
      GetProcAddress(helper_library, "GetDesktopName") != nullptr &&
      GetProcAddress(helper_library, "GetDesktopIdByNumber") != nullptr &&
      GetProcAddress(helper_library, "CreateDesktop") != nullptr &&
      GetProcAddress(helper_library, "SetDesktopName") != nullptr &&
      GetProcAddress(helper_library, "RemoveDesktop") != nullptr;

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
