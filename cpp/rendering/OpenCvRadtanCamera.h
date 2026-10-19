// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <filesystem>

#include "Radtan5Distortion.h"
#include "RenderCamera.h"

namespace Isis::SmallBodyRender {

// Strict OpenCV radtan5 intrinsics/distortion reader. J2000 position and
// attitude are deliberately not stored here; they are supplied by FrameState.
class OpenCvRadtanCamera final : public RenderCamera {
  public:
    static OpenCvRadtanCamera read(
        const std::filesystem::path &path, const FrameState &frame,
        DistortionInverseControls inverseControls = {});

    Vector3 rayCamera(double samplePx, double linePx) const override;
    CameraRayStatistics rayStatistics() const override;

  private:
    OpenCvRadtanCamera(PinholeIntrinsics intrinsics,
                       Radtan5Coefficients coefficients,
                       DistortionInverseControls inverseControls);

    PinholeIntrinsics intrinsics_;
    Radtan5Distortion distortion_;
    DistortionInverseControls inverseControls_;
    mutable CameraRayStatistics rayStatistics_{0, 0, 0, 0.0};
};

}  // namespace Isis::SmallBodyRender
