#include "uv/UvCommands.h"

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Log.h"
#include "scene/Camera.h"
#include "scene/Components.h"
#include "scene/DeformEval.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "geo/Symmetry.h"
#include "uv/Hotspot.h"
#include "uv/SymmetryUv.h"
#include "uv/Uv.h"
#include "uv/UvEdit.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <unordered_set>

namespace lila::uv
{
namespace
{
using scene::ComponentMode;
using scene::Graph;
using scene::MeshNode;
using scene::MeshPtr;
using scene::Node;
using scene::NodeId;
using scene::components::Ids;

Graph& graph() { return Graph::current(); }

/** The base mesh's plug under any modifiers, so uv edits keep the stack. */
scene::PlugRef basePlug(Node& n)
{
    return scene::modifiers::editPlug(graph(), n.id());
}

geo::Mesh baseMesh(const scene::PlugRef& plug)
{
    return *std::get<MeshPtr>(graph().storedValue(plug));
}

/** The mesh component ids and uvs index: the base under any modifiers. */
MeshPtr cageOf(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    return n && n->type().isA("mesh") ? scene::modifiers::cage(g, id) : nullptr;
}

std::string uvSetArg(const Args& a)
{
    return a.has("uvSet") ? a.get<std::string>("uvSet") : currentUvSet();
}

/** The mesh's uv sets (uv0, uv1, ...) in number order. */
std::vector<std::string> uvSetNames(const geo::Mesh& m)
{
    std::vector<std::pair<int, std::string>> found;
    for (const std::string& name : m.cornerAttrs().names())
    {
        const geo::Attribute* at = m.cornerAttrs().find(name);
        if (!at || at->type() != geo::AttrType::Float2 || name.size() < 3 || !name.starts_with("uv")
            || !std::all_of(name.begin() + 2, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        found.emplace_back(std::stoi(name.substr(2)), name);
    }
    std::ranges::sort(found);
    std::vector<std::string> out;
    for (auto& [index, name] : found)
        out.push_back(std::move(name));
    return out;
}

/** Renames the sets so order[i] becomes uv<i>; renderers and exporters stop at the first missing number. */
void renumberUvSets(geo::Mesh& m, const std::vector<std::string>& order)
{
    geo::AttributeSet& corners = m.cornerAttrs();
    for (size_t i = 0; i < order.size(); ++i)
        corners.rename(order[i], "~uvset" + std::to_string(i));
    for (size_t i = 0; i < order.size(); ++i)
        corners.rename("~uvset" + std::to_string(i), geo::attr::uvName(int(i)));
}

/** Meshes to work on: the component selection converted to kind, or whole selected meshes in object mode. */
std::vector<NodeId> selectedMeshes(const Graph& g)
{
    if (includeChildren())
        return meshesUnder(g, g.selection());
    std::vector<NodeId> out;
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("mesh"))
            out.push_back(id);
    return out;
}

std::map<NodeId, Ids> targets(ComponentMode kind, bool allowWhole)
{
    Graph& g = graph();
    g.evaluate();
    std::map<NodeId, Ids> out;
    ComponentMode from = scene::components::mode();
    // An instance's uvs are its root's: the edit goes there, so every member shows it.
    if (from != ComponentMode::Object)
        for (auto& [node, ids] : scene::components::selected(from))
            if (MeshPtr cage = cageOf(g, node))
            {
                Ids conv = scene::components::convert(*cage, from, ids, kind);
                if (conv.empty())
                    continue;
                Ids& into = out[scene::instancing::root(g, node)];
                into.insert(into.end(), conv.begin(), conv.end());
                std::sort(into.begin(), into.end());
                into.erase(std::unique(into.begin(), into.end()), into.end());
            }
    if (out.empty() && allowWhole)
        for (NodeId id : selectedMeshes(g))
            if (cageOf(g, id))
                out[scene::instancing::root(g, id)] = {};
    return out;
}

using UvEditFn = std::function<json(geo::Mesh&, const Ids& ids, Node&)>;

/**
 * Runs fn on a copy of every target mesh and writes it back to inMesh as one undo step. uv edits keep
 * topology, so component ids and the selection stay valid. Returns fn's results keyed by mesh name.
 */
json editUv(const char* label, ComponentMode kind, const UvEditFn& fn, bool allowWhole = true)
{
    LILA_ZONE();

    std::map<NodeId, Ids> work = targets(kind, allowWhole);
    if (work.empty())
        throw CommandError(std::string(label) + ": select a mesh or some components first");
    Graph& g = graph();
    UndoChunk chunk(label);
    json out = json::object();
    for (auto& [node, ids] : work)
    {
        Node& n = *g.node(node);
        scene::PlugRef base = basePlug(n);
        geo::Mesh mesh = baseMesh(base);
        out[n.path()] = fn(mesh, ids, n);
        g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    }
    return out.size() == 1 ? out.begin().value() : out;
}

/** Same targets, read only. */
json queryUv(const std::function<json(const geo::Mesh&, const Ids&, Node&)>& fn)
{
    std::map<NodeId, Ids> work = targets(ComponentMode::Face, true);
    if (work.empty())
        throw CommandError("select a mesh or some components first");
    json out = json::object();
    for (auto& [node, ids] : work)
    {
        Node& n = *graph().node(node);
        out[n.path()] = fn(*cageOf(graph(), node), ids, n);
    }
    return out.size() == 1 ? out.begin().value() : out;
}

std::vector<ProjectionRecord>& lastProjectionsMutable()
{
    static std::vector<ProjectionRecord> records;
    return records;
}

/** Keeps lastProjections() matching the projection undo and redo land on, for the projection handles. */
struct ProjectionsAction : UndoAction
{
    std::vector<ProjectionRecord> before, after;
    void undo() override { lastProjectionsMutable() = before; }
    void redo() override { lastProjectionsMutable() = after; }
};

glm::dmat3 worldLinear(Node& n)
{
    return glm::dmat3(n.as<glm::dmat4>(scene::Transform::kWorldMatrix));
}

std::vector<double> listArg(const Args& a, std::string_view name, size_t n, double fallback);

PackOptions packArgs(const Args& a)
{
    PackOptions p;
    p.resolution = std::max(4, a.get<int>("resolution", 1024));
    p.padding = std::max(0.0, a.get<double>("padding", 4.0));
    p.margin = a.get<double>("margin", -1.0);
    p.paddingMips = std::clamp(a.get<int>("paddingMips", 0), 0, 12);
    p.rotateStep = std::clamp(a.get<int>("rotateStep", 90), 0, 180);
    if (a.has("angles"))
        for (const json& v : a.flags[std::string("angles")])
            p.angles.push_back(v.get<double>());
    p.freeRotation = a.get<bool>("freeRotation", false);
    p.flip = a.get<bool>("flip", false);
    p.preOrient = a.get<bool>("orient", true);
    std::string scale = a.get<std::string>("scaleMode", "normalize");
    p.normalizeScale = scale == "normalize" && a.get<bool>("normalize", true);
    p.lockScale = scale == "lock";
    p.stack = a.get<bool>("stack", false);
    p.stackMirrored = a.get<bool>("stackMirrored", true);
    p.stackTolerance = std::max(0.0, a.get<double>("stackTolerance", 0.002));
    p.tilesU = std::max(1, a.get<int>("tilesU", 1));
    p.tilesV = std::max(1, a.get<int>("tilesV", 1));
    p.texelsPerUnit = std::max(0.0, a.get<double>("texelsPerUnit", 0.0));
    std::vector<double> region = listArg(a, "region", 4, 0.0);
    if (a.has("region"))
    {
        p.regionMin = {region[0], region[1]};
        p.regionMax = {region[2], region[3]};
    }
    p.effort = std::clamp(a.get<int>("effort", 1), 0, 3);
    p.timeLimit = std::max(0.0, a.get<double>("timeLimit", 0.0));
    p.workingResolution = std::max(0, a.get<int>("workingResolution", 0));
    std::string layout = a.get<std::string>("layout", "fill");
    p.layout = layout == "groupTiles" ? PackLayout::GroupTiles : layout == "groupRegions" ? PackLayout::GroupRegions
             : layout == "keepTile"   ? PackLayout::KeepTile
             : layout == "groupLayouts" ? PackLayout::GroupLayouts
                                      : PackLayout::Fill;
    std::string by = a.get<std::string>("groupBy", "none");
    p.groupBy = by == "object" ? PackGroupBy::Object : by == "material" ? PackGroupBy::Material : by == "attribute" ? PackGroupBy::Attribute : PackGroupBy::None;
    p.groupAttr = a.get<std::string>("groupAttr", "uvGroup");
    p.fixPinned = a.get<bool>("fixPinned", false);
    return p;
}

std::vector<FlagDef> packFlags()
{
    using T = ArgType;
    return {presetFlag({"resolution", "", T::Int, 1024, "Target texture size, px. Padding and margin are in its texels.", {}, {}, 4.0}, kTextureSizePresets),
            {"padding", "", T::Float, 4.0, "Texels between islands, held exactly (the search never lets two islands closer).", {}, {}, 0.0},
            {"margin", "", T::Float, -1.0, "Texels from the tile (or region) border; negative is half the padding."},
            {"paddingMips", "", T::Int, 0, "Padding and margin survive this many mip levels: both are multiplied by 2^paddingMips.", {}, {}, 0.0, 12.0},
            {"rotateStep", "", T::Int, 90, "Rotation step to try, degrees: 0 none, 90 quarter turns, smaller for more.", {}, {}, 0.0, 180.0},
            {"angles", "", T::List, nullptr, "Exact angles to try, degrees (replaces rotateStep), e.g. [0, 90, 45]."},
            {"freeRotation", "", T::Bool, false, "Any angle: each island also tries its three best-fitting angles (smallest bounding box) and their quarter turns."},
            {"flip", "", T::Bool, false, "Also try islands mirrored (the texture then reads mirrored on them)."},
            {"orient", "", T::Bool, true, "Turn islands to their smallest bounding rectangle first."},
            {"scaleMode", "", T::String, "normalize",
             "normalize: even texel density across islands and objects (world space). keep: islands keep their relative uv sizes. "
             "lock: islands keep their exact uv size and are only moved and turned.",
             {"normalize", "keep", "lock"}},
            {"normalize", "", T::Bool, true, "Same as scaleMode=normalize when on, keep when off."},
            {"stack", "", T::Bool, false, "Islands with the same shape (within stackTolerance) share one spot."},
            {"stackMirrored", "", T::Bool, true, "Mirror-image islands stack too (the mirrored halves of a symmetric model)."},
            {"stackTolerance", "", T::Float, 0.002, "Largest vertex mismatch for two islands to stack, as a fraction of the island's size.", {}, {}, 0.0},
            {"tilesU", "", T::Int, 1, "UDIM tiles across.", {}, {}, 1.0},
            {"tilesV", "", T::Int, 1, "UDIM tiles up.", {}, {}, 1.0},
            {"region", "", T::List, nullptr, "Pack into [uMin, vMin, uMax, vMax] of each tile instead of all of it."},
            {"texelsPerUnit", "", T::Float, 0.0, "Pack at this density (px/cm) instead of filling the tiles; overflow spills into further tiles.", {}, {}, 0.0},
            {"effort", "", T::Int, 1, "Search effort 0-3: island orders tried per scale (1, 3, 6, 12). Higher packs denser, slower.", {}, {}, 0.0, 3.0},
            {"timeLimit", "", T::Float, 0.0, "Stop refining after this many seconds (0 never); results can then vary between runs.", {}, {}, 0.0},
            {"workingResolution", "", T::Int, 0, "Search raster cells across a tile; 0 uses the resolution up to 4096.", {}, {}, 0.0}};
}

/** Flags only uvPack has: where groups go and what stays fixed. */
std::vector<FlagDef> packLayoutFlags()
{
    using T = ArgType;
    return {{"layout", "", T::String, "fill",
             "fill: fill the UDIM tiles in order. groupTiles: one tile per group. groupRegions: one part of tile 1001 per group, "
             "sized by its area. keepTile: islands stay in the tile they are in. groupLayouts: each group packs on its own into the "
             "full tiles, overlapping the others (a texture set per material).",
             {"fill", "groupTiles", "groupRegions", "keepTile", "groupLayouts"}},
            {"groupBy", "", T::String, "none", "What makes a group: object, material slot, or the face int attribute groupAttr.",
             {"none", "object", "material", "attribute"}},
            {"groupAttr", "", T::String, "uvGroup", "Face int attribute holding each face's group, for groupBy=attribute."},
            {"keep", "", T::String, "none",
             "selected: the selected islands stay where they are and the rest pack around them. unselected: only the selection packs, "
             "around the rest.",
             {"none", "selected", "unselected"}},
            {"fixPinned", "", T::Bool, false, "Islands with pinned uvs stay where they are."},
            {"perObject", "", T::Bool, false, "Pack each selected mesh into a layout of its own instead of one shared layout."}};
}

json packJson(const PackResult& r)
{
    return {{"ok", r.ok},       {"coverage", r.coverage}, {"density", std::round(r.coverage * 1000.0) / 10.0},
            {"texelsPerUnit", r.texelsPerUnit}, {"tiles", r.tilesUsed}, {"stacked", r.stacked},
            {"seconds", r.seconds}, {"layouts", r.attempts}};
}

/** Packs the islands of the faces with their density measured on the exported mesh. */
void packOnExport(geo::Mesh& m, std::span<const Index> faces, std::string_view uvSet, Node& n, const PackOptions& o)
{
    PackTarget t;
    t.mesh = &m;
    t.faces.assign(faces.begin(), faces.end());
    t.stack = exportStack(n);
    PackResult r = packMeshes(std::span<PackTarget>(&t, 1), uvSet, o);
    for (const std::string& w : r.warnings)
        log::warn("{}: {}", n.name(), w);
}

/** The attr marking a transform as a seam view: its weight. */
constexpr const char* kSeamViewAttr = "seamView";

/** Seam views in world space, for seamViewsIn to bring into each mesh's space. */
std::vector<SeamView> seamViewArgs(const Args& a)
{
    std::vector<SeamView> out;
    if (!a.get<bool>("useViews", true))
        return out;
    Graph& g = graph();
    g.evaluate();
    std::vector<Node*> nodes;
    if (a.has("views"))
        for (const json& name : a.flags["views"])
        {
            Node* n = g.find(name.get<std::string>());
            if (!n || !n->type().isA("transform"))
                throw CommandError("views: " + name.get<std::string>() + " is not a camera, locator or mesh");
            nodes.push_back(n);
        }
    else
        for (NodeId id : g.nodes())
            if (Node* n = g.node(id); n && n->type().isA("transform") && n->type().attrIndex(kSeamViewAttr) >= 0)
                nodes.push_back(n);
    for (Node* n : nodes)
    {
        SeamView v;
        const glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        v.position = glm::dvec3(world[3]);
        // A mesh is a plane: it sees along its average normal.
        if (MeshPtr mesh = cageOf(g, n->id()))
        {
            glm::dvec3 normal(0.0), center(0.0);
            double area = 0.0;
            for (Index f = 0; f < mesh->faceCapacity(); ++f)
                if (mesh->faceValid(f))
                {
                    normal += mesh->faceAreaVector(f);
                    center += mesh->faceCentroid(f) * mesh->faceArea(f);
                    area += mesh->faceArea(f);
                }
            normal = glm::transpose(glm::inverse(glm::dmat3(world))) * normal;
            if (area > 0.0 && glm::length(normal) > 1e-12)
            {
                v.position = glm::dvec3(world * glm::dvec4(center / area, 1.0));
                v.planeNormal = glm::normalize(normal);
            }
        }
        if (int i = n->type().attrIndex(kSeamViewAttr); i >= 0)
            if (const double* w = std::get_if<double>(&n->value(i)))
                v.weight = *w;
        out.push_back(v);
    }
    return out;
}

/** Seam views brought from world space into the node's. */
std::vector<SeamView> viewsIn(std::vector<SeamView> views, Node& node)
{
    const glm::dmat4 world = node.as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::dmat4 toObject = glm::inverse(world);
    for (SeamView& v : views)
    {
        v.position = glm::dvec3(toObject * glm::dvec4(v.position, 1.0));
        if (v.planeNormal)
            v.planeNormal = glm::normalize(glm::transpose(glm::dmat3(world)) * *v.planeNormal);
    }
    return views;
}

/** The options for one mesh: seam views in its space, and with symmetry on, its mirror edges. */
AutoSeamOptions seamsIn(AutoSeamOptions o, Node& node, const geo::Mesh& m, const std::optional<geo::SymmetryMap>& map)
{
    o.views = viewsIn(std::move(o.views), node);
    if (map)
    {
        std::vector<Index> all(m.edgeCapacity());
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            all[e] = m.edgeValid(e) ? e : kInvalid;
        o.mirrorEdge = map->mirrorEdges(m, all);
    }
    return o;
}

AutoSeamOptions seamArgs(const Args& a)
{
    AutoSeamOptions o;
    o.hardAngle = a.get<double>("hardAngle", 60.0);
    o.maxDistortion = std::max(0.0, a.get<double>("maxDistortion", 0.08));
    o.keepSeams = a.get<bool>("keepSeams", false);
    o.visibility = a.get<bool>("visibility", true);
    o.merge = a.get<bool>("merge", true);
    o.useHardEdges = a.get<bool>("useHardEdges", true);
    std::string method = a.get<std::string>("method", "split");
    if (method != "split" && method != "optcuts")
        throw CommandError("method must be split or optcuts");
    o.method = method == "optcuts" ? SeamMethod::OptCuts : SeamMethod::Split;
    o.distortionBound = std::max(0.0, a.get<double>("distortionBound", 0.05));
    o.seamWeight = std::max(0.0, a.get<double>("seamWeight", 0.5));
    o.views = seamViewArgs(a);
    return o;
}

FlagDef viewsFlag()
{
    return nodeFlag({"views", "", ArgType::List, nullptr,
                     "Cameras, locators or planes seams hide from; every seam view in the scene when not given. A plane (any mesh) "
                     "sees along its normal, like an orthographic camera."},
                    "transform");
}

std::vector<FlagDef> seamFlagDefs()
{
    using T = ArgType;
    return {{"hardAngle", "", T::Float, 60.0, "Edges sharper than this always get a seam, degrees.", {}, {}, 0.0, 180.0},
            {"useHardEdges", "", T::Bool, true, "Edges marked hard always get a seam."},
            {"keepSeams", "", T::Bool, false, "Keep the seams already there."},
            {"maxDistortion", "", T::Float, 0.08, "Split islands that stretch more than this when flattened.", {}, {}, 0.0},
            {"visibility", "", T::Bool, true, "Hide cuts where they are least visible."},
            {"merge", "", T::Bool, true, "Merge islands back while they still flatten well."},
            {"method", "", T::String, "split",
             "split (hard edges, open to disks, split where stretched) or optcuts (Auto Seams 2: seams and distortion optimised together).",
             {"split", "optcuts"}},
            {"distortionBound", "", T::Float, 0.05, "optcuts: stretch (symmetric Dirichlet excess, all islands) to get under.", {}, {}, 0.0},
            {"seamWeight", "", T::Float, 0.5, "optcuts: price of seam length against stretch; higher keeps seams shorter.", {}, {}, 0.0},
            {"useViews", "", T::Bool, true, "Keep seams out of sight of the seam views (uvSeamView)."},
            viewsFlag()};
}

template <class... L> std::vector<FlagDef> join(std::vector<FlagDef> a, L... rest)
{
    (a.insert(a.end(), rest.begin(), rest.end()), ...);
    return a;
}

std::vector<double> listArg(const Args& a, std::string_view name, size_t n, double fallback)
{
    std::vector<double> out(n, fallback);
    if (!a.has(name))
        return out;
    const json& j = a.flags[std::string(name)];
    if (j.is_number())
        std::fill(out.begin(), out.end(), j.get<double>());
    else
        for (size_t i = 0; i < n && i < j.size(); ++i)
            out[i] = j[i].get<double>();
    return out;
}

/** Unmarks the seams and welds the uvs across them, first moving each smaller island onto its neighbour. */
json sewEdges(geo::Mesh& m, std::span<const Index> edges, std::string_view uvSet, bool move = true)
{
    std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    std::span<glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(uvSet);
    size_t sewn = 0;
    if (uv.empty())
    {
        for (Index e : edges)
            if (m.edgeValid(e))
                seam[e] = false;
        return {{"sewn", 0}};
    }
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = uvSet;
    std::vector<std::vector<Index>> islands = islandFaces(m, {}, co);
    std::vector<Index> islandOf(m.faceCapacity(), kInvalid);
    for (size_t i = 0; i < islands.size(); ++i)
        for (Index f : islands[i])
            islandOf[f] = Index(i);
    std::vector<uint8_t> moved(islands.size(), 0);

    struct Pair
    {
        Index a, b; // corners on either side at the same point
    };
    std::vector<Pair> welds;
    for (Index e : edges)
    {
        if (!m.edgeValid(e) || !m.isManifoldEdge(e) || m.isBoundaryEdge(e))
            continue;
        seam[e] = false;
        Index c1 = m.edgeCorner(e), c2 = m.cornerRadial(c1);
        bool opposite = m.cornerPoint(c1) != m.cornerPoint(c2);
        Index a1 = c1, a2 = opposite ? m.cornerNext(c2) : c2;
        Index b1 = m.cornerNext(c1), b2 = opposite ? c2 : m.cornerNext(c2);
        Index i1 = islandOf[m.cornerFace(c1)], i2 = islandOf[m.cornerFace(c2)];
        if (move && i1 != i2 && !moved[i1] && !moved[i2])
        {
            // The smaller island travels: rotate its edge onto the other side's, then translate.
            bool firstSmaller = islands[i1].size() < islands[i2].size();
            Index mover = firstSmaller ? i1 : i2;
            glm::dvec2 fa = firstSmaller ? uv[a1] : uv[a2], fb = firstSmaller ? uv[b1] : uv[b2];
            glm::dvec2 ta = firstSmaller ? uv[a2] : uv[a1], tb = firstSmaller ? uv[b2] : uv[b1];
            double ang = std::atan2(tb.y - ta.y, tb.x - ta.x) - std::atan2(fb.y - fa.y, fb.x - fa.x);
            double c = std::cos(ang), s = std::sin(ang);
            for (Index f : islands[mover])
                m.forEachFaceCorner(f, [&](Index k) {
                    glm::dvec2 d = glm::dvec2(uv[k]) - fa;
                    uv[k] = glm::vec2(ta + glm::dvec2(d.x * c - d.y * s, d.x * s + d.y * c));
                });
            moved[mover] = 1;
        }
        welds.push_back({a1, a2});
        welds.push_back({b1, b2});
        ++sewn;
    }
    for (const Pair& w : welds)
    {
        glm::vec2 oa = uv[w.a], ob = uv[w.b], mid = (oa + ob) * 0.5f;
        Index p = m.cornerPoint(w.a);
        auto same = [](glm::vec2 x, glm::vec2 y) { return glm::all(glm::lessThanEqual(glm::abs(x - y), glm::vec2(1e-6f))); };
        m.forEachPointCorner(p, [&](Index k) {
            if (same(uv[k], oa) || same(uv[k], ob))
                uv[k] = mid;
        });
    }
    return {{"sewn", sewn}};
}
} // namespace

Stack exportStack(Node& n)
{
    Graph& g = graph();
    const NodeId mesh = scene::instancing::root(g, n.id());
    const std::vector<NodeId> mods = scene::modifiers::stack(g, mesh);
    // A modifier showing its input for editing makes that input the cage; it and the ones after it still export.
    auto first = std::find_if(mods.begin(), mods.end(), [&](NodeId id) {
        const auto* m = scene::modifiers::asModifier(g.node(id));
        return m && m->editingInput();
    });
    if (first == mods.end())
        first = mods.begin();
    Stack out;
    for (auto it = first; it != mods.end(); ++it)
    {
        auto* mod = scene::modifiers::asModifier(g.node(*it));
        if (!mod || !scene::modifiers::exportEnabled(*mod))
            continue;
        std::string name = mod->name();
        const bool subdivides = mod->type().name == "subdivisionModifier";
        if (!mod->apply)
            out.push_back({name, [name](const geo::Mesh&) -> geo::Mesh { throw std::runtime_error(name + " can't be re-run"); }, subdivides});
        else
            out.push_back({name, [mod](const geo::Mesh& in) {
                               scene::deformEval::CpuOnly cpu;
                               return mod->apply(in, *mod);
                           }, subdivides});
    }
    return out;
}

json refineOnExport(geo::Mesh& m, std::span<const Index> faces, std::string_view uvSet, Node& n, const FinalSolveOptions& opts, bool on)
{
    if (!on)
        return nullptr;
    const Stack stack = exportStack(n);
    if (stack.empty())
        return nullptr;
    LILA_ZONE_N("refineOnExport");
    const FinalRefineResult r = refineFinal(m, faces, uvSet, stack, opts);
    if (!r.warning.empty())
    {
        log::warn("{}: {}", n.name(), r.warning);
        return {{"method", "cage"}, {"warning", r.warning}};
    }
    if (r.method == FinalMethod::Cage)
        return nullptr;
    return {{"method", "probed"},
            {"islands", r.islands},
            {"failed", r.failed},
            {"collapsed", {r.collapsedBefore, r.collapsedAfter}},
            {"angleDistortion", {r.distortionBefore, r.distortionAfter}}};
}

FinalMap exportTraceOf(const geo::Mesh& m, Node& n, bool on)
{
    if (!on)
        return {};
    const Stack stack = exportStack(n);
    if (stack.empty())
        return {};
    FinalMap map = traceFaces(m, stack);
    if (!map.warning.empty())
        log::warn("{}: {}", n.name(), map.warning);
    return map;
}

json finishOnExport(geo::Mesh& m, std::span<const Index> faces, std::string_view uvSet, Node& n, bool on, const PackOptions* repack)
{
    json fm = refineOnExport(m, faces, uvSet, n, {}, on);
    if (repack && fm.is_object() && fm.value("method", "") == "probed")
        packOnExport(m, faces, uvSet, n, *repack);
    return fm;
}

std::string densityOnExport(const geo::Mesh& m, std::span<Chart> charts, std::string_view uvSet, Node& n, bool on)
{
    if (!on)
        return {};
    const Stack stack = exportStack(n);
    if (stack.empty())
        return {};
    const FinalMap map = applyDensityAreas(m, charts, uvSet, stack);
    if (!map.warning.empty())
        log::warn("{}: {}", n.name(), map.warning);
    return map.warning;
}

json editUvMeshes(const char* label, ComponentMode kind, const UvMeshEditFn& fn, bool allowWhole)
{
    return editUv(label, kind, fn, allowWhole);
}

scene::PlugRef uvEditPlug(Node& n)
{
    return basePlug(n);
}

json sewUvEdges(geo::Mesh& m, std::span<const Index> edges, std::string_view uvSet, bool move)
{
    return sewEdges(m, edges, uvSet, move);
}

int uvSymmetryAxis(const Node& n)
{
    if (!n.type().isA("mesh"))
        return -1;
    int64_t axis = n.as<int64_t>(MeshNode::kSymmetry);
    return axis >= 1 && axis <= 3 ? int(axis - 1) : -1;
}

namespace
{
double symmetryTolerance(const geo::Mesh& mesh)
{
    return std::max(1e-4, glm::length(mesh.bounds().size()) * 1e-5);
}

std::vector<uint8_t> foldedFaces(const geo::Mesh& m, std::string_view uvSet)
{
    std::vector<FaceDistortion> d = faceDistortion(m, uvSet);
    std::vector<uint8_t> out(d.size(), 0);
    for (size_t f = 0; f < d.size(); ++f)
        out[f] = d[f].folded ? 1 : 0;
    return out;
}

/**
 * Puts back the uvs from `edited` on every island that symmetrizing folded: mirroring a layout that is not itself symmetric
 * can turn faces over near the plane, and an unmirrored island is better than a folded one.
 */
void restoreFolded(geo::Mesh& m, std::string_view uvSet, std::span<const glm::vec2> edited)
{
    std::span<glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(uvSet);
    if (uv.size() != edited.size())
        return;
    std::vector<uint8_t> now = foldedFaces(m, uvSet);
    std::vector<glm::vec2> mirrored(uv.begin(), uv.end());
    std::copy(edited.begin(), edited.end(), uv.begin());
    std::vector<uint8_t> was = foldedFaces(m, uvSet);
    std::vector<Index> seeds;
    for (Index f = 0; f < now.size(); ++f)
        if (now[f] && !was[f])
            seeds.push_back(f);
    if (seeds.empty())
    {
        std::copy(mirrored.begin(), mirrored.end(), uv.begin());
        return;
    }
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = uvSet;
    // Whole islands as the edit left them, grown by the islands mirroring made of them, so nothing tears.
    std::copy(mirrored.begin(), mirrored.end(), uv.begin());
    std::vector<Index> grown = islandsOfFaces(m, seeds, co);
    std::copy(edited.begin(), edited.end(), uv.begin());
    std::vector<uint8_t> restore(m.faceCapacity(), 0);
    for (Index f : islandsOfFaces(m, grown, co))
        restore[f] = 1;
    for (Index c = 0; c < uv.size(); ++c)
        if (!m.cornerValid(c) || !restore[m.cornerFace(c)])
            uv[c] = mirrored[c];
}
} // namespace

std::optional<geo::SymmetryMap> uvSymmetryMap(const Node& n, const geo::Mesh& mesh)
{
    const int axis = uvSymmetryAxis(n);
    if (axis < 0)
        return std::nullopt;
    geo::Mesh copy = mesh;
    return geo::SymmetryMap::persistent(copy, axis, symmetryTolerance(copy), nullptr, symmetryOffset(n, axis));
}

json withUvSymmetry(geo::Mesh& m, const Node& n, std::string_view uvSet, UvSymmetry mode, std::span<const Index> moved, const std::function<json()>& edit)
{
    if (const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(n); spec && spec->radial)
    {
        // Radial copies stack in uv: whatever the edit did to one wedge's faces, their images take the same uvs.
        geo::SymmetryMap map = geo::SymmetryMap::persistent(m, *spec->radial, symmetryTolerance(m));
        std::span<const glm::vec2> was = m.cornerAttrs().get<glm::vec2>(uvSet);
        std::vector<glm::vec2> before(was.begin(), was.end());
        json r = edit();
        std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(uvSet);
        before.resize(uv.size(), glm::vec2(0.0f));
        std::vector<Index> faces;
        for (Index c = 0; c < uv.size(); ++c)
            if (m.cornerValid(c) && (uv[c] != before[c] || std::ranges::binary_search(moved, c)))
                faces.push_back(m.cornerFace(c));
        std::ranges::sort(faces);
        faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
        if (!faces.empty())
            stackRadialUvs(m, map, uvSet, radialFaceWedge(m, map, faces), faces);
        return r;
    }
    const int axis = uvSymmetryAxis(n);
    if (axis < 0)
        return edit();
    geo::SymmetryMap map = geo::SymmetryMap::persistent(m, axis, symmetryTolerance(m), nullptr, symmetryOffset(n, axis));
    std::optional<UvMirror> mirror;
    if (mode == UvSymmetry::Delta)
        mirror = buildUvMirror(m, map, uvSet);
    std::span<const glm::vec2> was = m.cornerAttrs().get<glm::vec2>(uvSet);
    std::vector<glm::vec2> before(was.begin(), was.end());
    before.resize(m.cornerCapacity(), glm::vec2(0.0f));
    json r = edit();
    std::span<glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(uvSet);
    if (uv.size() != before.size())
        return r;
    std::vector<Index> changed;
    for (Index c = 0; c < uv.size(); ++c)
        if (uv[c] != before[c])
            changed.push_back(c);
    if (mode == UvSymmetry::Absolute)
    {
        // Islands as the edit left them, placed through the pairing the uvs had before.
        UvMirror after = buildUvMirror(m, map, uvSet);
        fitUvMirror(after, before);
        std::vector<glm::vec2> edited(uv.begin(), uv.end());
        symmetrizeUvIslands(after, changed, uvMirrorDrive(after, changed), uv);
        restoreFolded(m, uvSet, edited);
        return r;
    }
    std::vector<Index> all(moved.begin(), moved.end());
    all.insert(all.end(), changed.begin(), changed.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    applyUvFollow(uvFollowRules(*mirror, all, uvMirrorDrive(*mirror, all)), before, uv);
    return r;
}

std::vector<Index> mirrorComponents(const geo::SymmetryMap& map, const geo::Mesh& m, ComponentMode mode, std::span<const Index> ids)
{
    switch (mode)
    {
    case ComponentMode::Vertex: return map.expandPoints(ids);
    case ComponentMode::Edge: return map.expandEdges(m, ids);
    case ComponentMode::Face: return map.expandFaces(m, ids);
    default: return {ids.begin(), ids.end()};
    }
}

UvMeshEditFn symmetricUvEdit(std::string uvSet, UvMeshEditFn fn)
{
    return [uvSet = std::move(uvSet), fn = std::move(fn)](geo::Mesh& m, const Ids& ids, Node& n) -> json {
        return withUvSymmetry(m, n, uvSet, UvSymmetry::Absolute, {}, [&] { return fn(m, ids, n); });
    };
}

std::vector<Index> mirrorCorners(const geo::SymmetryMap& map, const geo::Mesh& m, std::span<const Index> corners)
{
    std::vector<Index> out(corners.begin(), corners.end());
    for (Index c : corners)
    {
        if (!m.cornerValid(c))
            continue;
        for (int g = 1; g < map.order(); ++g)
        {
            Index f = map.imageFace(m, g, m.cornerFace(c)), p = map.image(g, m.cornerPoint(c));
            if (f != kInvalid && p != kInvalid)
                if (Index mc = m.findCorner(f, p); mc != kInvalid)
                    out.push_back(mc);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<Index> withMirrorCorners(const Node& n, const geo::Mesh& m, std::span<const Index> corners)
{
    if (std::optional<geo::SymmetryMap> map = uvSymmetryMap(n, m))
        return mirrorCorners(*map, m, corners);
    std::vector<Index> out(corners.begin(), corners.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<Index> selectionCorners(const geo::Mesh& m, scene::NodeId node, ComponentMode kind, std::span<const uint32_t> ids, std::string_view uvSet)
{
    if (kind == ComponentMode::Vertex)
        if (const Ids& picked = scene::components::uvCorners(node); !picked.empty())
        {
            std::vector<Index> out;
            for (uint32_t c : picked)
                if (m.cornerValid(c))
                    out.push_back(c);
            return out;
        }
    if (kind == ComponentMode::Face || ids.empty())
        return uvCornersOfFaces(m, uvSet, ids);
    return cornersOfPoints(m, scene::components::convert(m, kind, Ids(ids.begin(), ids.end()), ComponentMode::Vertex));
}

std::vector<Index> withMirrorEdges(const Node& n, const geo::Mesh& m, std::span<const Index> edges)
{
    if (std::optional<geo::SymmetryMap> map = uvSymmetryMap(n, m))
        return map->expandEdges(m, edges);
    std::vector<Index> out(edges.begin(), edges.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

namespace
{
/**
 * When the faces lie on both sides of the symmetry plane: those on the side holding more of them, plus any across the plane.
 * The projection fits and maps these and the other side mirrors them; a fit through both halves averages their normals away.
 */
std::vector<Index> oneSide(const geo::Mesh& m, const geo::SymmetryMap& map, std::span<const Index> faces)
{
    std::vector<Index> pos, neg, across;
    for (Index f : liveFaces(m, faces))
    {
        int sum = 0;
        m.forEachFaceCorner(f, [&](Index c) { sum += map.side(m.cornerPoint(c)); });
        (sum > 0 ? pos : sum < 0 ? neg : across).push_back(f);
    }
    if (pos.empty() || neg.empty())
        return {};
    std::vector<Index>& out = pos.size() >= neg.size() ? pos : neg;
    out.insert(out.end(), across.begin(), across.end());
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

std::function<std::optional<glm::dvec3>()>& projectionViewProvider()
{
    static std::function<std::optional<glm::dvec3>()> fn;
    return fn;
}

std::function<std::optional<glm::dmat4>()>& projectionCameraProvider()
{
    static std::function<std::optional<glm::dmat4>()> fn;
    return fn;
}

namespace
{
/** World to clip for a camera node, or for the active view when name is empty, with clip Y up so V follows the image. */
glm::dmat4 cameraWorldToClip(const std::string& name)
{
    glm::dmat4 m{1.0};
    if (name.empty())
    {
        std::optional<glm::dmat4> view = projectionCameraProvider() ? projectionCameraProvider()() : std::nullopt;
        if (!view)
            throw CommandError("uvProject: type=camera needs camera= or matrix= when there is no viewport");
        m = *view;
    }
    else
    {
        Graph& g = graph();
        g.evaluate();
        Node* n = g.find(name);
        if (!n || !n->type().isA("camera"))
            throw CommandError("uvProject: " + name + " is not a camera");
        glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        m = scene::camera::projection(*n, scene::renderSettings().deviceAspect()) * scene::camera::viewMatrix(world);
    }
    // Vulkan clip space has Y down.
    return glm::scale(glm::dmat4(1.0), glm::dvec3(1.0, -1.0, 1.0)) * m;
}
} // namespace

const std::vector<ProjectionRecord>& lastProjections()
{
    return lastProjectionsMutable();
}

std::vector<NodeId> meshesUnder(const Graph& g, std::span<const NodeId> roots)
{
    std::vector<NodeId> out;
    std::unordered_set<NodeId> seen;
    auto add = [&](NodeId id) {
        if (Node* n = g.node(id); n && n->type().isA("mesh") && seen.insert(id).second)
            out.push_back(id);
    };
    for (NodeId root : roots)
    {
        add(root);
        for (NodeId d : g.descendants(root))
            add(d);
    }
    return out;
}

bool includeChildren()
{
    return PrefStore::get().value<bool>("general", "/uv/includeChildren", true);
}

void setIncludeChildren(bool on)
{
    PrefStore::get().set("general", "/uv/includeChildren", on);
}

std::vector<NodeId> shownMeshes()
{
    Graph& g = graph();
    std::vector<NodeId> out = selectedMeshes(g);
    ComponentMode mode = scene::components::mode();
    // Component selections outlive the object selection, so only the current kind counts, and only with no objects selected.
    if (out.empty() && mode != ComponentMode::Object)
        for (auto& [id, ids] : scene::components::selected(mode))
            if (Node* n = g.node(id); n && n->type().isA("mesh") && !ids.empty())
                out.push_back(id);
    return out;
}

void registerCommands()
{
    registerLiveCommands();
    registerOptimizeCommands();

    using T = ArgType;
    using K = ComponentMode;

    ops::registerCommand({
        .name = "uvUnwrap",
        .doc = "Unwrap the selected faces' islands along their seams (LSCM, refined with SLIM for low stretch). Closed or holed islands "
               "are an error unless openIslands=True cuts them open. Islands keep their place unless pack=True or they had no uvs.",
        .flags = join({uvSetFlag(),
                       {"method", "", T::String, "slim", "slim (low stretch), conformal (LSCM only) or bff (Boundary First Flattening: conformal, boundary lengths kept).",
                        {"slim", "conformal", "bff"}},
                       {"iterations", "", T::Int, 12, "SLIM iterations.", {}, {}, 0.0, 200.0},
                       {"pins", "", T::Bool, true, "Respect pinned uvs (corner attribute uvPin)."},
                       {"pack", "", T::Bool, false, "Pack the unwrapped islands into 0-1."},
                       {"openIslands", "", T::Bool, false, "Cut closed or holed islands open where they cannot unfold, instead of stopping."},
                       finalMeshFlag()},
                      packFlags()),
        .run = [](const Args& a) -> json {
            UnwrapFacesOptions o;
            std::string method = a.get<std::string>("method", "slim");
            o.unwrap.method = method == "conformal" ? UnwrapMethod::Conformal : method == "bff" ? UnwrapMethod::Bff : UnwrapMethod::Slim;
            o.unwrap.iterations = std::clamp(a.get<int>("iterations", 12), 0, 200);
            o.unwrap.usePins = a.get<bool>("pins", true);
            o.pack = a.get<bool>("pack", false);
            o.packing = packArgs(a);
            o.openIslands = a.get<bool>("openIslands", false);
            const bool final = a.get<bool>("finalMesh", true);
            std::string set = uvSetArg(a);
            return editUv("UV Unwrap", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                Index mirrored = 0;
                if (std::optional<geo::SymmetryMap> map = uvSymmetryMap(n, m))
                    mirrored = symmetricSeams(m, *map, f, o.openIslands);
                // On a mesh whose modifiers move its surface, the islands are re-solved on the export before they are packed.
                const bool refine = final && !exportStack(n).empty();
                UnwrapFacesOptions uo = o;
                uo.pack = o.pack && !refine;
                UnwrapFacesResult r;
                withUvSymmetry(m, n, set, UvSymmetry::Absolute, {}, [&]() -> json {
                    r = unwrapFaces(m, f, set, uo);
                    return nullptr;
                });
                if (r.closed)
                    throw CommandError("uvUnwrap: " + std::to_string(r.closed) + (r.closed == 1 ? " island is" : " islands are") +
                                       " closed or holed and cannot unfold; cut seams on " + n.name() + " or unwrap with openIslands=True");
                json out = {{"islands", r.islands}, {"failed", r.failed}, {"seamsAdded", r.seamsAdded + mirrored}};
                if (refine)
                {
                    FinalSolveOptions fo;
                    fo.usePins = o.unwrap.usePins;
                    if (json fm = refineOnExport(m, f, set, n, fo, true); !fm.is_null())
                        out["finalMesh"] = std::move(fm);
                    if (o.pack)
                        packOnExport(m, f, set, n, o.packing);
                }
                return out;
            });
        },
    });

    ops::registerCommand({
        .name = "uvUnwrapExported",
        .doc = "Apply the selected meshes' modifier stacks, then unwrap the result along its seams: the uvs are solved on the exported "
               "surface itself, so no triangle collapses. The modifiers are gone afterwards (undo brings them back).",
        .flags = join({uvSetFlag(),
                       {"method", "", T::String, "slim", "slim (low stretch), conformal (LSCM only) or bff (Boundary First Flattening).",
                        {"slim", "conformal", "bff"}},
                       {"iterations", "", T::Int, 12, "SLIM iterations.", {}, {}, 0.0, 200.0},
                       {"pack", "", T::Bool, false, "Pack the unwrapped islands into 0-1."}},
                      packFlags()),
        .run = [](const Args& a) -> json {
            UnwrapFacesOptions o;
            const std::string method = a.get<std::string>("method", "slim");
            o.unwrap.method = method == "conformal" ? UnwrapMethod::Conformal : method == "bff" ? UnwrapMethod::Bff : UnwrapMethod::Slim;
            o.unwrap.iterations = std::clamp(a.get<int>("iterations", 12), 0, 200);
            o.pack = a.get<bool>("pack", false);
            o.packing = packArgs(a);
            // Seams the export's islands need to unfold are cut rather than refused: this path always ends unwrapped.
            o.openIslands = true;
            const std::string set = uvSetArg(a);
            Graph& g = graph();
            std::vector<NodeId> meshes;
            for (const auto& [node, ids] : targets(K::Face, true))
                meshes.push_back(node);
            if (meshes.empty())
                throw CommandError("uvUnwrapExported: select a mesh first");
            UndoChunk chunk("Unwrap Exported Mesh");
            json out = json::object();
            for (NodeId id : meshes)
            {
                Node& n = *g.node(id);
                const bool applied = !exportStack(n).empty();
                if (applied)
                    ops::execute("bakeModifierStack", {{"args", {n.path()}}});
                g.evaluate();
                const scene::PlugRef base = basePlug(n);
                geo::Mesh m = baseMesh(base);
                if (m.cornerAttrs().get<glm::vec2>(set).empty())
                    throw CommandError("uvUnwrapExported: " + n.name() + " has no " + set);
                const UnwrapFacesResult r = unwrapFaces(m, {}, set, o);
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
                out[n.path()] = {{"modifiersApplied", applied}, {"islands", r.islands}, {"failed", r.failed}, {"seamsAdded", r.seamsAdded}};
            }
            return out.size() == 1 ? out.begin().value() : out;
        },
    });

    ops::registerCommand({
        .name = "uvProject",
        .doc = "Project uvs onto the selected faces: planar, box, cylindrical, spherical or camera (matrix=object to clip, 16 numbers). "
               "Camera uses camera=, else the active view, unless matrix= is given. The frame is fitted to the faces unless given; "
               "returns the frame used.",
        .flags = {uvSetFlag(),
                  {"type", "", T::String, "planar", "planar, box, cylindrical, spherical or camera.", {"planar", "box", "cylindrical", "spherical", "camera"}},
                  {"axis", "", T::String, "best", "x, y, z or best (fitted to the faces).", {"best", "x", "y", "z"}},
                  {"fit", "", T::String, "auto", "Frame orientation when not given: auto (principal axes and normals), world or object axes.",
                   {"auto", "world", "object"}},
                  {"center", "", T::Vec3, nullptr, "Frame centre, object space."},
                  {"rotation", "", T::Vec3, nullptr, "Frame orientation, XYZ euler degrees in object space; Z is the projection axis."},
                  {"width", "", T::Float, 0.0, "Planar and box size along the frame's X; 0 fits the faces.", {}, {}, 0.0},
                  {"height", "", T::Float, 0.0, "Planar and box size along Y, cylinder length along its axis; 0 fits the faces.", {}, {}, 0.0},
                  {"depth", "", T::Float, 0.0, "Box size along Z; 0 fits the faces.", {}, {}, 0.0},
                  {"radius", "", T::Float, 0.0, "Cylinder and sphere radius; 0 fits the faces.", {}, {}, 0.0},
                  {"seamAngle", "", T::Float, nullptr,
                   "Degrees about the axis where U wraps; unset places it where it cuts fewest faces, away from the camera."},
                  {"scaleU", "", T::Float, 1.0, "U scale, applied after the projection."},
                  {"scaleV", "", T::Float, 1.0, "V scale, applied after the projection."},
                  {"offsetU", "", T::Float, 0.0, "U offset, applied after the scale."},
                  {"offsetV", "", T::Float, 0.0, "V offset, applied after the scale."},
                  {"viewDir", "", T::Vec3, nullptr, "World direction the camera looks along, for the seam; the UI passes the active view's."},
                  {"normalize", "", T::Bool, true, "Fit into 0-1 keeping proportions."},
                  {"keepSeams", "", T::Bool, false, "Keep the seams already cut inside the faces; off joins the faces into the projection's islands."},
                  nodeFlag({"camera", "", T::String, nullptr,
                            "Camera node to project through, for type=camera; the active view when not given. UVs span its resolution gate."},
                           "camera"),
                  {"matrix", "", T::List, nullptr, "Object to clip, column major, for type=camera; overrides camera."},
                  {"pack", "", T::Bool, false, "Pack the resulting islands afterwards."}},
        .run = [](const Args& a) -> json {
            ProjectOptions o;
            std::string type = a.get<std::string>("type", "planar");
            if (type == "planar")
                o.type = Projection::Planar;
            else if (type == "box")
                o.type = Projection::Box;
            else if (type == "cylindrical")
                o.type = Projection::Cylindrical;
            else if (type == "spherical")
                o.type = Projection::Spherical;
            else if (type == "camera")
                o.type = Projection::Camera;
            else
                throw CommandError("uvProject: type must be planar, box, cylindrical, spherical or camera");
            std::string axis = a.get<std::string>("axis", "best");
            o.axis = axis == "x" ? ProjectAxis::X : axis == "y" ? ProjectAxis::Y : axis == "z" ? ProjectAxis::Z : ProjectAxis::BestFit;
            std::string fit = a.get<std::string>("fit", "auto");
            if (fit != "auto" && fit != "world" && fit != "object")
                throw CommandError("uvProject: fit must be auto, world or object");
            o.fit = fit == "world" ? ProjectFit::World : fit == "object" ? ProjectFit::Object : ProjectFit::Auto;
            o.normalize = a.get<bool>("normalize", true);
            o.keepSeams = a.get<bool>("keepSeams", false);
            auto vec3 = [&](const char* name) -> std::optional<glm::dvec3> {
                if (!a.has(name))
                    return std::nullopt;
                std::vector<double> v = listArg(a, name, 3, 0.0);
                return glm::dvec3(v[0], v[1], v[2]);
            };
            auto number = [&](const char* name) -> std::optional<double> {
                return a.has(name) ? std::optional<double>(a.get<double>(name)) : std::nullopt;
            };
            o.center = vec3("center");
            o.rotation = vec3("rotation");
            o.width = number("width"), o.height = number("height"), o.depth = number("depth"), o.radius = number("radius");
            o.seamAngle = number("seamAngle");
            o.scale = {a.get<double>("scaleU", 1.0), a.get<double>("scaleV", 1.0)};
            o.offset = {a.get<double>("offsetU", 0.0), a.get<double>("offsetV", 0.0)};
            std::optional<glm::dvec3> view = vec3("viewDir");
            if (!view && projectionViewProvider())
                view = projectionViewProvider()();
            std::optional<glm::dmat4> worldToClip;
            if (o.type == Projection::Camera && a.has("matrix"))
            {
                std::vector<double> mat = listArg(a, "matrix", 16, 0.0);
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        o.camera[c][r] = mat[size_t(c * 4 + r)];
            }
            else if (o.type == Projection::Camera)
                worldToClip = cameraWorldToClip(a.get<std::string>("camera", ""));
            bool pack = a.get<bool>("pack", false);
            std::string set = uvSetArg(a);
            std::vector<ProjectionRecord> records;
            json out = editUv("UV Project", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                ProjectOptions mo = o;
                glm::dmat4 world = n.as<glm::dmat4>(scene::Transform::kWorldMatrix);
                mo.worldLinear = glm::dmat3(world);
                if (worldToClip)
                    mo.camera = *worldToClip * world;
                if (view)
                    mo.viewDir = glm::inverse(mo.worldLinear) * *view;
                std::vector<Index> half;
                if (mo.type == Projection::Planar)
                    if (std::optional<geo::SymmetryMap> map = uvSymmetryMap(n, m))
                        half = oneSide(m, *map, f);
                ProjectFrame fr;
                bool projected = false;
                if (half.empty())
                    projected = project(m, f, set, mo, &fr);
                else
                    withUvSymmetry(m, n, set, UvSymmetry::Absolute, {}, [&]() -> json {
                        projected = project(m, half, set, mo, &fr);
                        return nullptr;
                    });
                if (!projected)
                    throw CommandError("uvProject: nothing to project");
                if (pack)
                    packIslands(m, f, set, {});
                records.push_back({n.id(), o.type, fr, world, o.scale, o.offset});
                if (o.type == Projection::Camera)
                    return nullptr;
                glm::dvec3 rot = frameEuler(fr.axes);
                json r = {{"type", type}, {"center", {fr.center.x, fr.center.y, fr.center.z}}, {"rotation", {rot.x, rot.y, rot.z}}};
                if (o.type == Projection::Planar || o.type == Projection::Box)
                    r["width"] = fr.width, r["height"] = fr.height;
                if (o.type == Projection::Box)
                    r["depth"] = fr.depth;
                if (o.type == Projection::Cylindrical)
                    r["height"] = fr.height;
                if (o.type == Projection::Cylindrical || o.type == Projection::Spherical)
                    r["radius"] = fr.radius, r["seamAngle"] = glm::degrees(fr.seamAngle);
                return r;
            });
            auto action = std::make_unique<ProjectionsAction>();
            action->before = lastProjectionsMutable();
            action->after = records;
            UndoStack::get().record(std::move(action));
            lastProjectionsMutable() = std::move(records);
            return out;
        },
        .adjustable = true,
    });

    ops::registerCommand({
        .name = "uvAutoSeams",
        .doc = "Place seams for game assets on the selected faces: hard edges, then cuts that open each island along hidden, creased paths and "
               "split it where it would stretch.",
        .flags = seamFlagDefs(),
        .run = [](const Args& a) -> json {
            AutoSeamOptions o = seamArgs(a);
            return editUv("UV Auto Seams", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                std::optional<geo::SymmetryMap> map = uvSymmetryMap(n, m);
                AutoSeamResult r = autoSeams(m, f, seamsIn(o, n, m, map));
                if (!map)
                    return {{"islands", r.charts.size()}, {"seams", r.seamEdges}};
                // The halves' choices can still differ where costs tie; the union is symmetric.
                const Index added = symmetricSeams(m, *map, f);
                ChartOptions co;
                return {{"islands", islandFaces(m, f, co).size()}, {"seams", r.seamEdges + added}};
            });
        },
    });

    ops::registerCommand({
        .name = "uvAuto",
        .doc = "Automatic uvs: seams, low-stretch unwrap and pack, in one step.",
        .flags = join({uvSetFlag(), {"iterations", "", T::Int, 12, "SLIM iterations.", {}, {}, 0.0, 200.0}, finalMeshFlag()}, seamFlagDefs(),
                      packFlags()),
        .run = [](const Args& a) -> json {
            const bool final = a.get<bool>("finalMesh", true);
            AutoUvOptions o;
            o.seams = seamArgs(a);
            o.unwrap.iterations = std::clamp(a.get<int>("iterations", 12), 0, 200);
            o.pack = packArgs(a);
            std::string set = uvSetArg(a);
            return editUv("Auto UV", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                std::optional<geo::SymmetryMap> map = uvSymmetryMap(n, m);
                AutoUvOptions mo = o;
                mo.seams = seamsIn(o.seams, n, m, map);
                if (map)
                    mo.afterSeams = [&](geo::Mesh& mesh, std::span<const Index> faces) { return symmetricSeams(mesh, *map, faces); };
                AutoUvResult r = autoUv(m, f, set, mo);
                json j = packJson(r.pack);
                j["islands"] = r.islands;
                j["seams"] = r.seams;
                if (json fm = finishOnExport(m, f, set, n, final, &mo.pack); !fm.is_null())
                    j["finalMesh"] = std::move(fm);
                return j;
            });
        },
    });

    ops::registerCommand({
        .name = "uvHose",
        .doc = "Uvs for pipes, hoses, cables and rings: each tube is cut once along its least seen side and straightened into a strip, "
               "runs longer than maxAspect are cut across where least seen, and ends and rims are projected flat. Faces that are neither "
               "get auto seams. Works from the quad topology, so any swept tube qualifies.",
        .flags = join({uvSetFlag(),
                       {"maxAspect", "", T::Float, 8.0, "Longest strip as a multiple of its circumference; 0 never cuts across.", {}, {}, 0.0, 64.0},
                       {"maxLength", "", T::Float, 0.0, "Longest strip in cm; replaces maxAspect when set.", {}, {}, 0.0},
                       {"visibility", "", T::Bool, true, "Hide cuts where they are least visible: occluded, concave and downward facing."},
                       {"keepSeams", "", T::Bool, false, "Keep the seams already there."},
                       {"useViews", "", T::Bool, true, "Keep seams out of sight of the views."},
                       viewsFlag(),
                       {"pack", "", T::Bool, true, "Pack the islands into 0-1 afterwards."},
                       finalMeshFlag()},
                      packFlags()),
        .run = [](const Args& a) -> json {
            const bool final = a.get<bool>("finalMesh", true);
            HoseOptions o;
            o.maxAspect = std::max(0.0, a.get<double>("maxAspect", 8.0));
            o.maxLength = std::max(0.0, a.get<double>("maxLength", 0.0));
            o.visibility = a.get<bool>("visibility", true);
            o.keepSeams = a.get<bool>("keepSeams", false);
            o.views = seamViewArgs(a);
            const bool pack = a.get<bool>("pack", true);
            const PackOptions packing = packArgs(a);
            std::string set = uvSetArg(a);
            return editUv("UV Hose", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                HoseOptions mo = o;
                mo.views = viewsIn(o.views, n);
                HoseResult r = hoseUv(m, f, set, mo);
                json j = {{"strips", r.strips}, {"lengthCuts", r.lengthCuts}, {"caps", r.caps}, {"otherFaces", r.otherFaces},
                          {"unstraightened", r.unstraightened}};
                if (json fm = refineOnExport(m, f, set, n, {}, final); !fm.is_null())
                    j["finalMesh"] = std::move(fm);
                if (pack)
                {
                    PackTarget t;
                    t.mesh = &m;
                    t.faces.assign(f.begin(), f.end());
                    if (final)
                        t.stack = exportStack(n);
                    j["pack"] = packJson(packMeshes(std::span<PackTarget>(&t, 1), set, packing));
                }
                return j;
            });
        },
    });

    ops::registerCommand({
        .name = "uvSeamView",
        .doc = "Create a seam view: a locator that uvAuto and uvAutoSeams keep seams out of sight of, such as where a first person "
               "camera sits or where a character's face is seen from. Move it and run Auto UV again to see the seams move. Its "
               "seamView attribute is the weight: higher hides seams harder, negative draws them towards it. Returns its path.",
        .flags = {{"position", "p", T::Vec3, nullptr, "World position."},
                  {"weight", "w", T::Float, 1.0, "How hard seams avoid what it sees; negative draws them towards it.", {}, {}, -1.0, 4.0},
                  {"name", "n", T::String, "seamView", "Name; made unique among its siblings."}},
        .run = [](const Args& a) -> json {
            json call = {{"name", a.get<std::string>("name", "seamView")}};
            if (a.has("position"))
                call["position"] = a.flags["position"];
            const std::string path = ops::execute("spaceLocator", call).get<std::string>();
            ops::execute("addAttr", {{"args", {path}},
                                     {"longName", kSeamViewAttr},
                                     {"attributeType", "double"},
                                     {"defaultValue", 1.0},
                                     {"minValue", -1.0},
                                     {"maxValue", 4.0},
                                     {"keyable", true},
                                     {"doc", "How hard Auto UV keeps seams out of this view's sight; negative draws them towards it."}});
            graph().setValue(graph().plug(path + "." + kSeamViewAttr), std::clamp(a.get<double>("weight", 1.0), -1.0, 4.0));
            return path;
        },
    });

    ops::registerCommand({
        .name = "uvCutEdges",
        .doc = "Mark the selected edges as uv seams.",
        .run = [](const Args&) -> json {
            return editUv("UV Cut", K::Edge, [](geo::Mesh& m, const Ids& sel, Node& node) -> json {
                std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
                const std::vector<Index> e = withMirrorEdges(node, m, sel);
                size_t n = 0;
                for (Index i : e)
                    if (m.edgeValid(i))
                    {
                        seam[i] = true;
                        ++n;
                    }
                return {{"cut", n}};
            }, false);
        },
    });

    ops::registerCommand({
        .name = "uvSewEdges",
        .doc = "Unmark the selected seams and weld their uvs, moving the smaller island over to meet the other.",
        .flags = {uvSetFlag()},
        .run = [](const Args& a) -> json {
            std::string set = uvSetArg(a);
            return editUv("UV Sew", K::Edge, [&](geo::Mesh& m, const Ids& e, Node& n) { return sewEdges(m, withMirrorEdges(n, m, e), set); }, false);
        },
    });

    ops::registerCommand({
        .name = "uvDelete",
        .doc = "Delete the uvs of the selected faces, leaving the faces: their uvs collapse to 0,0, so unwrap and projections treat them as "
               "having none. With whole meshes selected, removes the uv set.",
        .flags = {uvSetFlag()},
        .run = [](const Args& a) -> json {
            std::string set = uvSetArg(a);
            return editUv("UV Delete", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                if (f.empty())
                    return {{"removed", m.cornerAttrs().remove(set)}};
                std::span<glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(set);
                if (uv.empty())
                    return {{"faces", 0}};
                std::span<bool> pin = m.cornerAttrs().get<bool>("uvPin");
                std::optional<geo::SymmetryMap> map = uvSymmetryMap(n, m);
                const std::vector<Index> faces = map ? map->expandFaces(m, f) : std::vector<Index>(f.begin(), f.end());
                for (Index face : faces)
                    if (m.faceValid(face))
                        m.forEachFaceCorner(face, [&](Index c) {
                            uv[c] = glm::vec2(0.0f);
                            if (!pin.empty())
                                pin[c] = false;
                        });
                return {{"faces", faces.size()}};
            });
        },
    });

    ops::registerCommand({
        .name = "uvPack",
        .doc = "Pack the uv islands of the selected faces (or whole meshes) into 0-1 with padding for a texture size. Every selected "
               "mesh goes into one shared layout, or each into its own with perObject. Islands are packed by their real shapes, into the holes of other islands, with "
               "rotations, optional mirroring and stacking of identical islands; the densest of several island orders wins. Returns "
               "the density reached (percent of the used tiles covered) and the texel density. Runs on worker threads and can be "
               "cancelled from the progress bar.",
        .flags = join({uvSetFlag(), finalMeshFlag()}, packFlags(), packLayoutFlags()),
        .run = [](const Args& a) -> json {
            PackOptions o = packArgs(a);
            std::string set = uvSetArg(a), keep = a.get<std::string>("keep", "none");
            const bool final = a.get<bool>("finalMesh", true);
            std::map<NodeId, Ids> work = targets(K::Face, true);
            if (work.empty())
                throw CommandError("uvPack: select a mesh or some components first");
            Graph& g = graph();
            UndoChunk chunk("UV Pack");
            std::vector<NodeId> nodes;
            std::vector<scene::PlugRef> bases;
            std::vector<geo::Mesh> meshes;
            meshes.reserve(work.size());
            for (auto& [node, ids] : work)
            {
                bases.push_back(basePlug(*g.node(node)));
                meshes.push_back(baseMesh(bases.back()));
                nodes.push_back(node);
            }
            std::vector<PackTarget> packTargets(nodes.size());
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                PackTarget& t = packTargets[i];
                const Ids& ids = work[nodes[i]];
                t.mesh = &meshes[i];
                t.linear = worldLinear(*g.node(nodes[i]));
                if (final)
                    t.stack = exportStack(*g.node(nodes[i]));
                if (keep == "selected")
                    t.fixedFaces.assign(ids.begin(), ids.end());
                else
                {
                    t.faces.assign(ids.begin(), ids.end());
                    t.othersFixed = keep == "unselected" && !ids.empty();
                }
            }
            auto pack = [&](std::span<PackTarget> t) {
                PackResult r = packMeshes(t, set, o);
                for (const std::string& w : r.warnings)
                    log::warn("uvPack: {}", w);
                if (r.cancelled)
                    throw CommandError("uvPack: cancelled");
                if (!r.ok)
                    throw CommandError("uvPack: the islands do not fit; lower the padding, add tiles or lower texelsPerUnit");
                return r;
            };
            json result;
            if (a.get<bool>("perObject", false))
            {
                // The least dense layout stands for the batch; each mesh's own result is under "meshes".
                json each = json::array();
                PackResult worst;
                worst.coverage = 1.0;
                worst.ok = true;
                double seconds = 0.0;
                for (size_t i = 0; i < packTargets.size(); ++i)
                {
                    PackResult r = pack({&packTargets[i], 1});
                    each.push_back(packJson(r));
                    each.back()["mesh"] = g.node(nodes[i])->path();
                    seconds += r.seconds;
                    if (r.coverage <= worst.coverage)
                        worst = r;
                }
                worst.seconds = seconds;
                result = packJson(worst);
                result["meshes"] = std::move(each);
            }
            else
                result = packJson(pack(packTargets));
            for (size_t i = 0; i < nodes.size(); ++i)
                g.setValue(bases[i], MeshPtr(std::make_shared<geo::Mesh>(std::move(meshes[i]))));
            return result;
        },
    });

    ops::registerCommand({
        .name = "uvStacks",
        .doc = "Stacked islands of the selected meshes: islands lying on one another with the same shape, as uvPack stack=True "
               "leaves them. Returns per mesh a list of stacks, each a list of islands as face ids. select=True selects their faces.",
        .flags = {uvSetFlag(),
                  {"tolerance", "", T::Float, 0.002, "Largest vertex mismatch, as a fraction of the island's size.", {}, {}, 0.0},
                  {"select", "", T::Bool, false, "Select the faces of every stacked island."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string set = uvSetArg(a);
            double tol = std::max(0.0, a.get<double>("tolerance", 0.002));
            std::map<NodeId, Ids> picked;
            json r = queryUv([&](const geo::Mesh& m, const Ids&, Node& n) -> json {
                json stacks = json::array();
                for (const auto& stack : stackedIslands(m, set, tol))
                {
                    json s = json::array();
                    for (const auto& faces : stack)
                    {
                        s.push_back(faces);
                        picked[n.id()].insert(picked[n.id()].end(), faces.begin(), faces.end());
                    }
                    stacks.push_back(std::move(s));
                }
                return stacks;
            });
            if (a.get<bool>("select", false))
            {
                for (auto& [node, ids] : picked)
                    std::sort(ids.begin(), ids.end());
                scene::components::setMode(ComponentMode::Face);
                scene::components::setAll({}, {}, std::move(picked));
            }
            return r;
        },
    });

    ops::registerCommand({
        .name = "uvStraighten",
        .doc = "Rectify the selected islands: quad grids become straight rows and columns, other islands are fitted to a rectangle.",
        .flags = {uvSetFlag()},
        .run = [](const Args& a) -> json {
            std::string set = uvSetArg(a);
            return editUv("UV Straighten", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                return withUvSymmetry(m, n, set, UvSymmetry::Absolute, {}, [&]() -> json {
                    ChartOptions co;
                    co.source = IslandSource::Uvs;
                    co.uvSet = set;
                    std::vector<Chart> charts = buildCharts(m, f, co);
                    size_t grid = 0, rect = 0, failed = 0;
                    for (Chart& ch : charts)
                    {
                        StraightenResult r = straightenChart(ch);
                        grid += r == StraightenResult::Grid;
                        rect += r == StraightenResult::Rectangle;
                        failed += r == StraightenResult::Failed;
                    }
                    writeCharts(m, charts, set);
                    return {{"grid", grid}, {"rectangle", rect}, {"failed", failed}};
                });
            });
        },
    });

    ops::registerCommand({
        .name = "uvOrient",
        .doc = "Rotate the selected islands upright: by bounding box, principal axis or longest edge.",
        .flags = {uvSetFlag(), {"mode", "", T::String, "bbox", "bbox, principal or edge.", {"bbox", "principal", "edge"}}},
        .run = [](const Args& a) -> json {
            std::string mode = a.get<std::string>("mode", "bbox");
            OrientMode om = mode == "principal" ? OrientMode::Principal : mode == "edge" ? OrientMode::LongestEdge : OrientMode::BoundingBox;
            std::string set = uvSetArg(a);
            return editUv("UV Orient", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                return withUvSymmetry(m, n, set, UvSymmetry::Absolute, {}, [&]() -> json {
                    ChartOptions co;
                    co.source = IslandSource::Uvs;
                    co.uvSet = set;
                    std::vector<Chart> charts = buildCharts(m, islandsOfFaces(m, f, co), co);
                    for (Chart& ch : charts)
                        orientChart(ch, om);
                    writeCharts(m, charts, set);
                    return {{"islands", charts.size()}};
                });
            });
        },
    });

    ops::registerCommand({
        .name = "uvTexelDensity",
        .doc = "Texel density of the selection in px/cm for a texture size. With density=, scales each island to it (world space). "
               "Faces mapped onto a trim sheet take their density from the sheet: trims='skip' (the default) leaves their islands "
               "on their trims and reports how many were skipped, 'rescale' scales them too and clears their trim mapping.",
        .flags = {uvSetFlag(),
                  presetFlag({"resolution", "", T::Int, 1024, "Texture size, px.", {}, {}, 1.0}, kTextureSizePresets),
                  {"density", "", T::Float, nullptr, "Set to this many px/cm.", {}, {}, 0.0},
                  {"worldSpace", "", T::Bool, true, "Measure world size, with the mesh's scale."},
                  {"trims", "", T::String, "skip", "Trim-mapped faces: skip keeps them on their trims, rescale scales them off.", {"skip", "rescale"}},
                  finalMeshFlag()},
        .run = [](const Args& a) -> json {
            double res = double(std::max(1, a.get<int>("resolution", 1024)));
            bool world = a.get<bool>("worldSpace", true);
            const bool final = a.get<bool>("finalMesh", true);
            std::string set = uvSetArg(a);
            if (!a.has("density"))
                return queryUv([&](const geo::Mesh& m, const Ids& f, Node& n) -> json {
                    const FinalMap map = exportTraceOf(m, n, final);
                    return texelDensity(m, set, f, res, world ? worldLinear(n) : glm::dmat3(1.0), &map);
                });
            double density = a.get<double>("density");
            const bool rescaleTrims = a.get<std::string>("trims", "skip") == "rescale";
            size_t skippedAll = 0, editedAll = 0;
            json out = editUv("Set Texel Density", K::Face, [&](geo::Mesh& m, const Ids& f, Node& n) -> json {
                glm::dmat3 lin = world ? worldLinear(n) : glm::dmat3(1.0);
                std::vector<Index> faces = liveFaces(m, f);
                std::span<int32_t> trim = m.faceAttrs().get<int32_t>(kTrimFaceAttr);
                const size_t before = faces.size();
                if (rescaleTrims)
                {
                    for (Index fc : faces)
                        if (trimMapped(trim, fc))
                            trim[fc] = 0;
                }
                else
                    std::erase_if(faces, [&](Index fc) { return trimMapped(trim, fc); });
                const size_t skipped = before - faces.size();
                skippedAll += skipped;
                editedAll += faces.size();
                const FinalMap map = exportTraceOf(m, n, final);
                if (!faces.empty())
                    withUvSymmetry(m, n, set, UvSymmetry::Absolute, {}, [&]() -> json {
                        setTexelDensity(m, set, faces, res, density, lin, &map);
                        return nullptr;
                    });
                const FinalMap after = map.exported ? exportTraceOf(m, n, final) : FinalMap{};
                json r = faces.empty() ? json(nullptr) : json(texelDensity(m, set, faces, res, lin, &after));
                return skipped ? json{{"density", r}, {"skippedTrimFaces", skipped}} : r;
            });
            if (editedAll == 0 && skippedAll > 0)
                throw CommandError("uvTexelDensity: these faces are mapped onto a trim sheet, whose texel density is the sheet's pxPerM; "
                                   "change the sheet (trimSheetEdit(values={'pxPerM': ...})) or pass trims='rescale' to take them off it");
            return out;
        },
    });

    ops::registerCommand({
        .name = "uvLightmap",
        .doc = "Generate Unreal lightmap uvs: the source set's islands repacked without overlaps, padded for the lightmap resolution.",
        .flags = {{"uvSet", "", T::String, "uv1", "Lightmap set to write."},
                  {"source", "", T::String, "uv0", "Set whose islands to reuse (auto seams when missing)."},
                  presetFlag({"resolution", "", T::Int, 64, "Lightmap resolution in Unreal.", {}, {}, 4.0}, std::vector<double>{16, 32, 64, 128, 256, 512, 1024}),
                  {"padding", "", T::Float, 2.0, "Texels between charts, held exactly at the lightmap resolution.", {}, {}, 0.0},
                  {"effort", "", T::Int, 2, "Packing search effort 0-3; higher packs denser, slower.", {}, {}, 0.0, 3.0},
                  {"rotateStep", "", T::Int, 90, "Rotation step for the charts, degrees (0 keeps their orientation).", {}, {}, 0.0, 180.0}},
        .run = [](const Args& a) -> json {
            std::string target = a.get<std::string>("uvSet", "uv1"), source = a.get<std::string>("source", "uv0");
            LightmapOptions o;
            o.resolution = std::max(4, a.get<int>("resolution", 64));
            o.padding = std::max(0.0, a.get<double>("padding", 2.0));
            o.effort = std::clamp(a.get<int>("effort", 2), 0, 3);
            o.rotateStep = std::clamp(a.get<int>("rotateStep", 90), 0, 180);
            o.source = source;
            o.target = target;
            return editUv("Lightmap UVs", K::Face, [&](geo::Mesh& m, const Ids&, Node&) -> json {
                LightmapResult r = generateLightmapUvs(m, o);
                if (!r.ok)
                    throw CommandError("uvLightmap: the charts do not fit at this resolution; raise it or lower the padding");
                json j = packJson(r.pack);
                j["islands"] = r.islands;
                j["rewrapped"] = r.rewrapped;
                return j;
            });
        },
    });

    ops::registerCommand({
        .name = "uvLayout",
        .doc = "UV report for the selection: islands, overlapping and folded faces, distortion and texel density. Faces mapped onto a "
               "trim sheet share texels on purpose: their overlaps count in trimOverlapFaces, not overlappingFaces (trimCheck checks "
               "them against their sheet).",
        .flags = {uvSetFlag(), presetFlag({"resolution", "", T::Int, 1024, "Texture size for the density, px.", {}, {}, 1.0}, kTextureSizePresets),
                  finalMeshFlag()},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string set = uvSetArg(a);
            double res = double(std::max(1, a.get<int>("resolution", 1024)));
            const bool final = a.get<bool>("finalMesh", true);
            return queryUv([&](const geo::Mesh& m, const Ids& f, Node& n) -> json {
                if (m.cornerAttrs().get<glm::vec2>(set).empty())
                    return {{"uvSet", set}, {"exists", false}};
                const FinalMap map = exportTraceOf(m, n, final);
                ChartOptions co;
                co.source = IslandSource::Uvs;
                co.uvSet = set;
                std::vector<uint8_t> flags;
                overlappingFaces(m, set, flags, f);
                std::span<const int32_t> trim = m.faceAttrs().get<int32_t>(kTrimFaceAttr);
                size_t overlapping = 0, trimOverlap = 0;
                for (Index fc = 0; fc < flags.size(); ++fc)
                    if (flags[fc])
                        ++(trimMapped(trim, fc) ? trimOverlap : overlapping);
                std::vector<FaceDistortion> d = faceDistortion(m, set, f, &map);
                DistortionSummary s = summarize(m, d, f);
                std::vector<std::string> sets;
                for (const std::string& name : m.cornerAttrs().names())
                    if (const geo::Attribute* at = m.cornerAttrs().find(name); at && at->type() == geo::AttrType::Float2)
                        sets.push_back(name);
                return {{"uvSet", set},
                        {"uvSets", sets},
                        {"islands", islandFaces(m, f, co).size()},
                        {"overlappingFaces", overlapping},
                        {"trimOverlapFaces", trimOverlap},
                        {"foldedFaces", s.folded},
                        {"areaDistortion", {{"mean", s.meanArea}, {"max", s.maxArea}}},
                        {"angleDistortion", {{"mean", s.meanAngle}, {"max", s.maxAngle}}},
                        {"texelDensity", texelDensity(m, set, f, res, worldLinear(n), &map)},
                        {"measuredOn", map.exported ? "exported" : "cage"}};
            });
        },
    });

    ops::registerCommand({
        .name = "uvSetCurrent",
        .doc = "The uv set the uv tools and editor work on. With no argument, returns it.",
        .positionalDoc = "[uvSet]",
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.positional.empty())
                setCurrentUvSet(a.positional[0].get<std::string>());
            return currentUvSet();
        },
    });

    ops::registerCommand({
        .name = "polyUVSet",
        .doc = "Delete, copy or reorder a mesh's uv sets, or query them. Meshes without uvSet are skipped. Sets stay numbered uv0, uv1, ... with no gaps: deleting "
               "or moving one renumbers the sets after it. Returns {sets} for the last mesh, and for copy {created}: each mesh's new set, which differs between meshes with different set counts.",
        .positionalDoc = "meshes",
        .flags = {{"delete", "", T::Bool, false, "Remove uvSet."},
                  {"copy", "", T::Bool, false, "Copy uvSet into newUvSet (made or overwritten); a new set at the end when empty."},
                  {"query", "", T::Bool, false, "Only report the sets."},
                  uvSetFlag(),
                  {"newUvSet", "", T::String, "", "copy: the set to write."},
                  {"moveTo", "", T::Int, -1, "Move uvSet to this position (0 is the set textures and exporters use first).", {}, {}, -1.0}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json sets = json::array();
            int found = 0;
            json created = json::object();
            UndoChunk chunk("UV Set");
            for (const std::string& name : scene::meshObjects(a, "polyUVSet"))
            {
                Node* n = g.find(name);
                if (!n || !n->type().isA("mesh"))
                    throw CommandError("polyUVSet: " + name + " is not a mesh");
                g.evaluate();
                if (a.get<bool>("query"))
                {
                    MeshPtr m = scene::modifiers::cage(g, n->id());
                    sets = m ? json(uvSetNames(*m)) : json::array();
                    continue;
                }
                scene::PlugRef base = basePlug(*n);
                g.evaluate();
                MeshPtr cur = std::get<MeshPtr>(g.storedValue(base));
                if (!cur)
                    continue;
                auto m = std::make_shared<geo::Mesh>(*cur);
                const std::string set = uvSetArg(a);
                std::vector<std::string> names = uvSetNames(*m);
                auto at = std::find(names.begin(), names.end(), set);
                if (at == names.end())
                    continue;
                ++found;
                int index = int(at - names.begin());
                std::string to;
                if (a.get<bool>("copy"))
                {
                    to = a.get<std::string>("newUvSet", "");
                    if (to.empty())
                        to = geo::attr::uvName(int(names.size()));
                    if (to != set)
                    {
                        auto from = m->cornerAttrs().get<glm::vec2>(set);
                        std::vector<glm::vec2> copy(from.begin(), from.end());
                        std::ranges::copy(copy, m->cornerAttrs().getOrAdd<glm::vec2>(to).begin());
                    }
                    names = uvSetNames(*m);
                }
                if (a.get<bool>("delete"))
                {
                    m->cornerAttrs().remove(set);
                    names.erase(names.begin() + index);
                    index = -1;
                }
                if (int pos = a.get<int>("moveTo", -1); pos >= 0 && index >= 0)
                {
                    pos = std::min(pos, int(names.size()) - 1);
                    std::string moved = names[size_t(index)];
                    names.erase(names.begin() + index);
                    names.insert(names.begin() + pos, moved);
                }
                renumberUvSets(*m, names);
                if (auto made = std::ranges::find(names, to); !to.empty() && made != names.end())
                    created[name] = geo::attr::uvName(int(made - names.begin()));
                sets = uvSetNames(*m);
                g.setValue(base, MeshPtr(std::move(m)));
            }
            if (!a.get<bool>("query") && found == 0)
                throw CommandError("polyUVSet: no given mesh has uv set " + uvSetArg(a));
            if (!a.get<bool>("query") && std::ranges::find(sets, json(currentUvSet())) == sets.end())
                setCurrentUvSet(geo::attr::uvName(0));
            return a.get<bool>("copy") ? json{{"sets", sets}, {"created", created}} : json{{"sets", sets}};
        },
    });

    ops::registerCommand({
        .name = "uvTransform",
        .doc = "Move, rotate (degrees) and scale the uvs of the selected components about a pivot (default: their centre).",
        .flags = {uvSetFlag(),
                  {"translate", "", T::List, nullptr, "[u, v] offset."},
                  {"rotate", "", T::Float, 0.0, "Degrees, counter-clockwise."},
                  {"scale", "", T::List, nullptr, "[su, sv], or one number."},
                  {"pivot", "", T::List, nullptr, "[u, v]."},
                  {"perIsland", "", T::Bool, false, "Transform each island touched by the selection, whole, about its own centre."}},
        .run = [](const Args& a) -> json {
            std::vector<double> t = listArg(a, "translate", 2, 0.0), sc = listArg(a, "scale", 2, 1.0);
            const bool perIsland = a.get<bool>("perIsland") && !a.has("pivot");
            double rot = glm::radians(a.get<double>("rotate", 0.0));
            std::string set = uvSetArg(a);
            K kind = scene::components::mode() == K::Object ? K::Face : scene::components::mode();
            std::optional<glm::dvec2> pivot;
            if (a.has("pivot"))
            {
                std::vector<double> p = listArg(a, "pivot", 2, 0.0);
                pivot = glm::dvec2(p[0], p[1]);
            }
            return editUv("UV Transform", kind, [&](geo::Mesh& m, const Ids& ids, Node& n) -> json {
                if (m.cornerAttrs().get<glm::vec2>(set).empty())
                    return nullptr;
                std::optional<UvMirror> mirror;
                if (std::optional<geo::SymmetryMap> map = uvSymmetryMap(n, m))
                    mirror = buildUvMirror(m, *map, set);
                std::vector<std::vector<Index>> groups;
                if (perIsland)
                {
                    ChartOptions co;
                    co.source = IslandSource::Uvs;
                    co.uvSet = set;
                    Ids faces = ids.empty() || kind == K::Face ? ids : scene::components::convert(m, kind, ids, K::Face);
                    std::vector<Index> whole = faces.empty() ? std::vector<Index>{} : islandsOfFaces(m, faces, co);
                    for (const std::vector<Index>& island : islandFaces(m, whole, co))
                        groups.push_back(uvCornersOfFaces(m, set, island));
                }
                else
                    groups.push_back(selectionCorners(m, n.id(), kind, ids, set));
                std::vector<Index> all;
                for (const std::vector<Index>& corners : groups)
                    all.insert(all.end(), corners.begin(), corners.end());
                // With symmetry the partner side follows mirrored, so the pivot is the driving side's.
                const int drive = mirror ? uvMirrorDrive(*mirror, all) : 0;
                const double c = std::cos(rot), s = std::sin(rot);
                size_t moved = 0;
                withUvSymmetry(m, n, set, UvSymmetry::Delta, all, [&]() -> json {
                    std::span<glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(set);
                    for (const std::vector<Index>& corners : groups)
                    {
                        if (corners.empty())
                            continue;
                        glm::dvec2 piv(0.0);
                        if (pivot)
                            piv = *pivot;
                        else
                        {
                            glm::dvec2 lo(1e30), hi(-1e30);
                            for (int pass = 0; pass < 2 && lo.x > hi.x; ++pass)
                                for (Index k : corners)
                                    if (pass == 1 || !mirror || mirror->side[k] != -drive)
                                    {
                                        lo = glm::min(lo, glm::dvec2(uv[k]));
                                        hi = glm::max(hi, glm::dvec2(uv[k]));
                                    }
                            piv = (lo + hi) * 0.5;
                        }
                        for (Index k : corners)
                        {
                            glm::dvec2 d = (glm::dvec2(uv[k]) - piv) * glm::dvec2(sc[0], sc[1]);
                            uv[k] = glm::vec2(piv + glm::dvec2(d.x * c - d.y * s, d.x * s + d.y * c) + glm::dvec2(t[0], t[1]));
                        }
                        moved += corners.size();
                    }
                    return nullptr;
                });
                if (moved == 0)
                    return nullptr;
                return {{"corners", moved}};
            });
        },
    });

    for (const char* cmd : {"uvUnwrap", "uvOptimize", "uvLiveRelax", "uvPack", "uvAuto", "uvHose", "uvBff", "uvMosaic", "uvTexelDensity", "uvLayout"})
    {
        ops::options::Override o;
        if (const ops::options::Override* had = ops::options::findOverride(cmd))
            o = *had;
        o.labels["finalMesh"] = "After Modifiers";
        ops::options::setOverride(cmd, std::move(o));
    }
}
} // namespace lila::uv
