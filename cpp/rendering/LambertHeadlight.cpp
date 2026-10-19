// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "LambertHeadlight.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace Isis::SmallBodyRender {
namespace {

Vector3 checkedUnit(const Vector3 &vector, const std::string &field) {
  const double norm = vector.norm();
  if (!vector.allFinite() || !std::isfinite(norm) || !(norm > 0.0)) {
    throw std::invalid_argument(field +
                                ": must be a finite nonzero vector");
  }
  return vector / norm;
}

void requirePhotometry(const Photometry &photometry) {
  if (!std::isfinite(photometry.ambient) || photometry.ambient < 0.0) {
    throw std::invalid_argument(
        "photometry.ambient: must be finite and non-negative");
  }
  if (!std::isfinite(photometry.diffuse) || photometry.diffuse < 0.0) {
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

std::uint16_t lambertHeadlightDn(
    const Vector3 &normalBody, const Vector3 &hitBodyM,
    const Vector3 &cameraOriginBodyM, const Photometry &photometry) {
  requirePhotometry(photometry);
  const Vector3 normal = checkedUnit(normalBody, "normalBody");
  const Vector3 view =
      checkedUnit(cameraOriginBodyM - hitBodyM, "view");
  const double cosineIncidence = std::max(0.0, normal.dot(view));
  const double linear =
      std::clamp(photometry.ambient +
                     photometry.diffuse * cosineIncidence,
                 0.0, 1.0);
  return static_cast<std::uint16_t>(
      std::llround(linear * 65535.0));
}

Vector3 normalBodyToCamera(
    const Vector3 &normalBody, const Matrix3 &rotationJ2000FromDsk,
    const Matrix3 &rotationJ2000FromCamera) {
  if (!normalBody.allFinite() ||
      std::abs(normalBody.norm() - 1.0) > 1.0e-10) {
    throw std::invalid_argument(
        "normalBody: must be finite and unit length within 1e-10");
  }
  const Vector3 normalCamera =
      rotationJ2000FromCamera.transpose() *
      rotationJ2000FromDsk * normalBody;
  if (!normalCamera.allFinite() ||
      std::abs(normalCamera.norm() - 1.0) > 1.0e-10) {
    throw std::invalid_argument(
        "normalCamera: transformed normal must remain unit length "
        "within 1e-10");
  }
  return normalCamera;
}

}  // namespace Isis::SmallBodyRender
