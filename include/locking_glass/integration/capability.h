#pragma once

#include <string>

namespace locking_glass::integration {

enum class CapabilityStatus {
  kReady,
  kUnavailable,
  kStubbed,
};

struct CapabilityReport {
  std::string component;
  CapabilityStatus status;
  std::string detail;
};

const char* ToString(CapabilityStatus status);

}  // namespace locking_glass::integration
