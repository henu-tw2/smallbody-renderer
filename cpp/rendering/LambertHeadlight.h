// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <cstdint>

#include "RenderDataContract.h"

namespace Isis::SmallBodyRender {

std::uint16_t lambertHeadlightDn(
    const Vector3 &normalBody, const Vector3 &hitBodyM,
    const Vector3 &cameraOriginBodyM, const Photometry &photometry);

Vector3 normalBodyToCamera(
    const Vector3 &normalBody, const Matrix3 &rotationJ2000FromDsk,
    const Matrix3 &rotationJ2000FromCamera);

}  // namespace Isis::SmallBodyRender
