// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "SolarLambert.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace Isis::SmallBodyRender {
namespace {

Vector3 checkedUnit(const Vector3 &vector, const std::string &field) {
  if (!vector.allFinite()) {
    throw std::invalid_argument(field +
                                ": must be a finite nonzero vector");
  }
  const double scale = vector.cwiseAbs().maxCoeff();
  if (!std::isfinite(scale) || !(scale > 0.0)) {
    throw std::invalid_argument(field +
                                ": must be a finite nonzero vector");
  }
  const Vector3 scaled = vector / scale;
  const double scaledNorm = scaled.norm();
  if (!scaled.allFinite() || !std::isfinite(scaledNorm) ||
      !(scaledNorm > 0.0)) {
    throw std::invalid_argument(field +
                                ": must be a finite nonzero vector");
  }
  const Vector3 unit = scaled / scaledNorm;
  if (!unit.allFinite()) {
    throw std::invalid_argument(field +
                                ": must be a finite nonzero vector");
  }
  return unit;
}

void requireFiniteVector(const Vector3 &vector,
                         const std::string &field) {
  if (!vector.allFinite()) {
    throw std::invalid_argument(field + ": must be finite");
  }
}

void requireFinitePositive(double value, const std::string &field) {
  if (!std::isfinite(value) || !(value > 0.0)) {
    throw std::invalid_argument(field + ": must be finite and positive");
  }
}

void requireSolarPhotometry(const Photometry &photometry) {
  if (!std::isfinite(photometry.ambient) ||
      !(photometry.ambient > 0.0)) {
    throw std::invalid_argument(
        "photometry.ambient: must be finite and positive");
  }
  if (!std::isfinite(photometry.diffuse) ||
      photometry.diffuse < 0.0) {
    throw std::invalid_argument(
        "photometry.diffuse: must be finite and non-negative");
  }
  if (std::abs(photometry.ambient + photometry.diffuse - 1.0) >
      1.0e-12) {
    throw std::invalid_argument(
        "photometry: ambient + diffuse must equal one");
  }
}

}  // namespace

SolarGeometry solarGeometry(const Vector3 &normalBody,
                            const Vector3 &hitBodyM,
                            const Vector3 &targetToSunBodyM) {
  // lightDirectionBody = normalize(targetToSunBodyM - hitBodyM)
  // mu0 = max(0, dot(normalize(normalBody), lightDirectionBody))
  //
  // hitBodyM and targetToSunBodyM are positions in metres, both expressed
  // in DSK body-fixed frame D from the DSK shape center. normalBody is the
  // outward surface normal in D and is normalized here so its input scale
  // cannot change mu0. lightDirectionBody points from the hit to the point
  // Sun and is dimensionless. A point Sun has no penumbra.
  const Vector3 normal = checkedUnit(normalBody, "normalBody");
  requireFiniteVector(hitBodyM, "hitBodyM");
  checkedUnit(targetToSunBodyM, "targetToSunBodyM");
  const Vector3 lightDirection =
      checkedUnit(targetToSunBodyM - hitBodyM, "lightDirectionBody");
  const double mu0 = std::clamp(normal.dot(lightDirection), 0.0, 1.0);
  return {lightDirection, mu0,
          mu0 > 0.0 ? IlluminationClass::Direct
                    : IlluminationClass::Night};
}

std::uint16_t solarLambertDn(double mu0, bool occluded,
                            double relativeAlbedo,
                            double exposureScale,
                            const Photometry &photometry) {
  if (!std::isfinite(mu0) || mu0 < 0.0 || mu0 > 1.0) {
    throw std::invalid_argument("mu0: must be finite and in [0, 1]");
  }
  requireFinitePositive(relativeAlbedo, "relativeAlbedo");
  requireFinitePositive(exposureScale, "exposureScale");
  requireSolarPhotometry(photometry);

  // visibility = occluded ? 0 : 1
  // normalized = exposureScale * relativeAlbedo
  //            * (ambient + diffuse * mu0 * visibility)
  // dn = max(1, round(65535 * clamp(normalized, 0, 1)))
  //
  // mu0 and all photometric factors are dimensionless. Occlusion removes
  // only direct light, so both a self-shadowed point and a night-side point
  // retain positive ambient DN. Background DN=0 is assigned by the caller.
  const double visibility = occluded ? 0.0 : 1.0;
  const double normalized =
      exposureScale * relativeAlbedo *
      (photometry.ambient +
       photometry.diffuse * mu0 * visibility);
  const auto rounded = static_cast<std::uint32_t>(std::llround(
      65535.0 * std::clamp(normalized, 0.0, 1.0)));
  return static_cast<std::uint16_t>(std::max<std::uint32_t>(1, rounded));
}

}  // namespace Isis::SmallBodyRender
