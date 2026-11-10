#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "layout/Layout.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace lila::layout
{
/** A spot found on the surface. */
struct SurfaceHit
{
    glm::dvec3 point{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    Index face = kInvalid;
    Index triangle = kInvalid;
    glm::dvec3 bary{0.0};
};

/**
 * The mesh a layout is drawn on, with what finding spots on it needs. Holds the mesh by pointer: it must outlive this.
 * All positions are in the mesh's own space.
 */
class LILA_SCENE_API Surface
{
public:
    explicit Surface(const geo::Mesh& mesh);

    const geo::Mesh& mesh() const { return *mMesh; }
    const geo::MeshBvh& bvh() const { return mBvh; }
    /** Diagonal of the mesh bounds; scales default tolerances. */
    double size() const { return mSize; }

    /** Closest spot on the surface to p. */
    std::optional<SurfaceHit> closest(const glm::dvec3& p, double maxDistance = std::numeric_limits<double>::max()) const;
    /** First spot along a ray. */
    std::optional<SurfaceHit> raycast(const glm::dvec3& origin, const glm::dvec3& dir) const;
    /** Smooth normal at a hit (blend of the triangle's point normals). */
    glm::dvec3 normalAt(Index triangle, const glm::dvec3& bary) const;

    /** An anchor at the hit. */
    Anchor anchor(const SurfaceHit& h) const;
    /** An anchor at the closest spot to p. */
    Anchor anchorAt(const glm::dvec3& p) const;
    /**
     * Where an anchor is now: its blend of face points while the face still has those points, else the closest spot to
     * its last position. `rebound` is set when it had to fall back.
     */
    glm::dvec3 resolve(const Anchor& a, bool* rebound = nullptr) const;
    /** The anchor brought up to date: position resolved, and face and blend re-found when it fell back. */
    Anchor refreshed(const Anchor& a, bool* rebound = nullptr) const;

    /** p moved onto the surface, with its normal. */
    SurfaceHit project(const glm::dvec3& p) const;

private:
    const geo::Mesh* mMesh;
    geo::MeshBvh mBvh;
    std::vector<glm::dvec3> mPointNormals;
    double mSize = 1.0;
};
} // namespace lila::layout
