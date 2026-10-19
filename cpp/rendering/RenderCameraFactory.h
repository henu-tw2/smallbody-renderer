// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <memory>

#include "RenderCamera.h"

namespace Isis::SmallBodyRender {

std::unique_ptr<RenderCamera> loadRenderCamera(
    const FrameState &frame,
    const DistortionInverseControls &inverseControls = {});

}  // namespace Isis::SmallBodyRender
