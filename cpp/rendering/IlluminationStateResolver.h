// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "RenderDataContract.h"

namespace Isis::SmallBodyRender {

// Computes target_to_secondary_J = secondary_absolute_J -
// target_absolute_J. Both operands must be finite positions in metres, at the
// same ET/TDB epoch, relative to the same caller-declared reference origin,
// and expressed on J2000 axes. This arithmetic has no quaternion convention.
// Non-finite operands are rejected.
Vector3 targetToSecondary(const Vector3 &targetAbsoluteJ2000M,
                          const Vector3 &secondaryAbsoluteJ2000M);

struct IlluminationResolutionSummary {
  std::size_t frameCount;
  // The required common-origin declaration supplied by the caller. The two
  // exact-column CSV inputs do not contain origin metadata, so this value is
  // validated syntactically and retained as an audit receipt, not inferred or
  // independently verified from the input data.
  std::string referenceOrigin;
  // Lowercase hexadecimal SHA-256 of the exact published CSV bytes.
  std::string outputSha256;
};

// Reads exact-column target and Sun absolute-state CSV files. Each row is
// frame_id, ET/TDB seconds, and a finite absolute position in metres on J2000
// axes. Frame sets must match, frame IDs must be unique, and per-frame ET
// values may differ by at most 1e-6 s. The common reference origin is a
// required caller declaration. The output direction is target to Sun, with
// J2000 axes and metres, and uses the target table's ET/TDB value.
//
// The exact SPEC 6.1 illumination CSV is fully reread and checked before an
// atomic, no-replace publication in outputPath's directory. Existing outputs,
// missing/invalid values, and publication failures are rejected; failures do
// not leave a formal output.
IlluminationResolutionSummary resolveAndPublishIllumination(
    const std::filesystem::path &targetStatesPath,
    const std::filesystem::path &sunStatesPath,
    const std::string &referenceOrigin,
    const std::filesystem::path &outputPath);

namespace Testing {

using PreVerifyHook = void (*)(const std::filesystem::path &temporaryPath);

// Test seam for proving that byte tampering between durable temporary write
// and verification is rejected before formal publication. Production callers
// and the CLI use resolveAndPublishIllumination() without a hook.
IlluminationResolutionSummary
resolveAndPublishIlluminationWithPreVerifyHook(
    const std::filesystem::path &targetStatesPath,
    const std::filesystem::path &sunStatesPath,
    const std::string &referenceOrigin,
    const std::filesystem::path &outputPath,
    PreVerifyHook hook);

}  // namespace Testing

}  // namespace Isis::SmallBodyRender
