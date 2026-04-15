#pragma once

#include <memory>
#include <string>
#include <vector>

namespace locking_glass::platform {

struct MonitorBounds {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

struct MonitorDescriptor {
  std::string stable_id;
  std::string device_path;
  std::string edid_serial;
  std::string display_name;
  std::string label;
  MonitorBounds bounds;
  bool is_primary = false;
};

class MonitorGateway {
 public:
  virtual ~MonitorGateway() = default;
  virtual std::vector<MonitorDescriptor> Enumerate() const = 0;
};

std::unique_ptr<MonitorGateway> CreateMonitorGateway();

}  // namespace locking_glass::platform
