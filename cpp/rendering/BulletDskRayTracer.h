// Modified by: gengxun + Codex
// Date: 2026-10-19
// Summary: Extract this generic renderer component from the mission repository.

#pragma once

#include <cstdint>
#include <memory>

#include "DskMetadata.h"

namespace Isis {
class BulletShapeModel;
class BulletTargetShape;
class FirstHitRayModel;
}  // namespace Isis

namespace Isis::SmallBodyRender {

struct SurfaceHit {
  bool found;
  double depthM;
  Vector3 pointBodyM;
  Vector3 normalBody;
  std::uint32_t plateId;
};

class SurfaceRayTracer {
  public:
    virtual ~SurfaceRayTracer() = default;

    /**
     * Return the finite, strictly positive maximum DSK vertex radius.
     *
     * The radius is measured in metres from the DSK shape center (the origin
     * of frame D) to the farthest mesh vertex.
     */
    virtual double maximumVertexRadiusM() const = 0;

    virtual SurfaceHit traceClosest(
        const Vector3 &originBodyM,
        const Vector3 &directionBody) const = 0;

    /**
     * Return whether the closest forward surface hit is no farther than
     * maximumDistanceM, using an exact `depthM <= maximumDistanceM` boundary.
     *
     * originBodyM is a finite D body-fixed position in metres relative to the
     * DSK shape center. directionBody is a finite nonzero dimensionless
     * direction in the same D axes; it need not be unit length, and its scale
     * is passed unchanged to traceClosest(). maximumDistanceM must be finite
     * and strictly positive metres. Invalid inputs throw std::invalid_argument
     * before traceClosest() is called. A no-hit or a non-finite/negative
     * returned depth is not occluded.
     */
    virtual bool isOccluded(
        const Vector3 &originBodyM,
        const Vector3 &directionBody,
        double maximumDistanceM) const;
};

class BulletDskRayTracer final : public SurfaceRayTracer {
  public:
    BulletDskRayTracer(const DskSpecification &dsk,
                       double maximumRangeM);
    ~BulletDskRayTracer() override;

    BulletDskRayTracer(const BulletDskRayTracer &) = delete;
    BulletDskRayTracer &operator=(const BulletDskRayTracer &) = delete;

    SurfaceHit traceClosest(
        const Vector3 &originBodyM,
        const Vector3 &directionBody) const override;
    double maximumVertexRadiusM() const override;
    const DskMetadata &metadata() const;

  private:
    DskMetadata metadata_;
    std::unique_ptr<Isis::BulletTargetShape> target_;
    std::unique_ptr<Isis::BulletShapeModel> shapeModel_;
    std::unique_ptr<Isis::FirstHitRayModel> rayModel_;
};

}  // namespace Isis::SmallBodyRender
