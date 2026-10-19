// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "TiffProductWriter.h"

#include <tiffio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "SolarLambert.h"
#include "SurfaceAppearance.h"

namespace Isis::SmallBodyRender {
namespace {

constexpr int MaximumTileEdgePx = 4096;
constexpr std::size_t MaximumTilePixels = 1048576;

struct TiffHandle {
  TIFF *value{nullptr};
  std::filesystem::path path;
};

std::size_t checkedMultiply(std::size_t left, std::size_t right,
                            const std::string &field) {
  if (left != 0 &&
      right > std::numeric_limits<std::size_t>::max() / left) {
    throw std::invalid_argument(
        field + ": multiplication exceeds size_t");
  }
  return left * right;
}

tmsize_t checkedTiffBytes(std::size_t elements, std::size_t bytesPerElement,
                          const std::string &field) {
  const std::size_t bytes =
      checkedMultiply(elements, bytesPerElement, field);
  const auto maximum =
      static_cast<std::uintmax_t>(std::numeric_limits<tmsize_t>::max());
  if (static_cast<std::uintmax_t>(bytes) > maximum) {
    throw std::invalid_argument(
        field + ": byte count exceeds tmsize_t maximum");
  }
  return static_cast<tmsize_t>(bytes);
}

std::size_t validateTileWorkset(int tileWidth, int tileHeight,
                                const std::string &prefix = "") {
  if (tileWidth <= 0 || tileWidth % 16 != 0) {
    throw std::invalid_argument(
        prefix + "tile_width_px: must be a positive multiple of 16");
  }
  if (tileHeight <= 0 || tileHeight % 16 != 0) {
    throw std::invalid_argument(
        prefix + "tile_height_px: must be a positive multiple of 16");
  }
  if (tileWidth > MaximumTileEdgePx) {
    throw std::invalid_argument(
        prefix + "tile_width_px: must be <= 4096");
  }
  if (tileHeight > MaximumTileEdgePx) {
    throw std::invalid_argument(
        prefix + "tile_height_px: must be <= 4096");
  }
  const auto width = static_cast<std::size_t>(tileWidth);
  const auto height = static_cast<std::size_t>(tileHeight);
  if (height > MaximumTilePixels / width) {
    throw std::invalid_argument(
        prefix +
        "tile_pixels: tile_width_px * tile_height_px must be <= 1048576");
  }
  const std::size_t pixels = width * height;
  constexpr std::size_t MaximumWriterBytesPerPixel =
      sizeof(std::uint16_t) + sizeof(std::uint8_t) + sizeof(float) +
      3 * sizeof(float) + sizeof(std::uint32_t) + sizeof(float) +
      sizeof(std::uint8_t) + sizeof(float);
  static_cast<void>(
      checkedMultiply(pixels, MaximumWriterBytesPerPixel,
                      prefix + "tile workset bytes"));
  static_cast<void>(
      checkedTiffBytes(pixels, 3 * sizeof(float),
                       prefix + "tile_pixels"));
  return pixels;
}

std::size_t positiveCeilingDivision(int extent, int tile,
                                    const std::string &field) {
  if (extent <= 0 || tile <= 0) {
    throw std::invalid_argument(
        field + ": extent and tile must be positive");
  }
  return 1U + static_cast<std::size_t>(extent - 1) /
                  static_cast<std::size_t>(tile);
}

int checkedTileOrigin(std::size_t tileIndex, int tileExtent,
                      const std::string &field) {
  const std::size_t origin =
      checkedMultiply(tileIndex, static_cast<std::size_t>(tileExtent),
                      field);
  if (origin >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument(field + ": exceeds the C++ int range");
  }
  return static_cast<int>(origin);
}

void close(TiffHandle &handle) {
  if (handle.value != nullptr) {
    TIFFClose(handle.value);
    handle.value = nullptr;
  }
}

void requireSet(bool success, const std::filesystem::path &path,
                const std::string &tag) {
  if (!success) {
    throw std::runtime_error("TIFF " + path.string() +
                             ": failed to set " + tag);
  }
}

TiffHandle openProduct(
    const std::filesystem::path &path, int width, int height,
    int tileWidth, int tileHeight, std::uint16_t bits,
    std::uint16_t samples, std::uint16_t sampleFormat, bool extras,
    std::vector<std::filesystem::path> &createdByThisConstruction) {
  TIFF *tiff = TIFFOpen(path.c_str(), "w");
  if (tiff == nullptr) {
    throw std::runtime_error("TIFF path: cannot create " + path.string());
  }
  createdByThisConstruction.push_back(path);
  TiffHandle result{tiff, path};
  try {
    requireSet(TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width) == 1, path,
               "IMAGEWIDTH");
    requireSet(TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height) == 1, path,
               "IMAGELENGTH");
    requireSet(TIFFSetField(tiff, TIFFTAG_TILEWIDTH, tileWidth) == 1, path,
               "TILEWIDTH");
    requireSet(TIFFSetField(tiff, TIFFTAG_TILELENGTH, tileHeight) == 1, path,
               "TILELENGTH");
    requireSet(TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, bits) == 1, path,
               "BITSPERSAMPLE");
    requireSet(TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, samples) == 1,
               path, "SAMPLESPERPIXEL");
    requireSet(TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, sampleFormat) == 1,
               path, "SAMPLEFORMAT");
    requireSet(TIFFSetField(tiff, TIFFTAG_PLANARCONFIG,
                            PLANARCONFIG_CONTIG) == 1,
               path, "PLANARCONFIG");
    requireSet(TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC,
                            PHOTOMETRIC_MINISBLACK) == 1,
               path, "PHOTOMETRIC");
    requireSet(TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_LZW) == 1,
               path, "COMPRESSION");
    requireSet(TIFFSetField(tiff, TIFFTAG_ORIENTATION,
                            ORIENTATION_TOPLEFT) == 1,
               path, "ORIENTATION");
    if (extras) {
      std::uint16_t extraTypes[2] = {EXTRASAMPLE_UNSPECIFIED,
                                     EXTRASAMPLE_UNSPECIFIED};
      requireSet(TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, 2,
                              extraTypes) == 1,
                 path, "EXTRASAMPLES");
    }
  }
  catch (...) {
    close(result);
    throw;
  }
  return result;
}

std::vector<std::filesystem::path> validateOutputPaths(
    const ProductPaths &paths, bool diagnosticsEnabled) {
  struct NamedPath {
    const char *field;
    const std::filesystem::path *path;
  };
  std::vector<NamedPath> namedPaths{
      {"imageU16", &paths.imageU16},
      {"maskU8", &paths.maskU8},
      {"depthMFloat32", &paths.depthMFloat32},
      {"normalCameraFloat32", &paths.normalCameraFloat32},
      {"facetIdU32", &paths.facetIdU32}};
  if (diagnosticsEnabled) {
    namedPaths.push_back({"mu0Float32", &paths.mu0Float32});
    namedPaths.push_back(
        {"illuminationMaskU8", &paths.illuminationMaskU8});
  }
  if (paths.albedoFloat32.has_value()) {
    namedPaths.push_back(
        {"albedoFloat32", &paths.albedoFloat32.value()});
  }

  std::set<std::filesystem::path> normalizedPaths;
  std::vector<std::filesystem::path> result;
  result.reserve(namedPaths.size());
  for (const NamedPath &named : namedPaths) {
    if (named.path->empty()) {
      throw std::invalid_argument(
          std::string("paths.") + named.field + ": must not be empty");
    }
    std::error_code error;
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(*named.path, error);
    if (error) {
      throw std::invalid_argument(
          std::string("paths.") + named.field +
          ": cannot resolve output path " + named.path->string() +
          ": " + error.message());
    }
    if (!normalizedPaths.insert(normalized).second) {
      throw std::invalid_argument(
          std::string("paths.") + named.field +
          ": duplicate output path " + named.path->string());
    }
    const std::filesystem::file_status outputStatus =
        std::filesystem::symlink_status(*named.path, error);
    if (outputStatus.type() == std::filesystem::file_type::not_found) {
      error.clear();
    }
    if (error) {
      throw std::invalid_argument(
          std::string("paths.") + named.field +
          ": cannot inspect output path " + named.path->string() +
          ": " + error.message());
    }
    if (outputStatus.type() != std::filesystem::file_type::not_found) {
      throw std::invalid_argument(
          std::string("paths.") + named.field +
          ": output path already exists: " + named.path->string());
    }
    result.push_back(*named.path);
  }
  return result;
}

std::vector<std::string> removeCreatedProducts(
    const std::vector<std::filesystem::path> &createdPaths) {
  std::vector<std::string> failures;
  for (auto path = createdPaths.rbegin(); path != createdPaths.rend();
       ++path) {
    std::error_code error;
    const bool removed = std::filesystem::remove(*path, error);
    if (error) {
      failures.push_back(
          path->string() + ": " + error.message());
      continue;
    }
    if (!removed) {
      const std::filesystem::file_status status =
          std::filesystem::symlink_status(*path, error);
      if (status.type() == std::filesystem::file_type::not_found) {
        error.clear();
      }
      if (error) {
        failures.push_back(
            path->string() + ": " + error.message());
      }
      else if (status.type() !=
               std::filesystem::file_type::not_found) {
        failures.push_back(path->string() + ": was not removed");
      }
    }
  }
  return failures;
}

std::string constructionCleanupError(
    const std::string &original,
    const std::vector<std::string> &cleanupFailures) {
  std::string message = "TIFF construction failed: " + original +
                        "; cleanup failed";
  for (const std::string &failure : cleanupFailures) {
    message += "; " + failure;
  }
  return message;
}

void validatePixel(const PixelProducts &pixel, std::size_t index,
                   bool albedoEnabled) {
  const std::string prefix = "pixels[" + std::to_string(index) + "].";
  if (pixel.mask == 0) {
    if (pixel.dn != 0 || pixel.plateId != 0 ||
        !std::isnan(pixel.depthM) ||
        !std::isnan(pixel.normalCamera[0]) ||
        !std::isnan(pixel.normalCamera[1]) ||
        !std::isnan(pixel.normalCamera[2])) {
      throw std::invalid_argument(
          prefix + "mask: background requires DN=0, NaN geometry, plate=0");
    }
    if (!std::isnan(pixel.mu0)) {
      throw std::invalid_argument(
          prefix + "mu0: background requires mu0=NaN");
    }
    if (pixel.illuminationClass != static_cast<std::uint8_t>(
                                         IlluminationClass::Background)) {
      throw std::invalid_argument(
          prefix + "illuminationClass: background requires class 0");
    }
    if (albedoEnabled && !std::isnan(pixel.relativeAlbedoLinear)) {
      throw std::invalid_argument(
          prefix + "relativeAlbedoLinear: background requires NaN");
    }
    return;
  }
  if (pixel.mask != 1 || pixel.dn == 0 || pixel.plateId == 0 ||
      !std::isfinite(pixel.depthM) || !(pixel.depthM > 0.0F) ||
      !std::isfinite(pixel.normalCamera[0]) ||
      !std::isfinite(pixel.normalCamera[1]) ||
      !std::isfinite(pixel.normalCamera[2])) {
    throw std::invalid_argument(
        prefix + "mask: hit requires mask=1 and valid nonzero products");
  }
  const double normalNorm =
      std::hypot(std::hypot(pixel.normalCamera[0], pixel.normalCamera[1]),
                 pixel.normalCamera[2]);
  if (std::abs(normalNorm - 1.0) > 1.0e-5) {
    throw std::invalid_argument(
        prefix + "normalCamera: hit normal must be finite and unit");
  }
  if (!std::isfinite(pixel.mu0) || pixel.mu0 < 0.0F ||
      pixel.mu0 > 1.0F) {
    throw std::invalid_argument(
        prefix + "mu0: hit value must be finite and in [0,1]");
  }
  const auto direct =
      static_cast<std::uint8_t>(IlluminationClass::Direct);
  const auto night =
      static_cast<std::uint8_t>(IlluminationClass::Night);
  const auto selfShadow =
      static_cast<std::uint8_t>(IlluminationClass::SelfShadow);
  if (pixel.illuminationClass != direct &&
      pixel.illuminationClass != night &&
      pixel.illuminationClass != selfShadow) {
    throw std::invalid_argument(
        prefix + "illuminationClass: hit class must be 1, 2, or 3");
  }
  if (albedoEnabled &&
      !isRenderableRelativeAlbedo(pixel.relativeAlbedoLinear)) {
    throw std::invalid_argument(
        prefix + "relativeAlbedoLinear: hit value must be finite and "
                 "positive");
  }
}

template <typename Value>
void writeBuffer(TiffHandle &handle, int x, int y,
                 const std::vector<Value> &buffer) {
  const tmsize_t bytes = checkedTiffBytes(
      buffer.size(), sizeof(Value), "TIFF tile byte count");
  if (TIFFWriteTile(handle.value,
                    const_cast<Value *>(buffer.data()), x, y, 0, 0) !=
      bytes) {
    throw std::runtime_error("TIFF " + handle.path.string() +
                             ": failed to write complete tile");
  }
}

void validateStructure(const std::filesystem::path &path, int width,
                       int height, int tileWidth, int tileHeight,
                       std::uint16_t bits, std::uint16_t samples,
                       std::uint16_t sampleFormat) {
  TIFF *tiff = TIFFOpen(path.c_str(), "r");
  if (tiff == nullptr) {
    throw std::runtime_error("TIFF validation: cannot reopen " +
                             path.string());
  }
  std::uint32_t actualWidth = 0;
  std::uint32_t actualHeight = 0;
  std::uint32_t actualTileWidth = 0;
  std::uint32_t actualTileHeight = 0;
  std::uint16_t actualBits = 0;
  std::uint16_t actualSamples = 0;
  std::uint16_t actualFormat = 0;
  std::uint16_t compression = 0;
  const bool valid =
      TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &actualWidth) == 1 &&
      TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &actualHeight) == 1 &&
      TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &actualTileWidth) == 1 &&
      TIFFGetField(tiff, TIFFTAG_TILELENGTH, &actualTileHeight) == 1;
  TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &actualBits);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &actualSamples);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &actualFormat);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_COMPRESSION, &compression);
  TIFFClose(tiff);
  if (!valid || actualWidth != static_cast<std::uint32_t>(width) ||
      actualHeight != static_cast<std::uint32_t>(height) ||
      actualTileWidth != static_cast<std::uint32_t>(tileWidth) ||
      actualTileHeight != static_cast<std::uint32_t>(tileHeight) ||
      actualBits != bits || actualSamples != samples ||
      actualFormat != sampleFormat || compression != COMPRESSION_LZW) {
    throw std::runtime_error("TIFF validation: structural tag mismatch in " +
                             path.string());
  }
}

}  // namespace

class TiffProductWriter::Implementation {
  public:
    Implementation(ProductPaths paths, int width, int height, int tileWidth,
                   int tileHeight)
        : paths_(std::move(paths)),
          width_(width),
          height_(height),
          tileWidth_(tileWidth),
          tileHeight_(tileHeight) {
      const bool hasMu0Path = !paths_.mu0Float32.empty();
      const bool hasIlluminationPath =
          !paths_.illuminationMaskU8.empty();
      if (hasMu0Path != hasIlluminationPath) {
        if (!hasMu0Path) {
          throw std::invalid_argument(
              "paths.mu0Float32: required when "
              "paths.illuminationMaskU8 is provided");
        }
        throw std::invalid_argument(
            "paths.illuminationMaskU8: required when "
            "paths.mu0Float32 is provided");
      }
      diagnosticsEnabled_ = hasMu0Path;
      albedoEnabled_ = paths_.albedoFloat32.has_value();
      if (width_ <= 0 || height_ <= 0) {
        throw std::invalid_argument("image dimensions: must be positive");
      }
      tilePixels_ = validateTileWorkset(tileWidth_, tileHeight_);
      tileColumns_ =
          positiveCeilingDivision(width_, tileWidth_, "tile_columns");
      tileRows_ =
          positiveCeilingDivision(height_, tileHeight_, "tile_rows");
      tileCount_ =
          checkedMultiply(tileColumns_, tileRows_, "tile_count");
      const std::vector<std::filesystem::path> outputPaths =
          validateOutputPaths(paths_, diagnosticsEnabled_);
      std::vector<std::filesystem::path> createdByThisConstruction;
      createdByThisConstruction.reserve(outputPaths.size());
      try {
        image_ = openProduct(paths_.imageU16, width_, height_, tileWidth_,
                             tileHeight_, 16, 1, SAMPLEFORMAT_UINT, false,
                             createdByThisConstruction);
        mask_ = openProduct(paths_.maskU8, width_, height_, tileWidth_,
                            tileHeight_, 8, 1, SAMPLEFORMAT_UINT, false,
                            createdByThisConstruction);
        depth_ = openProduct(paths_.depthMFloat32, width_, height_,
                             tileWidth_, tileHeight_, 32, 1,
                             SAMPLEFORMAT_IEEEFP, false,
                             createdByThisConstruction);
        normal_ = openProduct(paths_.normalCameraFloat32, width_, height_,
                              tileWidth_, tileHeight_, 32, 3,
                              SAMPLEFORMAT_IEEEFP, true,
                              createdByThisConstruction);
        facet_ = openProduct(paths_.facetIdU32, width_, height_, tileWidth_,
                             tileHeight_, 32, 1, SAMPLEFORMAT_UINT, false,
                             createdByThisConstruction);
        if (diagnosticsEnabled_) {
          mu0_ = openProduct(paths_.mu0Float32, width_, height_, tileWidth_,
                             tileHeight_, 32, 1, SAMPLEFORMAT_IEEEFP,
                             false, createdByThisConstruction);
          illumination_ =
              openProduct(paths_.illuminationMaskU8, width_, height_,
                          tileWidth_, tileHeight_, 8, 1,
                          SAMPLEFORMAT_UINT, false,
                          createdByThisConstruction);
        }
        if (albedoEnabled_) {
          albedo_ = openProduct(paths_.albedoFloat32.value(), width_,
                                height_, tileWidth_, tileHeight_, 32, 1,
                                SAMPLEFORMAT_IEEEFP, false,
                                createdByThisConstruction);
        }
      }
      catch (const std::exception &error) {
        const std::string original = error.what();
        closeAll();
        const std::vector<std::string> cleanupFailures =
            removeCreatedProducts(createdByThisConstruction);
        if (!cleanupFailures.empty()) {
          throw std::runtime_error(
              constructionCleanupError(original, cleanupFailures));
        }
        throw;
      }
      catch (...) {
        closeAll();
        const std::vector<std::string> cleanupFailures =
            removeCreatedProducts(createdByThisConstruction);
        if (!cleanupFailures.empty()) {
          throw std::runtime_error(constructionCleanupError(
              "non-standard exception", cleanupFailures));
        }
        throw;
      }
    }

    ~Implementation() {
      closeAll();
    }

    void writeTile(const TileBounds &bounds,
                   const std::vector<PixelProducts> &pixels) {
      if (closed_) {
        throw std::logic_error("writer closed: cannot write tile");
      }
      if (nextTile_ >= tileCount_) {
        throw std::invalid_argument(
            "tile order: bounds do not match the next logical tile");
      }
      const std::size_t tileColumn = nextTile_ % tileColumns_;
      const std::size_t tileRow = nextTile_ / tileColumns_;
      const int expectedX =
          checkedTileOrigin(tileColumn, tileWidth_, "tile x origin");
      const int expectedY =
          checkedTileOrigin(tileRow, tileHeight_, "tile y origin");
      const int expectedWidth = std::min(tileWidth_, width_ - expectedX);
      const int expectedHeight =
          std::min(tileHeight_, height_ - expectedY);
      if (bounds.x != expectedX || bounds.y != expectedY ||
          bounds.width != expectedWidth ||
          bounds.height != expectedHeight) {
        throw std::invalid_argument(
            "tile order: bounds do not match the next logical tile");
      }
      if (bounds.width <= 0 || bounds.height <= 0) {
        throw std::invalid_argument(
            "tile bounds: width and height must be positive");
      }
      const std::size_t logicalCount = checkedMultiply(
          static_cast<std::size_t>(bounds.width),
          static_cast<std::size_t>(bounds.height), "pixel count");
      if (pixels.size() != logicalCount) {
        throw std::invalid_argument(
            "pixel count: must equal logical tile width times height");
      }
      for (std::size_t index = 0; index < pixels.size(); ++index) {
        validatePixel(pixels[index], index, albedoEnabled_);
      }

      const std::size_t paddedCount = tilePixels_;
      const float nan = std::numeric_limits<float>::quiet_NaN();
      std::vector<std::uint16_t> image(paddedCount, 0);
      std::vector<std::uint8_t> mask(paddedCount, 0);
      std::vector<float> depth(paddedCount, nan);
      std::vector<float> normal(
          checkedMultiply(3U, paddedCount, "normal tile elements"), nan);
      std::vector<std::uint32_t> facet(paddedCount, 0);
      std::vector<float> mu0;
      std::vector<std::uint8_t> illumination;
      std::vector<float> albedo;
      if (diagnosticsEnabled_) {
        mu0.assign(paddedCount, nan);
        illumination.assign(paddedCount, 0);
      }
      if (albedoEnabled_) {
        albedo.assign(paddedCount, nan);
      }
      for (int y = 0; y < bounds.height; ++y) {
        for (int x = 0; x < bounds.width; ++x) {
          const std::size_t source =
              static_cast<std::size_t>(y) *
                  static_cast<std::size_t>(bounds.width) +
              static_cast<std::size_t>(x);
          const std::size_t target =
              static_cast<std::size_t>(y) *
                  static_cast<std::size_t>(tileWidth_) +
              static_cast<std::size_t>(x);
          const PixelProducts &pixel = pixels[source];
          image[target] = pixel.dn;
          mask[target] = pixel.mask;
          depth[target] = pixel.depthM;
          facet[target] = pixel.plateId;
          if (diagnosticsEnabled_) {
            mu0[target] = pixel.mu0;
            illumination[target] = pixel.illuminationClass;
          }
          if (albedoEnabled_) {
            albedo[target] = pixel.relativeAlbedoLinear;
          }
          for (std::size_t band = 0; band < 3; ++band) {
            normal[3 * target + band] = pixel.normalCamera[band];
          }
        }
      }
      writeBuffer(image_, bounds.x, bounds.y, image);
      writeBuffer(mask_, bounds.x, bounds.y, mask);
      writeBuffer(depth_, bounds.x, bounds.y, depth);
      writeBuffer(normal_, bounds.x, bounds.y, normal);
      writeBuffer(facet_, bounds.x, bounds.y, facet);
      if (diagnosticsEnabled_) {
        writeBuffer(mu0_, bounds.x, bounds.y, mu0);
        writeBuffer(illumination_, bounds.x, bounds.y, illumination);
      }
      if (albedoEnabled_) {
        writeBuffer(albedo_, bounds.x, bounds.y, albedo);
      }
      ++nextTile_;
    }

    void closeAndValidate() {
      if (closed_) {
        throw std::logic_error("writer closed: closeAndValidate called twice");
      }
      if (nextTile_ != tileCount_) {
        throw std::logic_error(
            "incomplete image: every logical tile must be written");
      }
      closeAll();
      closed_ = true;
      validateStructure(paths_.imageU16, width_, height_, tileWidth_,
                        tileHeight_, 16, 1, SAMPLEFORMAT_UINT);
      validateStructure(paths_.maskU8, width_, height_, tileWidth_,
                        tileHeight_, 8, 1, SAMPLEFORMAT_UINT);
      validateStructure(paths_.depthMFloat32, width_, height_, tileWidth_,
                        tileHeight_, 32, 1, SAMPLEFORMAT_IEEEFP);
      validateStructure(paths_.normalCameraFloat32, width_, height_,
                        tileWidth_, tileHeight_, 32, 3,
                        SAMPLEFORMAT_IEEEFP);
      validateStructure(paths_.facetIdU32, width_, height_, tileWidth_,
                        tileHeight_, 32, 1, SAMPLEFORMAT_UINT);
      if (diagnosticsEnabled_) {
        validateStructure(paths_.mu0Float32, width_, height_, tileWidth_,
                          tileHeight_, 32, 1, SAMPLEFORMAT_IEEEFP);
        validateStructure(paths_.illuminationMaskU8, width_, height_,
                          tileWidth_, tileHeight_, 8, 1,
                          SAMPLEFORMAT_UINT);
      }
      if (albedoEnabled_) {
        validateStructure(paths_.albedoFloat32.value(), width_, height_,
                          tileWidth_, tileHeight_, 32, 1,
                          SAMPLEFORMAT_IEEEFP);
      }
    }

  private:
    void closeAll() {
      close(albedo_);
      close(illumination_);
      close(mu0_);
      close(facet_);
      close(normal_);
      close(depth_);
      close(mask_);
      close(image_);
    }

    ProductPaths paths_;
    int width_;
    int height_;
    int tileWidth_;
    int tileHeight_;
    std::size_t tilePixels_{0};
    std::size_t tileColumns_{0};
    std::size_t tileRows_{0};
    std::size_t tileCount_{0};
    std::size_t nextTile_{0};
    bool closed_{false};
    bool diagnosticsEnabled_{false};
    bool albedoEnabled_{false};
    TiffHandle image_;
    TiffHandle mask_;
    TiffHandle depth_;
    TiffHandle normal_;
    TiffHandle facet_;
    TiffHandle mu0_;
    TiffHandle illumination_;
    TiffHandle albedo_;
};

TiffProductWriter::TiffProductWriter(
    const ProductPaths &paths, int widthPx, int heightPx, int tileWidthPx,
    int tileHeightPx)
    : implementation_(std::make_unique<Implementation>(
          paths, widthPx, heightPx, tileWidthPx, tileHeightPx)) {}

TiffProductWriter::~TiffProductWriter() = default;

void TiffProductWriter::writeTile(
    const TileBounds &bounds,
    const std::vector<PixelProducts> &pixels) {
  implementation_->writeTile(bounds, pixels);
}

void TiffProductWriter::closeAndValidate() {
  implementation_->closeAndValidate();
}

IlluminationProductCounts validatePointSunDiagnosticProducts(
    const ProductPaths &paths, int widthPx, int heightPx,
    int tileWidthPx, int tileHeightPx) {
  if (widthPx <= 0 || heightPx <= 0 || paths.maskU8.empty() ||
      paths.mu0Float32.empty() || paths.illuminationMaskU8.empty()) {
    throw std::invalid_argument(
        "ILLUMINATION_PRODUCT_ERROR: paths and dimensions must be "
        "complete and positive");
  }
  const std::size_t tilePixels =
      validateTileWorkset(
          tileWidthPx, tileHeightPx, "ILLUMINATION_PRODUCT_ERROR: ");
  constexpr std::size_t DiagnosticBytesPerPixel =
      2 * sizeof(std::uint8_t) + sizeof(float);
  static_cast<void>(
      checkedMultiply(tilePixels, DiagnosticBytesPerPixel,
                      "ILLUMINATION_PRODUCT_ERROR: tile workset bytes"));
  const tmsize_t expectedByteTile =
      checkedTiffBytes(tilePixels, sizeof(std::uint8_t),
                       "tile_pixels");
  const tmsize_t expectedFloatTile =
      checkedTiffBytes(tilePixels, sizeof(float), "tile_pixels");
  const std::size_t tileColumns =
      positiveCeilingDivision(widthPx, tileWidthPx, "tile_columns");
  const std::size_t tileRows =
      positiveCeilingDivision(heightPx, tileHeightPx, "tile_rows");
  static_cast<void>(
      checkedMultiply(tileColumns, tileRows, "tile_count"));
  TiffHandle mask{TIFFOpen(paths.maskU8.c_str(), "r"), paths.maskU8};
  TiffHandle mu0{TIFFOpen(paths.mu0Float32.c_str(), "r"),
                  paths.mu0Float32};
  TiffHandle illumination{
      TIFFOpen(paths.illuminationMaskU8.c_str(), "r"),
      paths.illuminationMaskU8};
  try {
    const std::array<
        std::tuple<TiffHandle *, std::uint16_t, std::uint16_t>, 3>
        expectedProducts{{
            {&mask, 8, SAMPLEFORMAT_UINT},
            {&mu0, 32, SAMPLEFORMAT_IEEEFP},
            {&illumination, 8, SAMPLEFORMAT_UINT},
        }};
    for (const auto &expected : expectedProducts) {
      TiffHandle *handle = std::get<0>(expected);
      if (handle->value == nullptr) {
        throw std::runtime_error(
            "cannot open " + handle->path.string());
      }
      std::uint32_t width = 0;
      std::uint32_t height = 0;
      std::uint32_t tileWidth = 0;
      std::uint32_t tileHeight = 0;
      std::uint16_t bits = 0;
      std::uint16_t samples = 0;
      std::uint16_t sampleFormat = 0;
      std::uint16_t planar = 0;
      std::uint16_t compression = 0;
      std::uint16_t orientation = 0;
      std::uint16_t photometric = 0;
      TIFFGetFieldDefaulted(handle->value, TIFFTAG_BITSPERSAMPLE, &bits);
      TIFFGetFieldDefaulted(handle->value, TIFFTAG_SAMPLESPERPIXEL,
                            &samples);
      TIFFGetFieldDefaulted(handle->value, TIFFTAG_SAMPLEFORMAT,
                            &sampleFormat);
      TIFFGetFieldDefaulted(handle->value, TIFFTAG_PLANARCONFIG, &planar);
      TIFFGetFieldDefaulted(handle->value, TIFFTAG_COMPRESSION,
                            &compression);
      TIFFGetFieldDefaulted(handle->value, TIFFTAG_ORIENTATION,
                            &orientation);
      TIFFGetFieldDefaulted(handle->value, TIFFTAG_PHOTOMETRIC,
                            &photometric);
      if (!TIFFIsTiled(handle->value) ||
          TIFFGetField(handle->value, TIFFTAG_IMAGEWIDTH, &width) != 1 ||
          TIFFGetField(handle->value, TIFFTAG_IMAGELENGTH, &height) != 1 ||
          TIFFGetField(handle->value, TIFFTAG_TILEWIDTH, &tileWidth) != 1 ||
          TIFFGetField(handle->value, TIFFTAG_TILELENGTH, &tileHeight) != 1 ||
          width != static_cast<std::uint32_t>(widthPx) ||
          height != static_cast<std::uint32_t>(heightPx) ||
          tileWidth != static_cast<std::uint32_t>(tileWidthPx) ||
          tileHeight != static_cast<std::uint32_t>(tileHeightPx) ||
          bits != std::get<1>(expected) || samples != 1 ||
          sampleFormat != std::get<2>(expected) ||
          planar != PLANARCONFIG_CONTIG ||
          compression != COMPRESSION_LZW ||
          orientation != ORIENTATION_TOPLEFT ||
          photometric != PHOTOMETRIC_MINISBLACK) {
        throw std::runtime_error(
            handle->path.string() +
            ": expected exact single-band production tiled layout");
      }
    }

    std::vector<std::uint8_t> maskValues(tilePixels);
    std::vector<float> mu0Values(tilePixels);
    std::vector<std::uint8_t> illuminationValues(tilePixels);
    IlluminationProductCounts counts{0, 0, 0, 0};
    for (std::size_t tileRow = 0; tileRow < tileRows; ++tileRow) {
      const int y =
          checkedTileOrigin(tileRow, tileHeightPx, "tile y origin");
      for (std::size_t tileColumn = 0; tileColumn < tileColumns;
           ++tileColumn) {
        const int x =
            checkedTileOrigin(tileColumn, tileWidthPx, "tile x origin");
        const tmsize_t maskBytes =
            TIFFReadTile(mask.value, maskValues.data(), x, y, 0, 0);
        const tmsize_t mu0Bytes =
            TIFFReadTile(mu0.value, mu0Values.data(), x, y, 0, 0);
        const tmsize_t illuminationBytes = TIFFReadTile(
            illumination.value, illuminationValues.data(), x, y, 0, 0);
        if (maskBytes != expectedByteTile ||
            mu0Bytes != expectedFloatTile ||
            illuminationBytes != expectedByteTile) {
          throw std::runtime_error(
              "failed to decode aligned diagnostic tile at x=" +
              std::to_string(x) + ", y=" + std::to_string(y));
        }
        const int validWidth = std::min(tileWidthPx, widthPx - x);
        const int validHeight = std::min(tileHeightPx, heightPx - y);
        for (int localY = 0; localY < validHeight; ++localY) {
          for (int localX = 0; localX < validWidth; ++localX) {
            const std::size_t index =
                static_cast<std::size_t>(localY) *
                    static_cast<std::size_t>(tileWidthPx) +
                static_cast<std::size_t>(localX);
            const std::uint8_t geometry = maskValues[index];
            const float incidence = mu0Values[index];
            const std::uint8_t classification =
                illuminationValues[index];
            const std::string location =
                " at sample=" + std::to_string(x + localX) +
                ", line=" + std::to_string(y + localY);
            if (geometry == 0) {
              if (!std::isnan(incidence) || classification != 0) {
                throw std::runtime_error(
                    "background requires NaN mu0 and class 0" + location);
              }
              continue;
            }
            if (geometry != 1 || !std::isfinite(incidence) ||
                incidence < 0.0F || incidence > 1.0F ||
                classification < 1 || classification > 3) {
              throw std::runtime_error(
                  "valid geometry requires mask 1, finite mu0 in [0,1], "
                  "and class 1/2/3" +
                  location);
            }
            ++counts.validPixels;
            if (classification == 1) {
              if (!(incidence > 0.0F)) {
                throw std::runtime_error(
                    "direct class 1 requires positive mu0" + location);
              }
              ++counts.directPixels;
            }
            else if (classification == 2) {
              if (incidence != 0.0F) {
                throw std::runtime_error(
                    "night class 2 requires zero mu0" + location);
              }
              ++counts.nightPixels;
            }
            else {
              if (!(incidence > 0.0F)) {
                throw std::runtime_error(
                    "self-shadow class 3 requires positive mu0 and "
                    "valid geometry" +
                    location);
              }
              ++counts.selfShadowPixels;
            }
          }
        }
      }
    }
    close(illumination);
    close(mu0);
    close(mask);
    return counts;
  }
  catch (const std::exception &error) {
    close(illumination);
    close(mu0);
    close(mask);
    const std::string message(error.what());
    if (message.rfind("ILLUMINATION_PRODUCT_ERROR:", 0) == 0) {
      throw;
    }
    throw std::runtime_error("ILLUMINATION_PRODUCT_ERROR: " + message);
  }
}

}  // namespace Isis::SmallBodyRender
