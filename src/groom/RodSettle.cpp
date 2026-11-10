#include "groom/RodSettle.h"

#include "core/Profile.h"
#include "groom/Groom.h"
#include "groom/GroomLabBridge.h"

#include <groomlab/simulation.h>

#include <algorithm>
#include <cmath>

namespace lila::groom
{
void settleOnRods(Splines& curves, const std::vector<std::vector<double>>& radii, const Surface* scalp, const SettleOptions& o, SettleStats* stats)
{
    LILA_ZONE();
    if (!scalp || curves.empty())
    {
        settle(curves, radii, scalp, o, stats);
        return;
    }
    const std::shared_ptr<const groomlab::ScalpMesh> mesh = groomLabBridge::scalpMesh(scalp->mesh());
    const std::shared_ptr<const groomlab::GuideSet> guides = groomLabBridge::guideSet(curves, *mesh);

    // The locks' typical thickness: what they keep off the scalp and apart from each other on the rods.
    double radiusSum = 0.0;
    size_t radiusCount = 0;
    for (const std::vector<double>& r : radii)
        for (double v : r)
        {
            radiusSum += v;
            ++radiusCount;
        }
    const float radius = radiusCount ? float(radiusSum / double(radiusCount)) : 0.1f;

    groomlab::SimSettings s;
    const double g = glm::length(o.gravity);
    s.gravity = float(g);
    if (g > 1e-9)
        s.gravityDirection = groomLabBridge::toVec(o.gravity / g);
    // The styled shape is the starting point, not a pose to hold against gravity: the locks fall.
    s.groomedUnderGravity = false;
    s.bend = float(std::clamp(o.bend, 0.0, 1.0));
    s.damping = float(std::clamp(o.damping * 2.0, 0.0, 1.0));
    s.friction = float(std::clamp(1.0 - o.slip, 0.0, 1.0));
    s.collideScalp = true;
    s.collisionOffset = radius;
    s.repulsion = o.lockCollisions ? 0.5f : 0.0f;
    s.repulsionRadius = 2.0f * radius;
    s.rootStiffnessProfile = groomlab::Ramp{{0.0f, float(o.rootStiffness)}, {0.2f, float(o.holdShape)}, {1.0f, float(o.holdShape)}};
    s.iterations = std::max(1, o.iterations);
    s.substeps = 4;
    s.live = false;

    groomlab::SimInputs in;
    in.guides = guides;
    in.scalp = mesh;
    groomlab::GuideSolver solver;
    constexpr double kFps = 24.0;
    solver.start(s, kFps, 0, in);
    const int frames = std::max(1, int(std::ceil(double(o.steps) * o.dt * kFps)));
    solver.step(in, frames);

    const groomlab::GuidesPtr settled = solver.guides();
    size_t k = 0;
    for (size_t ci = 0; ci < curves.size() && k < settled->guides().size(); ++ci)
    {
        Spline& c = curves[ci];
        if (c.size() < 2)
            continue;
        const std::vector<groomlab::Vec3f>& p = settled->guides()[k++].points;
        const std::vector<double>* w = ci < o.weights.size() ? &o.weights[ci] : nullptr;
        for (size_t i = 0; i < c.points.size() && i < p.size(); ++i)
        {
            const double t = w && i < w->size() ? std::clamp((*w)[i], 0.0, 1.0) : 1.0;
            c.points[i] += (glm::dvec3(p[i].x, p[i].y, p[i].z) - c.points[i]) * t;
        }
    }

    // Lock contacts at each lock's own thickness: a short pass of the thickness-aware solver from where the rods came to rest.
    SettleOptions contacts = o;
    contacts.steps = std::min(o.steps, 20);
    contacts.holdShape = std::max(o.holdShape, 0.6);
    settle(curves, radii, scalp, contacts, stats);
}
} // namespace lila::groom
