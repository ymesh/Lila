#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Knife.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshEditInternal.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/ObjectSets.h"

#include <map>

namespace lila::scene::detail
{
namespace
{
/** A single knife point on an edge: splits that edge (and its symmetric images) with a new vertex there. */
json insertEdgePoint(Graph& g, Node& n, PlugRef base, geo::Mesh mesh, const geo::KnifePoint& k, const geo::SymmetryMap* sym, bool select)
{
    if (!mesh.edgeValid(k.id))
        throw CommandError("polyKnife: " + n.path() + " has no edge " + std::to_string(k.id));
    const Index a = mesh.edgePoint(k.id, 0);
    const Index b = mesh.edgePoint(k.id, 1);
    // Each image edge is split at the image of the spot, so t flips when the image runs the other way.
    std::vector<std::pair<Index, double>> splits{{k.id, k.t}};
    for (int e = 1; sym && e < sym->order(); ++e)
    {
        const Index ia = sym->image(e, a);
        const Index ib = sym->image(e, b);
        if (ia == geo::kInvalid || ib == geo::kInvalid)
            continue;
        const Index ie = mesh.findEdge(ia, ib);
        if (ie == geo::kInvalid || std::ranges::any_of(splits, [&](const auto& s) { return s.first == ie; }))
            continue;
        splits.emplace_back(ie, mesh.edgePoint(ie, 0) == ia ? k.t : 1.0 - k.t);
    }

    UndoChunk chunk("Knife");
    std::vector<glm::dvec3> before;
    int drive = 0;
    if (sym)
    {
        before.assign(mesh.positions().begin(), mesh.positions().end());
        drive = sym->majorityWedge(Ids{a, b});
    }
    const Index firstNew = mesh.pointCapacity();
    Ids points, edges;
    for (const auto& [e, t] : splits)
    {
        points.push_back(mesh.splitEdge(e, t));
        edges.push_back(mesh.lastSplitEdge());
    }
    geo::clearSymmetryKeys(mesh, firstNew);
    if (sym)
        maintainSymmetry(n, mesh, before, drive);
    geo::MeshRemap remap;
    mesh.compact(&remap);
    Ids mappedPoints = remapIds(points, remap.points);
    Ids mappedEdges = remapIds(edges, remap.edges);
    auto edited = std::make_shared<geo::Mesh>(std::move(mesh));
    g.setValue(base, MeshPtr(edited));
    modifiers::baseRemapped(g, n.id(), remap, *edited);
    objectSets::meshRemapped(g, n.id(), remap);
    if (select)
    {
        components::setAll({{n.id(), mappedPoints}}, {}, {});
        components::setMode(ComponentMode::Vertex);
    }
    return mappedEdges;
}
} // namespace

void registerMeshEditKnifeCommands()
{
    using T = ArgType;
    using K = ComponentMode;

    ops::registerCommand({
        .name = "polyKnife",
        .doc = "Knife / multi-cut: cut a mesh along a path of points, splitting the faces between them and the edges the path "
               "crosses. Between two points the cut follows the surface under the straight screen line joining them (seen from "
               "eye= or along direction=; without either, along the points' average normal). With loopEdge= it inserts an edge "
               "loop across that edge's quad ring instead, and with deleteEdge= it dissolves that edge. Follows the mesh's "
               "symmetry. A single point on an edge inserts a vertex there. Returns the new edges.",
        .flags = {nodeFlag({"mesh", "", T::String, nullptr, "Mesh to cut; default: the selected or hilited mesh."}, "mesh"),
                  {"points", "", T::List, nullptr,
                   "Path points: {\"vertex\": id}, {\"edge\": id, \"t\": 0..1 from its first point}, or {\"face\": id, \"position\": "
                   "[x, y, z] world}."},
                  {"closed", "", T::Bool, false, "Also cut from the last point back to the first."},
                  {"through", "", T::Bool, false, "Cut every face under the line, hidden faces too."},
                  {"eye", "", T::Vec3, nullptr, "World eye of a perspective view."},
                  {"direction", "", T::Vec3, nullptr, "World view direction of an orthographic view."},
                  {"loopEdge", "", T::Int, -1, "Insert an edge loop across this edge's ring instead of cutting a path.", {}, {}, -1.0},
                  {"fraction", "", T::Float, 0.5, "Where along loopEdge the loop crosses it, from its first point.", {}, {}, 0.0, 1.0},
                  {"deleteEdge", "", T::Int, -1, "Dissolve this edge, joining its two faces, instead of cutting.", {}, {}, -1.0},
                  {"deleteLoop", "", T::Bool, false, "With deleteEdge, dissolve the whole edge loop through it."},
                  {"select", "", T::Bool, true, "Select the new edges."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            Node& n = meshFlag(a, "polyKnife");
            const std::string name = n.path();
            PlugRef base = basePlug(n);
            geo::Mesh mesh = baseMesh(base);
            const glm::dmat4 world = n.as<glm::dmat4>(Transform::kWorldMatrix);
            const glm::dmat4 toLocal = glm::inverse(world);
            auto sym = symmetryFor(n, mesh);

            if (const int deleteEdge = a.get<int>("deleteEdge"); deleteEdge >= 0)
            {
                if (!mesh.edgeValid(Index(deleteEdge)))
                    throw CommandError("polyKnife: " + name + " has no edge " + std::to_string(deleteEdge));
                const bool loop = a.get<bool>("deleteLoop");
                UndoChunk chunk(loop ? "Delete Edge Loop" : "Delete Edge");
                const std::vector<Index> picked = loop ? geo::edgeLoop(mesh, Index(deleteEdge)) : std::vector<Index>{Index(deleteEdge)};
                std::vector<glm::dvec3> before;
                int drive = 0;
                if (sym)
                {
                    before.assign(mesh.positions().begin(), mesh.positions().end());
                    drive = sym->majorityWedge(components::convert(mesh, K::Edge, picked, K::Vertex));
                }
                geo::dissolveEdges(mesh, withMirror(mesh, sym ? &*sym : nullptr, picked, K::Edge), true);
                if (sym)
                    maintainSymmetry(n, mesh, before, drive);
                geo::MeshRemap remap;
                mesh.compact(&remap);
                const std::vector<Index>* maps[3] = {&remap.points, &remap.edges, &remap.faces};
                std::map<NodeId, Ids> sel[3] = {components::selected(K::Vertex), components::selected(K::Edge), components::selected(K::Face)};
                for (int k = 0; k < 3; ++k)
                    if (auto it = sel[k].find(n.id()); it != sel[k].end())
                        if (Ids kept = remapIds(it->second, *maps[k]); kept.empty())
                            sel[k].erase(it);
                        else
                            it->second = std::move(kept);
                auto edited = std::make_shared<geo::Mesh>(std::move(mesh));
                g.setValue(base, MeshPtr(edited));
                modifiers::baseRemapped(g, n.id(), remap, *edited);
                objectSets::meshRemapped(g, n.id(), remap);
                components::setAll(sel[0], sel[1], sel[2]);
                return json::array();
            }

            geo::KnifePlan plan;
            const int loopEdge = a.get<int>("loopEdge");
            std::vector<geo::KnifePoint> pts;
            if (loopEdge >= 0)
            {
                if (!mesh.edgeValid(Index(loopEdge)))
                    throw CommandError("polyKnife: " + name + " has no edge " + std::to_string(loopEdge));
                plan = geo::knifeLoopPlan(mesh, Index(loopEdge), a.get<double>("fraction"), sym ? &*sym : nullptr);
            }
            else
            {
                for (const json& p : a.flags.value("points", json::array()))
                {
                    if (!p.is_object())
                        throw CommandError("polyKnife: each point is {vertex:}, {edge:, t:} or {face:, position:}");
                    if (p.contains("vertex"))
                        pts.push_back(geo::KnifePoint::point(p["vertex"].get<Index>()));
                    else if (p.contains("edge"))
                        pts.push_back(geo::KnifePoint::edge(p["edge"].get<Index>(), std::clamp(p.value("t", 0.5), 0.0, 1.0)));
                    else if (p.contains("face") && p.contains("position"))
                        pts.push_back(geo::KnifePoint::face(p["face"].get<Index>(), glm::dvec3(toLocal * glm::dvec4(vec(p["position"]), 1.0))));
                    else
                        throw CommandError("polyKnife: each point is {vertex:}, {edge:, t:} or {face:, position:}");
                }
                if (pts.size() == 1 && pts[0].kind == geo::KnifePoint::Kind::Edge)
                    return insertEdgePoint(g, n, base, std::move(mesh), pts[0], sym ? &*sym : nullptr, a.get<bool>("select"));
                if (pts.size() < 2)
                    throw CommandError("polyKnife: give at least two points, or one on an edge");
                geo::KnifeOptions o;
                o.closed = a.get<bool>("closed");
                o.through = a.get<bool>("through");
                if (a.has("eye"))
                    o.eye = glm::dvec3(toLocal * glm::dvec4(vec(a.flags["eye"]), 1.0));
                else if (a.has("direction"))
                    o.viewDir = glm::inverse(glm::dmat3(world)) * vec(a.flags["direction"]);
                o.symmetry = sym ? &*sym : nullptr;
                plan = geo::knifePlan(mesh, pts, o);
            }

            UndoChunk chunk("Knife");
            std::vector<glm::dvec3> before;
            int drive = 0;
            if (sym)
            {
                before.assign(mesh.positions().begin(), mesh.positions().end());
                Ids touched;
                for (const geo::KnifeSegment& s : plan.segments)
                    if (!s.mirrored)
                        for (Index p : mesh.facePoints(s.face))
                            touched.push_back(p);
                drive = sym->majorityWedge(touched);
            }
            const Index firstNew = mesh.pointCapacity();
            Ids cut = geo::knifeApply(mesh, plan);
            if (cut.empty())
                throw CommandError("polyKnife: the path crosses no faces it can cut");
            geo::clearSymmetryKeys(mesh, firstNew);
            if (sym)
                maintainSymmetry(n, mesh, before, drive);
            geo::MeshRemap remap;
            mesh.compact(&remap);
            Ids mapped = remapIds(cut, remap.edges);
            auto edited = std::make_shared<geo::Mesh>(std::move(mesh));
            g.setValue(base, MeshPtr(edited));
            modifiers::baseRemapped(g, n.id(), remap, *edited);
            if (a.get<bool>("select"))
            {
                components::setAll({}, {{n.id(), mapped}}, {});
                if (components::mode() == ComponentMode::Object)
                    components::setMode(ComponentMode::Edge);
            }
            return mapped;
        },
    });
}
} // namespace lila::scene::detail
