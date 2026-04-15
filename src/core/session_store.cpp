#include "locking_glass/core/session_store.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace locking_glass::core {

namespace {

constexpr std::string_view kVersionTag = "version";
constexpr std::string_view kMonitorTag = "monitor";
constexpr std::string_view kFormatVersion = "1";

bool BoundsEqual(const platform::MonitorBounds& left,
                 const platform::MonitorBounds& right) {
  return left.left == right.left && left.top == right.top &&
         left.right == right.right && left.bottom == right.bottom;
}

bool IdentityEqual(const platform::MonitorDescriptor& left,
                   const platform::MonitorDescriptor& right) {
  return left.stable_id == right.stable_id &&
         left.device_path == right.device_path &&
         left.edid_serial == right.edid_serial &&
         left.display_name == right.display_name &&
         BoundsEqual(left.bounds, right.bounds) &&
         left.is_primary == right.is_primary;
}

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
  if (saved.requires_confirmation && IdentityEqual(saved.monitor, live)) {
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
             BoundsEqual(saved.monitor.bounds, live.bounds)) {
    score = 40;
  } else if (HasValue(saved.monitor.display_name) &&
             saved.monitor.display_name == live.display_name &&
             saved.monitor.is_primary == live.is_primary) {
    score = 25;
  } else {
    return 0;
  }

  if (BoundsEqual(saved.monitor.bounds, live.bounds)) {
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
  };

  for (const auto& live_monitor : live_monitors) {
    int best_score = 0;
    std::size_t best_index = 0;
    bool best_is_unique = true;

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

}  // namespace

std::filesystem::path ResolveDefaultSessionPath() {
  if (const char* override_path = std::getenv("LOCKING_GLASS_SESSION_PATH");
      override_path != nullptr && override_path[0] != '\0') {
    return std::filesystem::path(override_path);
  }

#if defined(_WIN32)
  if (const char* local_app_data = std::getenv("LOCALAPPDATA");
      local_app_data != nullptr && local_app_data[0] != '\0') {
    return std::filesystem::path(local_app_data) / "LockingGlass" /
           "monitor-session-state.tsv";
  }
  if (const char* roaming_app_data = std::getenv("APPDATA");
      roaming_app_data != nullptr && roaming_app_data[0] != '\0') {
    return std::filesystem::path(roaming_app_data) / "LockingGlass" /
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
      .detail = "Monitor lock state persists at " + storage_path_.string(),
  };
}

SessionSnapshot SessionStore::Load() const {
  SessionSnapshot snapshot;
  std::ifstream input(storage_path_);
  if (!input.is_open()) {
    return snapshot;
  }

  std::string line;
  bool version_ok = false;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }

    const std::vector<std::string> fields = SplitFields(line);
    if (fields.empty()) {
      continue;
    }

    if (fields[0] == kVersionTag) {
      version_ok = fields.size() == 2U && fields[1] == kFormatVersion;
      continue;
    }

    if (!version_ok) {
      snapshot.monitors.clear();
      return snapshot;
    }

    SessionMonitorState monitor_state;
    if (ParseMonitor(fields, &monitor_state)) {
      snapshot.monitors.push_back(std::move(monitor_state));
    }
  }

  return snapshot;
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
    return false;
  }
  output.close();

  std::error_code remove_error;
  std::filesystem::remove(storage_path_, remove_error);

  std::error_code rename_error;
  std::filesystem::rename(temporary_path, storage_path_, rename_error);
  if (rename_error) {
    std::error_code cleanup_error;
    std::filesystem::remove(temporary_path, cleanup_error);
    return false;
  }

  return true;
}

SessionRefreshResult SessionStore::Preview(
    const std::vector<platform::MonitorDescriptor>& live_monitors) const {
  const bool loaded_from_disk = std::filesystem::exists(storage_path_);
  return ReconcileSnapshot(Load(), live_monitors, storage_path_,
                           loaded_from_disk);
}

SessionRefreshResult SessionStore::Restore(
    const std::vector<platform::MonitorDescriptor>& live_monitors) const {
  auto result = Preview(live_monitors);
  Save(result.snapshot);
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
    if (IdentityEqual(monitor_state.monitor, monitor)) {
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
  best_match->requires_confirmation = false;
  return true;
}

}  // namespace locking_glass::core
