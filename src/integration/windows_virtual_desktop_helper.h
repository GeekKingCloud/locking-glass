#pragma once

#if defined(_WIN32)

#include "virtual_desktop_controller_internal.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace locking_glass::integration::internal {

inline constexpr char kStagingDesktopName[] = "Locking Glass";
inline constexpr char kVirtualDesktopAccessorSha256[] =
    "8740C572A1C000E3B87FFEB1E4C397EAE9AF3BD4A2ABDC3BCFFACAB4493F8FF5";

std::filesystem::path ResolvePreferredHelperDllPath(
    const std::filesystem::path& asset_root);
bool VerifyVirtualDesktopAccessorSha256(const std::filesystem::path& path,
                                        std::string* detail);
HMODULE LoadVerifiedVirtualDesktopAccessor(const std::filesystem::path& path,
                                           std::string* detail);

class WindowsVirtualDesktopHelper {
 public:
  using GetDesktopCountFn = int(WINAPI*)();
  using GetDesktopNameFn = int(WINAPI*)(int, char*, std::size_t);
  using GetDesktopIdByNumberFn = GUID(WINAPI*)(int);
  using GetWindowDesktopNumberFn = int(WINAPI*)(HWND);
  using MoveWindowToDesktopNumberFn = int(WINAPI*)(HWND, int);
  using CreateDesktopFn = int(WINAPI*)();
  using SetDesktopNameFn = int(WINAPI*)(int, const char*);
  using RemoveDesktopFn = int(WINAPI*)(int, int);

  WindowsVirtualDesktopHelper(HMODULE library,
                              GetDesktopCountFn get_desktop_count,
                              GetDesktopNameFn get_desktop_name,
                              GetDesktopIdByNumberFn get_desktop_id_by_number,
                              GetWindowDesktopNumberFn get_window_desktop_number,
                              MoveWindowToDesktopNumberFn move_window_to_desktop_number,
                              CreateDesktopFn create_desktop,
                              SetDesktopNameFn set_desktop_name,
                              RemoveDesktopFn remove_desktop);
  ~WindowsVirtualDesktopHelper();

  WindowsVirtualDesktopHelper(const WindowsVirtualDesktopHelper&) = delete;
  WindowsVirtualDesktopHelper& operator=(const WindowsVirtualDesktopHelper&) =
      delete;

  int GetDesktopCount() const;
  DesktopIdentity GetDesktopIdentity(int desktop_number) const;
  std::vector<DesktopIdentity> ListDesktops() const;
  int GetWindowDesktopNumber(HWND window) const;
  int MoveWindowToDesktopNumber(HWND window, int desktop_number) const;
  std::optional<DesktopIdentity> EnsureStagingDesktop(std::string* detail);
  bool RemoveStagingDesktopIfUnused(std::string* detail);
  bool RemoveKnownStagingDesktopIfUnused(const DesktopIdentity& staging_identity,
                                         std::string* detail);
  bool RemoveEmptyStagingDesktopByName(std::string* detail);

  static std::unique_ptr<WindowsVirtualDesktopHelper> Load(
      const std::filesystem::path& repository_root, std::string* detail);

 private:
  HMODULE library_ = nullptr;
  GetDesktopCountFn get_desktop_count_ = nullptr;
  GetDesktopNameFn get_desktop_name_ = nullptr;
  GetDesktopIdByNumberFn get_desktop_id_by_number_ = nullptr;
  GetWindowDesktopNumberFn get_window_desktop_number_ = nullptr;
  MoveWindowToDesktopNumberFn move_window_to_desktop_number_ = nullptr;
  CreateDesktopFn create_desktop_ = nullptr;
  SetDesktopNameFn set_desktop_name_ = nullptr;
  RemoveDesktopFn remove_desktop_ = nullptr;
  std::optional<DesktopIdentity> owned_staging_desktop_;
};

}  // namespace locking_glass::integration::internal

#endif
