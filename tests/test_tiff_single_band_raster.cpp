// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include <tiffio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "TiffSingleBandRaster.h"

namespace {

namespace fs = std::filesystem;

using Isis::SmallBodyRender::RasterSampleType;
using Isis::SmallBodyRender::TiffSingleBandRaster;

constexpr std::uint32_t kWidth = 4;
constexpr std::uint32_t kHeight = 3;

void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

template <typename Callable>
void requireRasterRejects(Callable callable, const std::string &context) {
  try {
    callable();
  }
  catch (const std::exception &error) {
    require(std::string(error.what()).rfind("TIFF_SINGLE_BAND_RASTER_ERROR",
                                            0) == 0,
            context + " must use TIFF_SINGLE_BAND_RASTER_ERROR: " +
                error.what());
    return;
  }
  require(false, "expected TIFF_SINGLE_BAND_RASTER_ERROR for " + context);
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    static std::size_t sequence = 0;
    do {
      path_ = fs::temp_directory_path() /
              ("tiff-single-band-raster-" + std::to_string(++sequence));
    } while (!fs::create_directory(path_));
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path &path() const { return path_; }

 private:
  fs::path path_;
};

template <typename Value>
fs::path writeSingleBandFixture(const fs::path &directory,
                                const std::string &name,
                                std::vector<Value> values,
                                std::uint16_t sampleFormat,
                                std::uint16_t samplesPerPixel = 1,
                                std::uint16_t orientation =
                                    ORIENTATION_TOPLEFT,
                                std::uint16_t compression = COMPRESSION_NONE,
                                std::uint32_t width = kWidth,
                                std::uint32_t height = kHeight) {
  require(values.size() == static_cast<std::size_t>(width) * height *
                               samplesPerPixel,
          "fixture values cover every sample");
  const fs::path path = directory / name;
  TIFF *tiff = TIFFOpen(path.c_str(), "w");
  require(tiff != nullptr, "create TIFF fixture " + path.string());
  require(TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width) == 1,
          "set fixture width");
  require(TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height) == 1,
          "set fixture height");
  require(TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE,
                       static_cast<std::uint16_t>(8U * sizeof(Value))) == 1,
          "set fixture bits");
  require(TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, samplesPerPixel) == 1,
          "set fixture samples");
  require(TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, sampleFormat) == 1,
          "set fixture sample format");
  require(TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) == 1,
          "set fixture planar configuration");
  const std::uint16_t photometric =
      samplesPerPixel == 1 ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_RGB;
  require(TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, photometric) == 1,
          "set fixture photometric");
  require(TIFFSetField(tiff, TIFFTAG_ORIENTATION, orientation) == 1,
          "set fixture orientation");
  require(TIFFSetField(tiff, TIFFTAG_COMPRESSION, compression) == 1,
          "set fixture compression");
  for (std::uint32_t row = 0; row < height; ++row) {
    Value *const rowStart = values.data() +
                            static_cast<std::size_t>(row) * width *
                                samplesPerPixel;
    require(TIFFWriteScanline(tiff, rowStart, row, 0) >= 0,
            "write fixture scanline");
  }
  TIFFClose(tiff);
  return path;
}

template <typename Value>
fs::path writeTiledSingleBandFixture(const fs::path &directory,
                                     const std::string &name,
                                     const std::vector<Value> &values,
                                     std::uint32_t width,
                                     std::uint32_t height,
                                     std::uint32_t tileWidth,
                                     std::uint32_t tileHeight,
                                     bool omitLastTile = false) {
  require(values.size() == static_cast<std::size_t>(width) * height,
          "tiled fixture values cover every pixel");
  const fs::path path = directory / name;
  TIFF *tiff = TIFFOpen(path.c_str(), "w");
  require(tiff != nullptr, "create tiled TIFF fixture " + path.string());
  require(TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width) == 1,
          "set tiled fixture width");
  require(TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height) == 1,
          "set tiled fixture height");
  require(TIFFSetField(tiff, TIFFTAG_TILEWIDTH, tileWidth) == 1,
          "set tiled fixture tile width");
  require(TIFFSetField(tiff, TIFFTAG_TILELENGTH, tileHeight) == 1,
          "set tiled fixture tile height");
  require(TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE,
                       static_cast<std::uint16_t>(8U * sizeof(Value))) == 1,
          "set tiled fixture bits");
  require(TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1) == 1,
          "set tiled fixture samples");
  require(TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT) == 1,
          "set tiled fixture sample format");
  require(TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) == 1,
          "set tiled fixture planar configuration");
  require(TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK) ==
              1,
          "set tiled fixture photometric");
  require(TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) == 1,
          "set tiled fixture orientation");
  require(TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE) == 1,
          "set tiled fixture compression");
  std::vector<Value> tileValues(
      static_cast<std::size_t>(tileWidth) * tileHeight, Value{});
  for (std::uint32_t tileY = 0; tileY < height; tileY += tileHeight) {
    for (std::uint32_t tileX = 0; tileX < width; tileX += tileWidth) {
      std::fill(tileValues.begin(), tileValues.end(), Value{});
      const std::uint32_t rows = std::min(tileHeight, height - tileY);
      const std::uint32_t columns = std::min(tileWidth, width - tileX);
      for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
          tileValues[static_cast<std::size_t>(row) * tileWidth + column] =
              values[static_cast<std::size_t>(tileY + row) * width +
                     tileX + column];
        }
      }
      const bool isLastTile = tileX + tileWidth >= width &&
                              tileY + tileHeight >= height;
      if (!omitLastTile || !isLastTile) {
        require(TIFFWriteTile(tiff, tileValues.data(), tileX, tileY, 0, 0) >=
                    0,
                "write tiled fixture tile");
      }
    }
  }
  TIFFClose(tiff);
  return path;
}

template <typename Value>
void requireTypedFixture(const fs::path &directory, const std::string &name,
                         const std::vector<Value> &values,
                         RasterSampleType expectedSampleType) {
  const fs::path path = writeSingleBandFixture(
      directory, name, values,
      std::is_floating_point_v<Value> ? SAMPLEFORMAT_IEEEFP
                                      : SAMPLEFORMAT_UINT);
  const TiffSingleBandRaster raster = TiffSingleBandRaster::read(path);
  require(raster.widthPx() == static_cast<int>(kWidth) &&
              raster.heightPx() == static_cast<int>(kHeight),
          "reader preserves fixture dimensions");
  require(raster.sampleType() == expectedSampleType,
          "reader reports the exact fixture sample type");
  for (std::size_t index = 0; index < values.size(); ++index) {
    require(raster.valueAsDouble(index) == static_cast<double>(values[index]),
            "reader preserves top-left row-major sample order");
    if constexpr (!std::is_floating_point_v<Value>) {
      require(raster.valueAsUInt32(index) ==
                  static_cast<std::uint32_t>(values[index]),
              "integer reader preserves exact sample value");
    }
  }
}

void testReadsSupportedSingleBandSamplesInTopLeftOrder() {
  TemporaryDirectory temporary;
  requireTypedFixture(
      temporary.path(), "u8.tif",
      std::vector<std::uint8_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 255},
      RasterSampleType::UInt8);
  requireTypedFixture(
      temporary.path(), "u16.tif",
      std::vector<std::uint16_t>{0, 1, 2, 3, 1024, 2048,
                                 4096, 8192, 16384, 32768, 65534, 65535},
      RasterSampleType::UInt16);
  requireTypedFixture(
      temporary.path(), "u32.tif",
      std::vector<std::uint32_t>{0U, 1U, 2U, 3U, 65536U, 1048576U,
                                 16777216U, 268435456U, 1073741824U,
                                 2147483648U, 4294967294U, 4294967295U},
      RasterSampleType::UInt32);
  requireTypedFixture(
      temporary.path(), "f32.tif",
      std::vector<float>{0.0F, 0.25F, -0.5F, 1.0F, 1.5F, 2.0F,
                         4.0F, 8.0F, 16.0F, 32.0F, 64.0F, 128.0F},
      RasterSampleType::Float32);
}

void testPassesFloatNanToCallingLayer() {
  TemporaryDirectory temporary;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const fs::path path = writeSingleBandFixture(
      temporary.path(), "float-nan.tif",
      std::vector<float>{0.0F, 1.0F, 2.0F, 3.0F, 4.0F, nan,
                         6.0F, 7.0F, 8.0F, 9.0F, 10.0F, 11.0F},
      SAMPLEFORMAT_IEEEFP);

  const TiffSingleBandRaster raster = TiffSingleBandRaster::read(path);
  require(std::isnan(raster.valueAsDouble(5)),
          "reader returns float NaN for caller-side invalid handling");
  requireRasterRejects([&] { (void)raster.valueAsUInt32(5); },
                       "integer accessor for float sample");
}

void testReadsTiledRasterWithCroppedEdgesAndRejectsMissingTile() {
  TemporaryDirectory temporary;
  constexpr std::uint32_t width = 19;
  constexpr std::uint32_t height = 17;
  constexpr std::uint32_t tileWidth = 16;
  constexpr std::uint32_t tileHeight = 16;
  std::vector<std::uint16_t> values;
  values.reserve(static_cast<std::size_t>(width) * height);
  for (std::uint32_t row = 0; row < height; ++row) {
    for (std::uint32_t column = 0; column < width; ++column) {
      values.push_back(static_cast<std::uint16_t>(100U * row + column));
    }
  }
  const fs::path tiled = writeTiledSingleBandFixture(
      temporary.path(), "cropped-tiles.tif", values, width, height,
      tileWidth, tileHeight);
  const TiffSingleBandRaster raster = TiffSingleBandRaster::read(
      tiled, static_cast<int>(width), static_cast<int>(height));
  for (std::size_t index = 0; index < values.size(); ++index) {
    require(raster.valueAsUInt32(index) == values[index],
            "tiled reader preserves top-left row-major values through edges");
  }

  const fs::path missingTile = writeTiledSingleBandFixture(
      temporary.path(), "missing-tile.tif", values, width, height,
      tileWidth, tileHeight, true);
  TIFFErrorHandler previousErrorHandler = TIFFSetErrorHandler(nullptr);
  requireRasterRejects([&] { (void)TiffSingleBandRaster::read(missingTile); },
                       "missing tiled TIFF data");
  TIFFSetErrorHandler(previousErrorHandler);
}

void testRejectsUnsupportedOrUnreadableTiffs() {
  TemporaryDirectory temporary;
  const std::vector<std::uint8_t> oneBandValues(
      static_cast<std::size_t>(kWidth) * kHeight, 7U);
  const fs::path multiBand = writeSingleBandFixture(
      temporary.path(), "multi-band.tif",
      std::vector<std::uint8_t>(oneBandValues.size() * 3U, 7U),
      SAMPLEFORMAT_UINT, 3);
  requireRasterRejects([&] { (void)TiffSingleBandRaster::read(multiBand); },
                       "multi-band TIFF");

  const fs::path signedSamples = writeSingleBandFixture(
      temporary.path(), "signed.tif", oneBandValues, SAMPLEFORMAT_INT);
  requireRasterRejects(
      [&] { (void)TiffSingleBandRaster::read(signedSamples); },
      "unsupported sample format");

  const fs::path bottomLeft = writeSingleBandFixture(
      temporary.path(), "bottom-left.tif", oneBandValues, SAMPLEFORMAT_UINT,
      1, ORIENTATION_BOTLEFT);
  requireRasterRejects([&] { (void)TiffSingleBandRaster::read(bottomLeft); },
                       "non-top-left orientation");

  const fs::path alternateExtent = writeSingleBandFixture(
      temporary.path(), "alternate-extent.tif",
      std::vector<std::uint8_t>(15U, 9U), SAMPLEFORMAT_UINT, 1,
      ORIENTATION_TOPLEFT, COMPRESSION_NONE, 5, 3);
  const TiffSingleBandRaster alternate = TiffSingleBandRaster::read(
      alternateExtent, 5, 3);
  require(alternate.widthPx() == 5 && alternate.heightPx() == 3,
          "expected-extent reader accepts matching dimensions");
  requireRasterRejects(
      [&] { (void)TiffSingleBandRaster::read(alternateExtent, 4, 3); },
      "mismatched expected width");
  requireRasterRejects(
      [&] { (void)TiffSingleBandRaster::read(alternateExtent, 5, 4); },
      "mismatched expected height");
  requireRasterRejects(
      [&] { (void)TiffSingleBandRaster::read(alternateExtent, 0, 3); },
      "zero expected width");
  requireRasterRejects(
      [&] { (void)TiffSingleBandRaster::read(alternateExtent, 5, -1); },
      "negative expected height");

  const fs::path compressed = writeSingleBandFixture(
      temporary.path(), "truncated.tif", oneBandValues, SAMPLEFORMAT_UINT,
      1, ORIENTATION_TOPLEFT, COMPRESSION_LZW);
  const std::uintmax_t originalBytes = fs::file_size(compressed);
  require(originalBytes > 1U, "compressed fixture has bytes to truncate");
  fs::resize_file(compressed, originalBytes / 2U);
  TIFFErrorHandler previousErrorHandler = TIFFSetErrorHandler(nullptr);
  requireRasterRejects([&] { (void)TiffSingleBandRaster::read(compressed); },
                       "compressed TIFF read failure");
  TIFFSetErrorHandler(previousErrorHandler);
}

}  // namespace

int main() {
  testReadsSupportedSingleBandSamplesInTopLeftOrder();
  testPassesFloatNanToCallingLayer();
  testReadsTiledRasterWithCroppedEdgesAndRejectsMissingTile();
  testRejectsUnsupportedOrUnreadableTiffs();
  return EXIT_SUCCESS;
}
