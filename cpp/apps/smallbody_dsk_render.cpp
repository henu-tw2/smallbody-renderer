// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include <sys/resource.h>

#include <chrono>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <SpiceUsr.h>
#include <bullet/LinearMath/btScalar.h>
#include <nlohmann/json.hpp>
#include <tiffio.h>

#include <QByteArray>
#include <QCryptographicHash>

#include "BulletDskRayTracer.h"
#include "DskFrameRenderer.h"
#include "Environment.h"
#include "IException.h"
#include "PlateAlbedoTable.h"
#include "RenderDataContract.h"
#include "RenderCameraFactory.h"
#include "SurfaceAppearance.h"
#include "TiffProductWriter.h"

namespace {

using Isis::SmallBodyRender::BulletDskRayTracer;
using Isis::SmallBodyRender::CameraModelFormat;
using Isis::SmallBodyRender::CameraRayStatistics;
using Isis::SmallBodyRender::DskFrameRenderer;
using Isis::SmallBodyRender::IlluminationMode;
using Isis::SmallBodyRender::Matrix3;
using Isis::SmallBodyRender::PlateAlbedoStatistics;
using Isis::SmallBodyRender::PlateAlbedoTable;
using Isis::SmallBodyRender::ProductPaths;
using Isis::SmallBodyRender::QuaternionWxyz;
using Isis::SmallBodyRender::RenderSummary;
using Isis::SmallBodyRender::RenderCamera;
using Isis::SmallBodyRender::ResolvedRenderConfig;
using Isis::SmallBodyRender::SurfaceAppearance;
using Isis::SmallBodyRender::SurfaceAppearanceMode;
using Isis::SmallBodyRender::TiffProductWriter;
using Isis::SmallBodyRender::UniformSurfaceAppearance;
using Isis::SmallBodyRender::Vector3;
using Isis::SmallBodyRender::loadRenderCamera;
using nlohmann::json;

class CliError : public std::runtime_error {
  public:
    CliError(std::string category, std::string message)
        : std::runtime_error(std::move(message)),
          category_(std::move(category)) {}

    const std::string &category() const {
      return category_;
    }

  private:
    std::string category_;
};

struct Options {
  std::filesystem::path resolvedConfig;
  std::string frameId;
  std::filesystem::path outputDirectory;
};

Options parseArguments(int argc, char *argv[]) {
  if (argc != 7) {
    throw CliError(
        "ARGUMENT_ERROR",
        "expected --resolved-config PATH --frame-id ID --output-dir PATH");
  }
  std::map<std::string, std::string> values;
  for (int index = 1; index < argc; index += 2) {
    const std::string key(argv[index]);
    if (key != "--resolved-config" && key != "--frame-id" &&
        key != "--output-dir") {
      throw CliError("ARGUMENT_ERROR", "unknown option: " + key);
    }
    if (!values.emplace(key, argv[index + 1]).second) {
      throw CliError("ARGUMENT_ERROR", "duplicate option: " + key);
    }
  }
  for (const char *required :
       {"--resolved-config", "--frame-id", "--output-dir"}) {
    if (values.count(required) == 0 || values.at(required).empty()) {
      throw CliError("ARGUMENT_ERROR",
                     std::string("missing value for ") + required);
    }
  }
  return {values.at("--resolved-config"), values.at("--frame-id"),
          values.at("--output-dir")};
}

json failurePayload(const std::string &category,
                    const std::string &message, int schemaVersion) {
  return {{"schema_version", schemaVersion},
          {"status", "failed"},
          {"category", category},
          {"message", message}};
}

void writeJsonAtomically(const std::filesystem::path &path,
                         const json &payload) {
  const std::filesystem::path temporary =
      path.parent_path() / (path.filename().string() + ".tmp");
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("cannot create JSON file " +
                               temporary.string());
    }
    stream << payload.dump(2) << '\n';
    if (!stream) {
      throw std::runtime_error("cannot write JSON file " +
                               temporary.string());
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    throw std::runtime_error("cannot publish JSON file " + path.string() +
                             ": " + error.message());
  }
}

void createAttemptDirectory(const std::filesystem::path &path) {
  if (!path.is_absolute()) {
    throw CliError("OUTPUT_DIRECTORY_ERROR",
                   "--output-dir must be absolute");
  }
  std::error_code error;
  if (std::filesystem::exists(path, error)) {
    throw CliError("OUTPUT_DIRECTORY_EXISTS",
                   "output directory already exists: " + path.string());
  }
  if (error) {
    throw CliError("OUTPUT_DIRECTORY_ERROR",
                   "cannot inspect output directory: " + error.message());
  }
  if (!std::filesystem::is_directory(path.parent_path())) {
    throw CliError("OUTPUT_DIRECTORY_ERROR",
                   "output parent directory does not exist: " +
                       path.parent_path().string());
  }
  if (!std::filesystem::create_directory(path, error) || error) {
    throw CliError("OUTPUT_DIRECTORY_ERROR",
                   "cannot exclusively create output directory: " +
                       error.message());
  }
}

std::vector<double> flatten(const Matrix3 &matrix) {
  std::vector<double> result;
  result.reserve(9);
  for (Eigen::Index row = 0; row < 3; ++row) {
    for (Eigen::Index column = 0; column < 3; ++column) {
      result.push_back(matrix(row, column));
    }
  }
  return result;
}

std::vector<double> values(const QuaternionWxyz &quaternion) {
  return {quaternion.w, quaternion.x, quaternion.y, quaternion.z};
}

long long peakRssKib() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    throw std::runtime_error("getrusage failed");
  }
  return static_cast<long long>(usage.ru_maxrss);
}

json distortionReport(CameraModelFormat format,
                      const CameraRayStatistics &statistics) {
  const char *model = "none";
  const char *mapping = "none";
  switch (format) {
    case CameraModelFormat::AspTsaiNull:
      break;
    case CameraModelFormat::AspTsai5:
      model = "asp_tsai5";
      mapping = "ideal_to_observed";
      break;
    case CameraModelFormat::OpenCvRadtan5Json:
      model = "opencv_radtan5";
      mapping = "ideal_to_observed";
      break;
  }
  return {{"model", model},
          {"mapping", mapping},
          {"inverse",
           {{"requested_pixel_count", statistics.requestedPixelCount},
            {"failed_pixel_count", statistics.inverseFailureCount},
            {"maximum_iterations", statistics.maximumInverseIterations},
            {"maximum_roundtrip_error_px",
             statistics.maximumRoundTripErrorPx}}}};
}

json successReport(const ResolvedRenderConfig &config,
                   const BulletDskRayTracer &tracer,
                   const RenderSummary &summary, double elapsedSeconds,
                   long long peakRss,
                   const json &rawProductSha256,
                   const std::optional<PlateAlbedoStatistics> &
                       plateAlbedoStatistics,
                   const CameraRayStatistics &cameraStatistics) {
  const auto &frame = config.frame;
  const auto &dsk = tracer.metadata();
  json coordinateContract = {
      {"origin", "DSK shape center"},
      {"j2000_axes", "target-centered axes parallel to J2000"},
      {"dsk_rotation",
       "R_JD: DSK body-fixed to target-centered J2000"},
      {"camera_rotation",
       "R_JC: camera to target-centered J2000"},
      {"quaternion_order", "wxyz"},
      {"rotation_convention", "active"},
      {"time_scale", "ET/TDB"},
      {"length_unit", "m"}};
  if (config.schemaVersion == 2) {
    coordinateContract["camera_vector_origin"] = "DSK shape center";
    coordinateContract["camera_vector_direction"] =
        "DSK shape center to camera";
    coordinateContract["camera_vector_axes"] = "J2000";
  }
  json frameReport = {
      {"frame_id", frame.frameId},
      {"et_tdb_s", frame.etTdbS},
      {"width_px", frame.widthPx},
      {"height_px", frame.heightPx},
      {"q_dsk_to_j2000_wxyz", values(frame.dskToJ2000Quaternion)},
      {"q_camera_to_j2000_wxyz", values(frame.cameraToJ2000Quaternion)},
      {"rotation_j2000_from_dsk", flatten(frame.rotationJ2000FromDsk)},
      {"rotation_j2000_from_camera",
       flatten(frame.rotationJ2000FromCamera)}};
  const json cameraVector = {
      frame.targetToCameraJ2000M.x(), frame.targetToCameraJ2000M.y(),
      frame.targetToCameraJ2000M.z()};
  if (config.schemaVersion == 1) {
    frameReport["camera_center_j2000_m"] = cameraVector;
  }
  else {
    frameReport["target_to_camera_j2000_m"] = cameraVector;
  }
  json photometryReport = {
      {"model", "Lambert camera-coincident headlight"},
      {"ambient", config.photometry.ambient},
      {"diffuse", config.photometry.diffuse},
      {"cast_shadows", false}};
  if (config.illumination.mode == IlluminationMode::PointSunLambert) {
    if (!frame.targetToSunJ2000M.has_value()) {
      throw std::runtime_error(
          "point_sun_lambert report requires target_to_sun_j2000_m");
    }
    const auto &sun = *frame.targetToSunJ2000M;
    const Vector3 sunDskM =
        frame.rotationJ2000FromDsk.transpose() * sun;
    const double sunDistanceM = std::hypot(sun.x(), sun.y(), sun.z());
    if (!std::isfinite(sunDistanceM) || !(sunDistanceM > 0.0)) {
      throw std::runtime_error(
          "point_sun_lambert sun_distance_m must be finite and positive");
    }
    photometryReport = {
        {"model", "point_sun_lambert"},
        {"ambient", config.photometry.ambient},
        {"diffuse", config.photometry.diffuse},
        {"target_to_sun_j2000_m", {sun.x(), sun.y(), sun.z()}},
        {"target_to_sun_dsk_m",
         {sunDskM.x(), sunDskM.y(), sunDskM.z()}},
        {"sun_distance_m", sunDistanceM},
        {"aberration_correction", "NONE"},
        {"state_source", config.illumination.stateSource},
        {"input_sha256",
         {{"dsk", config.dsk.sha256},
          {"camera", frame.cameraSha256},
          {"illumination_csv", config.illumination.stateSha256}}},
        {"cast_shadows", config.illumination.castShadows},
        {"shadow_origin_offset_m",
         config.illumination.shadowOriginOffsetM},
        {"shadow_maximum_range_m",
         config.illumination.shadowMaximumRangeM},
        {"direct_pixels", summary.directPixels},
        {"night_pixels", summary.nightPixels},
        {"self_shadow_pixels", summary.selfShadowPixels}};
  }
  json report = {
      {"schema_version", config.schemaVersion},
      {"status", "passed"},
      {"coordinate_contract", coordinateContract},
      {"frame", frameReport},
      {"dsk",
       {{"center_id", dsk.centerId},
        {"surface_id", dsk.surfaceId},
        {"frame_id", dsk.frameId},
        {"frame_name", dsk.frameName},
        {"vertex_count", dsk.vertexCount},
        {"plate_count", dsk.plateCount}}},
      {"summary",
       {{"total_pixels", summary.totalPixels},
        {"valid_pixels", summary.validPixels},
        {"minimum_valid_dn", summary.minimumValidDn},
        {"maximum_valid_dn", summary.maximumValidDn},
        {"minimum_depth_m", summary.minimumDepthM},
        {"maximum_depth_m", summary.maximumDepthM},
        {"distinct_plate_count", summary.distinctPlateCount}}},
      {"photometry", photometryReport},
      {"versions",
       {{"app", "smallbody_dsk_render/1"},
        {"isis", Isis::Environment::isisVersion().toStdString()},
        {"bullet", std::to_string(BT_BULLET_VERSION)},
        {"cspice", tkvrsn_c("TOOLKIT")},
        {"libtiff", TIFFGetVersion()}}},
      {"resources",
       {{"elapsed_seconds", elapsedSeconds}, {"peak_rss_kib", peakRss}}}};
  if (!rawProductSha256.empty()) {
    report["raw_product_sha256"] = rawProductSha256;
  }
  if (config.schemaVersion == 2) {
    json surfaceAppearance = {
        {"mode", config.surfaceAppearance.mode ==
                     SurfaceAppearanceMode::PlateAlbedo
                     ? "plate_albedo"
                     : "uniform"},
        {"default_albedo", config.surfaceAppearance.defaultAlbedo}};
    if (config.surfaceAppearance.mode ==
        SurfaceAppearanceMode::PlateAlbedo) {
      if (!plateAlbedoStatistics.has_value()) {
        throw std::runtime_error(
            "plate_albedo report requires table statistics");
      }
      const PlateAlbedoStatistics &statistics = *plateAlbedoStatistics;
      surfaceAppearance["input_sha256"] =
          config.surfaceAppearance.plateAlbedoSha256;
      surfaceAppearance["valid_plate_count"] = statistics.validPlateCount;
      surfaceAppearance["missing_plate_count"] = statistics.missingPlateCount;
      surfaceAppearance["invalid_plate_count"] = statistics.invalidPlateCount;
      surfaceAppearance["minimum_relative_albedo"] =
          statistics.minimumRelativeAlbedo;
      surfaceAppearance["maximum_relative_albedo"] =
          statistics.maximumRelativeAlbedo;
      surfaceAppearance["median_relative_albedo"] =
          statistics.medianRelativeAlbedo;
    }
    report["surface_appearance"] = std::move(surfaceAppearance);
    report["distortion"] =
        distortionReport(frame.cameraModelFormat, cameraStatistics);
  }
  return report;
}

ProductPaths productPaths(const std::filesystem::path &directory,
                          IlluminationMode illuminationMode,
                          SurfaceAppearanceMode surfaceAppearanceMode) {
  ProductPaths paths{directory / "image_u16.tif",
                     directory / "mask_u8.tif",
                     directory / "depth_m_f32.tif",
                     directory / "normal_camera_f32.tif",
                     directory / "facet_id_u32.tif",
                     {},
                     {},
                     std::nullopt};
  if (illuminationMode == IlluminationMode::PointSunLambert) {
    paths.mu0Float32 = directory / "mu0_f32.tif";
    paths.illuminationMaskU8 =
        directory / "illumination_mask_u8.tif";
  }
  if (surfaceAppearanceMode == SurfaceAppearanceMode::PlateAlbedo) {
    paths.albedoFloat32 = directory / "albedo_f32.tif";
  }
  return paths;
}

std::string sha256File(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot hash raw product " + path.string());
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  std::array<char, 1024 * 1024> buffer{};
  while (stream) {
    stream.read(buffer.data(),
                static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = stream.gcount();
    if (count > 0) {
      hash.addData(buffer.data(), count);
    }
  }
  if (!stream.eof()) {
    throw std::runtime_error("failed while hashing raw product " +
                             path.string());
  }
  return hash.result().toHex().toStdString();
}

json rawProductHashes(const ProductPaths &paths) {
  json hashes = json::object();
  const std::array<std::pair<const char *, const std::filesystem::path *>, 5>
      baseProducts{{
          {"image_u16.tif", &paths.imageU16},
          {"mask_u8.tif", &paths.maskU8},
          {"depth_m_f32.tif", &paths.depthMFloat32},
          {"normal_camera_f32.tif", &paths.normalCameraFloat32},
          {"facet_id_u32.tif", &paths.facetIdU32},
      }};
  for (const auto &[name, path] : baseProducts) {
    hashes[name] = sha256File(*path);
  }
  if (paths.mu0Float32.empty() != paths.illuminationMaskU8.empty()) {
    throw std::runtime_error("raw product diagnostics paths are incomplete");
  }
  if (!paths.mu0Float32.empty()) {
    hashes["mu0_f32.tif"] = sha256File(paths.mu0Float32);
    hashes["illumination_mask_u8.tif"] =
        sha256File(paths.illuminationMaskU8);
  }
  if (paths.albedoFloat32.has_value()) {
    hashes["albedo_f32.tif"] = sha256File(*paths.albedoFloat32);
  }
  return hashes;
}

}  // namespace

int main(int argc, char *argv[]) {
  std::filesystem::path outputDirectory;
  bool outputCreated = false;
  int reportSchemaVersion = 1;
  try {
    const Options options = parseArguments(argc, argv);
    outputDirectory = options.outputDirectory;
    createAttemptDirectory(outputDirectory);
    outputCreated = true;
    const auto started = std::chrono::steady_clock::now();

    const ResolvedRenderConfig config =
        Isis::SmallBodyRender::loadResolvedRenderConfig(
            options.resolvedConfig, options.frameId);
    reportSchemaVersion = config.schemaVersion;
    const std::unique_ptr<RenderCamera> camera =
        loadRenderCamera(config.frame, config.distortionInverse);
    BulletDskRayTracer tracer(config.dsk, config.maximumRangeM);
    const int plateCount = tracer.metadata().plateCount;
    if (plateCount <= 0) {
      throw std::runtime_error(
          "DSK_METADATA_ERROR: verified Type-2 DSK plateCount must be "
          "positive");
    }
    std::optional<PlateAlbedoStatistics> plateAlbedoStatistics;
    std::unique_ptr<SurfaceAppearance> appearance;
    if (config.surfaceAppearance.mode ==
        SurfaceAppearanceMode::PlateAlbedo) {
      PlateAlbedoTable table = PlateAlbedoTable::read(
          config.surfaceAppearance.plateAlbedoPath,
          static_cast<std::uint32_t>(plateCount),
          config.surfaceAppearance.defaultAlbedo,
          config.surfaceAppearance.plateAlbedoSha256);
      plateAlbedoStatistics = table.statistics();
      appearance = std::make_unique<PlateAlbedoTable>(std::move(table));
    }
    else {
      appearance = std::make_unique<UniformSurfaceAppearance>(
          config.surfaceAppearance.defaultAlbedo,
          static_cast<std::uint32_t>(plateCount));
    }
    const ProductPaths paths =
        productPaths(outputDirectory, config.illumination.mode,
                     config.surfaceAppearance.mode);
    TiffProductWriter writer(paths,
                             config.frame.widthPx, config.frame.heightPx,
                             config.tileWidthPx, config.tileHeightPx);
    const RenderSummary summary =
        DskFrameRenderer().render(config, *camera, tracer, *appearance, writer);
    json rawHashes = json::object();
    if (config.illumination.mode ==
        IlluminationMode::PointSunLambert) {
      const auto counts =
          Isis::SmallBodyRender::validatePointSunDiagnosticProducts(
              paths, config.frame.widthPx, config.frame.heightPx,
              config.tileWidthPx, config.tileHeightPx);
      if (counts.validPixels != summary.validPixels ||
          counts.directPixels != summary.directPixels ||
          counts.nightPixels != summary.nightPixels ||
          counts.selfShadowPixels != summary.selfShadowPixels) {
        throw std::runtime_error(
            "ILLUMINATION_PRODUCT_ERROR: diagnostic readback counts "
            "do not match render summary");
      }
    }
    if (config.illumination.mode == IlluminationMode::PointSunLambert ||
        config.surfaceAppearance.mode == SurfaceAppearanceMode::PlateAlbedo) {
      rawHashes = rawProductHashes(paths);
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      started)
            .count();
    writeJsonAtomically(outputDirectory / "render.json",
                        successReport(config, tracer, summary, elapsed,
                                      peakRssKib(), rawHashes,
                                      plateAlbedoStatistics,
                                      camera->rayStatistics()));
    return EXIT_SUCCESS;
  }
  catch (const CliError &error) {
    const json payload =
        failurePayload(error.category(), error.what(), reportSchemaVersion);
    std::cerr << payload.dump() << '\n';
    if (outputCreated) {
      try {
        writeJsonAtomically(outputDirectory / "failure.json", payload);
      }
      catch (...) {
      }
    }
  }
  catch (const Isis::IException &error) {
    const json payload =
        failurePayload("ISIS_ERROR", error.toString().toStdString(),
                       reportSchemaVersion);
    std::cerr << payload.dump() << '\n';
    if (outputCreated) {
      try {
        writeJsonAtomically(outputDirectory / "failure.json", payload);
      }
      catch (...) {
      }
    }
  }
  catch (const std::exception &error) {
    const std::string message = error.what();
    std::string category = "RENDER_ERROR";
    if (message.rfind("DISTORTION_INVERSE_ERROR", 0) == 0) {
      category = "DISTORTION_INVERSE_ERROR";
    }
    else if (message.rfind("CAMERA_MODEL_ERROR", 0) == 0) {
      category = "CAMERA_MODEL_ERROR";
    }
    const json payload =
        failurePayload(category, message, reportSchemaVersion);
    std::cerr << payload.dump() << '\n';
    if (outputCreated) {
      try {
        writeJsonAtomically(outputDirectory / "failure.json", payload);
      }
      catch (...) {
      }
    }
  }
  catch (...) {
    const json payload =
        failurePayload("UNKNOWN_ERROR", "unknown native exception",
                       reportSchemaVersion);
    std::cerr << payload.dump() << '\n';
    if (outputCreated) {
      try {
        writeJsonAtomically(outputDirectory / "failure.json", payload);
      }
      catch (...) {
      }
    }
  }
  return EXIT_FAILURE;
}
