// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

// Modified by: gengxun + Codex
// Date: 2026-08-11
// Summary: Keep schema-1 ASP provenance while accepting the audited ASP-free schema-2 executable contract.

#include "RenderDataContract.h"

#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace Isis::SmallBodyRender {
namespace {

using nlohmann::json;

constexpr int MaximumTileEdgePx = 4096;
constexpr std::uint64_t MaximumTilePixels = 1048576;

[[noreturn]] void reject(const std::string &field,
                         const std::string &reason) {
  throw std::invalid_argument(field + ": " + reason);
}

const json &requireMember(const json &object, const std::string &key,
                          const std::string &context) {
  if (!object.is_object()) {
    reject(context, "must be an object");
  }
  if (!object.contains(key)) {
    reject(context.empty() ? key : context + "." + key,
           "required field is missing");
  }
  return object.at(key);
}

void requireExactKeys(const json &object,
                      std::initializer_list<const char *> allowed,
                      const std::string &context) {
  if (!object.is_object()) {
    reject(context, "must be an object");
  }
  std::set<std::string> allowedSet;
  for (const char *key : allowed) {
    allowedSet.emplace(key);
    if (!object.contains(key)) {
      reject(context.empty() ? key : context + "." + key,
             "required field is missing");
    }
  }
  for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
    if (allowedSet.count(iterator.key()) == 0) {
      reject(context.empty() ? iterator.key()
                             : context + "." + iterator.key(),
             "unknown field");
    }
  }
}

int requireInteger(const json &value, const std::string &field) {
  if (!value.is_number_integer()) {
    reject(field, "must be an integer");
  }
  const auto wide = value.get<std::int64_t>();
  if (wide < std::numeric_limits<int>::min() ||
      wide > std::numeric_limits<int>::max()) {
    reject(field, "is outside the supported integer range");
  }
  return static_cast<int>(wide);
}

void validateTileWorkset(int tileWidth, int tileHeight) {
  if (tileWidth <= 0 || tileWidth % 16 != 0) {
    reject("render.tile_width_px", "must be a positive multiple of 16");
  }
  if (tileHeight <= 0 || tileHeight % 16 != 0) {
    reject("render.tile_height_px", "must be a positive multiple of 16");
  }
  if (tileWidth > MaximumTileEdgePx) {
    reject("render.tile_width_px", "must be <= 4096");
  }
  if (tileHeight > MaximumTileEdgePx) {
    reject("render.tile_height_px", "must be <= 4096");
  }
  const auto width = static_cast<std::uint64_t>(tileWidth);
  const auto height = static_cast<std::uint64_t>(tileHeight);
  if (height > MaximumTilePixels / width) {
    reject("render.tile_pixels",
           "tile_width_px * tile_height_px must be <= 1048576");
  }
}

double requireFiniteNumber(const json &value, const std::string &field) {
  if (!value.is_number()) {
    reject(field, "must be a number");
  }
  const double number = value.get<double>();
  if (!std::isfinite(number)) {
    reject(field, "must be finite");
  }
  return number;
}

std::string requireString(const json &value, const std::string &field) {
  if (!value.is_string()) {
    reject(field, "must be a string");
  }
  return value.get<std::string>();
}

bool requireBoolean(const json &value, const std::string &field) {
  if (!value.is_boolean()) {
    reject(field, "must be a boolean");
  }
  return value.get<bool>();
}

std::filesystem::path requireAbsolutePath(const json &value,
                                          const std::string &field) {
  const std::filesystem::path path(requireString(value, field));
  if (!path.is_absolute()) {
    reject(field, "must be an absolute path");
  }
  return path;
}

std::string requireSha256(const json &value, const std::string &field) {
  const std::string hash = requireString(value, field);
  static const std::regex sha256Pattern("^[0-9a-f]{64}$");
  if (!std::regex_match(hash, sha256Pattern)) {
    reject(field, "must contain 64 lowercase hexadecimal characters");
  }
  return hash;
}

std::string requireSafeFrameId(const json &value, const std::string &field) {
  const std::string id = requireString(value, field);
  static const std::regex safePattern("^[A-Za-z0-9][A-Za-z0-9._-]*$");
  if (!std::regex_match(id, safePattern) || id == "." || id == "..") {
    reject(field, "must be a safe portable identifier");
  }
  return id;
}

Vector3 requireVector3(const json &value, const std::string &field) {
  if (!value.is_array() || value.size() != 3) {
    reject(field, "must be an array of three numbers");
  }
  Vector3 result;
  for (std::size_t index = 0; index < 3; ++index) {
    result[static_cast<Eigen::Index>(index)] =
        requireFiniteNumber(value.at(index),
                            field + "[" + std::to_string(index) + "]");
  }
  return result;
}

QuaternionWxyz requireQuaternion(const json &value,
                                 const std::string &field) {
  if (!value.is_array() || value.size() != 4) {
    reject(field, "must be a wxyz array of four numbers");
  }
  QuaternionWxyz quaternion{
      requireFiniteNumber(value.at(0), field + "[0]"),
      requireFiniteNumber(value.at(1), field + "[1]"),
      requireFiniteNumber(value.at(2), field + "[2]"),
      requireFiniteNumber(value.at(3), field + "[3]")};
  try {
    static_cast<void>(activeRotationFromWxyz(quaternion));
  }
  catch (const std::exception &error) {
    reject(field, error.what());
  }
  return quaternion;
}

Photometry parsePhotometry(const json &value, const std::string &context) {
  requireExactKeys(value, {"ambient", "diffuse"}, context);
  const Photometry photometry{
      requireFiniteNumber(value.at("ambient"), context + ".ambient"),
      requireFiniteNumber(value.at("diffuse"), context + ".diffuse")};
  if (photometry.ambient < 0.0) {
    reject(context + ".ambient", "must be non-negative");
  }
  if (photometry.diffuse < 0.0) {
    reject(context + ".diffuse", "must be non-negative");
  }
  if (std::abs(photometry.ambient + photometry.diffuse - 1.0) > 1.0e-12) {
    reject(context, "ambient + diffuse must equal one");
  }
  return photometry;
}

CameraModelFormat parseCameraModelFormat(const json &value,
                                         const std::string &field) {
  const std::string format = requireString(value, field);
  if (format == "asp_tsai_null") {
    return CameraModelFormat::AspTsaiNull;
  }
  if (format == "asp_tsai5") {
    return CameraModelFormat::AspTsai5;
  }
  if (format == "opencv_radtan5_json") {
    return CameraModelFormat::OpenCvRadtan5Json;
  }
  reject(field, "unknown value '" + format + "'");
}

IlluminationMode parseIlluminationMode(const json &value,
                                       const std::string &field) {
  const std::string mode = requireString(value, field);
  if (mode == "camera_headlight") {
    return IlluminationMode::CameraHeadlight;
  }
  if (mode == "point_sun_lambert") {
    return IlluminationMode::PointSunLambert;
  }
  reject(field, "unknown value '" + mode + "'");
}

SurfaceAppearanceMode parseSurfaceAppearanceMode(
    const json &value, const std::string &field) {
  const std::string mode = requireString(value, field);
  if (mode == "uniform") {
    return SurfaceAppearanceMode::Uniform;
  }
  if (mode == "plate_albedo") {
    return SurfaceAppearanceMode::PlateAlbedo;
  }
  reject(field, "unknown value '" + mode + "'");
}

DistortionInverseControls parseDistortionInverseControls(
    const json &value) {
  const std::string context = "render.camera_distortion";
  requireExactKeys(value,
                   {"inverse_max_iterations", "roundtrip_tolerance_px"},
                   context);
  const int maximumIterations =
      requireInteger(value.at("inverse_max_iterations"),
                     context + ".inverse_max_iterations");
  if (maximumIterations <= 0) {
    reject(context + ".inverse_max_iterations", "must be positive");
  }
  const double roundTripTolerancePx =
      requireFiniteNumber(value.at("roundtrip_tolerance_px"),
                          context + ".roundtrip_tolerance_px");
  if (!(roundTripTolerancePx > 0.0)) {
    reject(context + ".roundtrip_tolerance_px", "must be positive");
  }
  return {maximumIterations, roundTripTolerancePx};
}

IlluminationControls parseIlluminationControls(const json &value) {
  const std::string context = "render.illumination";
  try {
    const IlluminationMode mode =
        parseIlluminationMode(requireMember(value, "mode", context),
                              context + ".mode");
    if (mode == IlluminationMode::CameraHeadlight) {
      requireExactKeys(value, {"mode", "ambient", "diffuse", "cast_shadows"},
                       context);
    }
    else {
      requireExactKeys(value,
                       {"mode", "ambient", "diffuse", "cast_shadows",
                        "shadow_origin_offset_m", "shadow_maximum_range_m",
                        "state_source", "state_sha256"},
                       context);
    }
    const Photometry photometry = parsePhotometry(
        {{"ambient", value.at("ambient")},
         {"diffuse", value.at("diffuse")}},
        context);
    const bool castShadows =
        requireBoolean(value.at("cast_shadows"), context + ".cast_shadows");
    if (mode == IlluminationMode::CameraHeadlight && castShadows) {
      reject(context + ".cast_shadows",
             "camera_headlight does not support shadows");
    }
    if (mode == IlluminationMode::CameraHeadlight) {
      return {mode, photometry.ambient, photometry.diffuse, castShadows, 0.0,
              0.0, {}, {}};
    }
    if (!(photometry.ambient > 0.0)) {
      reject(context + ".ambient",
             "must be positive for point_sun_lambert");
    }
    double shadowOriginOffsetM = 0.0;
    double shadowMaximumRangeM = 0.0;
    try {
      shadowOriginOffsetM =
          requireFiniteNumber(value.at("shadow_origin_offset_m"),
                              context + ".shadow_origin_offset_m");
      if (!(shadowOriginOffsetM > 0.0)) {
        reject(context + ".shadow_origin_offset_m", "must be positive");
      }
      shadowMaximumRangeM =
          requireFiniteNumber(value.at("shadow_maximum_range_m"),
                              context + ".shadow_maximum_range_m");
      if (!(shadowMaximumRangeM > 0.0)) {
        reject(context + ".shadow_maximum_range_m", "must be positive");
      }
    }
    catch (const std::exception &error) {
      throw std::invalid_argument(
          "SHADOW_CONFIGURATION_ERROR: " + std::string(error.what()));
    }
    const std::string stateSource =
        requireString(value.at("state_source"), context + ".state_source");
    if (stateSource != "illumination_csv") {
      reject(context + ".state_source",
             "must equal 'illumination_csv'");
    }
    const std::string stateSha256 =
        requireSha256(value.at("state_sha256"),
                      context + ".state_sha256");
    return {mode, photometry.ambient, photometry.diffuse, castShadows,
            shadowOriginOffsetM, shadowMaximumRangeM, stateSource,
            stateSha256};
  }
  catch (const std::exception &error) {
    const std::string message(error.what());
    if (message.rfind("SHADOW_CONFIGURATION_ERROR:", 0) == 0) {
      throw;
    }
    throw std::invalid_argument("ILLUMINATION_CONTRACT_ERROR: " + message);
  }
}

SurfaceAppearanceControls parseSurfaceAppearanceControls(
    const json &value) {
  const std::string context = "render.surface_appearance";
  const SurfaceAppearanceMode mode =
      parseSurfaceAppearanceMode(requireMember(value, "mode", context),
                                 context + ".mode");
  if (mode == SurfaceAppearanceMode::Uniform) {
    requireExactKeys(value, {"mode", "default_albedo"}, context);
  }
  else {
    requireExactKeys(value,
                     {"mode", "default_albedo", "plate_albedo_path",
                      "plate_albedo_sha256"},
                     context);
  }
  const double defaultAlbedo =
      requireFiniteNumber(value.at("default_albedo"),
                          context + ".default_albedo");
  if (!(defaultAlbedo > 0.0)) {
    reject(context + ".default_albedo", "must be positive");
  }
  if (mode == SurfaceAppearanceMode::Uniform) {
    return {mode, defaultAlbedo, {}, {}};
  }
  return {mode,
          defaultAlbedo,
          requireAbsolutePath(value.at("plate_albedo_path"),
                              context + ".plate_albedo_path"),
          requireSha256(value.at("plate_albedo_sha256"),
                        context + ".plate_albedo_sha256")};
}

void validateAuxiliarySections(const json &root, int schemaVersion) {
  const json &stretch = root.at("isis_stretch");
  requireExactKeys(stretch, {"minpercent", "maxpercent"}, "isis_stretch");
  const double minPercent =
      requireFiniteNumber(stretch.at("minpercent"),
                          "isis_stretch.minpercent");
  const double maxPercent =
      requireFiniteNumber(stretch.at("maxpercent"),
                          "isis_stretch.maxpercent");
  if (minPercent < 0.0 || maxPercent > 100.0 ||
      !(minPercent < maxPercent)) {
    reject("isis_stretch", "must satisfy 0 <= minpercent < maxpercent <= 100");
  }

  const json &batch = root.at("batch");
  requireExactKeys(batch, {"jobs"}, "batch");
  if (requireInteger(batch.at("jobs"), "batch.jobs") <= 0) {
    reject("batch.jobs", "must be positive");
  }

  const json &executables = root.at("executables");
  if (schemaVersion == 1) {
    requireExactKeys(executables,
                     {"native", "std2isis", "isis2std", "stats", "tiffinfo",
                      "gdalinfo", "asp_version", "parallel"},
                     "executables");
  }
  else {
    requireExactKeys(executables,
                     {"native", "std2isis", "isis2std", "stats", "tiffinfo",
                      "gdalinfo", "parallel"},
                     "executables");
  }
  for (const char *key : {"native", "std2isis", "isis2std", "stats",
                          "tiffinfo", "gdalinfo", "parallel"}) {
    static_cast<void>(requireAbsolutePath(
        executables.at(key), "executables." + std::string(key)));
  }
  if (schemaVersion == 1) {
    static_cast<void>(requireAbsolutePath(executables.at("asp_version"),
                                          "executables.asp_version"));
  }
}

DskSpecification parseDsk(const json &value) {
  requireExactKeys(value,
                   {"path", "sha256", "expected_center_id",
                    "expected_frame_id", "expected_frame_name",
                    "frame_kernel_paths"},
                   "dsk");
  const std::string frameName =
      requireString(value.at("expected_frame_name"),
                    "dsk.expected_frame_name");
  if (frameName.empty()) {
    reject("dsk.expected_frame_name", "must not be empty");
  }
  const json &kernels = value.at("frame_kernel_paths");
  if (!kernels.is_array()) {
    reject("dsk.frame_kernel_paths", "must be an array");
  }
  std::vector<std::filesystem::path> kernelPaths;
  kernelPaths.reserve(kernels.size());
  for (std::size_t index = 0; index < kernels.size(); ++index) {
    kernelPaths.push_back(requireAbsolutePath(
        kernels.at(index),
        "dsk.frame_kernel_paths[" + std::to_string(index) + "]"));
  }
  return {requireAbsolutePath(value.at("path"), "dsk.path"),
          requireSha256(value.at("sha256"), "dsk.sha256"),
          requireInteger(value.at("expected_center_id"),
                         "dsk.expected_center_id"),
          requireInteger(value.at("expected_frame_id"),
                         "dsk.expected_frame_id"),
          frameName,
          std::move(kernelPaths)};
}

FrameState parseFrame(const json &value, std::size_t index,
                      int schemaVersion,
                      IlluminationMode illuminationMode) {
  const std::string prefix = "frames[" + std::to_string(index) + "]";
  if (schemaVersion == 1) {
    requireExactKeys(value,
                     {"frame_id", "et_tdb_s", "camera_path", "camera_sha256",
                      "width_px", "height_px", "camera_center_j2000_m",
                      "q_dsk_to_j2000_wxyz",
                      "q_camera_to_j2000_wxyz"},
                     prefix);
  }
  else {
    if (illuminationMode == IlluminationMode::PointSunLambert) {
      try {
        requireExactKeys(
            value,
            {"frame_id", "et_tdb_s", "camera_path", "camera_sha256",
             "width_px", "height_px", "camera_model_format",
             "target_to_camera_j2000_m", "target_to_sun_j2000_m",
             "q_dsk_to_j2000_wxyz", "q_camera_to_j2000_wxyz"},
            prefix);
      }
      catch (const std::exception &error) {
        throw std::invalid_argument(
            "ILLUMINATION_CONTRACT_ERROR: " +
            std::string(error.what()));
      }
    }
    else {
      if (value.is_object() &&
          value.contains("target_to_sun_j2000_m")) {
        throw std::invalid_argument(
            "ILLUMINATION_CONTRACT_ERROR: " + prefix +
            ".target_to_sun_j2000_m: unknown field for camera_headlight");
      }
      requireExactKeys(
          value,
          {"frame_id", "et_tdb_s", "camera_path", "camera_sha256",
           "width_px", "height_px", "camera_model_format",
           "target_to_camera_j2000_m", "q_dsk_to_j2000_wxyz",
           "q_camera_to_j2000_wxyz"},
          prefix);
    }
  }
  const int width = requireInteger(value.at("width_px"), prefix + ".width_px");
  const int height =
      requireInteger(value.at("height_px"), prefix + ".height_px");
  if (width <= 0) {
    reject(prefix + ".width_px", "must be positive");
  }
  if (height <= 0) {
    reject(prefix + ".height_px", "must be positive");
  }
  const QuaternionWxyz dskQuaternion =
      requireQuaternion(value.at("q_dsk_to_j2000_wxyz"),
                        prefix + ".q_dsk_to_j2000_wxyz");
  const QuaternionWxyz cameraQuaternion =
      requireQuaternion(value.at("q_camera_to_j2000_wxyz"),
                        prefix + ".q_camera_to_j2000_wxyz");
  const CameraModelFormat cameraModelFormat =
      schemaVersion == 1
          ? CameraModelFormat::AspTsaiNull
          : parseCameraModelFormat(value.at("camera_model_format"),
                                   prefix + ".camera_model_format");
  const char *cameraVectorField =
      schemaVersion == 1 ? "camera_center_j2000_m"
                         : "target_to_camera_j2000_m";
  std::optional<Vector3> targetToSunJ2000M;
  if (schemaVersion == 2 &&
      illuminationMode == IlluminationMode::PointSunLambert) {
    try {
      targetToSunJ2000M =
          requireVector3(value.at("target_to_sun_j2000_m"),
                         prefix + ".target_to_sun_j2000_m");
    }
    catch (const std::exception &error) {
      throw std::invalid_argument(
          "ILLUMINATION_CONTRACT_ERROR: " + std::string(error.what()));
    }
  }
  return {
      requireSafeFrameId(value.at("frame_id"), prefix + ".frame_id"),
      requireFiniteNumber(value.at("et_tdb_s"), prefix + ".et_tdb_s"),
      requireAbsolutePath(value.at("camera_path"), prefix + ".camera_path"),
      requireSha256(value.at("camera_sha256"), prefix + ".camera_sha256"),
      width,
      height,
      cameraModelFormat,
      requireVector3(value.at(cameraVectorField),
                     prefix + "." + cameraVectorField),
      targetToSunJ2000M,
      dskQuaternion,
      cameraQuaternion,
      activeRotationFromWxyz(dskQuaternion),
      activeRotationFromWxyz(cameraQuaternion)};
}

}  // namespace

Matrix3 activeRotationFromWxyz(const QuaternionWxyz &quaternion) {
  const double norm =
      std::hypot(std::hypot(quaternion.w, quaternion.x),
                 std::hypot(quaternion.y, quaternion.z));
  if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1.0e-8) {
    throw std::invalid_argument(
        "quaternion norm must be within 1e-8 of unity");
  }
  const Eigen::Quaterniond normalized(
      quaternion.w / norm, quaternion.x / norm, quaternion.y / norm,
      quaternion.z / norm);
  const Matrix3 rotation = normalized.toRotationMatrix();
  if ((rotation * rotation.transpose() - Matrix3::Identity()).norm() >
          1.0e-10 ||
      std::abs(rotation.determinant() - 1.0) > 1.0e-10) {
    throw std::invalid_argument("quaternion must produce a proper rotation");
  }
  return rotation;
}

ResolvedRenderConfig loadResolvedRenderConfig(
    const std::filesystem::path &path, const std::string &frameId) {
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("resolved_config.path: cannot open " +
                             path.string());
  }
  json root;
  try {
    stream >> root;
  }
  catch (const json::exception &error) {
    throw std::invalid_argument("resolved_config: invalid JSON: " +
                                std::string(error.what()));
  }
  requireExactKeys(root,
                   {"schema_version", "dsk", "render", "isis_stretch",
                    "batch", "executables", "frames"},
                   "");
  const int schemaVersion =
      requireInteger(root.at("schema_version"), "schema_version");
  if (schemaVersion != 1 && schemaVersion != 2) {
    reject("schema_version", "unsupported value " +
                                 std::to_string(schemaVersion));
  }

  const DskSpecification dsk = parseDsk(root.at("dsk"));
  const json &render = root.at("render");
  if (schemaVersion == 1) {
    requireExactKeys(render,
                     {"maximum_range_m", "tile_width_px", "tile_height_px",
                      "photometry"},
                     "render");
  }
  else {
    requireExactKeys(render,
                     {"maximum_range_m", "tile_width_px", "tile_height_px",
                      "exposure_scale", "illumination",
                      "camera_distortion", "surface_appearance"},
                     "render");
  }
  const double maximumRange =
      requireFiniteNumber(render.at("maximum_range_m"),
                          "render.maximum_range_m");
  if (!(maximumRange > 0.0)) {
    reject("render.maximum_range_m", "must be positive");
  }
  const int tileWidth =
      requireInteger(render.at("tile_width_px"), "render.tile_width_px");
  const int tileHeight =
      requireInteger(render.at("tile_height_px"), "render.tile_height_px");
  validateTileWorkset(tileWidth, tileHeight);

  Photometry photometry{};
  double exposureScale = 1.0;
  DistortionInverseControls distortionInverse{20, 1.0e-6};
  IlluminationControls illumination{};
  SurfaceAppearanceControls surfaceAppearance{};
  if (schemaVersion == 1) {
    photometry = parsePhotometry(render.at("photometry"),
                                 "render.photometry");
    illumination = {IlluminationMode::CameraHeadlight,
                    photometry.ambient,
                    photometry.diffuse,
                    false,
                    0.0,
                    0.0,
                    {},
                    {}};
    surfaceAppearance = {SurfaceAppearanceMode::Uniform, 1.0, {}, {}};
  }
  else {
    exposureScale =
        requireFiniteNumber(render.at("exposure_scale"),
                            "render.exposure_scale");
    if (!(exposureScale > 0.0)) {
      reject("render.exposure_scale", "must be positive");
    }
    distortionInverse =
        parseDistortionInverseControls(render.at("camera_distortion"));
    illumination =
        parseIlluminationControls(render.at("illumination"));
    surfaceAppearance =
        parseSurfaceAppearanceControls(render.at("surface_appearance"));
    if (illumination.mode == IlluminationMode::CameraHeadlight &&
        exposureScale != 1.0) {
      reject("render.exposure_scale",
             "recognized non-identity value is not implemented; current "
             "shared contract requires 1.0");
    }
    if (illumination.mode == IlluminationMode::CameraHeadlight &&
        surfaceAppearance.defaultAlbedo != 1.0) {
      reject("render.surface_appearance.default_albedo",
             "recognized non-identity value is not implemented; current "
             "shared contract requires 1.0");
    }
    photometry = {illumination.ambient, illumination.diffuse};
  }

  validateAuxiliarySections(root, schemaVersion);

  const json &frames = root.at("frames");
  if (!frames.is_array() || frames.empty()) {
    reject("frames", "must be a non-empty array");
  }
  std::set<std::string> seenFrameIds;
  std::vector<FrameState> parsedFrames;
  parsedFrames.reserve(frames.size());
  for (std::size_t index = 0; index < frames.size(); ++index) {
    FrameState frame = parseFrame(
        frames.at(index), index, schemaVersion, illumination.mode);
    if (!seenFrameIds.insert(frame.frameId).second) {
      reject("frames[" + std::to_string(index) + "].frame_id",
             "duplicate value " + frame.frameId);
    }
    parsedFrames.push_back(std::move(frame));
  }

  const FrameState *selected = nullptr;
  for (const FrameState &frame : parsedFrames) {
    if (frame.frameId == frameId) {
      selected = &frame;
      break;
    }
  }
  if (selected == nullptr) {
    reject("frame_id", "requested frame was not found: " + frameId);
  }

  return {schemaVersion, dsk, maximumRange, tileWidth, tileHeight,
          photometry, exposureScale, distortionInverse, illumination,
          surfaceAppearance, *selected};
}

}  // namespace Isis::SmallBodyRender
