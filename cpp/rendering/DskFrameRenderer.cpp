// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "DskFrameRenderer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

#include "LambertHeadlight.h"
#include "SolarLambert.h"

namespace Isis::SmallBodyRender {
namespace {

constexpr int MaximumTileEdgePx = 4096;
constexpr std::size_t MaximumTilePixels = 1048576;

PixelProducts backgroundPixel() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  return {0,
          0,
          nan,
          {nan, nan, nan},
          0,
          nan,
          static_cast<std::uint8_t>(
              IlluminationClass::Background)};
}

void requireValidHit(const SurfaceHit &hit, const Vector3 &originBodyM,
                     const Vector3 &directionBody,
                     bool requireUnitNormal) {
  const double normalScale =
      hit.normalBody.allFinite()
          ? hit.normalBody.cwiseAbs().maxCoeff()
          : std::numeric_limits<double>::quiet_NaN();
  if (!std::isfinite(hit.depthM) || !(hit.depthM > 0.0) ||
      !hit.pointBodyM.allFinite() || !hit.normalBody.allFinite() ||
      !(normalScale > 0.0) ||
      (requireUnitNormal &&
       std::abs(hit.normalBody.norm() - 1.0) > 1.0e-10) ||
      hit.plateId == 0) {
    throw std::runtime_error(
        "SurfaceHit: found hit has invalid depth, point, normal, or plateId");
  }
  const Vector3 unitDirection = directionBody.normalized();
  const Vector3 expectedPoint = originBodyM + hit.depthM * unitDirection;
  const double tolerance = std::max(1.0e-6, 1.0e-9 * hit.depthM);
  if ((expectedPoint - hit.pointBodyM).norm() > tolerance) {
    throw std::runtime_error(
        "SurfaceHit: pointBodyM disagrees with origin + depth * ray");
  }
}

Vector3 checkedUnit(const Vector3 &value, const std::string &errorPrefix,
                    const std::string &field) {
  if (!value.allFinite()) {
    throw std::runtime_error(errorPrefix + ": " + field +
                             " must be finite and nonzero");
  }
  const double scale = value.cwiseAbs().maxCoeff();
  if (!std::isfinite(scale) || !(scale > 0.0)) {
    throw std::runtime_error(errorPrefix + ": " + field +
                             " must be finite and nonzero");
  }
  const Vector3 scaled = value / scale;
  const double norm = scaled.norm();
  if (!scaled.allFinite() || !std::isfinite(norm) || !(norm > 0.0)) {
    throw std::runtime_error(errorPrefix + ": " + field +
                             " must be finite and nonzero");
  }
  return scaled / norm;
}

std::size_t checkedMultiply(std::size_t left, std::size_t right,
                            const std::string &field) {
  if (left != 0 &&
      right > std::numeric_limits<std::size_t>::max() / left) {
    throw std::runtime_error(
        "RENDER_TILE_ERROR: " + field + " exceeds size_t");
  }
  return left * right;
}

std::size_t positiveCeilingDivision(int extent, int tile,
                                    const std::string &field) {
  if (extent <= 0 || tile <= 0) {
    throw std::runtime_error(
        "RENDER_TILE_ERROR: " + field +
        " extent and tile must be positive");
  }
  return 1U + static_cast<std::size_t>(extent - 1) /
                  static_cast<std::size_t>(tile);
}

int checkedTileOrigin(std::size_t tileIndex, int tileExtent,
                      const std::string &field) {
  const std::size_t origin = checkedMultiply(
      tileIndex, static_cast<std::size_t>(tileExtent), field);
  if (origin >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error(
        "RENDER_TILE_ERROR: " + field + " exceeds the C++ int range");
  }
  return static_cast<int>(origin);
}

void validateRenderTileWorkset(int width, int height, int tileWidth,
                               int tileHeight) {
  if (width <= 0 || height <= 0) {
    throw std::runtime_error(
        "RENDER_TILE_ERROR: image width_px and height_px must be positive");
  }
  if (tileWidth <= 0 || tileWidth % 16 != 0) {
    throw std::runtime_error(
        "RENDER_TILE_ERROR: tile_width_px must be a positive multiple "
        "of 16");
  }
  if (tileHeight <= 0 || tileHeight % 16 != 0) {
    throw std::runtime_error(
        "RENDER_TILE_ERROR: tile_height_px must be a positive multiple "
        "of 16");
  }
  if (tileWidth > MaximumTileEdgePx) {
    throw std::runtime_error(
        "RENDER_TILE_ERROR: tile_width_px must be <= 4096");
  }
  if (tileHeight > MaximumTileEdgePx) {
    throw std::runtime_error(
        "RENDER_TILE_ERROR: tile_height_px must be <= 4096");
  }
  const auto widthElements = static_cast<std::size_t>(tileWidth);
  const auto heightElements = static_cast<std::size_t>(tileHeight);
  if (heightElements > MaximumTilePixels / widthElements) {
    throw std::runtime_error(
        "RENDER_TILE_ERROR: tile_pixels must be <= 1048576");
  }
}

double checkedRelativeAlbedo(const SurfaceAppearance &appearance,
                             std::uint32_t plateId) {
  const double relativeAlbedo = appearance.relativeAlbedo(plateId);
  if (!isRenderableRelativeAlbedo(relativeAlbedo)) {
    throw std::runtime_error(
        "SURFACE_APPEARANCE_ERROR: relativeAlbedo must remain finite and "
        "positive in IEEE float32");
  }
  return relativeAlbedo;
}

std::uint16_t headlightDn(const SurfaceHit &hit,
                          const Vector3 &originBodyM,
                          const Photometry &photometry,
                          double relativeAlbedo) {
  static_cast<void>(lambertHeadlightDn(
      hit.normalBody, hit.pointBodyM, originBodyM, photometry));
  const Vector3 viewDirectionBody =
      (originBodyM - hit.pointBodyM).normalized();
  const double normalizedIntensity =
      relativeAlbedo * std::clamp(
                           photometry.ambient +
                               photometry.diffuse * std::max(
                                                        0.0,
                                                        hit.normalBody.dot(
                                                            viewDirectionBody)),
                           0.0, 1.0);
  const auto rounded = static_cast<std::uint32_t>(std::llround(
      65535.0 * std::clamp(normalizedIntensity, 0.0, 1.0)));
  return static_cast<std::uint16_t>(
      std::max<std::uint32_t>(1, rounded));
}

}  // namespace

RenderSummary DskFrameRenderer::render(
    const ResolvedRenderConfig &config, const RenderCamera &camera,
    const SurfaceRayTracer &tracer, const SurfaceAppearance &appearance,
    TiffProductWriter &writer) const {
  const FrameState &frame = config.frame;
  validateRenderTileWorkset(
      frame.widthPx, frame.heightPx, config.tileWidthPx,
      config.tileHeightPx);
  const bool pointSun =
      config.illumination.mode == IlluminationMode::PointSunLambert;
  const Matrix3 rotationDskFromJ2000 =
      frame.rotationJ2000FromDsk.transpose();
  Vector3 targetToSunBodyM = Vector3::Zero();
  if (pointSun) {
    if (!std::isfinite(config.exposureScale) ||
        !(config.exposureScale > 0.0)) {
      throw std::runtime_error(
          "ILLUMINATION_CONTRACT_ERROR: exposureScale must be finite "
          "and positive");
    }
    if (!std::isfinite(config.surfaceAppearance.defaultAlbedo) ||
        !(config.surfaceAppearance.defaultAlbedo > 0.0)) {
      throw std::runtime_error(
          "ILLUMINATION_CONTRACT_ERROR: point_sun_lambert requires a "
          "finite positive defaultAlbedo");
    }
    if (!std::isfinite(config.photometry.ambient) ||
        !(config.photometry.ambient > 0.0) ||
        !std::isfinite(config.photometry.diffuse) ||
        config.photometry.diffuse < 0.0 ||
        std::abs(config.photometry.ambient +
                     config.photometry.diffuse -
                 1.0) >
            1.0e-12) {
      throw std::runtime_error(
          "ILLUMINATION_CONTRACT_ERROR: point-sun photometry requires "
          "ambient > 0, diffuse >= 0, and ambient + diffuse == 1");
    }
    if (!frame.targetToSunJ2000M.has_value()) {
      throw std::runtime_error(
          "ILLUMINATION_CONTRACT_ERROR: "
          "frame.targetToSunJ2000M is required for point_sun_lambert");
    }
    static_cast<void>(
        checkedUnit(*frame.targetToSunJ2000M,
                    "ILLUMINATION_STATE_ERROR",
                    "frame.targetToSunJ2000M"));
    targetToSunBodyM =
        rotationDskFromJ2000 * *frame.targetToSunJ2000M;
    static_cast<void>(
        checkedUnit(targetToSunBodyM, "ILLUMINATION_STATE_ERROR",
                    "targetToSunBodyM after transpose(R_JD)"));

    if (!std::isfinite(config.illumination.shadowOriginOffsetM) ||
        !(config.illumination.shadowOriginOffsetM > 0.0)) {
      throw std::runtime_error(
          "SHADOW_CONFIGURATION_ERROR: shadowOriginOffsetM must be "
          "finite and positive metres");
    }
    if (!std::isfinite(config.illumination.shadowMaximumRangeM) ||
        !(config.illumination.shadowMaximumRangeM > 0.0)) {
      throw std::runtime_error(
          "SHADOW_CONFIGURATION_ERROR: shadowMaximumRangeM must be "
          "finite and positive metres");
    }
    const double radiusM = tracer.maximumVertexRadiusM();
    if (!std::isfinite(radiusM) || !(radiusM > 0.0)) {
      throw std::runtime_error(
          "SHADOW_CONFIGURATION_ERROR: maximumVertexRadiusM must be "
          "finite and positive metres from the DSK shape center");
    }
    if (radiusM > std::numeric_limits<double>::max() / 2.0) {
      throw std::runtime_error(
          "SHADOW_CONFIGURATION_ERROR: twice maximumVertexRadiusM "
          "would overflow");
    }
    const double requiredShadowRangeM = 2.0 * radiusM;
    if (config.illumination.castShadows &&
        config.illumination.shadowMaximumRangeM <
            requiredShadowRangeM) {
      throw std::runtime_error(
          "SHADOW_CONFIGURATION_ERROR: shadowMaximumRangeM must be at "
          "least twice maximumVertexRadiusM");
    }
  }
  const std::uint64_t totalPixels =
      static_cast<std::uint64_t>(frame.widthPx) *
      static_cast<std::uint64_t>(frame.heightPx);
  RenderSummary summary{totalPixels,
                        0,
                        std::numeric_limits<std::uint16_t>::max(),
                        0,
                        std::numeric_limits<double>::infinity(),
                        0.0,
                        0,
                        0,
                        0,
                        0};
  std::set<std::uint32_t> plates;
  const Vector3 originBodyM =
      rotationDskFromJ2000 * frame.targetToCameraJ2000M;
  const std::size_t tileColumns = positiveCeilingDivision(
      frame.widthPx, config.tileWidthPx, "tile_columns");
  const std::size_t tileRows = positiveCeilingDivision(
      frame.heightPx, config.tileHeightPx, "tile_rows");
  static_cast<void>(
      checkedMultiply(tileColumns, tileRows, "tile_count"));

  for (std::size_t tileRow = 0; tileRow < tileRows; ++tileRow) {
    const int tileY =
        checkedTileOrigin(tileRow, config.tileHeightPx, "tile y origin");
    for (std::size_t tileColumn = 0; tileColumn < tileColumns;
         ++tileColumn) {
      const int tileX = checkedTileOrigin(
          tileColumn, config.tileWidthPx, "tile x origin");
      const TileBounds bounds{
          tileX,
          tileY,
          std::min(config.tileWidthPx,
                   frame.widthPx - tileX),
          std::min(config.tileHeightPx,
                   frame.heightPx - tileY)};
      std::vector<PixelProducts> pixels;
      pixels.reserve(checkedMultiply(
          static_cast<std::size_t>(bounds.width),
          static_cast<std::size_t>(bounds.height),
          "logical tile pixel count"));
      for (int localY = 0; localY < bounds.height; ++localY) {
        for (int localX = 0; localX < bounds.width; ++localX) {
          const double sample = bounds.x + localX;
          const double line = bounds.y + localY;
          const Vector3 rayCamera = camera.rayCamera(sample, line);
          const Vector3 rayJ2000 =
              frame.rotationJ2000FromCamera * rayCamera;
          const Vector3 rayBody = rotationDskFromJ2000 * rayJ2000;
          const SurfaceHit hit =
              tracer.traceClosest(originBodyM, rayBody);
          if (!hit.found) {
            pixels.push_back(backgroundPixel());
            continue;
          }
          requireValidHit(hit, originBodyM, rayBody, !pointSun);
          const double relativeAlbedo =
              checkedRelativeAlbedo(appearance, hit.plateId);
          std::uint16_t dn = 0;
          float mu0 = 0.0F;
          IlluminationClass illuminationClass =
              IlluminationClass::Direct;
          Vector3 normalForProducts = hit.normalBody;
          if (!pointSun) {
            dn = headlightDn(hit, originBodyM, config.photometry,
                             relativeAlbedo);
            const Vector3 viewDirectionBody =
                (originBodyM - hit.pointBodyM).normalized();
            mu0 = static_cast<float>(
                std::max(0.0,
                         hit.normalBody.dot(viewDirectionBody)));
            ++summary.directPixels;
          }
          else {
            normalForProducts =
                checkedUnit(hit.normalBody,
                            "ILLUMINATION_STATE_ERROR", "normalBody");
            SolarGeometry solar;
            try {
              solar = solarGeometry(
                  normalForProducts, hit.pointBodyM, targetToSunBodyM);
            }
            catch (const std::exception &error) {
              throw std::runtime_error(
                  "ILLUMINATION_STATE_ERROR: " +
                  std::string(error.what()));
            }
            bool occluded = false;
            if (solar.classification == IlluminationClass::Direct &&
                config.illumination.castShadows) {
              const Vector3 shadowOrigin =
                  hit.pointBodyM +
                  config.illumination.shadowOriginOffsetM *
                      normalForProducts;
              if (!shadowOrigin.allFinite()) {
                throw std::runtime_error(
                    "SHADOW_CONFIGURATION_ERROR: shadow origin is "
                    "non-finite");
              }
              occluded = tracer.isOccluded(
                  shadowOrigin, solar.lightDirectionBody,
                  config.illumination.shadowMaximumRangeM);
            }
            dn = solarLambertDn(
                solar.mu0, occluded,
                relativeAlbedo,
                config.exposureScale, config.photometry);
            mu0 = static_cast<float>(solar.mu0);
            illuminationClass =
                occluded ? IlluminationClass::SelfShadow
                         : solar.classification;
            if (illuminationClass == IlluminationClass::Direct) {
              ++summary.directPixels;
            }
            else if (illuminationClass == IlluminationClass::Night) {
              ++summary.nightPixels;
            }
            else {
              ++summary.selfShadowPixels;
            }
          }
          if (dn == 0) {
            throw std::runtime_error(
                "Lambert DN: valid hit must have nonzero DN");
          }
          const Vector3 normalCamera =
              normalBodyToCamera(normalForProducts,
                                 frame.rotationJ2000FromDsk,
                                 frame.rotationJ2000FromCamera);
          pixels.push_back(
              {dn,
               1,
               static_cast<float>(hit.depthM),
               {static_cast<float>(normalCamera.x()),
                static_cast<float>(normalCamera.y()),
                static_cast<float>(normalCamera.z())},
               hit.plateId,
               mu0,
               static_cast<std::uint8_t>(illuminationClass),
               static_cast<float>(relativeAlbedo)});
          ++summary.validPixels;
          summary.minimumValidDn = std::min(summary.minimumValidDn, dn);
          summary.maximumValidDn = std::max(summary.maximumValidDn, dn);
          summary.minimumDepthM =
              std::min(summary.minimumDepthM, hit.depthM);
          summary.maximumDepthM =
              std::max(summary.maximumDepthM, hit.depthM);
          plates.insert(hit.plateId);
        }
      }
      writer.writeTile(bounds, pixels);
    }
  }
  writer.closeAndValidate();
  if (summary.validPixels == 0) {
    throw std::runtime_error(
        "NO_VALID_PIXELS: every camera ray missed the DSK");
  }
  summary.distinctPlateCount =
      static_cast<std::uint64_t>(plates.size());
  return summary;
}

}  // namespace Isis::SmallBodyRender
