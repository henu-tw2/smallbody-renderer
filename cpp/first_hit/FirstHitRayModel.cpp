// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

// Modified by: gengxun + Codex
// Date: 2026-08-11
// Summary: Match Bullet's independent lower-bound edge tests when two barycentric coordinates are slightly negative near a shared vertex.

#include "FirstHitRayModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "BulletClosestRayCallback.h"
#include "BulletShapeModel.h"
#include "BulletTargetShape.h"
#include "IException.h"
#include "IsisBullet.h"

namespace Isis {
namespace {

using Vector = std::array<double, 3>;
using Triangle = std::array<Vector, 3>;

constexpr double kKilometresToMetres = 1000.0;
constexpr double kGrazingDenominator = 1.0e-12;

bool finite(const Vector &value) {
  return std::isfinite(value[0]) && std::isfinite(value[1]) &&
         std::isfinite(value[2]);
}

double dot(const Vector &left, const Vector &right) {
  return left[0] * right[0] + left[1] * right[1] +
         left[2] * right[2];
}

Vector subtract(const Vector &left, const Vector &right) {
  return {left[0] - right[0], left[1] - right[1],
          left[2] - right[2]};
}

Vector scale(const Vector &value, double factor) {
  return {value[0] * factor, value[1] * factor, value[2] * factor};
}

Vector divideComponents(const Vector &value, double divisor) {
  return {value[0] / divisor, value[1] / divisor, value[2] / divisor};
}

Vector cross(const Vector &left, const Vector &right) {
  return {left[1] * right[2] - left[2] * right[1],
          left[2] * right[0] - left[0] * right[2],
          left[0] * right[1] - left[1] * right[0]};
}

double stableNorm(const Vector &value) {
  const double maximum =
      std::max({std::abs(value[0]), std::abs(value[1]), std::abs(value[2])});
  if (!(maximum > 0.0) || !std::isfinite(maximum)) {
    return maximum;
  }
  const Vector scaled = divideComponents(value, maximum);
  return maximum * std::sqrt(dot(scaled, scaled));
}

Vector stableUnit(const Vector &value, const QString &context,
                  IException::ErrorType errorType) {
  if (!finite(value)) {
    throw IException(errorType, context + " must contain finite values",
                     _FILEINFO_);
  }
  const double maximum =
      std::max({std::abs(value[0]), std::abs(value[1]), std::abs(value[2])});
  if (!(maximum > 0.0)) {
    throw IException(errorType, context + " must be nonzero", _FILEINFO_);
  }
  const Vector scaled = divideComponents(value, maximum);
  const double scaledNorm = std::sqrt(dot(scaled, scaled));
  if (!(scaledNorm > 0.0) || !std::isfinite(scaledNorm)) {
    throw IException(errorType, context + " could not be normalized stably",
                     _FILEINFO_);
  }
  return divideComponents(scaled, scaledNorm);
}

Vector fromBullet(const btVector3 &value) {
  return {static_cast<double>(value.x()), static_cast<double>(value.y()),
          static_cast<double>(value.z())};
}

Vector readVertex(const unsigned char *address, PHY_ScalarType type,
                  int partId, int vertexIndex);
int vertexScalarSize(PHY_ScalarType type);

double maximumShapeRadiusM(const BulletShapeModel &model) {
  double maximumRadiusM = 0.0;
  const BulletWorldManager &world = model.model();
  for (int targetIndex = 0; targetIndex < world.size(); ++targetIndex) {
    const BulletTargetShape *target = world.getTarget(targetIndex);
    if (target == nullptr || target->body() == nullptr ||
        target->body()->getCollisionShape() == nullptr) {
      throw IException(IException::Programmer,
                       "Bullet shape contains a null target body",
                       _FILEINFO_);
    }
    const auto *shape = dynamic_cast<const btBvhTriangleMeshShape *>(
        target->body()->getCollisionShape());
    if (shape == nullptr || shape->getMeshInterface() == nullptr) {
      throw IException(IException::Programmer,
                       "Bullet target is not a triangle mesh",
                       _FILEINFO_);
    }
    const btStridingMeshInterface *mesh = shape->getMeshInterface();
    const btVector3 scaling = mesh->getScaling();
    const btTransform transform = target->body()->getWorldTransform();
    for (int part = 0; part < mesh->getNumSubParts(); ++part) {
      const unsigned char *vertexBase = nullptr;
      const unsigned char *indexBase = nullptr;
      int vertexCount = 0;
      int vertexStride = 0;
      int indexStride = 0;
      int triangleCount = 0;
      PHY_ScalarType vertexType = PHY_FLOAT;
      PHY_ScalarType indexType = PHY_INTEGER;
      mesh->getLockedReadOnlyVertexIndexBase(
          &vertexBase, vertexCount, vertexType, vertexStride, &indexBase,
          indexStride, triangleCount, indexType, part);
      struct Unlock {
        const btStridingMeshInterface *mesh;
        int part;
        ~Unlock() { mesh->unLockReadOnlyVertexBase(part); }
      } unlock{mesh, part};
      const int vertexSize = vertexScalarSize(vertexType);
      if (vertexBase == nullptr || vertexCount <= 0 || vertexSize == 0 ||
          vertexStride < 3 * vertexSize) {
        throw IException(IException::Programmer,
                         "Bullet triangle mesh has an invalid vertex layout",
                         _FILEINFO_);
      }
      for (int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
        const Vector local = readVertex(
            vertexBase + static_cast<std::size_t>(vertexIndex) *
                             static_cast<std::size_t>(vertexStride),
            vertexType, part, vertexIndex);
        const btVector3 scaled(local[0] * scaling.x(),
                               local[1] * scaling.y(),
                               local[2] * scaling.z());
        const Vector pointM =
            scale(fromBullet(transform * scaled), kKilometresToMetres);
        const double radiusM = stableNorm(pointM);
        if (!std::isfinite(radiusM)) {
          throw IException(IException::Programmer,
                           "Bullet triangle mesh has a non-finite vertex",
                           _FILEINFO_);
        }
        maximumRadiusM = std::max(maximumRadiusM, radiusM);
      }
    }
  }
  if (!(maximumRadiusM > 0.0)) {
    throw IException(IException::Programmer,
                     "Bullet shape has no positive vertex radius",
                     _FILEINFO_);
  }
  return maximumRadiusM;
}

FirstHitResult rejected(const QString &reason) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  return {false, nan, {nan, nan, nan}, {nan, nan, nan}, -1, -1, nan, reason};
}

template <typename Value>
Value readScalar(const unsigned char *address) {
  Value value;
  std::memcpy(&value, address, sizeof(Value));
  return value;
}

int readIndex(const unsigned char *address, PHY_ScalarType type,
              int partId, int triangleIndex, int corner) {
  switch (type) {
    case PHY_INTEGER:
      return readScalar<int>(address);
    case PHY_SHORT:
      return static_cast<int>(readScalar<unsigned short>(address));
    case PHY_UCHAR:
      return static_cast<int>(readScalar<unsigned char>(address));
    default:
      throw IException(
          IException::Programmer,
          QString("Unsupported Bullet index scalar type for part %1 triangle "
                  "%2 corner %3")
              .arg(partId)
              .arg(triangleIndex)
              .arg(corner),
          _FILEINFO_);
  }
}

int indexScalarSize(PHY_ScalarType type) {
  switch (type) {
    case PHY_INTEGER:
      return sizeof(int);
    case PHY_SHORT:
      return sizeof(unsigned short);
    case PHY_UCHAR:
      return sizeof(unsigned char);
    default:
      return 0;
  }
}

Vector readVertex(const unsigned char *address, PHY_ScalarType type,
                  int partId, int vertexIndex) {
  switch (type) {
    case PHY_DOUBLE:
      return {readScalar<double>(address),
              readScalar<double>(address + sizeof(double)),
              readScalar<double>(address + 2 * sizeof(double))};
    case PHY_FLOAT:
      return {static_cast<double>(readScalar<float>(address)),
              static_cast<double>(readScalar<float>(address + sizeof(float))),
              static_cast<double>(
                  readScalar<float>(address + 2 * sizeof(float)))};
    default:
      throw IException(
          IException::Programmer,
          QString("Unsupported Bullet vertex scalar type for part %1 vertex "
                  "%2")
              .arg(partId)
              .arg(vertexIndex),
          _FILEINFO_);
  }
}

int vertexScalarSize(PHY_ScalarType type) {
  switch (type) {
    case PHY_DOUBLE:
      return sizeof(double);
    case PHY_FLOAT:
      return sizeof(float);
    default:
      return 0;
  }
}

Triangle extractTriangle(const BulletClosestRayCallback &callback,
                         TriangleIdentity identity) {
  const BulletTargetShape *target = callback.body();
  if (target == nullptr || target->body() == nullptr ||
      target->body()->getCollisionShape() == nullptr) {
    throw IException(IException::Programmer,
                     "Bullet hit has no target collision shape", _FILEINFO_);
  }
  const auto *shape = dynamic_cast<const btBvhTriangleMeshShape *>(
      target->body()->getCollisionShape());
  if (shape == nullptr) {
    throw IException(IException::Programmer,
                     "Bullet target is not a btBvhTriangleMeshShape",
                     _FILEINFO_);
  }
  const btStridingMeshInterface *mesh = shape->getMeshInterface();
  if (mesh == nullptr) {
    throw IException(IException::Programmer,
                     "Bullet triangle mesh interface is null", _FILEINFO_);
  }
  const int partCount = mesh->getNumSubParts();
  if (identity.partId < 0 || identity.partId >= partCount) {
    throw IException(
        IException::Programmer,
        QString("Bullet part ID %1 is outside [0,%2)")
            .arg(identity.partId)
            .arg(partCount),
        _FILEINFO_);
  }

  const unsigned char *vertexBase = nullptr;
  const unsigned char *indexBase = nullptr;
  int vertexCount = 0;
  int vertexStride = 0;
  int indexStride = 0;
  int triangleCount = 0;
  PHY_ScalarType vertexType = PHY_FLOAT;
  PHY_ScalarType indexType = PHY_INTEGER;
  mesh->getLockedReadOnlyVertexIndexBase(
      &vertexBase, vertexCount, vertexType, vertexStride, &indexBase,
      indexStride, triangleCount, indexType, identity.partId);
  struct Unlock {
    const btStridingMeshInterface *mesh;
    int part;
    ~Unlock() { mesh->unLockReadOnlyVertexBase(part); }
  } unlock{mesh, identity.partId};

  if (vertexBase == nullptr || indexBase == nullptr || vertexCount <= 0 ||
      triangleCount <= 0) {
    throw IException(IException::Programmer,
                     "Bullet indexed mesh contains null or empty arrays",
                     _FILEINFO_);
  }
  if (identity.triangleIndex < 0 ||
      identity.triangleIndex >= triangleCount) {
    throw IException(
        IException::Programmer,
        QString("Bullet triangle ID %1 is outside [0,%2) in part %3")
            .arg(identity.triangleIndex)
            .arg(triangleCount)
            .arg(identity.partId),
        _FILEINFO_);
  }
  const int indexSize = indexScalarSize(indexType);
  const int vertexSize = vertexScalarSize(vertexType);
  if (indexSize == 0 || indexStride < 3 * indexSize || vertexSize == 0 ||
      vertexStride < 3 * vertexSize) {
    throw IException(
        IException::Programmer,
        QString("Unsupported or inconsistent Bullet mesh layout in part %1")
            .arg(identity.partId),
        _FILEINFO_);
  }

  const unsigned char *triangleAddress =
      indexBase + static_cast<std::size_t>(identity.triangleIndex) *
                      static_cast<std::size_t>(indexStride);
  Triangle triangle;
  const btVector3 scaling = mesh->getScaling();
  const btTransform transform = target->body()->getWorldTransform();
  for (int corner = 0; corner < 3; ++corner) {
    const int vertexIndex =
        readIndex(triangleAddress + corner * indexSize, indexType,
                  identity.partId, identity.triangleIndex, corner);
    if (vertexIndex < 0 || vertexIndex >= vertexCount) {
      throw IException(
          IException::Programmer,
          QString("Bullet vertex index %1 is outside [0,%2) in part %3 "
                  "triangle %4")
              .arg(vertexIndex)
              .arg(vertexCount)
              .arg(identity.partId)
              .arg(identity.triangleIndex),
          _FILEINFO_);
    }
    Vector local = readVertex(
        vertexBase + static_cast<std::size_t>(vertexIndex) *
                         static_cast<std::size_t>(vertexStride),
        vertexType, identity.partId, vertexIndex);
    if (!finite(local)) {
      throw IException(IException::Programmer,
                       "Bullet triangle contains a non-finite vertex",
                       _FILEINFO_);
    }
    const btVector3 scaled(local[0] * scaling.x(), local[1] * scaling.y(),
                           local[2] * scaling.z());
    triangle[corner] =
        scale(fromBullet(transform * scaled), kKilometresToMetres);
  }
  return triangle;
}

double pointSegmentDistance(const Vector &point, const Vector &start,
                            const Vector &end) {
  const Vector edge = subtract(end, start);
  const double edgeScale = stableNorm(edge);
  if (!(edgeScale > 0.0) || !std::isfinite(edgeScale)) {
    throw IException(IException::Programmer,
                     "Bullet hit triangle has a degenerate edge", _FILEINFO_);
  }
  const Vector scaledEdge = divideComponents(edge, edgeScale);
  const Vector scaledRelative =
      divideComponents(subtract(point, start), edgeScale);
  const double lengthSquared = dot(scaledEdge, scaledEdge);
  const double parameter = std::clamp(
      dot(scaledRelative, scaledEdge) / lengthSquared, 0.0, 1.0);
  return stableNorm(
      subtract(point, {start[0] + parameter * edge[0],
                       start[1] + parameter * edge[1],
                       start[2] + parameter * edge[2]}));
}

struct TriangleGeometry {
  Vector normal;
  double planeConstant;
  double edgeDistanceM;
};

TriangleGeometry validateTriangle(const Triangle &triangle,
                                  const Vector &point,
                                  const Vector &callbackNormal,
                                  double limbMarginM) {
  const Vector edge01 = subtract(triangle[1], triangle[0]);
  const Vector edge02 = subtract(triangle[2], triangle[0]);
  const double edgeScale =
      std::max({stableNorm(edge01), stableNorm(edge02),
                stableNorm(subtract(triangle[2], triangle[1]))});
  if (!(edgeScale > 0.0) || !std::isfinite(edgeScale)) {
    throw IException(IException::Programmer,
                     "Bullet hit triangle is degenerate", _FILEINFO_);
  }
  const Vector scaledEdge01 = divideComponents(edge01, edgeScale);
  const Vector scaledEdge02 = divideComponents(edge02, edgeScale);
  const Vector scaledCross = cross(scaledEdge01, scaledEdge02);
  const double scaledCrossNorm = stableNorm(scaledCross);
  if (!(scaledCrossNorm > 0.0) || !std::isfinite(scaledCrossNorm)) {
    throw IException(IException::Programmer,
                     "Bullet hit triangle is degenerate", _FILEINFO_);
  }
  const Vector normal = divideComponents(scaledCross, scaledCrossNorm);
  if (dot(normal, callbackNormal) < 1.0 - 1.0e-10) {
    throw IException(IException::Programmer,
                     "Bullet callback normal disagrees with hit triangle "
                     "winding",
                     _FILEINFO_);
  }

  const double planeConstant = dot(normal, triangle[0]);
  const double planeError = std::abs(dot(normal, point) - planeConstant);
  const double planeTolerance = std::max(1.0e-7, 1.0e-10 * edgeScale);
  if (!std::isfinite(planeError) || planeError > planeTolerance) {
    throw IException(IException::Programmer,
                     "Bullet intercept is inconsistent with reported "
                     "triangle plane",
                     _FILEINFO_);
  }

  const Vector relative =
      divideComponents(subtract(point, triangle[0]), edgeScale);
  const double d00 = dot(scaledEdge01, scaledEdge01);
  const double d01 = dot(scaledEdge01, scaledEdge02);
  const double d11 = dot(scaledEdge02, scaledEdge02);
  const double d20 = dot(relative, scaledEdge01);
  const double d21 = dot(relative, scaledEdge02);
  const double denominator = d00 * d11 - d01 * d01;
  if (!(denominator >
        64.0 * std::numeric_limits<double>::epsilon() * d00 * d11)) {
    throw IException(IException::Programmer,
                     "Bullet hit triangle has unstable barycentric geometry",
                     _FILEINFO_);
  }
  const double b = (d11 * d20 - d01 * d21) / denominator;
  const double c = (d00 * d21 - d01 * d20) / denominator;
  const double a = 1.0 - b - c;
  const double edgeDistance =
      std::min({pointSegmentDistance(point, triangle[0], triangle[1]),
                pointSegmentDistance(point, triangle[1], triangle[2]),
                pointSegmentDistance(point, triangle[2], triangle[0])});
  if (!std::isfinite(edgeDistance)) {
    throw IException(IException::Programmer,
                     "Bullet triangle edge distance is non-finite",
                     _FILEINFO_);
  }
  constexpr double containmentTolerance = 1.0e-8;
  const bool outside =
      !std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) ||
      a < -containmentTolerance || b < -containmentTolerance ||
      c < -containmentTolerance || a > 1.0 + containmentTolerance ||
      b > 1.0 + containmentTolerance || c > 1.0 + containmentTolerance;
  // Bullet 2.86 deliberately permits this normalized edge tolerance in
  // btTriangleRaycastCallback so that rays exactly on a shared edge hit one
  // of its triangles. In barycentric coordinates the same bound is 1.0e-4.
  constexpr double bulletRaycastEdgeTolerance = 1.0e-4;
  const bool outsideBulletTolerance =
      !std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) ||
      a < -bulletRaycastEdgeTolerance || b < -bulletRaycastEdgeTolerance ||
      c < -bulletRaycastEdgeTolerance;
  const double marginSlack =
      64.0 * std::numeric_limits<double>::epsilon() *
      std::max(1.0, edgeScale);
  if (outside && outsideBulletTolerance &&
      edgeDistance > limbMarginM + marginSlack) {
    throw IException(IException::Programmer,
                     QString("Bullet intercept is outside its reported "
                             "triangle and limb protection margin: "
                             "barycentric=(%1,%2,%3), tolerance=%4, "
                             "bullet_raycast_tolerance=%5, "
                             "edge_distance_m=%6, limb_margin_m=%7")
                         .arg(a, 0, 'g', 17)
                         .arg(b, 0, 'g', 17)
                         .arg(c, 0, 'g', 17)
                         .arg(containmentTolerance, 0, 'g', 17)
                         .arg(bulletRaycastEdgeTolerance, 0, 'g', 17)
                         .arg(edgeDistance, 0, 'g', 17)
                         .arg(limbMarginM, 0, 'g', 17),
                     _FILEINFO_);
  }
  return {normal, planeConstant, edgeDistance};
}

}  // namespace

struct FirstHitRayModel::Evaluation {
  FirstHitResult hit;
  TriangleIdentity triangle;
  Vector direction;
  Vector planeNormal;
  double planeConstant;
  double edgeDistanceM;
};

FirstHitRayModel::FirstHitRayModel(
    const BulletShapeModel &model, FirstHitConfiguration configuration)
    : model_(model), configuration_(configuration) {
  if (!std::isfinite(configuration_.maximumRangeM) ||
      !(configuration_.maximumRangeM > 0.0) ||
      !std::isfinite(configuration_.maximumIncidenceDeg) ||
      configuration_.maximumIncidenceDeg < 0.0 ||
      configuration_.maximumIncidenceDeg > 90.0 ||
      !std::isfinite(configuration_.limbMarginM) ||
      configuration_.limbMarginM < 0.0) {
    throw IException(
        IException::User,
        "FirstHitConfiguration requires finite maximumRangeM > 0, "
        "maximumIncidenceDeg in [0,90], and limbMarginM >= 0",
        _FILEINFO_);
  }
}

FirstHitRayModel::Evaluation FirstHitRayModel::evaluateGeometry(
    const BodyFixedRay &ray, bool validateReportedTriangle) const {
  if (!finite(ray.originBodyM)) {
    throw IException(IException::User,
                     "BodyFixedRay origin must contain finite metre values",
                     _FILEINFO_);
  }
  const Vector direction =
      stableUnit(ray.directionBody, "BodyFixedRay direction", IException::User);
  const btVector3 originKm(ray.originBodyM[0] / kKilometresToMetres,
                           ray.originBodyM[1] / kKilometresToMetres,
                           ray.originBodyM[2] / kKilometresToMetres);
  const double maximumRangeKm =
      configuration_.maximumRangeM / kKilometresToMetres;
  const btVector3 rayEnd(
      originKm.x() + direction[0] * maximumRangeKm,
      originKm.y() + direction[1] * maximumRangeKm,
      originKm.z() + direction[2] * maximumRangeKm);
  BulletClosestRayCallback callback(originKm, rayEnd);
  if (!model_.model().raycast(originKm, rayEnd, callback) ||
      !callback.isValid()) {
    return {rejected("NO_HIT"), {-1, -1}, direction, {0.0, 0.0, 0.0},
            0.0, 0.0};
  }

  const TriangleIdentity identity{callback.partId(), callback.triangleIndex()};
  const Vector pointM = scale(fromBullet(callback.point()),
                              kKilometresToMetres);
  Vector normal = stableUnit(fromBullet(callback.normal()),
                             "Bullet callback normal",
                             IException::Programmer);
  const Vector delta = subtract(pointM, ray.originBodyM);
  const double rangeM = stableNorm(delta);
  const double rangeTolerance = std::max(
      1.0e-9, 64.0 * std::numeric_limits<double>::epsilon() *
                  configuration_.maximumRangeM);
  if (!finite(pointM) || !std::isfinite(rangeM) || !(rangeM > 0.0) ||
      rangeM > configuration_.maximumRangeM + rangeTolerance) {
    throw IException(IException::Programmer,
                     "Bullet callback returned invalid or out-of-range "
                     "first-hit geometry",
                     _FILEINFO_);
  }
  const double alongRay = dot(delta, direction);
  const double lateralError = stableNorm(subtract(delta, scale(direction, alongRay)));
  const double rayTolerance = std::max(1.0e-7, 1.0e-10 * rangeM);
  if (!(alongRay > 0.0) || std::abs(alongRay - rangeM) > rayTolerance ||
      lateralError > rayTolerance) {
    throw IException(IException::Programmer,
                     "Bullet first hit is inconsistent with the forward ray",
                     _FILEINFO_);
  }

  const double cosine = std::clamp(dot(normal, scale(direction, -1.0)),
                                   -1.0, 1.0);
  const double incidenceDeg =
      std::acos(cosine) * 180.0 / std::acos(-1.0);
  FirstHitResult hit{true, rangeM, pointM, normal, identity.partId, identity.triangleIndex,
                     incidenceDeg, QString()};
  if (!validateReportedTriangle) {
    return {hit, identity, direction, normal, 0.0, 0.0};
  }
  const Triangle triangle = extractTriangle(callback, identity);
  const TriangleGeometry geometry =
      validateTriangle(triangle, pointM, normal, configuration_.limbMarginM);
  normal = geometry.normal;
  hit.normalBody = normal;
  hit.incidenceDeg =
      std::acos(std::clamp(dot(normal, scale(direction, -1.0)), -1.0, 1.0)) *
      180.0 / std::acos(-1.0);
  return {hit, identity, direction, normal, geometry.planeConstant,
          geometry.edgeDistanceM};
}

FirstHitResult FirstHitRayModel::applyLaserGates(
    Evaluation evaluation,
    std::optional<TriangleIdentity> expectedTriangle) const {
  if (!evaluation.hit.valid) {
    return evaluation.hit;
  }
  FirstHitResult &hit = evaluation.hit;
  if (expectedTriangle && !(evaluation.triangle == *expectedTriangle)) {
    hit.valid = false;
    hit.rejectionReason = "TRIANGLE_SWITCH";
    return hit;
  }
  const double denominator =
      dot(evaluation.planeNormal, evaluation.direction);
  if (std::abs(denominator) <= kGrazingDenominator ||
      hit.incidenceDeg > configuration_.maximumIncidenceDeg + 1.0e-12) {
    hit.valid = false;
    hit.rejectionReason = "GRAZING_INCIDENCE";
    return hit;
  }
  if (evaluation.edgeDistanceM < configuration_.limbMarginM) {
    hit.valid = false;
    hit.rejectionReason = "LIMB_MARGIN";
    return hit;
  }
  return hit;
}

FirstHitResult FirstHitRayModel::predict(
    const BodyFixedRay &ray,
    std::optional<TriangleIdentity> expectedTriangle) const {
  if (expectedTriangle &&
      (expectedTriangle->partId < 0 || expectedTriangle->triangleIndex < 0)) {
    throw IException(IException::User,
                     "expected triangle part and index must be nonnegative",
                     _FILEINFO_);
  }
  return applyLaserGates(evaluateGeometry(ray, true), expectedTriangle);
}

FirstHitResult FirstHitRayModel::traceClosest(
    const BodyFixedRay &ray) const {
  return evaluateGeometry(ray, true).hit;
}

FirstHitResult FirstHitRayModel::predictForInitialization(
    const BodyFixedRay &ray) const {
  return evaluateGeometry(ray, false).hit;
}

ShapeBoundedClosestApproach
FirstHitRayModel::closestApproachInitialization(
    const BodyFixedRay &ray) const {
  if (!finite(ray.originBodyM)) {
    throw IException(IException::User,
                     "BodyFixedRay origin must contain finite metre values",
                     _FILEINFO_);
  }
  const Vector direction =
      stableUnit(ray.directionBody, "BodyFixedRay direction", IException::User);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double forwardRangeM = -dot(ray.originBodyM, direction);
  const double maximumRadiusM = maximumShapeRadiusM(model_);
  if (!(forwardRangeM > 0.0)) {
    return {false, false, forwardRangeM, nan, maximumRadiusM,
            {nan, nan, nan}, "BEHIND_CAMERA"};
  }
  if (forwardRangeM > configuration_.maximumRangeM) {
    return {false, false, forwardRangeM, nan, maximumRadiusM,
            {nan, nan, nan}, "RANGE_LIMIT"};
  }
  const Vector pointM{
      ray.originBodyM[0] + forwardRangeM * direction[0],
      ray.originBodyM[1] + forwardRangeM * direction[1],
      ray.originBodyM[2] + forwardRangeM * direction[2]};
  const double distanceToCenterM = stableNorm(pointM);
  if (!std::isfinite(distanceToCenterM)) {
    throw IException(IException::Programmer,
                     "BodyFixedRay closest approach is non-finite",
                     _FILEINFO_);
  }
  if (distanceToCenterM > maximumRadiusM) {
    const Vector boundedPoint =
        scale(pointM, maximumRadiusM / distanceToCenterM);
    return {true, true, forwardRangeM, distanceToCenterM, maximumRadiusM,
            boundedPoint, QString()};
  }
  return {true, false, forwardRangeM, distanceToCenterM, maximumRadiusM,
          pointM, QString()};
}

FixedTriangleRangeLinearization FirstHitRayModel::linearize(
    const BodyFixedRay &ray, TriangleIdentity expectedTriangle) const {
  if (expectedTriangle.partId < 0 || expectedTriangle.triangleIndex < 0) {
    throw IException(IException::User,
                     "expected triangle part and index must be nonnegative",
                     _FILEINFO_);
  }
  Evaluation evaluation = evaluateGeometry(ray, true);
  const FirstHitResult gated =
      applyLaserGates(evaluation, expectedTriangle);
  if (!gated.valid) {
    throw IException(
        IException::User,
        "Cannot linearize rejected first hit: " +
            gated.rejectionReason,
        _FILEINFO_);
  }
  evaluation.hit = gated;
  const double denominator = dot(evaluation.planeNormal, evaluation.direction);
  if (std::abs(denominator) <= kGrazingDenominator) {
    throw IException(IException::User,
                     "Cannot linearize GRAZING_INCIDENCE first hit",
                     _FILEINFO_);
  }
  const double analyticRange =
      (evaluation.planeConstant -
       dot(evaluation.planeNormal, ray.originBodyM)) /
      denominator;
  const double tolerance = std::max(1.0e-7, 1.0e-10 * evaluation.hit.rangeM);
  if (!std::isfinite(analyticRange) || analyticRange <= 0.0 ||
      std::abs(analyticRange - evaluation.hit.rangeM) > tolerance) {
    throw IException(IException::Programmer,
                     "Fixed-triangle plane range disagrees with Bullet hit",
                     _FILEINFO_);
  }
  FixedTriangleRangeLinearization linearization{
      evaluation.hit.rangeM,
      {},
      {},
      evaluation.triangle.triangleIndex,
      evaluation.triangle};
  for (int axis = 0; axis < 3; ++axis) {
    linearization.ds_dOrigin[axis] =
        -evaluation.planeNormal[axis] / denominator;
    linearization.ds_dDirection[axis] =
        -evaluation.hit.rangeM * evaluation.planeNormal[axis] / denominator;
  }
  return linearization;
}

}  // namespace Isis
