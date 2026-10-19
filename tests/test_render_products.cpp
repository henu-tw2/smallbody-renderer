// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include <tiffio.h>

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "SolarLambert.h"
#include "TiffProductWriter.h"

namespace {

using Isis::SmallBodyRender::PixelProducts;
using Isis::SmallBodyRender::ProductPaths;
using Isis::SmallBodyRender::TiffProductWriter;
using Isis::SmallBodyRender::TileBounds;
using Isis::SmallBodyRender::IlluminationClass;
using Isis::SmallBodyRender::validatePointSunDiagnosticProducts;

void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

template <typename Callable>
void requireRejects(Callable callable, const std::string &field) {
  try {
    callable();
  }
  catch (const std::exception &error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "diagnostic must contain '" + field + "': " + error.what());
    return;
  }
  require(false, "expected rejection for " + field);
}

class TemporaryProducts {
  public:
    TemporaryProducts() {
      static std::size_t sequence = 0;
      do {
        root_ = std::filesystem::temp_directory_path() /
                ("smallbody-render-products-" +
                 std::to_string(++sequence));
      } while (!std::filesystem::create_directory(root_));
    }

    ~TemporaryProducts() {
      std::error_code error;
      std::filesystem::remove_all(root_, error);
    }

    ProductPaths paths() const {
      return {root_ / "image_u16.tif", root_ / "mask_u8.tif",
              root_ / "depth_m_f32.tif",
              root_ / "normal_camera_f32.tif",
              root_ / "facet_id_u32.tif",
              root_ / "mu0_f32.tif",
              root_ / "illumination_mask_u8.tif",
              std::nullopt};
    }

    ProductPaths pathsWithoutDiagnostics() const {
      return {root_ / "image_u16.tif", root_ / "mask_u8.tif",
              root_ / "depth_m_f32.tif",
              root_ / "normal_camera_f32.tif",
              root_ / "facet_id_u32.tif",
              {},
              {},
              std::nullopt};
    }

    const std::filesystem::path &root() const {
      return root_;
    }

  private:
    std::filesystem::path root_;
};

void requireDirectoryEmpty(const std::filesystem::path &path,
                           const std::string &message) {
  for (const auto &entry : std::filesystem::directory_iterator(path)) {
    require(false, message + ": unexpected " + entry.path().string());
  }
}

struct CleanupFailureInjection {
  std::filesystem::path createdProduct;
  bool invoked{false};
  bool replacedWithNonemptyDirectory{false};
};

CleanupFailureInjection *activeCleanupFailureInjection = nullptr;

void injectCleanupFailure(const char *, const char *, va_list) {
  CleanupFailureInjection *injection =
      activeCleanupFailureInjection;
  if (injection == nullptr || injection->invoked) {
    return;
  }
  injection->invoked = true;
  std::error_code error;
  const bool removed =
      std::filesystem::remove(injection->createdProduct, error);
  if (!removed || error) {
    return;
  }
  const bool directoryCreated =
      std::filesystem::create_directory(
          injection->createdProduct, error);
  if (!directoryCreated || error) {
    return;
  }
  std::ofstream marker(injection->createdProduct / "keep");
  marker << "force remove failure";
  marker.close();
  injection->replacedWithNonemptyDirectory =
      static_cast<bool>(marker);
}

PixelProducts pixel(int x, int y) {
  const auto illuminationClass = static_cast<std::uint8_t>(
      IlluminationClass::Background);
  if ((x + y) % 4 == 0) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    return {0, 0, nan, {nan, nan, nan}, 0, nan, illuminationClass};
  }
  const int illuminationCase = (x + y) % 4;
  const float mu0 =
      illuminationCase == 1 ? 0.75F
                            : (illuminationCase == 2 ? 0.0F : 0.5F);
  const auto hitClass = static_cast<std::uint8_t>(
      illuminationCase == 1
          ? IlluminationClass::Direct
          : (illuminationCase == 2 ? IlluminationClass::Night
                                   : IlluminationClass::SelfShadow));
  return {static_cast<std::uint16_t>(1000 + 10 * y + x),
          1,
          static_cast<float>(20.0 + y + 0.01 * x),
          {0.0F, 0.0F, 1.0F},
          static_cast<std::uint32_t>(1 + (x + y) % 4),
          mu0,
          hitClass};
}

std::vector<PixelProducts> tilePixels(const TileBounds &bounds) {
  std::vector<PixelProducts> result;
  for (int y = 0; y < bounds.height; ++y) {
    for (int x = 0; x < bounds.width; ++x) {
      result.push_back(pixel(bounds.x + x, bounds.y + y));
    }
  }
  return result;
}

struct ExpectedTags {
  std::uint16_t bits;
  std::uint16_t samples;
  std::uint16_t sampleFormat;
};

void requireTags(const std::filesystem::path &path,
                 const ExpectedTags &expected, bool normalProduct = false) {
  TIFF *tiff = TIFFOpen(path.c_str(), "r");
  require(tiff != nullptr, "reopen " + path.string());
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t tileWidth = 0;
  std::uint32_t tileHeight = 0;
  std::uint16_t bits = 0;
  std::uint16_t samples = 0;
  std::uint16_t sampleFormat = 0;
  std::uint16_t compression = 0;
  std::uint16_t planar = 0;
  std::uint16_t orientation = 0;
  std::uint16_t photometric = 0;
  require(TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) == 1 &&
              TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) == 1 &&
              TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tileWidth) == 1 &&
              TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tileHeight) == 1,
          "required TIFF dimensions");
  TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sampleFormat);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_COMPRESSION, &compression);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_ORIENTATION, &orientation);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_PHOTOMETRIC, &photometric);
  require(width == 19 && height == 17 && tileWidth == 16 &&
              tileHeight == 16,
          "exact logical and tile dimensions");
  require(bits == expected.bits && samples == expected.samples &&
              sampleFormat == expected.sampleFormat,
          "exact sample tags");
  require(compression == COMPRESSION_LZW &&
              planar == PLANARCONFIG_CONTIG &&
              orientation == ORIENTATION_TOPLEFT &&
              photometric == PHOTOMETRIC_MINISBLACK,
          "lossless contiguous tiled top-left grayscale layout");
  void *unused = nullptr;
  require(TIFFGetField(tiff, 33550, &unused) == 0 &&
              TIFFGetField(tiff, 33922, &unused) == 0 &&
              TIFFGetField(tiff, 34735, &unused) == 0,
          "no GeoTIFF transform or coordinate-system tags");
  if (normalProduct) {
    std::uint16_t count = 0;
    std::uint16_t *types = nullptr;
    require(TIFFGetField(tiff, TIFFTAG_EXTRASAMPLES, &count, &types) == 1 &&
                count == 2 && types[0] == EXTRASAMPLE_UNSPECIFIED &&
                types[1] == EXTRASAMPLE_UNSPECIFIED,
            "normal bands use two unspecified extra samples");
  }
  TIFFClose(tiff);
}

template <typename Value>
std::vector<Value> readTile(const std::filesystem::path &path, int x, int y,
                            int samples) {
  TIFF *tiff = TIFFOpen(path.c_str(), "r");
  require(tiff != nullptr, "reopen tile product");
  std::vector<Value> buffer(static_cast<std::size_t>(16 * 16 * samples));
  require(TIFFReadTile(tiff, buffer.data(), x, y, 0, 0) >= 0,
          "read TIFF tile");
  TIFFClose(tiff);
  return buffer;
}

void testExactTagsValuesAndEdgeTiles() {
  TemporaryProducts products;
  const ProductPaths paths = products.paths();
  TiffProductWriter writer(paths, 19, 17, 16, 16);
  for (const TileBounds bounds :
       {TileBounds{0, 0, 16, 16}, {16, 0, 3, 16},
        {0, 16, 16, 1}, {16, 16, 3, 1}}) {
    writer.writeTile(bounds, tilePixels(bounds));
  }
  writer.closeAndValidate();
  requireRejects(
      [&] {
        const TileBounds bounds{0, 0, 16, 16};
        writer.writeTile(bounds, tilePixels(bounds));
      },
      "closed");
  requireRejects([&] { writer.closeAndValidate(); }, "closed");

  requireTags(paths.imageU16, {16, 1, SAMPLEFORMAT_UINT});
  requireTags(paths.maskU8, {8, 1, SAMPLEFORMAT_UINT});
  requireTags(paths.depthMFloat32, {32, 1, SAMPLEFORMAT_IEEEFP});
  requireTags(paths.normalCameraFloat32, {32, 3, SAMPLEFORMAT_IEEEFP}, true);
  requireTags(paths.facetIdU32, {32, 1, SAMPLEFORMAT_UINT});
  requireTags(paths.mu0Float32, {32, 1, SAMPLEFORMAT_IEEEFP});
  requireTags(paths.illuminationMaskU8, {8, 1, SAMPLEFORMAT_UINT});

  const auto dn = readTile<std::uint16_t>(paths.imageU16, 16, 16, 1);
  const auto mask = readTile<std::uint8_t>(paths.maskU8, 16, 16, 1);
  const auto depth = readTile<float>(paths.depthMFloat32, 16, 16, 1);
  const auto normal =
      readTile<float>(paths.normalCameraFloat32, 16, 16, 3);
  const auto plate = readTile<std::uint32_t>(paths.facetIdU32, 16, 16, 1);
  const auto mu0 = readTile<float>(paths.mu0Float32, 16, 16, 1);
  const auto illumination =
      readTile<std::uint8_t>(paths.illuminationMaskU8, 16, 16, 1);
  for (int localX = 0; localX < 3; ++localX) {
    const PixelProducts expected = pixel(16 + localX, 16);
    require(dn[localX] == expected.dn && mask[localX] == expected.mask &&
                plate[localX] == expected.plateId &&
                illumination[localX] == expected.illuminationClass,
            "exact bottom-right edge-tile integer values");
    if (expected.mask == 0) {
      require(std::isnan(depth[localX]) &&
                  std::isnan(normal[3 * localX]) &&
                  std::isnan(normal[3 * localX + 1]) &&
                  std::isnan(normal[3 * localX + 2]) &&
                  std::isnan(mu0[localX]),
              "background float products remain NaN");
    }
    else {
      require(depth[localX] == expected.depthM &&
                  normal[3 * localX + 2] == 1.0F &&
                  mu0[localX] == expected.mu0,
              "hit float products remain exact");
    }
  }
  for (std::size_t index = 0; index < 16U * 16U; ++index) {
    const std::size_t localX = index % 16U;
    const std::size_t localY = index / 16U;
    if (localX < 3U && localY == 0U) {
      continue;
    }
    require(dn[index] == 0 && mask[index] == 0 &&
                std::isnan(depth[index]) &&
                std::isnan(normal[3 * index]) &&
                std::isnan(normal[3 * index + 1]) &&
                std::isnan(normal[3 * index + 2]) &&
                plate[index] == 0 && std::isnan(mu0[index]) &&
                illumination[index] ==
                    static_cast<std::uint8_t>(
                        IlluminationClass::Background),
            "every physical bottom-right tile padding sample uses "
            "background values");
  }

  const auto firstMask =
      readTile<std::uint8_t>(paths.maskU8, 0, 0, 1);
  const auto firstMu0 = readTile<float>(paths.mu0Float32, 0, 0, 1);
  const auto firstIllumination =
      readTile<std::uint8_t>(paths.illuminationMaskU8, 0, 0, 1);
  require(firstMask[0] == 0 && std::isnan(firstMu0[0]) &&
              firstIllumination[0] ==
                  static_cast<std::uint8_t>(
                      IlluminationClass::Background),
          "background is geometrically invalid with NaN mu0 and class 0");
  require(firstMask[1] == 1 && firstMu0[1] == 0.75F &&
              firstIllumination[1] ==
                  static_cast<std::uint8_t>(
                      IlluminationClass::Direct),
          "direct illumination preserves geometry mask and class 1");
  require(firstMask[2] == 1 && firstMu0[2] == 0.0F &&
              firstIllumination[2] ==
                  static_cast<std::uint8_t>(
                      IlluminationClass::Night),
          "night illumination preserves geometry mask and class 2");
  require(firstMask[3] == 1 && firstMu0[3] == 0.5F &&
              firstIllumination[3] ==
                  static_cast<std::uint8_t>(
                      IlluminationClass::SelfShadow),
          "self-shadow preserves geometry mask, positive mu0, and class 3");
}

void testJointReadbackRejectsCorruptedMu0Semantics() {
  TemporaryProducts products;
  const ProductPaths paths = products.paths();
  TiffProductWriter writer(paths, 19, 17, 16, 16);
  for (const TileBounds bounds :
       {TileBounds{0, 0, 16, 16}, {16, 0, 3, 16},
        {0, 16, 16, 1}, {16, 16, 3, 1}}) {
    writer.writeTile(bounds, tilePixels(bounds));
  }
  writer.closeAndValidate();
  const auto counts =
      validatePointSunDiagnosticProducts(paths, 19, 17, 16, 16);
  require(counts.directPixels + counts.nightPixels +
                  counts.selfShadowPixels ==
              counts.validPixels &&
              counts.validPixels > 0,
          "joint readback partitions valid geometry");

  TIFF *mu0 = TIFFOpen(paths.mu0Float32.c_str(), "r+");
  require(mu0 != nullptr, "open mu0 for controlled corruption");
  std::vector<float> zeros(16U * 16U, 0.0F);
  require(TIFFWriteTile(mu0, zeros.data(), 0, 0, 0, 0) >= 0,
          "replace first mu0 tile with finite zeros");
  TIFFClose(mu0);
  requireRejects(
      [&] {
        static_cast<void>(
            validatePointSunDiagnosticProducts(paths, 19, 17, 16, 16));
      },
      "ILLUMINATION_PRODUCT_ERROR");
}

template <typename Mutator>
void requireInvalidPixel(Mutator mutator, const std::string &field) {
  TemporaryProducts products;
  TiffProductWriter writer(products.paths(), 19, 17, 16, 16);
  auto values = tilePixels({0, 0, 16, 16});
  mutator(values[1]);
  requireRejects(
      [&] { writer.writeTile({0, 0, 16, 16}, values); }, field);
}

void testRejectsOrderingCountsLifecycleAndInvalidTuples() {
  {
    TemporaryProducts products;
    TiffProductWriter writer(products.paths(), 19, 17, 16, 16);
    requireRejects(
        [&] {
          const TileBounds second{16, 0, 3, 16};
          writer.writeTile(second, tilePixels(second));
        },
        "tile order");
  }
  {
    TemporaryProducts products;
    TiffProductWriter writer(products.paths(), 19, 17, 16, 16);
    const TileBounds first{0, 0, 16, 16};
    writer.writeTile(first, tilePixels(first));
    requireRejects(
        [&] { writer.writeTile(first, tilePixels(first)); },
        "tile order");
  }
  {
    TemporaryProducts products;
    TiffProductWriter writer(products.paths(), 19, 17, 16, 16);
    requireRejects(
        [&] {
          writer.writeTile({0, 0, 16, 16},
                           std::vector<PixelProducts>(1, pixel(1, 0)));
        },
        "pixel count");
  }
  {
    TemporaryProducts products;
    TiffProductWriter writer(products.paths(), 19, 17, 16, 16);
    auto values = tilePixels({0, 0, 16, 16});
    values[0] = {1,
                 0,
                 2.0F,
                 {0.0F, 0.0F, 1.0F},
                 1,
                 0.5F,
                 static_cast<std::uint8_t>(
                     IlluminationClass::Direct)};
    requireRejects(
        [&] { writer.writeTile({0, 0, 16, 16}, values); },
        "mask");
  }
  {
    TemporaryProducts products;
    TiffProductWriter writer(products.paths(), 19, 17, 16, 16);
    requireRejects([&] { writer.closeAndValidate(); }, "incomplete");
  }

  requireInvalidPixel(
      [](PixelProducts &value) {
        value.mu0 = std::numeric_limits<float>::quiet_NaN();
      },
      "mu0");
  requireInvalidPixel([](PixelProducts &value) { value.mu0 = -0.01F; },
                      "mu0");
  requireInvalidPixel([](PixelProducts &value) { value.mu0 = 1.01F; },
                      "mu0");
  requireInvalidPixel(
      [](PixelProducts &value) {
        value.mu0 = std::numeric_limits<float>::infinity();
      },
      "mu0");
  requireInvalidPixel(
      [](PixelProducts &value) {
        value.illuminationClass = static_cast<std::uint8_t>(
            IlluminationClass::Background);
      },
      "illuminationClass");
  requireInvalidPixel(
      [](PixelProducts &value) { value.illuminationClass = 4; },
      "illuminationClass");
  requireInvalidPixel(
      [](PixelProducts &value) {
        value.mask = 0;
        value.dn = 0;
        value.depthM = std::numeric_limits<float>::quiet_NaN();
        value.normalCamera.fill(
            std::numeric_limits<float>::quiet_NaN());
        value.plateId = 0;
        value.mu0 = 0.0F;
        value.illuminationClass = static_cast<std::uint8_t>(
            IlluminationClass::Background);
      },
      "mu0");
  requireInvalidPixel(
      [](PixelProducts &value) {
        value.mask = 0;
        value.dn = 0;
        value.depthM = std::numeric_limits<float>::quiet_NaN();
        value.normalCamera.fill(
            std::numeric_limits<float>::quiet_NaN());
        value.plateId = 0;
        value.mu0 = std::numeric_limits<float>::quiet_NaN();
        value.illuminationClass = static_cast<std::uint8_t>(
            IlluminationClass::Direct);
      },
      "illuminationClass");
}

void testOptionalDiagnosticPathsArePairedAndDisabledCleanly() {
  {
    TemporaryProducts products;
    TiffProductWriter writer(
        products.pathsWithoutDiagnostics(), 19, 17, 16, 16);
    auto values = tilePixels({0, 0, 16, 16});
    values[1] = {1000, 1, 20.0F, {0.0F, 0.0F, 1.0F}, 1};
    requireRejects(
        [&] { writer.writeTile({0, 0, 16, 16}, values); }, "mu0");
    values[1].mu0 = 0.5F;
    requireRejects(
        [&] { writer.writeTile({0, 0, 16, 16}, values); },
        "illuminationClass");
  }
  {
    TemporaryProducts products;
    const ProductPaths paths = products.pathsWithoutDiagnostics();
    TiffProductWriter writer(paths, 19, 17, 16, 16);
    for (const TileBounds bounds :
         {TileBounds{0, 0, 16, 16}, {16, 0, 3, 16},
          {0, 16, 16, 1}, {16, 16, 3, 1}}) {
      writer.writeTile(bounds, tilePixels(bounds));
    }
    writer.closeAndValidate();
    require(!std::filesystem::exists(products.root() / "mu0_f32.tif") &&
                !std::filesystem::exists(
                    products.root() / "illumination_mask_u8.tif"),
            "disabled diagnostic paths do not create diagnostic files");
  }
  {
    TemporaryProducts products;
    ProductPaths paths = products.pathsWithoutDiagnostics();
    paths.mu0Float32 = products.root() / "mu0_f32.tif";
    requireRejects(
        [&] { TiffProductWriter writer(paths, 19, 17, 16, 16); },
        "illuminationMaskU8");
    requireDirectoryEmpty(
        products.root(), "partial diagnostic paths create no file");
  }
  {
    TemporaryProducts products;
    ProductPaths paths = products.pathsWithoutDiagnostics();
    paths.illuminationMaskU8 =
        products.root() / "illumination_mask_u8.tif";
    requireRejects(
        [&] { TiffProductWriter writer(paths, 19, 17, 16, 16); },
        "mu0Float32");
    requireDirectoryEmpty(
        products.root(), "partial diagnostic paths create no file");
  }
}

void testOptionalAlbedoProductIsAbsentOrWritesLinearFloat32() {
  {
    TemporaryProducts products;
    ProductPaths paths = products.pathsWithoutDiagnostics();
    paths.albedoFloat32 = std::nullopt;
    TiffProductWriter writer(paths, 19, 17, 16, 16);
    for (const TileBounds bounds :
         {TileBounds{0, 0, 16, 16}, {16, 0, 3, 16},
          {0, 16, 16, 1}, {16, 16, 3, 1}}) {
      auto values = tilePixels(bounds);
      for (std::size_t index = 0; index < values.size(); ++index) {
        values[index].relativeAlbedoLinear =
            values[index].mask == 0
                ? std::numeric_limits<float>::quiet_NaN()
                : (index == 1U ? 0.5F : 1.0F);
      }
      writer.writeTile(bounds, values);
    }
    writer.closeAndValidate();
    require(!std::filesystem::exists(products.root() / "albedo_f32.tif"),
            "disabled optional albedo path does not create a TIFF");
  }
  {
    TemporaryProducts products;
    ProductPaths paths = products.pathsWithoutDiagnostics();
    const std::filesystem::path albedoPath =
        products.root() / "albedo_f32.tif";
    paths.albedoFloat32 = albedoPath;
    TiffProductWriter writer(paths, 19, 17, 16, 16);
    for (const TileBounds bounds :
         {TileBounds{0, 0, 16, 16}, {16, 0, 3, 16},
          {0, 16, 16, 1}, {16, 16, 3, 1}}) {
      auto values = tilePixels(bounds);
      for (std::size_t index = 0; index < values.size(); ++index) {
        values[index].relativeAlbedoLinear =
            values[index].mask == 0
                ? std::numeric_limits<float>::quiet_NaN()
                : (index == 1U ? 0.5F : 1.0F);
      }
      writer.writeTile(bounds, values);
    }
    writer.closeAndValidate();

    requireTags(albedoPath, {32, 1, SAMPLEFORMAT_IEEEFP});
    const auto albedo = readTile<float>(albedoPath, 0, 0, 1);
    require(std::isnan(albedo[0]),
            "albedo background is NaN");
    require(albedo[1] == 0.5F && albedo[2] == 1.0F,
            "albedo valid faces retain expected finite linear values");
  }
}

void testConstructionFailureIsTransactional() {
  {
    TemporaryProducts products;
    ProductPaths paths = products.paths();
    paths.mu0Float32 = products.root() / "missing" / "mu0_f32.tif";
    requireRejects(
        [&] { TiffProductWriter writer(paths, 19, 17, 16, 16); },
        paths.mu0Float32.string());
    requireDirectoryEmpty(
        products.root(), "sixth product failure leaves no product file");
  }
  {
    TemporaryProducts products;
    ProductPaths paths = products.paths();
    paths.illuminationMaskU8 =
        products.root() / "missing" / "illumination_mask_u8.tif";
    requireRejects(
        [&] { TiffProductWriter writer(paths, 19, 17, 16, 16); },
        paths.illuminationMaskU8.string());
    requireDirectoryEmpty(
        products.root(), "seventh product failure leaves no product file");
  }
  {
    TemporaryProducts products;
    ProductPaths paths = products.paths();
    paths.maskU8 = paths.imageU16;
    requireRejects(
        [&] { TiffProductWriter writer(paths, 19, 17, 16, 16); },
        "duplicate");
    requireDirectoryEmpty(
        products.root(), "duplicate product paths create no file");
  }
  {
    TemporaryProducts products;
    ProductPaths paths = products.paths();
    paths.depthMFloat32.clear();
    requireRejects(
        [&] { TiffProductWriter writer(paths, 19, 17, 16, 16); },
        "depthMFloat32");
    requireDirectoryEmpty(
        products.root(), "empty legacy path creates no file");
  }
  {
    TemporaryProducts products;
    const ProductPaths paths = products.paths();
    const std::string sentinel = "do not overwrite";
    {
      std::ofstream stream(paths.illuminationMaskU8);
      stream << sentinel;
    }
    requireRejects(
        [&] { TiffProductWriter writer(paths, 19, 17, 16, 16); },
        paths.illuminationMaskU8.string());
    std::ifstream stream(paths.illuminationMaskU8);
    const std::string actual{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    require(actual == sentinel,
            "pre-existing target sentinel remains unchanged");
    std::size_t entryCount = 0;
    for (const auto &entry :
         std::filesystem::directory_iterator(products.root())) {
      ++entryCount;
      require(entry.path() == paths.illuminationMaskU8,
              "pre-existing target rejection creates no other file");
    }
    require(entryCount == 1,
            "only the pre-existing sentinel remains after rejection");
  }
  {
    TemporaryProducts products;
    const ProductPaths paths = products.paths();
    const std::filesystem::path target =
        products.root() / "symlink-target";
    const std::string sentinel = "symlink target sentinel";
    {
      std::ofstream stream(target);
      stream << sentinel;
    }
    std::filesystem::create_symlink(
        target, paths.illuminationMaskU8);
    requireRejects(
        [&] { TiffProductWriter writer(paths, 19, 17, 16, 16); },
        paths.illuminationMaskU8.string());
    std::ifstream stream(target);
    const std::string actual{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    require(actual == sentinel,
            "pre-existing symlink target remains unchanged");
    require(std::filesystem::is_symlink(
                std::filesystem::symlink_status(
                    paths.illuminationMaskU8)),
            "pre-existing output symlink remains intact");
    for (const std::filesystem::path &path :
         {paths.imageU16, paths.maskU8, paths.depthMFloat32,
          paths.normalCameraFloat32, paths.facetIdU32,
          paths.mu0Float32}) {
      require(!std::filesystem::exists(path),
              "symlink target rejection creates no product " +
                  path.string());
    }
  }
}

void testSymlinkedParentAliasRejectedBeforeConstruction() {
  TemporaryProducts products;
  const std::filesystem::path real = products.root() / "real";
  const std::filesystem::path alias = products.root() / "alias";
  std::filesystem::create_directory(real);
  std::filesystem::create_directory_symlink(real, alias);
  ProductPaths paths = products.paths();
  paths.imageU16 = real / "shared.tif";
  paths.maskU8 = alias / "shared.tif";
  requireRejects(
      [&] { TiffProductWriter writer(paths, 19, 17, 16, 16); },
      "duplicate");
  requireDirectoryEmpty(
      real, "symlinked-parent alias rejection creates no shared product");
  for (const std::filesystem::path &path :
       {paths.depthMFloat32, paths.normalCameraFloat32,
        paths.facetIdU32, paths.mu0Float32,
        paths.illuminationMaskU8}) {
    require(!std::filesystem::exists(path),
            "symlinked-parent alias rejection creates no product " +
                path.string());
  }
}

void testCleanupFailureReportsOriginalAndLeftoverPath() {
  TemporaryProducts products;
  ProductPaths paths = products.paths();
  paths.mu0Float32 = products.root() / "missing" / "mu0_f32.tif";
  CleanupFailureInjection injection{paths.imageU16};
  activeCleanupFailureInjection = &injection;
  TIFFErrorHandler previousHandler =
      TIFFSetErrorHandler(injectCleanupFailure);
  std::string message;
  try {
    TiffProductWriter writer(paths, 19, 17, 16, 16);
  }
  catch (const std::exception &error) {
    message = error.what();
  }
  TIFFSetErrorHandler(previousHandler);
  activeCleanupFailureInjection = nullptr;

  require(injection.invoked &&
              injection.replacedWithNonemptyDirectory,
          "libtiff error seam creates a controlled remove failure");
  require(message.find(paths.mu0Float32.string()) != std::string::npos,
          "combined failure preserves original construction path");
  require(message.find("cleanup") != std::string::npos &&
              message.find(paths.imageU16.string()) !=
                  std::string::npos,
          "combined failure reports cleanup path");
  require(std::filesystem::is_directory(paths.imageU16) &&
              std::filesystem::is_regular_file(
                  paths.imageU16 / "keep"),
          "reported cleanup failure path is the controlled nonempty "
          "directory");
  for (const std::filesystem::path &path :
       {paths.maskU8, paths.depthMFloat32,
        paths.normalCameraFloat32, paths.facetIdU32,
        paths.illuminationMaskU8}) {
    require(!std::filesystem::exists(path),
            "cleanup continues after one remove failure: " +
                path.string());
  }
}

void testRuntimeWriteFailureRetainsAttemptProducts() {
  TemporaryProducts products;
  const ProductPaths paths = products.paths();
  {
    TiffProductWriter writer(paths, 19, 17, 16, 16);
    requireRejects(
        [&] {
          writer.writeTile(
              {0, 0, 16, 16},
              std::vector<PixelProducts>(1, pixel(1, 0)));
        },
        "pixel count");
  }
  for (const std::filesystem::path &path :
       {paths.imageU16, paths.maskU8, paths.depthMFloat32,
        paths.normalCameraFloat32, paths.facetIdU32,
        paths.mu0Float32, paths.illuminationMaskU8}) {
    require(std::filesystem::is_regular_file(path),
            "runtime write rejection retains attempt product " +
                path.string());
  }
}

void testTileSafetyIsDefendedAtWriterAndValidatorEntrypoints() {
  {
    TemporaryProducts products;
    TiffProductWriter writer(products.paths(), 5000, 3000, 4096, 256);
    require(std::filesystem::is_regular_file(products.paths().imageU16),
            "maximum safe tile workset reaches TIFF construction");
  }

  const auto requireUnsafeConstructor =
      [](int tileWidth, int tileHeight, const std::string &field,
         const std::string &limit) {
        TemporaryProducts products;
        try {
          TiffProductWriter writer(
              products.paths(), 5000, 3000, tileWidth, tileHeight);
        }
        catch (const std::exception &error) {
          const std::string message = error.what();
          require(message.find(field) != std::string::npos,
                  "constructor diagnostic contains " + field + ": " +
                      message);
          require(message.find(limit) != std::string::npos,
                  "constructor diagnostic contains limit " + limit + ": " +
                      message);
          requireDirectoryEmpty(
              products.root(),
              "unsafe constructor rejects before creating TIFF products");
          return;
        }
        require(false, "unsafe constructor must reject " + field);
      };
  requireUnsafeConstructor(4112, 16, "tile_width_px", "4096");
  requireUnsafeConstructor(16, 4112, "tile_height_px", "4096");
  requireUnsafeConstructor(4096, 272, "tile_pixels", "1048576");
  requireUnsafeConstructor(65536, 16, "tile_width_px", "4096");
  requireUnsafeConstructor(
      2147483632, 16, "tile_width_px", "4096");

  TemporaryProducts products;
  for (const auto &[tileWidth, tileHeight, field] :
       std::array<std::tuple<int, int, const char *>, 3>{
           std::tuple{4112, 16, "tile_width_px"},
           std::tuple{4096, 272, "tile_pixels"},
           std::tuple{2147483632, 16, "tile_width_px"}}) {
    requireRejects(
        [&] {
          static_cast<void>(validatePointSunDiagnosticProducts(
              products.paths(), 5000, 3000, tileWidth, tileHeight));
        },
        "ILLUMINATION_PRODUCT_ERROR: " + std::string(field));
  }
  requireDirectoryEmpty(
      products.root(),
      "unsafe validator rejects before opening or allocating products");
}

}  // namespace

int main() {
  testExactTagsValuesAndEdgeTiles();
  testJointReadbackRejectsCorruptedMu0Semantics();
  testRejectsOrderingCountsLifecycleAndInvalidTuples();
  testOptionalDiagnosticPathsArePairedAndDisabledCleanly();
  testOptionalAlbedoProductIsAbsentOrWritesLinearFloat32();
  testConstructionFailureIsTransactional();
  testSymlinkedParentAliasRejectedBeforeConstruction();
  testCleanupFailureReportsOriginalAndLeftoverPath();
  testRuntimeWriteFailureRetainsAttemptProducts();
  testTileSafetyIsDefendedAtWriterAndValidatorEntrypoints();
  std::cout << "PASS: tiled render product tests\n";
  return EXIT_SUCCESS;
}
