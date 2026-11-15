#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/Symmetry.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshEditInternal.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <format>
#include <optional>

namespace lila::scene::detail
{
namespace
{
/**
 * The radial count the mesh's points fit: the largest that leaves none without an orbit, else the one leaving fewest.
 * 0 when no count fits any point, or the mesh is too dense to try them all.
 */
int bestRadialCount(const geo::Mesh& mesh, const geo::RadialSymmetry& radial, double tolerance)
{
    LILA_ZONE();

    if (mesh.numPoints() > 20000)
        return 0;
    int best = 0;
    Index fewest = mesh.numPoints();
    for (int count = geo::kMaxRadialCount; count >= 2; --count)
    {
        const geo::RadialSymmetry r(radial.origin(), radial.axis(), count, radial.mirror(), radial.reference());
        const Index unmatched = geo::SymmetryMap::buildRadial(mesh, r, tolerance).unmatchedCount();
        if (unmatched == 0)
            return count;
        if (unmatched < fewest)
        {
            fewest = unmatched;
            best = count;
        }
    }
    return best;
}
} // namespace

void registerMeshEditSymmetryCommands()
{
    using T = ArgType;

    // Warns when a mesh just given symmetry is not symmetric about it, so edits will not mirror cleanly. For radial
    // symmetry it also names the count the mesh fits best: a count that does not match the topology round the axis is
    // the usual mistake.
    auto warnIfAsymmetric = [](NodeId id) {
        Graph& g = graph();
        g.evaluate();
        Node* n = g.node(id);
        MeshPtr cage = n ? modifiers::cage(g, id) : nullptr;
        const std::optional<geo::SymmetrySpec> spec = n ? symmetrySpec(*n) : std::nullopt;
        if (!cage || !spec)
            return;
        geo::Mesh m = *cage;
        const double tolerance = symmetryTolerance(m);
        geo::SymmetryMap map = geo::SymmetryMap::persistent(m, *spec, tolerance);
        geo::SymmetryCheck c = map.check(m);
        if (c.unmatched == 0 && c.maxAsymmetry <= tolerance)
            return;
        if (!spec->radial)
        {
            log::warn("{} is not symmetric in {}: {} points without a partner, max asymmetry {:g} cm", n->path(), char('X' + spec->axis), c.unmatched,
                      c.maxAsymmetry);
            return;
        }
        const int best = bestRadialCount(m, *spec->radial, tolerance);
        log::warn("{} does not fit {}: {} points without a full orbit (shown in red), max asymmetry {:g} cm{}", n->path(), symmetryLabel(*spec),
                  c.unmatched, c.maxAsymmetry,
                  best > 0 && best != spec->radial->count() ? std::format("; a count of {} fits its topology best", best) : std::string());
    };

    // The radial settings flags shared by setSymmetry and symmetrizeRadial, applied to a mesh node; true if any changed.
    auto applyRadialFlags = [](const Args& a, Node& n, const char* cmd) {
        Graph& g = graph();
        bool changed = false;
        auto set = [&](int attr, const PlugValue& v) {
            if (n.value(attr) != v)
            {
                g.setValue({n.id(), attr, -1}, v);
                changed = true;
            }
        };
        if (a.has("count"))
        {
            const int count = a.get<int>("count");
            if (count < 2 || count > geo::kMaxRadialCount)
                throw CommandError(std::format("{}: count must be 2 to {}", cmd, geo::kMaxRadialCount));
            set(MeshNode::kRadialCount, int64_t(count));
        }
        if (a.has("radialAxis"))
        {
            const std::string axis = a.get<std::string>("radialAxis");
            const int64_t v = axis == "x" ? 0 : axis == "y" ? 1 : axis == "z" ? 2 : axis == "custom" ? 3 : -1;
            if (v < 0)
                throw CommandError(std::string(cmd) + ": radialAxis must be x, y, z or custom");
            set(MeshNode::kRadialAxis, v);
        }
        if (a.has("direction"))
        {
            const glm::dvec3 d = vec(a.flags["direction"]);
            if (glm::length(d) < 1e-12)
                throw CommandError(std::string(cmd) + ": direction must not be zero");
            set(MeshNode::kRadialDirection, d);
            set(MeshNode::kRadialAxis, int64_t(3));
        }
        if (a.has("mirror"))
            set(MeshNode::kRadialMirror, a.get<bool>("mirror"));
        if (a.has("phase"))
            set(MeshNode::kRadialPhase, a.get<double>("phase"));
        return changed;
    };

    const std::vector<FlagDef> radialFlags = {
        {"count", "", T::Int, nullptr, "Radial: how many wedges repeat round the axis, 2 to 64; default: the mesh's own.", {}, {}, 2.0, double(geo::kMaxRadialCount)},
        {"radialAxis", "", T::String, nullptr, "Radial: the axis, x, y, z or custom (direction=); default: the mesh's own.", {"x", "y", "z", "custom"}},
        {"direction", "", T::Vec3, nullptr, "Radial: a custom axis direction, object space."},
        {"mirror", "", T::Bool, nullptr, "Radial: also mirror each wedge across a plane through the axis (dihedral symmetry)."},
        {"phase", "", T::Float, nullptr, "Radial: degrees the wedge boundaries and mirror planes turn about the axis."}};

    std::vector<FlagDef> setSymmetryFlags = {{"axis", "", T::String, "x", "off, x, y, z or radial.", {"off", "x", "y", "z", "radial"}}};
    setSymmetryFlags.insert(setSymmetryFlags.end(), radialFlags.begin(), radialFlags.end());
    ops::registerCommand({
        .name = "setSymmetry",
        .doc = "Object-space symmetry for component edits on meshes: 'off', a mirror plane 'x', 'y' or 'z', or 'radial': count "
               "wedges repeated round radialAxis (with mirror=True also mirrored within each wedge), every edit applied to all of them.",
        .positionalDoc = "objects",
        .flags = setSymmetryFlags,
        .run = [warnIfAsymmetric, applyRadialFlags](const Args& a) -> json {
            std::string axis = a.get<std::string>("axis", "x");
            int64_t v = axis == "off" ? 0 : axis == "x" ? 1 : axis == "y" ? 2 : axis == "z" ? 3 : axis == "radial" ? kSymmetryRadial : -1;
            if (v < 0)
                throw CommandError("setSymmetry: axis must be off, x, y, z or radial");
            for (const std::string& name : meshObjects(a, "setSymmetry"))
            {
                Node& n = meshArg(name, "setSymmetry");
                // A custom plane (or radial centre) belongs to the setting it was set on.
                if (v > 0 && v != n.as<int64_t>(MeshNode::kSymmetry) && n.as<int64_t>(MeshNode::kSymmetryPlane) == 1)
                    graph().setValue({n.id(), MeshNode::kSymmetryPlane, -1}, int64_t(0));
                bool changed = v != n.as<int64_t>(MeshNode::kSymmetry);
                if (applyRadialFlags(a, n, "setSymmetry") && v == kSymmetryRadial)
                    changed = true;
                graph().setValue({n.id(), MeshNode::kSymmetry, -1}, v);
                if (v > 0 && changed)
                    warnIfAsymmetric(n.id());
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "setSymmetryPlane",
        .doc = "Set where meshes' symmetry plane lies: through the selected edges or vertices (which must lie flat across an "
               "axis; that axis becomes the symmetry axis), or back through the pivot with pivot=True. With radial symmetry the "
               "radial axis passes through the selection's centre instead. Returns the plane per mesh as [axis, offset], or "
               "['radial', centre].",
        .positionalDoc = "objects",
        .flags = {{"pivot", "", T::Bool, false, "Put the plane (or radial axis) back through the pivot."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            json out = json::object();
            UndoChunk chunk("Set Symmetry Plane");
            auto forget = [&](Node& n) {
                // Pairs stored about the old plane pair the wrong points about the new one.
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                geo::removeSymmetryLayers(m);
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            };
            auto planeOf = [](const Node& n) -> json {
                const std::optional<geo::SymmetrySpec> spec = symmetrySpec(n);
                if (spec && spec->radial)
                {
                    const glm::dvec3 o = spec->radial->origin();
                    return {"radial", {o.x, o.y, o.z}};
                }
                const int axis = std::max(0, symmetryAxis(n));
                return {std::string(1, char('x' + axis)), symmetryOffset(n, axis)};
            };
            if (a.get<bool>("pivot"))
            {
                std::vector<std::string> names = a.objects();
                if (names.empty())
                    for (NodeId id : g.selection())
                        names.push_back(g.path(id));
                for (const std::string& name : names)
                {
                    Node* n = g.find(name);
                    if (!n || !n->type().isA("mesh") || n->as<int64_t>(MeshNode::kSymmetryPlane) == 0)
                        continue;
                    g.setValue({n->id(), MeshNode::kSymmetryPlane, -1}, int64_t(0));
                    forget(*n);
                    out[n->path()] = planeOf(*n);
                }
                return out;
            }
            const ComponentMode from = components::mode();
            if (from == ComponentMode::Object)
                throw CommandError("setSymmetryPlane: select edges or vertices lying on the plane");
            for (auto& [id, ids] : components::selected(from))
            {
                Node* n = g.node(id);
                MeshPtr cage = n && n->type().isA("mesh") ? modifiers::cage(g, id) : nullptr;
                if (!cage)
                    continue;
                const geo::Mesh& m = *cage;
                glm::dvec3 lo(1e300), hi(-1e300), sum(0.0);
                int count = 0;
                for (Index p : components::convert(m, from, ids, ComponentMode::Vertex))
                    if (m.pointValid(p))
                    {
                        lo = glm::min(lo, m.position(p)), hi = glm::max(hi, m.position(p)), sum += m.position(p), ++count;
                    }
                if (count == 0)
                    continue;
                if (n->as<int64_t>(MeshNode::kSymmetry) == kSymmetryRadial)
                {
                    // A ring of points round the axis: its centre is where the axis passes.
                    g.setValue({id, MeshNode::kSymmetryPlane, -1}, int64_t(1));
                    g.setValue({id, MeshNode::kRadialCenter, -1}, sum / double(count));
                    forget(*n);
                    out[n->path()] = planeOf(*n);
                    continue;
                }
                // The mesh's own axis when the selection is flat across it, else whichever axis it is flattest across.
                const glm::dvec3 spread = hi - lo;
                int axis = symmetryAxis(*n);
                if (axis < 0 || spread[axis] > symmetryTolerance(m))
                    axis = spread.x <= spread.y && spread.x <= spread.z ? 0 : spread.y <= spread.z ? 1 : 2;
                if (spread[axis] > std::max(symmetryTolerance(m), glm::length(m.bounds().size()) * 1e-3))
                    throw CommandError(std::format("setSymmetryPlane: the selection on {} does not lie flat across an axis", n->path()));
                const double offset = sum[axis] / count;
                g.setValue({id, MeshNode::kSymmetry, -1}, int64_t(axis + 1));
                g.setValue({id, MeshNode::kSymmetryPlane, -1}, int64_t(1));
                g.setValue({id, MeshNode::kSymmetryOffset, -1}, offset);
                forget(*n);
                out[n->path()] = {std::string(1, char('x' + axis)), offset};
            }
            if (out.empty())
                throw CommandError("setSymmetryPlane: select edges or vertices lying on the plane");
            return out;
        },
    });

    ops::registerCommand({
        .name = "toggleSymmetry",
        .doc = "Symmetry off on the meshes if every one already has it on (on axis=, when given), else on for all of them. Non-meshes are skipped. Returns the axis set, or 'off'.",
        .positionalDoc = "objects",
        .flags = {{"axis", "", T::String, nullptr, "x, y, z or radial to turn on with; default: the axis already in use, else the last one picked."}},
        .run = [warnIfAsymmetric](const Args& a) -> json {
            std::vector<NodeId> ids;
            for (const std::string& name : a.objects())
                if (Node* n = graph().find(name); n && n->type().isA("mesh"))
                    ids.push_back(n->id());
            if (ids.empty())
                throw CommandError("toggleSymmetry: no meshes given or selected");
            SymmetryState st = symmetryState(graph(), ids);
            auto stateAxis = [](int axis) { return axis == SymmetryState::kRadialState ? std::string("radial") : std::string(1, char('x' + axis)); };
            std::string axis = a.get<std::string>("axis", "");
            if (axis.empty())
                axis = st.axis >= 0 ? stateAxis(st.axis) : PrefStore::get().value<std::string>("general", "/symmetry/axis", "x");
            if (axis != "x" && axis != "y" && axis != "z" && axis != "radial")
                throw CommandError("toggleSymmetry: axis must be x, y, z or radial");
            const int64_t wanted = axis == "radial" ? kSymmetryRadial : axis[0] - 'x' + 1;
            const bool off = st.on == st.meshes && (!a.has("axis") || (st.axis >= 0 && stateAxis(st.axis) == axis));
            const int64_t v = off ? 0 : wanted;
            for (NodeId id : ids)
            {
                const bool changed = graph().node(id)->as<int64_t>(MeshNode::kSymmetry) != v;
                graph().setValue({id, MeshNode::kSymmetry, -1}, v);
                if (!off && changed)
                    warnIfAsymmetric(id);
            }
            if (!off)
                PrefStore::get().set("general", "/symmetry/axis", axis);
            return off ? "off" : axis;
        },
    });

    auto symmetricMesh = [](const std::string& name, const char* cmd) -> Node& {
        Node& n = meshArg(name, cmd);
        if (!symmetrySpec(n))
            throw CommandError(std::string(cmd) + ": " + name + " has symmetry off");
        return n;
    };
    // Objects named or selected, else the meshes with selected components.
    auto symmetryTargets = [](const Args& a, const char* cmd) {
        std::vector<std::string> names = a.objects().empty() ? std::vector<std::string>{} : meshObjects(a, cmd);
        if (names.empty() && components::mode() != ComponentMode::Object)
            for (auto& [node, ids] : components::selected(components::mode()))
                if (Node* n = graph().node(node))
                    names.push_back(n->path());
        return names;
    };

    /** A mirror plane this far (degrees) off every world axis is reported: the mesh mirrors across a plane no other part shares. */
    constexpr double kPlaneTiltWarn = 0.05;

    ops::registerCommand({
        .name = "symmetryCheck",
        .doc = "Report how symmetric meshes are across their symmetry plane (or round their radial axis): the largest distance "
               "between a point and its partner's reflection (or its orbit's image), and the points without a partner. For "
               "radial symmetry also the unmatched point ids and the count the mesh fits best. For a mirror, planeTilt is how far "
               "(degrees) the plane sits off the world axes under the object's transform; a tilted one is warned about. Changes nothing.",
        .positionalDoc = "objects",
        .run = [symmetricMesh, symmetryTargets](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            json out = json::object();
            for (const std::string& name : symmetryTargets(a, "symmetryCheck"))
            {
                Node& n = symmetricMesh(name, "symmetryCheck");
                MeshPtr cage = modifiers::cage(g, n.id());
                if (!cage)
                    continue;
                geo::Mesh m = *cage;
                const geo::SymmetrySpec spec = *symmetrySpec(n);
                const bool stored = m.pointAttrs().has(symmetryLayer(spec));
                geo::SymmetrySync sync;
                geo::SymmetryMap map = geo::SymmetryMap::persistent(m, spec, symmetryTolerance(m), &sync);
                geo::SymmetryCheck c = map.check(m);
                json r = {{"axis", spec.radial ? std::string("radial") : std::string(1, char('x' + spec.axis))},
                          {"maxAsymmetry", c.maxAsymmetry},
                          {"worstPoint", c.worstPoint == geo::kInvalid ? json(nullptr) : json(c.worstPoint)},
                          {"pairs", c.pairs},
                          {"onPlane", c.onPlane},
                          {"unmatched", c.unmatched},
                          {"stored", stored},
                          {"withoutStoredPartner", sync.topological + sync.byPosition + sync.unmatched}};
                if (spec.radial)
                {
                    r["count"] = spec.radial->count();
                    r["mirror"] = spec.radial->mirror();
                    std::vector<Index> unmatched = map.unmatchedPoints(m);
                    r["unmatchedPoints"] = unmatched;
                    r["bestCount"] = unmatched.empty() ? spec.radial->count() : bestRadialCount(m, *spec.radial, symmetryTolerance(m));
                }
                // The plane is the object's: under a turned transform the halves mirror across a tilted plane, not the world's.
                if (!spec.radial)
                {
                    const glm::dmat3 linear(n.as<glm::dmat4>(Transform::kWorldMatrix));
                    glm::dvec3 axis(0.0);
                    axis[spec.axis] = 1.0;
                    const glm::dvec3 normal = glm::normalize(glm::transpose(glm::inverse(linear)) * axis);
                    const double along = std::max({std::abs(normal.x), std::abs(normal.y), std::abs(normal.z)});
                    const double tilt = glm::degrees(std::acos(std::min(along, 1.0)));
                    r["planeTilt"] = tilt;
                    if (tilt > kPlaneTiltWarn)
                        log::warn("{}: its symmetry plane is tilted {:.3g} degrees off the world axes (its transform is rotated), so its halves "
                                  "do not mirror across the world plane. Freeze Transforms, then Rebuild Symmetry, squares it", n.path(), tilt);
                }
                out[n.path()] = r;
                log::write(LogLevel::Info, std::format("{}: symmetry {} max asymmetry {:g} cm, {} {}, {} {}, {} unmatched", n.path(), symmetryName(spec),
                                                       c.maxAsymmetry, c.pairs, spec.radial ? "orbits" : "pairs", c.onPlane,
                                                       spec.radial ? "fixed" : "on the plane", c.unmatched));
            }
            return out;
        },
    });

    /** The wedge most of the selected components lie in, else the one holding most of the mesh (a single modeled wedge). */
    auto keepWedge = [](const Node& n, const geo::Mesh& m, const geo::RadialSymmetry& r, double tol) {
        std::vector<Index> count(size_t(r.order()), 0);
        auto tally = [&](Index p) {
            if (m.pointValid(p) && r.axisDistance(m.position(p)) > tol)
                ++count[size_t(r.wedgeOf(m.position(p)))];
        };
        const ComponentMode from = components::mode();
        if (from != ComponentMode::Object)
            for (Index p : components::convert(m, from, components::selected(n.id(), from), ComponentMode::Vertex))
                tally(p);
        if (std::ranges::all_of(count, [](Index c) { return c == 0; }))
            for (Index p = 0; p < m.pointCapacity(); ++p)
                tally(p);
        return int(std::max_element(count.begin(), count.end()) - count.begin());
    };

    ops::registerCommand({
        .name = "symmetrize",
        .doc = "Make meshes exactly symmetric: every point on one side of the symmetry plane is copied, reflected, onto its partner, and "
               "on-plane points are put on the plane. With radial symmetry, one wedge's points are copied round the axis onto their orbits.",
        .positionalDoc = "objects",
        .flags = {{"side", "", T::String, "positive", "The side that is kept.", {"positive", "negative"}},
                  {"wedge", "", T::Int, -1, "Radial: the wedge that is kept; -1: the one most of the selected components are in, else 0.", {}, {}, -1.0}},
        .run = [symmetricMesh, symmetryTargets](const Args& a) -> json {
            Graph& g = graph();
            const int drive = a.get<std::string>("side", "positive") == "negative" ? -1 : 1;
            UndoChunk chunk("Symmetrize");
            for (const std::string& name : symmetryTargets(a, "symmetrize"))
            {
                Node& n = symmetricMesh(name, "symmetrize");
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                auto sym = symmetryFor(n, m);
                if (sym->isRadial())
                {
                    int wedge = a.get<int>("wedge");
                    if (wedge < 0)
                    {
                        const ComponentMode from = components::mode();
                        wedge = from == ComponentMode::Object
                                    ? 0
                                    : sym->majorityWedge(components::convert(m, from, components::selected(n.id(), from), ComponentMode::Vertex));
                    }
                    if (wedge >= sym->order())
                        throw CommandError(std::format("symmetrize: {} has wedges 0 to {}", n.path(), sym->order() - 1));
                    sym->symmetrizeWedge(m, wedge);
                }
                else
                    sym->symmetrize(m, drive);
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            }
            return nullptr;
        },
    });

    /**
     * The side to keep: the one most of the selected components lie on, else the side holding nearly all of the mesh (a
     * half model), else positive.
     */
    auto keepSide = [](const Node& n, const geo::Mesh& m, int axis, double plane, double tol) {
        const ComponentMode from = components::mode();
        double sum = 0.0;
        if (from != ComponentMode::Object)
            for (Index p : components::convert(m, from, components::selected(n.id(), from), ComponentMode::Vertex))
                if (m.pointValid(p))
                    sum += m.position(p)[axis] - plane;
        if (std::abs(sum) > tol)
            return sum > 0.0 ? 1 : -1;
        Index below = 0, above = 0;
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
            {
                const double d = m.position(p)[axis] - plane;
                below += d < -tol, above += d > tol;
            }
        return below > 4 * above ? -1 : 1;
    };

    /** Whether every point has a partner and every face a mirror face, so the two halves differ in position only. */
    auto pairsEveryFace = [](const geo::Mesh& m, const geo::SymmetryMap& map) {
        if (map.empty() || map.unmatchedCount() > 0)
            return false;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f) && map.mirrorFace(m, f) == geo::kInvalid)
                return false;
        return true;
    };

    ops::registerCommand({
        .name = "symmetryRebuild",
        .doc = "Rebuild meshes symmetric: the kept side is mirrored across the plane through the object's pivot, replacing the other side, "
               "welded on the plane, and the symmetry pairs are stored again. A mesh with radial symmetry is rebuilt from its kept "
               "wedge instead (see symmetrizeRadial). With pairsOnly, the geometry is left alone and only "
               "the stored pairs are matched again, by position or, for meshes that are no longer symmetric, by topology outward "
               "from the selected edge (which must lie on the plane; any edge for radial symmetry). Returns the points left unmatched per mesh.",
        .positionalDoc = "objects",
        .flags = {{"axis", "", T::String, nullptr, "x, y or z; default: the mesh's symmetry axis, else x."},
                  {"side", "", T::String, "auto", "The side that is kept; auto: the side of the selected components, else the side holding nearly all of the "
                   "mesh, else positive.",
                   {"auto", "positive", "negative"}},
                  {"wedge", "", T::Int, -1, "Radial: the wedge that is kept; -1: the one most of the selected components are in, else the one holding most of the mesh.", {}, {}, -1.0},
                  {"pairsOnly", "", T::Bool, false, "Only match the stored pairs again, leaving the geometry as it is."},
                  {"topological", "", T::Bool, false, "With pairsOnly: match by topology from the selected edge."},
                  {"tolerance", "", T::Float, 0.0, "Weld or match distance, cm (0: scaled to the mesh).", {}, {}, 0.0}},
        .run = [symmetricMesh, symmetryTargets, keepSide, keepWedge, pairsEveryFace](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            const std::string side = a.get<std::string>("side", "auto");
            if (side != "auto" && side != "positive" && side != "negative")
                throw CommandError("symmetryRebuild: side must be auto, positive or negative");
            const std::string axisFlag = a.get<std::string>("axis", "");
            if (!axisFlag.empty() && (axisFlag.size() != 1 || axisFlag[0] < 'x' || axisFlag[0] > 'z'))
                throw CommandError("symmetryRebuild: axis must be x, y or z");
            const bool pairsOnly = a.get<bool>("pairsOnly") || a.get<bool>("topological");
            json out = json::object();
            UndoChunk chunk("Rebuild Symmetry");
            bool rebuilt = false;
            for (const std::string& name : symmetryTargets(a, "symmetryRebuild"))
            {
                Node& n = pairsOnly && axisFlag.empty() ? symmetricMesh(name, "symmetryRebuild") : meshArg(name, "symmetryRebuild");
                const std::optional<geo::SymmetrySpec> spec = symmetrySpec(n);
                const bool radial = axisFlag.empty() && spec && spec->radial;
                const int axis = !axisFlag.empty() ? axisFlag[0] - 'x' : std::max(0, symmetryAxis(n));
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                const double tol = a.get<double>("tolerance") > 0.0 ? a.get<double>("tolerance") : symmetryTolerance(m);
                geo::SymmetryMap map;
                ComponentMode from = components::mode();
                auto selectedEdge = [&]() {
                    Ids sel = from == ComponentMode::Object ? Ids{} : components::convert(m, from, components::selected(n.id(), from), ComponentMode::Edge);
                    if (sel.empty())
                        throw CommandError(radial ? "symmetryRebuild: select an edge of " + n.path()
                                                  : "symmetryRebuild: select an edge on the symmetry plane of " + n.path());
                    return sel.front();
                };
                if (!pairsOnly && radial)
                {
                    const int wedge = a.get<int>("wedge") >= 0 ? a.get<int>("wedge") : keepWedge(n, m, *spec->radial, tol);
                    map = geo::symmetrizeRadialTopology(m, *spec->radial, wedge, tol);
                    if (m.numFaces() == 0)
                        throw CommandError(std::format("symmetryRebuild: {} has nothing in wedge {}", n.path(), wedge));
                    rebuilt = true;
                }
                else if (!pairsOnly)
                {
                    const double plane = symmetryOffset(n, axis);
                    const int keep = side == "positive" ? 1 : side == "negative" ? -1 : keepSide(n, m, axis, plane, tol);
                    // Halves that already pair face for face only need their points moved, which keeps each half's own UVs.
                    geo::Mesh paired = m;
                    geo::SymmetryMap pairs = geo::SymmetryMap::persistent(paired, axis, tol, nullptr, plane);
                    if (pairsEveryFace(paired, pairs))
                    {
                        pairs.symmetrize(paired, keep);
                        m = std::move(paired);
                        map = std::move(pairs);
                    }
                    else
                    {
                        map = geo::symmetrizeTopology(m, axis, keep, tol, plane);
                        if (m.numFaces() == 0)
                            throw CommandError(std::format("symmetryRebuild: {} has nothing on the {} side of the {} plane", n.path(),
                                                           keep > 0 ? "+" : "-", char('X' + axis)));
                        rebuilt = true;
                    }
                    const std::string kept = std::format("{}{}", keep > 0 ? "+" : "-", char('X' + axis));
                    log::write(LogLevel::Info, std::format("{}: rebuilt from its {} side", n.path(), kept));
                    if (ops::find("viewportToast"))
                        ops::execute("viewportToast", {{"args", {"Rebuilt from " + kept + " side"}}, {"duration", 3.0}});
                }
                else if (a.get<bool>("topological"))
                {
                    map = radial ? geo::SymmetryMap::buildRadialTopological(m, selectedEdge(), *spec->radial)
                                 : geo::SymmetryMap::buildTopological(m, selectedEdge(), geo::axisPlane(axis, symmetryOffset(n, axis)));
                    if (map.empty())
                        throw CommandError("symmetryRebuild: the topology of " + n.path() + " is not symmetric about that edge");
                    map.store(m, axis);
                }
                else
                {
                    map = radial ? geo::SymmetryMap::buildRadial(m, *spec->radial, tol) : geo::SymmetryMap::build(m, geo::axisPlane(axis, symmetryOffset(n, axis)), tol);
                    map.store(m, axis);
                }
                out[n.path()] = map.unmatchedCount();
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            }
            if (rebuilt)
                components::clear();
            return out;
        },
    });

    std::vector<FlagDef> symmetrizeRadialFlags = radialFlags;
    symmetrizeRadialFlags.push_back({"wedge", "", T::Int, -1,
                                     "The wedge that is kept; -1: the one most of the selected components are in, else the one holding most of the mesh.", {}, {}, -1.0});
    symmetrizeRadialFlags.push_back({"tolerance", "", T::Float, 0.0, "Weld distance along the seams and the axis, cm (0: scaled to the mesh).", {}, {}, 0.0});
    ops::registerCommand({
        .name = "symmetrizeRadial",
        .doc = "Rebuild meshes exactly radially symmetric: the kept wedge is repeated round the axis (and mirrored within each wedge with "
               "mirror=True), replacing the rest, and welded along the seams and the axis; then the mesh's symmetry is set to radial with "
               "these settings, so every edit after keeps it. Settings default to the mesh's own radial symmetry. Model one wedge and run "
               "this to get the whole. Returns the faces per mesh.",
        .positionalDoc = "objects",
        .flags = symmetrizeRadialFlags,
        .run = [symmetryTargets, applyRadialFlags, keepWedge](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            std::vector<std::string> names = symmetryTargets(a, "symmetrizeRadial");
            if (names.empty())
                throw CommandError("symmetrizeRadial: select a mesh");
            json out = json::object();
            UndoChunk chunk("Symmetrize Radial");
            for (const std::string& name : names)
            {
                Node& n = meshArg(name, "symmetrizeRadial");
                applyRadialFlags(a, n, "symmetrizeRadial");
                if (n.as<int64_t>(MeshNode::kSymmetry) != kSymmetryRadial)
                {
                    // A custom mirror plane says nothing about where the radial axis goes.
                    if (n.as<int64_t>(MeshNode::kSymmetryPlane) == 1)
                        g.setValue({n.id(), MeshNode::kSymmetryPlane, -1}, int64_t(0));
                    g.setValue({n.id(), MeshNode::kSymmetry, -1}, kSymmetryRadial);
                }
                const geo::SymmetrySpec spec = *symmetrySpec(n);
                PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                const double tol = a.get<double>("tolerance") > 0.0 ? a.get<double>("tolerance") : symmetryTolerance(m);
                const int wedge = a.get<int>("wedge") >= 0 ? a.get<int>("wedge") : keepWedge(n, m, *spec.radial, tol);
                if (wedge >= spec.radial->order())
                    throw CommandError(std::format("symmetrizeRadial: {} has wedges 0 to {}", n.path(), spec.radial->order() - 1));
                geo::symmetrizeRadialTopology(m, *spec.radial, wedge, tol);
                if (m.numFaces() == 0)
                    throw CommandError(std::format("symmetrizeRadial: {} has nothing in wedge {}", n.path(), wedge));
                out[n.path()] = m.numFaces();
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            }
            components::clear();
            return out;
        },
    });
}
} // namespace lila::scene::detail
