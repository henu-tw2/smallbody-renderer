// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "TiffSingleBandRaster.h"

#include <tiffio.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Isis::SmallBodyRender {
namespace {

[[noreturn]] void reject(const std::string &message) {
  throw std::invalid_argument("TIFF_SINGLE_BAND_RASTER_ERROR: " + message);
}

class TiffHandle final {
 public:
  explicit TiffHandle(TIFF *value) : value_(value) {}
  ~TiffHandle() {
    if (value_ != nullptr) TIFFClose(value_);
  }
  TiffHandle(const TiffHandle &) = delete;
  TiffHandle &operator=(const TiffHandle &) = delete;
  TIFF *get() const { return value_; }

 private:
  TIFF *value_;
};

std::size_t checkedPixels(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) reject("image dimensions must be positive");
  if (width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    reject("image dimensions exceed int range");
  }
  const std::size_t widthSize = width;
  const std::size_t heightSize = height;
  if (heightSize > std::numeric_limits<std::size_t>::max() / widthSize) {
    reject("image dimensions exceed size_t range");
  }
  return widthSize * heightSize;
}

std::size_t bytesPerSample(RasterSampleType type) {
  switch (type) {
    case RasterSampleType::UInt8: return sizeof(std::uint8_t);
    case RasterSampleType::UInt16: return sizeof(std::uint16_t);
    case RasterSampleType::UInt32: return sizeof(std::uint32_t);
    case RasterSampleType::Float32: return sizeof(float);
  }
  reject("unrecognized sample type");
}

RasterSampleType readTags(TIFF *tiff, std::uint32_t &width,
                          std::uint32_t &height) {
  std::uint16_t bits = 0;
  std::uint16_t samples = 0;
  std::uint16_t format = 0;
  std::uint16_t planar = 0;
  std::uint16_t orientation = 0;
  if (TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) != 1 ||
      TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) != 1) {
    reject("missing image dimensions");
  }
  TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &format);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_ORIENTATION, &orientation);
  if (samples != 1) reject("SAMPLESPERPIXEL must be 1");
  if (planar != PLANARCONFIG_CONTIG) reject("PLANARCONFIG must be CONTIG");
  if (orientation != ORIENTATION_TOPLEFT) reject("ORIENTATION must be TOPLEFT");
  if (format == SAMPLEFORMAT_UINT && bits == 8) return RasterSampleType::UInt8;
  if (format == SAMPLEFORMAT_UINT && bits == 16) return RasterSampleType::UInt16;
  if (format == SAMPLEFORMAT_UINT && bits == 32) return RasterSampleType::UInt32;
  if (format == SAMPLEFORMAT_IEEEFP && bits == 32) return RasterSampleType::Float32;
  reject("only uint8, uint16, uint32, and float32 are supported");
}

template <typename Value>
void copySamples(const std::vector<unsigned char> &source,
                 std::size_t sourceOffset, std::size_t count,
                 std::vector<double> &destination,
                 std::size_t destinationOffset) {
  if (sourceOffset > source.size() ||
      count > (source.size() - sourceOffset) / sizeof(Value)) {
    reject("TIFF sample buffer is smaller than declared extent");
  }
  for (std::size_t index = 0; index < count; ++index) {
    Value value{};
    std::memcpy(&value, source.data() + sourceOffset + index * sizeof(Value),
                sizeof(Value));
    destination.at(destinationOffset + index) = static_cast<double>(value);
  }
}

void copyTypedSamples(RasterSampleType type,
                      const std::vector<unsigned char> &source,
                      std::size_t sourceOffset, std::size_t count,
                      std::vector<double> &destination,
                      std::size_t destinationOffset) {
  switch (type) {
    case RasterSampleType::UInt8:
      copySamples<std::uint8_t>(source, sourceOffset, count, destination, destinationOffset); return;
    case RasterSampleType::UInt16:
      copySamples<std::uint16_t>(source, sourceOffset, count, destination, destinationOffset); return;
    case RasterSampleType::UInt32:
      copySamples<std::uint32_t>(source, sourceOffset, count, destination, destinationOffset); return;
    case RasterSampleType::Float32:
      copySamples<float>(source, sourceOffset, count, destination, destinationOffset); return;
  }
  reject("unrecognized sample type");
}

void readStrips(TIFF *tiff, RasterSampleType type, std::uint32_t width,
                std::uint32_t height, std::vector<double> &values) {
  const tmsize_t bytes = TIFFScanlineSize(tiff);
  const std::size_t required = static_cast<std::size_t>(width) * bytesPerSample(type);
  if (bytes < 0 || static_cast<std::uintmax_t>(bytes) < required) {
    reject("TIFF scanline size is invalid");
  }
  std::vector<unsigned char> buffer(static_cast<std::size_t>(bytes));
  for (std::uint32_t row = 0; row < height; ++row) {
    if (TIFFReadScanline(tiff, buffer.data(), row, 0) < 0) {
      reject("failed to read scanline " + std::to_string(row));
    }
    copyTypedSamples(type, buffer, 0, width, values,
                     static_cast<std::size_t>(row) * width);
  }
}

void readTiles(TIFF *tiff, RasterSampleType type, std::uint32_t width,
               std::uint32_t height, std::vector<double> &values) {
  std::uint32_t tileWidth = 0;
  std::uint32_t tileHeight = 0;
  if (TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tileWidth) != 1 ||
      TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tileHeight) != 1 ||
      tileWidth == 0 || tileHeight == 0) reject("missing tile dimensions");
  const std::size_t sampleBytes = bytesPerSample(type);
  const std::size_t tw = tileWidth;
  const std::size_t th = tileHeight;
  if (th > std::numeric_limits<std::size_t>::max() / tw ||
      tw * th > std::numeric_limits<std::size_t>::max() / sampleBytes) {
    reject("tile dimensions exceed size_t range");
  }
  const std::size_t required = tw * th * sampleBytes;
  const tmsize_t bytes = TIFFTileSize(tiff);
  if (bytes < 0 || static_cast<std::uintmax_t>(bytes) < required) reject("TIFF tile size is invalid");
  std::vector<unsigned char> buffer(static_cast<std::size_t>(bytes));
  for (std::uint32_t y = 0; y < height; y += tileHeight) {
    for (std::uint32_t x = 0; x < width; x += tileWidth) {
      if (TIFFReadTile(tiff, buffer.data(), x, y, 0, 0) < 0) {
        reject("failed to read tile at " + std::to_string(x) + "," + std::to_string(y));
      }
      const std::uint32_t rows = std::min(tileHeight, height - y);
      const std::uint32_t columns = std::min(tileWidth, width - x);
      for (std::uint32_t row = 0; row < rows; ++row) {
        copyTypedSamples(type, buffer, static_cast<std::size_t>(row) * tileWidth * sampleBytes,
                         columns, values, static_cast<std::size_t>(y + row) * width + x);
      }
    }
  }
}

}  // namespace

TiffSingleBandRaster::TiffSingleBandRaster(int widthPx, int heightPx,
                                           RasterSampleType sampleType,
                                           std::vector<double> values)
    : widthPx_(widthPx), heightPx_(heightPx), sampleType_(sampleType), values_(std::move(values)) {}

TiffSingleBandRaster TiffSingleBandRaster::read(const std::filesystem::path &path) {
  if (path.empty()) reject("path must not be empty");
  TIFF *raw = TIFFOpen(path.c_str(), "r");
  if (raw == nullptr) reject("cannot open TIFF " + path.string());
  TiffHandle tiff(raw);
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  const RasterSampleType type = readTags(tiff.get(), width, height);
  std::vector<double> values(checkedPixels(width, height));
  if (TIFFIsTiled(tiff.get()) != 0) readTiles(tiff.get(), type, width, height, values);
  else readStrips(tiff.get(), type, width, height, values);
  return TiffSingleBandRaster(static_cast<int>(width), static_cast<int>(height), type, std::move(values));
}

TiffSingleBandRaster TiffSingleBandRaster::read(
    const std::filesystem::path &path, int expectedWidthPx,
    int expectedHeightPx) {
  if (expectedWidthPx <= 0 || expectedHeightPx <= 0) {
    reject("expected image dimensions must be positive");
  }
  TiffSingleBandRaster raster = read(path);
  if (raster.widthPx() != expectedWidthPx ||
      raster.heightPx() != expectedHeightPx) {
    reject("image dimensions do not match expected extent");
  }
  return raster;
}

int TiffSingleBandRaster::widthPx() const { return widthPx_; }
int TiffSingleBandRaster::heightPx() const { return heightPx_; }
RasterSampleType TiffSingleBandRaster::sampleType() const { return sampleType_; }
double TiffSingleBandRaster::valueAsDouble(std::size_t index) const {
  if (index >= values_.size()) reject("pixel index is outside raster bounds");
  return values_[index];
}
std::uint32_t TiffSingleBandRaster::valueAsUInt32(std::size_t index) const {
  if (sampleType_ == RasterSampleType::Float32) reject("valueAsUInt32 is unavailable for float32 samples");
  return static_cast<std::uint32_t>(valueAsDouble(index));
}

}  // namespace Isis::SmallBodyRender
