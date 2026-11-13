#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Knife.h"
#include "geo/LoopTools.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "scene/AutoBevel.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/LastOp.h"
#include "scene/MeshEditInternal.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/ObjectSets.h"
#include "scene/RigUtil.h"
#include "scene/SoftSelect.h"

#include <format>
#include <map>
#include <set>

namespace lila::scene::detail
{
namespace
{
/**
 * Maya's default for a component edit run in object mode: every component of kind on the selected meshes gets selected
 * and kind becomes the mode, so Extrude on an object extrudes all its faces.
 */
void selectWholeInObjectMode(ComponentMode kind)
{
    if (components::mode() != ComponentMode::Object)
        return;
    Graph& g = graph();
    g.evaluate();
    std::map<NodeId, Ids> all;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("mesh"))
            if (MeshPtr cage = modifiers::cage(g, id))
            {
                const geo::Mesh& m = *cage;
                Ids ids;
                const Index count = kind == ComponentMode::Vertex ? m.pointCapacity() : kind == ComponentMode::Edge ? m.edgeCapacity() : m.faceCapacity();
                for (Index i = 0; i < count; ++i)
                    if (kind == ComponentMode::Vertex ? m.pointValid(i) : kind == ComponentMode::Edge ? m.edgeValid(i) : m.faceValid(i))
                        ids.push_back(i);
                if (!ids.empty())
                    all[id] = std::move(ids);
            }
    if (all.empty())
        return;
    components::setAll(kind == ComponentMode::Vertex ? all : std::map<NodeId, Ids>{}, kind == ComponentMode::Edge ? all : std::map<NodeId, Ids>{},
                       kind == ComponentMode::Face ? all : std::map<NodeId, Ids>{});
    components::setModeUndoable(kind);
}

/** The named curve's first spline as a polyline in the selected mesh's space, for polyBridge's Path. */
std::vector<glm::dvec3> bridgePath(const std::string& name)
{
    if (name.empty())
        return {};
    Graph& g = graph();
    g.evaluate();
    Node* c = g.find(name);
    if (!c)
        throw CommandError("polyBridge: no curve " + name);
    const curve::Splines& s = curve::splinesOf(c->value(curve::CurveShape::kWorldSpace));
    if (s.empty())
        throw CommandError("polyBridge: " + name + " has no curve");
    const curve::Polyline pl = curve::tessellate(s.front());
    glm::dmat4 toLocal(1.0);
    const auto sel = components::selected(components::mode());
    if (sel.size() == 1)
        if (Node* n = g.node(sel.begin()->first))
            toLocal = glm::inverse(rigutil::worldOf(*n));
    std::vector<glm::dvec3> out;
    for (const glm::dvec3& p : pl.points)
        out.push_back(glm::dvec3(toLocal * glm::dvec4(p, 1.0)));
    return out;
}

/** Edges with exactly one of the faces on them: the outline of each region the faces make. */
Ids regionBorderEdges(const geo::Mesh& m, const Ids& faces)
{
    std::set<Index> sel(faces.begin(), faces.end());
    std::set<Index> out;
    for (Index f : faces)
        if (m.faceValid(f))
            m.forEachFaceCorner(f, [&](Index c) {
                const Index e = m.cornerEdge(c);
                int count = 0;
                m.forEachEdgeCorner(e, [&](Index ec) { count += sel.count(m.cornerFace(ec)) ? 1 : 0; });
                if (count == 1)
                    out.insert(e);
            });
    return Ids(out.begin(), out.end());
}

/** polyExtrude duplicate=True: each mesh's selected faces extruded into a new mesh beside it, which ends up selected with its cap. */
json extrudeDuplicate(geo::ExtrudeOptions opts, bool keepSides)
{
    LILA_ZONE();

    Graph& g = graph();
    g.evaluate();
    std::map<NodeId, Ids> targets;
    for (auto& [node, ids] : components::selected(components::mode()))
        if (Node* n = g.node(node); n && n->type().isA("mesh"))
            if (MeshPtr cage = modifiers::cage(g, node))
                if (Ids f = components::convert(*cage, components::mode(), ids, ComponentMode::Face); !f.empty())
                    targets[node] = std::move(f);
    if (targets.empty())
        throw CommandError("polyExtrude: no faces selected");

    UndoChunk chunk("Extrude");
    json out = json::array();
    std::vector<NodeId> made, sources;
    std::map<NodeId, Ids> caps;
    for (auto& [node, sel] : targets)
    {
        Node& n = *g.node(node);
        geo::Mesh m = baseMesh(basePlug(n));
        auto sym = symmetryFor(n, m);
        opts.symmetry = sym ? &*sym : nullptr;
        opts.space = glm::dmat3(n.as<glm::dmat4>(Transform::kWorldMatrix));
        geo::ExtrudeCopy copy = geo::extrudeCopy(m, sel, opts, keepSides);
        if (copy.cap.empty())
            continue;
        copy.mesh.compact();
        Node* piece = g.createNode(n.type(), n.name(), g.parent(n.id()), false);
        for (int attr : {Transform::kTranslate, Transform::kRotate, Transform::kScale, Transform::kRotatePivot, Transform::kScalePivot,
                         Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate, Transform::kRotateOrder})
            g.setValue({piece->id(), attr, -1}, g.storedValue({n.id(), attr, -1}));
        g.setValue({piece->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(copy.mesh))));
        out.push_back(piece->path());
        made.push_back(piece->id());
        sources.push_back(node);
        caps[piece->id()] = std::move(copy.cap);
    }
    isolation::inherit(made, sources);
    g.select(made);
    // The cap stays selected in face mode, so the extrude handles and the last-op panel keep working on the copy.
    components::keepModeAfterCreate();
    components::setAll({}, {}, caps);
    if (components::mode() != ComponentMode::Face)
        components::setModeUndoable(ComponentMode::Face);
    return out;
}
} // namespace

void registerMeshEditComponentCommands()
{
    using T = ArgType;
    using K = ComponentMode;

    ops::registerCommand({
        .name = "polyExtrude",
        .doc = "Extrude the selected faces along their normals, or in edge mode grow a strip of faces off the selected edges. "
               "Connected faces extrude as regions unless individual=True.",
        .flags = {{"thickness", "", T::Float, 0.0, "Distance along the normals, cm."},
                  {"offset", "", T::Float, 0.0,
                   "Faces: inward distance of the extruded border, cm; negative grows them. Edges: outward distance from their faces, cm."},
                  {"individual", "", T::Bool, false, "Each face separately."},
                  {"keepBack", "", T::Bool, true,
                   "Faces: where the extruded faces only border open edges, as a whole open shell, keep a copy facing back at the base."},
                  {"duplicate", "", T::Bool, false, "Faces: leave the mesh as it is and put the extruded faces in a new mesh."},
                  {"keepSides", "", T::Bool, false, "With duplicate, the new mesh also keeps the side faces."}},
        .run = [](const Args& a) -> json {
            selectWholeInObjectMode(K::Face);
            if (a.get<bool>("duplicate") && components::mode() != K::Edge)
            {
                geo::ExtrudeOptions o{a.get<double>("thickness"), a.get<bool>("individual"), nullptr, a.get<double>("offset"),
                                      a.get<bool>("keepBack")};
                return extrudeDuplicate(o, a.get<bool>("keepSides"));
            }
            if (components::mode() == K::Edge)
                return editSelected("Extrude", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                    geo::EdgeExtrudeOptions o{a.get<double>("thickness"), a.get<double>("offset"), s};
                    return edges(geo::extrudeEdges(m, sel, o).edges);
                });
            return editSelected("Extrude", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                geo::ExtrudeOptions o{a.get<double>("thickness"), a.get<bool>("individual"), s, a.get<double>("offset"), a.get<bool>("keepBack"),
                                      editSpace()};
                return faces(geo::extrudeFaces(m, sel, o).faces);
            });
        },
    });

    ops::registerCommand({
        .name = "polyInset",
        .doc = "Inset the selected faces.",
        .flags = {{"thickness", "", T::Float, 5.0, "Inward distance, cm."},
                  {"depth", "", T::Float, 0.0, "Push along the normals afterwards, cm."},
                  {"individual", "", T::Bool, false, "Each face separately."},
                  {"clamp", "", T::Bool, true, "Shrink the thickness where the inset would cross itself, as on narrow faces."}},
        .run = [](const Args& a) -> json {
            selectWholeInObjectMode(K::Face);
            return editSelected("Inset", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                geo::InsetOptions o{a.get<double>("thickness"), a.get<double>("depth"), a.get<bool>("individual"), s,
                                    a.get<bool>("clamp"), editSpace()};
                return faces(geo::insetFaces(m, sel, o).faces);
            });
        },
    });

    ops::registerCommand({
        .name = "polyBevel",
        .doc = "Bevel the selected edges (vertex or face selections convert to their edges).",
        .flags = {{"offset", "", T::Float, 2.0, "Width, cm.", {}, {}, 0.0},
                  {"segments", "", T::Int, 1, "Segments across the bevel.", {}, {}, 1.0, 64.0},
                  {"profile", "", T::Float, 0.5, "0.5 round, 0.25 straight, toward 1 square.", {}, {}, 0.0, 1.0},
                  {"clamp", "", T::Bool, true, "Keep widths from overlapping."},
                  {"smoothingAngle", "", T::Float, 30.0, "New edges sharper than this are hard.", {}, {}, 0.0, 180.0}},
        .run = [](const Args& a) -> json {
            selectWholeInObjectMode(K::Edge);
            double used = a.get<double>("offset");
            // Faces bevel the border of each selected region, not the edges between selected faces (Maya).
            const bool fromFaces = components::mode() == K::Face;
            json out = editSelected("Bevel", fromFaces ? K::Face : K::Edge, [&](geo::Mesh& m, const Ids& picked, const geo::SymmetryMap* s) {
                const Ids sel = fromFaces ? regionBorderEdges(m, withMirror(m, s, picked, K::Face)) : picked;
                geo::BevelOptions o;
                o.offset = a.get<double>("offset");
                o.segments = std::max(1, a.get<int>("segments"));
                o.profile = a.get<double>("profile");
                o.clamp = a.get<bool>("clamp");
                o.smoothingAngle = a.get<double>("smoothingAngle");
                o.symmetry = s;
                double applied = o.offset;
                o.appliedOffset = &applied;
                EditResult r = faces(geo::bevelEdges(m, sel, o));
                used = std::min(used, applied);
                return r;
            });
            if (a.get<bool>("clamp") && used < a.get<double>("offset") - 1e-9)
                lastOp::limitFlag("offset", std::floor(used * 1000.0) / 1000.0);
            return out;
        },
    });

    ops::registerCommand({
        .name = "deleteComponents",
        .doc = "Delete the selected vertices or faces (with whatever depends on them), or the selected edges, joining their faces and keeping every vertex.",
        .run = [](const Args&) -> json {
            K kind = components::mode();
            return editSelected("Delete Components", kind, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                if (kind == K::Vertex)
                    geo::deletePoints(m, sel, s);
                else if (kind == K::Edge)
                    geo::dissolveEdges(m, withMirror(m, s, sel, K::Edge), false);
                else
                    geo::deleteFaces(m, sel, s);
                return EditResult{};
            });
        },
    });

    ops::registerCommand({
        .name = "polyInsertLoop",
        .doc = "Insert edge loops across the ring of the first selected edge. Several loops are centred on fraction, spacing apart.",
        .flags = {{"fraction", "", T::Float, 0.5, "Where along the ring, 0 to 1; with several loops, where their middle goes.", {}, {}, 0.0, 1.0},
                  {"count", "", T::Int, 1, "Number of loops.", {}, {}, 1.0},
                  {"spacing", "", T::Float, 1.0, "Several loops: gap between neighbours, 1 even spacing across the edge, smaller packs them together.", {}, {}, 0.0}},
        .run = [](const Args& a) -> json {
            return editSelected("Insert Edge Loop", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                if (sel.empty())
                    return edges({});
                geo::EdgeLoopOptions o{a.get<double>("fraction"), std::max(1, a.get<int>("count")), a.get<double>("spacing")};
                if (!s)
                    return edges(geo::insertEdgeLoop(m, sel.front(), o));
                // One plan for every loop and its images, so a ring that is its own image gets each mirrored spot once.
                geo::KnifePlan plan;
                for (double t : geo::edgeLoopFractions(o))
                    std::ranges::copy(geo::knifeLoopPlan(m, sel.front(), t, s).segments, std::back_inserter(plan.segments));
                return edges(geo::knifeApply(m, plan));
            });
        },
    });

    ops::registerCommand({
        .name = "polySplitRings",
        .doc = "Subdivide one way: insert loops across every ring through the selected edges, or across the selected faces in one "
               "direction, without the perpendicular cuts a full subdivide adds. Returns the new edges.",
        .flags = {{"count", "", T::Int, 1, "Loops per ring, evenly spaced.", {}, {}, 1.0, 32.0},
                  {"direction", "", T::String, "along", "Faces: across adds lines across the selection's short way, along the long way.",
                   {"across", "along"}},
                  {"edgeFlow", "", T::Bool, true, "Curve the new loops to follow the surrounding surface rather than lie flat."}},
        .run = [](const Args& a) -> json {
            // Object mode splits whole meshes, read as every face so direction still picks the way.
            const K mode = components::mode();
            const K kind = mode == K::Face || mode == K::Object ? K::Face : K::Edge;
            const int count = std::max(1, a.get<int>("count"));
            const bool across = a.get<std::string>("direction") == "across";
            const bool flow = a.get<bool>("edgeFlow");
            return editSelected("Split Rings", kind, [&](geo::Mesh& m, const Ids& picked, const geo::SymmetryMap* s) {
                Ids sel = picked;
                if (sel.empty())
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            sel.push_back(f);
                const Ids ringEdges = kind == K::Face ? geo::regionRingEdges(m, sel, across) : sel;
                const Ids made = geo::splitRings(m, ringEdges, count, s);
                if (flow && !made.empty())
                    for (const geo::PointMove& mv : geo::editEdgeFlow(m, made, {}))
                        m.setPosition(mv.point, mv.position);
                return edges(made);
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyOffsetLoop",
        .doc = "Insert an edge loop either side of the selected edges, parallel to them (Maya's Offset Edge Loop). Partial loops end on "
               "a border, or inside the mesh by meeting at the end vertex or wrapping round it. Returns the new edges.",
        .flags = {{"distance", "", T::Float, 0.25, "How far out: a fraction of each crossing edge's length, or cm with absolute.", {}, {}, 0.0},
                  {"absolute", "", T::Bool, false, "distance is in cm, square to the selected edges."},
                  {"side", "", T::String, "both", "both, left or right of the edges' direction.", {"both", "left", "right"}},
                  {"ends", "", T::String, "converge", "Open ends inside the mesh: converge (meet at the end vertex) or wrap (round it).",
                   {"converge", "wrap"}},
                  {"deleteEdge", "", T::Bool, false, "Dissolve the selected edges afterward, leaving the two new loops."}},
        .run = [](const Args& a) -> json {
            return editSelected("Offset Edge Loop", K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                const std::string side = a.get<std::string>("side");
                geo::OffsetLoopOptions o;
                o.distance = std::max(0.0, a.get<double>("distance"));
                o.absolute = a.get<bool>("absolute");
                o.side = side == "left" ? geo::OffsetSide::Left : side == "right" ? geo::OffsetSide::Right : geo::OffsetSide::Both;
                o.ends = a.get<std::string>("ends") == "wrap" ? geo::OffsetEnds::Wrap : geo::OffsetEnds::Converge;
                o.symmetry = s;
                return edges(geo::offsetEdgeLoops(m, sel, o, a.get<bool>("deleteEdge")));
            });
        },
    });

    ops::registerCommand({
        .name = "polyMerge",
        .doc = "Weld selected vertices closer than a distance (all of a mesh's points if it has no component selection). "
               "Exactly two selected vertices weld whatever the distance.",
        .flags = {{"distance", "", T::Float, 0.01, "Weld distance, cm.", {}, {}, 0.0}, {"average", "", T::Bool, false, "Move merged points to their average."}},
        .run = [](const Args& a) -> json {
            const bool pair = components::mode() == K::Vertex;
            return editSelected("Merge", K::Vertex, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                geo::MergeOptions o{a.get<double>("distance"), a.get<bool>("average"), s};
                // Just past the pair's own gap, so a mirrored pair welds too without reaching further. A symmetric
                // selection is the pair once per side.
                const Ids one = s && sel.size() > 2 ? s->wedgeGroups(sel).front() : sel;
                if (pair && one.size() == 2 && m.pointValid(one[0]) && m.pointValid(one[1]))
                    o.distance = std::max(o.distance, glm::distance(m.position(one[0]), m.position(one[1])) * 1.001 + 1e-6);
                return points(geo::mergeByDistance(m, sel, o));
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyMergeCenter",
        .doc = "Weld the selected vertices into one at their centre.",
        .run = [](const Args&) -> json {
            return editSelected("Merge to Center", K::Vertex, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                Ids out;
                for (const Ids& group : s ? s->wedgeGroups(sel) : std::vector<Ids>{sel})
                    if (const Index p = geo::mergeToCenter(m, group); p != geo::kInvalid)
                        out.push_back(p);
                return points(std::move(out));
            });
        },
    });

    ops::registerCommand({
        .name = "polyTargetWeld",
        .doc = "Target weld: merge one vertex into another, which keeps its place (or both meet halfway with center=), or an "
               "edge into another, end to nearest end. Follows the mesh's symmetry; a weld across the symmetry plane meets on it. "
               "Returns the welded vertices.",
        .flags = {nodeFlag({"mesh", "", T::String, nullptr, "Mesh to weld; default: the selected or hilited mesh."}, "mesh"),
                  {"source", "", T::Int, -1, "Vertex (or edge) that moves.", {}, {}, -1.0},
                  {"target", "", T::Int, -1, "Vertex (or edge) it merges into.", {}, {}, -1.0},
                  {"edge", "", T::Bool, false, "source and target are edges."},
                  {"center", "", T::Bool, false, "Meet halfway instead of at the target."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            Node& n = meshFlag(a, "polyTargetWeld");
            PlugRef base = basePlug(n);
            geo::Mesh mesh = baseMesh(base);
            const bool edge = a.get<bool>("edge");
            const Index source = Index(a.get<int>("source")), target = Index(a.get<int>("target"));
            const bool valid = edge ? mesh.edgeValid(source) && mesh.edgeValid(target) : mesh.pointValid(source) && mesh.pointValid(target);
            if (!valid || source == target)
                throw CommandError(std::format("polyTargetWeld: {} has no {} {} and {} to weld", n.path(), edge ? "edges" : "vertices",
                                               a.get<int>("source"), a.get<int>("target")));
            auto sym = symmetryFor(n, mesh);
            std::vector<geo::WeldPair> pairs;
            auto add = [&](Index s, Index t) {
                if (edge)
                    std::ranges::copy(geo::edgeWeldPairs(mesh, s, t), std::back_inserter(pairs));
                else
                    pairs.push_back({s, t});
            };
            add(source, target);
            bool center = a.get<bool>("center");
            if (sym)
            {
                std::set<std::pair<Index, Index>> added{{source, target}};
                for (int k = 1; k < sym->order(); ++k)
                {
                    const Index ms = edge ? sym->imageEdge(mesh, k, source) : sym->image(k, source);
                    const Index mt = edge ? sym->imageEdge(mesh, k, target) : sym->image(k, target);
                    if (ms == target)
                        center = true;
                    else if (ms != geo::kInvalid && mt != geo::kInvalid && added.insert({ms, mt}).second)
                        add(ms, mt);
                }
            }

            UndoChunk chunk("Target Weld");
            std::vector<glm::dvec3> before;
            int drive = 0;
            if (sym)
            {
                before.assign(mesh.positions().begin(), mesh.positions().end());
                drive = sym->majorityWedge(edge ? components::convert(mesh, ComponentMode::Edge, {source}, ComponentMode::Vertex) : Ids{source});
            }
            Ids welded = geo::targetWeld(mesh, pairs, center);
            if (welded.empty())
                throw CommandError("polyTargetWeld: nothing to weld");
            if (sym)
                maintainSymmetry(n, mesh, before, drive);
            geo::MeshRemap remap;
            mesh.compact(&remap);
            const std::vector<Index>* maps[3] = {&remap.points, &remap.edges, &remap.faces};
            std::map<NodeId, Ids> sel[3] = {components::selected(ComponentMode::Vertex), components::selected(ComponentMode::Edge),
                                            components::selected(ComponentMode::Face)};
            for (int k = 0; k < 3; ++k)
                if (auto it = sel[k].find(n.id()); it != sel[k].end())
                    if (Ids kept = remapIds(it->second, *maps[k]); kept.empty())
                        sel[k].erase(it);
                    else
                        it->second = std::move(kept);
            Ids mapped = remapIds(welded, remap.points);
            auto edited = std::make_shared<geo::Mesh>(std::move(mesh));
            g.setValue(base, MeshPtr(edited));
            modifiers::baseRemapped(g, n.id(), remap, *edited);
            objectSets::meshRemapped(g, n.id(), remap);
            components::setAll(sel[0], sel[1], sel[2]);
            return mapped;
        },
    });

    ops::registerCommand({
        .name = "polyCollapse",
        .doc = "Collapse each connected group of selected edges to its centre.",
        .run = [](const Args&) -> json {
            return editSelected("Collapse", K::Edge, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                return points(geo::collapseEdges(m, withMirror(m, s, sel, K::Edge)));
            });
        },
    });

    ops::registerCommand({
        .name = "polyDissolve",
        .doc = "Remove selected edges or vertices by merging the faces around them, cleanly.",
        .run = [](const Args&) -> json {
            K kind = components::mode() == K::Vertex ? K::Vertex : K::Edge;
            return editSelected("Dissolve", kind, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                Ids all = withMirror(m, s, sel, kind);
                return kind == K::Vertex ? faces(geo::dissolvePoints(m, all)) : faces(geo::dissolveEdges(m, all, true));
            });
        },
    });

    ops::registerCommand({
        .name = "polyBridge",
        .doc = "Join selected border loops or chains with faces. Several pairs bridge at once; a single border round a slot or sliver bridges "
               "its two long sides; borders of different lengths pair by arc length. Selected faces are deleted and the holes bridged.",
        .flags = {{"divisions", "", T::Int, -1, "Rows between the borders; -1 picks them from the gap, the faces around it and the open ends.", {}, {}, -1.0, 256.0},
                  {"curveType", "", T::String, "auto", "How the rows run between the borders.", {"linear", "blend", "path", "auto"}},
                  {"smoothness", "", T::Float, 1.0, "0 straightens the curve, 1 is the full curve.", {}, {}, 0.0, 1.0},
                  {"tensionSource", "", T::Float, 1.0, "Handle length leaving the source border (Blend and Auto).", {}, {}, 0.0, 4.0},
                  {"tensionTarget", "", T::Float, 1.0, "Handle length entering the target border (Blend and Auto).", {}, {}, 0.0, 4.0},
                  {"bridgeOffset", "", T::Int, 0, "Steps the pairing round closed loops."},
                  {"directionSource", "", T::Bool, false, "Read the source border the other way round."},
                  {"directionTarget", "", T::Bool, false, "Read the target border the other way round."},
                  {"twist", "", T::Float, 0.0, "Turns the middle rows about the bridge's axis, degrees at the target."},
                  {"taper", "", T::Float, 1.0, "Scale of the middle rows about their centre.", {}, {}, 0.0, 10.0},
                  {"bias", "", T::Float, 0.5, "Where the taper peaks, 0 at the source, 1 at the target.", {}, {}, 0.0, 1.0},
                  {"taperCurve", "", T::List, nullptr, "Taper strength along the bridge, values evenly spaced from source to target."},
                  {"profileFactor", "", T::Float, 0.0, "Bulge (positive) or pinch (negative) of the middle rows, as a fraction of the gap.", {}, {}, -2.0, 2.0},
                  {"profileShape", "", T::String, "smooth", "Falloff of the profile across the bridge.",
                   {"smooth", "sphere", "root", "sharp", "linear", "inverseSquare", "constant"}},
                  nodeFlag({"curve", "", T::String, "", "The curve Path follows."}, "curve"),
                  {"transition", "", T::String, "quads", "How borders of different lengths make up the difference.", {"quads", "triangles"}},
                  {"weldEnds", "", T::Bool, true, "Open bridges land their end rows on the border between the two sides' ends."},
                  {"weldTolerance", "", T::Float, 0.35, "How far a row may be from an end border point and still weld, as a fraction of a row.", {}, {}, 0.0, 1.0},
                  {"relax", "", T::Int, 0, "Smoothing passes over the new points.", {}, {}, 0.0, 50.0},
                  {"smoothingAngle", "", T::Float, 30.0, "New edges sharper than this are hard.", {}, {}, 0.0, 180.0},
                  {"merge", "", T::Bool, false, "Merge the borders into one instead of bridging."},
                  {"mergeFactor", "", T::Float, 0.5, "Where merged points go: 0 the source, 1 the target.", {}, {}, 0.0, 1.0},
                  {"connect", "", T::String, "pairs", "Pair loops up, or run one bridge through them all in order.", {"pairs", "chain", "chainClosed"}},
                  {"startSource", "", T::Int, -1, "Source point that pairs with startTarget."},
                  {"startTarget", "", T::Int, -1, "Target point that pairs with startSource."},
                  {"pairs", "", T::List, nullptr, "More point pairs to hold: [[source, target], ...]."}},
        .run = [](const Args& a) -> json {
            geo::BridgeOptions o;
            o.divisions = a.get<int>("divisions");
            const std::string curve = a.get<std::string>("curveType");
            o.curve = curve == "linear" ? geo::BridgeCurve::Linear
                      : curve == "blend" ? geo::BridgeCurve::Blend
                      : curve == "path"  ? geo::BridgeCurve::Path
                                         : geo::BridgeCurve::Auto;
            o.smoothness = a.get<double>("smoothness");
            o.tensionSource = a.get<double>("tensionSource");
            o.tensionTarget = a.get<double>("tensionTarget");
            o.offset = a.get<int>("bridgeOffset");
            o.reverseSource = a.get<bool>("directionSource");
            o.reverseTarget = a.get<bool>("directionTarget");
            o.twist = a.get<double>("twist");
            o.taper = a.get<double>("taper");
            o.bias = a.get<double>("bias");
            if (a.has("taperCurve"))
                for (const json& v : a.flags["taperCurve"])
                    if (v.is_number())
                        o.taperCurve.push_back(v.get<double>());
            o.profileFactor = a.get<double>("profileFactor");
            const std::string shape = a.get<std::string>("profileShape");
            const char* shapes[] = {"smooth", "sphere", "root", "sharp", "linear", "inverseSquare", "constant"};
            for (size_t k = 0; k < std::size(shapes); ++k)
                if (shape == shapes[k])
                    o.profileShape = geo::BridgeProfile(k);
            o.transition = a.get<std::string>("transition") == "triangles" ? geo::BridgeTransition::Triangles : geo::BridgeTransition::Quads;
            o.weldEnds = a.get<bool>("weldEnds");
            o.weldTolerance = a.get<double>("weldTolerance");
            o.relax = std::max(0, a.get<int>("relax"));
            o.smoothingAngle = a.get<double>("smoothingAngle");
            o.merge = a.get<bool>("merge");
            o.mergeFactor = a.get<double>("mergeFactor");
            const std::string connect = a.get<std::string>("connect");
            o.connect = connect == "chain"         ? geo::BridgeConnect::Chain
                        : connect == "chainClosed" ? geo::BridgeConnect::ChainClosed
                                                   : geo::BridgeConnect::Pairs;
            if (a.get<int>("startSource") >= 0 && a.get<int>("startTarget") >= 0)
                o.pairs.push_back({Index(a.get<int>("startSource")), Index(a.get<int>("startTarget"))});
            if (a.has("pairs"))
                for (const json& pr : a.flags["pairs"])
                    if (pr.is_array() && pr.size() == 2 && pr[0].is_number_integer() && pr[1].is_number_integer())
                        o.pairs.push_back({pr[0].get<Index>(), pr[1].get<Index>()});
            if (o.curve == geo::BridgeCurve::Path)
                o.path = bridgePath(a.get<std::string>("curve"));

            geo::BridgeResult total;
            const bool fromFaces = components::mode() == K::Face;
            json out = editSelected("Bridge", fromFaces ? K::Face : K::Edge, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                const Ids all = withMirror(m, s, sel, fromFaces ? K::Face : K::Edge);
                geo::BridgeOptions so = o;
                so.symmetry = s;
                geo::BridgeResult r = fromFaces ? geo::bridgeFaces(m, all, so) : geo::bridge(m, all, so);
                total.rows = std::max(total.rows, r.rows);
                total.welded += r.welded;
                total.split += r.split;
                total.unwelded += r.unwelded;
                total.triangles += r.triangles;
                total.pairsBridged += r.pairsBridged;
                total.warnings.insert(total.warnings.end(), r.warnings.begin(), r.warnings.end());
                return faces(std::move(r.faces));
            });
            for (const std::string& w : total.warnings)
                log::warn("polyBridge: {}", w);
            if (total.pairsBridged == 0)
                log::warn("polyBridge: nothing to bridge; select two borders, or one border round a slot");
            else if (total.unwelded > 0)
                log::warn("polyBridge: {} end border points fell between rows and stay as corners of the end faces", total.unwelded);
            return out;
        },
    });

    ops::registerCommand({
        .name = "polyPoke",
        .doc = "Fan each selected face into triangles around its centre.",
        .run = [](const Args&) -> json {
            return editSelected("Poke", K::Face, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                return faces(geo::pokeFaces(m, withMirror(m, s, sel, K::Face)));
            });
        },
    });

    ops::registerCommand({
        .name = "polySpinEdge",
        .doc = "Turn each selected edge one step within its two faces.",
        .run = [](const Args&) -> json {
            return editSelected("Spin Edge", K::Edge, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                return edges(geo::spinEdges(m, withMirror(m, s, sel, K::Edge)));
            });
        },
    });

    ops::registerCommand({
        .name = "polyFillHole",
        .doc = "Cap the holes bordered by the selected edges (every hole when nothing is selected).",
        .run = [](const Args&) -> json {
            return editSelected("Fill Hole", K::Edge, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap*) {
                return faces(geo::fillHoles(m, sel));
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyGridFill",
        .doc = "Rebuild triangle fans and other patches as a quad grid inside a ring of quads: select the fans' centre vertices "
               "or the faces. With edges selected, fills the holes they border the same way.",
        .flags = {{"gridSize", "", T::Float, 0.4, "The grid's size against the hole; 1 grids right to the border with no ring.", {}, {}, 0.05, 1.0},
                  {"rotate", "", T::Int, 0, "Steps the grid's corners round the border."}},
        .run = [](const Args& a) -> json {
            geo::GridFillOptions o;
            o.gridSize = std::clamp(a.get<double>("gridSize"), 0.05, 1.0);
            o.rotate = a.get<int>("rotate");
            const K mode = components::mode();
            const K kind = mode == K::Vertex || mode == K::Edge ? mode : K::Face;
            return editSelected("Grid Fill", kind, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                const Ids all = withMirror(m, s, sel, kind);
                if (kind == K::Edge)
                    return faces(geo::gridFillHoles(m, all, o, s));
                return faces(geo::gridFillFaces(m, kind == K::Vertex ? geo::facesOfPoints(m, all) : all, o, s));
            });
        },
    });

    ops::registerCommand({
        .name = "polyFlip",
        .doc = "Reverse the winding of the selected faces (the whole mesh in object mode).",
        .run = [](const Args&) -> json {
            return editSelected("Flip Normals", K::Face, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                Ids all = sel;
                if (all.empty())
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            all.push_back(f);
                geo::flipNormals(m, withMirror(m, s, all, K::Face));
                return sel.empty() ? EditResult{} : faces(sel);
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyConform",
        .doc = "Make face winding consistent; closed parts face outward.",
        .run = [](const Args&) -> json {
            return editSelected("Conform", K::Face, [](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap*) {
                geo::conformNormals(m, sel);
                return EditResult{};
            }, true);
        },
    });

    ops::registerCommand({
        .name = "polyTriangulate",
        .doc = "Triangulate the selected faces (the whole mesh in object mode), deterministically, as Unreal will. Concave and "
               "non-planar faces, collinear points and faces touching themselves split without overlaps or slivers, and no "
               "diagonal doubles an edge another face already has. With symmetry on, the mirrored faces split the mirrored way. "
               "A whole mesh with modifiers gets a triangulate modifier on top instead, so the result is triangulated rather "
               "than the cage.",
        .flags = {{"beauty", "", T::Bool, true, "Flip n-gon diagonals toward the largest smallest angle (what display and export use)."},
                  {"quadSplit", "", T::String, "shortest", "Quads split along the shorter diagonal (what display, export and MikkTSpace use) or always from the first corner.", {"shortest", "first"}}},
        .run = [](const Args& a) -> json {
            geo::TriangulateOptions o;
            o.beauty = a.get<bool>("beauty");
            const std::string split = a.get<std::string>("quadSplit", "shortest");
            if (split != "shortest" && split != "first")
                throw CommandError("polyTriangulate: quadSplit must be shortest or first");
            o.quads = split == "first" ? geo::QuadSplit::FirstCorner : geo::QuadSplit::ShortestDiagonal;
            UndoChunk chunk("Triangulate");
            const auto onStack = [](const Node& mesh) {
                Graph& g = graph();
                const std::vector<NodeId> stack = modifiers::stack(g, mesh.id());
                if (stack.empty())
                    return false;
                if (g.node(stack.back())->type().name != "triangulateModifier")
                    ops::execute("addModifier", {{"args", {mesh.path()}}, {"kind", "triangulate"}});
                return true;
            };
            return editSelected("Triangulate", K::Face, [&](geo::Mesh& m, const Ids& sel, const geo::SymmetryMap* s) {
                Ids all = withMirror(m, s, sel, K::Face);
                if (all.empty())
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            all.push_back(f);
                return faces(geo::triangulateFaces(m, all, o));
            }, true, onStack);
        },
    });
}
} // namespace lila::scene::detail
