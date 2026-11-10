#include "ui/GeometrySnap.h"

#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/Rig.h"
#include "scene/Transform.h"
#include "ui/Isolate.h"
#include "ui/Tools.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace lila::ui::geometrySnap
{
namespace
{
double distPx(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }
} // namespace

void Nearest::point(const glm::dvec3& world)
{
    if (auto s = mView.project(world))
        if (double d = distPx(*s, mPx); d < mBest)
        {
            mBest = d;
            mHit = world;
        }
}

void Nearest::segment(const glm::dvec3& a, const glm::dvec3& b)
{
    auto sa = mView.project(a), sb = mView.project(b);
    if (!sa || !sb)
        return;
    QPointF dir = *sb - *sa;
    double len2 = QPointF::dotProduct(dir, dir);
    double t = len2 > 0 ? std::clamp(QPointF::dotProduct(mPx - *sa, dir) / len2, 0.0, 1.0) : 0.0;
    if (double d = distPx(*sa + dir * t, mPx); d < mBest)
    {
        mBest = d;
        // Screen-space t is close enough along short segments; perspective skew is tiny at snap distances.
        mHit = glm::mix(a, b, t);
    }
}

bool isPointObject(scene::Graph& g, scene::NodeId id)
{
    const scene::Node* node = g.node(id);
    return node && (node->type().isA("locator") || scene::rig::isJoint(*node));
}

bool addPointObject(Nearest& n, scene::Graph& g, scene::NodeId id, unsigned snaps)
{
    if (!isPointObject(g, id))
        return false;
    if ((snaps & SnapVertex) && scene::effectivelyVisible(g, id) && isolate::shows(scene::isolation::active(), id))
        n.point(glm::dvec3(g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]));
    return true;
}

void addCurve(Nearest& n, scene::Graph& g, scene::NodeId id, unsigned snaps, const std::vector<uint32_t>& moving)
{
    LILA_ZONE();

    scene::Node* node = g.node(id);
    if (!node || !node->type().isA("curve") || !(snaps & (SnapVertex | SnapEdge)) || !scene::effectivelyVisible(g, id) ||
        !isolate::shows(scene::isolation::active(), id))
        return;
    const curve::Splines& ss = curve::shapeSplines(g, id);
    glm::dmat4 world = node->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    auto at = [&](const glm::dvec3& p) { return glm::dvec3(world * glm::dvec4(p, 1.0)); };

    std::set<uint32_t> movingIds(moving.begin(), moving.end());
    std::set<size_t> reshaping;
    for (uint32_t c : moving)
    {
        size_t si = 0, pi = 0;
        curve::PointRole role{};
        curve::decodeComponent(c, &si, &pi, &role);
        reshaping.insert(si);
    }

    if (snaps & SnapVertex)
        for (auto& [c, p] : curve::componentPositions(ss))
        {
            size_t si = 0, pi = 0;
            curve::PointRole role{};
            curve::decodeComponent(c, &si, &pi, &role);
            bool target = role == curve::PointRole::Point || (role == curve::PointRole::Edit && !reshaping.contains(si));
            if (target && !movingIds.contains(c))
                n.point(at(p));
        }
    if (snaps & SnapEdge)
        for (size_t si = 0; si < ss.size(); ++si)
        {
            if (reshaping.contains(si) || !curve::isValid(ss[si]))
                continue;
            curve::Polyline pl = curve::tessellate(ss[si]);
            size_t segs = pl.closed ? pl.size() : (pl.size() ? pl.size() - 1 : 0);
            for (size_t i = 0; i < segs; ++i)
                n.segment(at(pl.points[i]), at(pl.points[(i + 1) % pl.size()]));
        }
}
} // namespace lila::ui::geometrySnap
