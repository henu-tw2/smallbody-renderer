// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <string>
#include <tuple>

#include "TsaiPinholeCamera.h"

namespace {

using Isis::SmallBodyRender::FrameState;
using Isis::SmallBodyRender::DistortionInverseControls;
using Isis::SmallBodyRender::Matrix3;
using Isis::SmallBodyRender::QuaternionWxyz;
using Isis::SmallBodyRender::TsaiPinholeCamera;
using Isis::SmallBodyRender::Vector3;

class CommaDecimalPunct final : public std::numpunct<char> {
  protected:
    char do_decimal_point() const override {
      return ',';
    }
};

class ScopedCommaDecimalLocale final {
  public:
    ScopedCommaDecimalLocale() : original_(std::locale()) {
      std::locale::global(std::locale(original_, new CommaDecimalPunct));
    }

    ~ScopedCommaDecimalLocale() noexcept {
      try {
        std::locale::global(original_);
      }
      catch (...) {
        std::terminate();
      }
    }

    ScopedCommaDecimalLocale(const ScopedCommaDecimalLocale &) = delete;
    ScopedCommaDecimalLocale &operator=(const ScopedCommaDecimalLocale &) =
        delete;

  private:
    std::locale original_;
};

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

void requireVectorNear(const Vector3 &actual, const Vector3 &expected,
                       double tolerance, const std::string &message) {
  require((actual - expected).norm() <= tolerance,
          message + ": norm error " +
              std::to_string((actual - expected).norm()));
}

class TemporaryTsai {
  public:
    explicit TemporaryTsai(const std::string &contents) {
      static std::size_t sequence = 0;
      path_ = std::filesystem::temp_directory_path() /
              ("smallbody-render-camera-" + std::to_string(++sequence) +
               ".tsai");
      std::ofstream stream(path_);
      require(static_cast<bool>(stream), "open temporary TSAI");
      stream << contents;
      require(static_cast<bool>(stream), "write temporary TSAI");
    }

    ~TemporaryTsai() {
      std::error_code error;
      std::filesystem::remove(path_, error);
    }

    const std::filesystem::path &path() const {
      return path_;
    }

  private:
    std::filesystem::path path_;
};

std::string canonicalTsai() {
  return "VERSION_4\n"
         "PINHOLE\n"
         "fu = 1000\n"
         "fv = 900\n"
         "cu = 4\n"
         "cv = 3\n"
         "u_direction = 1 0 0\n"
         "v_direction = 0 1 0\n"
         "w_direction = 0 0 1\n"
         "C = 10 20 30\n"
         "R = 0 -1 0 1 0 0 0 0 1\n"
         "pitch = 1\n"
         "NULL\n";
}

std::string canonicalTsai5() {
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
         "R = 0 -1 0 1 0 0 0 0 1\n"
         "pitch = 1\n"
         "TSAI\n"
         "0.1\n"
         "0\n"
         "0\n"
         "0\n"
         "0\n";
}

std::string canonicalDecimalTsai5() {
  return "VERSION_4\n"
         "PINHOLE\n"
         "fu = 1000.5\n"
         "fv = 1000.25\n"
         "cu = 0.5\n"
         "cv = 0.25\n"
         "u_direction = 1 0 0\n"
         "v_direction = 0 1 0\n"
         "w_direction = 0 0 1\n"
         "C = 10 20 30\n"
         "R = 0 -1 0 1 0 0 0 0 1\n"
         "pitch = 1\n"
         "TSAI\n"
         "0.1\n"
         "0.01\n"
         "0.001\n"
         "0.0001\n"
         "0.00001\n";
}

std::string secondFrameTsai() {
  return "VERSION_4\n"
         "PINHOLE\n"
         "fu = 9269.2307692307695\n"
         "fv = 9269.2307692307695\n"
         "cu = 522.89999999999998\n"
         "cv = 489.5\n"
         "u_direction = 1 0 0\n"
         "v_direction = 0 1 0\n"
         "w_direction = 0 0 1\n"
         "C = 11912.679612283 -15043.128449123 -5478.456100328\n"
         "R = -0.80681368754626126 -0.54739805323303914 "
         "-0.22227695540627379 -0.018125225331254102 "
         "-0.35311608436010516 0.93540392728108546 "
         "-0.59052785691675469 0.75872551181759129 "
         "0.27497717709373454\n"
         "pitch = 1\n"
         "NULL\n";
}

template <typename Callable>
void requireRejects(Callable callable, const std::string &field) {
  try {
    callable();
  }
  catch (const std::exception &error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "diagnostic must contain '" + field + "': " + error.what());
    return;
  }
  require(false, "expected rejection for " + field);
}

template <typename Callable>
std::string rejectionMessage(Callable callable) {
  try {
    callable();
  }
  catch (const std::exception &error) {
    return error.what();
  }
  require(false, "expected rejection");
  return {};
}

FrameState matchingFrame() {
  Matrix3 rotationJ2000FromCamera;
  rotationJ2000FromCamera << 0.0, 1.0, 0.0, -1.0, 0.0, 0.0, 0.0, 0.0,
      1.0;
  return {"f0001",
          123456.25,
          "/input/f0001.tsai",
          std::string(64, 'a'),
          5000,
          3000,
          Isis::SmallBodyRender::CameraModelFormat::AspTsaiNull,
          Vector3(10.0, 20.0, 30.0),
          std::nullopt,
          QuaternionWxyz{1.0, 0.0, 0.0, 0.0},
          QuaternionWxyz{std::sqrt(0.5), 0.0, 0.0, -std::sqrt(0.5)},
          Matrix3::Identity(),
          rotationJ2000FromCamera};
}

void testCanonicalRaysProjectionAndPoseDirection() {
  const TemporaryTsai file(canonicalTsai());
  const auto camera = TsaiPinholeCamera::readCanonical(file.path());

  requireNear(camera.fu(), 1000.0, 0.0, "fu");
  requireNear(camera.fv(), 900.0, 0.0, "fv");
  requireNear(camera.cu(), 4.0, 0.0, "cu");
  requireNear(camera.cv(), 3.0, 0.0, "cv");
  requireVectorNear(camera.rayCamera(4.0, 3.0), Vector3(0.0, 0.0, 1.0),
                    1.0e-15, "principal-point ray");
  const Vector3 halfPixelExpected =
      Vector3(0.0005, 0.5 / 900.0, 1.0).normalized();
  requireVectorNear(camera.rayCamera(4.5, 3.5), halfPixelExpected, 1.0e-15,
                    "sample and line are used directly without hidden offset");

  const Vector3 pointJ =
      camera.centerJ2000M() +
      camera.rotationCameraFromJ2000().transpose() *
          Vector3(2.0, -1.0, 10.0);
  const Eigen::Vector2d pixel = camera.projectJ2000(pointJ);
  requireNear(pixel.x(), 204.0, 1.0e-12, "known point sample");
  requireNear(pixel.y(), -87.0, 1.0e-12, "known point line");
  camera.requireMatches(matchingFrame());
}

void testTsai5RaysProjectAndRejectInvalidTerminatorParameters() {
  const TemporaryTsai file(canonicalTsai5());
  const auto camera = TsaiPinholeCamera::readCanonical(
      file.path(), DistortionInverseControls{20, 1.0e-6});

  requireVectorNear(camera.rayCamera(100.5, 201.0),
                    Vector3(0.1, 0.2, 1.0).normalized(), 1.0e-12,
                    "TSAI5 ray undistorts observed sample and line");
  const Vector3 pointJ =
      camera.centerJ2000M() +
      camera.rotationCameraFromJ2000().transpose() *
          Vector3(0.1, 0.2, 1.0);
  const Eigen::Vector2d observed = camera.projectJ2000(pointJ);
  requireNear(observed.x(), 100.5, 1.0e-10,
              "TSAI5 projection distorts ideal sample");
  requireNear(observed.y(), 201.0, 1.0e-10,
              "TSAI5 projection distorts ideal line");

  for (const auto &[suffix, field] :
       {std::pair<std::string, std::string>{"TSAI\n0.1\n0\n0\n0\n",
                                             "TSAI"},
        {"TSAI\n0.1\n0\n0\n0\n0\n0\n", "TSAI"},
        {"TSAI\nnan\n0\n0\n0\n0\n", "TSAI"},
        {"UNKNOWN\n", "TSAI"}}) {
    std::string malformed = canonicalTsai5();
    malformed.replace(malformed.find("TSAI\n"),
                      std::string("TSAI\n0.1\n0\n0\n0\n0\n").size(),
                      suffix);
    const TemporaryTsai malformedFile(malformed);
    requireRejects(
        [&] { TsaiPinholeCamera::readCanonical(malformedFile.path()); },
        field);
  }
}

void testNumericParserUsesClassicLocale() {
  const ScopedCommaDecimalLocale commaDecimalLocale;
  const TemporaryTsai canonicalFile(canonicalDecimalTsai5());
  const auto camera = TsaiPinholeCamera::readCanonical(canonicalFile.path());
  requireNear(camera.fu(), 1000.5, 0.0,
              "canonical dot-decimal focal sample parses under comma locale");
  requireNear(camera.fv(), 1000.25, 0.0,
              "canonical dot-decimal focal line parses under comma locale");
  requireNear(camera.cu(), 0.5, 0.0,
              "canonical dot-decimal principal sample parses under comma locale");
  requireNear(camera.cv(), 0.25, 0.0,
              "canonical dot-decimal principal line parses under comma locale");

  std::string commaIntrinsic = canonicalDecimalTsai5();
  commaIntrinsic.replace(commaIntrinsic.find("fu = 1000.5"),
                         std::string("fu = 1000.5").size(), "fu = 1000,5");
  const TemporaryTsai commaIntrinsicFile(commaIntrinsic);
  requireRejects(
      [&] { TsaiPinholeCamera::readCanonical(commaIntrinsicFile.path()); },
      "fu");

  std::string commaCoefficient = canonicalDecimalTsai5();
  commaCoefficient.replace(commaCoefficient.find("0.1\n"),
                           std::string("0.1").size(), "0,1");
  const TemporaryTsai commaCoefficientFile(commaCoefficient);
  requireRejects(
      [&] { TsaiPinholeCamera::readCanonical(commaCoefficientFile.path()); },
      "TSAI");
}

void testRejectsNoncanonicalStructureAndAssignments() {
  for (const auto &[from, field] :
       {std::pair<std::string, std::string>{"VERSION_4", "VERSION_4"},
        {"PINHOLE", "PINHOLE"}, {"NULL", "NULL"}}) {
    std::string malformed = canonicalTsai();
    malformed.replace(malformed.find(from), from.size(), "BAD");
    const TemporaryTsai file(malformed);
    requireRejects([&] { TsaiPinholeCamera::readCanonical(file.path()); },
                   field);
  }

  std::string missing = canonicalTsai();
  missing.erase(missing.find("cv = 3\n"), std::string("cv = 3\n").size());
  const TemporaryTsai missingFile(missing);
  requireRejects(
      [&] { TsaiPinholeCamera::readCanonical(missingFile.path()); }, "cv");

  std::string duplicate = canonicalTsai();
  duplicate.insert(duplicate.find("fv ="), "fu = 1000\n");
  const TemporaryTsai duplicateFile(duplicate);
  requireRejects(
      [&] { TsaiPinholeCamera::readCanonical(duplicateFile.path()); }, "fu");

  std::string unknown = canonicalTsai();
  unknown.insert(unknown.find("NULL"), "extra = 1\n");
  const TemporaryTsai unknownFile(unknown);
  requireRejects(
      [&] { TsaiPinholeCamera::readCanonical(unknownFile.path()); }, "extra");
}

void testRejectsInvalidCameraGeometry() {
  for (const auto &[from, replacement, field] :
       {std::tuple<std::string, std::string, std::string>{
            "fu = 1000", "fu = 0", "fu"},
        {"pitch = 1", "pitch = 2", "pitch"},
        {"u_direction = 1 0 0", "u_direction = 0 1 0", "u_direction"},
        {"v_direction = 0 1 0", "v_direction = 0 0 1", "v_direction"},
        {"w_direction = 0 0 1", "w_direction = 1 0 0", "w_direction"},
        {"R = 0 -1 0 1 0 0 0 0 1", "R = 1 0 0 0 1 0 0 1 0", "R"}}) {
    std::string malformed = canonicalTsai();
    malformed.replace(malformed.find(from), from.size(), replacement);
    const TemporaryTsai file(malformed);
    requireRejects([&] { TsaiPinholeCamera::readCanonical(file.path()); },
                   field);
  }
}

void testRejectsPoseDisagreementAndPointsBehindCamera() {
  const TemporaryTsai file(canonicalTsai());
  const auto camera = TsaiPinholeCamera::readCanonical(file.path());

  FrameState frame = matchingFrame();
  frame.rotationJ2000FromCamera = Matrix3::Identity();
  requireRejects([&] { camera.requireMatches(frame); },
                 "q_camera_to_j2000_wxyz");

  requireRejects(
      [&] {
        camera.projectJ2000(camera.centerJ2000M() +
                            Vector3(0.0, 0.0, -1.0));
      },
      "depth");
}

void testPoseDisagreementNamesBothVersionedCameraFields() {
  const TemporaryTsai file(canonicalTsai());
  const auto camera = TsaiPinholeCamera::readCanonical(file.path());
  FrameState frame = matchingFrame();
  frame.targetToCameraJ2000M.x() += 1.0e-3;

  const std::string message =
      rejectionMessage([&] { camera.requireMatches(frame); });
  require(message.find("target_to_camera_j2000_m") != std::string::npos,
          "V2 pose diagnostic names target_to_camera_j2000_m");
  require(message.find("camera_center_j2000_m") != std::string::npos,
          "V1 pose diagnostic retains camera_center_j2000_m");
}

void testNearIdenticalPoseDoesNotBecomeFalseAngularError() {
  const TemporaryTsai file(secondFrameTsai());
  const auto camera = TsaiPinholeCamera::readCanonical(file.path());
  FrameState frame{
      "074940",
      584481049.89122,
      file.path(),
      std::string(64, 'a'),
      1024,
      1024,
      Isis::SmallBodyRender::CameraModelFormat::AspTsaiNull,
      Vector3(11912.679612283, -15043.128449123, -5478.456100328),
      std::nullopt,
      QuaternionWxyz{1.0, 0.0, 0.0, 0.0},
      QuaternionWxyz{0.16959319354514793, 0.26044443731826439,
                     -0.54284445886745991, -0.78020941884216266},
      Matrix3::Identity(),
      Matrix3::Identity()};
  frame.rotationJ2000FromCamera =
      Isis::SmallBodyRender::activeRotationFromWxyz(
          frame.cameraToJ2000Quaternion);
  camera.requireMatches(frame);
}

}  // namespace

int main() {
  testCanonicalRaysProjectionAndPoseDirection();
  testTsai5RaysProjectAndRejectInvalidTerminatorParameters();
  testNumericParserUsesClassicLocale();
  testRejectsNoncanonicalStructureAndAssignments();
  testRejectsInvalidCameraGeometry();
  testRejectsPoseDisagreementAndPointsBehindCamera();
  testPoseDisagreementNamesBothVersionedCameraFields();
  testNearIdenticalPoseDoesNotBecomeFalseAngularError();
  std::cout << "PASS: canonical TSAI camera tests\n";
  return EXIT_SUCCESS;
}
