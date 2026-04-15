#pragma once

#include <memory>

#include "locking_glass/integration/capability.h"

namespace locking_glass::integration {

class FfmpegProbe {
 public:
  virtual ~FfmpegProbe() = default;
  virtual CapabilityReport Probe() const = 0;
};

std::unique_ptr<FfmpegProbe> CreateFfmpegProbe();

}  // namespace locking_glass::integration
