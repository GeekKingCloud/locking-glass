#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "locking_glass/integration/capability.h"
#include "locking_glass/platform/monitor_gateway.h"

namespace locking_glass::core {

bool MonitorBoundsEqual(const platform::MonitorBounds& left,
                        const platform::MonitorBounds& right);
bool ExactMonitorIdentityEqual(const platform::MonitorDescriptor& left,
                               const platform::MonitorDescriptor& right);

struct SessionMonitorState {
  platform::MonitorDescriptor monitor;
  bool locked = false;
  bool is_present = false;
  bool requires_confirmation = false;
};

struct SessionSnapshot {
  std::vector<SessionMonitorState> monitors;
};

enum class SessionStorageIssue {
  kNone,
  kUnreadable,
  kMissingVersion,
  kUnsupportedVersion,
  kMalformedRecord,
};

const char* ToString(SessionStorageIssue issue);

struct SessionLoadResult {
  SessionSnapshot snapshot;
  bool loaded_from_disk = false;
  SessionStorageIssue storage_issue = SessionStorageIssue::kNone;
  std::string storage_detail;
};

struct SessionRefreshResult {
  SessionSnapshot snapshot;
  std::filesystem::path storage_path;
  bool loaded_from_disk = false;
  std::size_t restored_locked_monitors = 0;
  std::size_t disconnected_monitors = 0;
  std::size_t new_monitors = 0;
  std::size_t review_monitors = 0;
  SessionStorageIssue storage_issue = SessionStorageIssue::kNone;
  bool recovered_invalid_data = false;
  std::filesystem::path invalid_storage_backup_path;
  std::string storage_detail;
};

std::filesystem::path ResolveDefaultSessionPath();

class SessionStore {
 public:
  explicit SessionStore(
      std::filesystem::path storage_path = ResolveDefaultSessionPath());

  const std::filesystem::path& storage_path() const;
  locking_glass::integration::CapabilityReport Probe() const;
  SessionLoadResult Load() const;
  bool Save(const SessionSnapshot& snapshot) const;
  // Preview is read-only diagnostics, StartUnlocked is process startup policy,
  // and Restore is in-run reconciliation that may preserve active locks.
  SessionRefreshResult Preview(
      const std::vector<platform::MonitorDescriptor>& live_monitors) const;
  SessionRefreshResult StartUnlocked(
      const std::vector<platform::MonitorDescriptor>& live_monitors) const;
  SessionRefreshResult Restore(
      const std::vector<platform::MonitorDescriptor>& live_monitors) const;
  bool SetLocked(SessionSnapshot* snapshot,
                 const platform::MonitorDescriptor& monitor,
                 bool locked) const;

 private:
  std::filesystem::path storage_path_;
};

}  // namespace locking_glass::core
