// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace Isis::SmallBodyRender {

struct PixelProducts {
  std::uint16_t dn;
  std::uint8_t mask;
  float depthM;
  std::array<float, 3> normalCamera;
  std::uint32_t plateId;
  float mu0{std::numeric_limits<float>::quiet_NaN()};
  std::uint8_t illuminationClass{0};
  float relativeAlbedoLinear{std::numeric_limits<float>::quiet_NaN()};
};

struct ProductPaths {
  std::filesystem::path imageU16;
  std::filesystem::path maskU8;
  std::filesystem::path depthMFloat32;
  std::filesystem::path normalCameraFloat32;
  std::filesystem::path facetIdU32;
  std::filesystem::path mu0Float32{};
  std::filesystem::path illuminationMaskU8{};
  std::optional<std::filesystem::path> albedoFloat32;
};

struct TileBounds {
  int x;
  int y;
  int width;
  int height;
};

struct IlluminationProductCounts {
  std::uint64_t validPixels;
  std::uint64_t directPixels;
  std::uint64_t nightPixels;
  std::uint64_t selfShadowPixels;
};

// Reopens the three production tiled products together after writer close.
// Memory is bounded by three physical tiles, independent of image dimensions.
IlluminationProductCounts validatePointSunDiagnosticProducts(
    const ProductPaths &paths, int widthPx, int heightPx,
    int tileWidthPx, int tileHeightPx);

class TiffProductWriter {
  public:
    TiffProductWriter(const ProductPaths &paths, int widthPx, int heightPx,
                      int tileWidthPx, int tileHeightPx);
    ~TiffProductWriter();

    TiffProductWriter(const TiffProductWriter &) = delete;
    TiffProductWriter &operator=(const TiffProductWriter &) = delete;

    void writeTile(const TileBounds &bounds,
                   const std::vector<PixelProducts> &pixels);
    void closeAndValidate();

  private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace Isis::SmallBodyRender
