// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tuple>

#include "OpenCvRadtanCamera.h"
#include "RenderCameraFactory.h"

namespace {

using Isis::SmallBodyRender::CameraModelFormat;
using Isis::SmallBodyRender::DistortionInverseControls;
using Isis::SmallBodyRender::FrameState;
using Isis::SmallBodyRender::Matrix3;
using Isis::SmallBodyRender::OpenCvRadtanCamera;
using Isis::SmallBodyRender::QuaternionWxyz;
using Isis::SmallBodyRender::Vector3;
using Isis::SmallBodyRender::loadRenderCamera;

void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void requireVectorNear(const Vector3 &actual, const Vector3 &expected,
                       double tolerance, const std::string &message) {
  require((actual - expected).norm() <= tolerance,
          message + ": norm error " +
              std::to_string((actual - expected).norm()));
}

class TemporaryJson final {
  public:
    explicit TemporaryJson(const std::string &contents) {
      static std::size_t sequence = 0;
      path_ = std::filesystem::temp_directory_path() /
              ("smallbody-opencv-radtan-" + std::to_string(++sequence) +
               ".json");
      std::ofstream stream(path_);
      require(static_cast<bool>(stream), "open temporary OpenCV JSON");
      stream << contents;
      require(static_cast<bool>(stream), "write temporary OpenCV JSON");
    }

    ~TemporaryJson() {
      std::error_code error;
      std::filesystem::remove(path_, error);
    }

    const std::filesystem::path &path() const { return path_; }

  private:
    std::filesystem::path path_;
};

std::string canonicalOpenCvJson() {
  return R"({
  "schema_version": 1,
  "model": "opencv_radtan5",
  "pixel_coordinate": {
    "origin": "top_left",
    "index_base": 0,
    "integer_location": "pixel_center",
    "sample_axis": "right",
    "line_axis": "down"
  },
  "image_size_px": [5000, 3000],
  "intrinsics_px": {
    "fx": 1000.0,
    "fy": 1000.0,
    "cx": 0.0,
    "cy": 0.0,
    "skew": 0.0
  },
  "distortion": {
    "mapping": "ideal_to_observed",
    "coefficients": {
      "k1": 0.1,
      "k2": 0.0,
      "p1": 0.0,
      "p2": 0.0,
      "k3": 0.0
    }
  }
})";
}

std::string canonicalTsai(const std::string &terminal) {
  return "VERSION_4\n"
         "PINHOLE\n"
         "fu = 1000\n"
         "fv = 1000\n"
         "cu = 0\n"
         "cv = 0\n"
         "u_direction = 1 0 0\n"
         "v_direction = 0 1 0\n"
         "w_direction = 0 0 1\n"
         "C = 10 20 30\n"
         "R = 1 0 0 0 1 0 0 0 1\n"
         "pitch = 1\n" +
         terminal;
}

FrameState matchingFrame(const std::filesystem::path &cameraPath) {
  return {"f0001",
          123456.25,
          cameraPath,
          std::string(64, 'a'),
          5000,
          3000,
          CameraModelFormat::OpenCvRadtan5Json,
          Vector3(10.0, 20.0, 30.0),
          std::nullopt,
          QuaternionWxyz{1.0, 0.0, 0.0, 0.0},
          QuaternionWxyz{1.0, 0.0, 0.0, 0.0},
          Matrix3::Identity(),
          Matrix3::Identity()};
}

FrameState matchingTsaiFrame(const std::filesystem::path &cameraPath,
                             CameraModelFormat format) {
  FrameState frame = matchingFrame(cameraPath);
  frame.cameraModelFormat = format;
  frame.targetToCameraJ2000M = Vector3(10.0, 20.0, 30.0);
  return frame;
}

template <typename Callable>
void requireCameraModelRejects(Callable callable, const std::string &field) {
  try {
    callable();
  }
  catch (const std::exception &error) {
    const std::string message = error.what();
    require(message.find("CAMERA_MODEL_ERROR") != std::string::npos,
            "diagnostic must contain CAMERA_MODEL_ERROR: " + message);
    require(message.find(field) != std::string::npos,
            "diagnostic must contain '" + field + "': " + message);
    return;
  }
  require(false, "expected CAMERA_MODEL_ERROR for " + field);
}

void testCanonicalRayAndFactory() {
  const TemporaryJson file(canonicalOpenCvJson());
  const FrameState frame = matchingFrame(file.path());
  const DistortionInverseControls controls{20, 1.0e-6};
  const Vector3 expected = Vector3(0.1, 0.2, 1.0).normalized();

  const OpenCvRadtanCamera camera =
      OpenCvRadtanCamera::read(file.path(), frame, controls);
  requireVectorNear(camera.rayCamera(100.5, 201.0), expected, 1.0e-12,
                    "OpenCV radtan5 ray undistorts observed pixel");

  const auto factoryCamera = loadRenderCamera(frame, controls);
  requireVectorNear(factoryCamera->rayCamera(100.5, 201.0), expected,
                    1.0e-12, "factory dispatches OpenCV radtan5 camera");
}

void testRejectsInvalidSchemaAndFrameWithNamedCameraModelErrors() {
  const std::string canonical = canonicalOpenCvJson();
  for (const auto &[from, replacement, field] :
       {std::tuple<std::string, std::string, std::string>{
            "\"opencv_radtan5\"", "\"other_model\"", "model"},
        {"\"top_left\"", "\"bottom_left\"", "origin"},
        {"\"index_base\": 0", "\"index_base\": 1", "index_base"},
        {"\"pixel_center\"", "\"pixel_corner\"", "integer_location"},
        {"\"skew\": 0.0", "\"skew\": 1.0", "skew"},
        {"\"ideal_to_observed\"", "\"observed_to_ideal\"", "mapping"}}) {
    std::string malformed = canonical;
    malformed.replace(malformed.find(from), from.size(), replacement);
    const TemporaryJson file(malformed);
    const FrameState frame = matchingFrame(file.path());
    requireCameraModelRejects(
        [&] { OpenCvRadtanCamera::read(file.path(), frame); }, field);
  }

  std::string unknown = canonical;
  unknown.insert(unknown.find("\"model\""), "\"unknown\": true,\n  ");
  const TemporaryJson unknownFile(unknown);
  const FrameState unknownFrame = matchingFrame(unknownFile.path());
  requireCameraModelRejects(
      [&] { OpenCvRadtanCamera::read(unknownFile.path(), unknownFrame); },
      "unknown");

  std::string missingK3 = canonical;
  missingK3.erase(missingK3.find("      \"k3\": 0.0\n"),
                  std::string("      \"k3\": 0.0\n").size());
  missingK3.replace(missingK3.find("      \"p2\": 0.0,\n"),
                    std::string("      \"p2\": 0.0,\n").size(),
                    "      \"p2\": 0.0\n");
  const TemporaryJson missingK3File(missingK3);
  const FrameState missingK3Frame = matchingFrame(missingK3File.path());
  requireCameraModelRejects(
      [&] { OpenCvRadtanCamera::read(missingK3File.path(), missingK3Frame); },
      "k3");

  const TemporaryJson imageSizeFile(canonical);
  FrameState mismatchedFrame = matchingFrame(imageSizeFile.path());
  mismatchedFrame.widthPx = 4999;
  requireCameraModelRejects(
      [&] { OpenCvRadtanCamera::read(imageSizeFile.path(), mismatchedFrame); },
      "image_size_px");
}

void testFactoryRejectsDeclaredTsaiFormatMismatches() {
  std::string failures;
  for (const auto &[contents, declaredFormat, declaredName, actualName] :
       {std::tuple<std::string, CameraModelFormat, std::string, std::string>{
            canonicalTsai("NULL\n"), CameraModelFormat::AspTsai5,
            "asp_tsai5", "NULL"},
        {canonicalTsai("TSAI\n0.1\n0\n0\n0\n0\n"),
         CameraModelFormat::AspTsaiNull, "asp_tsai_null", "TSAI"}}) {
    const TemporaryJson file(contents);
    const FrameState frame = matchingTsaiFrame(file.path(), declaredFormat);
    try {
      static_cast<void>(loadRenderCamera(frame));
      failures += declaredName + "/" + actualName +
                  " was accepted; ";
    }
    catch (const std::exception &error) {
      const std::string message = error.what();
      if (message.find("CAMERA_MODEL_ERROR") == std::string::npos ||
          message.find("camera_model_format") == std::string::npos ||
          message.find(declaredName) == std::string::npos ||
          message.find(actualName) == std::string::npos) {
        failures += declaredName + "/" + actualName +
                    " had incomplete diagnostic: " + message + "; ";
      }
    }
  }
  require(failures.empty(),
          "factory rejects both declared TSAI format mismatches: " + failures);
}

}  // namespace

int main() {
  testCanonicalRayAndFactory();
  testRejectsInvalidSchemaAndFrameWithNamedCameraModelErrors();
  testFactoryRejectsDeclaredTsaiFormatMismatches();
  std::cout << "PASS: strict OpenCV radtan5 camera tests\n";
  return EXIT_SUCCESS;
}
