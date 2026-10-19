// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "TsaiPinholeCamera.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Isis::SmallBodyRender {
namespace {

constexpr const char *kCameraModelError = "CAMERA_MODEL_ERROR";

[[noreturn]] void reject(const std::string &field,
                         const std::string &reason) {
  throw std::invalid_argument(std::string(kCameraModelError) + ": " + field +
                              ": " + reason);
}

[[noreturn]] void rejectRuntimeInput(const std::string &field,
                                     const std::string &reason) {
  throw std::invalid_argument(field + ": " + reason);
}

std::string precise(double value) {
  std::ostringstream stream;
  stream << std::setprecision(17) << value;
  return stream.str();
}

std::string trim(const std::string &text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::vector<double> parseNumbers(const std::string &value,
                                 std::size_t expectedCount,
                                 const std::string &field) {
  std::istringstream stream(value);
  stream.imbue(std::locale::classic());
  std::vector<double> numbers;
  double number = 0.0;
  while (stream >> number) {
    if (!std::isfinite(number)) {
      reject(field, "must contain only finite numbers");
    }
    numbers.push_back(number);
  }
  if (!stream.eof() || numbers.size() != expectedCount) {
    reject(field, "must contain exactly " + std::to_string(expectedCount) +
                      " finite number(s)");
  }
  return numbers;
}

double scalar(const std::map<std::string, std::string> &assignments,
              const std::string &field) {
  return parseNumbers(assignments.at(field), 1, field).front();
}

Vector3 vector3(const std::map<std::string, std::string> &assignments,
                const std::string &field) {
  const std::vector<double> values =
      parseNumbers(assignments.at(field), 3, field);
  return {values[0], values[1], values[2]};
}

void requireExactVector(const Vector3 &actual, const Vector3 &expected,
                        const std::string &field) {
  if (actual.x() != expected.x() || actual.y() != expected.y() ||
      actual.z() != expected.z()) {
    reject(field, "must use the canonical camera axis");
  }
}

}  // namespace

TsaiPinholeCamera::TsaiPinholeCamera(
    double fu, double fv, double cu, double cv, Vector3 centerJ2000M,
    Matrix3 rotationCameraFromJ2000,
    std::optional<Radtan5Distortion> distortion,
    DistortionInverseControls inverseControls)
    : fu_(fu),
      fv_(fv),
      cu_(cu),
      cv_(cv),
      centerJ2000M_(std::move(centerJ2000M)),
      rotationCameraFromJ2000_(std::move(rotationCameraFromJ2000)),
      distortion_(std::move(distortion)),
      inverseControls_(inverseControls) {}

TsaiPinholeCamera TsaiPinholeCamera::readCanonical(
    const std::filesystem::path &path,
    DistortionInverseControls inverseControls) {
  std::ifstream stream(path);
  if (!stream) {
    reject("camera_path", "cannot open " + path.string());
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(stream, line)) {
    const std::string stripped = trim(line);
    if (!stripped.empty()) {
      lines.push_back(stripped);
    }
  }
  if (!stream.eof()) {
    reject("camera_path", "failed while reading " + path.string());
  }
  if (lines.empty() || lines.front() != "VERSION_4") {
    reject("VERSION_4", "canonical TSAI header is required");
  }
  if (lines.size() < 2 || lines[1] != "PINHOLE") {
    reject("PINHOLE", "canonical camera model is required");
  }
  std::optional<std::size_t> terminatorIndex;
  for (std::size_t index = 2; index < lines.size(); ++index) {
    if (lines[index] == "NULL" || lines[index] == "TSAI") {
      if (terminatorIndex.has_value()) {
        reject("NULL/TSAI", "canonical distortion terminator is duplicated");
      }
      terminatorIndex = index;
    }
  }
  if (!terminatorIndex.has_value()) {
    reject("NULL/TSAI", "canonical distortion terminator is required");
  }
  const bool hasTsai5 = lines[*terminatorIndex] == "TSAI";
  if ((!hasTsai5 && *terminatorIndex + 1 != lines.size()) ||
      (hasTsai5 && *terminatorIndex + 6 != lines.size())) {
    reject("NULL/TSAI",
           "terminal marker must be NULL or TSAI with exactly five parameters");
  }

  const std::vector<std::string> required{
      "fu",          "fv",          "cu", "cv", "u_direction",
      "v_direction", "w_direction", "C",  "R",  "pitch"};
  std::map<std::string, std::string> assignments;
  for (std::size_t index = 2; index < *terminatorIndex; ++index) {
    const std::string &assignment = lines[index];
    const std::size_t equals = assignment.find('=');
    if (equals == std::string::npos ||
        assignment.find('=', equals + 1) != std::string::npos) {
      reject("camera assignment", "expected exactly one '=' in " +
                                      assignment);
    }
    const std::string key = trim(assignment.substr(0, equals));
    const std::string value = trim(assignment.substr(equals + 1));
    if (std::find(required.begin(), required.end(), key) == required.end()) {
      reject(key.empty() ? "camera assignment" : key, "unknown field");
    }
    if (!assignments.emplace(key, value).second) {
      reject(key, "duplicate assignment");
    }
  }
  for (const std::string &key : required) {
    if (assignments.count(key) == 0) {
      reject(key, "required assignment is missing");
    }
  }

  const double fu = scalar(assignments, "fu");
  const double fv = scalar(assignments, "fv");
  const double cu = scalar(assignments, "cu");
  const double cv = scalar(assignments, "cv");
  if (!(fu > 0.0)) {
    reject("fu", "must be positive");
  }
  if (!(fv > 0.0)) {
    reject("fv", "must be positive");
  }
  static_cast<void>(cu);
  static_cast<void>(cv);
  requireExactVector(vector3(assignments, "u_direction"),
                     Vector3::UnitX(), "u_direction");
  requireExactVector(vector3(assignments, "v_direction"),
                     Vector3::UnitY(), "v_direction");
  requireExactVector(vector3(assignments, "w_direction"),
                     Vector3::UnitZ(), "w_direction");
  if (scalar(assignments, "pitch") != 1.0) {
    reject("pitch", "must equal one");
  }

  const std::vector<double> rotationValues =
      parseNumbers(assignments.at("R"), 9, "R");
  Matrix3 rotation;
  for (Eigen::Index row = 0; row < 3; ++row) {
    for (Eigen::Index column = 0; column < 3; ++column) {
      rotation(row, column) =
          rotationValues[static_cast<std::size_t>(3 * row + column)];
    }
  }
  if ((rotation * rotation.transpose() - Matrix3::Identity()).norm() >
          1.0e-12 ||
      std::abs(rotation.determinant() - 1.0) > 1.0e-12) {
    reject("R", "must be a proper orthonormal J2000-to-camera rotation");
  }

  std::optional<Radtan5Distortion> distortion;
  if (hasTsai5) {
    std::vector<double> coefficients;
    coefficients.reserve(5);
    for (std::size_t index = *terminatorIndex + 1; index < lines.size();
         ++index) {
      coefficients.push_back(parseNumbers(lines[index], 1, "TSAI").front());
    }
    distortion.emplace(PinholeIntrinsics{fu, fv, cu, cv},
                       Radtan5Coefficients{coefficients[0], coefficients[1],
                                           coefficients[2], coefficients[3],
                                           coefficients[4]});
  }

  return TsaiPinholeCamera(fu, fv, cu, cv, vector3(assignments, "C"),
                           rotation, std::move(distortion), inverseControls);
}

Vector3 TsaiPinholeCamera::rayCamera(double sample, double line) const {
  if (!std::isfinite(sample) || !std::isfinite(line)) {
    rejectRuntimeInput("sample,line", "must be finite");
  }
  Eigen::Vector2d idealPixel(sample, line);
  if (distortion_.has_value()) {
    ++rayStatistics_.requestedPixelCount;
    try {
      const DistortionInverseResult inverse =
          distortion_->undistortObservedPixel(
              {sample, line}, inverseControls_.maximumIterations,
              inverseControls_.roundTripTolerancePx);
      rayStatistics_.maximumInverseIterations = std::max(
          rayStatistics_.maximumInverseIterations, inverse.iterations);
      rayStatistics_.maximumRoundTripErrorPx = std::max(
          rayStatistics_.maximumRoundTripErrorPx, inverse.roundTripErrorPx);
      idealPixel = inverse.idealPixelPx;
    }
    catch (const std::exception &error) {
      ++rayStatistics_.inverseFailureCount;
      throw std::runtime_error(
          "DISTORTION_INVERSE_ERROR: samplePx=" + precise(sample) +
          " linePx=" + precise(line) + " configuredMaximumIterations=" +
          std::to_string(inverseControls_.maximumIterations) +
          " error=" + error.what());
    }
  }
  const Vector3 unnormalized((idealPixel.x() - cu_) / fu_,
                             (idealPixel.y() - cv_) / fv_, 1.0);
  return unnormalized.normalized();
}

CameraRayStatistics TsaiPinholeCamera::rayStatistics() const {
  return rayStatistics_;
}

Eigen::Vector2d TsaiPinholeCamera::projectJ2000(
    const Vector3 &pointJ2000M) const {
  const Vector3 pointCamera =
      rotationCameraFromJ2000_ * (pointJ2000M - centerJ2000M_);
  if (!(pointCamera.z() > 0.0) || !pointCamera.allFinite()) {
    throw std::domain_error(
        "depth: projected point must have finite positive depth");
  }
  const Eigen::Vector2d idealPixel(
      fu_ * pointCamera.x() / pointCamera.z() + cu_,
      fv_ * pointCamera.y() / pointCamera.z() + cv_);
  return distortion_.has_value() ? distortion_->distortIdealPixel(idealPixel)
                                 : idealPixel;
}

void TsaiPinholeCamera::requireMatches(
    const FrameState &frame, double centerToleranceM,
    double attitudeToleranceRad) const {
  if (!std::isfinite(centerToleranceM) || centerToleranceM < 0.0) {
    rejectRuntimeInput("centerToleranceM", "must be finite and non-negative");
  }
  if (!std::isfinite(attitudeToleranceRad) ||
      attitudeToleranceRad < 0.0) {
    rejectRuntimeInput("attitudeToleranceRad",
                       "must be finite and non-negative");
  }
  const double centerError =
      (centerJ2000M_ - frame.targetToCameraJ2000M).norm();
  if (!std::isfinite(centerError) || centerError > centerToleranceM) {
    rejectRuntimeInput("target_to_camera_j2000_m (schema v2) / "
                       "camera_center_j2000_m (schema v1)",
                       "disagrees with TSAI C by " + precise(centerError) +
                           " m");
  }

  const Matrix3 tsaiRotationJ2000FromCamera =
      rotationCameraFromJ2000_.transpose();
  const Matrix3 disagreement =
      frame.rotationJ2000FromCamera.transpose() *
      tsaiRotationJ2000FromCamera;
  const double cosine =
      std::clamp((disagreement.trace() - 1.0) / 2.0, -1.0, 1.0);
  const Vector3 skew(disagreement(2, 1) - disagreement(1, 2),
                     disagreement(0, 2) - disagreement(2, 0),
                     disagreement(1, 0) - disagreement(0, 1));
  const double sine = 0.5 * skew.norm();
  const double angle = std::atan2(sine, cosine);
  if (!std::isfinite(angle) || angle > attitudeToleranceRad) {
    rejectRuntimeInput("q_camera_to_j2000_wxyz",
                       "disagrees with transpose(TSAI R) by " +
                           precise(angle) + " rad");
  }
}

double TsaiPinholeCamera::fu() const {
  return fu_;
}

double TsaiPinholeCamera::fv() const {
  return fv_;
}

double TsaiPinholeCamera::cu() const {
  return cu_;
}

double TsaiPinholeCamera::cv() const {
  return cv_;
}

const Vector3 &TsaiPinholeCamera::centerJ2000M() const {
  return centerJ2000M_;
}

const Matrix3 &TsaiPinholeCamera::rotationCameraFromJ2000() const {
  return rotationCameraFromJ2000_;
}

CameraModelFormat TsaiPinholeCamera::cameraModelFormat() const {
  return distortion_.has_value() ? CameraModelFormat::AspTsai5
                                 : CameraModelFormat::AspTsaiNull;
}

}  // namespace Isis::SmallBodyRender
