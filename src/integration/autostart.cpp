#include "locking_glass/integration/autostart.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace locking_glass::integration {

namespace {

constexpr char kAutostartScope[] = "current-user logon";
constexpr char kAutostartLocation[] =
    "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr char kAutostartEntryName[] = "Locking Glass";
constexpr char kStartupApprovedRunLocation[] =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr char kBackgroundArgument[] = "--background";
constexpr const char* kLegacyAutostartEntryNames[] = {
    "Locking Glass.exe",
    "LockingGlass",
    "LockingGlass.exe",
};

#if defined(_WIN32)
std::wstring Widen(const std::string& value) {
  if (value.empty()) {
    return {};
  }

  const int size =
      MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (size <= 1) {
    return {};
  }

  std::wstring buffer(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, buffer.data(), size);
  buffer.pop_back();
  return buffer;
}

std::wstring ReadValue(HKEY key, const wchar_t* value_name) {
  DWORD type = 0;
  DWORD size = 0;
  LONG status =
      RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &size);
  if (status != ERROR_SUCCESS || type != REG_SZ || size == 0) {
    return {};
  }

  std::wstring value(size / sizeof(wchar_t), L'\0');
  status = RegQueryValueExW(key, value_name, nullptr, nullptr,
                            reinterpret_cast<LPBYTE>(value.data()), &size);
  if (status != ERROR_SUCCESS) {
    return {};
  }

  if (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return value;
}

LONG DeleteValue(HKEY key, const std::string& value_name) {
  const std::wstring wide_value_name = Widen(value_name);
  return RegDeleteValueW(key, wide_value_name.c_str());
}

bool DeleteLegacyValues(HKEY key) {
  bool changed = false;
  for (const char* legacy_name : kLegacyAutostartEntryNames) {
    changed = DeleteValue(key, legacy_name) == ERROR_SUCCESS || changed;
  }
  return changed;
}

bool DeleteStartupApprovedValues() {
  HKEY key = nullptr;
  const std::wstring subkey = Widen(kStartupApprovedRunLocation);
  const LONG open_status =
      RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_SET_VALUE, &key);
  if (open_status != ERROR_SUCCESS) {
    return false;
  }

  bool changed = DeleteValue(key, kAutostartEntryName) == ERROR_SUCCESS;
  changed = DeleteLegacyValues(key) || changed;
  RegCloseKey(key);
  return changed;
}
#endif

std::string BuildLaunchCommand(const std::string& executable_path) {
  return QuoteWindowsCommandArg(executable_path) + " " + kBackgroundArgument;
}

class AutostartManagerImpl final : public AutostartManager {
 public:
  CapabilityReport Probe() const override {
#if defined(_WIN32)
    HMODULE advapi32 = LoadLibraryW(L"advapi32.dll");
    const bool registry_ready =
        advapi32 != nullptr && GetProcAddress(advapi32, "RegCreateKeyExW") != nullptr &&
        GetProcAddress(advapi32, "RegQueryValueExW") != nullptr &&
        GetProcAddress(advapi32, "RegSetValueExW") != nullptr &&
        GetProcAddress(advapi32, "RegDeleteValueW") != nullptr;

    if (advapi32 != nullptr) {
      FreeLibrary(advapi32);
    }

    if (registry_ready) {
      return CapabilityReport{
          .component = "autostart",
          .status = CapabilityStatus::kReady,
          .detail =
              "HKCU Run registration is available and targets the background launch mode.",
      };
    }

    return CapabilityReport{
        .component = "autostart",
        .status = CapabilityStatus::kUnavailable,
        .detail =
            "Autostart registration is unavailable because registry APIs could not be resolved.",
    };
#else
    return CapabilityReport{
        .component = "autostart",
        .status = CapabilityStatus::kStubbed,
        .detail =
            "Windows Run-key autostart is disabled on non-Windows hosts; diagnostics still expose the intended launch command.",
    };
#endif
  }

  AutostartPlan BuildPlan(const std::string& executable_path) const override {
    return AutostartPlan{
        .scope = kAutostartScope,
        .location = kAutostartLocation,
        .entry_name = kAutostartEntryName,
        .launch_mode = kBackgroundArgument,
        .launch_command = BuildLaunchCommand(executable_path),
    };
  }

  AutostartRegistrationResult Enable(
      const std::string& executable_path) const override {
    const auto plan = BuildPlan(executable_path);
    if (executable_path.empty()) {
      return AutostartRegistrationResult{
          .success = false,
          .changed = false,
          .detail =
              "Autostart registration requires a concrete executable path.",
      };
    }

#if defined(_WIN32)
    HKEY key = nullptr;
    const std::wstring subkey = Widen("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    const LONG create_status =
        RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (create_status != ERROR_SUCCESS) {
      return AutostartRegistrationResult{
          .success = false,
          .changed = false,
          .detail =
              "Failed to open the current-user Run key for Locking Glass autostart.",
      };
    }

    const bool startup_cache_removed = DeleteStartupApprovedValues();
    const std::wstring value_name = Widen(plan.entry_name);
    const std::wstring expected_command = Widen(plan.launch_command);
    const bool legacy_run_removed = DeleteLegacyValues(key);
    const std::wstring existing_command = ReadValue(key, value_name.c_str());

    if (existing_command == expected_command) {
      RegCloseKey(key);
      return AutostartRegistrationResult{
          .success = true,
          .changed = startup_cache_removed || legacy_run_removed,
          .detail =
              startup_cache_removed || legacy_run_removed
                  ? "Refreshed current-user Locking Glass autostart metadata."
                  : "Locking Glass autostart was already enabled for the current user.",
      };
    }

    const LONG write_status = RegSetValueExW(
        key, value_name.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(expected_command.c_str()),
        static_cast<DWORD>((expected_command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);

    if (write_status != ERROR_SUCCESS) {
      return AutostartRegistrationResult{
          .success = false,
          .changed = false,
          .detail =
              "Failed to write the Locking Glass Run entry for background startup.",
      };
    }

    return AutostartRegistrationResult{
        .success = true,
        .changed = true,
        .detail = "Enabled current-user Locking Glass autostart in the Windows Run key.",
    };
#else
    return AutostartRegistrationResult{
        .success = false,
        .changed = false,
        .detail =
            "Autostart installation can only run on Windows because it writes the current-user Run key.",
    };
#endif
  }

  AutostartRegistrationResult Disable() const override {
#if defined(_WIN32)
    const bool startup_cache_removed = DeleteStartupApprovedValues();
    HKEY key = nullptr;
    const std::wstring subkey =
        Widen("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
    const LONG open_status =
        RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (open_status == ERROR_FILE_NOT_FOUND) {
      return AutostartRegistrationResult{
          .success = true,
          .changed = startup_cache_removed,
          .detail =
              startup_cache_removed
                  ? "Removed stale Locking Glass startup approval metadata."
                  : "Locking Glass autostart was already disabled for the current user.",
      };
    }
    if (open_status != ERROR_SUCCESS) {
      return AutostartRegistrationResult{
          .success = false,
          .changed = false,
          .detail =
              "Failed to open the current-user Run key for Locking Glass autostart removal.",
      };
    }

    const LONG delete_status = DeleteValue(key, kAutostartEntryName);
    const bool legacy_run_removed = DeleteLegacyValues(key);
    RegCloseKey(key);

    if (delete_status == ERROR_FILE_NOT_FOUND) {
      return AutostartRegistrationResult{
          .success = true,
          .changed = startup_cache_removed || legacy_run_removed,
          .detail =
              startup_cache_removed || legacy_run_removed
                  ? "Removed stale Locking Glass autostart metadata."
                  : "Locking Glass autostart was already disabled for the current user.",
      };
    }
    if (delete_status != ERROR_SUCCESS) {
      return AutostartRegistrationResult{
          .success = false,
          .changed = false,
          .detail =
              "Failed to remove the Locking Glass Run entry for background startup.",
      };
    }

    return AutostartRegistrationResult{
        .success = true,
        .changed = true,
        .detail = "Disabled current-user Locking Glass autostart in the Windows Run key.",
    };
#else
    return AutostartRegistrationResult{
        .success = false,
        .changed = false,
        .detail =
            "Autostart removal can only run on Windows because it writes the current-user Run key.",
    };
#endif
  }
};

}  // namespace

std::string QuoteWindowsCommandArg(const std::string& value) {
  if (value.empty()) {
    return "\"\"";
  }

  const bool needs_quotes = std::any_of(value.begin(), value.end(), [](const char ch) {
    return std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '"';
  });
  if (!needs_quotes) {
    return value;
  }

  std::string quoted;
  quoted.push_back('"');

  std::size_t backslash_count = 0;
  for (const char ch : value) {
    if (ch == '\\') {
      ++backslash_count;
      continue;
    }

    if (ch == '"') {
      quoted.append(backslash_count * 2 + 1, '\\');
      quoted.push_back('"');
      backslash_count = 0;
      continue;
    }

    quoted.append(backslash_count, '\\');
    backslash_count = 0;
    quoted.push_back(ch);
  }

  quoted.append(backslash_count * 2, '\\');
  quoted.push_back('"');
  return quoted;
}

std::unique_ptr<AutostartManager> CreateAutostartManager() {
  return std::make_unique<AutostartManagerImpl>();
}

}  // namespace locking_glass::integration
