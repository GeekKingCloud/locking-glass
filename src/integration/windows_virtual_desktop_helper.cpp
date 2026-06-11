#include "windows_virtual_desktop_helper.h"

#if defined(_WIN32)

#include <array>
#include <bit>
#include <cstdio>
#include <system_error>

#include <wincrypt.h>

namespace locking_glass::integration::internal {

namespace {

bool GuidIsZero(const GUID& guid) {
  return guid.Data1 == 0 && guid.Data2 == 0 && guid.Data3 == 0 &&
         guid.Data4[0] == 0 && guid.Data4[1] == 0 && guid.Data4[2] == 0 &&
         guid.Data4[3] == 0 && guid.Data4[4] == 0 && guid.Data4[5] == 0 &&
         guid.Data4[6] == 0 && guid.Data4[7] == 0;
}

struct DesktopWindowSearchContext {
  const WindowsVirtualDesktopHelper* helper = nullptr;
  int desktop_number = -1;
  bool found = false;
};

BOOL CALLBACK FindWindowOnDesktop(HWND window, LPARAM raw_context) {
  auto* context = reinterpret_cast<DesktopWindowSearchContext*>(raw_context);
  if (context == nullptr || context->helper == nullptr) {
    return FALSE;
  }

  if (context->helper->GetWindowDesktopNumber(window) ==
      context->desktop_number) {
    context->found = true;
    return FALSE;
  }

  return TRUE;
}

bool IsStrongDesktopIdentity(const DesktopIdentity& desktop) {
  return !desktop.guid.empty() || !desktop.display_id.empty();
}

const DesktopIdentity* FindStrongMatchingDesktop(
    const std::vector<DesktopIdentity>& desktops,
    const DesktopIdentity& remembered_desktop) {
  for (const auto& desktop : desktops) {
    if (!remembered_desktop.guid.empty() &&
        desktop.guid == remembered_desktop.guid) {
      return &desktop;
    }
    if (!remembered_desktop.display_id.empty() &&
        desktop.display_id == remembered_desktop.display_id) {
      return &desktop;
    }
  }
  return nullptr;
}

class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
  ~ScopedHandle() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }

  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;

  explicit operator bool() const {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  HANDLE get() const { return handle_; }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::string EncodeHex(const BYTE* bytes, const DWORD byte_count) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(static_cast<std::size_t>(byte_count) * 2U);
  for (DWORD index = 0; index < byte_count; ++index) {
    encoded.push_back(kHex[(bytes[index] >> 4) & 0x0F]);
    encoded.push_back(kHex[bytes[index] & 0x0F]);
  }
  return encoded;
}

ScopedHandle OpenHelperDllForReadLock(const std::filesystem::path& path,
                                      std::string* detail) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (detail != nullptr) {
      *detail = "Failed to open VirtualDesktopAccessor.dll at " +
                path.string() + " with a read lock (Win32 error " +
                std::to_string(GetLastError()) + ").";
    }
  }
  return ScopedHandle(file);
}

bool ComputeSha256Hex(HANDLE file, const std::filesystem::path& path,
                      std::string* digest,
                      std::string* detail) {
  LARGE_INTEGER file_start{};
  if (!SetFilePointerEx(file, file_start, nullptr, FILE_BEGIN)) {
    if (detail != nullptr) {
      *detail = "Failed to rewind VirtualDesktopAccessor.dll at " +
                path.string() + " for SHA-256 verification (Win32 error " +
                std::to_string(GetLastError()) + ").";
    }
    return false;
  }

  HCRYPTPROV provider = 0;
  if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                            CRYPT_VERIFYCONTEXT)) {
    if (detail != nullptr) {
      *detail = "Failed to initialize SHA-256 provider for " + path.string() +
                " (Win32 error " + std::to_string(GetLastError()) + ").";
    }
    return false;
  }

  HCRYPTHASH hash = 0;
  if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
    if (detail != nullptr) {
      *detail = "Failed to initialize SHA-256 hash for " + path.string() +
                " (Win32 error " + std::to_string(GetLastError()) + ").";
    }
    CryptReleaseContext(provider, 0);
    return false;
  }

  std::array<BYTE, 8192> buffer = {};
  while (true) {
    DWORD bytes_read = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &bytes_read, nullptr)) {
      if (detail != nullptr) {
        *detail = "Failed while reading VirtualDesktopAccessor.dll at " +
                  path.string() + " for SHA-256 verification (Win32 error " +
                  std::to_string(GetLastError()) + ").";
      }
      CryptDestroyHash(hash);
      CryptReleaseContext(provider, 0);
      return false;
    }

    if (bytes_read == 0) {
      break;
    }

    if (!CryptHashData(hash, buffer.data(), bytes_read, 0)) {
      if (detail != nullptr) {
        *detail = "Failed to hash VirtualDesktopAccessor.dll at " +
                  path.string() + " (Win32 error " +
                  std::to_string(GetLastError()) + ").";
      }
      CryptDestroyHash(hash);
      CryptReleaseContext(provider, 0);
      return false;
    }
  }

  std::array<BYTE, 32> hash_bytes = {};
  DWORD hash_size = static_cast<DWORD>(hash_bytes.size());
  if (!CryptGetHashParam(hash, HP_HASHVAL, hash_bytes.data(), &hash_size, 0)) {
    if (detail != nullptr) {
      *detail = "Failed to finish SHA-256 verification for " + path.string() +
                " (Win32 error " + std::to_string(GetLastError()) + ").";
    }
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    return false;
  }

  if (digest != nullptr) {
    *digest = EncodeHex(hash_bytes.data(), hash_size);
  }
  CryptDestroyHash(hash);
  CryptReleaseContext(provider, 0);
  return true;
}

bool VerifyOpenHelperDllSha256(HANDLE file, const std::filesystem::path& path,
                               std::string* detail) {
  std::string actual_hash;
  if (!ComputeSha256Hex(file, path, &actual_hash, detail)) {
    return false;
  }

  if (actual_hash != kVirtualDesktopAccessorSha256) {
    if (detail != nullptr) {
      *detail = "VirtualDesktopAccessor.dll at " + path.string() +
                " had SHA-256 '" + actual_hash + "', expected '" +
                kVirtualDesktopAccessorSha256 + "'.";
    }
    return false;
  }

  return true;
}

}  // namespace

std::filesystem::path ResolvePreferredHelperDllPath(
    const std::filesystem::path& asset_root) {
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

  return {};
}

bool VerifyVirtualDesktopAccessorSha256(const std::filesystem::path& path,
                                        std::string* detail) {
  const auto file = OpenHelperDllForReadLock(path, detail);
  if (!file) {
    return false;
  }

  return VerifyOpenHelperDllSha256(file.get(), path, detail);
}

HMODULE LoadVerifiedVirtualDesktopAccessor(const std::filesystem::path& path,
                                           std::string* detail) {
  const auto file = OpenHelperDllForReadLock(path, detail);
  if (!file) {
    return nullptr;
  }

  if (!VerifyOpenHelperDllSha256(file.get(), path, detail)) {
    return nullptr;
  }

  HMODULE library =
      LoadLibraryExW(path.c_str(), nullptr,
                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                         LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (library == nullptr) {
    if (detail != nullptr) {
      const DWORD load_error = GetLastError();
      *detail =
          "LoadLibraryExW failed for " + path.string() + " (Win32 error " +
          std::to_string(load_error) + ").";
    }
    return nullptr;
  }

  return library;
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
  RemoveStagingDesktopIfUnused(nullptr);
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

bool WindowsVirtualDesktopHelper::RemoveStagingDesktopIfUnused(
    std::string* detail) {
  if (!owned_staging_desktop_.has_value()) {
    if (detail != nullptr) {
      *detail = "staging desktop was not created by this helper";
    }
    return true;
  }
  return RemoveKnownStagingDesktopIfUnused(*owned_staging_desktop_, detail);
}

bool WindowsVirtualDesktopHelper::RemoveKnownStagingDesktopIfUnused(
    const DesktopIdentity& staging_identity, std::string* detail) {
  if (remove_desktop_ == nullptr || get_desktop_count_ == nullptr ||
      get_window_desktop_number_ == nullptr) {
    if (detail != nullptr) {
      *detail = "staging desktop cleanup is unavailable";
    }
    return false;
  }

  if (staging_identity.name != kStagingDesktopName ||
      !IsStrongDesktopIdentity(staging_identity)) {
    if (detail != nullptr) {
      *detail = "staging desktop cleanup requires an exact helper identity";
    }
    return false;
  }

  std::optional<DesktopIdentity> staging_desktop;
  const auto desktops = ListDesktops();
  if (const auto* desktop =
          FindStrongMatchingDesktop(desktops, staging_identity);
      desktop != nullptr && desktop->name == kStagingDesktopName) {
    staging_desktop = *desktop;
  }
  if (!staging_desktop.has_value()) {
    if (owned_staging_desktop_.has_value() &&
        DesktopIdentityEquals(*owned_staging_desktop_, staging_identity)) {
      owned_staging_desktop_.reset();
    }
    if (detail != nullptr) {
      *detail = "staging desktop was not present";
    }
    return true;
  }

  DesktopWindowSearchContext context{
      .helper = this,
      .desktop_number = staging_desktop->number,
      .found = false,
  };
  EnumWindows(FindWindowOnDesktop, reinterpret_cast<LPARAM>(&context));
  if (context.found) {
    if (detail != nullptr) {
      *detail = "staging desktop still contains windows";
    }
    return false;
  }

  const int desktop_count = GetDesktopCount();
  int fallback_desktop_number = 0;
  if (fallback_desktop_number == staging_desktop->number) {
    fallback_desktop_number = 1;
  }
  if (fallback_desktop_number < 0 ||
      fallback_desktop_number >= desktop_count) {
    if (detail != nullptr) {
      *detail = "no fallback desktop is available for cleanup";
    }
    return false;
  }

  if (remove_desktop_(staging_desktop->number, fallback_desktop_number) < 0) {
    if (detail != nullptr) {
      *detail = "RemoveDesktop returned a failure status during cleanup";
    }
    return false;
  }

  if (owned_staging_desktop_.has_value() &&
      DesktopIdentityEquals(*owned_staging_desktop_, *staging_desktop)) {
    owned_staging_desktop_.reset();
  }
  if (detail != nullptr) {
    *detail = "staging desktop removed";
  }
  return true;
}

bool WindowsVirtualDesktopHelper::RemoveEmptyStagingDesktopByName(
    std::string* detail) {
  std::vector<DesktopIdentity> matches;
  for (const auto& desktop : ListDesktops()) {
    if (desktop.name == kStagingDesktopName && IsStrongDesktopIdentity(desktop)) {
      matches.push_back(desktop);
    }
  }

  if (matches.empty()) {
    if (detail != nullptr) {
      *detail = "no staging desktop was present";
    }
    return true;
  }
  if (matches.size() != 1U) {
    if (detail != nullptr) {
      *detail = "multiple staging desktops matched the Locking Glass name";
    }
    return false;
  }

  // This sweep handles empty leftovers from a previous process. It still uses
  // the exact live identity and refuses removal when any window remains there.
  return RemoveKnownStagingDesktopIfUnused(matches.front(), detail);
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

  std::string last_error =
      "VirtualDesktopAccessor.dll was not found in the packaged or staged "
      "helper locations.";
  for (const auto& candidate : candidates) {
    std::error_code exists_error;
    if (!std::filesystem::exists(candidate, exists_error) || exists_error) {
      continue;
    }

    HMODULE library = LoadVerifiedVirtualDesktopAccessor(candidate, &last_error);
    if (library == nullptr) {
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
          " was missing a required desktop move, identity, or lifecycle export.";
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
