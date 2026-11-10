// The overlap::Provider Stagehand gives other extensions: how deep a convex piece sits in a heightfield, measured by
// the same physics dressCheck uses for overlaps.

#include "Stagehand.h"

#include <lila/Extension.h>
#include <lila/Heightfield.h>
#include <lila/Overlap.h>
#include <lila/Profile.h>

#include <RigidScene.h>

#include <algorithm>

namespace stagehand
{
namespace
{
namespace physics = lila::physics;

class GroundSession final : public lila::overlap::Session
{
public:
    explicit GroundSession(const lila::geo::Heightfield& h) : mGround(h)
    {
        physics::Settings s;
        s.serial = true;
        s.recordContacts = false;
        mScene = std::make_unique<physics::RigidScene>(s);
        physics::BodyDesc ground;
        ground.role = physics::Role::Static;
        ground.shape.kind = physics::Shape::Kind::Heightfield;
        ground.shape.heights = std::make_shared<const std::vector<float>>(h.heights);
        ground.shape.samplesX = h.nx;
        ground.shape.samplesZ = h.nz;
        ground.shape.cell = h.cell;
        ground.shape.gridOrigin = h.origin;
        ground.shape.rounding = 0.0;
        mScene->add(ground);
    }

    double depth(std::span<const glm::dvec3> hull, const glm::dvec3& out) override
    {
        LILA_ZONE();
        if (hull.size() < 4)
            return 0.0;
        glm::dvec3 centre(0.0);
        for (const glm::dvec3& p : hull)
            centre += p;
        centre /= double(hull.size());
        auto points = std::make_shared<std::vector<glm::dvec3>>();
        points->reserve(hull.size());
        for (const glm::dvec3& p : hull)
            points->push_back(p - centre);
        physics::BodyDesc piece;
        piece.role = physics::Role::Kinematic;
        piece.shape.kind = physics::Shape::Kind::Hull;
        piece.shape.points = std::move(points);
        piece.shape.rounding = 0.0;
        piece.position = centre;
        piece.canSleep = false;
        const int body = mScene->add(piece);
        const glm::dvec3 dir = glm::length(out) > 1e-9 ? glm::normalize(out) : glm::dvec3(0.0, 1.0, 0.0);
        // The ground is solid below its surface, but its triangles only touch a hull that crosses them.
        auto deepestPoint = [&](double move) {
            double deepest = -1e300;
            for (const glm::dvec3& p : hull)
            {
                const glm::dvec3 q = p + dir * move;
                deepest = std::max(deepest, mGround.heightAt(q.x, q.z) - q.y);
            }
            return deepest;
        };
        auto stillIn = [&](double move) {
            if (deepestPoint(move) > 1e-3)
                return true;
            mScene->setPose(body, {centre + dir * move, glm::dquat(1, 0, 0, 0)});
            return mScene->penetration(body) > 1e-3;
        };
        double nearest = 1e300, farthest = -1e300;
        for (const glm::dvec3& p : hull)
        {
            nearest = std::min(nearest, glm::dot(p, dir));
            farthest = std::max(farthest, glm::dot(p, dir));
        }
        const double span = std::max(1.0, farthest - nearest);
        // Marched out from where it stands: further out the ground can rise again (the next hill), so the first clear
        // pose is the depth.
        const double limit = 2.0 * span + std::max(0.0, deepestPoint(0.0)) / std::max(0.05, dir.y);
        double d = 0.0;
        if (stillIn(0.0))
        {
            const double step = std::max(span / 16.0, limit / 256.0);
            double lo = 0.0, hi = limit;
            for (double move = step; move < limit; move += step)
            {
                if (!stillIn(move))
                {
                    hi = move;
                    break;
                }
                lo = move;
            }
            if (hi < limit)
                while (hi - lo > 0.25)
                {
                    const double mid = 0.5 * (lo + hi);
                    (stillIn(mid) ? lo : hi) = mid;
                }
            d = hi;
        }
        // Bodies can't be removed: each checked piece is parked far below, clear of the next.
        mScene->setPose(body, {glm::dvec3(0.0, -1e7 - 1e5 * double(body), 0.0), glm::dquat(1, 0, 0, 0)});
        return d;
    }

private:
    lila::geo::Heightfield mGround;
    std::unique_ptr<physics::RigidScene> mScene;
};
} // namespace

void registerPenetrationCheck(lila::ext::Context& ctx)
{
    ctx.add(lila::overlap::providers(), "overlapProvider", "stagehand",
            lila::overlap::Provider{.label = "stagehand", .open = [](const lila::geo::Heightfield& h) -> std::unique_ptr<lila::overlap::Session> {
                                        if (h.empty())
                                            return nullptr;
                                        return std::make_unique<GroundSession>(h);
                                    }});
}
} // namespace stagehand
