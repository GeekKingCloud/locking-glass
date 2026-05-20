#include "windows_virtual_desktop_helper.h"

#if defined(_WIN32)

#include <bit>
#include <cstdio>
#include <system_error>

namespace locking_glass::integration::internal {

namespace {

bool GuidIsZero(const GUID& guid) {
  return guid.Data1 == 0 && guid.Data2 == 0 && guid.Data3 == 0 &&
         guid.Data4[0] == 0 && guid.Data4[1] == 0 && guid.Data4[2] == 0 &&
         guid.Data4[3] == 0 && guid.Data4[4] == 0 && guid.Data4[5] == 0 &&
         guid.Data4[6] == 0 && guid.Data4[7] == 0;
}

}  // namespace

std::filesystem::path ResolvePreferredHelperDllPath(
    const std::filesystem::path& asset_root) {
  wchar_t helper_path[MAX_PATH];
  const DWORD length = GetEnvironmentVariableW(
      L"LOCKING_GLASS_VIRTUAL_DESKTOP_HELPER", helper_path, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return std::filesystem::path(helper_path);
  }

  if (!asset_root.empty()) {
    const auto repository_helper =
        asset_root / "build" / "windows-live-desktop-probe" /
        "VirtualDesktopAccessor.dll";
    if (std::filesystem::exists(repository_helper)) {
      return repository_helper;
    }

    const auto bundled_helper = asset_root / "VirtualDesktopAccessor.dll";
    if (std::filesystem::exists(bundled_helper)) {
      return bundled_helper;
    }
  }

  return std::filesystem::path("VirtualDesktopAccessor.dll");
}

WindowsVirtualDesktopHelper::WindowsVirtualDesktopHelper(
    HMODULE library, GetDesktopCountFn get_desktop_count,
    GetDesktopNameFn get_desktop_name,
    GetDesktopIdByNumberFn get_desktop_id_by_number,
    GetWindowDesktopNumberFn get_window_desktop_number,
    MoveWindowToDesktopNumberFn move_window_to_desktop_number,
    CreateDesktopFn create_desktop, SetDesktopNameFn set_desktop_name,
    RemoveDesktopFn remove_desktop)
    : library_(library),
      get_desktop_count_(get_desktop_count),
      get_desktop_name_(get_desktop_name),
      get_desktop_id_by_number_(get_desktop_id_by_number),
      get_window_desktop_number_(get_window_desktop_number),
      move_window_to_desktop_number_(move_window_to_desktop_number),
      create_desktop_(create_desktop),
      set_desktop_name_(set_desktop_name),
      remove_desktop_(remove_desktop) {}

WindowsVirtualDesktopHelper::~WindowsVirtualDesktopHelper() {
  if (library_ != nullptr) {
    FreeLibrary(library_);
  }
}

int WindowsVirtualDesktopHelper::GetDesktopCount() const {
  return get_desktop_count_ != nullptr ? get_desktop_count_() : 0;
}

DesktopIdentity WindowsVirtualDesktopHelper::GetDesktopIdentity(
    int desktop_number) const {
  std::string desktop_name;
  if (get_desktop_name_ != nullptr) {
    char buffer[1024] = {};
    if (get_desktop_name_(desktop_number, buffer, sizeof(buffer)) >= 0) {
      desktop_name = buffer;
    }
  }

  std::string desktop_guid;
  if (get_desktop_id_by_number_ != nullptr) {
    const GUID guid = get_desktop_id_by_number_(desktop_number);
    if (!GuidIsZero(guid)) {
      char buffer[64];
      std::snprintf(
          buffer, sizeof(buffer),
          "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
          guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
          guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
          guid.Data4[6], guid.Data4[7]);
      desktop_guid = buffer;
    }
  }

  return MakeDesktopIdentity(desktop_number, desktop_guid, desktop_name);
}

std::vector<DesktopIdentity> WindowsVirtualDesktopHelper::ListDesktops() const {
  std::vector<DesktopIdentity> desktops;
  const int desktop_count = GetDesktopCount();
  if (desktop_count <= 0) {
    return desktops;
  }

  desktops.reserve(static_cast<std::size_t>(desktop_count));
  for (int desktop_number = 0; desktop_number < desktop_count;
       ++desktop_number) {
    desktops.push_back(GetDesktopIdentity(desktop_number));
  }
  return desktops;
}

int WindowsVirtualDesktopHelper::GetWindowDesktopNumber(HWND window) const {
  return get_window_desktop_number_(window);
}

int WindowsVirtualDesktopHelper::MoveWindowToDesktopNumber(
    HWND window, int desktop_number) const {
  return move_window_to_desktop_number_(window, desktop_number);
}

std::optional<DesktopIdentity> WindowsVirtualDesktopHelper::EnsureStagingDesktop(
    std::string* detail) {
  if (owned_staging_desktop_.has_value()) {
    const auto desktops = ListDesktops();
    if (const auto* desktop = FindMatchingDesktop(
            desktops, *owned_staging_desktop_);
        desktop != nullptr) {
      return *desktop;
    }
    owned_staging_desktop_.reset();
  }

  // The staging desktop is only safe when this process created and resolved
  // its exact identity. Reusing an existing desktop by name could steal a
  // user's own workspace and hide unrelated windows there.
  if (create_desktop_ == nullptr || set_desktop_name_ == nullptr ||
      remove_desktop_ == nullptr || get_desktop_name_ == nullptr ||
      get_desktop_id_by_number_ == nullptr) {
    if (detail != nullptr) {
      *detail =
          "VirtualDesktopAccessor.dll is missing CreateDesktop or "
          "SetDesktopName lifecycle support, so Locking Glass cannot create "
          "the staging desktop safely.";
    }
    return std::nullopt;
  }

  const int created_desktop_number = create_desktop_();
  if (created_desktop_number < 0) {
    if (detail != nullptr) {
      *detail = "CreateDesktop returned a failure status.";
    }
    return std::nullopt;
  }

  if (set_desktop_name_(created_desktop_number, kStagingDesktopName) < 0) {
    // No user windows have been staged yet, so removing this new desktop is
    // safe and avoids leaving behind a nameless scratch workspace.
    remove_desktop_(created_desktop_number, 0);
    if (detail != nullptr) {
      *detail = "SetDesktopName returned a failure status for the staging desktop.";
    }
    return std::nullopt;
  }

  for (const auto& desktop : ListDesktops()) {
    if (desktop.number == created_desktop_number &&
        desktop.name == kStagingDesktopName) {
      owned_staging_desktop_ = desktop;
      return desktop;
    }
  }

  remove_desktop_(created_desktop_number, 0);
  if (detail != nullptr) {
    *detail = "The staging desktop was created but could not be resolved.";
  }
  return std::nullopt;
}

std::unique_ptr<WindowsVirtualDesktopHelper> WindowsVirtualDesktopHelper::Load(
    const std::filesystem::path& repository_root, std::string* detail) {
  std::vector<std::filesystem::path> candidates;
  const auto preferred_path = ResolvePreferredHelperDllPath(repository_root);
  if (!preferred_path.empty()) {
    candidates.push_back(preferred_path);
  }

  wchar_t module_path[MAX_PATH];
  const DWORD module_length =
      GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  if (module_length > 0 && module_length < MAX_PATH) {
    candidates.push_back(std::filesystem::path(module_path).parent_path() /
                         "VirtualDesktopAccessor.dll");
  }

  candidates.push_back(std::filesystem::current_path() /
                       "VirtualDesktopAccessor.dll");
  candidates.push_back(std::filesystem::path("VirtualDesktopAccessor.dll"));

  std::string last_error =
      "VirtualDesktopAccessor.dll could not be loaded for live window moves.";
  for (const auto& candidate : candidates) {
    if (candidate.has_parent_path()) {
      std::error_code exists_error;
      if (!std::filesystem::exists(candidate, exists_error) || exists_error) {
        continue;
      }
    }

    HMODULE library = LoadLibraryW(candidate.c_str());
    if (library == nullptr) {
      last_error =
          "LoadLibraryW failed for " + candidate.string() + " (Win32 error " +
          std::to_string(GetLastError()) + ").";
      continue;
    }

    const FARPROC get_desktop_count_symbol =
        GetProcAddress(library, "GetDesktopCount");
    const FARPROC get_desktop_name_symbol =
        GetProcAddress(library, "GetDesktopName");
    const FARPROC get_desktop_id_by_number_symbol =
        GetProcAddress(library, "GetDesktopIdByNumber");
    const FARPROC get_window_desktop_number_symbol =
        GetProcAddress(library, "GetWindowDesktopNumber");
    const FARPROC move_window_to_desktop_number_symbol =
        GetProcAddress(library, "MoveWindowToDesktopNumber");
    const FARPROC create_desktop_symbol =
        GetProcAddress(library, "CreateDesktop");
    const FARPROC set_desktop_name_symbol =
        GetProcAddress(library, "SetDesktopName");
    const FARPROC remove_desktop_symbol =
        GetProcAddress(library, "RemoveDesktop");
    const auto get_desktop_count =
        get_desktop_count_symbol != nullptr
            ? std::bit_cast<GetDesktopCountFn>(get_desktop_count_symbol)
            : nullptr;
    const auto get_desktop_name =
        get_desktop_name_symbol != nullptr
            ? std::bit_cast<GetDesktopNameFn>(get_desktop_name_symbol)
            : nullptr;
    const auto get_desktop_id_by_number =
        get_desktop_id_by_number_symbol != nullptr
            ? std::bit_cast<GetDesktopIdByNumberFn>(
                  get_desktop_id_by_number_symbol)
            : nullptr;
    const auto get_window_desktop_number =
        get_window_desktop_number_symbol != nullptr
            ? std::bit_cast<GetWindowDesktopNumberFn>(
                  get_window_desktop_number_symbol)
            : nullptr;
    const auto move_window_to_desktop_number =
        move_window_to_desktop_number_symbol != nullptr
            ? std::bit_cast<MoveWindowToDesktopNumberFn>(
                  move_window_to_desktop_number_symbol)
            : nullptr;
    const auto create_desktop =
        create_desktop_symbol != nullptr
            ? std::bit_cast<CreateDesktopFn>(create_desktop_symbol)
            : nullptr;
    const auto set_desktop_name =
        set_desktop_name_symbol != nullptr
            ? std::bit_cast<SetDesktopNameFn>(set_desktop_name_symbol)
            : nullptr;
    const auto remove_desktop =
        remove_desktop_symbol != nullptr
            ? std::bit_cast<RemoveDesktopFn>(remove_desktop_symbol)
            : nullptr;
    if (get_desktop_count == nullptr || get_desktop_name == nullptr ||
        get_desktop_id_by_number == nullptr ||
        get_window_desktop_number == nullptr ||
        move_window_to_desktop_number == nullptr ||
        create_desktop == nullptr || set_desktop_name == nullptr ||
        remove_desktop == nullptr) {
      last_error =
          "VirtualDesktopAccessor.dll at " + candidate.string() +
          " was missing a required desktop hook, move, identity, or "
          "lifecycle export.";
      FreeLibrary(library);
      continue;
    }

    if (detail != nullptr) {
      *detail = candidate.string();
    }
    return std::make_unique<WindowsVirtualDesktopHelper>(
        library, get_desktop_count, get_desktop_name,
        get_desktop_id_by_number, get_window_desktop_number,
        move_window_to_desktop_number, create_desktop, set_desktop_name,
        remove_desktop);
  }

  if (detail != nullptr) {
    *detail = last_error;
  }
  return nullptr;
}

}  // namespace locking_glass::integration::internal

#endif
