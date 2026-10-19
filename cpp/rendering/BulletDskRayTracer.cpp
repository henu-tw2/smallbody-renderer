// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#include "BulletDskRayTracer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include <QString>

#include "BulletShapeModel.h"
#include "BulletTargetShape.h"
#include "FirstHitRayModel.h"
#include "IException.h"
#include "Pvl.h"

namespace Isis::SmallBodyRender {
namespace {

SurfaceHit noHit() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  return {false, nan, Vector3::Constant(nan), Vector3::Constant(nan), 0};
}

bool finiteVector(const Vector3 &value) {
  return std::isfinite(value.x()) && std::isfinite(value.y()) &&
         std::isfinite(value.z());
}

}  // namespace

bool SurfaceRayTracer::isOccluded(
    const Vector3 &originBodyM,
    const Vector3 &directionBody,
    double maximumDistanceM) const {
  if (!finiteVector(originBodyM)) {
    throw std::invalid_argument(
        "originBodyM: must contain finite D body-fixed metre "
        "coordinates relative to the DSK shape center");
  }
  if (!finiteVector(directionBody)) {
    throw std::invalid_argument(
        "directionBody: must contain finite components");
  }
  const double directionScale =
      std::max({std::abs(directionBody.x()),
                std::abs(directionBody.y()),
                std::abs(directionBody.z())});
  if (!(directionScale > 0.0)) {
    throw std::invalid_argument(
        "directionBody: must be a nonzero dimensionless direction");
  }
  if (!std::isfinite(maximumDistanceM) ||
      !(maximumDistanceM > 0.0)) {
    throw std::invalid_argument(
        "maximumDistanceM: must be finite and positive metres");
  }
  const SurfaceHit hit = traceClosest(originBodyM, directionBody);
  return hit.found && std::isfinite(hit.depthM) &&
         hit.depthM >= 0.0 && hit.depthM <= maximumDistanceM;
}

BulletDskRayTracer::BulletDskRayTracer(
    const DskSpecification &dsk, double maximumRangeM)
    : metadata_(inspectSingleType2Dsk(dsk)) {
  if (!std::isfinite(maximumRangeM) || !(maximumRangeM > 0.0)) {
    throw std::invalid_argument(
        "maximum_range_m: must be finite and positive");
  }
  try {
    target_.reset(Isis::BulletTargetShape::loadDSK(
        QString::fromStdString(dsk.path.string())));
    if (!target_) {
      throw std::runtime_error(
          "ISIS BulletTargetShape::loadDSK returned null");
    }
    Isis::Pvl unused;
    shapeModel_ = std::make_unique<Isis::BulletShapeModel>(
        target_.get(), nullptr, unused);
    rayModel_ = std::make_unique<Isis::FirstHitRayModel>(
        *shapeModel_,
        Isis::FirstHitConfiguration{maximumRangeM, 90.0, 0.0});
  }
  catch (const Isis::IException &error) {
    throw std::runtime_error("dsk.path: ISIS Bullet load failed: " +
                             error.toString().toStdString());
  }
}

BulletDskRayTracer::~BulletDskRayTracer() = default;

double BulletDskRayTracer::maximumVertexRadiusM() const {
  return metadata_.maximumVertexRadiusM;
}

SurfaceHit BulletDskRayTracer::traceClosest(
    const Vector3 &originBodyM,
    const Vector3 &directionBody) const {
  const Isis::FirstHitResult result = rayModel_->traceClosest(
      {{originBodyM.x(), originBodyM.y(), originBodyM.z()},
       {directionBody.x(), directionBody.y(), directionBody.z()}});
  if (!result.valid) {
    if (result.rejectionReason == "NO_HIT") {
      return noHit();
    }
    throw std::runtime_error(
        "Bullet traceClosest rejected geometry: " +
        result.rejectionReason.toStdString());
  }
  if (result.partId != 0) {
    throw std::runtime_error(
        "Bullet partId: renderer requires the single DSK mesh part 0");
  }
  if (result.triangleIndex < 0 ||
      result.triangleIndex >= metadata_.plateCount) {
    throw std::runtime_error(
        "Bullet triangleIndex: outside validated DSK plate range");
  }
  return {true,
          result.rangeM,
          Vector3(result.pointBodyM[0], result.pointBodyM[1],
                  result.pointBodyM[2]),
          Vector3(result.normalBody[0], result.normalBody[1],
                  result.normalBody[2]),
          static_cast<std::uint32_t>(result.triangleIndex + 1)};
}

const DskMetadata &BulletDskRayTracer::metadata() const {
  return metadata_;
}

}  // namespace Isis::SmallBodyRender
