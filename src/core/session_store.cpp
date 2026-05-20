#include "locking_glass/core/session_store.h"

#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace locking_glass::core {

bool MonitorBoundsEqual(const platform::MonitorBounds& left,
                        const platform::MonitorBounds& right) {
  return left.left == right.left && left.top == right.top &&
         left.right == right.right && left.bottom == right.bottom;
}

bool ExactMonitorIdentityEqual(const platform::MonitorDescriptor& left,
                               const platform::MonitorDescriptor& right) {
  return left.stable_id == right.stable_id &&
         left.device_path == right.device_path &&
         left.edid_serial == right.edid_serial &&
         left.display_name == right.display_name &&
         MonitorBoundsEqual(left.bounds, right.bounds) &&
         left.is_primary == right.is_primary;
}

namespace {

constexpr std::string_view kVersionTag = "version";
constexpr std::string_view kMonitorTag = "monitor";
constexpr std::string_view kFormatVersion = "1";

std::string EscapeField(const std::string_view field) {
  std::string escaped;
  escaped.reserve(field.size());

  for (const char character : field) {
    switch (character) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\t':
        escaped += "\\t";
        break;
      case '\n':
        escaped += "\\n";
        break;
      default:
        escaped.push_back(character);
        break;
    }
  }

  return escaped;
}

std::string UnescapeField(const std::string_view field) {
  std::string unescaped;
  unescaped.reserve(field.size());

  for (std::size_t index = 0; index < field.size(); ++index) {
    const char character = field[index];
    if (character != '\\' || index + 1 >= field.size()) {
      unescaped.push_back(character);
      continue;
    }

    ++index;
    switch (field[index]) {
      case '\\':
        unescaped.push_back('\\');
        break;
      case 't':
        unescaped.push_back('\t');
        break;
      case 'n':
        unescaped.push_back('\n');
        break;
      default:
        unescaped.push_back(field[index]);
        break;
    }
  }

  return unescaped;
}

std::vector<std::string> SplitFields(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t end = line.find('\t', start);
    if (end == std::string::npos) {
      fields.push_back(UnescapeField(std::string_view(line).substr(start)));
      break;
    }

    fields.push_back(
        UnescapeField(std::string_view(line).substr(start, end - start)));
    start = end + 1;
  }
  return fields;
}

void AppendEscapedField(std::ostringstream* builder, const std::string& field) {
  if (builder->tellp() > 0) {
    *builder << '\t';
  }
  *builder << EscapeField(field);
}

std::string BoolField(const bool value) { return value ? "1" : "0"; }

std::filesystem::path InvalidBackupPath(const std::filesystem::path& storage_path) {
  return storage_path.string() + ".invalid";
}

void AppendStorageDetail(std::string* detail, const std::string& suffix) {
  if (detail == nullptr || suffix.empty()) {
    return;
  }
  if (!detail->empty()) {
    *detail += ' ';
  }
  *detail += suffix;
}

bool ParseBool(const std::string& value, bool* parsed) {
  if (value == "1") {
    *parsed = true;
    return true;
  }
  if (value == "0") {
    *parsed = false;
    return true;
  }
  return false;
}

bool ParseInt(const std::string& value, int* parsed) {
  try {
    std::size_t consumed = 0;
    const int candidate = std::stoi(value, &consumed);
    if (consumed != value.size()) {
      return false;
    }
    *parsed = candidate;
    return true;
  } catch (...) {
    return false;
  }
}

bool HasValue(const std::string& value) { return !value.empty(); }

int MatchScore(const SessionMonitorState& saved,
               const platform::MonitorDescriptor& live) {
  if (saved.requires_confirmation &&
      ExactMonitorIdentityEqual(saved.monitor, live)) {
    return 1000;
  }

  int score = 0;
  if (HasValue(saved.monitor.device_path) &&
      saved.monitor.device_path == live.device_path) {
    score = 100;
  } else if (HasValue(saved.monitor.edid_serial) &&
             saved.monitor.edid_serial == live.edid_serial) {
    score = 80;
  } else if (HasValue(saved.monitor.stable_id) &&
             saved.monitor.stable_id == live.stable_id) {
    score = 60;
  } else if (HasValue(saved.monitor.display_name) &&
             saved.monitor.display_name == live.display_name &&
             MonitorBoundsEqual(saved.monitor.bounds, live.bounds)) {
    score = 40;
  } else if (HasValue(saved.monitor.display_name) &&
             saved.monitor.display_name == live.display_name &&
             saved.monitor.is_primary == live.is_primary) {
    score = 25;
  } else {
    return 0;
  }

  if (MonitorBoundsEqual(saved.monitor.bounds, live.bounds)) {
    score += 5;
  }
  if (saved.monitor.is_primary == live.is_primary) {
    score += 1;
  }
  return score;
}

SessionRefreshResult ReconcileSnapshot(
    SessionSnapshot snapshot,
    const std::vector<platform::MonitorDescriptor>& live_monitors,
    const std::filesystem::path& storage_path,
    const bool loaded_from_disk) {
  for (auto& monitor_state : snapshot.monitors) {
    monitor_state.is_present = false;
  }

  std::vector<bool> matched(snapshot.monitors.size(), false);
  SessionRefreshResult result{
      .snapshot = std::move(snapshot),
      .storage_path = storage_path,
      .loaded_from_disk = loaded_from_disk,
      .restored_locked_monitors = 0,
      .disconnected_monitors = 0,
      .new_monitors = 0,
      .review_monitors = 0,
      .storage_issue = SessionStorageIssue::kNone,
      .recovered_invalid_data = false,
      .invalid_storage_backup_path = {},
      .storage_detail = {},
  };

  for (const auto& live_monitor : live_monitors) {
    int best_score = 0;
    std::size_t best_index = 0;
    bool best_is_unique = true;

    // Monitor device IDs can drift after driver or topology changes, so the
    // restore path accepts a saved monitor only when the best fuzzy match is
    // unique. Ambiguous matches become new, unlocked monitors that require
    // confirmation instead of silently inheriting an old lock.
    for (std::size_t index = 0; index < result.snapshot.monitors.size(); ++index) {
      if (matched[index]) {
        continue;
      }

      const int candidate_score =
          MatchScore(result.snapshot.monitors[index], live_monitor);
      if (candidate_score == 0) {
        continue;
      }

      if (candidate_score > best_score) {
        best_score = candidate_score;
        best_index = index;
        best_is_unique = true;
        continue;
      }

      if (candidate_score == best_score) {
        best_is_unique = false;
      }
    }

    if (best_score > 0 && best_is_unique) {
      auto& matched_monitor = result.snapshot.monitors[best_index];
      matched[best_index] = true;
      matched_monitor.monitor = live_monitor;
      matched_monitor.is_present = true;
      if (matched_monitor.locked) {
        ++result.restored_locked_monitors;
      }
      if (matched_monitor.requires_confirmation) {
        ++result.review_monitors;
      }
      continue;
    }

    result.snapshot.monitors.push_back(SessionMonitorState{
        .monitor = live_monitor,
        .locked = false,
        .is_present = true,
        .requires_confirmation = true,
    });
    matched.push_back(true);
    ++result.new_monitors;
    ++result.review_monitors;
  }

  for (const auto& monitor_state : result.snapshot.monitors) {
    if (!monitor_state.is_present) {
      ++result.disconnected_monitors;
    }
  }

  return result;
}

bool ReplaceFile(const std::filesystem::path& source,
                 const std::filesystem::path& destination) {
#if defined(_WIN32)
  return ::MoveFileExW(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code rename_error;
  std::filesystem::rename(source, destination, rename_error);
  return !rename_error;
#endif
}

bool CopyFileWithOverwrite(const std::filesystem::path& source,
                           const std::filesystem::path& destination) {
#if defined(_WIN32)
  return ::CopyFileW(source.c_str(), destination.c_str(), FALSE) != 0;
#else
  std::error_code copy_error;
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::overwrite_existing,
                             copy_error);
  return !copy_error;
#endif
}

std::string SerializeMonitor(const SessionMonitorState& monitor_state) {
  std::ostringstream line;
  AppendEscapedField(&line, std::string(kMonitorTag));
  AppendEscapedField(&line, monitor_state.monitor.stable_id);
  AppendEscapedField(&line, monitor_state.monitor.device_path);
  AppendEscapedField(&line, monitor_state.monitor.edid_serial);
  AppendEscapedField(&line, monitor_state.monitor.display_name);
  AppendEscapedField(&line, monitor_state.monitor.label);
  AppendEscapedField(&line, std::to_string(monitor_state.monitor.bounds.left));
  AppendEscapedField(&line, std::to_string(monitor_state.monitor.bounds.top));
  AppendEscapedField(&line, std::to_string(monitor_state.monitor.bounds.right));
  AppendEscapedField(&line, std::to_string(monitor_state.monitor.bounds.bottom));
  AppendEscapedField(&line, BoolField(monitor_state.monitor.is_primary));
  AppendEscapedField(&line, BoolField(monitor_state.locked));
  AppendEscapedField(&line, BoolField(monitor_state.requires_confirmation));
  return line.str();
}

bool ParseMonitor(const std::vector<std::string>& fields,
                  SessionMonitorState* monitor_state) {
  if (fields.size() != 13U || fields[0] != kMonitorTag) {
    return false;
  }

  platform::MonitorBounds bounds;
  bool is_primary = false;
  bool locked = false;
  bool requires_confirmation = false;
  if (!ParseInt(fields[6], &bounds.left) || !ParseInt(fields[7], &bounds.top) ||
      !ParseInt(fields[8], &bounds.right) ||
      !ParseInt(fields[9], &bounds.bottom) ||
      !ParseBool(fields[10], &is_primary) || !ParseBool(fields[11], &locked) ||
      !ParseBool(fields[12], &requires_confirmation)) {
    return false;
  }

  *monitor_state = SessionMonitorState{
      .monitor =
          platform::MonitorDescriptor{
              .stable_id = fields[1],
              .device_path = fields[2],
              .edid_serial = fields[3],
              .display_name = fields[4],
              .label = fields[5],
              .bounds = bounds,
              .is_primary = is_primary,
          },
      .locked = locked,
      .is_present = false,
      .requires_confirmation = requires_confirmation,
  };
  return true;
}

void PreserveRejectedSessionFile(const std::filesystem::path& storage_path,
                                 SessionRefreshResult* result) {
  if (result == nullptr || result->storage_issue == SessionStorageIssue::kNone ||
      !result->loaded_from_disk) {
    return;
  }

  const auto backup_path = InvalidBackupPath(storage_path);
  if (!CopyFileWithOverwrite(storage_path, backup_path)) {
#if defined(_WIN32)
    const DWORD error_code = GetLastError();
    const std::error_code copy_error(static_cast<int>(error_code),
                                     std::system_category());
#else
    const std::error_code copy_error(errno, std::generic_category());
#endif
    AppendStorageDetail(
        &result->storage_detail,
        "Failed to preserve the rejected session file at " +
            backup_path.string() + ": " + copy_error.message() + '.');
  } else {
    result->invalid_storage_backup_path = backup_path;
  }
}

}  // namespace

const char* ToString(const SessionStorageIssue issue) {
  switch (issue) {
    case SessionStorageIssue::kNone:
      return "none";
    case SessionStorageIssue::kUnreadable:
      return "unreadable";
    case SessionStorageIssue::kMissingVersion:
      return "missing-version";
    case SessionStorageIssue::kUnsupportedVersion:
      return "unsupported-version";
    case SessionStorageIssue::kMalformedRecord:
      return "malformed-record";
  }

  return "unknown";
}

std::filesystem::path ResolveDefaultSessionPath() {
  if (const char* override_path = std::getenv("LOCKING_GLASS_SESSION_PATH");
      override_path != nullptr && override_path[0] != '\0') {
    return std::filesystem::path(override_path);
  }

#if defined(_WIN32)
  if (const char* local_app_data = std::getenv("LOCALAPPDATA");
      local_app_data != nullptr && local_app_data[0] != '\0') {
    return std::filesystem::path(local_app_data) / "Locking Glass" /
           "monitor-session-state.tsv";
  }
  if (const char* roaming_app_data = std::getenv("APPDATA");
      roaming_app_data != nullptr && roaming_app_data[0] != '\0') {
    return std::filesystem::path(roaming_app_data) / "Locking Glass" /
           "monitor-session-state.tsv";
  }
#endif

  if (const char* xdg_state_home = std::getenv("XDG_STATE_HOME");
      xdg_state_home != nullptr && xdg_state_home[0] != '\0') {
    return std::filesystem::path(xdg_state_home) / "locking-glass" /
           "monitor-session-state.tsv";
  }

  if (const char* home = std::getenv("HOME");
      home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / ".local" / "state" / "locking-glass" /
           "monitor-session-state.tsv";
  }

  return std::filesystem::current_path() / ".locking-glass" /
         "monitor-session-state.tsv";
}

SessionStore::SessionStore(std::filesystem::path storage_path)
    : storage_path_(std::move(storage_path)) {}

const std::filesystem::path& SessionStore::storage_path() const {
  return storage_path_;
}

locking_glass::integration::CapabilityReport SessionStore::Probe() const {
  return locking_glass::integration::CapabilityReport{
      .component = "session-store",
      .status = locking_glass::integration::CapabilityStatus::kReady,
      .detail = "Monitor session state persists at " + storage_path_.string() +
                "; app startup clears saved monitor locks.",
  };
}

SessionLoadResult SessionStore::Load() const {
  SessionLoadResult result;

  std::error_code exists_error;
  const bool storage_exists = std::filesystem::exists(storage_path_, exists_error);
  if (exists_error) {
    result.storage_issue = SessionStorageIssue::kUnreadable;
    result.storage_detail =
        "Failed to inspect session file: " + exists_error.message();
    return result;
  }

  result.loaded_from_disk = storage_exists;
  if (!storage_exists) {
    result.storage_detail = "No saved session file found.";
    return result;
  }

  std::ifstream input(storage_path_, std::ios::binary);
  if (!input.is_open()) {
    result.storage_issue = SessionStorageIssue::kUnreadable;
    result.storage_detail =
        std::string("Failed to open existing session file: ") +
        std::strerror(errno);
    return result;
  }

  auto reject = [&](const SessionStorageIssue issue,
                    std::string detail) -> SessionLoadResult {
    result.snapshot.monitors.clear();
    result.storage_issue = issue;
    result.storage_detail = std::move(detail);
    return result;
  };

  std::string line;
  bool version_ok = false;
  bool saw_content = false;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty()) {
      continue;
    }

    saw_content = true;
    const std::vector<std::string> fields = SplitFields(line);
    if (fields.empty()) {
      continue;
    }

    if (fields[0] == kVersionTag) {
      if (version_ok) {
        return reject(SessionStorageIssue::kMalformedRecord,
                      "Line " + std::to_string(line_number) +
                          ": duplicate session format header.");
      }
      if (fields.size() != 2U) {
        return reject(SessionStorageIssue::kMalformedRecord,
                      "Line " + std::to_string(line_number) +
                          ": malformed session format header.");
      }
      if (fields[1] != kFormatVersion) {
        return reject(SessionStorageIssue::kUnsupportedVersion,
                      "Line " + std::to_string(line_number) +
                          ": unsupported session format version \"" + fields[1] +
                          "\".");
      }
      version_ok = true;
      continue;
    }

    if (!version_ok) {
      return reject(SessionStorageIssue::kMissingVersion,
                    "Line " + std::to_string(line_number) +
                        ": session file is missing the required version header.");
    }

    SessionMonitorState monitor_state;
    if (!ParseMonitor(fields, &monitor_state)) {
      return reject(SessionStorageIssue::kMalformedRecord,
                    "Line " + std::to_string(line_number) +
                        ": malformed monitor record.");
    }

    result.snapshot.monitors.push_back(std::move(monitor_state));
  }

  if (!input.eof()) {
    return reject(SessionStorageIssue::kUnreadable,
                  "Failed while reading the session file.");
  }

  if (!version_ok) {
    return reject(SessionStorageIssue::kMissingVersion,
                  saw_content
                      ? "Session file ended before a valid version header was found."
                      : "Session file is empty and missing the required version header.");
  }

  result.storage_detail =
      "Loaded session file format version " + std::string(kFormatVersion) + ".";
  return result;
}

bool SessionStore::Save(const SessionSnapshot& snapshot) const {
  const auto parent = storage_path_.parent_path();
  if (!parent.empty()) {
    std::error_code create_error;
    std::filesystem::create_directories(parent, create_error);
    if (create_error) {
      return false;
    }
  }

  const auto temporary_path = storage_path_.string() + ".tmp";
  std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }

  output << kVersionTag << '\t' << kFormatVersion << '\n';
  for (const auto& monitor_state : snapshot.monitors) {
    output << SerializeMonitor(monitor_state) << '\n';
  }
  output.flush();
  if (!output.good()) {
    output.close();
    std::error_code cleanup_error;
    std::filesystem::remove(temporary_path, cleanup_error);
    return false;
  }
  output.close();

  if (!ReplaceFile(temporary_path, storage_path_)) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary_path, cleanup_error);
    return false;
  }

  return true;
}

SessionRefreshResult SessionStore::Preview(
    const std::vector<platform::MonitorDescriptor>& live_monitors) const {
  auto load_result = Load();
  auto result = ReconcileSnapshot(std::move(load_result.snapshot), live_monitors,
                                  storage_path_, load_result.loaded_from_disk);
  result.storage_issue = load_result.storage_issue;
  result.storage_detail = std::move(load_result.storage_detail);
  return result;
}

SessionRefreshResult SessionStore::StartUnlocked(
    const std::vector<platform::MonitorDescriptor>& live_monitors) const {
  auto load_result = Load();
  auto result = ReconcileSnapshot(std::move(load_result.snapshot), live_monitors,
                                  storage_path_, load_result.loaded_from_disk);
  result.storage_issue = load_result.storage_issue;
  result.storage_detail = std::move(load_result.storage_detail);
  PreserveRejectedSessionFile(storage_path_, &result);

  // Startup deliberately persists the unlocked state. A previous run may have
  // saved locks, but every new process starts inert until the user locks a
  // monitor from the tray in this run.
  for (auto& monitor_state : result.snapshot.monitors) {
    monitor_state.locked = false;
  }
  result.restored_locked_monitors = 0;

  if (!Save(result.snapshot)) {
    AppendStorageDetail(&result.storage_detail,
                        "Failed to write the startup-unlocked session file.");
  } else if (result.storage_issue != SessionStorageIssue::kNone) {
    result.recovered_invalid_data = true;
    AppendStorageDetail(&result.storage_detail,
                        "Rebuilt the active session file from live monitor state.");
    if (!result.invalid_storage_backup_path.empty()) {
      AppendStorageDetail(&result.storage_detail,
                          "Rejected data was copied to " +
                              result.invalid_storage_backup_path.string() + '.');
    }
  }
  return result;
}

SessionRefreshResult SessionStore::Restore(
    const std::vector<platform::MonitorDescriptor>& live_monitors) const {
  auto load_result = Load();
  auto result = ReconcileSnapshot(std::move(load_result.snapshot), live_monitors,
                                  storage_path_, load_result.loaded_from_disk);
  result.storage_issue = load_result.storage_issue;
  result.storage_detail = std::move(load_result.storage_detail);

  // Rejected session files are preserved for inspection, then the active file
  // is rebuilt from live monitors. Bad persistence must not resurrect old locks.
  PreserveRejectedSessionFile(storage_path_, &result);

  const bool save_ok = Save(result.snapshot);
  if (!save_ok) {
    AppendStorageDetail(&result.storage_detail,
                        "Failed to write the reconciled session file.");
  } else if (result.storage_issue != SessionStorageIssue::kNone) {
    result.recovered_invalid_data = true;
    AppendStorageDetail(&result.storage_detail,
                        "Rebuilt the active session file from live monitor state.");
    if (!result.invalid_storage_backup_path.empty()) {
      AppendStorageDetail(&result.storage_detail,
                          "Rejected data was copied to " +
                              result.invalid_storage_backup_path.string() + '.');
    }
  }
  return result;
}

bool SessionStore::SetLocked(SessionSnapshot* snapshot,
                             const platform::MonitorDescriptor& monitor,
                             const bool locked) const {
  if (snapshot == nullptr) {
    return false;
  }

  SessionMonitorState* best_match = nullptr;
  int best_score = 0;
  for (auto& monitor_state : snapshot->monitors) {
    if (ExactMonitorIdentityEqual(monitor_state.monitor, monitor)) {
      best_match = &monitor_state;
      break;
    }

    const int candidate_score = MatchScore(monitor_state, monitor);
    if (candidate_score > best_score) {
      best_score = candidate_score;
      best_match = &monitor_state;
    }
  }

  if (best_match == nullptr) {
    return false;
  }

  best_match->monitor = monitor;
  best_match->locked = locked;
  // A tray toggle is the user's confirmation boundary for both lock and unlock.
  best_match->requires_confirmation = false;
  return true;
}

}  // namespace locking_glass::core
