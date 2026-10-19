// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace Isis::SmallBodyRender {

enum class RasterSampleType { UInt8, UInt16, UInt32, Float32 };

class TiffSingleBandRaster final {
 public:
  static TiffSingleBandRaster read(const std::filesystem::path &path);
  static TiffSingleBandRaster read(const std::filesystem::path &path,
                                   int expectedWidthPx,
                                   int expectedHeightPx);

  int widthPx() const;
  int heightPx() const;
  RasterSampleType sampleType() const;
  double valueAsDouble(std::size_t index) const;
  std::uint32_t valueAsUInt32(std::size_t index) const;

 private:
  TiffSingleBandRaster(int widthPx, int heightPx, RasterSampleType sampleType,
                       std::vector<double> values);

  int widthPx_;
  int heightPx_;
  RasterSampleType sampleType_;
  std::vector<double> values_;
};

}  // namespace Isis::SmallBodyRender
