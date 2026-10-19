// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "OpenCvRadtanCamera.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace Isis::SmallBodyRender {
namespace {

using nlohmann::json;

constexpr const char *kCameraModelError = "CAMERA_MODEL_ERROR";

[[noreturn]] void reject(const std::string &field,
                         const std::string &reason) {
  throw std::invalid_argument(std::string(kCameraModelError) + ": " + field +
                              ": " + reason);
}

std::string precise(double value) {
  std::ostringstream stream;
  stream << std::setprecision(17) << value;
  return stream.str();
}

void requireExactObject(const json &value,
                        std::initializer_list<const char *> keys,
                        const std::string &field) {
  if (!value.is_object()) {
    reject(field, "must be an object");
  }
  for (const auto &[key, unused] : value.items()) {
    static_cast<void>(unused);
    bool known = false;
    for (const char *allowed : keys) {
      if (key == allowed) {
        known = true;
        break;
      }
    }
    if (!known) {
      reject(key, "unknown field");
    }
  }
  for (const char *required : keys) {
    if (!value.contains(required)) {
      reject(required, "required field is missing");
    }
  }
}

const std::string &requireString(const json &value, const std::string &field) {
  if (!value.is_string()) {
    reject(field, "must be a string");
  }
  return value.get_ref<const std::string &>();
}

double requireFiniteNumber(const json &value, const std::string &field) {
  if (!value.is_number()) {
    reject(field, "must be a finite number");
  }
  const double number = value.get<double>();
  if (!std::isfinite(number)) {
    reject(field, "must be a finite number");
  }
  return number;
}

int requirePositiveInteger(const json &value, const std::string &field) {
  if (value.is_number_integer()) {
    const long long number = value.get<long long>();
    if (number <= 0 || number > std::numeric_limits<int>::max()) {
      reject(field, "must be a positive integer");
    }
    return static_cast<int>(number);
  }
  if (value.is_number_unsigned()) {
    const unsigned long long number = value.get<unsigned long long>();
    if (number == 0 || number > static_cast<unsigned long long>(
                                      std::numeric_limits<int>::max())) {
      reject(field, "must be a positive integer");
    }
    return static_cast<int>(number);
  }
  reject(field, "must be a positive integer");
}

void requireExactInteger(const json &value, const std::string &field,
                         int expected) {
  if (value.is_number_integer()) {
    if (value.get<long long>() == expected) {
      return;
    }
  } else if (value.is_number_unsigned()) {
    if (expected >= 0 &&
        value.get<unsigned long long>() ==
            static_cast<unsigned long long>(expected)) {
      return;
    }
  }
  reject(field, "must equal " + std::to_string(expected));
}

void requireExactSchemaVersion(const json &value) {
  if (value.is_number_integer() && value.get<long long>() == 1) {
    return;
  }
  if (value.is_number_unsigned() && value.get<unsigned long long>() == 1U) {
    return;
  }
  reject("schema_version", "must equal 1");
}

void requireExactString(const json &value, const std::string &field,
                        const char *expected) {
  if (requireString(value, field) != expected) {
    reject(field, "must equal '" + std::string(expected) + "'");
  }
}

}  // namespace

OpenCvRadtanCamera::OpenCvRadtanCamera(
    PinholeIntrinsics intrinsics, Radtan5Coefficients coefficients,
    DistortionInverseControls inverseControls)
    : intrinsics_(intrinsics),
      distortion_(intrinsics, coefficients),
      inverseControls_(inverseControls) {}

OpenCvRadtanCamera OpenCvRadtanCamera::read(
    const std::filesystem::path &path, const FrameState &frame,
    DistortionInverseControls inverseControls) {
  std::ifstream stream(path);
  if (!stream) {
    reject("camera_path", "cannot open " + path.string());
  }

  json root;
  try {
    stream >> root;
  }
  catch (const json::exception &error) {
    reject("camera_path", "invalid JSON: " + std::string(error.what()));
  }
  if (stream.bad()) {
    reject("camera_path", "failed while reading JSON");
  }

  requireExactObject(root,
                     {"schema_version", "model", "pixel_coordinate",
                      "image_size_px", "intrinsics_px", "distortion"},
                     "root");
  requireExactSchemaVersion(root.at("schema_version"));
  requireExactString(root.at("model"), "model", "opencv_radtan5");

  const json &pixelCoordinate = root.at("pixel_coordinate");
  requireExactObject(pixelCoordinate,
                     {"origin", "index_base", "integer_location",
                      "sample_axis", "line_axis"},
                     "pixel_coordinate");
  requireExactString(pixelCoordinate.at("origin"), "origin", "top_left");
  requireExactInteger(pixelCoordinate.at("index_base"), "index_base", 0);
  requireExactString(pixelCoordinate.at("integer_location"),
                     "integer_location", "pixel_center");
  requireExactString(pixelCoordinate.at("sample_axis"), "sample_axis",
                     "right");
  requireExactString(pixelCoordinate.at("line_axis"), "line_axis", "down");

  const json &imageSize = root.at("image_size_px");
  if (!imageSize.is_array() || imageSize.size() != 2) {
    reject("image_size_px", "must be a two-element [width, height] array");
  }
  const int widthPx = requirePositiveInteger(imageSize.at(0), "image_size_px");
  const int heightPx = requirePositiveInteger(imageSize.at(1), "image_size_px");
  if (widthPx != frame.widthPx || heightPx != frame.heightPx) {
    reject("image_size_px", "must match FrameState widthPx and heightPx");
  }

  const json &intrinsics = root.at("intrinsics_px");
  requireExactObject(intrinsics, {"fx", "fy", "cx", "cy", "skew"},
                     "intrinsics_px");
  const PinholeIntrinsics pinhole{
      requireFiniteNumber(intrinsics.at("fx"), "fx"),
      requireFiniteNumber(intrinsics.at("fy"), "fy"),
      requireFiniteNumber(intrinsics.at("cx"), "cx"),
      requireFiniteNumber(intrinsics.at("cy"), "cy")};
  if (!(pinhole.fxPx > 0.0)) {
    reject("fx", "must be positive px");
  }
  if (!(pinhole.fyPx > 0.0)) {
    reject("fy", "must be positive px");
  }
  if (requireFiniteNumber(intrinsics.at("skew"), "skew") != 0.0) {
    reject("skew", "must equal 0");
  }

  const json &distortion = root.at("distortion");
  requireExactObject(distortion, {"mapping", "coefficients"},
                     "distortion");
  requireExactString(distortion.at("mapping"), "mapping",
                     "ideal_to_observed");
  const json &coefficients = distortion.at("coefficients");
  requireExactObject(coefficients, {"k1", "k2", "p1", "p2", "k3"},
                     "coefficients");
  const Radtan5Coefficients radtan{
      requireFiniteNumber(coefficients.at("k1"), "k1"),
      requireFiniteNumber(coefficients.at("k2"), "k2"),
      requireFiniteNumber(coefficients.at("p1"), "p1"),
      requireFiniteNumber(coefficients.at("p2"), "p2"),
      requireFiniteNumber(coefficients.at("k3"), "k3")};
  return OpenCvRadtanCamera(pinhole, radtan, inverseControls);
}

Vector3 OpenCvRadtanCamera::rayCamera(double samplePx, double linePx) const {
  if (!std::isfinite(samplePx) || !std::isfinite(linePx)) {
    reject("samplePx,linePx", "must be finite");
  }
  ++rayStatistics_.requestedPixelCount;
  Eigen::Vector2d ideal;
  try {
    const DistortionInverseResult inverse =
        distortion_.undistortObservedPixel(
            {samplePx, linePx}, inverseControls_.maximumIterations,
            inverseControls_.roundTripTolerancePx);
    rayStatistics_.maximumInverseIterations = std::max(
        rayStatistics_.maximumInverseIterations, inverse.iterations);
    rayStatistics_.maximumRoundTripErrorPx = std::max(
        rayStatistics_.maximumRoundTripErrorPx, inverse.roundTripErrorPx);
    ideal = inverse.idealPixelPx;
  }
  catch (const std::exception &error) {
    ++rayStatistics_.inverseFailureCount;
    throw std::runtime_error(
        "DISTORTION_INVERSE_ERROR: samplePx=" + precise(samplePx) +
        " linePx=" + precise(linePx) + " configuredMaximumIterations=" +
        std::to_string(inverseControls_.maximumIterations) +
        " error=" + error.what());
  }
  return Vector3((ideal.x() - intrinsics_.cxPx) / intrinsics_.fxPx,
                 (ideal.y() - intrinsics_.cyPx) / intrinsics_.fyPx, 1.0)
      .normalized();
}

CameraRayStatistics OpenCvRadtanCamera::rayStatistics() const {
  return rayStatistics_;
}

}  // namespace Isis::SmallBodyRender
