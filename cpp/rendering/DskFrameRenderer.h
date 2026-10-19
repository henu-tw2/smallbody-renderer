// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <cstdint>

#include "BulletDskRayTracer.h"
#include "RenderCamera.h"
#include "SurfaceAppearance.h"
#include "TiffProductWriter.h"

namespace Isis::SmallBodyRender {

struct RenderSummary {
  std::uint64_t totalPixels;
  std::uint64_t validPixels;
  std::uint16_t minimumValidDn;
  std::uint16_t maximumValidDn;
  double minimumDepthM;
  double maximumDepthM;
  std::uint64_t distinctPlateCount;
  std::uint64_t directPixels;
  std::uint64_t nightPixels;
  std::uint64_t selfShadowPixels;
};

class DskFrameRenderer {
  public:
    RenderSummary render(const ResolvedRenderConfig &config,
                         const RenderCamera &camera,
                         const SurfaceRayTracer &tracer,
                         const SurfaceAppearance &appearance,
                         TiffProductWriter &writer) const;
};

}  // namespace Isis::SmallBodyRender
