#pragma once

#include "render/FrameState.h"

#include <glm/glm.hpp>

#include <memory>

namespace largeworld
{
/** Where the tests put the scene: the origin, a UTM-sized offset (500 km east, 4000 km north) and 3000 km on both axes. */
inline const glm::dvec3 kOffsets[] = {{0.0, 0.0, 0.0}, {500000.0, 0.0, 4000000.0}, {3e8, 0.0, 3e8}};

/** A closed box, half size h, with face normals and wire edges. */
inline std::shared_ptr<lila::render::RenderMesh> box(float h)
{
    using namespace lila::render;
    auto m = std::make_shared<RenderMesh>();
    for (int axis = 0; axis < 3; ++axis)
        for (int sign = -1; sign <= 1; sign += 2)
        {
            glm::vec3 n(0.0f);
            n[axis] = float(sign);
            const glm::vec3 u = axis == 0 ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
            const glm::vec3 v = glm::cross(n, u);
            const uint32_t base = uint32_t(m->vertices.size());
            for (int c = 0; c < 4; ++c)
            {
                MeshVertex vert{};
                vert.pos = (n + u * ((c & 1) ? 1.0f : -1.0f) + v * ((c & 2) ? 1.0f : -1.0f)) * h;
                vert.normal = n;
                vert.tangent = glm::vec4(u, 1.0f);
                m->vertices.push_back(vert);
            }
            m->indices.insert(m->indices.end(), {base, base + 1, base + 3, base, base + 3, base + 2});
            m->edgePoints.insert(m->edgePoints.end(), {m->vertices[base].pos, m->vertices[base + 1].pos, m->vertices[base + 1].pos,
                                                       m->vertices[base + 3].pos});
        }
    m->boundsMin = glm::vec3(-h);
    m->boundsMax = glm::vec3(h);
    return m;
}
} // namespace largeworld
