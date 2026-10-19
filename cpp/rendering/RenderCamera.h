// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <cstdint>

#include "RenderDataContract.h"

namespace Isis::SmallBodyRender {

struct CameraRayStatistics {
  std::uint64_t requestedPixelCount;
  std::uint64_t inverseFailureCount;
  int maximumInverseIterations;
  double maximumRoundTripErrorPx;
};

class RenderCamera {
  public:
    virtual ~RenderCamera() = default;
    virtual Vector3 rayCamera(double samplePx,
                              double linePx) const = 0;
    virtual CameraRayStatistics rayStatistics() const {
      return {0, 0, 0, 0.0};
    }
};

}  // namespace Isis::SmallBodyRender
