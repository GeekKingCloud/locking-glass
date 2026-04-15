#pragma once

#include <memory>

#include "locking_glass/integration/capability.h"

namespace locking_glass::platform {

class BackgroundSession {
 public:
  virtual ~BackgroundSession() = default;
  virtual locking_glass::integration::CapabilityReport Probe() const = 0;
  virtual int Run() const = 0;
};

std::unique_ptr<BackgroundSession> CreateBackgroundSession();

}  // namespace locking_glass::platform
