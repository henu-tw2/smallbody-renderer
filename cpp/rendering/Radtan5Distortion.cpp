// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "Radtan5Distortion.h"

#include <Eigen/LU>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Isis::SmallBodyRender {
namespace {

constexpr const char *kInverseError = "DISTORTION_INVERSE_ERROR";

[[noreturn]] void reject(const std::string &reason) {
  throw std::runtime_error(std::string(kInverseError) + ": " + reason);
}

std::string precise(double value) {
  std::ostringstream stream;
  stream << std::setprecision(std::numeric_limits<double>::max_digits10)
         << value;
  return stream.str();
}

void requireFinite(double value, const char *name) {
  if (!std::isfinite(value)) {
    reject(std::string(name) + " must be finite");
  }
}

void requireFinitePixel(const Eigen::Vector2d &pixel, const char *name) {
  if (!pixel.allFinite()) {
    reject(std::string(name) + " sample and line must be finite");
  }
}

struct ForwardNormalized {
  Eigen::Vector2d observed;
  Eigen::Matrix2d jacobianIdealToObserved;
};

ForwardNormalized forwardNormalized(
    const Eigen::Vector2d &normalizedIdeal,
    const Radtan5Coefficients &coefficients) {
  const double x = normalizedIdeal.x();
  const double y = normalizedIdeal.y();

  // SPEC 7.2 ideal_to_observed, in dimensionless normalized coordinates:
  // r2 = x*x + y*y; r4 = r2*r2; r6 = r4*r2.
  const double r2 = x * x + y * y;
  const double r4 = r2 * r2;
  const double r6 = r4 * r2;
  // radialScale = 1 + k1*r2 + k2*r4 + k3*r6, where coefficients are ordered
  // [k1, k2, p1, p2, k3] and are dimensionless.
  const double radialScale =
      1.0 + coefficients.k1 * r2 + coefficients.k2 * r4 +
      coefficients.k3 * r6;
  // xObserved = x*radialScale + 2*p1*x*y + p2*(r2 + 2*x*x).
  const double xObserved =
      x * radialScale + 2.0 * coefficients.p1 * x * y +
      coefficients.p2 * (r2 + 2.0 * x * x);
  // yObserved = y*radialScale + p1*(r2 + 2*y*y) + 2*p2*x*y.
  const double yObserved =
      y * radialScale + coefficients.p1 * (r2 + 2.0 * y * y) +
      2.0 * coefficients.p2 * x * y;

  // dRadial/dx = 2*x*(k1 + 2*k2*r2 + 3*k3*r4), dimensionless.
  const double radialDerivativeScale =
      2.0 * (coefficients.k1 + 2.0 * coefficients.k2 * r2 +
             3.0 * coefficients.k3 * r4);
  const double radialX = x * radialDerivativeScale;
  const double radialY = y * radialDerivativeScale;

  // Analytic 2x2 d(ideal_to_observed)/d(x,y) used by the inverse Newton
  // solve. x/y remain sample/line normalized coordinates, respectively.
  Eigen::Matrix2d jacobian;
  jacobian(0, 0) = radialScale + x * radialX + 2.0 * coefficients.p1 * y +
                   6.0 * coefficients.p2 * x;
  jacobian(0, 1) = x * radialY + 2.0 * coefficients.p1 * x +
                   2.0 * coefficients.p2 * y;
  jacobian(1, 0) = y * radialX + 2.0 * coefficients.p1 * x +
                   2.0 * coefficients.p2 * y;
  jacobian(1, 1) = radialScale + y * radialY + 6.0 * coefficients.p1 * y +
                   2.0 * coefficients.p2 * x;
  return {{xObserved, yObserved}, jacobian};
}

bool jacobianIsSingular(const Eigen::Matrix2d &jacobian) {
  if (!jacobian.allFinite()) {
    return true;
  }
  const double scale = std::max(1.0, jacobian.cwiseAbs().maxCoeff());
  return std::abs(jacobian.determinant()) <=
         64.0 * std::numeric_limits<double>::epsilon() * scale * scale;
}

}  // namespace

Radtan5Distortion::Radtan5Distortion(PinholeIntrinsics intrinsics,
                                     Radtan5Coefficients coefficients)
    : intrinsics_(intrinsics), coefficients_(coefficients) {
  requireFinite(intrinsics_.fxPx, "fxPx");
  requireFinite(intrinsics_.fyPx, "fyPx");
  requireFinite(intrinsics_.cxPx, "cxPx");
  requireFinite(intrinsics_.cyPx, "cyPx");
  if (!(intrinsics_.fxPx > 0.0)) {
    reject("fxPx must be positive px");
  }
  if (!(intrinsics_.fyPx > 0.0)) {
    reject("fyPx must be positive px");
  }
  requireFinite(coefficients_.k1, "k1");
  requireFinite(coefficients_.k2, "k2");
  requireFinite(coefficients_.p1, "p1");
  requireFinite(coefficients_.p2, "p2");
  requireFinite(coefficients_.k3, "k3");
}

Eigen::Vector2d Radtan5Distortion::distortIdealPixel(
    const Eigen::Vector2d &idealPixelPx) const {
  requireFinitePixel(idealPixelPx, "idealPixelPx");

  // Pixel-to-normalized conversion preserves top-left, zero-based
  // pixel-center semantics: x is sample/right and y is line/down. Pixel
  // quantities are px; x/y and all radial/tangential terms are dimensionless.
  const Eigen::Vector2d normalizedIdeal(
      (idealPixelPx.x() - intrinsics_.cxPx) / intrinsics_.fxPx,
      (idealPixelPx.y() - intrinsics_.cyPx) / intrinsics_.fyPx);
  const ForwardNormalized normalizedObserved =
      forwardNormalized(normalizedIdeal, coefficients_);
  if (!normalizedObserved.observed.allFinite()) {
    reject("ideal_to_observed produced non-finite normalized coordinates");
  }

  // xObserved/yObserved return to observed sample/line pixels, still with no
  // origin, axis-direction, or pixel-center conversion.
  const Eigen::Vector2d observedPixel(
      intrinsics_.fxPx * normalizedObserved.observed.x() + intrinsics_.cxPx,
      intrinsics_.fyPx * normalizedObserved.observed.y() + intrinsics_.cyPx);
  requireFinitePixel(observedPixel, "ideal_to_observed result");
  return observedPixel;
}

DistortionInverseResult Radtan5Distortion::undistortObservedPixel(
    const Eigen::Vector2d &observedPixelPx, int maximumIterations,
    double roundTripTolerancePx) const {
  requireFinitePixel(observedPixelPx, "observedPixelPx");
  if (maximumIterations <= 0) {
    reject("maximumIterations must be positive");
  }
  if (!std::isfinite(roundTripTolerancePx) ||
      !(roundTripTolerancePx > 0.0)) {
    reject("roundTripTolerancePx must be finite and positive px");
  }

  // The observed pixel is the initial ideal-pixel estimate. Convert once to
  // normalized sample/line coordinates; these remain dimensionless in Newton.
  const Eigen::Vector2d normalizedObserved(
      (observedPixelPx.x() - intrinsics_.cxPx) / intrinsics_.fxPx,
      (observedPixelPx.y() - intrinsics_.cyPx) / intrinsics_.fyPx);
  Eigen::Vector2d normalizedIdeal = normalizedObserved;

  for (int iteration = 0; iteration < maximumIterations; ++iteration) {
    const Eigen::Vector2d idealPixel(
        intrinsics_.fxPx * normalizedIdeal.x() + intrinsics_.cxPx,
        intrinsics_.fyPx * normalizedIdeal.y() + intrinsics_.cyPx);
    const Eigen::Vector2d reobservedPixel = distortIdealPixel(idealPixel);
    const double roundTripError = (reobservedPixel - observedPixelPx).norm();
    if (std::isfinite(roundTripError) &&
        roundTripError <= roundTripTolerancePx) {
      return {idealPixel, iteration, roundTripError};
    }

    const ForwardNormalized forward =
        forwardNormalized(normalizedIdeal, coefficients_);
    const Eigen::Vector2d residual = forward.observed - normalizedObserved;
    if (!residual.allFinite() || jacobianIsSingular(forward.jacobianIdealToObserved)) {
      reject("Newton Jacobian is singular or non-finite");
    }
    const Eigen::Vector2d update =
        forward.jacobianIdealToObserved.fullPivLu().solve(residual);
    if (!update.allFinite()) {
      reject("Newton update is non-finite");
    }
    normalizedIdeal -= update;
    if (!normalizedIdeal.allFinite()) {
      reject("Newton ideal coordinate is non-finite");
    }
  }

  const Eigen::Vector2d idealPixel(
      intrinsics_.fxPx * normalizedIdeal.x() + intrinsics_.cxPx,
      intrinsics_.fyPx * normalizedIdeal.y() + intrinsics_.cyPx);
  const Eigen::Vector2d reobservedPixel = distortIdealPixel(idealPixel);
  const double roundTripError = (reobservedPixel - observedPixelPx).norm();
  if (std::isfinite(roundTripError) &&
      roundTripError <= roundTripTolerancePx) {
    return {idealPixel, maximumIterations, roundTripError};
  }
  reject("Newton inverse exhausted: maximumIterations=" +
         std::to_string(maximumIterations) + " roundTripErrorPx=" +
         precise(roundTripError) + " roundTripTolerancePx=" +
         precise(roundTripTolerancePx));
}

}  // namespace Isis::SmallBodyRender
