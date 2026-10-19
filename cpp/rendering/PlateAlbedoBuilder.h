// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Isis::SmallBodyRender {

struct PlateAlbedoSourceFrame {
  std::string frameId;
  std::filesystem::path observedLinearTif;
  std::filesystem::path facetIdTif;
  std::filesystem::path geometryMaskTif;
  std::filesystem::path illuminationMaskTif;
  double blackLevelDn;
  double saturationDn;
  std::string sourceKind;
};

struct PlateAlbedoEstimate {
  std::uint32_t plateId;
  double relativeAlbedoLinear;
  bool valid;
  std::uint32_t sourceCount;
  std::uint64_t pixelCount;
  double sourceRms;
};

struct PlateAlbedoBuildStatistics {
  std::uint32_t inputFrameCount;
  std::uint32_t contributingFrameCount;
  std::uint32_t validPlateCount;
  std::uint64_t validPixelCount;
  std::string estimationMethod;
  double normalizationFactor;
  std::string normalizationMethod;
};

struct PlateAlbedoBuildResult {
  // Contains only valid contributing plates, ordered by ascending one-based ID.
  std::vector<PlateAlbedoEstimate> estimates;
  PlateAlbedoBuildStatistics statistics;
};

class PlateAlbedoBuilder final {
 public:
  static PlateAlbedoBuildResult build(
      const std::vector<PlateAlbedoSourceFrame> &sourceFrames);
};

}  // namespace Isis::SmallBodyRender
