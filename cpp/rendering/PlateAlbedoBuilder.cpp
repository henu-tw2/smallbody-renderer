// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "PlateAlbedoBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "TiffSingleBandRaster.h"
#include "SurfaceAppearance.h"

namespace Isis::SmallBodyRender {
namespace {

constexpr char kEstimationMethod[] =
    "source_linear_median_without_photometric_correction";
constexpr char kNormalizationMethod[] = "valid_plate_median";

[[noreturn]] void reject(const std::string &message) {
  throw std::invalid_argument("PLATE_ALBEDO_BUILDER_ERROR: " + message);
}

double median(std::vector<double> values) {
  if (values.empty()) reject("median requires at least one value");
  std::sort(values.begin(), values.end());
  const std::size_t midpoint = values.size() / 2;
  if (values.size() % 2 == 0) {
    return 0.5 * values.at(midpoint - 1) + 0.5 * values.at(midpoint);
  }
  return values.at(midpoint);
}

void requireSourceMetadata(const PlateAlbedoSourceFrame &source) {
  if (source.frameId.empty()) reject("frame_id must not be empty");
  if (!std::isfinite(source.blackLevelDn) ||
      !std::isfinite(source.saturationDn) ||
      !(source.saturationDn > source.blackLevelDn)) {
    reject("frame " + source.frameId +
           " requires finite saturation_dn greater than black_level_dn");
  }
}

void requireDiagnosticTypes(const TiffSingleBandRaster &facet,
                            const TiffSingleBandRaster &geometry,
                            const TiffSingleBandRaster &illumination,
                            const std::string &frameId) {
  if (facet.sampleType() != RasterSampleType::UInt32) {
    reject("frame " + frameId + " facet_id_tif must be uint32");
  }
  if (geometry.sampleType() != RasterSampleType::UInt8) {
    reject("frame " + frameId + " geometry_mask_tif must be uint8");
  }
  if (illumination.sampleType() != RasterSampleType::UInt8) {
    reject("frame " + frameId + " illumination_mask_tif must be uint8");
  }
}

struct PlateAccumulator {
  std::vector<double> frameMedians;
  std::uint64_t pixelCount{0};
};

}  // namespace

PlateAlbedoBuildResult PlateAlbedoBuilder::build(
    const std::vector<PlateAlbedoSourceFrame> &sourceFrames) {
  if (sourceFrames.empty()) reject("source frames must not be empty");
  if (sourceFrames.size() > std::numeric_limits<std::uint32_t>::max()) {
    reject("source frame count exceeds uint32 range");
  }

  std::map<std::uint32_t, PlateAccumulator> perPlate;
  PlateAlbedoBuildStatistics statistics{
      static_cast<std::uint32_t>(sourceFrames.size()), 0, 0, 0,
      kEstimationMethod, 1.0, kNormalizationMethod};

  for (const PlateAlbedoSourceFrame &source : sourceFrames) {
    requireSourceMetadata(source);
    const TiffSingleBandRaster observed =
        TiffSingleBandRaster::read(source.observedLinearTif);
    if (observed.sampleType() != RasterSampleType::UInt8 &&
        observed.sampleType() != RasterSampleType::UInt16 &&
        observed.sampleType() != RasterSampleType::Float32) {
      reject("frame " + source.frameId +
             " observed_linear_tif must be uint8, uint16, or float32");
    }
    if (observed.sampleType() == RasterSampleType::UInt8 &&
        source.sourceKind != "linear_display_stretch") {
      reject("frame " + source.frameId +
             " uint8 observed_linear_tif requires source_kind "
             "linear_display_stretch");
    }
    const int width = observed.widthPx();
    const int height = observed.heightPx();
    const TiffSingleBandRaster facet = TiffSingleBandRaster::read(
        source.facetIdTif, width, height);
    const TiffSingleBandRaster geometry = TiffSingleBandRaster::read(
        source.geometryMaskTif, width, height);
    const TiffSingleBandRaster illumination = TiffSingleBandRaster::read(
        source.illuminationMaskTif, width, height);
    requireDiagnosticTypes(facet, geometry, illumination, source.frameId);

    const std::size_t pixelCount = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height);
    std::vector<double> frameSignals;
    std::map<std::uint32_t, std::vector<double>> signalsByPlate;
    for (std::size_t index = 0; index < pixelCount; ++index) {
      const std::uint32_t plateId = facet.valueAsUInt32(index);
      if (plateId == 0 || geometry.valueAsUInt32(index) != 1 ||
          illumination.valueAsUInt32(index) != 1) {
        continue;
      }
      const double observedValue = observed.valueAsDouble(index);
      if (!std::isfinite(observedValue) ||
          observedValue >= source.saturationDn) {
        continue;
      }
      const double signal = observedValue - source.blackLevelDn;
      if (!std::isfinite(signal) || !(signal > 0.0)) continue;
      frameSignals.push_back(signal);
      signalsByPlate[plateId].push_back(signal);
    }
    if (frameSignals.empty()) continue;

    const double frameMedian = median(std::move(frameSignals));
    if (!std::isfinite(frameMedian) || !(frameMedian > 0.0)) {
      reject("frame " + source.frameId + " has invalid valid-signal median");
    }
    ++statistics.contributingFrameCount;
    for (const auto &[plateId, signals] : signalsByPlate) {
      std::vector<double> normalized;
      normalized.reserve(signals.size());
      for (const double signal : signals) {
        const double normalizedSignal = signal / frameMedian;
        if (!std::isfinite(normalizedSignal) ||
            !(normalizedSignal > 0.0)) {
          reject("frame " + source.frameId +
                 " produced an invalid normalized signal");
        }
        normalized.push_back(normalizedSignal);
      }
      PlateAccumulator &accumulator = perPlate[plateId];
      accumulator.frameMedians.push_back(median(std::move(normalized)));
      if (accumulator.pixelCount >
          std::numeric_limits<std::uint64_t>::max() - signals.size()) {
        reject("plate pixel count exceeds uint64 range");
      }
      accumulator.pixelCount += signals.size();
      if (statistics.validPixelCount >
          std::numeric_limits<std::uint64_t>::max() - signals.size()) {
        reject("valid pixel count exceeds uint64 range");
      }
      statistics.validPixelCount += signals.size();
    }
  }

  std::vector<PlateAlbedoEstimate> estimates;
  estimates.reserve(perPlate.size());
  for (const auto &[plateId, accumulator] : perPlate) {
    if (accumulator.frameMedians.size() >
        std::numeric_limits<std::uint32_t>::max()) {
      reject("plate source count exceeds uint32 range");
    }
    const double finalMedian = median(accumulator.frameMedians);
    if (!std::isfinite(finalMedian) || !(finalMedian > 0.0)) {
      reject("plate " + std::to_string(plateId) +
             " produced an invalid cross-frame median");
    }
    const double rootCount =
        std::sqrt(static_cast<double>(accumulator.frameMedians.size()));
    double rms = 0.0;
    for (const double frameMedian : accumulator.frameMedians) {
      const double residual = frameMedian - finalMedian;
      if (!std::isfinite(residual)) {
        reject("plate " + std::to_string(plateId) +
               " produced a non-finite residual");
      }
      rms = std::hypot(rms, residual / rootCount);
    }
    if (!std::isfinite(rms) || rms < 0.0) {
      reject("plate " + std::to_string(plateId) +
             " produced an invalid source RMS");
    }
    estimates.push_back(
        {plateId, finalMedian, true,
         static_cast<std::uint32_t>(accumulator.frameMedians.size()),
         accumulator.pixelCount, rms});
    ++statistics.validPlateCount;
  }

  if (!estimates.empty()) {
    std::vector<double> validValues;
    validValues.reserve(estimates.size());
    for (const PlateAlbedoEstimate &estimate : estimates) {
      validValues.push_back(estimate.relativeAlbedoLinear);
    }
    statistics.normalizationFactor = median(std::move(validValues));
    if (!std::isfinite(statistics.normalizationFactor) ||
        !(statistics.normalizationFactor > 0.0)) {
      reject("valid-plate normalization factor must be finite and positive");
    }
    for (PlateAlbedoEstimate &estimate : estimates) {
      estimate.relativeAlbedoLinear /= statistics.normalizationFactor;
      estimate.sourceRms /= statistics.normalizationFactor;
      if (!isRenderableRelativeAlbedo(estimate.relativeAlbedoLinear)) {
        reject("plate " + std::to_string(estimate.plateId) +
               " relative albedo must remain finite and positive in IEEE "
               "float32");
      }
      if (!std::isfinite(estimate.sourceRms) || estimate.sourceRms < 0.0) {
        reject("plate " + std::to_string(estimate.plateId) +
               " normalized source RMS must be finite and non-negative");
      }
    }
  }
  return {std::move(estimates), std::move(statistics)};
}

}  // namespace Isis::SmallBodyRender
