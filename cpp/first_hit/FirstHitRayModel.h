// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#ifndef Isis_FirstHitRayModel_h
#define Isis_FirstHitRayModel_h

#include <array>
#include <optional>

#include <QString>

namespace Isis {

class BulletShapeModel;

/** A body-fixed ray. The origin is metres from the DSK origin; direction is
 * dimensionless and need not be normalized. */
struct BodyFixedRay {
  std::array<double, 3> originBodyM;
  std::array<double, 3> directionBody;
};

/** First-hit acceptance settings. All distances are metres and incidence is
 * degrees. */
struct FirstHitConfiguration {
  double maximumRangeM;
  double maximumIncidenceDeg;
  /** Minimum in-plane Euclidean distance from the intercept to each of the
   * fixed triangle's three edge segments; smaller distances are rejected. */
  double limbMarginM;
};

struct TriangleIdentity {
  int partId;
  int triangleIndex;

  bool operator==(const TriangleIdentity &other) const {
    return partId == other.partId && triangleIndex == other.triangleIndex;
  }
};

struct FirstHitResult {
  bool valid;
  double rangeM;
  std::array<double, 3> pointBodyM;
  std::array<double, 3> normalBody;
  int partId;
  int triangleIndex;
  double incidenceDeg;
  QString rejectionReason;
};

/**
 * Minimum-distance point from a forward body-fixed image ray to the DSK
 * origin. Distances and point coordinates are metres.
 */
struct ShapeBoundedClosestApproach {
  bool valid;
  bool clampedToShapeBound;
  double forwardRangeM;
  double distanceToCenterM;
  double maximumShapeRadiusM;
  std::array<double, 3> pointBodyM;
  QString rejectionReason;
};

/** Fixed-plane range derivative with respect to body-frame origin metres and
 * already-normalized body-frame direction components. */
struct FixedTriangleRangeLinearization {
  double rangeM;
  std::array<double, 3> ds_dOrigin;
  std::array<double, 3> ds_dDirection;
  int triangleIndex;
  TriangleIdentity triangle;
};

/**
 * Read-only adapter around a caller-owned ISIS Bullet shape model.
 *
 * The referenced model and its Bullet mesh must outlive this object. Calls use
 * a local ray callback and never alter BulletShapeModel's cached surface point.
 * ISIS/Bullet does not promise concurrent const ray casts are thread-safe, so
 * callers must externally serialize calls sharing a BulletShapeModel.
 *
 * Geometry is body-fixed metres. Conversion to kilometres occurs only at the
 * ISIS Bullet ray/mesh boundary. The maximum-range comparison allows
 * max(1e-9 m, 64*epsilon*maximumRangeM) for floating roundoff.
 */
class FirstHitRayModel {
  public:
    FirstHitRayModel(const BulletShapeModel &model,
                     FirstHitConfiguration configuration);

    FirstHitResult predict(
        const BodyFixedRay &ray,
        std::optional<TriangleIdentity> expectedTriangle = std::nullopt) const;

    /**
     * Return validated forward closest-hit geometry for rendering without
     * applying laser incidence, limb-margin, or triangle-lock gates.
     */
    FirstHitResult traceClosest(const BodyFixedRay &ray) const;

    /**
     * Return validated forward Bullet first-hit geometry without extracting or
     * locking a triangle. This is only for free Tie XYZ initialization.
     */
    FirstHitResult predictForInitialization(
        const BodyFixedRay &ray) const;

    /**
     * Return a no-truth-leakage initialization point for a ray that misses the
     * DSK. The point is accepted only when it is forward, within the configured
     * range. The closest point is constrained to the closed ball defined by
     * the DSK mesh's conservative maximum vertex radius.
     */
    ShapeBoundedClosestApproach closestApproachInitialization(
        const BodyFixedRay &ray) const;

    FixedTriangleRangeLinearization linearize(
        const BodyFixedRay &ray, TriangleIdentity expectedTriangle) const;

  private:
    struct Evaluation;
    Evaluation evaluateGeometry(const BodyFixedRay &ray,
                                bool validateReportedTriangle) const;
    FirstHitResult applyLaserGates(
        Evaluation evaluation,
        std::optional<TriangleIdentity> expectedTriangle) const;

    const BulletShapeModel &model_;
    FirstHitConfiguration configuration_;
};

}  // namespace Isis

#endif
