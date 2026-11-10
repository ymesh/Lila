#include "Plates.h"

#include <lila/MeshOps.h>
#include <lila/Profile.h>

#include <algorithm>
#include <format>
#include <map>
#include <unordered_set>

namespace plating
{
using namespace lila;
using geo::Index;

PanelValues panelValues(const json& v)
{
    PanelValues p;
    if (!v.is_object())
        return p;
    p.hidden = v.value("hidden", false);
    if (v.contains("elevation") && v["elevation"].is_number())
        p.elevation = v["elevation"].get<double>();
    if (v.contains("thickness") && v["thickness"].is_number())
        p.thickness = v["thickness"].get<double>();
    return p;
}

namespace
{
/** Edges of the cap faces that border a face outside the cap: where the plate's top meets its sides. */
std::vector<Index> capBorder(const geo::Mesh& m, const std::unordered_set<Index>& cap)
{
    std::vector<Index> out;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        int inCap = 0, outside = 0;
        m.forEachEdgeCorner(e, [&](Index c) { (cap.count(m.cornerFace(c)) ? inCap : outside) += 1; });
        if (inCap == 1 && outside == 1)
            out.push_back(e);
    }
    return out;
}

/** One panel's faces as a plate: moved out by its elevation, extruded by its thickness, lipped and bevelled. */
geo::Mesh plate(const geo::Mesh& surface, const std::vector<Index>& faces, const PlateOptions& o, double elevation, double thickness)
{
    geo::Mesh p = geo::extractFaces(surface, faces);
    if (elevation != 0.0)
    {
        const std::vector<glm::vec3> n = geo::pointNormals(p);
        for (Index i = 0; i < p.pointCapacity(); ++i)
            if (p.pointValid(i))
                p.setPosition(i, p.position(i) + glm::dvec3(n[i]) * elevation);
    }
    std::vector<Index> all;
    for (Index f = 0; f < p.faceCapacity(); ++f)
        if (p.faceValid(f))
            all.push_back(f);
    geo::ExtrudeOptions ex;
    ex.offset = thickness;
    ex.keepBack = o.closed;
    const geo::ExtrudeResult r = geo::extrudeFaces(p, all, ex);
    std::unordered_set<Index> cap(r.faces.begin(), r.faces.end());

    if (o.lipWidth > 0.0)
    {
        geo::InsetOptions in;
        in.thickness = o.lipWidth;
        in.depth = -std::abs(o.lipDepth);
        const geo::ExtrudeResult lip = geo::insetFaces(p, r.faces, in);
        cap.insert(lip.sides.begin(), lip.sides.end());
    }
    if (o.bevel > 0.0)
    {
        const std::vector<Index> border = capBorder(p, cap);
        geo::BevelOptions bv;
        bv.offset = o.bevel;
        bv.segments = std::max(1, o.bevelSegments);
        bv.smoothingAngle = o.hardAngle;
        geo::bevelEdges(p, border, bv);
    }
    geo::setHardEdgesByAngle(p, o.hardAngle);
    return p;
}
} // namespace

PlatesResult makePlates(const layout::Resolved& r, const PlateOptions& o)
{
    LILA_ZONE();
    PlatesResult out;
    if (!r.mesh() || r.runs().empty())
        return out;

    geo::Mesh cut = *r.mesh();
    const geo::MeshBvh original(*r.mesh());
    const double half = std::max(0.0, o.gap * 0.5);
    for (const layout::ResolvedRun& run : r.runs())
    {
        if (half > 0.0)
        {
            layout::cutRun(cut, *r.mesh(), original, run, half, half);
            layout::cutRun(cut, *r.mesh(), original, run, -half, half);
        }
        else
            layout::cutRun(cut, *r.mesh(), original, run, 0.0, 0.0);
    }

    // Faces whose middle lies within the gap belong to no plate; the rest go to the panel they lie in.
    std::map<int, std::vector<Index>> byPanel;
    size_t unplaced = 0;
    for (Index f = 0; f < cut.faceCapacity(); ++f)
    {
        if (!cut.faceValid(f))
            continue;
        const glm::dvec3 c = cut.faceCentroid(f);
        if (half > 0.0)
            if (const layout::LineSample line = r.lineAt(c, half); line.run >= 0 && line.distance < half * 0.999)
                continue;
        const layout::PanelSample panel = r.panelAt(c);
        if (panel.panel < 0)
        {
            ++unplaced;
            continue;
        }
        byPanel[panel.panel].push_back(f);
    }

    const std::vector<layout::PanelInfo>& panels = r.panels();
    const auto& overrides = r.layout().panels;
    for (const auto& [index, faces] : byPanel)
    {
        const layout::PanelInfo& info = panels[size_t(index)];
        const PanelValues v = info.override >= 0 && size_t(info.override) < overrides.size() ? panelValues(overrides[size_t(info.override)].values)
                                                                                              : PanelValues{};
        if (v.hidden)
            continue;
        const double thickness = v.thickness.value_or(o.thickness);
        if (thickness <= 0.0)
            continue;
        out.mesh.append(plate(cut, faces, o, v.elevation.value_or(o.elevation), thickness));
        ++out.plates;
    }
    if (o.bevel > 0.0 && o.bevel * 2.0 > o.thickness)
        out.warnings.push_back(std::format("Bevel {:.3g} cm is over half the plate thickness {:.3g} cm; plate edges may overlap", o.bevel, o.thickness));
    if (unplaced)
        out.warnings.push_back(std::format("{} faces found no panel and were left out", unplaced));
    return out;
}
} // namespace plating
