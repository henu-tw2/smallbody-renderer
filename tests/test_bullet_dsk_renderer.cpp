// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include <SpiceUsr.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "BulletDskRayTracer.h"
#include "DskMetadata.h"

namespace {

using Isis::SmallBodyRender::BulletDskRayTracer;
using Isis::SmallBodyRender::DskSpecification;
using Isis::SmallBodyRender::SurfaceHit;
using Isis::SmallBodyRender::SurfaceRayTracer;
using Isis::SmallBodyRender::Vector3;
using Isis::SmallBodyRender::inspectSingleType2Dsk;

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

DskSpecification specification(const std::filesystem::path &path) {
  return {path,
          std::string(64, 'a'),
          999001,
          1,
          "J2000",
          {}};
}

struct SpiceDsk {
  explicit SpiceDsk(const std::filesystem::path &path) {
    dasopr_c(path.c_str(), &handle);
    require(!failed_c(), "CSPICE opens generated DSK");
    dlabfs_c(handle, &descriptor, &found);
    require(!failed_c() && found, "CSPICE finds generated DSK segment");
  }

  ~SpiceDsk() {
    if (handle >= 0) {
      dascls_c(handle);
      if (failed_c()) {
        reset_c();
      }
    }
  }

  SpiceInt handle{-1};
  SpiceDLADescr descriptor{};
  SpiceBoolean found{SPICEFALSE};
};

struct ZeroRadiusDskFixture {
  ZeroRadiusDskFixture()
      : path(std::filesystem::path(ONE_SEGMENT_DSK).parent_path() /
             "asymmetric-zero-radius-runtime.bds") {
    std::error_code error;
    std::filesystem::remove(path, error);
    require(!error, "remove stale zero-radius DSK fixture");
    std::filesystem::copy_file(ONE_SEGMENT_DSK, path, error);
    require(!error, "copy zero-radius DSK fixture");

    SpiceInt handle = -1;
    dasopw_c(path.c_str(), &handle);
    require(!failed_c(), "CSPICE opens zero-radius fixture for update");
    SpiceDLADescr descriptor{};
    SpiceBoolean found = SPICEFALSE;
    dlabfs_c(handle, &descriptor, &found);
    require(!failed_c() && found,
            "CSPICE finds zero-radius fixture segment");
    SpiceInt vertexCount = 0;
    SpiceInt plateCount = 0;
    dskz02_c(handle, &descriptor, &vertexCount, &plateCount);
    require(!failed_c() && vertexCount == 4 && plateCount == 4,
            "zero-radius fixture starts from the four-vertex Type-2 DSK");

    const SpiceInt vertexCoordinateCount = 3 * vertexCount;
    require(descriptor.dsize >= vertexCoordinateCount,
            "Type-2 DLA double component contains all vertex coordinates");
    // Type-2 stores its contiguous vertex array at the end of the DLA
    // double component. Preserve the valid descriptor and spatial index,
    // changing only the coordinates consumed by dskv02_c().
    const SpiceInt firstVertexCoordinate =
        descriptor.dbase + descriptor.dsize - vertexCoordinateCount + 1;
    const SpiceInt lastVertexCoordinate =
        descriptor.dbase + descriptor.dsize;
    const std::vector<SpiceDouble> zeros(vertexCoordinateCount, 0.0);
    dasudd_c(handle, firstVertexCoordinate, lastVertexCoordinate,
             zeros.data());
    require(!failed_c(),
            "CSPICE writes controlled zero-radius vertex coordinates");
    dascls_c(handle);
    require(!failed_c(), "CSPICE closes zero-radius fixture");
  }

  ~ZeroRadiusDskFixture() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  std::filesystem::path path;
};

struct ExpectedHit {
  std::uint32_t plateId;
  Vector3 pointM;
};

class FixedHitTracer final : public SurfaceRayTracer {
  public:
    explicit FixedHitTracer(SurfaceHit hit) : hit_(std::move(hit)) {}

    double maximumVertexRadiusM() const override {
      return 10.0;
    }

    SurfaceHit traceClosest(const Vector3 &,
                            const Vector3 &) const override {
      return hit_;
    }

  private:
    SurfaceHit hit_;
};

class RecordingHitTracer final : public SurfaceRayTracer {
  public:
    double maximumVertexRadiusM() const override {
      return 10.0;
    }

    SurfaceHit traceClosest(const Vector3 &origin,
                            const Vector3 &direction) const override {
      tracedOrigin = origin;
      tracedDirection = direction;
      ++traceCount;
      return {true, 1.0, Vector3::UnitX(), Vector3::UnitX(), 1};
    }

    mutable Vector3 tracedOrigin{Vector3::Zero()};
    mutable Vector3 tracedDirection{Vector3::Zero()};
    mutable int traceCount{0};
};

ExpectedHit cspiceHit(const SpiceDsk &dsk, const Vector3 &originM,
                      const Vector3 &direction) {
  SpiceDouble originKm[3] = {originM.x() / 1000.0,
                             originM.y() / 1000.0,
                             originM.z() / 1000.0};
  SpiceDouble rayDirection[3] = {direction.x(), direction.y(),
                                 direction.z()};
  SpiceDouble pointKm[3] = {};
  SpiceInt plateId = 0;
  SpiceBoolean found = SPICEFALSE;
  dskx02_c(dsk.handle, &dsk.descriptor, originKm, rayDirection, &plateId,
           pointKm, &found);
  require(!failed_c() && found, "CSPICE ray hits generated DSK");
  return {static_cast<std::uint32_t>(plateId),
          Vector3(1000.0 * pointKm[0], 1000.0 * pointKm[1],
                  1000.0 * pointKm[2])};
}

void testMetadataAndEveryPlateAgainstCspice() {
  const auto spec = specification(ONE_SEGMENT_DSK);
  const auto metadata = inspectSingleType2Dsk(spec);
  require(metadata.centerId == 999001, "exact DSK center ID");
  require(metadata.surfaceId == 1, "exact DSK surface ID");
  require(metadata.frameId == 1, "exact J2000 frame ID");
  require(metadata.frameName == "J2000", "exact J2000 frame name");
  require(metadata.vertexCount == 4, "four DSK vertices");
  require(metadata.plateCount == 4, "four DSK plates");
  requireNear(metadata.maximumVertexRadiusM, std::sqrt(30.0), 1.0e-12,
              "last asymmetric DSK vertex has maximum radius in metres");

  const BulletDskRayTracer tracer(spec, 100.0);
  requireNear(tracer.metadata().maximumVertexRadiusM, std::sqrt(30.0),
              1.0e-12, "Bullet tracer exposes inspected DSK radius");
  requireNear(tracer.maximumVertexRadiusM(), std::sqrt(30.0), 1.0e-12,
              "polymorphic tracer exposes DSK shape-center radius in metres");
  const SpiceDsk cspice(spec.path);
  const std::array<Vector3, 4> vertices{
      Vector3(0.0, 0.0, 4.0), Vector3(-3.0, -2.0, -1.0),
      Vector3(4.0, -1.0, -1.0), Vector3(1.0, 5.0, -2.0)};
  const std::array<std::array<int, 3>, 4> faces{
      std::array<int, 3>{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
  const Vector3 centroid =
      (vertices[0] + vertices[1] + vertices[2] + vertices[3]) / 4.0;

  std::array<bool, 5> visited{};
  for (const auto &face : faces) {
    const Vector3 faceCenter =
        (vertices[face[0]] + vertices[face[1]] + vertices[face[2]]) / 3.0;
    const Vector3 outward = (faceCenter - centroid).normalized();
    const Vector3 origin = faceCenter + 20.0 * outward;
    const Vector3 direction = -outward;
    const ExpectedHit expected = cspiceHit(cspice, origin, direction);
    const SurfaceHit actual = tracer.traceClosest(origin, direction);
    require(actual.found, "Bullet ray hits generated DSK face");
    require(actual.plateId == expected.plateId,
            "Bullet triangle maps to identical 1-based CSPICE plate ID");
    require(actual.plateId >= 1 && actual.plateId <= 4,
            "plate ID is within generated DSK");
    visited[actual.plateId] = true;
    const double tolerance =
        std::max(1.0e-6, 1.0e-9 * actual.depthM);
    require((actual.pointBodyM - expected.pointM).norm() <= tolerance,
            "Bullet and CSPICE intercept agree in metres");
    requireNear(actual.normalBody.norm(), 1.0, 1.0e-10,
                "Bullet outward normal is unit");
  }
  require(visited[1] && visited[2] && visited[3] && visited[4],
          "boundary-safe rays exercise all four DSK plates");
}

void testStrictMetadataAndRuntimeRejections() {
  requireRejects(
      [&] { inspectSingleType2Dsk(specification(TWO_SEGMENT_DSK)); },
      "segment");

  auto wrongCenter = specification(ONE_SEGMENT_DSK);
  wrongCenter.expectedCenterId = 42;
  requireRejects([&] { inspectSingleType2Dsk(wrongCenter); },
                 "expected_center_id");

  auto wrongFrame = specification(ONE_SEGMENT_DSK);
  wrongFrame.expectedFrameId = 42;
  requireRejects([&] { inspectSingleType2Dsk(wrongFrame); },
                 "expected_frame_id");

  auto missingKernel = specification(ONE_SEGMENT_DSK);
  missingKernel.frameKernelPaths = {"/definitely/missing/render_frames.tf"};
  requireRejects([&] { inspectSingleType2Dsk(missingKernel); },
                 "frame_kernel_paths[0]");

  const ZeroRadiusDskFixture zeroRadius;
  requireRejects(
      [&] { inspectSingleType2Dsk(specification(zeroRadius.path)); },
      "maximumVertexRadiusM");
}

void testNoHitAndMaximumRange() {
  const BulletDskRayTracer tracer(specification(ONE_SEGMENT_DSK), 100.0);
  const Vector3 outward = Vector3(1.0, 1.0, 1.0).normalized();
  const SurfaceHit miss =
      tracer.traceClosest(Vector3(100.0, 100.0, 100.0), outward);
  require(!miss.found, "ray directed away from DSK is a no-hit");
  require(miss.plateId == 0 && std::isnan(miss.depthM),
          "no-hit uses zero plate and NaN depth");

  const BulletDskRayTracer tooShort(specification(ONE_SEGMENT_DSK), 1.0);
  const SurfaceHit beyond =
      tooShort.traceClosest(Vector3(0.0, 0.0, 20.0),
                            Vector3(0.0, 0.0, -1.0));
  require(!beyond.found, "hit beyond configured maximum range is not returned");
}

void testBoundedOcclusion() {
  const Vector3 origin(0.0, 0.0, 20.0);
  const Vector3 direction(0.0, 0.0, -1.0);
  const BulletDskRayTracer tracer(specification(ONE_SEGMENT_DSK), 100.0);
  require(tracer.isOccluded(origin, direction, 100.0),
          "near first hit occludes");
  require(!tracer.isOccluded(origin, direction, 0.1),
          "hit beyond maximum distance does not occlude");

  const SurfaceHit tenMetreHit{
      true, 10.0, Vector3(10.0, 0.0, 0.0), Vector3::UnitX(), 1};
  const FixedHitTracer exactTracer(tenMetreHit);
  require(exactTracer.isOccluded(Vector3::Zero(), Vector3::UnitX(), 10.0),
          "hit at exact maximum distance occludes");
  require(!exactTracer.isOccluded(
              Vector3::Zero(), Vector3::UnitX(),
              std::nextafter(10.0, 0.0)),
          "hit beyond exact maximum distance is not accepted by epsilon");

  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  const FixedHitTracer zeroDepthHit(
      {true, 0.0, Vector3::Zero(), Vector3::UnitX(), 1});
  require(zeroDepthHit.isOccluded(
              Vector3::Zero(), Vector3::UnitX(), 10.0),
          "finite zero-depth hit is nonnegative and occludes");
  const FixedHitTracer noHit(
      {false, nan, Vector3::Constant(nan), Vector3::Constant(nan), 0});
  require(!noHit.isOccluded(Vector3::Zero(), Vector3::UnitX(), 10.0),
          "no hit does not occlude");
  for (const double invalidDepth : {-1.0, nan, infinity}) {
    const FixedHitTracer invalidHit(
        {true, invalidDepth, Vector3::Zero(), Vector3::UnitX(), 1});
    require(!invalidHit.isOccluded(
                Vector3::Zero(), Vector3::UnitX(), 10.0),
            "non-finite or negative hit depth does not occlude");
  }

  for (const double invalidMaximumDistance :
       {0.0, -1.0, nan, infinity}) {
    requireRejects(
        [&] {
          exactTracer.isOccluded(
              Vector3::Zero(), Vector3::UnitX(),
              invalidMaximumDistance);
        },
        "maximumDistanceM");
  }

  requireRejects(
      [&] {
        tracer.isOccluded(origin, Vector3::Zero(), 100.0);
      },
      "direction");
  requireRejects(
      [&] {
        tracer.isOccluded(
            origin, Vector3(nan, 0.0, -1.0), 100.0);
      },
      "direction");
}

void testOcclusionGeometryContract() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  RecordingHitTracer ignoringGeometry;

  for (const Vector3 &invalidOrigin :
       {Vector3(nan, 0.0, 0.0),
        Vector3(0.0, infinity, 0.0),
        Vector3(0.0, 0.0, -infinity)}) {
    requireRejects(
        [&] {
          ignoringGeometry.isOccluded(
              invalidOrigin, Vector3::UnitX(), 10.0);
        },
        "originBodyM");
    require(ignoringGeometry.traceCount == 0,
            "invalid origin is rejected before traceClosest");
  }
  for (const Vector3 &invalidDirection :
       {Vector3(0.0, 0.0, 0.0),
        Vector3(nan, 0.0, 0.0),
        Vector3(0.0, infinity, 0.0),
        Vector3(0.0, 0.0, -infinity)}) {
    requireRejects(
        [&] {
          ignoringGeometry.isOccluded(
              Vector3::Zero(), invalidDirection, 10.0);
        },
        "directionBody");
    require(ignoringGeometry.traceCount == 0,
            "invalid direction is rejected before traceClosest");
  }

  const std::array<Vector3, 2> validUnscaledDirections{
      Vector3(std::numeric_limits<double>::denorm_min(), 0.0, 0.0),
      Vector3(std::numeric_limits<double>::max(),
              std::numeric_limits<double>::max(), 0.0)};
  for (const Vector3 &direction : validUnscaledDirections) {
    RecordingHitTracer recording;
    require(recording.isOccluded(Vector3(1.0, 2.0, 3.0),
                                 direction, 10.0),
            "finite nonzero unscaled direction reaches traceClosest");
    require(recording.traceCount == 1,
            "valid geometry calls traceClosest exactly once");
    require(recording.tracedOrigin.x() == 1.0 &&
                recording.tracedOrigin.y() == 2.0 &&
                recording.tracedOrigin.z() == 3.0,
            "origin metre coordinates pass through unchanged");
    require(recording.tracedDirection.x() == direction.x() &&
                recording.tracedDirection.y() == direction.y() &&
                recording.tracedDirection.z() == direction.z(),
            "direction scale passes through unchanged");
  }
}

}  // namespace

int main() {
  erract_c("SET", 0, const_cast<char *>("RETURN"));
  testMetadataAndEveryPlateAgainstCspice();
  testStrictMetadataAndRuntimeRejections();
  testNoHitAndMaximumRange();
  testBoundedOcclusion();
  testOcclusionGeometryContract();
  std::cout << "PASS: Bullet DSK renderer integration tests\n";
  return EXIT_SUCCESS;
}
