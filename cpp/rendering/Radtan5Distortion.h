// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <Eigen/Core>

namespace Isis::SmallBodyRender {

// Pinhole intrinsics in pixels. Pixels use the renderer convention: a
// top-left, zero-based, pixel-center coordinate system; sample increases to
// the right and line increases downward. No one-based or half-pixel offset is
// applied by this math layer.
struct PinholeIntrinsics {
  double fxPx;
  double fyPx;
  double cxPx;
  double cyPx;
};

// OpenCV radtan5 coefficients in this fixed order: [k1, k2, p1, p2, k3].
// They are dimensionless because they operate on dimensionless normalized
// ideal coordinates; only PinholeIntrinsics and returned pixel vectors use px.
struct Radtan5Coefficients {
  double k1;
  double k2;
  double p1;
  double p2;
  double k3;
};

struct DistortionInverseResult {
  // Ideal sample/line in the top-left, zero-based pixel-center system, in px.
  Eigen::Vector2d idealPixelPx;
  // Number of Newton updates performed; this is zero when the initial guess
  // already meets the requested forward round-trip tolerance.
  int iterations;
  // Euclidean observed-pixel residual after the final ideal_to_observed pass.
  double roundTripErrorPx;
};

// Pure ideal_to_observed OpenCV radtan5 pixel distortion. The inverse uses a
// two-dimensional Newton solve and never changes coordinate origin, sample or
// line direction. Construction and both public methods reject non-finite
// inputs; construction also rejects fxPx <= 0 or fyPx <= 0. The inverse also
// rejects maximumIterations <= 0, non-positive/non-finite tolerance, a
// singular/non-finite Jacobian, a non-finite Newton step, and exhaustion when
// the measured final forward round trip exceeds tolerance. Every rejection
// includes the stable diagnostic token DISTORTION_INVERSE_ERROR.
class Radtan5Distortion final {
  public:
    Radtan5Distortion(PinholeIntrinsics intrinsics,
                      Radtan5Coefficients coefficients);

    // Maps an ideal (undistorted) [sample, line] pixel to its observed
    // (distorted) [sample, line] pixel. Inputs and output are px.
    Eigen::Vector2d distortIdealPixel(
        const Eigen::Vector2d &idealPixelPx) const;

    // Solves observed_to_ideal by Newton iteration, then verifies the
    // ideal_to_observed pixel residual against roundTripTolerancePx.
    DistortionInverseResult undistortObservedPixel(
        const Eigen::Vector2d &observedPixelPx, int maximumIterations,
        double roundTripTolerancePx) const;

  private:
    PinholeIntrinsics intrinsics_;
    Radtan5Coefficients coefficients_;
};

}  // namespace Isis::SmallBodyRender
