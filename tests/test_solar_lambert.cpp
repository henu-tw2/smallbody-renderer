// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "SolarLambert.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

namespace {

using Isis::SmallBodyRender::IlluminationClass;
using Isis::SmallBodyRender::Photometry;
using Isis::SmallBodyRender::SolarGeometry;
using Isis::SmallBodyRender::Vector3;
using Isis::SmallBodyRender::solarGeometry;
using Isis::SmallBodyRender::solarLambertDn;

void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void requireNear(double actual, double expected, double tolerance,
                 const std::string &message) {
  require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
          message + ": got " + std::to_string(actual) + ", expected " +
              std::to_string(expected));
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

void testGeometryUsesPointToSunDirectionAndNormalizesNormal() {
  const SolarGeometry facing = solarGeometry(
      7.0 * Vector3::UnitZ(), Vector3::Zero(),
      Vector3(0.0, 0.0, 100.0));
  requireVectorNear(facing.lightDirectionBody, Vector3::UnitZ(), 1.0e-15,
                    "normal-incidence light direction");
  requireNear(facing.mu0, 1.0, 1.0e-15, "normal incidence");
  require(facing.classification == IlluminationClass::Direct,
          "normal incidence is direct");

  const SolarGeometry sixtyDegrees = solarGeometry(
      7.0 * Vector3::UnitZ(), Vector3::Zero(),
      Vector3(std::sqrt(3.0), 0.0, 1.0));
  requireNear(sixtyDegrees.mu0, 0.5, 1.0e-15,
              "60-degree incidence");
  require(sixtyDegrees.classification == IlluminationClass::Direct,
          "60-degree incidence is direct");

  const SolarGeometry offsetHit = solarGeometry(
      Vector3::UnitX(), Vector3(10.0, 2.0, 3.0),
      Vector3(110.0, 2.0, 3.0));
  requireVectorNear(offsetHit.lightDirectionBody, Vector3::UnitX(),
                    1.0e-15,
                    "light direction is Sun position minus hit position");
  requireNear(offsetHit.mu0, 1.0, 1.0e-15,
              "nonzero hit position affects light direction");
}

void testGeometryClassifiesNightAndMu0Boundary() {
  const SolarGeometry night = solarGeometry(
      Vector3::UnitZ(), Vector3::Zero(),
      Vector3(0.0, 0.0, -100.0));
  requireNear(night.mu0, 0.0, 0.0, "back-facing mu0");
  require(night.classification == IlluminationClass::Night,
          "back-facing surface is night");

  const SolarGeometry tangent = solarGeometry(
      Vector3::UnitX(), Vector3::Zero(), Vector3::UnitZ());
  requireNear(tangent.mu0, 0.0, 0.0, "tangent mu0 boundary");
  require(tangent.classification == IlluminationClass::Night,
          "zero mu0 is night");

  const SolarGeometry barelyLit = solarGeometry(
      Vector3(1.0, 0.0, 1.0e-12), Vector3::Zero(),
      Vector3::UnitZ());
  require(barelyLit.mu0 > 0.0, "positive mu0 remains positive");
  require(barelyLit.classification == IlluminationClass::Direct,
          "positive mu0 is direct");
}

void testGeometryNormalizesAcrossFiniteDoubleRange() {
  const double inverseSqrtTwo = 1.0 / std::sqrt(2.0);
  const Vector3 diagonalUnit(inverseSqrtTwo, inverseSqrtTwo, 0.0);

  const SolarGeometry referenceNormal = solarGeometry(
      diagonalUnit, Vector3::Zero(), Vector3::UnitX());
  const SolarGeometry maximumNormal = solarGeometry(
      Vector3(DBL_MAX, DBL_MAX, 0.0), Vector3::Zero(),
      Vector3::UnitX());
  requireNear(maximumNormal.mu0, referenceNormal.mu0, 1.0e-15,
              "DBL_MAX normal has scale-independent mu0");
  requireNear(maximumNormal.mu0, inverseSqrtTwo, 1.0e-15,
              "DBL_MAX normal preserves diagonal direction");

  const SolarGeometry maximumSun = solarGeometry(
      diagonalUnit, Vector3::Zero(),
      Vector3(DBL_MAX, DBL_MAX, 0.0));
  requireVectorNear(maximumSun.lightDirectionBody, diagonalUnit, 1.0e-15,
                    "DBL_MAX Sun displacement is normalized");
  requireNear(maximumSun.mu0, 1.0, 1.0e-15,
              "DBL_MAX Sun displacement preserves direction");

  const SolarGeometry largeFinite = solarGeometry(
      diagonalUnit, Vector3::Zero(), Vector3(1.0e150, 1.0e150, 0.0));
  requireVectorNear(largeFinite.lightDirectionBody, diagonalUnit, 1.0e-15,
                    "1e150 Sun displacement is normalized");

  const SolarGeometry minimumNormal = solarGeometry(
      Vector3(DBL_MIN, DBL_MIN, 0.0), Vector3::Zero(),
      Vector3::UnitX());
  requireNear(minimumNormal.mu0, inverseSqrtTwo, 1.0e-15,
              "DBL_MIN normal is normalized");

  const double denormal = std::numeric_limits<double>::denorm_min();
  const SolarGeometry denormalSun = solarGeometry(
      diagonalUnit, Vector3::Zero(),
      Vector3(denormal, denormal, 0.0));
  requireVectorNear(denormalSun.lightDirectionBody, diagonalUnit, 1.0e-15,
                    "denormal Sun displacement is normalized");
  requireNear(denormalSun.mu0, 1.0, 1.0e-15,
              "denormal Sun displacement preserves direction");
}

void testGeometryRejectsInvalidVectors() {
  const double infinity = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  requireRejects(
      [&] {
        solarGeometry(Vector3::Zero(), Vector3::Zero(),
                      Vector3::UnitZ());
      },
      "normalBody");
  requireRejects(
      [&] {
        solarGeometry(Vector3(infinity, 0.0, 1.0), Vector3::Zero(),
                      Vector3::UnitZ());
      },
      "normalBody");
  requireRejects(
      [&] {
        solarGeometry(Vector3::UnitZ(), Vector3(nan, 0.0, 0.0),
                      Vector3::UnitZ());
      },
      "hitBodyM");
  requireRejects(
      [&] {
        solarGeometry(Vector3::UnitZ(), Vector3::Zero(),
                      Vector3::Zero());
      },
      "targetToSunBodyM");
  requireRejects(
      [&] {
        solarGeometry(Vector3::UnitZ(), Vector3::Zero(),
                      Vector3(0.0, infinity, 1.0));
      },
      "targetToSunBodyM");
  requireRejects(
      [&] {
        solarGeometry(Vector3::UnitZ(), Vector3(1.0, 2.0, 3.0),
                      Vector3(1.0, 2.0, 3.0));
      },
      "lightDirectionBody");
  requireRejects(
      [&] {
        solarGeometry(Vector3::UnitZ(),
                      Vector3(-DBL_MAX, 0.0, 0.0),
                      Vector3(DBL_MAX, 0.0, 0.0));
      },
      "lightDirectionBody");
}

void testDnDirectShadowNightScalingAndBounds() {
  const Photometry model{0.05, 0.95};
  require(solarLambertDn(1.0, false, 1.0, 1.0, model) == 65535,
          "fully lit unit albedo");
  require(
      solarLambertDn(1.0, true, 1.0, 1.0, model) ==
          static_cast<std::uint16_t>(std::llround(0.05 * 65535.0)),
      "occlusion removes direct light but retains ambient");
  require(
      solarLambertDn(0.0, false, 1.0, 1.0, model) ==
          static_cast<std::uint16_t>(std::llround(0.05 * 65535.0)),
      "night retains ambient without requiring occlusion");
  require(
      solarLambertDn(1.0, false, 0.5, 0.5, model) == 16384,
      "non-identity exposure and relative albedo scale linear intensity");
  require(solarLambertDn(1.0, false, 2.0, 2.0, model) == 65535,
          "intensity saturates at the unsigned 16-bit maximum");
  require(solarLambertDn(0.0, true, 1.0e-20, 1.0, model) == 1,
          "valid surface DN has a lower bound of one");
}

void testDnRejectsInvalidInputs() {
  const double infinity = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const Photometry model{0.05, 0.95};

  requireRejects(
      [&] { solarLambertDn(nan, false, 1.0, 1.0, model); }, "mu0");
  requireRejects(
      [&] { solarLambertDn(-1.0e-15, false, 1.0, 1.0, model); },
      "mu0");
  requireRejects(
      [&] { solarLambertDn(1.0 + 1.0e-15, false, 1.0, 1.0, model); },
      "mu0");
  requireRejects(
      [&] { solarLambertDn(0.5, false, 0.0, 1.0, model); },
      "relativeAlbedo");
  requireRejects(
      [&] { solarLambertDn(0.5, false, infinity, 1.0, model); },
      "relativeAlbedo");
  requireRejects(
      [&] { solarLambertDn(0.5, false, 1.0, 0.0, model); },
      "exposureScale");
  requireRejects(
      [&] { solarLambertDn(0.5, false, 1.0, infinity, model); },
      "exposureScale");
  requireRejects(
      [&] {
        solarLambertDn(0.5, false, 1.0, 1.0, Photometry{0.0, 1.0});
      },
      "photometry.ambient");
  requireRejects(
      [&] {
        solarLambertDn(0.5, false, 1.0, 1.0,
                       Photometry{infinity, 0.0});
      },
      "photometry.ambient");
  requireRejects(
      [&] {
        solarLambertDn(0.5, false, 1.0, 1.0,
                       Photometry{0.05, -0.05});
      },
      "photometry.diffuse");
  requireRejects(
      [&] {
        solarLambertDn(0.5, false, 1.0, 1.0,
                       Photometry{0.05, nan});
      },
      "photometry.diffuse");
  requireRejects(
      [&] {
        solarLambertDn(0.5, false, 1.0, 1.0,
                       Photometry{0.05, 0.90});
      },
      "photometry");
}

void testIlluminationClassEncoding() {
  static_assert(
      std::is_same_v<std::underlying_type_t<IlluminationClass>,
                     std::uint8_t>,
      "illumination class storage must be uint8_t");
  require(static_cast<std::uint8_t>(IlluminationClass::Background) == 0,
          "background class encoding");
  require(static_cast<std::uint8_t>(IlluminationClass::Direct) == 1,
          "direct class encoding");
  require(static_cast<std::uint8_t>(IlluminationClass::Night) == 2,
          "night class encoding");
  require(static_cast<std::uint8_t>(IlluminationClass::SelfShadow) == 3,
          "self-shadow class encoding");
}

}  // namespace

int main() {
  testGeometryUsesPointToSunDirectionAndNormalizesNormal();
  testGeometryClassifiesNightAndMu0Boundary();
  testGeometryNormalizesAcrossFiniteDoubleRange();
  testGeometryRejectsInvalidVectors();
  testDnDirectShadowNightScalingAndBounds();
  testDnRejectsInvalidInputs();
  testIlluminationClassEncoding();
  std::cout << "PASS: Solar Lambert tests\n";
  return EXIT_SUCCESS;
}
