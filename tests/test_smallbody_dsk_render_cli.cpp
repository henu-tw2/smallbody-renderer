// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

// Modified by: gengxun + Codex
// Date: 2026-08-11
// Summary: Exercise CLI rendering with the ASP-free schema-2 executable contract emitted by Python preflight.

#include <tiffio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>

#include <nlohmann/json.hpp>

namespace {

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
          message + ": got " + std::to_string(actual) +
              ", expected " + std::to_string(expected));
}

void writeText(const std::filesystem::path &path, const std::string &text) {
  std::ofstream stream(path);
  require(static_cast<bool>(stream), "open " + path.string());
  stream << text;
  require(static_cast<bool>(stream), "write " + path.string());
}

std::string readBytes(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  require(static_cast<bool>(stream), "read " + path.string());
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

struct RunResult {
  int exitCode;
  std::string standardOutput;
  std::string standardError;
};

RunResult run(const QStringList &arguments) {
  QProcess process;
  process.start(QString::fromUtf8(SMALLBODY_DSK_RENDER_EXE), arguments);
  require(process.waitForStarted(5000), "native renderer starts");
  require(process.waitForFinished(30000), "native renderer finishes");
  return {process.exitCode(),
          process.readAllStandardOutput().toStdString(),
          process.readAllStandardError().toStdString()};
}

json errorPayload(const RunResult &result) {
  require(!result.standardError.empty(), "failure emits JSON to stderr");
  return json::parse(result.standardError);
}

json resolvedConfig(const std::filesystem::path &cameraPath) {
  const std::string hash(64, 'a');
  return {
      {"schema_version", 1},
      {"dsk",
       {{"path", std::filesystem::path(ONE_SEGMENT_DSK).string()},
        {"sha256", hash},
        {"expected_center_id", 999001},
        {"expected_frame_id", 1},
        {"expected_frame_name", "J2000"},
        {"frame_kernel_paths", json::array()}}},
      {"render",
       {{"maximum_range_m", 100.0},
        {"tile_width_px", 16},
        {"tile_height_px", 16},
        {"photometry", {{"ambient", 0.1}, {"diffuse", 0.9}}}}},
      {"isis_stretch", {{"minpercent", 0.1}, {"maxpercent", 99.9}}},
      {"batch", {{"jobs", 1}}},
      {"executables",
       {{"native", SMALLBODY_DSK_RENDER_EXE},
        {"std2isis", "/bin/true"},
        {"isis2std", "/bin/true"},
        {"stats", "/bin/true"},
        {"tiffinfo", "/bin/true"},
        {"gdalinfo", "/bin/true"},
        {"asp_version", "/bin/true"},
        {"parallel", "/bin/true"}}},
      {"frames",
       {{{"frame_id", "f0001"},
         {"et_tdb_s", 123456.25},
         {"camera_path", cameraPath.string()},
         {"camera_sha256", hash},
         {"width_px", 19},
         {"height_px", 17},
         {"camera_center_j2000_m", {0.0, 0.0, 20.0}},
         {"q_dsk_to_j2000_wxyz", {1.0, 0.0, 0.0, 0.0}},
         {"q_camera_to_j2000_wxyz", {0.0, 1.0, 0.0, 0.0}}}}}};
}

json resolvedConfigV2(const std::filesystem::path &cameraPath) {
  json document = resolvedConfig(cameraPath);
  document["schema_version"] = 2;
  document["executables"].erase("asp_version");
  document["render"] = {
      {"maximum_range_m", 100.0},
      {"tile_width_px", 16},
      {"tile_height_px", 16},
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
  json &frame = document["frames"][0];
  frame["camera_model_format"] = "asp_tsai_null";
  frame["target_to_camera_j2000_m"] =
      frame.at("camera_center_j2000_m");
  frame.erase("camera_center_j2000_m");
  return document;
}

json resolvedConfigV2PointSun(const std::filesystem::path &cameraPath) {
  json document = resolvedConfigV2(cameraPath);
  document["render"]["exposure_scale"] = 0.75;
  document["render"]["illumination"] = {
      {"mode", "point_sun_lambert"},
      {"ambient", 0.1},
      {"diffuse", 0.9},
      {"cast_shadows", true},
      {"shadow_origin_offset_m", 0.01},
      {"shadow_maximum_range_m", 100.0},
      {"state_source", "illumination_csv"},
      {"state_sha256", std::string(64, 'c')}};
  document["render"]["surface_appearance"]["default_albedo"] = 0.8;
  document["frames"][0]["target_to_sun_j2000_m"] =
      json::array({0.0, 0.0, 100.0});
  return document;
}

json resolvedConfigV2PlateAlbedo(const std::filesystem::path &cameraPath,
                                 const std::filesystem::path &tablePath) {
  json document = resolvedConfigV2(cameraPath);
  document["render"]["surface_appearance"] = {
      {"mode", "plate_albedo"},
      {"default_albedo", 1.0},
      {"plate_albedo_path", tablePath.string()},
      {"plate_albedo_sha256",
       "570d62a0681752690b21677bbdf99176d8299c10cf87892ea7ff2b52404e9fd5"}};
  return document;
}

json resolvedConfigV2PointSunPlateAlbedo(
    const std::filesystem::path &cameraPath,
    const std::filesystem::path &tablePath) {
  json document = resolvedConfigV2PointSun(cameraPath);
  document["render"]["surface_appearance"] = {
      {"mode", "plate_albedo"},
      {"default_albedo", 1.0},
      {"plate_albedo_path", tablePath.string()},
      {"plate_albedo_sha256",
       "570d62a0681752690b21677bbdf99176d8299c10cf87892ea7ff2b52404e9fd5"}};
  return document;
}

std::string canonicalTsai(const std::string &terminal) {
  return "VERSION_4\n"
         "PINHOLE\n"
         "fu = 100\n"
         "fv = 100\n"
         "cu = 9\n"
         "cv = 8\n"
         "u_direction = 1 0 0\n"
         "v_direction = 0 1 0\n"
         "w_direction = 0 0 1\n"
         "C = 0 0 20\n"
         "R = 1 0 0 0 -1 0 0 0 -1\n"
         "pitch = 1\n" +
         terminal;
}

void requireTiffSize(const std::filesystem::path &path) {
  TIFF *tiff = TIFFOpen(path.c_str(), "r");
  require(tiff != nullptr, "open output TIFF");
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  require(TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) == 1 &&
              TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) == 1 &&
              width == 19 && height == 17,
          "raw TIFF has exact dimensions");
  TIFFClose(tiff);
}

float readDepthPixel(const std::filesystem::path &path, int sample, int line) {
  TIFF *tiff = TIFFOpen(path.c_str(), "r");
  require(tiff != nullptr, "open depth TIFF");
  std::uint32_t tileWidth = 0;
  std::uint32_t tileHeight = 0;
  std::uint16_t bits = 0;
  std::uint16_t samples = 0;
  std::uint16_t sampleFormat = 0;
  require(TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tileWidth) == 1 &&
              TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tileHeight) == 1,
          "depth TIFF is tiled");
  TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sampleFormat);
  require(bits == 32 && samples == 1 && sampleFormat == SAMPLEFORMAT_IEEEFP,
          "depth TIFF is one Float32 band");
  const tmsize_t tileBytes = TIFFTileSize(tiff);
  require(tileBytes > 0 && tileBytes % static_cast<tmsize_t>(sizeof(float)) == 0,
          "depth TIFF tile size is valid");
  std::vector<float> tile(static_cast<std::size_t>(tileBytes) / sizeof(float));
  require(TIFFReadTile(tiff, tile.data(), sample, line, 0, 0) >= 0,
          "read depth TIFF tile");
  const std::size_t index =
      static_cast<std::size_t>(line % static_cast<int>(tileHeight)) *
          tileWidth +
      static_cast<std::size_t>(sample % static_cast<int>(tileWidth));
  require(index < tile.size(), "depth pixel is within TIFF tile");
  const float value = tile[index];
  TIFFClose(tiff);
  return value;
}

float readFloatPixel(const std::filesystem::path &path, int sample, int line) {
  return readDepthPixel(path, sample, line);
}

template <typename Value>
Value readSingleBandIntegerPixel(const std::filesystem::path &path,
                                 int sample, int line,
                                 std::uint16_t expectedBits) {
  TIFF *tiff = TIFFOpen(path.c_str(), "r");
  require(tiff != nullptr, "open integer TIFF");
  std::uint32_t tileWidth = 0;
  std::uint32_t tileHeight = 0;
  std::uint16_t bits = 0;
  std::uint16_t samples = 0;
  std::uint16_t sampleFormat = 0;
  require(TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tileWidth) == 1 &&
              TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tileHeight) == 1,
          "integer TIFF is tiled");
  TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sampleFormat);
  require(bits == expectedBits && samples == 1 &&
              sampleFormat == SAMPLEFORMAT_UINT,
          "integer TIFF is exact unsigned single band");
  const tmsize_t tileBytes = TIFFTileSize(tiff);
  require(tileBytes > 0 &&
              tileBytes % static_cast<tmsize_t>(sizeof(Value)) == 0,
          "integer TIFF tile size is valid");
  std::vector<Value> tile(
      static_cast<std::size_t>(tileBytes) / sizeof(Value));
  require(TIFFReadTile(tiff, tile.data(), sample, line, 0, 0) >= 0,
          "read integer TIFF tile");
  const std::size_t index =
      static_cast<std::size_t>(line % static_cast<int>(tileHeight)) *
          tileWidth +
      static_cast<std::size_t>(sample % static_cast<int>(tileWidth));
  require(index < tile.size(), "integer pixel is within TIFF tile");
  const Value value = tile[index];
  TIFFClose(tiff);
  return value;
}

std::uint16_t readUInt16Pixel(const std::filesystem::path &path, int sample,
                              int line) {
  return readSingleBandIntegerPixel<std::uint16_t>(path, sample, line, 16);
}

std::uint32_t readUInt32Pixel(const std::filesystem::path &path, int sample,
                              int line) {
  return readSingleBandIntegerPixel<std::uint32_t>(path, sample, line, 32);
}

void testSuccessfulReportAndNoReplaceOutput() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary CLI fixture");
  const std::filesystem::path root(temporary.path().toStdString());
  const auto camera = root / "camera.tsai";
  writeText(camera,
            "VERSION_4\nPINHOLE\nfu = 30\nfv = 30\ncu = 9\ncv = 8\n"
            "u_direction = 1 0 0\nv_direction = 0 1 0\n"
            "w_direction = 0 0 1\nC = 0 0 20\n"
            "R = 1 0 0 0 -1 0 0 0 -1\npitch = 1\nNULL\n");
  const auto configPath = root / "resolved.json";
  writeText(configPath, resolvedConfig(camera).dump(2));
  const auto output = root / "attempt-f0001";

  const RunResult success =
      run({"--resolved-config", QString::fromStdString(configPath.string()),
           "--frame-id", "f0001", "--output-dir",
           QString::fromStdString(output.string())});
  require(success.exitCode == 0, "valid CLI render succeeds: " +
                                     success.standardError);
  std::set<std::string> names;
  for (const auto &entry : std::filesystem::directory_iterator(output)) {
    names.insert(entry.path().filename().string());
  }
  require(names == std::set<std::string>{
                       "image_u16.tif", "mask_u8.tif", "depth_m_f32.tif",
                       "normal_camera_f32.tif", "facet_id_u32.tif",
                       "render.json"},
          "success writes only five TIFFs and render.json");
  requireTiffSize(output / "image_u16.tif");

  const json report = json::parse(readBytes(output / "render.json"));
  require(report.at("schema_version") == 1 &&
              report.at("status") == "passed" &&
              report.count("raw_product_sha256") == 0,
          "passed report schema");
  require(report.at("frame").at("frame_id") == "f0001" &&
              report.at("frame").at("et_tdb_s") == 123456.25 &&
              report.at("frame").at("camera_center_j2000_m") ==
                  json::array({0.0, 0.0, 20.0}) &&
              report.at("frame").at("q_dsk_to_j2000_wxyz").size() == 4 &&
              report.at("frame").at("q_camera_to_j2000_wxyz").size() == 4 &&
              report.at("frame").at("rotation_j2000_from_dsk").size() == 9 &&
              report.at("frame").at("rotation_j2000_from_camera").size() == 9,
          "report freezes explicit frame state");
  require(report.at("frame").count("target_to_camera_j2000_m") == 0 &&
              report.at("coordinate_contract")
                      .count("camera_vector_origin") == 0 &&
              report.at("coordinate_contract")
                      .count("camera_vector_direction") == 0 &&
              report.at("coordinate_contract")
                      .count("camera_vector_axes") == 0,
          "v1 report does not acquire v2-only fields");
  require(report.at("coordinate_contract").at("quaternion_order") ==
                  "wxyz" &&
              report.at("coordinate_contract").at("time_scale") ==
                  "ET/TDB" &&
              report.at("coordinate_contract").at("length_unit") == "m" &&
              report.at("coordinate_contract").at("dsk_rotation") ==
                  "R_JD: DSK body-fixed to target-centered J2000" &&
              report.at("coordinate_contract").at("camera_rotation") ==
                  "R_JC: camera to target-centered J2000",
          "coordinate directions and units are explicit");
  require(report.at("dsk").at("center_id") == 999001 &&
              report.at("dsk").at("frame_id") == 1 &&
              report.at("dsk").at("plate_count") == 4 &&
              report.at("summary").at("valid_pixels").get<int>() > 0,
          "DSK metadata and render summary");
  for (const std::string key :
       {"app", "isis", "bullet", "cspice", "libtiff"}) {
    require(report.at("versions").at(key).is_string() &&
                !report.at("versions").at(key).get<std::string>().empty(),
            "nonempty dependency version " + key);
  }
  require(report.at("resources").at("elapsed_seconds").get<double>() >= 0.0 &&
              report.at("resources").at("peak_rss_kib").get<long long>() > 0,
          "elapsed time and peak RSS reported");

  const std::string before = readBytes(output / "render.json");
  const RunResult repeated =
      run({"--resolved-config", QString::fromStdString(configPath.string()),
           "--frame-id", "f0001", "--output-dir",
           QString::fromStdString(output.string())});
  require(repeated.exitCode != 0 &&
              errorPayload(repeated).at("category") ==
                  "OUTPUT_DIRECTORY_EXISTS" &&
              readBytes(output / "render.json") == before,
          "existing attempt is rejected before any overwrite");
}

void testVersionTwoReportAndParsedFailureUseVersionTwo() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary v2 CLI fixture");
  const std::filesystem::path root(temporary.path().toStdString());
  const auto camera = root / "camera.tsai";
  writeText(camera,
            "VERSION_4\nPINHOLE\nfu = 30\nfv = 30\ncu = 9\ncv = 8\n"
            "u_direction = 1 0 0\nv_direction = 0 1 0\n"
            "w_direction = 0 0 1\nC = 0 0 20\n"
            "R = 1 0 0 0 -1 0 0 0 -1\npitch = 1\nNULL\n");
  const auto configPath = root / "resolved-v2.json";
  writeText(configPath, resolvedConfigV2(camera).dump(2));
  const auto output = root / "attempt-v2-f0001";

  const RunResult success =
      run({"--resolved-config", QString::fromStdString(configPath.string()),
           "--frame-id", "f0001", "--output-dir",
           QString::fromStdString(output.string())});
  require(success.exitCode == 0, "valid v2 CLI render succeeds: " +
                                     success.standardError);
  const json report = json::parse(readBytes(output / "render.json"));
  require(report.at("schema_version") == 2, "v2 report schema");
  require(report.at("coordinate_contract")
              .at("camera_vector_origin") == "DSK shape center",
          "explicit target-to-camera origin");
  require(report.at("coordinate_contract")
              .at("camera_vector_direction") ==
              "DSK shape center to camera",
          "explicit target-to-camera direction");
  require(report.at("coordinate_contract").at("camera_vector_axes") ==
                  "J2000" &&
              report.at("coordinate_contract").at("length_unit") == "m" &&
              report.at("coordinate_contract").at("time_scale") == "ET/TDB",
          "v2 camera vector axes, length, and time");
  require(report.at("frame").at("target_to_camera_j2000_m") ==
                  json::array({0.0, 0.0, 20.0}) &&
              report.at("frame").count("camera_center_j2000_m") == 0,
          "v2 frame uses only direction-explicit camera vector field");

  json failingDocument = resolvedConfigV2(root / "missing-camera.tsai");
  const auto failingConfigPath = root / "resolved-v2-failing.json";
  writeText(failingConfigPath, failingDocument.dump(2));
  const auto failingOutput = root / "attempt-v2-failing";
  const RunResult failure =
      run({"--resolved-config",
           QString::fromStdString(failingConfigPath.string()), "--frame-id",
           "f0001", "--output-dir",
           QString::fromStdString(failingOutput.string())});
  require(failure.exitCode != 0, "v2 post-parse renderer failure");
  require(errorPayload(failure).at("schema_version") == 2 &&
              json::parse(readBytes(failingOutput / "failure.json"))
                      .at("schema_version") == 2,
          "parsed v2 version propagates to failure reports");
}

void testVersionTwoPointSunPublishesDiagnosticsAndAuditableReport() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary v2 point-sun CLI fixture");
  const std::filesystem::path root(temporary.path().toStdString());
  const auto camera = root / "camera.tsai";
  writeText(camera,
            "VERSION_4\nPINHOLE\nfu = 30\nfv = 30\ncu = 9\ncv = 8\n"
            "u_direction = 1 0 0\nv_direction = 0 1 0\n"
            "w_direction = 0 0 1\nC = 0 0 20\n"
            "R = 1 0 0 0 -1 0 0 0 -1\npitch = 1\nNULL\n");
  const auto configPath = root / "resolved-v2-point-sun.json";
  writeText(configPath, resolvedConfigV2PointSun(camera).dump(2));
  const auto output = root / "attempt-v2-point-sun";

  const RunResult success =
      run({"--resolved-config", QString::fromStdString(configPath.string()),
           "--frame-id", "f0001", "--output-dir",
           QString::fromStdString(output.string())});
  require(success.exitCode == 0, "point-sun CLI render succeeds: " +
                                     success.standardError);
  std::set<std::string> names;
  for (const auto &entry : std::filesystem::directory_iterator(output)) {
    names.insert(entry.path().filename().string());
  }
  require(names == std::set<std::string>{
                       "image_u16.tif", "mask_u8.tif", "depth_m_f32.tif",
                       "normal_camera_f32.tif", "facet_id_u32.tif",
                       "mu0_f32.tif", "illumination_mask_u8.tif",
                       "render.json"},
          "point-sun publishes seven TIFFs and render.json");
  requireTiffSize(output / "mu0_f32.tif");
  requireTiffSize(output / "illumination_mask_u8.tif");

  const json report = json::parse(readBytes(output / "render.json"));
  const json &photometry = report.at("photometry");
  require(photometry.at("model") == "point_sun_lambert" &&
              photometry.at("target_to_sun_j2000_m") ==
                  json::array({0.0, 0.0, 100.0}) &&
              photometry.at("target_to_sun_dsk_m") ==
                  json::array({0.0, 0.0, 100.0}) &&
              photometry.at("sun_distance_m") == 100.0 &&
              photometry.at("aberration_correction") == "NONE" &&
              photometry.at("state_source") == "illumination_csv" &&
              photometry.at("input_sha256") ==
                  json{{"dsk", std::string(64, 'a')},
                       {"camera", std::string(64, 'a')},
                       {"illumination_csv", std::string(64, 'c')}} &&
              photometry.at("cast_shadows") == true &&
              photometry.at("shadow_origin_offset_m") == 0.01 &&
              photometry.at("shadow_maximum_range_m") == 100.0,
          "point-sun report freezes geometry and shadow controls");
  const json &rawHashes = report.at("raw_product_sha256");
  require(rawHashes.size() == 7, "all seven raw TIFF hashes reported");
  for (const std::string name :
       {"image_u16.tif", "mask_u8.tif", "depth_m_f32.tif",
        "normal_camera_f32.tif", "facet_id_u32.tif", "mu0_f32.tif",
        "illumination_mask_u8.tif"}) {
    require(rawHashes.at(name).is_string() &&
                rawHashes.at(name).get<std::string>().size() == 64 &&
                rawHashes.at(name).get<std::string>().find_first_not_of(
                    "0123456789abcdef") == std::string::npos,
            "lowercase SHA-256 reported for " + name);
  }
  const auto direct = photometry.at("direct_pixels").get<std::uint64_t>();
  const auto night = photometry.at("night_pixels").get<std::uint64_t>();
  const auto shadow =
      photometry.at("self_shadow_pixels").get<std::uint64_t>();
  require(direct + night + shadow ==
              report.at("summary").at("valid_pixels").get<std::uint64_t>(),
          "illumination counts partition valid geometry");
}

void testVersionTwoPlateAlbedoPublishesLinearAppearanceAndAudit() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary v2 plate-albedo CLI fixture");
  const std::filesystem::path root(temporary.path().toStdString());
  const auto camera = root / "camera.tsai";
  writeText(camera,
            "VERSION_4\nPINHOLE\nfu = 30\nfv = 30\ncu = 9\ncv = 8\n"
            "u_direction = 1 0 0\nv_direction = 0 1 0\n"
            "w_direction = 0 0 1\nC = 0 0 20\n"
            "R = 1 0 0 0 -1 0 0 0 -1\npitch = 1\nNULL\n");
  const auto table = root / "plate-albedo.csv";
  writeText(table,
            "plate_id,relative_albedo_linear,valid,source_count,source_rms\n"
            "1,0.5,1,3,0.1\n2,0.75,1,4,0.2\n3,1.0,1,2,0.3\n");
  const auto uniformConfigPath = root / "resolved-v2-uniform.json";
  writeText(uniformConfigPath, resolvedConfigV2(camera).dump(2));
  const auto uniformOutput = root / "attempt-v2-uniform";
  const RunResult uniform =
      run({"--resolved-config", QString::fromStdString(uniformConfigPath.string()),
           "--frame-id", "f0001", "--output-dir",
           QString::fromStdString(uniformOutput.string())});
  require(uniform.exitCode == 0,
          "uniform reference CLI render succeeds: " + uniform.standardError);
  const auto configPath = root / "resolved-v2-plate-albedo.json";
  writeText(configPath, resolvedConfigV2PlateAlbedo(camera, table).dump(2));
  const auto output = root / "attempt-v2-plate-albedo";

  const RunResult success =
      run({"--resolved-config", QString::fromStdString(configPath.string()),
           "--frame-id", "f0001", "--output-dir",
           QString::fromStdString(output.string())});
  require(success.exitCode == 0,
          "plate-albedo CLI render succeeds: " + success.standardError);
  requireTiffSize(output / "albedo_f32.tif");

  bool sawDarkPlate = false;
  bool sawBrightPlate = false;
  for (int line = 0; line < 17; ++line) {
    for (int sample = 0; sample < 19; ++sample) {
      const float value = readFloatPixel(output / "albedo_f32.tif", sample,
                                         line);
      sawDarkPlate = sawDarkPlate || std::abs(value - 0.5F) < 1.0e-6F;
      sawBrightPlate = sawBrightPlate || std::abs(value - 0.75F) < 1.0e-6F;
    }
  }
  require(sawDarkPlate && sawBrightPlate,
          "plate 1 and plate 2 publish different linear appearances");

  std::array<double, 3> ratioSums{0.0, 0.0, 0.0};
  std::array<std::uint32_t, 3> ratioCounts{0U, 0U, 0U};
  for (int line = 0; line < 17; ++line) {
    for (int sample = 0; sample < 19; ++sample) {
      const std::uint32_t plateId =
          readUInt32Pixel(output / "facet_id_u32.tif", sample, line);
      if (plateId != 1U && plateId != 2U) {
        continue;
      }
      require(readUInt32Pixel(uniformOutput / "facet_id_u32.tif", sample,
                              line) == plateId,
              "uniform and plate-albedo runs retain facet identity");
      const std::uint16_t uniformDn =
          readUInt16Pixel(uniformOutput / "image_u16.tif", sample, line);
      const std::uint16_t plateDn =
          readUInt16Pixel(output / "image_u16.tif", sample, line);
      require(uniformDn > 0U && plateDn > 0U,
              "joint facet/DN samples are valid rendered hits");
      ratioSums[plateId] += static_cast<double>(plateDn) /
                            static_cast<double>(uniformDn);
      ++ratioCounts[plateId];
    }
  }
  require(ratioCounts[1] > 0U && ratioCounts[2] > 0U,
          "both CSV-controlled facets occur in the rendered DN grid");
  const double plateOneRatio = ratioSums[1] / ratioCounts[1];
  const double plateTwoRatio = ratioSums[2] / ratioCounts[2];
  requireNear(plateOneRatio, 0.5, 5.0e-5,
              "plate-one final DN ratio matches CSV relative albedo");
  requireNear(plateTwoRatio, 0.75, 5.0e-5,
              "plate-two final DN ratio matches CSV relative albedo");
  require(plateOneRatio < plateTwoRatio,
          "CSV relative-albedo ordering controls final DN direction");

  const json report = json::parse(readBytes(output / "render.json"));
  const json &appearance = report.at("surface_appearance");
  require(appearance.at("mode") == "plate_albedo" &&
              appearance.at("default_albedo") == 1.0 &&
              appearance.at("missing_plate_count") == 1 &&
              appearance.at("input_sha256") ==
                  "570d62a0681752690b21677bbdf99176d8299c10cf87892ea7ff2b52404e9fd5",
          "plate-albedo report freezes source and fallback statistics");
}

void testVersionTwoPointSunPlateAlbedoCorrelatesAllDiagnostics() {
  QTemporaryDir temporary;
  require(temporary.isValid(),
          "temporary point-sun plus plate-albedo CLI fixture");
  const std::filesystem::path root(temporary.path().toStdString());
  const auto camera = root / "camera.tsai";
  writeText(camera,
            "VERSION_4\nPINHOLE\nfu = 30\nfv = 30\ncu = 9\ncv = 8\n"
            "u_direction = 1 0 0\nv_direction = 0 1 0\n"
            "w_direction = 0 0 1\nC = 0 0 20\n"
            "R = 1 0 0 0 -1 0 0 0 -1\npitch = 1\nNULL\n");
  const auto table = root / "plate-albedo.csv";
  writeText(table,
            "plate_id,relative_albedo_linear,valid,source_count,source_rms\n"
            "1,0.5,1,3,0.1\n2,0.75,1,4,0.2\n3,1.0,1,2,0.3\n");
  const auto configPath = root / "resolved-v2-point-sun-plate-albedo.json";
  writeText(configPath,
            resolvedConfigV2PointSunPlateAlbedo(camera, table).dump(2));
  const auto output = root / "attempt-v2-point-sun-plate-albedo";

  const RunResult result =
      run({"--resolved-config", QString::fromStdString(configPath.string()),
           "--frame-id", "f0001", "--output-dir",
           QString::fromStdString(output.string())});
  require(result.exitCode == 0,
          "point-sun plus plate-albedo render succeeds: " +
              result.standardError);

  std::set<std::string> names;
  for (const auto &entry : std::filesystem::directory_iterator(output)) {
    names.insert(entry.path().filename().string());
  }
  require(names == std::set<std::string>{
                       "image_u16.tif", "mask_u8.tif", "depth_m_f32.tif",
                       "normal_camera_f32.tif", "facet_id_u32.tif",
                       "mu0_f32.tif", "illumination_mask_u8.tif",
                       "albedo_f32.tif", "render.json"},
          "combined native mode publishes eight correlated TIFFs and report");

  std::uint64_t checkedPixels = 0;
  for (int line = 0; line < 17; ++line) {
    for (int sample = 0; sample < 19; ++sample) {
      const std::uint32_t plateId =
          readUInt32Pixel(output / "facet_id_u32.tif", sample, line);
      if (plateId == 0U) continue;
      const double expectedAlbedo =
          plateId == 1U ? 0.5 : (plateId == 2U ? 0.75 : 1.0);
      const float actualAlbedo =
          readFloatPixel(output / "albedo_f32.tif", sample, line);
      const float mu0 = readFloatPixel(output / "mu0_f32.tif", sample, line);
      const std::uint8_t illumination =
          readSingleBandIntegerPixel<std::uint8_t>(
              output / "illumination_mask_u8.tif", sample, line, 8);
      const double visibility =
          illumination == 1U ? 1.0 : 0.0;
      const double normalized =
          0.75 * expectedAlbedo * (0.1 + 0.9 * mu0 * visibility);
      const std::uint16_t expectedDn = static_cast<std::uint16_t>(
          std::max<long long>(1, std::llround(
                                     65535.0 * std::clamp(normalized, 0.0, 1.0))));
      require(actualAlbedo == static_cast<float>(expectedAlbedo),
              "facet ID selects exact published relative albedo");
      require(readUInt16Pixel(output / "image_u16.tif", sample, line) ==
                  expectedDn,
              "DN exactly combines plate albedo, mu0, and shadow class");
      ++checkedPixels;
    }
  }
  require(checkedPixels > 0U,
          "combined diagnostics cover at least one valid DSK hit");
  const json report = json::parse(readBytes(output / "render.json"));
  require(report.at("photometry").at("model") == "point_sun_lambert" &&
              report.at("surface_appearance").at("mode") ==
                  "plate_albedo" &&
              report.at("raw_product_sha256").size() == 8,
          "combined report audits both public modes and every raw TIFF");
}

void testVersionTwoOpenCvRadtanUsesObservedGridAndReportsInverseAudit() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary OpenCV radtan CLI fixture");
  const std::filesystem::path root(temporary.path().toStdString());
  const auto nullCamera = root / "camera-null.tsai";
  writeText(nullCamera,
            "VERSION_4\nPINHOLE\nfu = 100\nfv = 100\ncu = 9\ncv = 8\n"
            "u_direction = 1 0 0\nv_direction = 0 1 0\n"
            "w_direction = 0 0 1\nC = 0 0 20\n"
            "R = 1 0 0 0 -1 0 0 0 -1\npitch = 1\nNULL\n");
  const auto nullConfigPath = root / "resolved-null-v2.json";
  writeText(nullConfigPath, resolvedConfigV2(nullCamera).dump(2));
  const auto nullOutput = root / "attempt-null-v2";
  const RunResult nullResult =
      run({"--resolved-config",
           QString::fromStdString(nullConfigPath.string()), "--frame-id",
           "f0001", "--output-dir",
           QString::fromStdString(nullOutput.string())});
  require(nullResult.exitCode == 0,
          "NULL camera reference render succeeds: " +
              nullResult.standardError);

  const auto opencvCamera = root / "camera-radtan.json";
  writeText(opencvCamera,
            R"({
  "schema_version": 1,
  "model": "opencv_radtan5",
  "pixel_coordinate": {
    "origin": "top_left",
    "index_base": 0,
    "integer_location": "pixel_center",
    "sample_axis": "right",
    "line_axis": "down"
  },
  "image_size_px": [19, 17],
  "intrinsics_px": {"fx": 100, "fy": 100, "cx": 9, "cy": 8, "skew": 0},
  "distortion": {
    "mapping": "ideal_to_observed",
    "coefficients": {"k1": 5, "k2": 0, "p1": 0, "p2": 0, "k3": 0}
  }
})");
  json radtanConfig = resolvedConfigV2(opencvCamera);
  radtanConfig["frames"][0]["camera_model_format"] =
      "opencv_radtan5_json";
  const auto radtanConfigPath = root / "resolved-radtan-v2.json";
  writeText(radtanConfigPath, radtanConfig.dump(2));
  const auto radtanOutput = root / "attempt-radtan-v2";
  const RunResult radtanResult =
      run({"--resolved-config",
           QString::fromStdString(radtanConfigPath.string()), "--frame-id",
           "f0001", "--output-dir",
           QString::fromStdString(radtanOutput.string())});
  require(radtanResult.exitCode == 0,
          "OpenCV radtan render succeeds: " +
              radtanResult.standardError);

  const float nullPrincipal =
      readDepthPixel(nullOutput / "depth_m_f32.tif", 9, 8);
  const float radtanPrincipal =
      readDepthPixel(radtanOutput / "depth_m_f32.tif", 9, 8);
  // For the fixed asymmetric tetrahedron, observed sample/line (5, 0) has
  // NULL-camera J2000 direction proportional to (-0.04, 0.08, -1). It
  // intersects fixed face [0, 3, 1] at barycentric u=0.529, v=0.463, so this
  // is a hand-derived top-left silhouette hit; literal (0, 0) misses the mesh.
  constexpr int cornerSample = 5;
  constexpr int cornerLine = 0;
  const float nullCorner = readDepthPixel(
      nullOutput / "depth_m_f32.tif", cornerSample, cornerLine);
  const float radtanCorner = readDepthPixel(
      radtanOutput / "depth_m_f32.tif", cornerSample, cornerLine);
  require(std::isfinite(nullPrincipal) && std::isfinite(radtanPrincipal) &&
              std::isfinite(nullCorner) && std::isfinite(radtanCorner),
          "principal and top-left corner-region rays hit the asymmetric DSK");
  require(std::abs(nullPrincipal - radtanPrincipal) <= 1.0e-6F,
          "principal ray is unchanged because normalized ideal coordinates are zero");
  require(std::abs(nullCorner - radtanCorner) > 1.0e-3F,
          "top-left corner-region ray differs on the observed distorted pixel "
          "grid: NULL=" + std::to_string(nullCorner) +
              " radtan=" + std::to_string(radtanCorner));

  const json report = json::parse(readBytes(radtanOutput / "render.json"));
  require(report["distortion"]["model"] == "opencv_radtan5",
          "model reported");
  require(report["distortion"]["mapping"] == "ideal_to_observed",
          "mapping reported");
  require(report["distortion"]["inverse"]["failed_pixel_count"] == 0,
          "no inverse failures");
  require(report["distortion"]["inverse"]["requested_pixel_count"] ==
              323 &&
              report["distortion"]["inverse"]["maximum_iterations"] <= 20 &&
              report["distortion"]["inverse"]["maximum_roundtrip_error_px"] <=
                  1.0e-6,
          "inverse audit freezes observed-pixel requests and controls");
}

void requirePublishedInverseExhaustion(const RunResult &result,
                                       const std::filesystem::path &output,
                                       const std::string &cameraFormat) {
  const json standardError = errorPayload(result);
  const json published =
      json::parse(readBytes(output / "failure.json"));
  require(result.exitCode != 0 &&
              standardError.at("category") == "DISTORTION_INVERSE_ERROR" &&
              published == standardError,
          cameraFormat +
              " exhausted inverse writes the complete published failure");
  const std::string message = published.at("message").get<std::string>();
  for (const char *context :
       {"samplePx=0", "linePx=0", "configuredMaximumIterations=1",
        "Newton inverse exhausted: maximumIterations=1", "roundTripErrorPx="}) {
    require(message.find(context) != std::string::npos,
            cameraFormat + " inverse exhaustion reports " + context +
                ": " + message);
  }
}

void testVersionTwoDistortedCamerasPublishInverseExhaustionContext() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary distortion exhaustion CLI fixture");
  const std::filesystem::path root(temporary.path().toStdString());
  const auto opencvCamera = root / "camera-radtan.json";
  writeText(opencvCamera,
            R"({
  "schema_version": 1,
  "model": "opencv_radtan5",
  "pixel_coordinate": {
    "origin": "top_left",
    "index_base": 0,
    "integer_location": "pixel_center",
    "sample_axis": "right",
    "line_axis": "down"
  },
  "image_size_px": [19, 17],
  "intrinsics_px": {"fx": 100, "fy": 100, "cx": 9, "cy": 8, "skew": 0},
  "distortion": {
    "mapping": "ideal_to_observed",
    "coefficients": {"k1": 5, "k2": 0, "p1": 0, "p2": 0, "k3": 0}
  }
})");
  const auto tsai5Camera = root / "camera-tsai5.tsai";
  writeText(tsai5Camera,
            "VERSION_4\nPINHOLE\nfu = 100\nfv = 100\ncu = 9\ncv = 8\n"
            "u_direction = 1 0 0\nv_direction = 0 1 0\n"
            "w_direction = 0 0 1\nC = 0 0 20\n"
            "R = 1 0 0 0 -1 0 0 0 -1\npitch = 1\n"
            "TSAI\n5\n0\n0\n0\n0\n");

  for (const auto &[cameraPath, cameraFormat] :
       {std::pair{opencvCamera, std::string("opencv_radtan5_json")},
        std::pair{tsai5Camera, std::string("asp_tsai5")}}) {
    json config = resolvedConfigV2(cameraPath);
    config["frames"][0]["camera_model_format"] = cameraFormat;
    config["render"]["camera_distortion"]["inverse_max_iterations"] = 1;
    config["render"]["camera_distortion"]["roundtrip_tolerance_px"] =
        1.0e-30;
    const auto configPath = root / (cameraFormat + ".json");
    const auto output = root / ("attempt-" + cameraFormat);
    writeText(configPath, config.dump(2));
    const RunResult result =
        run({"--resolved-config", QString::fromStdString(configPath.string()),
             "--frame-id", "f0001", "--output-dir",
             QString::fromStdString(output.string())});
    requirePublishedInverseExhaustion(result, output, cameraFormat);
  }
}

std::string cameraModelFailureProblem(
    const RunResult &result, const std::filesystem::path &output,
    const std::string &context,
    const std::vector<std::string> &messageFragments) {
  if (result.exitCode == 0) {
    return context + " unexpectedly succeeded";
  }
  try {
    const json standardError = json::parse(result.standardError);
    if (!std::filesystem::exists(output / "failure.json")) {
      return context + " did not publish failure.json";
    }
    const json published = json::parse(readBytes(output / "failure.json"));
    if (standardError.at("category") != "CAMERA_MODEL_ERROR" ||
        published != standardError ||
        std::filesystem::exists(output / "render.json") ||
        std::filesystem::exists(output / "image_u16.tif")) {
      return context + " published category " +
             standardError.at("category").get<std::string>();
    }
    const std::string message =
        standardError.at("message").get<std::string>();
    for (const std::string &fragment : messageFragments) {
      if (message.find(fragment) == std::string::npos) {
        return context + " diagnostic omitted " + fragment + ": " + message;
      }
    }
  }
  catch (const std::exception &error) {
    return context + " failure payload was invalid: " + error.what();
  }
  return {};
}

void testVersionTwoCameraModelFailuresPreserveStableCategory() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary camera model error fixture");
  const std::filesystem::path root(temporary.path().toStdString());
  const auto nullCamera = root / "actual-null.tsai";
  const auto tsai5Camera = root / "actual-tsai5.tsai";
  const auto opencvCamera = root / "malformed.json";
  const auto tsaiCamera = root / "malformed.tsai";
  writeText(nullCamera, canonicalTsai("NULL\n"));
  writeText(tsai5Camera, canonicalTsai("TSAI\n0.1\n0\n0\n0\n0\n"));
  writeText(opencvCamera, "{ not-json");
  std::string malformedTsai = canonicalTsai("NULL\n");
  malformedTsai.replace(malformedTsai.find("fu = 100"),
                        std::string("fu = 100").size(), "fu = invalid");
  writeText(tsaiCamera, malformedTsai);

  std::string problems;
  for (const auto &[cameraPath, declaredFormat, expectedFragments] :
       {std::tuple<std::filesystem::path, std::string,
                   std::vector<std::string>>{
            nullCamera, "asp_tsai5",
            {"CAMERA_MODEL_ERROR", "camera_model_format", "asp_tsai5",
             "NULL"}},
        {tsai5Camera, "asp_tsai_null",
         {"CAMERA_MODEL_ERROR", "camera_model_format", "asp_tsai_null",
          "TSAI"}},
        {opencvCamera, "opencv_radtan5_json",
         {"CAMERA_MODEL_ERROR", "invalid JSON"}},
        {tsaiCamera, "asp_tsai_null",
         {"CAMERA_MODEL_ERROR", "fu"}}}) {
    json config = resolvedConfigV2(cameraPath);
    config["frames"][0]["camera_model_format"] = declaredFormat;
    const auto configPath =
        root / ("resolved-camera-error-" + declaredFormat + "-" +
                cameraPath.filename().string() + ".json");
    const auto output =
        root / ("attempt-camera-error-" + declaredFormat + "-" +
                cameraPath.filename().string());
    writeText(configPath, config.dump(2));

    const RunResult result =
        run({"--resolved-config", QString::fromStdString(configPath.string()),
             "--frame-id", "f0001", "--output-dir",
             QString::fromStdString(output.string())});
    const std::string problem = cameraModelFailureProblem(
        result, output, declaredFormat + "/" + cameraPath.filename().string(),
        expectedFragments);
    if (!problem.empty()) {
      problems += problem + "; ";
    }
  }
  require(problems.empty(),
          "both TSAI mismatches and both malformed readers preserve "
          "CAMERA_MODEL_ERROR: " +
              problems);
}

void testVersionTwoNonidentityBrightnessControlsAreRejectedBeforeRendering() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary v2 identity-control fixture");
  const std::filesystem::path root(temporary.path().toStdString());
  const auto camera = root / "camera.tsai";
  writeText(camera,
            "VERSION_4\nPINHOLE\nfu = 30\nfv = 30\ncu = 9\ncv = 8\n"
            "u_direction = 1 0 0\nv_direction = 0 1 0\n"
            "w_direction = 0 0 1\nC = 0 0 20\n"
            "R = 1 0 0 0 -1 0 0 0 -1\npitch = 1\nNULL\n");

  for (const std::string field : {"exposure_scale", "default_albedo"}) {
    json document = resolvedConfigV2(camera);
    if (field == "exposure_scale") {
      document["render"]["exposure_scale"] = 2.0;
    }
    else {
      document["render"]["surface_appearance"]["default_albedo"] = 2.0;
    }
    const auto configPath = root / ("resolved-v2-" + field + ".json");
    const auto output = root / ("attempt-v2-" + field);
    writeText(configPath, document.dump(2));

    const RunResult result =
        run({"--resolved-config", QString::fromStdString(configPath.string()),
             "--frame-id", "f0001", "--output-dir",
             QString::fromStdString(output.string())});
    const json error = errorPayload(result);
    require(result.exitCode != 0 && error.at("category") == "RENDER_ERROR" &&
                error.at("message").get<std::string>().find(field) !=
                    std::string::npos &&
                error.at("message").get<std::string>().find("non-identity") !=
                    std::string::npos &&
                !std::filesystem::exists(output / "render.json") &&
                !std::filesystem::exists(output / "image_u16.tif"),
            "native parser rejects non-identity " + field +
                " before rendering products");
  }
}

void testStableArgumentAndDirectoryErrors() {
  const RunResult malformed = run({"--unknown", "value"});
  require(malformed.exitCode != 0 &&
              errorPayload(malformed).at("schema_version") == 1 &&
              errorPayload(malformed).at("category") == "ARGUMENT_ERROR",
          "malformed arguments have stable category");

  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary directory error fixture");
  const std::filesystem::path root(temporary.path().toStdString());
  const RunResult missingParent =
      run({"--resolved-config", "/missing/config.json", "--frame-id", "f1",
           "--output-dir",
           QString::fromStdString((root / "missing-parent" / "attempt").string())});
  require(missingParent.exitCode != 0 &&
              errorPayload(missingParent).at("category") ==
                  "OUTPUT_DIRECTORY_ERROR",
          "output must be one new directory under an existing parent");
}

}  // namespace

int main() {
  testSuccessfulReportAndNoReplaceOutput();
  testVersionTwoReportAndParsedFailureUseVersionTwo();
  testVersionTwoPointSunPublishesDiagnosticsAndAuditableReport();
  testVersionTwoPlateAlbedoPublishesLinearAppearanceAndAudit();
  testVersionTwoPointSunPlateAlbedoCorrelatesAllDiagnostics();
  testVersionTwoOpenCvRadtanUsesObservedGridAndReportsInverseAudit();
  testVersionTwoCameraModelFailuresPreserveStableCategory();
  testVersionTwoDistortedCamerasPublishInverseExhaustionContext();
  testVersionTwoNonidentityBrightnessControlsAreRejectedBeforeRendering();
  testStableArgumentAndDirectoryErrors();
  std::cout << "PASS: native small-body DSK renderer CLI tests\n";
  return EXIT_SUCCESS;
}
