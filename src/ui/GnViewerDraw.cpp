#include "ui/GnViewerDraw.h"

#include "core/Profile.h"
#include "gn/Gn.h"
#include "gn/Viewer.h"
#include "gn/Volume.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/ViewportHooks.h"

#include <map>

namespace lila::ui
{
namespace
{
constexpr float kFillAlpha = 0.85f;
const glm::vec3 kPlain(0.75f);

void drawViewers(render::FrameState& s, OverlayPainter&, const ViewInfo&)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    for (const auto& [modifier, data] : gn::viewerSnapshots())
    {
        if (!data || !data->mesh || !g.node(modifier))
            continue;
        scene::NodeId meshId = scene::modifiers::meshOf(g, modifier);
        scene::Node* meshNode = g.node(meshId);
        glm::dmat4 world = meshNode ? meshNode->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
        const geo::Mesh& m = *data->mesh;
        auto at = [&](geo::Index i) -> glm::vec3 {
            if (!data->hasValues || i >= data->values.size())
                return kPlain;
            return gn::viewerColor(*data, data->values[i]);
        };
        auto wp = [&](geo::Index p) { return glm::dvec3(world * glm::dvec4(m.position(p), 1.0)); };
        // Per-element colour of a corner of a face, by the domain the values live on.
        auto cornerColor = [&](geo::Index f, geo::Index c) {
            switch (data->domain)
            {
            case gn::Domain::Face: return at(f);
            case gn::Domain::Corner: return at(c);
            case gn::Domain::Point: return at(m.cornerPoint(c));
            default: return kPlain;
            }
        };
        bool anyFace = false;
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            anyFace = true;
            std::vector<geo::Index> corners;
            m.forEachFaceCorner(f, [&](geo::Index c) { corners.push_back(c); });
            for (size_t k = 1; k + 1 < corners.size(); ++k)
                for (geo::Index c : {corners[0], corners[k], corners[k + 1]})
                    s.tris3d.push_back({wp(m.cornerPoint(c)), glm::vec4(cornerColor(f, c), kFillAlpha)});
        }
        for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        {
            if (!m.edgeValid(e))
                continue;
            glm::vec3 col = data->domain == gn::Domain::Edge ? at(e) : glm::vec3(0.1f);
            float alpha = data->domain == gn::Domain::Edge ? 1.0f : 0.5f;
            s.linesOnTop.push_back({wp(m.edgePoint(e, 0)), glm::vec4(col, alpha)});
            s.linesOnTop.push_back({wp(m.edgePoint(e, 1)), glm::vec4(col, alpha)});
        }
        if (anyFace)
            continue;
        // Point clouds and loose points: a small cross each, sized from the point spread.
        glm::dvec3 lo(1e30), hi(-1e30);
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
            {
                lo = glm::min(lo, m.position(p));
                hi = glm::max(hi, m.position(p));
            }
        double r = std::max(0.2, glm::length(hi - lo) * 0.004);
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        {
            if (!m.pointValid(p))
                continue;
            glm::vec4 col(data->domain == gn::Domain::Point ? at(p) : kPlain, 1.0f);
            for (int axis = 0; axis < 3; ++axis)
            {
                glm::dvec3 d(0.0);
                d[axis] = r;
                s.linesOnTop.push_back({glm::dvec3(world * glm::dvec4(m.position(p) - d, 1.0)), col});
                s.linesOnTop.push_back({glm::dvec3(world * glm::dvec4(m.position(p) + d, 1.0)), col});
            }
        }
    }
}
/** The surface a volume shows in the viewport: zero for a signed distance, a tenth of the peak for a density. */
std::shared_ptr<const geo::Mesh> volumePreview(const std::shared_ptr<const gn::Volume>& v)
{
    static std::map<const gn::Volume*, std::pair<std::weak_ptr<const gn::Volume>, std::shared_ptr<const geo::Mesh>>> cache;
    std::erase_if(cache, [](const auto& e) { return e.second.first.expired(); });
    if (auto it = cache.find(v.get()); it != cache.end())
        return it->second.second;
    float peak = 0.0f;
    for (float x : v->values)
        peak = std::max(peak, x);
    double threshold = v->isSdf() ? 0.0 : std::max(1e-6, 0.1 * double(peak));
    auto mesh = std::make_shared<const geo::Mesh>(gn::volumeToMesh(*v, threshold));
    cache[v.get()] = {v, mesh};
    return mesh;
}

/** Each gn modifier's volume as a see-through surface inside its grid's box; volumes never reach the output mesh. */
void drawVolumes(render::FrameState& s, OverlayPainter&, const ViewInfo&)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    for (const auto& [modifier, geometry] : gn::modifierGeometries())
    {
        if (!geometry || !geometry->volume || geometry->volume->empty() || !g.node(modifier))
            continue;
        scene::Node* meshNode = g.node(scene::modifiers::meshOf(g, modifier));
        if (!meshNode)
            continue;
        glm::dmat4 world = meshNode->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        const gn::Volume& v = *geometry->volume;
        const glm::vec4 tint = v.isSdf() ? glm::vec4(0.55f, 0.75f, 0.95f, 0.45f) : glm::vec4(0.85f, 0.85f, 0.85f, 0.3f);
        const geo::Mesh& m = *volumePreview(geometry->volume);
        auto wp = [&](const glm::dvec3& p) { return glm::dvec3(world * glm::dvec4(p, 1.0)); };
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            std::vector<geo::Index> corners;
            m.forEachFaceCorner(f, [&](geo::Index c) { corners.push_back(m.cornerPoint(c)); });
            for (size_t k = 1; k + 1 < corners.size(); ++k)
                for (geo::Index p : {corners[0], corners[k], corners[k + 1]})
                    s.tris3d.push_back({wp(m.position(p)), tint});
        }
        // The grid's box, through its own transform so a rotated grid shows rotated.
        glm::dvec3 hi = glm::dvec3(v.dims) - 0.5;
        glm::dvec3 corner[8];
        for (int k = 0; k < 8; ++k)
            corner[k] = v.worldOf({k & 1 ? hi.x : -0.5, k & 2 ? hi.y : -0.5, k & 4 ? hi.z : -0.5});
        const glm::vec4 edge(0.9f, 0.9f, 0.9f, 0.6f);
        for (int a = 0; a < 8; ++a)
            for (int bit : {1, 2, 4})
                if (!(a & bit))
                {
                    s.linesOnTop.push_back({wp(corner[a]), edge});
                    s.linesOnTop.push_back({wp(corner[a | bit]), edge});
                }
    }
}
} // namespace

void registerGnViewerDrawer()
{
    viewportHooks::addDrawer("lila.gnViewer", drawViewers);
    viewportHooks::addDrawer("lila.gnVolumes", drawVolumes);
}
} // namespace lila::ui
