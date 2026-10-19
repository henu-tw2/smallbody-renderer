// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <cstdint>

#include "RenderDataContract.h"

namespace Isis::SmallBodyRender {

enum class IlluminationClass : std::uint8_t {
  Background = 0,
  Direct = 1,
  Night = 2,
  SelfShadow = 3
};

struct SolarGeometry {
  Vector3 lightDirectionBody;
  double mu0;
  IlluminationClass classification;
};

// All vectors use DSK body-fixed axes and the DSK shape-center origin.
// Positions are metres; lightDirectionBody is a dimensionless unit vector
// directed from the surface hit toward the point Sun.
SolarGeometry solarGeometry(const Vector3 &normalBody,
                            const Vector3 &hitBodyM,
                            const Vector3 &targetToSunBodyM);

// Returns DN for a valid surface pixel. Background DN=0 is owned by the
// caller; occlusion removes direct visibility without removing ambient light.
std::uint16_t solarLambertDn(double mu0, bool occluded,
                            double relativeAlbedo,
                            double exposureScale,
                            const Photometry &photometry);

}  // namespace Isis::SmallBodyRender
