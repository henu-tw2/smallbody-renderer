// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <cstdint>

namespace Isis::SmallBodyRender {

// Relative albedo is persisted as IEEE float32. A value is renderable only
// when that conversion remains finite and strictly positive.
bool isRenderableRelativeAlbedo(double value);

class SurfaceAppearance {
 public:
  virtual ~SurfaceAppearance() = default;
  virtual double relativeAlbedo(std::uint32_t plateId) const = 0;
};

class UniformSurfaceAppearance final : public SurfaceAppearance {
 public:
  // plateCount is the positive Type-2 DSK plate count; accepted IDs are
  // one-based and inclusive in 1..plateCount.
  UniformSurfaceAppearance(double relativeAlbedo, std::uint32_t plateCount);
  double relativeAlbedo(std::uint32_t plateId) const override;

 private:
  double relativeAlbedo_;
  std::uint32_t plateCount_;
};

}  // namespace Isis::SmallBodyRender
