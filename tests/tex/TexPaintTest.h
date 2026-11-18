#pragma once

#include "TexTest.h"

#include "core/MainQueue.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "ops/Undo.h"
#include "render/Renderer.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace textest
{
inline bool gpu()
{
    render::Renderer& r = render::Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for texture painting tests");
    return false;
}

inline scene::Graph& sg() { return scene::Graph::current(); }

inline void newScene()
{
    run("newScene");
    UndoStack::get().clear();
    mainq::drain();
}

/**
 * Two quads sharing the edge x = 0, as two UV islands far apart: the left quad's seam side lands at u = 0.45, the right
 * quad's (mirrored in u and v) at u = 0.95. 100 units across, facing +Y.
 */
inline glm::vec2 seamUv(int face, double x, double z)
{
    const double t = (z + 50.0) / 100.0;
    if (face == 0)
        return {float(0.05 + (x + 50.0) / 50.0 * 0.4), float(0.9 - t * 0.8)};
    return {float(0.95 - x / 50.0 * 0.4), float(0.1 + t * 0.8)};
}

inline geo::Mesh seamPanel()
{
    const std::vector<glm::dvec3> pts = {{-50, 0, -50}, {0, 0, -50}, {50, 0, -50}, {-50, 0, 50}, {0, 0, 50}, {50, 0, 50}};
    const std::vector<geo::Index> sizes = {4, 4};
    const std::vector<geo::Index> idx = {0, 3, 4, 1, 1, 4, 5, 2};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            for (geo::Index c : m.faceCorners(f))
            {
                const glm::dvec3 p = m.position(m.cornerPoint(c));
                // The shared edge belongs to both faces; each face's own island decides.
                uv[c] = seamUv(int(f), p.x, p.z);
            }
    return m;
}

inline std::string addMesh(const std::string& name, geo::Mesh m)
{
    scene::Node* n = sg().createNode(*scene::types::find("mesh"), name);
    sg().setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return n->path();
}

inline scene::Node& makeSet(const std::string& name, int resolution)
{
    // M_<name> picks it, for the live viewport's maps.
    run("texCreateSet", {{"args", {name}}, {"resolution", resolution}, {"materials", {"M_" + name}}});
    return *tex::findSet(sg(), name);
}

inline json brushJson(double strength = 1.0) { return {{"falloff", "smooth"}, {"strength", strength}, {"flow", 1.0}, {"spacing", 0.15}}; }

inline json paintStroke(const std::string& set, const std::string& layer, const json& stroke)
{
    return run("texPaintStroke", {{"args", {set, layer}}, {"stroke", stroke}});
}

/** The texel under a uv, v up. */
inline const float* at(const image::Image& img, glm::vec2 uv)
{
    const int x = std::clamp(int(uv.x * img.width), 0, img.width - 1), y = std::clamp(int((1.0f - uv.y) * img.height), 0, img.height - 1);
    return &img.rgba[(size_t(y) * img.width + x) * 4];
}
} // namespace textest
