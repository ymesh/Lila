#pragma once

#include "core/Export.h"
#include "render/FrameState.h"

#include <glm/glm.hpp>

#include <cmath>

namespace lila::render
{
/** How far the eye goes from the origin (cm) before frames are rebased on it; nearer, float world space is already exact. */
inline constexpr double kRebaseBeyond = 1e5;

/**
 * Moves a frame into camera-relative space, in double: the eye goes to the origin, the view loses its translation, and
 * the frame's own world-space data (lights, shadow matrices, image planes, grid origin, fog height) moves with it.
 * renderOrigin gains the old eye. Mesh, strand, heightfield, point and line data stays where it is and is rebased where it
 * is uploaded, with relativeTo(renderOrigin). Running it again on a rebased frame changes nothing.
 */
LILA_UI_API void rebaseOnEye(FrameState& s);

/** A world matrix in the float space centred on origin: translate(-origin) * world, worked out in double. */
inline glm::mat4 relativeTo(const glm::dmat4& world, const glm::dvec3& origin)
{
    glm::dmat4 m = world;
    m[3] -= glm::dvec4(origin * world[3].w, 0.0);
    return glm::mat4(m);
}

/** A world point in the float space centred on origin. */
inline glm::vec3 relativeTo(const glm::dvec3& p, const glm::dvec3& origin) { return glm::vec3(p - origin); }

/** x wrapped into [0, period), in double: a world coordinate for a repeating pattern that keeps its precision. */
inline double wrapped(double x, double period)
{
    const double r = x - period * std::floor(x / period);
    return r >= period ? 0.0 : r;
}
} // namespace lila::render
