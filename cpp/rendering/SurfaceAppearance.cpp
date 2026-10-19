// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "SurfaceAppearance.h"

#include <cmath>
#include <stdexcept>

namespace Isis::SmallBodyRender {

bool isRenderableRelativeAlbedo(double value) {
  if (!std::isfinite(value) || !(value > 0.0)) return false;
  const float stored = static_cast<float>(value);
  return std::isfinite(stored) && stored > 0.0F;
}

UniformSurfaceAppearance::UniformSurfaceAppearance(
    double relativeAlbedo, std::uint32_t plateCount)
    : relativeAlbedo_(relativeAlbedo), plateCount_(plateCount) {
  if (!isRenderableRelativeAlbedo(relativeAlbedo_)) {
    throw std::invalid_argument(
        "surface appearance relative albedo must remain finite and positive "
        "in IEEE float32");
  }
  if (plateCount_ == 0) {
    throw std::invalid_argument("plateCount must be positive");
  }
}

double UniformSurfaceAppearance::relativeAlbedo(std::uint32_t plateId) const {
  if (plateId == 0 || plateId > plateCount_) {
    throw std::invalid_argument("plateId is outside 1..plateCount");
  }
  return relativeAlbedo_;
}

}  // namespace Isis::SmallBodyRender
