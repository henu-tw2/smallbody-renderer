// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include <tiffio.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "DskFrameRenderer.h"
#include "SolarLambert.h"
#include "SurfaceAppearance.h"
#include "TsaiPinholeCamera.h"

namespace {

using namespace Isis::SmallBodyRender;

void require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

class FixedRenderCamera final : public RenderCamera {
  public:
    Vector3 rayCamera(double samplePx, double linePx) const override {
      return Vector3(samplePx, linePx, 10.0).normalized();
    }
};

class ConstantRayCamera final : public RenderCamera {
  public:
    Vector3 rayCamera(double, double) const override {
      return Vector3(0.0, 0.0, 1.0);
    }
};

class TwoPlateAppearance final : public SurfaceAppearance {
  public:
    double relativeAlbedo(std::uint32_t plateId) const override {
      return plateId == 1 ? 0.5 : 1.0;
    }
};

class Fixture {
  public:
    Fixture() {
      static int sequence = 0;
      do {
        root = std::filesystem::temp_directory_path() /
               ("dsk-frame-renderer-" + std::to_string(++sequence));
      } while (!std::filesystem::create_directory(root));
      cameraPath = root / "camera.tsai";
      std::ofstream stream(cameraPath);
      stream << "VERSION_4\nPINHOLE\nfu = 10\nfv = 10\ncu = 9\ncv = 8\n"
                "u_direction = 1 0 0\nv_direction = 0 1 0\n"
                "w_direction = 0 0 1\nC = 0 0 -10\n"
                "R = 1 0 0 0 1 0 0 0 1\npitch = 1\nNULL\n";
    }
    ~Fixture() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
    ProductPaths paths() const {
      return {root / "dn.tif", root / "mask.tif", root / "depth.tif",
              root / "normal.tif", root / "plate.tif", {}, {},
              std::nullopt};
    }
    ProductPaths pathsWithDiagnostics() const {
      return {root / "dn.tif", root / "mask.tif", root / "depth.tif",
              root / "normal.tif", root / "plate.tif",
              root / "mu0.tif", root / "illumination.tif", std::nullopt};
    }
    std::filesystem::path root;
    std::filesystem::path cameraPath;
};

ResolvedRenderConfig config(const Fixture &fixture) {
  const QuaternionWxyz identity{1.0, 0.0, 0.0, 0.0};
  return {1,
          {"/shape.bds", std::string(64, 'a'), 1, 1, "J2000", {}},
          100.0,
          16,
          16,
          {0.1, 0.9},
          1.0,
          {20, 1.0e-6},
          {IlluminationMode::CameraHeadlight, 0.1, 0.9, false, 0.0, 0.0},
          {SurfaceAppearanceMode::Uniform, 1.0, {}, {}},
          {"f1", 10.0, fixture.cameraPath, std::string(64, 'b'), 19, 17,
           CameraModelFormat::AspTsaiNull, {0.0, 0.0, -10.0},
           std::nullopt, identity, identity, Matrix3::Identity(),
           Matrix3::Identity()}};
}

class RectangleTracer final : public SurfaceRayTracer {
  public:
    double maximumVertexRadiusM() const override {
      return 10.0;
    }

    SurfaceHit traceClosest(const Vector3 &origin,
                            const Vector3 &direction) const override {
      if (origins.empty()) {
        origins.push_back(origin);
        directions.push_back(direction);
      }
      if (!(direction.z() > 0.0)) {
        return miss();
      }
      const double depth = -origin.z() / direction.z();
      const Vector3 point = origin + depth * direction;
      constexpr double boundaryToleranceM = 1.0e-12;
      if (!(depth > 0.0) ||
          std::abs(point.x()) > 4.0 + boundaryToleranceM ||
          std::abs(point.y()) > 3.0 + boundaryToleranceM) {
        return miss();
      }
      return {true, depth, point, {0.0, 0.0, -1.0},
              point.x() < 0.0 ? 1U : 2U};
    }
    static SurfaceHit miss() {
      const double nan = std::numeric_limits<double>::quiet_NaN();
      return {false, nan, Vector3::Constant(nan), Vector3::Constant(nan), 0};
    }
    mutable std::vector<Vector3> origins;
    mutable std::vector<Vector3> directions;
};

class MissTracer final : public SurfaceRayTracer {
  public:
    double maximumVertexRadiusM() const override {
      return 10.0;
    }

    SurfaceHit traceClosest(const Vector3 &, const Vector3 &) const override {
      return RectangleTracer::miss();
    }
};

class AlternatingPlateTracer final : public SurfaceRayTracer {
  public:
    double maximumVertexRadiusM() const override {
      return 10.0;
    }

    SurfaceHit traceClosest(const Vector3 &, const Vector3 &direction) const override {
      if (!(direction.z() > 0.0)) {
        return RectangleTracer::miss();
      }
      const std::uint32_t plateId =
          (++traceCount % 2 == 1) ? 1U : 2U;
      return {true, 10.0, Vector3::Zero(), Vector3(0.0, 0.0, -1.0),
              plateId};
    }

  private:
    mutable std::uint64_t traceCount{0};
};

const UniformSurfaceAppearance &uniformAppearance() {
  static const UniformSurfaceAppearance appearance(1.0, 3);
  return appearance;
}

class PointSunTracer final : public SurfaceRayTracer {
  public:
    explicit PointSunTracer(double radiusM = 10.0) : radiusM(radiusM) {}

    double maximumVertexRadiusM() const override {
      return radiusM;
    }

    SurfaceHit traceClosest(const Vector3 &origin,
                            const Vector3 &direction) const override {
      ++primaryTraceCount;
      if (!(direction.z() > 0.0)) {
        return RectangleTracer::miss();
      }
      const double depth = -origin.z() / direction.z();
      const Vector3 point = origin + depth * direction;
      if (!(depth > 0.0) || point.x() < -1.0e-12 ||
          point.x() > 2.0 + 1.0e-12 || std::abs(point.y()) > 1.0e-12) {
        return RectangleTracer::miss();
      }
      const bool night = point.x() > 1.5;
      const double normalScale = point.x() < 0.5 ? 2.0 : 1.0;
      const Vector3 normal =
          night ? Vector3(0.0, 0.0, 1.0)
                : Vector3(0.0, 0.0, -normalScale);
      return {true, depth, point, normal,
              static_cast<std::uint32_t>(point.x()) + 1U};
    }

    bool isOccluded(const Vector3 &origin, const Vector3 &direction,
                    double maximumDistanceM) const override {
      shadowOrigins.push_back(origin);
      shadowDirections.push_back(direction);
      shadowRanges.push_back(maximumDistanceM);
      return origin.x() > 0.5;
    }

    double radiusM;
    mutable int primaryTraceCount{0};
    mutable std::vector<Vector3> shadowOrigins;
    mutable std::vector<Vector3> shadowDirections;
    mutable std::vector<double> shadowRanges;
};

template <typename Callable>
void requireRejects(Callable callable, const std::string &field) {
  try { callable(); }
  catch (const std::exception &error) {
    require(std::string(error.what()).find(field) != std::string::npos,
            "diagnostic contains " + field);
    return;
  }
  require(false, "expected rejection for " + field);
}

template <typename Value>
std::vector<Value> readFirstTile(const std::filesystem::path &path,
                                 int samples) {
  TIFF *tiff = TIFFOpen(path.c_str(), "r");
  require(tiff != nullptr, "reopen renderer product " + path.string());
  std::vector<Value> values(
      static_cast<std::size_t>(16 * 16 * samples));
  require(TIFFReadTile(tiff, values.data(), 0, 0, 0, 0) >= 0,
          "read first renderer product tile");
  TIFFClose(tiff);
  return values;
}

void testIdentitySilhouetteSummary() {
  Fixture fixture;
  auto resolved = config(fixture);
  const FixedRenderCamera camera;
  RectangleTracer tracer;
  const ProductPaths paths = fixture.pathsWithDiagnostics();
  TiffProductWriter writer(paths, 19, 17, 16, 16);
  const RenderSummary summary =
      DskFrameRenderer().render(resolved, camera, tracer,
                                 uniformAppearance(), writer);
  require(summary.totalPixels == 323 && summary.validPixels == 20,
          "fixed-camera 5 by 4 rectangle silhouette: total=" +
              std::to_string(summary.totalPixels) + " valid=" +
              std::to_string(summary.validPixels));
  require(summary.distinctPlateCount == 1, "one analytic plate ID");
  require(summary.directPixels == summary.validPixels &&
              summary.nightPixels == 0 &&
              summary.selfShadowPixels == 0,
          "headlight compatibility summary classifies every hit as direct");
  require(std::abs(summary.minimumDepthM - 10.0) < 1.0e-12,
          "principal depth is ten metres");

  const auto dn = readFirstTile<std::uint16_t>(paths.imageU16, 1);
  const auto mask = readFirstTile<std::uint8_t>(paths.maskU8, 1);
  const auto depth = readFirstTile<float>(paths.depthMFloat32, 1);
  const auto normal =
      readFirstTile<float>(paths.normalCameraFloat32, 3);
  const auto plate = readFirstTile<std::uint32_t>(paths.facetIdU32, 1);
  const auto mu0 = readFirstTile<float>(paths.mu0Float32, 1);
  const auto illumination =
      readFirstTile<std::uint8_t>(paths.illuminationMaskU8, 1);
  // V1 NULL compatibility snapshot. The complete 16 by 16 first TIFF tile is
  // literal pre-albedo output for this deterministic fixture, not computed by
  // the renderer under test. The 20 foreground values use the original V1
  // Lambert formula at samples 0..4 and lines 0..3; every remaining pixel is
  // the literal background DN.
  const std::vector<std::uint16_t> expectedV1Dn{
      65535U, 65242U, 64390U, 63048U, 61316U, 0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      65242U, 64954U, 64114U, 62790U, 61082U, 0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      64390U, 64114U, 63308U, 62039U, 60396U, 0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      63048U, 62790U, 62039U, 60850U, 59308U, 0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
      0U,     0U,     0U,     0U,     0U,     0U, 0U, 0U,
  };
  require(dn == expectedV1Dn,
          "uniform 1.0 preserves every V1 NULL first-tile DN");
  require(summary.totalPixels == 323 && summary.validPixels == 20 &&
              summary.distinctPlateCount == 1 && summary.directPixels == 20 &&
              summary.nightPixels == 0 && summary.selfShadowPixels == 0 &&
              summary.minimumDepthM == 10.0 &&
              summary.maximumDepthM == 11.180339887498949,
          "V1 NULL fixture summary remains unchanged");
  require(dn[1] == 65242U && mask[1] == 1U &&
              depth[1] == 10.049875259399414F &&
              normal[3] == 0.0F && normal[4] == 0.0F &&
              normal[5] == -1.0F && plate[1] == 2U &&
              mu0[1] == 0.9950371980667114F &&
              illumination[1] == 1U,
          "V1 NULL key TIFF pixels remain unchanged");
}

void testSurfaceAppearancePreservesUniformV1DnAndShadesPlateAlbedo() {
  Fixture fixture;
  auto resolved = config(fixture);
  resolved.frame.widthPx = 16;
  resolved.frame.heightPx = 16;
  resolved.tileWidthPx = 16;
  resolved.tileHeightPx = 16;
  const ProductPaths uniformPaths{
      fixture.root / "uniform-dn.tif", fixture.root / "uniform-mask.tif",
      fixture.root / "uniform-depth.tif", fixture.root / "uniform-normal.tif",
      fixture.root / "uniform-plate.tif", {}, {}, std::nullopt};
  const ProductPaths shadedPaths{
      fixture.root / "shaded-dn.tif", fixture.root / "shaded-mask.tif",
      fixture.root / "shaded-depth.tif", fixture.root / "shaded-normal.tif",
      fixture.root / "shaded-plate.tif", {}, {}, std::nullopt};
  const UniformSurfaceAppearance uniform(1.0, 2);
  const TwoPlateAppearance twoPlate;
  const ConstantRayCamera camera;
  AlternatingPlateTracer uniformTracer;
  TiffProductWriter uniformWriter(uniformPaths, 16, 16, 16, 16);
  static_cast<void>(DskFrameRenderer().render(
      resolved, camera, uniformTracer, uniform, uniformWriter));
  AlternatingPlateTracer shadedTracer;
  TiffProductWriter shadedWriter(shadedPaths, 16, 16, 16, 16);
  static_cast<void>(DskFrameRenderer().render(
      resolved, camera, shadedTracer, twoPlate, shadedWriter));

  const auto uniformDn = readFirstTile<std::uint16_t>(uniformPaths.imageU16, 1);
  const auto shadedDn = readFirstTile<std::uint16_t>(shadedPaths.imageU16, 1);
  require(uniformDn[0] == 65535U && uniformDn[1] == 65535U,
          "uniform 1.0 preserves the V1 headlight DN");
  require(shadedDn[0] == 32768U && shadedDn[1] == 65535U,
          "plate 1 linear DN is half plate 2 before uint16 quantization");
  require(readFirstTile<std::uint8_t>(uniformPaths.maskU8, 1) ==
              readFirstTile<std::uint8_t>(shadedPaths.maskU8, 1) &&
              readFirstTile<float>(uniformPaths.depthMFloat32, 1) ==
                  readFirstTile<float>(shadedPaths.depthMFloat32, 1) &&
              readFirstTile<float>(uniformPaths.normalCameraFloat32, 3) ==
                  readFirstTile<float>(shadedPaths.normalCameraFloat32, 3) &&
              readFirstTile<std::uint32_t>(uniformPaths.facetIdU32, 1) ==
                  readFirstTile<std::uint32_t>(shadedPaths.facetIdU32, 1),
          "surface appearance changes no geometry product");
}

ResolvedRenderConfig pointSunConfig(const Fixture &fixture) {
  auto resolved = config(fixture);
  resolved.schemaVersion = 2;
  resolved.exposureScale = 0.5;
  resolved.photometry = {0.1, 0.9};
  resolved.illumination = {IlluminationMode::PointSunLambert,
                           0.1, 0.9, true, 0.25, 20.0};
  resolved.surfaceAppearance =
      {SurfaceAppearanceMode::Uniform, 0.5, {}, {}};
  resolved.frame.targetToSunJ2000M = Vector3(0.0, 0.0, -100.0);
  return resolved;
}

void testPointSunClassesDiagnosticsAndShadowRayGeometry() {
  Fixture fixture;
  auto resolved = pointSunConfig(fixture);
  const FixedRenderCamera camera;
  PointSunTracer tracer;
  const ProductPaths paths = fixture.pathsWithDiagnostics();
  TiffProductWriter writer(paths, 19, 17, 16, 16);
  const RenderSummary summary =
      DskFrameRenderer().render(resolved, camera, tracer,
                                 uniformAppearance(), writer);

  require(summary.validPixels == 3, "point-sun fixture has three hits");
  require(summary.directPixels == 1 && summary.nightPixels == 1 &&
              summary.selfShadowPixels == 1,
          "point-sun counts direct, night, and self-shadow pixels");
  require(summary.directPixels + summary.nightPixels +
                  summary.selfShadowPixels ==
              summary.validPixels,
          "illumination counts partition valid pixels");
  require(tracer.shadowOrigins.size() == 2,
          "only direct pixels cast shadow rays");
  require((tracer.shadowOrigins[0] - Vector3(0.0, 0.0, -0.25)).norm() <
              1.0e-14,
          "non-unit fake normal is normalized before metre shadow offset");
  require((tracer.shadowDirections[0] - Vector3(0.0, 0.0, -1.0)).norm() <
                  1.0e-14 &&
              tracer.shadowRanges[0] == 20.0,
          "shadow ray points to Sun and preserves exact finite range");

  const auto dn = readFirstTile<std::uint16_t>(paths.imageU16, 1);
  const auto mask = readFirstTile<std::uint8_t>(paths.maskU8, 1);
  const auto depth = readFirstTile<float>(paths.depthMFloat32, 1);
  const auto plate = readFirstTile<std::uint32_t>(paths.facetIdU32, 1);
  const auto mu0 = readFirstTile<float>(paths.mu0Float32, 1);
  const auto illumination =
      readFirstTile<std::uint8_t>(paths.illuminationMaskU8, 1);
  require(mask[0] == 1 && mask[1] == 1 && mask[2] == 1,
          "self-shadow and night remain valid geometry");
  require(depth[0] == 10.0F && depth[1] > 10.0F && depth[2] > 10.0F &&
              plate[0] == 1 && plate[1] == 2 && plate[2] == 3,
          "point-sun classifications do not alter geometry products");
  require(mu0[0] == 1.0F && mu0[1] > 0.999F && mu0[2] == 0.0F,
          "mu0 TIFF records actual solar incidence");
  require(illumination[0] ==
              static_cast<std::uint8_t>(IlluminationClass::Direct) &&
              illumination[1] ==
                  static_cast<std::uint8_t>(
                      IlluminationClass::SelfShadow) &&
              illumination[2] ==
                  static_cast<std::uint8_t>(IlluminationClass::Night),
          "illumination TIFF records direct, shadow, and night classes");
  require(dn[0] > dn[1] && dn[1] == dn[2] && dn[2] >= 1,
          "shadow and night retain ambient while direct light stays brighter");
}

std::uint16_t firstPointSunDn(double exposureScale, double albedo) {
  Fixture fixture;
  auto resolved = pointSunConfig(fixture);
  resolved.exposureScale = exposureScale;
  resolved.surfaceAppearance.defaultAlbedo = albedo;
  resolved.illumination.castShadows = false;
  PointSunTracer tracer;
  const ProductPaths paths = fixture.pathsWithDiagnostics();
  TiffProductWriter writer(paths, 19, 17, 16, 16);
  const UniformSurfaceAppearance appearance(albedo, 3);
  static_cast<void>(
      DskFrameRenderer().render(resolved, FixedRenderCamera{}, tracer,
                                appearance, writer));
  return readFirstTile<std::uint16_t>(paths.imageU16, 1)[0];
}

void testPointSunConsumesExposureAndUniformAlbedo() {
  const std::uint16_t baseline = firstPointSunDn(0.25, 0.25);
  require(firstPointSunDn(0.5, 0.25) > baseline,
          "point-sun exposure changes unsaturated DN");
  require(firstPointSunDn(0.25, 0.5) > baseline,
          "point-sun default uniform albedo changes unsaturated DN");
}

void testDisabledShadowsDoNotTraceOrRequireDiameterRange() {
  Fixture fixture;
  auto resolved = pointSunConfig(fixture);
  resolved.illumination.castShadows = false;
  resolved.illumination.shadowMaximumRangeM =
      std::nextafter(20.0, 0.0);
  PointSunTracer tracer;
  TiffProductWriter writer(fixture.pathsWithDiagnostics(),
                           19, 17, 16, 16);
  const RenderSummary summary =
      DskFrameRenderer().render(resolved, FixedRenderCamera{}, tracer,
                                 uniformAppearance(), writer);
  require(tracer.shadowOrigins.empty(),
          "disabled shadows issue no occlusion ray");
  require(summary.directPixels == 2 && summary.nightPixels == 1 &&
              summary.selfShadowPixels == 0,
          "disabled shadows retain solar direct/night classification");
}

void testNonIdentityDskRotationTransformsSunDirection() {
  Fixture fixture;
  auto resolved = pointSunConfig(fixture);
  Matrix3 rJd = Matrix3::Identity();
  rJd(1, 1) = -1.0;
  rJd(2, 2) = -1.0;
  resolved.frame.rotationJ2000FromDsk = rJd;
  resolved.frame.rotationJ2000FromCamera = rJd;
  resolved.frame.targetToCameraJ2000M = Vector3(0.0, 0.0, 10.0);
  resolved.frame.targetToSunJ2000M = Vector3(0.0, 0.0, 100.0);
  PointSunTracer tracer;
  TiffProductWriter writer(fixture.pathsWithDiagnostics(),
                           19, 17, 16, 16);
  static_cast<void>(
      DskFrameRenderer().render(resolved, FixedRenderCamera{}, tracer,
                                uniformAppearance(), writer));
  require(!tracer.shadowDirections.empty() &&
              (tracer.shadowDirections[0] -
               Vector3(0.0, 0.0, -1.0)).norm() < 1.0e-14,
          "transpose(R_JD) transforms target-to-Sun J2000 vector into D");
}

void testRejectsPointSunStateAndShadowConfigurationBeforeTracing() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  const std::array<std::optional<Vector3>, 4> invalidSuns{
      std::optional<Vector3>{},
      std::optional<Vector3>{Vector3::Zero()},
      std::optional<Vector3>{Vector3(nan, 0.0, -100.0)},
      std::optional<Vector3>{Vector3(0.0, infinity, -100.0)}};
  for (const std::optional<Vector3> &invalidSun : invalidSuns) {
    Fixture fixture;
    auto resolved = pointSunConfig(fixture);
    resolved.frame.targetToSunJ2000M = invalidSun;
    PointSunTracer tracer;
    TiffProductWriter writer(fixture.pathsWithDiagnostics(),
                             19, 17, 16, 16);
    requireRejects(
        [&] {
          DskFrameRenderer().render(
              resolved, FixedRenderCamera{}, tracer, uniformAppearance(),
              writer);
        },
        invalidSun.has_value() ? "ILLUMINATION_STATE_ERROR"
                               : "ILLUMINATION_CONTRACT_ERROR");
    require(tracer.primaryTraceCount == 0,
            "invalid Sun state is rejected before any pixel trace");
  }
  {
    Fixture fixture;
    auto resolved = pointSunConfig(fixture);
    resolved.frame.targetToSunJ2000M =
        Vector3(std::numeric_limits<double>::max(), 0.0, 0.0);
    resolved.frame.rotationJ2000FromDsk =
        2.0 * Matrix3::Identity();
    PointSunTracer tracer;
    TiffProductWriter writer(fixture.pathsWithDiagnostics(),
                             19, 17, 16, 16);
    requireRejects(
        [&] {
          DskFrameRenderer().render(
              resolved, FixedRenderCamera{}, tracer, uniformAppearance(),
              writer);
        },
        "ILLUMINATION_STATE_ERROR");
    require(tracer.primaryTraceCount == 0,
            "Sun invalid after D-from-J rotation is rejected before tracing");
  }

  for (const double invalidOffset : {0.0, -1.0, nan, infinity}) {
    Fixture fixture;
    auto resolved = pointSunConfig(fixture);
    resolved.illumination.shadowOriginOffsetM = invalidOffset;
    PointSunTracer tracer;
    TiffProductWriter writer(fixture.pathsWithDiagnostics(),
                             19, 17, 16, 16);
    requireRejects(
        [&] {
          DskFrameRenderer().render(
              resolved, FixedRenderCamera{}, tracer, uniformAppearance(),
              writer);
        },
        "SHADOW_CONFIGURATION_ERROR");
    require(tracer.primaryTraceCount == 0,
            "invalid shadow offset is rejected before tracing");
  }
  for (const double invalidRange : {0.0, -1.0, nan, infinity}) {
    Fixture fixture;
    auto resolved = pointSunConfig(fixture);
    resolved.illumination.shadowMaximumRangeM = invalidRange;
    PointSunTracer tracer;
    TiffProductWriter writer(fixture.pathsWithDiagnostics(),
                             19, 17, 16, 16);
    requireRejects(
        [&] {
          DskFrameRenderer().render(
              resolved, FixedRenderCamera{}, tracer, uniformAppearance(),
              writer);
        },
        "SHADOW_CONFIGURATION_ERROR");
    require(tracer.primaryTraceCount == 0,
            "invalid shadow range is rejected before tracing");
  }
}

void testRejectsPointSunPhotometricContractBeforeTracing() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  for (const double invalidExposure : {0.0, -1.0, nan, infinity}) {
    Fixture fixture;
    auto resolved = pointSunConfig(fixture);
    resolved.exposureScale = invalidExposure;
    PointSunTracer tracer;
    TiffProductWriter writer(fixture.pathsWithDiagnostics(),
                             19, 17, 16, 16);
    requireRejects(
        [&] {
          DskFrameRenderer().render(
              resolved, FixedRenderCamera{}, tracer, uniformAppearance(),
              writer);
        },
        "ILLUMINATION_CONTRACT_ERROR");
    require(tracer.primaryTraceCount == 0,
            "invalid point-sun exposure is rejected before tracing");
  }
  for (const double invalidAlbedo : {0.0, -1.0, nan, infinity}) {
    Fixture fixture;
    auto resolved = pointSunConfig(fixture);
    resolved.surfaceAppearance.defaultAlbedo = invalidAlbedo;
    PointSunTracer tracer;
    TiffProductWriter writer(fixture.pathsWithDiagnostics(),
                             19, 17, 16, 16);
    requireRejects(
        [&] {
          DskFrameRenderer().render(
              resolved, FixedRenderCamera{}, tracer, uniformAppearance(),
              writer);
        },
        "ILLUMINATION_CONTRACT_ERROR");
    require(tracer.primaryTraceCount == 0,
            "invalid point-sun albedo is rejected before tracing");
  }
}

void testShadowDiameterBoundaryAndRadiusValidation() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  for (const double invalidRadius :
       {0.0, -1.0, nan, infinity,
        std::nextafter(std::numeric_limits<double>::max() / 2.0,
                       infinity)}) {
    Fixture fixture;
    auto resolved = pointSunConfig(fixture);
    PointSunTracer tracer(invalidRadius);
    TiffProductWriter writer(fixture.pathsWithDiagnostics(),
                             19, 17, 16, 16);
    requireRejects(
        [&] {
          DskFrameRenderer().render(
              resolved, FixedRenderCamera{}, tracer, uniformAppearance(),
              writer);
        },
        "SHADOW_CONFIGURATION_ERROR");
    require(tracer.primaryTraceCount == 0,
            "invalid or overflowing DSK radius is rejected before tracing");
  }

  {
    Fixture fixture;
    auto resolved = pointSunConfig(fixture);
    resolved.illumination.shadowMaximumRangeM = 20.0;
    PointSunTracer tracer(10.0);
    TiffProductWriter writer(fixture.pathsWithDiagnostics(),
                             19, 17, 16, 16);
    static_cast<void>(DskFrameRenderer().render(
        resolved, FixedRenderCamera{}, tracer, uniformAppearance(), writer));
  }
  {
    Fixture fixture;
    auto resolved = pointSunConfig(fixture);
    resolved.illumination.shadowMaximumRangeM =
        std::nextafter(20.0, 0.0);
    PointSunTracer tracer(10.0);
    TiffProductWriter writer(fixture.pathsWithDiagnostics(),
                             19, 17, 16, 16);
    requireRejects(
        [&] {
          DskFrameRenderer().render(
              resolved, FixedRenderCamera{}, tracer, uniformAppearance(),
              writer);
        },
        "SHADOW_CONFIGURATION_ERROR");
    require(tracer.primaryTraceCount == 0,
            "range one ULP below diameter is rejected without epsilon");
  }
}

void testIndependentFrameTransformDirections() {
  Fixture fixture;
  auto resolved = config(fixture);
  Matrix3 rJd;
  rJd << 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0;
  Matrix3 rJc;
  rJc << 1.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 1.0, 0.0;
  resolved.frame.rotationJ2000FromDsk = rJd;
  resolved.frame.rotationJ2000FromCamera = rJc;
  RectangleTracer tracer;
  TiffProductWriter writer(fixture.paths(), 19, 17, 16, 16);
  requireRejects(
      [&] { DskFrameRenderer().render(resolved,
                                      TsaiPinholeCamera::readCanonical(
                                          fixture.cameraPath),
                                      tracer, uniformAppearance(), writer); },
      "NO_VALID_PIXELS");
  const Vector3 expectedOrigin =
      rJd.transpose() * resolved.frame.targetToCameraJ2000M;
  const Vector3 expectedRay =
      rJd.transpose() * rJc *
      TsaiPinholeCamera::readCanonical(fixture.cameraPath).rayCamera(0, 0);
  require((tracer.origins.front() - expectedOrigin).norm() < 1.0e-14 &&
              (tracer.directions.front() - expectedRay).norm() < 1.0e-14,
          "origin_D and ray_D use explicit frame directions");
}

void testNoValidPixelsRejected() {
  Fixture fixture;
  auto resolved = config(fixture);
  MissTracer tracer;
  TiffProductWriter writer(fixture.paths(), 19, 17, 16, 16);
  requireRejects(
      [&] { DskFrameRenderer().render(
                resolved, TsaiPinholeCamera::readCanonical(fixture.cameraPath),
                tracer, uniformAppearance(), writer); },
      "NO_VALID_PIXELS");
}

void testRejectsUnsafeTileWorksetsBeforeTracing() {
  for (const auto &[tileWidth, tileHeight, field] :
       std::array<std::tuple<int, int, const char *>, 3>{
           std::tuple{4112, 16, "tile_width_px"},
           std::tuple{4096, 272, "tile_pixels"},
           std::tuple{2147483632, 16, "tile_width_px"}}) {
    Fixture fixture;
    auto resolved = config(fixture);
    resolved.tileWidthPx = tileWidth;
    resolved.tileHeightPx = tileHeight;
    RectangleTracer tracer;
    TiffProductWriter writer(fixture.paths(), 19, 17, 16, 16);
    requireRejects(
        [&] {
          static_cast<void>(DskFrameRenderer().render(
              resolved, FixedRenderCamera{}, tracer, uniformAppearance(),
              writer));
        },
        field);
    require(tracer.origins.empty(),
            "unsafe tile workset is rejected before surface tracing");
  }
}

}  // namespace

int main() {
  testIdentitySilhouetteSummary();
  testSurfaceAppearancePreservesUniformV1DnAndShadesPlateAlbedo();
  testPointSunClassesDiagnosticsAndShadowRayGeometry();
  testPointSunConsumesExposureAndUniformAlbedo();
  testDisabledShadowsDoNotTraceOrRequireDiameterRange();
  testNonIdentityDskRotationTransformsSunDirection();
  testRejectsPointSunStateAndShadowConfigurationBeforeTracing();
  testRejectsPointSunPhotometricContractBeforeTracing();
  testShadowDiameterBoundaryAndRadiusValidation();
  testIndependentFrameTransformDirections();
  testNoValidPixelsRejected();
  testRejectsUnsafeTileWorksetsBeforeTracing();
  std::cout << "PASS: DSK frame renderer tests\n";
}
