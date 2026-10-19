// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <filesystem>
#include <optional>

#include "Radtan5Distortion.h"
#include "RenderCamera.h"

namespace Isis::SmallBodyRender {

class TsaiPinholeCamera final : public RenderCamera {
  public:
    static TsaiPinholeCamera readCanonical(
        const std::filesystem::path &path,
        DistortionInverseControls inverseControls = {});

    Vector3 rayCamera(double sample, double line) const override;
    CameraRayStatistics rayStatistics() const override;
    Eigen::Vector2d projectJ2000(const Vector3 &pointJ2000M) const;
    void requireMatches(const FrameState &frame,
                        double centerToleranceM = 1.0e-6,
                        double attitudeToleranceRad = 1.0e-8) const;

    double fu() const;
    double fv() const;
    double cu() const;
    double cv() const;
    const Vector3 &centerJ2000M() const;
    const Matrix3 &rotationCameraFromJ2000() const;
    CameraModelFormat cameraModelFormat() const;

  private:
    TsaiPinholeCamera(double fu, double fv, double cu, double cv,
                      Vector3 centerJ2000M,
                      Matrix3 rotationCameraFromJ2000,
                      std::optional<Radtan5Distortion> distortion,
                      DistortionInverseControls inverseControls);

    double fu_;
    double fv_;
    double cu_;
    double cv_;
    Vector3 centerJ2000M_;
    Matrix3 rotationCameraFromJ2000_;
    std::optional<Radtan5Distortion> distortion_;
    DistortionInverseControls inverseControls_;
    mutable CameraRayStatistics rayStatistics_{0, 0, 0, 0.0};
};

}  // namespace Isis::SmallBodyRender
