// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "LambertHeadlight.h"

namespace {

using Isis::SmallBodyRender::Matrix3;
using Isis::SmallBodyRender::Photometry;
using Isis::SmallBodyRender::Vector3;
using Isis::SmallBodyRender::lambertHeadlightDn;
using Isis::SmallBodyRender::normalBodyToCamera;

void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void requireVectorNear(const Vector3 &actual, const Vector3 &expected,
                       double tolerance, const std::string &message) {
  require(actual.allFinite() && (actual - expected).norm() <= tolerance,
          message + ": norm error " +
              std::to_string((actual - expected).norm()));
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

void testDeterministicLambertDn() {
  const Photometry model{0.10, 0.90};
  require(lambertHeadlightDn({0.0, 0.0, 1.0}, {0.0, 0.0, 0.0},
                             {0.0, 0.0, 10.0}, model) == 65535,
          "front-facing DN");
  require(
      lambertHeadlightDn({0.0, std::sqrt(3.0) / 2.0, 0.5},
                         {0.0, 0.0, 0.0}, {0.0, 0.0, 10.0}, model) ==
          static_cast<std::uint16_t>(
              std::llround((0.10 + 0.90 * 0.5) * 65535.0)),
      "60-degree DN");
  require(lambertHeadlightDn({0.0, 1.0, 0.0}, {0.0, 0.0, 0.0},
                             {0.0, 0.0, 10.0}, model) == 6554,
          "tangential DN retains rounded ambient");
  require(lambertHeadlightDn({0.0, 0.0, -1.0}, {0.0, 0.0, 0.0},
                             {0.0, 0.0, 10.0}, model) == 6554,
          "back-facing DN retains rounded ambient");
  require(lambertHeadlightDn({0.0, 0.0, 7.0}, {0.0, 0.0, 0.0},
                             {0.0, 0.0, 10.0}, model) == 65535,
          "finite non-unit normal is normalized");
}

void testNormalRotationDirection() {
  Matrix3 rotationJ2000FromDsk;
  rotationJ2000FromDsk << 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0,
      0.0, 1.0;
  Matrix3 rotationJ2000FromCamera;
  rotationJ2000FromCamera << 1.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0,
      1.0, 0.0;
  const Vector3 normalCamera =
      normalBodyToCamera(Vector3::UnitX(), rotationJ2000FromDsk,
                         rotationJ2000FromCamera);
  requireVectorNear(normalCamera, Vector3(0.0, 0.0, -1.0), 1.0e-15,
                    "normal_C = transpose(R_JC) * R_JD * normal_D");
  require(std::abs(normalCamera.norm() - 1.0) <= 1.0e-15,
          "camera-frame normal remains unit length");
}

void testRejectsDegenerateAndNonfiniteInputs() {
  const Photometry model{0.10, 0.90};
  requireRejects(
      [&] {
        lambertHeadlightDn(Vector3::Zero(), Vector3::Zero(),
                          Vector3(0.0, 0.0, 10.0), model);
      },
      "normalBody");
  requireRejects(
      [&] {
        lambertHeadlightDn(Vector3::UnitZ(), Vector3::Zero(),
                          Vector3::Zero(), model);
      },
      "view");
  requireRejects(
      [&] {
        lambertHeadlightDn(
            Vector3(std::numeric_limits<double>::infinity(), 0.0, 1.0),
            Vector3::Zero(), Vector3(0.0, 0.0, 10.0), model);
      },
      "normalBody");
  requireRejects(
      [&] {
        normalBodyToCamera(2.0 * Vector3::UnitX(), Matrix3::Identity(),
                           Matrix3::Identity());
      },
      "normalBody");
}

}  // namespace

int main() {
  testDeterministicLambertDn();
  testNormalRotationDirection();
  testRejectsDegenerateAndNonfiniteInputs();
  std::cout << "PASS: Lambert headlight tests\n";
  return EXIT_SUCCESS;
}
