#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string>

namespace lila::geo
{
struct Heightfield;
}

namespace lila::overlap
{
/** How deep convex pieces sit in one ground, asked many times. */
class Session
{
public:
    virtual ~Session() = default;
    /**
     * How far, cm, the convex hull of these points must move along out (unit) to clear the ground, both in the ground's
     * own space; 0 when clear. The ground counts as solid below its surface. The first clear pose counts: a hull still in
     * the ground at twice its own extent along out (plus its distance under the surface) returns that limit.
     */
    virtual double depth(std::span<const glm::dvec3> hull, const glm::dvec3& out) = 0;
};

/**
 * A penetration check other extensions can use without linking its owner: Stagehand registers its physics one, so
 * Terrain's cliff kits reject over-buried pieces the way dressCheck finds overlaps.
 */
struct Provider
{
    /** For reports: what measured the depth. */
    std::string label;
    std::function<std::unique_ptr<Session>(const geo::Heightfield& ground)> open;
};

LILA_SCENE_API Registry<Provider>& providers();

/** A session from the first provider registered, or null when none is (the caller says what it used instead). */
LILA_SCENE_API std::unique_ptr<Session> open(const geo::Heightfield& ground, std::string* label = nullptr);
} // namespace lila::overlap
