// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <string>

#include "RenderDataContract.h"

namespace Isis::SmallBodyRender {

struct DskMetadata {
  int centerId;
  int surfaceId;
  int frameId;
  std::string frameName;
  int vertexCount;
  int plateCount;
  double maximumVertexRadiusM;
};

DskMetadata inspectSingleType2Dsk(
    const DskSpecification &specification);

}  // namespace Isis::SmallBodyRender
