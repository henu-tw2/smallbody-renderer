// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Radtan5Distortion.h"

namespace {

using Isis::SmallBodyRender::DistortionInverseResult;
using Isis::SmallBodyRender::PinholeIntrinsics;
using Isis::SmallBodyRender::Radtan5Coefficients;
using Isis::SmallBodyRender::Radtan5Distortion;

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

template <typename Callable>
std::string inverseErrorMessage(Callable callable,
                                const std::string &message) {
  try {
    callable();
  }
  catch (const std::exception &error) {
    const std::string errorMessage = error.what();
    require(errorMessage.find("DISTORTION_INVERSE_ERROR") !=
                std::string::npos,
            message + ": diagnostic must include DISTORTION_INVERSE_ERROR; got " +
                errorMessage);
    return errorMessage;
  }
  require(false, message + ": expected exception");
  return {};
}

template <typename Callable>
void requireInverseError(Callable callable, const std::string &message) {
  static_cast<void>(inverseErrorMessage(callable, message));
}

double requireDiagnosticDouble(const std::string &message,
                              const std::string &field) {
  const std::string marker = field + "=";
  const std::size_t valueStart = message.find(marker);
  require(valueStart != std::string::npos,
          "diagnostic must contain " + marker + ": " + message);
  const std::size_t numberStart = valueStart + marker.size();
  const std::size_t numberEnd = message.find(' ', numberStart);
  const std::string number = message.substr(numberStart, numberEnd - numberStart);
  std::size_t parsed = 0;
  const double value = std::stod(number, &parsed);
  require(parsed == number.size() && std::isfinite(value),
          "diagnostic field " + field + " must be finite: " + message);
  return value;
}

void testKnownRadialAndTangentialPoints() {
  const PinholeIntrinsics intrinsics{1000.0, 1000.0, 0.0, 0.0};
  const Eigen::Vector2d ideal(100.0, 200.0);

  const Radtan5Distortion radialOnly(
      intrinsics, {0.1, 0.0, 0.0, 0.0, 0.0});
  requireNear(radialOnly.distortIdealPixel(ideal).x(), 100.5, 1.0e-12,
              "radial sample");
  requireNear(radialOnly.distortIdealPixel(ideal).y(), 201.0, 1.0e-12,
              "radial line");

  const Radtan5Distortion tangentialOnly(
      intrinsics, {0.0, 0.0, 0.01, 0.02, 0.0});
  requireNear(tangentialOnly.distortIdealPixel(ideal).x(), 101.8, 1.0e-12,
              "tangential sample");
  requireNear(tangentialOnly.distortIdealPixel(ideal).y(), 202.1, 1.0e-12,
              "tangential line");
}

void testForwardInverseForwardAtCornersAndPrincipalPoint() {
  const Radtan5Distortion distortion(
      {1000.0, 900.0, 320.0, 240.0},
      {0.1, -0.03, 0.002, -0.001, 0.005});
  const std::vector<Eigen::Vector2d> idealPixels{
      {0.0, 0.0}, {639.0, 0.0}, {0.0, 479.0}, {639.0, 479.0},
      {320.0, 240.0}};

  for (const Eigen::Vector2d &ideal : idealPixels) {
    const Eigen::Vector2d observed = distortion.distortIdealPixel(ideal);
    const DistortionInverseResult inverse =
        distortion.undistortObservedPixel(observed, 20, 1.0e-6);
    const Eigen::Vector2d reobserved =
        distortion.distortIdealPixel(inverse.idealPixelPx);
    require((reobserved - observed).norm() <= 1.0e-6,
            "forward-inverse-forward round trip");
    require((inverse.idealPixelPx - ideal).norm() <= 1.0e-6,
            "inverse recovers ideal pixel");
    require(inverse.iterations >= 0 && inverse.iterations <= 20,
            "inverse reports used Newton iterations");
    requireNear(inverse.roundTripErrorPx, (reobserved - observed).norm(),
                1.0e-15, "inverse reports final round-trip error");
  }
}

void testRejectsInvalidInputsAndInverseFailures() {
  const PinholeIntrinsics validIntrinsics{1000.0, 1000.0, 0.0, 0.0};
  const Radtan5Coefficients validCoefficients{0.1, 0.0, 0.0, 0.0, 0.0};
  const double nan = std::numeric_limits<double>::quiet_NaN();

  requireInverseError(
      [&] { Radtan5Distortion({nan, 1000.0, 0.0, 0.0}, validCoefficients); },
      "non-finite intrinsics");
  requireInverseError(
      [&] { Radtan5Distortion(validIntrinsics, {nan, 0.0, 0.0, 0.0, 0.0}); },
      "non-finite coefficients");
  requireInverseError(
      [&] { Radtan5Distortion({0.0, 1000.0, 0.0, 0.0}, validCoefficients); },
      "non-positive fx");

  const Radtan5Distortion distortion(validIntrinsics, validCoefficients);
  requireInverseError(
      [&] { distortion.undistortObservedPixel({100.0, 200.0}, 0, 1.0e-6); },
      "zero maximum iterations");

  const Radtan5Distortion singular(
      validIntrinsics, {-1.0 / 3.0, 0.0, 0.0, 0.0, 0.0});
  requireInverseError(
      [&] { singular.undistortObservedPixel({1000.0, 0.0}, 20, 1.0e-6); },
      "singular Newton Jacobian");

  const Radtan5Distortion principalPointDistortion(
      validIntrinsics, {0.1, -0.03, 0.002, -0.001, 0.005});
  const DistortionInverseResult principalPoint =
      principalPointDistortion.undistortObservedPixel({0.0, 0.0}, 20, 1.0e-30);
  require(principalPoint.iterations == 0,
          "principal point accepts a positive sub-64-epsilon tolerance");
  requireNear(principalPoint.roundTripErrorPx, 0.0, 0.0,
              "principal point has exactly zero round-trip residual");

  const Radtan5Distortion nonlinear(
      {1000.0, 900.0, 320.0, 240.0},
      {0.1, -0.03, 0.002, -0.001, 0.005});
  const Eigen::Vector2d strictObserved =
      nonlinear.distortIdealPixel({600.0, 400.0});
  const double strictTolerancePx = 1.0e-30;
  const std::string exhaustionMessage = inverseErrorMessage(
      [&] {
        nonlinear.undistortObservedPixel(strictObserved, 1,
                                         strictTolerancePx);
      },
      "strict positive tolerance exhausts Newton iterations");
  require(exhaustionMessage.find("maximumIterations=1") != std::string::npos,
          "exhaustion diagnostic reports maximumIterations");
  const double diagnosticError =
      requireDiagnosticDouble(exhaustionMessage, "roundTripErrorPx");
  const double diagnosticTolerance =
      requireDiagnosticDouble(exhaustionMessage, "roundTripTolerancePx");
  require(diagnosticError > strictTolerancePx,
          "exhaustion diagnostic reports a residual above tolerance");
  require(diagnosticTolerance == strictTolerancePx,
          "exhaustion diagnostic preserves requested tolerance");
}

}  // namespace

int main() {
  testKnownRadialAndTangentialPoints();
  testForwardInverseForwardAtCornersAndPrincipalPoint();
  testRejectsInvalidInputsAndInverseFailures();
  std::cout << "radtan5 distortion contracts passed\n";
  return EXIT_SUCCESS;
}
