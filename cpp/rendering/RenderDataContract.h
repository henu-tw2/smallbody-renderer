// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>

namespace Isis::SmallBodyRender {

using Vector3 = Eigen::Vector3d;
using Matrix3 = Eigen::Matrix3d;

struct QuaternionWxyz {
  double w;
  double x;
  double y;
  double z;
};

struct Photometry {
  double ambient;
  double diffuse;
};

enum class CameraModelFormat {
  AspTsaiNull,
  AspTsai5,
  OpenCvRadtan5Json
};

enum class IlluminationMode {
  CameraHeadlight,
  PointSunLambert
};

enum class SurfaceAppearanceMode {
  Uniform,
  PlateAlbedo
};

struct DistortionInverseControls {
  // Positive iteration count for inverse distortion.
  int maximumIterations{20};
  // Finite positive forward/inverse round-trip tolerance in pixels.
  double roundTripTolerancePx{1.0e-6};
};

struct IlluminationControls {
  IlluminationControls() = default;
  IlluminationControls(IlluminationMode illuminationMode,
                       double ambientWeight, double diffuseWeight,
                       bool shadows, double shadowOffsetM,
                       double shadowRangeM,
                       std::string source = {},
                       std::string sourceSha256 = {})
      : mode(illuminationMode),
        ambient(ambientWeight),
        diffuse(diffuseWeight),
        castShadows(shadows),
        shadowOriginOffsetM(shadowOffsetM),
        shadowMaximumRangeM(shadowRangeM),
        stateSource(std::move(source)),
        stateSha256(std::move(sourceSha256)) {}

  IlluminationMode mode{IlluminationMode::CameraHeadlight};
  // Finite dimensionless weights: both non-negative and their sum is one;
  // point-sun mode additionally requires ambient to be positive.
  double ambient{0.0};
  double diffuse{1.0};
  // Camera-headlight mode requires false.
  bool castShadows{false};
  // Finite positive metres in point-sun mode; both are zero while inactive
  // in camera-headlight mode.
  double shadowOriginOffsetM{0.0};
  double shadowMaximumRangeM{0.0};
  // Point-sun resolved provenance. Raw user configuration does not contain
  // these fields; Python preflight freezes the actual illumination CSV hash.
  std::string stateSource;
  std::string stateSha256;
};

struct SurfaceAppearanceControls {
  SurfaceAppearanceMode mode{SurfaceAppearanceMode::Uniform};
  // Finite positive, dimensionless linear relative albedo.
  double defaultAlbedo{1.0};
  // Plate mode requires an absolute path and a 64-character lowercase
  // hexadecimal SHA-256; both remain empty in uniform mode.
  std::filesystem::path plateAlbedoPath;
  std::string plateAlbedoSha256;
};

struct DskSpecification {
  std::filesystem::path path;
  std::string sha256;
  int expectedCenterId;
  int expectedFrameId;
  std::string expectedFrameName;
  std::vector<std::filesystem::path> frameKernelPaths;
};

struct FrameState {
  std::string frameId;
  // SPICE ephemeris time in ET/TDB seconds.
  double etTdbS;
  std::filesystem::path cameraPath;
  std::string cameraSha256;
  int widthPx;
  int heightPx;
  CameraModelFormat cameraModelFormat{CameraModelFormat::AspTsaiNull};
  // Vector from the contract's DSK target origin to the camera center,
  // expressed in axes parallel to J2000 and in metres. The origin must be the
  // same one used by the DSK coordinates; it is not implicitly a center of
  // mass, an SfM origin, or any other shape-center convention.
  Vector3 targetToCameraJ2000M{Vector3::Zero()};
  // Optional vector from that same target origin to the Sun, with the same
  // J2000-axis and metre conventions.
  std::optional<Vector3> targetToSunJ2000M;
  // Both quaternions are active rotations stored in wxyz order.
  QuaternionWxyz dskToJ2000Quaternion;
  QuaternionWxyz cameraToJ2000Quaternion;
  Matrix3 rotationJ2000FromDsk;
  Matrix3 rotationJ2000FromCamera;
};

struct ResolvedRenderConfig {
  // Version of the resolved JSON contract: exactly 1 or 2.
  int schemaVersion;
  DskSpecification dsk;
  double maximumRangeM;
  int tileWidthPx;
  int tileHeightPx;
  Photometry photometry;
  // Finite positive, dimensionless linear exposure multiplier.
  double exposureScale{1.0};
  DistortionInverseControls distortionInverse;
  IlluminationControls illumination;
  SurfaceAppearanceControls surfaceAppearance;
  FrameState frame;
};

Matrix3 activeRotationFromWxyz(const QuaternionWxyz &quaternion);

ResolvedRenderConfig loadResolvedRenderConfig(
    const std::filesystem::path &path, const std::string &frameId);

}  // namespace Isis::SmallBodyRender
