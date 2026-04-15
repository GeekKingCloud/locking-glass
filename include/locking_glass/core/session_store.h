#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

#include "locking_glass/integration/capability.h"
#include "locking_glass/platform/monitor_gateway.h"

namespace locking_glass::core {

struct SessionMonitorState {
  platform::MonitorDescriptor monitor;
  bool locked = false;
  bool is_present = false;
  bool requires_confirmation = false;
};

struct SessionSnapshot {
  std::vector<SessionMonitorState> monitors;
};

struct SessionRefreshResult {
  SessionSnapshot snapshot;
  std::filesystem::path storage_path;
  bool loaded_from_disk = false;
  std::size_t restored_locked_monitors = 0;
  std::size_t disconnected_monitors = 0;
  std::size_t new_monitors = 0;
  std::size_t review_monitors = 0;
};

std::filesystem::path ResolveDefaultSessionPath();

class SessionStore {
 public:
  explicit SessionStore(
      std::filesystem::path storage_path = ResolveDefaultSessionPath());

  const std::filesystem::path& storage_path() const;
  locking_glass::integration::CapabilityReport Probe() const;
  SessionSnapshot Load() const;
  bool Save(const SessionSnapshot& snapshot) const;
  SessionRefreshResult Preview(
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
