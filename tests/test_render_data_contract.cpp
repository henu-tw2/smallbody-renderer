// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

// Modified by: gengxun + Codex
// Date: 2026-08-11
// Summary: Enforce the ASP-free schema-2 executable contract consumed from Python preflight output.

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "RenderDataContract.h"

namespace {

using Isis::SmallBodyRender::CameraModelFormat;
using Isis::SmallBodyRender::IlluminationMode;
using Isis::SmallBodyRender::SurfaceAppearanceMode;
using Isis::SmallBodyRender::loadResolvedRenderConfig;
using nlohmann::json;

void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void requireNear(double actual, double expected, double tolerance,
                 const std::string &message) {
  require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
          message + ": expected " + std::to_string(expected) + ", got " +
              std::to_string(actual));
}

class TemporaryConfig {
  public:
    TemporaryConfig() {
      static std::size_t sequence = 0;
      path_ = std::filesystem::temp_directory_path() /
              ("smallbody-render-contract-" +
               std::to_string(++sequence) + ".json");
    }

    ~TemporaryConfig() {
      std::error_code error;
      std::filesystem::remove(path_, error);
    }

    const std::filesystem::path &write(const json &document) {
      std::ofstream stream(path_);
      require(static_cast<bool>(stream), "open temporary resolved config");
      stream << document.dump(2);
      require(static_cast<bool>(stream), "write temporary resolved config");
      return path_;
    }

  private:
    std::filesystem::path path_;
};


json validResolvedConfig() {
  const std::string hash(64, 'a');
  return {
      {"schema_version", 1},
      {"dsk",
       {{"path", "/input/shape.bds"},
        {"sha256", hash},
        {"expected_center_id", 999001},
        {"expected_frame_id", 999001},
        {"expected_frame_name", "BODY_FIXED"},
        {"frame_kernel_paths", {"/input/body_frames.tf"}}}},
      {"render",
       {{"maximum_range_m", 100000.0},
        {"tile_width_px", 256},
        {"tile_height_px", 256},
        {"photometry", {{"ambient", 0.1}, {"diffuse", 0.9}}}}},
      {"isis_stretch", {{"minpercent", 0.1}, {"maxpercent", 99.9}}},
      {"batch", {{"jobs", 1}}},
      {"executables",
       {{"native", "/build/smallbody_dsk_render"},
        {"std2isis", "/env/bin/std2isis"},
        {"isis2std", "/env/bin/isis2std"},
        {"stats", "/env/bin/stats"},
        {"tiffinfo", "/env/bin/tiffinfo"},
        {"gdalinfo", "/env/bin/gdalinfo"},
        {"asp_version", "/env/bin/bundle_adjust"},
        {"parallel", "/env/bin/parallel"}}},
      {"frames",
       {{{"frame_id", "f0001"},
         {"et_tdb_s", 123456.25},
         {"camera_path", "/input/f0001.tsai"},
         {"camera_sha256", hash},
         {"width_px", 5000},
         {"height_px", 3000},
         {"camera_center_j2000_m", {10.0, 20.0, 30.0}},
         {"q_dsk_to_j2000_wxyz",
          {std::sqrt(0.5), 0.0, 0.0, std::sqrt(0.5)}},
         {"q_camera_to_j2000_wxyz", {1.0, 0.0, 0.0, 0.0}}},
        {{"frame_id", "f0002"},
         {"et_tdb_s", 123457.25},
         {"camera_path", "/input/f0002.tsai"},
         {"camera_sha256", hash},
         {"width_px", 5000},
         {"height_px", 3000},
         {"camera_center_j2000_m", {11.0, 21.0, 31.0}},
         {"q_dsk_to_j2000_wxyz", {1.0, 0.0, 0.0, 0.0}},
         {"q_camera_to_j2000_wxyz", {1.0, 0.0, 0.0, 0.0}}}}}};
}

json validResolvedConfigV2() {
  json document = validResolvedConfig();
  document["schema_version"] = 2;
  document["executables"].erase("asp_version");
  document["render"] = {
      {"maximum_range_m", 100000.0},
      {"tile_width_px", 256},
      {"tile_height_px", 256},
      {"exposure_scale", 1.0},
      {"illumination",
       {{"mode", "camera_headlight"},
        {"ambient", 0.1},
        {"diffuse", 0.9},
        {"cast_shadows", false}}},
      {"camera_distortion",
       {{"inverse_max_iterations", 20},
        {"roundtrip_tolerance_px", 1.0e-6}}},
      {"surface_appearance",
       {{"mode", "uniform"}, {"default_albedo", 1.0}}}};
  for (json &frame : document["frames"]) {
    frame["camera_model_format"] = "asp_tsai_null";
    frame["target_to_camera_j2000_m"] =
        frame.at("camera_center_j2000_m");
    frame.erase("camera_center_j2000_m");
  }
  return document;
}

template <typename Mutation>
void requireMutationRejects(Mutation mutation, const std::string &field) {
  json document = validResolvedConfig();
  mutation(document);
  TemporaryConfig file;
  try {
    static_cast<void>(loadResolvedRenderConfig(file.write(document), "f0001"));
  }
  catch (const std::exception &error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "diagnostic must contain '" + field + "': " + error.what());
    return;
  }
  require(false, "expected rejection for " + field);
}

template <typename Mutation>
void requireV2MutationRejects(Mutation mutation, const std::string &field) {
  json document = validResolvedConfigV2();
  mutation(document);
  TemporaryConfig file;
  try {
    static_cast<void>(loadResolvedRenderConfig(file.write(document), "f0001"));
  }
  catch (const std::exception &error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "diagnostic must contain '" + field + "': " + error.what());
    return;
  }
  require(false, "expected v2 rejection for " + field);
}

void useValidPointSunLambert(json &document) {
  document["render"]["illumination"] = {
      {"mode", "point_sun_lambert"},
      {"ambient", 0.1},
      {"diffuse", 0.9},
      {"cast_shadows", true},
      {"shadow_origin_offset_m", 0.01},
      {"shadow_maximum_range_m", 2000.0},
      {"state_source", "illumination_csv"},
      {"state_sha256", std::string(64, 'c')}};
  for (json &frame : document["frames"]) {
    frame["target_to_sun_j2000_m"] =
        json::array({1000.0, 2000.0, 3000.0});
  }
}

void useValidPlateAlbedo(json &document) {
  document["render"]["surface_appearance"] = {
      {"mode", "plate_albedo"},
      {"default_albedo", 1.0},
      {"plate_albedo_path", "/input/albedo.csv"},
      {"plate_albedo_sha256", std::string(64, 'b')}};
}

void testSelectsFrameAndUsesActiveWxyzRotations() {
  TemporaryConfig file;
  const auto config =
      loadResolvedRenderConfig(file.write(validResolvedConfig()), "f0001");

  require(config.schemaVersion == 1, "schema version");
  require(config.frame.frameId == "f0001", "selected frame ID");
  requireNear(config.frame.etTdbS, 123456.25, 0.0, "ET/TDB seconds");
  requireNear(config.frame.targetToCameraJ2000M.x(), 10.0, 0.0,
              "target-to-camera x in J2000 metres");
  requireNear(config.maximumRangeM, 100000.0, 0.0,
              "maximum range in metres");
  requireNear(
      (config.frame.rotationJ2000FromDsk *
       Eigen::Vector3d::UnitX()).y(),
      1.0, 1.0e-12, "active +90 degree D-to-J rotation");
  requireNear(
      (config.frame.rotationJ2000FromCamera *
       Eigen::Vector3d::UnitZ()).z(),
      1.0, 1.0e-15, "identity camera-to-J2000 rotation");
  require(config.dsk.path == "/input/shape.bds", "absolute DSK path retained");
  require(config.dsk.frameKernelPaths.size() == 1,
          "frame-kernel path retained");
  require(config.frame.cameraModelFormat == CameraModelFormat::AspTsaiNull,
          "v1 camera model defaults to ASP TSAI NULL");
  require(!config.frame.targetToSunJ2000M.has_value(),
          "v1 has no target-to-Sun vector");
  requireNear(config.exposureScale, 1.0, 0.0, "v1 exposure scale");
  require(config.distortionInverse.maximumIterations == 20,
          "v1 inverse-distortion iteration default");
  requireNear(config.distortionInverse.roundTripTolerancePx, 1.0e-6, 0.0,
              "v1 inverse-distortion tolerance default");
  require(config.illumination.mode == IlluminationMode::CameraHeadlight,
          "v1 illumination mode");
  requireNear(config.illumination.ambient, 0.1, 0.0,
              "v1 illumination ambient maps photometry");
  requireNear(config.illumination.diffuse, 0.9, 0.0,
              "v1 illumination diffuse maps photometry");
  require(!config.illumination.castShadows,
          "v1 camera headlight does not cast shadows");
  requireNear(config.illumination.shadowOriginOffsetM, 0.0, 0.0,
              "v1 has no shadow origin offset");
  requireNear(config.illumination.shadowMaximumRangeM, 0.0, 0.0,
              "v1 has no shadow trace range");
  require(config.surfaceAppearance.mode == SurfaceAppearanceMode::Uniform,
          "v1 surface appearance mode");
  requireNear(config.surfaceAppearance.defaultAlbedo, 1.0, 0.0,
              "v1 uniform albedo");
  require(config.surfaceAppearance.plateAlbedoPath.empty() &&
              config.surfaceAppearance.plateAlbedoSha256.empty(),
          "v1 has no plate-albedo input");
}

void testPointSunFreezesStateProvenanceOnlyInPointSunMode() {
  json document = validResolvedConfigV2();
  useValidPointSunLambert(document);
  TemporaryConfig file;
  const auto config =
      loadResolvedRenderConfig(file.write(document), "f0001");
  require(config.illumination.stateSource == "illumination_csv",
          "point-sun state source");
  require(config.illumination.stateSha256 == std::string(64, 'c'),
          "point-sun state SHA-256");

  requireV2MutationRejects(
      [](json &headlight) {
        headlight["render"]["illumination"]["state_source"] =
            "illumination_csv";
        headlight["render"]["illumination"]["state_sha256"] =
            std::string(64, 'c');
      },
      "state_");
  json bad = validResolvedConfigV2();
  useValidPointSunLambert(bad);
  bad["render"]["illumination"]["state_source"] = "manual";
  TemporaryConfig badFile;
  try {
    static_cast<void>(
        loadResolvedRenderConfig(badFile.write(bad), "f0001"));
  }
  catch (const std::exception &error) {
    require(std::string(error.what()).find("state_source") !=
                std::string::npos,
            "invalid state source diagnostic");
    return;
  }
  require(false, "invalid point-sun state source must be rejected");
}


void testParsesVersionTwoDirectionExplicitCameraVector() {
  TemporaryConfig file;
  const auto config =
      loadResolvedRenderConfig(file.write(validResolvedConfigV2()), "f0001");

  require(config.schemaVersion == 2, "schema v2 version");
  requireNear(config.frame.targetToCameraJ2000M.x(), 10.0, 0.0,
              "v2 target-to-camera x in J2000 metres");
  require(config.frame.cameraModelFormat == CameraModelFormat::AspTsaiNull,
          "v2 camera model format");
  require(!config.frame.targetToSunJ2000M.has_value(),
          "camera-headlight v2 has no target-to-Sun vector");
  requireNear(config.exposureScale, 1.0, 0.0, "v2 exposure scale");
  require(config.distortionInverse.maximumIterations == 20,
          "v2 inverse-distortion iterations");
  requireNear(config.distortionInverse.roundTripTolerancePx, 1.0e-6, 0.0,
              "v2 inverse-distortion tolerance");
  require(config.illumination.mode == IlluminationMode::CameraHeadlight &&
              !config.illumination.castShadows,
          "v2 legacy illumination mode");
  require(config.surfaceAppearance.mode == SurfaceAppearanceMode::Uniform,
          "v2 legacy surface appearance mode");
}

void testAcceptsPointSunUniformWithNonIdentityPhotometry() {
  json document = validResolvedConfigV2();
  useValidPointSunLambert(document);
  document["render"]["exposure_scale"] = 0.5;
  document["render"]["surface_appearance"]["default_albedo"] = 0.25;
  TemporaryConfig file;
  const auto config =
      loadResolvedRenderConfig(file.write(document), "f0001");

  require(config.illumination.mode == IlluminationMode::PointSunLambert,
          "point-sun illumination mode");
  require(config.frame.targetToSunJ2000M.has_value(),
          "point-sun frame retains required target-to-Sun vector");
  requireNear(config.frame.targetToSunJ2000M->x(), 1000.0, 0.0,
              "target-to-Sun x in J2000 metres");
  requireNear(config.exposureScale, 0.5, 0.0,
              "point-sun non-identity exposure");
  requireNear(config.surfaceAppearance.defaultAlbedo, 0.25, 0.0,
              "point-sun non-identity uniform albedo");
}

void testRejectsFrameSelectionAndUnsafeIdentifiers() {
  requireMutationRejects(
      [](json &document) { document["frames"][1]["frame_id"] = "f0001"; },
      "frame_id");
  requireMutationRejects(
      [](json &document) { document["frames"][0]["frame_id"] = "../f0001"; },
      "frame_id");

  TemporaryConfig file;
  try {
    static_cast<void>(
        loadResolvedRenderConfig(file.write(validResolvedConfig()), "missing"));
  }
  catch (const std::exception &error) {
    require(std::string(error.what()).find("frame_id") != std::string::npos,
            "missing requested frame diagnostic contains frame_id");
    return;
  }
  require(false, "missing requested frame must be rejected");
}

void testRejectsMissingUnknownAndWrongSchemaFields() {
  requireMutationRejects(
      [](json &document) { document["dsk"].erase("expected_center_id"); },
      "expected_center_id");
  requireMutationRejects(
      [](json &document) { document["render"].erase("maximum_range_m"); },
      "maximum_range_m");
  requireMutationRejects(
      [](json &document) {
        document["frames"][0].erase("camera_center_j2000_m");
      },
      "camera_center_j2000_m");
  requireMutationRejects(
      [](json &document) { document["unexpected"] = true; }, "unexpected");
  requireMutationRejects(
      [](json &document) {
        document["frames"][0]["target_to_camera_j2000_m"] =
            json::array({10.0, 20.0, 30.0});
      },
      "target_to_camera_j2000_m");
  requireV2MutationRejects(
      [](json &document) {
        document["frames"][0]["camera_center_j2000_m"] =
            json::array({10.0, 20.0, 30.0});
      },
      "camera_center_j2000_m");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["unexpected"] = true;
      },
      "render.unexpected");
  requireV2MutationRejects(
      [](json &document) {
        document["frames"][0]["unexpected"] = true;
      },
      "frames[0].unexpected");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["illumination"]["unexpected"] = true;
      },
      "render.illumination.unexpected");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["camera_distortion"]["unexpected"] = true;
      },
      "render.camera_distortion.unexpected");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["surface_appearance"]["unexpected"] = true;
      },
      "render.surface_appearance.unexpected");
}

void testAcceptsPlannedAndRejectsUnknownVersionTwoCameraModes() {
  struct CameraFormatExpectation {
    const char *serialized;
    CameraModelFormat expected;
  };
  for (const CameraFormatExpectation expectation : {
           CameraFormatExpectation{"asp_tsai5", CameraModelFormat::AspTsai5},
           CameraFormatExpectation{"opencv_radtan5_json",
                                   CameraModelFormat::OpenCvRadtan5Json},
       }) {
    json document = validResolvedConfigV2();
    document["frames"][0]["camera_model_format"] = expectation.serialized;
    TemporaryConfig file;
    const auto config =
        loadResolvedRenderConfig(file.write(document), "f0001");
    require(config.frame.cameraModelFormat == expectation.expected,
            std::string("planned camera format is accepted: ") +
                expectation.serialized);
  }
  requireV2MutationRejects(
      [](json &document) {
        document["frames"][0]["camera_model_format"] = "unknown_camera";
      },
      "camera_model_format");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["illumination"]["mode"] = "unknown_light";
      },
      "render.illumination.mode");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["surface_appearance"]["mode"] = "unknown_surface";
      },
      "render.surface_appearance.mode");
  json plateAlbedo = validResolvedConfigV2();
  useValidPlateAlbedo(plateAlbedo);
  TemporaryConfig plateAlbedoFile;
  const auto plateAlbedoConfig = loadResolvedRenderConfig(
      plateAlbedoFile.write(plateAlbedo), "f0001");
  require(plateAlbedoConfig.surfaceAppearance.mode ==
              SurfaceAppearanceMode::PlateAlbedo,
          "implemented plate-albedo mode is accepted");
  require(plateAlbedoConfig.surfaceAppearance.plateAlbedoPath ==
              "/input/albedo.csv",
          "plate-albedo path is retained");
  require(plateAlbedoConfig.surfaceAppearance.plateAlbedoSha256 ==
              std::string(64, 'b'),
          "plate-albedo SHA-256 is retained");
}

void testValidatesFutureModeContractsBeforeRejectingImplementation() {
  json implementedPhotometry = validResolvedConfigV2();
  useValidPointSunLambert(implementedPhotometry);
  useValidPlateAlbedo(implementedPhotometry);
  implementedPhotometry["render"]["exposure_scale"] = 2.0;
  implementedPhotometry["render"]["surface_appearance"]["default_albedo"] =
      2.0;
  TemporaryConfig implementedPhotometryFile;
  const auto implementedPhotometryConfig = loadResolvedRenderConfig(
      implementedPhotometryFile.write(implementedPhotometry), "f0001");
  require(implementedPhotometryConfig.illumination.mode ==
              IlluminationMode::PointSunLambert,
          "implemented point-sun Lambert mode is accepted with plate albedo");
  require(implementedPhotometryConfig.surfaceAppearance.mode ==
              SurfaceAppearanceMode::PlateAlbedo,
          "implemented plate-albedo mode is retained with point-sun Lambert");
  requireNear(implementedPhotometryConfig.exposureScale, 2.0, 0.0,
              "point-sun plate-albedo exposure scale");
  requireNear(implementedPhotometryConfig.surfaceAppearance.defaultAlbedo,
              2.0, 0.0, "point-sun plate-albedo default albedo");
  require(implementedPhotometryConfig.surfaceAppearance.plateAlbedoPath ==
              "/input/albedo.csv" &&
              implementedPhotometryConfig.surfaceAppearance
                      .plateAlbedoSha256 == std::string(64, 'b'),
          "point-sun plate-albedo path and SHA-256 are retained");
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["render"]["illumination"]["shadow_range_metres"] = 2000.0;
      },
      "render.illumination.shadow_range_metres");
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["render"]["illumination"].erase("shadow_maximum_range_m");
      },
      "render.illumination.shadow_maximum_range_m");
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["render"]["illumination"]["cast_shadows"] = "yes";
      },
      "render.illumination.cast_shadows");
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["render"]["illumination"]["ambient"] = 0.0;
        document["render"]["illumination"]["diffuse"] = 1.0;
      },
      "render.illumination.ambient");
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["render"]["illumination"]["shadow_origin_offset_m"] = 0.0;
      },
      "render.illumination.shadow_origin_offset_m");
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["render"]["illumination"]["shadow_origin_offset_m"] = 0.0;
      },
      "SHADOW_CONFIGURATION_ERROR");
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["render"]["illumination"]["shadow_maximum_range_m"] = 0.0;
      },
      "render.illumination.shadow_maximum_range_m");
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["render"]["illumination"]["shadow_maximum_range_m"] = 0.0;
      },
      "SHADOW_CONFIGURATION_ERROR");

  requireV2MutationRejects(
      [](json &document) {
        useValidPlateAlbedo(document);
        document["render"]["surface_appearance"]["albedo_path"] =
            "/input/albedo.csv";
      },
      "render.surface_appearance.albedo_path");
  requireV2MutationRejects(
      [](json &document) {
        useValidPlateAlbedo(document);
        document["render"]["surface_appearance"].erase(
            "plate_albedo_sha256");
      },
      "render.surface_appearance.plate_albedo_sha256");
  requireV2MutationRejects(
      [](json &document) {
        useValidPlateAlbedo(document);
        document["render"]["surface_appearance"]["plate_albedo_path"] =
            "albedo.csv";
      },
      "render.surface_appearance.plate_albedo_path");
  requireV2MutationRejects(
      [](json &document) {
        useValidPlateAlbedo(document);
        document["render"]["surface_appearance"]["default_albedo"] = 0.0;
      },
      "render.surface_appearance.default_albedo");
  requireV2MutationRejects(
      [](json &document) {
        useValidPlateAlbedo(document);
        document["render"]["surface_appearance"]["plate_albedo_sha256"] =
            "bad";
      },
      "render.surface_appearance.plate_albedo_sha256");
}

void testRejectsInvalidVersionTwoControls() {
  requireV2MutationRejects(
      [](json &document) { document["render"]["exposure_scale"] = 2.0; },
      "render.exposure_scale: recognized non-identity");
  requireV2MutationRejects(
      [](json &document) { document["render"]["exposure_scale"] = 0.0; },
      "render.exposure_scale");
  requireV2MutationRejects(
      [](json &document) { document["render"]["exposure_scale"] = nullptr; },
      "render.exposure_scale");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["camera_distortion"]
                ["inverse_max_iterations"] = 0;
      },
      "inverse_max_iterations");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["camera_distortion"]
                ["roundtrip_tolerance_px"] = 0.0;
      },
      "roundtrip_tolerance_px");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["illumination"]["cast_shadows"] = true;
      },
      "cast_shadows");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["surface_appearance"]["default_albedo"] = 2.0;
      },
      "render.surface_appearance.default_albedo: recognized non-identity");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["surface_appearance"]["default_albedo"] = 0.0;
      },
      "default_albedo");
  requireV2MutationRejects(
      [](json &document) {
        document["frames"][0]["target_to_camera_j2000_m"][2] = nullptr;
      },
      "target_to_camera_j2000_m[2]");
}

void testRejectsMixedIlluminationFrameContracts() {
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["frames"][0].erase("target_to_sun_j2000_m");
      },
      "ILLUMINATION_CONTRACT_ERROR");
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["frames"][0].erase("target_to_sun_j2000_m");
      },
      "frames[0].target_to_sun_j2000_m");
  requireV2MutationRejects(
      [](json &document) {
        document["frames"][0]["target_to_sun_j2000_m"] =
            json::array({1000.0, 0.0, 0.0});
      },
      "ILLUMINATION_CONTRACT_ERROR");
  requireV2MutationRejects(
      [](json &document) {
        document["frames"][0]["target_to_sun_j2000_m"] =
            json::array({1000.0, 0.0, 0.0});
      },
      "frames[0].target_to_sun_j2000_m");
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["frames"][0]["target_to_sun_j2000_m"][1] = "bad";
      },
      "ILLUMINATION_CONTRACT_ERROR");
  requireV2MutationRejects(
      [](json &document) {
        useValidPointSunLambert(document);
        document["frames"][0]["target_to_sun_j2000_m"][1] = "bad";
      },
      "target_to_sun_j2000_m[1]");
}

void testRejectsNonFiniteAndInvalidQuaternionValues() {
  requireMutationRejects(
      [](json &document) {
        document["frames"][0]["et_tdb_s"] =
            std::numeric_limits<double>::infinity();
      },
      "et_tdb_s");
  requireMutationRejects(
      [](json &document) {
        document["frames"][0]["camera_center_j2000_m"][1] = nullptr;
      },
      "camera_center_j2000_m[1]");
  requireMutationRejects(
      [](json &document) {
        document["frames"][0]["q_camera_to_j2000_wxyz"] =
            json::array({2.0, 0.0, 0.0, 0.0});
      },
      "q_camera_to_j2000_wxyz");
}

void testRejectsInvalidDimensionsRangeAndPhotometry() {
  requireMutationRejects(
      [](json &document) { document["frames"][0]["width_px"] = 0; },
      "width_px");
  requireMutationRejects(
      [](json &document) { document["frames"][0]["height_px"] = -1; },
      "height_px");
  requireMutationRejects(
      [](json &document) { document["render"]["maximum_range_m"] = 0.0; },
      "maximum_range_m");
  requireMutationRejects(
      [](json &document) { document["render"]["tile_width_px"] = 250; },
      "tile_width_px");
  requireMutationRejects(
      [](json &document) { document["render"]["tile_height_px"] = 15; },
      "tile_height_px");
  requireMutationRejects(
      [](json &document) {
        document["render"]["photometry"]["ambient"] = -0.1;
      },
      "ambient");
  requireMutationRejects(
      [](json &document) {
        document["render"]["photometry"]["diffuse"] = -0.1;
      },
      "diffuse");
  requireMutationRejects(
      [](json &document) {
        document["render"]["photometry"]["ambient"] = 0.2;
      },
      "photometry");
}

void testEnforcesBoundedTileWorksets() {
  {
    json document = validResolvedConfigV2();
    document["render"]["tile_width_px"] = 4096;
    document["render"]["tile_height_px"] = 256;
    TemporaryConfig file;
    const auto resolved =
        loadResolvedRenderConfig(file.write(document), "f0001");
    require(resolved.tileWidthPx == 4096 &&
                resolved.tileHeightPx == 256,
            "maximum safe tile workset is accepted");
  }
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["tile_width_px"] = 4112;
        document["render"]["tile_height_px"] = 16;
      },
      "render.tile_width_px");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["tile_width_px"] = 16;
        document["render"]["tile_height_px"] = 4112;
      },
      "render.tile_height_px");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["tile_width_px"] = 4096;
        document["render"]["tile_height_px"] = 272;
      },
      "render.tile_pixels");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["tile_width_px"] = 65536;
        document["render"]["tile_height_px"] = 16;
      },
      "render.tile_width_px");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["tile_width_px"] = 2147483632;
        document["render"]["tile_height_px"] = 16;
      },
      "render.tile_width_px");
  requireV2MutationRejects(
      [](json &document) {
        document["render"]["tile_width_px"] = true;
      },
      "render.tile_width_px");
}

void testRejectsRelativePathsAndMalformedHashes() {
  requireMutationRejects(
      [](json &document) { document["dsk"]["path"] = "shape.bds"; },
      "dsk.path");
  requireMutationRejects(
      [](json &document) {
        document["dsk"]["frame_kernel_paths"][0] = "frames.tf";
      },
      "frame_kernel_paths[0]");
  requireMutationRejects(
      [](json &document) {
        document["frames"][0]["camera_path"] = "f0001.tsai";
      },
      "camera_path");
  requireMutationRejects(
      [](json &document) { document["dsk"]["sha256"] = "abc"; }, "sha256");
  requireMutationRejects(
      [](json &document) {
        document["frames"][0]["camera_sha256"] = std::string(64, 'A');
      },
      "camera_sha256");
}

}  // namespace

int main() {
  testSelectsFrameAndUsesActiveWxyzRotations();
  testPointSunFreezesStateProvenanceOnlyInPointSunMode();
  testParsesVersionTwoDirectionExplicitCameraVector();
  testAcceptsPointSunUniformWithNonIdentityPhotometry();
  testRejectsFrameSelectionAndUnsafeIdentifiers();
  testRejectsMissingUnknownAndWrongSchemaFields();
  testAcceptsPlannedAndRejectsUnknownVersionTwoCameraModes();
  testValidatesFutureModeContractsBeforeRejectingImplementation();
  testRejectsInvalidVersionTwoControls();
  testRejectsMixedIlluminationFrameContracts();
  testRejectsNonFiniteAndInvalidQuaternionValues();
  testRejectsInvalidDimensionsRangeAndPhotometry();
  testEnforcesBoundedTileWorksets();
  testRejectsRelativePathsAndMalformedHashes();
  std::cout << "PASS: render data contract tests\n";
  return EXIT_SUCCESS;
}
