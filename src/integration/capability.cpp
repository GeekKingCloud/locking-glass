#include "locking_glass/integration/capability.h"

namespace locking_glass::integration {

const char* ToString(const CapabilityStatus status) {
  switch (status) {
    case CapabilityStatus::kReady:
      return "ready";
    case CapabilityStatus::kUnavailable:
      return "unavailable";
    case CapabilityStatus::kStubbed:
      return "stubbed";
  }
  return "unknown";
}

}  // namespace locking_glass::integration
