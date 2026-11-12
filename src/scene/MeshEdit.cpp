#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "scene/MeshEditInternal.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/ObjectSets.h"
#include "scene/SoftSelect.h"

#include <cstring>
#include <format>
#include <map>

namespace lila::scene::detail
{
/** Direct edits work on the base mesh under any modifiers; a procedural source is frozen first. */
PlugRef basePlug(Node& n)
{
    return modifiers::editPlug(graph(), n.id());
}

geo::Mesh baseMesh(const PlugRef& p)
{
    graph().evaluate();
    const MeshPtr& m = std::get<MeshPtr>(graph().storedValue(p));
    return m ? *m : geo::Mesh();
}

int symmetryAxis(const Node& n)
{
    int64_t axis = n.as<int64_t>(MeshNode::kSymmetry);
    return axis >= 1 && axis <= 3 ? int(axis - 1) : -1;
}

/** Only for matching points that have no stored partner; stored pairs survive any drift. */
double symmetryTolerance(const geo::Mesh& mesh)
{
    return std::max(1e-4, glm::length(mesh.bounds().size()) * 1e-5);
}

std::string symmetryLayer(const geo::SymmetrySpec& spec)
{
    return spec.radial ? geo::radialLayerName(*spec.radial) : geo::symmetryLayerName(spec.axis);
}

std::string symmetryName(const geo::SymmetrySpec& spec)
{
    return spec.radial ? symmetryLabel(spec) : std::string(1, char('X' + spec.axis));
}

namespace
{
void reportSync(const Node& n, const geo::SymmetrySpec& spec, const geo::SymmetrySync& s, bool hadLayer)
{
    const Index rematched = s.topological + s.byPosition;
    if (rematched == 0)
        return;
    if (hadLayer)
        log::write(LogLevel::Warning, std::format("{}: symmetry {} re-matched {} points without a stored partner ({} by position), {} unmatched",
                                                  n.path(), symmetryName(spec), rematched, s.byPosition, s.unmatched));
    else
        log::write(LogLevel::Info, std::format("{}: symmetry {} paired {} points, {} unmatched", n.path(), symmetryName(spec), rematched, s.unmatched));
}
} // namespace

/** The mesh's persistent symmetry pairs (or radial orbits), stored onto it if it has none yet. */
std::optional<geo::SymmetryMap> symmetryFor(const Node& n, geo::Mesh& mesh)
{
    const std::optional<geo::SymmetrySpec> spec = symmetrySpec(n);
    if (!spec)
        return std::nullopt;
    const bool hadLayer = mesh.pointAttrs().has(symmetryLayer(*spec));
    geo::SymmetrySync sync;
    geo::SymmetryMap map = geo::SymmetryMap::persistent(mesh, *spec, symmetryTolerance(mesh), &sync);
    reportSync(n, *spec, sync, hadLayer);
    return map;
}

/**
 * After a topology op: pairs the points it created (every wedge was made together) and sets every moved or new
 * point's orbit to the exact images of its member in the driving wedge.
 */
void maintainSymmetry(const Node& n, geo::Mesh& mesh, const std::vector<glm::dvec3>& before, int driveWedge)
{
    LILA_ZONE();

    const std::optional<geo::SymmetrySpec> spec = symmetrySpec(n);
    if (!spec)
        return;
    const Index firstNew = Index(before.size());
    geo::SymmetryMap map = geo::SymmetryMap::persistent(mesh, *spec, symmetryTolerance(mesh));
    Ids touched;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && (p >= firstNew || std::memcmp(&before[p], &mesh.position(p), sizeof(glm::dvec3)) != 0))
            touched.push_back(p);
    map.enforceWedge(mesh, touched, driveWedge);
}

Ids remapIds(const Ids& ids, const std::vector<Index>& map)
{
    Ids out;
    for (Index i : ids)
        if (i < map.size() && map[i] != geo::kInvalid)
            out.push_back(map[i]);
    return out;
}

namespace
{
const Node* gEditNode = nullptr;
}

glm::dmat3 editSpace()
{
    return gEditNode ? glm::dmat3(gEditNode->as<glm::dmat4>(Transform::kWorldMatrix)) : glm::dmat3(1.0);
}

/**
 * Runs `fn` on every mesh with a component selection, converted to `kind`. Each mesh is edited on a copy, compacted,
 * written back to inMesh (undoable) and its component selection replaced by what the op returns.
 */
json editSelected(const char* label, ComponentMode kind, const EditFn& fn, bool allowWhole,
                  const std::function<bool(const Node&)>& handledWhole)
{
    LILA_ZONE();

    Graph& g = graph();
    g.evaluate();
    std::map<NodeId, Ids> targets;
    ComponentMode from = components::mode();
    if (from != ComponentMode::Object)
        for (auto& [node, ids] : components::selected(from))
            if (Node* n = g.node(node); n && n->type().isA("mesh"))
                if (MeshPtr cage = modifiers::cage(g, node))
                    targets[node] = components::convert(*cage, from, ids, kind);
    bool handled = false;
    if (targets.empty() && allowWhole)
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("mesh"))
            {
                if (handledWhole && handledWhole(*n))
                    handled = true;
                else
                    targets[id] = {};
            }
    if (targets.empty() && handled)
        return nullptr;
    if (targets.empty())
        throw CommandError(std::string(label) + ": no components selected");

    UndoChunk chunk(label);
    std::map<NodeId, Ids> newSel[3] = {components::selected(ComponentMode::Vertex), components::selected(ComponentMode::Edge),
                                       components::selected(ComponentMode::Face)};
    ComponentMode resultKind = ComponentMode::Object;
    for (auto& [node, sel] : targets)
    {
        Node& n = *g.node(node);
        PlugRef base = basePlug(n);
        geo::Mesh mesh = baseMesh(base);
        auto sym = symmetryFor(n, mesh);
        std::vector<glm::dvec3> before;
        int drive = 0;
        if (sym)
        {
            before.assign(mesh.positions().begin(), mesh.positions().end());
            drive = sym->majorityWedge(components::convert(mesh, kind, sel, ComponentMode::Vertex));
        }
        const Index firstNew = mesh.pointCapacity();
        EditResult r = [&] {
            gEditNode = &n;
            struct Reset
            {
                ~Reset() { gEditNode = nullptr; }
            } reset;
            return fn(mesh, sel, sym ? &*sym : nullptr);
        }();
        geo::clearSymmetryKeys(mesh, firstNew);
        if (sym)
            maintainSymmetry(n, mesh, before, drive);
        geo::MeshRemap remap;
        mesh.compact(&remap);
        const std::vector<Index>* maps[3] = {&remap.points, &remap.edges, &remap.faces};
        // The other kinds' selections follow the new ids, so they come back when their mode does.
        for (int k = 0; k < 3; ++k)
            if (auto it = newSel[k].find(node); it != newSel[k].end())
                if (Ids kept = remapIds(it->second, *maps[k]); kept.empty())
                    newSel[k].erase(it);
                else
                    it->second = std::move(kept);
        if (r.kind != ComponentMode::Object)
        {
            resultKind = r.kind;
            Ids mapped = remapIds(r.ids, *maps[int(r.kind) - 1]);
            if (mapped.empty())
                newSel[int(r.kind) - 1].erase(node);
            else
                newSel[int(r.kind) - 1][node] = std::move(mapped);
        }
        auto edited = std::make_shared<geo::Mesh>(std::move(mesh));
        g.setValue(base, MeshPtr(edited));
        modifiers::baseRemapped(g, node, remap, *edited);
        objectSets::meshRemapped(g, node, remap);
    }
    components::setAll(newSel[0], newSel[1], newSel[2]);
    // A component mode is the user's: repeated edits (bridge after bridge) must not bounce it to the result's kind.
    if (resultKind != ComponentMode::Object && from == ComponentMode::Object)
        components::setModeUndoable(resultKind);
    return nullptr;
}

Ids withMirror(const geo::Mesh& m, const geo::SymmetryMap* s, const Ids& ids, ComponentMode kind)
{
    if (!s)
        return ids;
    switch (kind)
    {
    case ComponentMode::Vertex: return s->expandPoints(ids);
    case ComponentMode::Edge: return s->expandEdges(m, ids);
    case ComponentMode::Face: return s->expandFaces(m, ids);
    default: return ids;
    }
}

Node& meshArg(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}
/** The mesh= flag's mesh, else the first selected or hilited mesh. */
Node& meshFlag(const Args& a, const char* cmd)
{
    Graph& g = graph();
    std::string name = a.get<std::string>("mesh", "");
    if (name.empty())
    {
        std::vector<NodeId> candidates = g.selection();
        for (auto& [node, ids] : components::selected(components::mode()))
            candidates.push_back(node);
        for (NodeId id : candidates)
            if (Node* n = g.node(id); n && n->type().isA("mesh"))
                return *n;
        throw CommandError(std::string(cmd) + ": give mesh= or select a mesh");
    }
    return meshArg(name, cmd);
}
} // namespace lila::scene::detail

namespace lila::scene
{
void registerMeshEditCommands()
{
    softSelect::registerCommands();

    detail::registerMeshEditComponentCommands();
    detail::registerMeshEditMeshOpsCommands();
    detail::registerMeshEditTransformCommands();
    detail::registerMeshEditSymmetryCommands();
    detail::registerMeshEditKnifeCommands();
    detail::registerMeshEditNormalsCommands();

    for (const char* name : {"polyExtrude", "polyInset", "polyBevel", "polyInsertLoop", "polySplitRings", "polyOffsetLoop", "polyMerge", "polyBridge", "polyGridFill", "polyQuadrangulate",
                             "polySubdivide", "polyMirror", "polyTriangulate", "polyReduce", "polySoftEdge", "polyCrease", "polyBevelWeight"})
        ops::setAdjustable(name);
    {
        namespace opt = ops::options;
        opt::Override loop;
        loop.ranges = {{"fraction", {0.0, 1.0, 0.01, ""}}, {"count", {1.0, 32.0, 1.0, ""}}, {"spacing", {0.0, 2.0, 0.05, ""}}};
        opt::setOverride("polyInsertLoop", std::move(loop));
        opt::Override rings;
        rings.ranges = {{"count", {1.0, 32.0, 1.0, ""}}};
        rings.labels = {{"edgeFlow", "Adjust Edge Flow"}};
        opt::setOverride("polySplitRings", std::move(rings));
        opt::Override offset;
        offset.ranges = {{"distance", {0.0, 1e6, 0.01, ""}}};
        offset.labels = {{"absolute", "Distance in cm"}, {"deleteEdge", "Delete Selected Edges"}};
        opt::setOverride("polyOffsetLoop", std::move(offset));
        opt::Override bridge;
        bridge.order = {"curveType", "divisions", "smoothness", "tensionSource", "tensionTarget", "curve"};
        for (const char* f : {"curveType", "divisions", "smoothness", "tensionSource", "tensionTarget", "curve"})
            bridge.sections[f] = "Shape";
        for (const char* f : {"twist", "taper", "bias", "profileFactor", "profileShape"})
            bridge.sections[f] = "Form";
        for (const char* f : {"bridgeOffset", "directionSource", "directionTarget", "startSource", "startTarget", "connect"})
            bridge.sections[f] = "Pairing";
        for (const char* f : {"weldEnds", "weldTolerance", "transition", "relax", "smoothingAngle"})
            bridge.sections[f] = "Ends and Edges";
        for (const char* f : {"merge", "mergeFactor"})
            bridge.sections[f] = "Merge";
        bridge.choices["curveType"] = {{"auto", "Auto"}, {"blend", "Blend"}, {"linear", "Linear"}, {"path", "Path"}};
        bridge.choices["connect"] = {{"pairs", "Loop Pairs"}, {"chain", "Open Chain"}, {"chainClosed", "Closed Chain"}};
        bridge.choices["transition"] = {{"quads", "Quads"}, {"triangles", "Triangles"}};
        bridge.labels = {{"divisions", "Divisions (-1 Auto)"}, {"curve", "Path Curve"}, {"bridgeOffset", "Bridge Offset"},
                         {"directionSource", "Reverse Source"}, {"directionTarget", "Reverse Target"}, {"profileFactor", "Profile"},
                         {"weldEnds", "Weld Open Ends"}};
        bridge.ranges["twist"] = {-720.0, 720.0, 1.0, " deg"};
        bridge.ranges["smoothingAngle"] = {0.0, 180.0, 1.0, " deg"};
        bridge.popupHidden = {"curve", "startSource", "startTarget", "connect", "weldTolerance"};
        bridge.docs = "bridge";
        opt::setOverride("polyBridge", std::move(bridge));
    }
    // Recording the last op is what makes these adjustable, in any host and not only once the UI installs it.
    lastOp::install();
}
} // namespace lila::scene
