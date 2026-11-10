#include "layout/Surface.h"

#include "core/Profile.h"
#include "geo/Normals.h"

#include <algorithm>

namespace lila::layout
{
Surface::Surface(const geo::Mesh& mesh) : mMesh(&mesh)
{
    LILA_ZONE();
    mBvh.build(mesh);
    const std::vector<glm::vec3> n = geo::pointNormals(mesh);
    mPointNormals.assign(n.begin(), n.end());
    mSize = std::max(1e-6, glm::length(mesh.bounds().size()));
}

glm::dvec3 Surface::normalAt(Index t, const glm::dvec3& bary) const
{
    const geo::Triangulation& tris = mBvh.triangles();
    glm::dvec3 n(0.0);
    for (int k = 0; k < 3; ++k)
    {
        const Index p = mMesh->cornerPoint(tris.corners[size_t(t) * 3 + k]);
        if (p < mPointNormals.size())
            n += mPointNormals[p] * bary[k];
    }
    const double len = glm::length(n);
    if (len > 1e-12)
        return n / len;
    glm::dvec3 a, b, c;
    mBvh.trianglePoints(t, a, b, c);
    const glm::dvec3 f = glm::cross(b - a, c - a);
    return glm::length(f) > 1e-20 ? glm::normalize(f) : glm::dvec3(0.0, 1.0, 0.0);
}

std::optional<SurfaceHit> Surface::closest(const glm::dvec3& p, double maxDistance) const
{
    geo::BvhClosest c;
    if (!mBvh.closestPoint(p, c, maxDistance))
        return std::nullopt;
    return SurfaceHit{c.point, normalAt(c.triangle, c.bary), c.face, c.triangle, c.bary};
}

std::optional<SurfaceHit> Surface::raycast(const glm::dvec3& origin, const glm::dvec3& dir) const
{
    geo::BvhHit h;
    if (!mBvh.raycast(origin, dir, h))
        return std::nullopt;
    return SurfaceHit{h.point, normalAt(h.triangle, h.bary), h.face, h.triangle, h.bary};
}

SurfaceHit Surface::project(const glm::dvec3& p) const
{
    if (auto h = closest(p))
        return *h;
    return SurfaceHit{p};
}

Anchor Surface::anchor(const SurfaceHit& h) const
{
    Anchor a;
    a.pos = h.point;
    if (h.triangle == kInvalid)
        return a;
    const geo::Triangulation& tris = mBvh.triangles();
    a.face = h.face;
    for (int k = 0; k < 3; ++k)
        a.points[size_t(k)] = mMesh->cornerPoint(tris.corners[size_t(h.triangle) * 3 + k]);
    a.bary = h.bary;
    return a;
}

Anchor Surface::anchorAt(const glm::dvec3& p) const { return anchor(project(p)); }

namespace
{
bool faceHasPoints(const geo::Mesh& m, Index f, const std::array<Index, 3>& pts)
{
    if (!m.faceValid(f))
        return false;
    for (Index p : pts)
        if (m.findCorner(f, p) == kInvalid)
            return false;
    return true;
}
} // namespace

glm::dvec3 Surface::resolve(const Anchor& a, bool* rebound) const
{
    if (a.face != kInvalid && faceHasPoints(*mMesh, a.face, a.points))
    {
        glm::dvec3 p(0.0);
        for (int k = 0; k < 3; ++k)
            p += mMesh->position(a.points[size_t(k)]) * a.bary[k];
        if (rebound)
            *rebound = false;
        return p;
    }
    if (rebound)
        *rebound = true;
    return project(a.pos).point;
}

Anchor Surface::refreshed(const Anchor& a, bool* rebound) const
{
    bool lost = false;
    const glm::dvec3 p = resolve(a, &lost);
    if (rebound)
        *rebound = lost;
    if (!lost)
    {
        Anchor out = a;
        out.pos = p;
        return out;
    }
    return anchorAt(a.pos);
}
} // namespace lila::layout
