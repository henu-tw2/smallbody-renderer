// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "SurfaceAppearance.h"

namespace Isis::SmallBodyRender {

struct PlateAlbedoStatistics {
  std::uint32_t validPlateCount{0};
  std::uint32_t missingPlateCount{0};
  std::uint32_t invalidPlateCount{0};
  double minimumRelativeAlbedo{0.0};
  double maximumRelativeAlbedo{0.0};
  double medianRelativeAlbedo{0.0};
};

class PlateAlbedoTable final : public SurfaceAppearance {
 public:
  static PlateAlbedoTable read(const std::filesystem::path &path,
                               std::uint32_t plateCount,
                               double defaultAlbedo,
                               const std::string &expectedSha256 = {});
  // Parses one immutable CSV byte snapshot. Its SHA-256, if supplied, is
  // computed from these exact bytes; this API never opens a filesystem path.
  static PlateAlbedoTable readSnapshot(std::string csvSnapshot,
                                       std::uint32_t plateCount,
                                       double defaultAlbedo,
                                       const std::string &expectedSha256 = {});

  double relativeAlbedo(std::uint32_t plateId) const override;
  const PlateAlbedoStatistics &statistics() const;

 private:
  PlateAlbedoTable(std::uint32_t plateCount, double defaultAlbedo,
                   std::vector<double> relativeAlbedos,
                   std::vector<bool> validPlates,
                   PlateAlbedoStatistics statistics);

  std::uint32_t plateCount_;
  double defaultAlbedo_;
  std::vector<double> relativeAlbedos_;
  std::vector<bool> validPlates_;
  PlateAlbedoStatistics statistics_;
};

}  // namespace Isis::SmallBodyRender
