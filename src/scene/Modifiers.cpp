#include "scene/Modifiers.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/ArrayMesh.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Reduce.h"
#include "geo/Topology.h"
#include "scene/AttrBuilders.h"
#include "scene/AutoBevel.h"
#include "scene/Components.h"
#include "scene/ExtrudeModifier.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/MeshNodes.h"
#include "scene/SplitRingsModifier.h"
#include "scene/Transform.h"

#include <map>
#include <set>

namespace lila::scene
{
using namespace attrdefs;

/** TransferAttributes.cpp. */
void registerTransferModifier();
void registerBooleanModifier();

namespace
{
double numOf(const Node& n, const char* a) { return n.as<double>(n.type().attrIndex(a)); }
int64_t intOf(const Node& n, const char* a) { return n.as<int64_t>(n.type().attrIndex(a)); }
bool flagOf(const Node& n, const char* a) { return n.as<bool>(n.type().attrIndex(a)); }
glm::dvec3 vectorOf(const Node& n, const char* a) { return n.as<glm::dvec3>(n.type().attrIndex(a)); }

/** arrayModifier's center enum. */
enum ArrayCenter : int64_t
{
    kCenterOrigin,
    kCenterPoint,
    kCenterObject,
};

/** The array layout and weld settings an arrayModifier's plugs describe. */
geo::ArrayOptions arrayOptions(const Node& n)
{
    geo::ArrayOptions o;
    geo::ArrayLayout& l = o.layout;
    l.mode = geo::ArrayMode(std::clamp<int64_t>(intOf(n, "mode"), 0, 2));
    l.count = int(std::clamp<int64_t>(intOf(n, "count"), 1, 1000));
    l.gridCount = {l.count, int(std::clamp<int64_t>(intOf(n, "countY"), 1, 1000)), int(std::clamp<int64_t>(intOf(n, "countZ"), 1, 1000))};
    // Grids stay under the same total as rows so a stray count cannot hang the evaluation.
    while (int64_t(l.gridCount.x) * l.gridCount.y * l.gridCount.z > 100000)
        l.gridCount = glm::max(l.gridCount / 2, glm::ivec3(1));
    l.offset = l.mode == geo::ArrayMode::Grid ? vectorOf(n, "spacing") : l.mode == geo::ArrayMode::Linear ? vectorOf(n, "offset") : glm::dvec3(0.0);
    l.rotate = vectorOf(n, "rotate");
    l.scale = vectorOf(n, "scale");
    l.axis = int(std::clamp<int64_t>(intOf(n, "axis"), 0, 2));
    l.angle = numOf(n, "angle");
    l.stepAngle = intOf(n, "angleMode") == 1;
    l.rise = numOf(n, "rise");
    switch (intOf(n, "center"))
    {
    case kCenterPoint: l.frame = glm::translate(glm::dmat4(1.0), vectorOf(n, "centerPoint")); break;
    case kCenterObject:
    {
        // The object's frame relative to this mesh; scale is dropped so it orients and places the centre only.
        glm::dmat4 rel = glm::inverse(n.as<glm::dmat4>(n.type().attrIndex("targetMatrix"))) * n.as<glm::dmat4>(n.type().attrIndex("centerMatrix"));
        for (int c = 0; c < 3; ++c)
            if (double len = glm::length(glm::dvec3(rel[c])); len > 1e-12)
                rel[c] /= len;
        l.frame = rel;
        break;
    }
    default: break;
    }
    o.weld = flagOf(n, "weld");
    o.mergeEnds = flagOf(n, "mergeEnds");
    o.tolerance = std::max(numOf(n, "tolerance"), 0.0);
    return o;
}

std::vector<geo::Index> allFaces(const geo::Mesh& m)
{
    std::vector<geo::Index> out;
    for (geo::Index fi = 0; fi < m.faceCapacity(); ++fi)
        if (m.faceValid(fi))
            out.push_back(fi);
    return out;
}

/** Edges whose faces meet sharper than the angle, or every hard edge when the angle is negative. */
std::vector<geo::Index> sharpEdges(const geo::Mesh& m, double degrees)
{
    std::vector<geo::Index> out;
    auto hard = m.edgeAttrs().get<bool>(geo::attr::kHard);
    double cosLimit = std::cos(glm::radians(degrees));
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2)
            continue;
        if (degrees < 0.0)
        {
            if (e < hard.size() && hard[e])
                out.push_back(e);
            continue;
        }
        geo::Index c = m.edgeCorner(e);
        geo::Index f0 = m.cornerFace(c), f1 = m.cornerFace(m.cornerRadial(c));
        if (glm::dot(m.faceNormal(f0), m.faceNormal(f1)) < cosLimit)
            out.push_back(e);
    }
    return out;
}

geo::Mesh solidify(const geo::Mesh& in, double thickness)
{
    geo::Mesh m = in;
    m.compact();
    auto normals = geo::pointNormals(m, geo::NormalWeight::Angle);
    geo::Index n = m.pointCapacity();
    geo::Mesh inner = m;
    for (geo::Index p = 0; p < n; ++p)
        inner.setPosition(p, m.position(p) - glm::dvec3(normals[p]) * thickness);
    geo::flipNormals(inner, allFaces(inner));
    std::vector<geo::EdgePath> loops = geo::boundaryLoops(m);
    geo::Index offset = m.append(inner);
    // Rim quads join each boundary edge to its copy on the inner shell.
    for (const geo::EdgePath& loop : loops)
        for (geo::Index e : loop.edges)
        {
            geo::Index c = m.edgeCorner(e);
            geo::Index a = m.cornerPoint(c), bb = m.cornerNextPoint(c);
            m.addFace({bb, a, a + offset, bb + offset});
        }
    return m;
}

geo::Mesh decimate(const geo::Mesh& in, const Node& n)
{
    geo::ReduceOptions o;
    o.ratio = std::clamp(numOf(n, "ratio"), 0.0, 1.0);
    o.targetTriangles = geo::Index(std::max<int64_t>(0, intOf(n, "targetTriangles")));
    o.keepBorders = flagOf(n, "keepBorders");
    o.keepHardEdges = flagOf(n, "keepHardEdges");
    o.symmetryAxis = int(std::clamp<int64_t>(intOf(n, "symmetry"), 0, 3)) - 1;
    o.quads = flagOf(n, "quads");
    return geo::reduceMesh(in, o).mesh;
}

struct ModifierType
{
    const char* name;
    const char* doc;
    std::vector<AttrDef> params;
    ModifierNode::Apply apply;
};

std::vector<ModifierType> modifierTypes()
{
    return {
        {"mirrorModifier",
         "Adds a mirrored copy across a plane through the object's origin and welds the seam. Model one half of a symmetrical "
         "object and let this build the other.",
         {param("axis", PlugType::Enum, int64_t(0), "Object-space axis the mirror plane faces: x mirrors left to right."),
          param("direction", PlugType::Enum, int64_t(0),
                "Which side builds the other: + to - keeps the positive side and replaces the negative one with its mirror, cutting "
                "faces that cross the plane; - to + the reverse; both mirrors the whole mesh."),
          param("weld", PlugType::Bool, true, "Merge points on the plane with their mirror images so the halves join."),
          param("tolerance", PlugType::Float, 0.01, "Points this close to the plane are welded, cm.", 0.0)},
         [](const geo::Mesh& in, const Node& n) {
             geo::Mesh m = in;
             geo::MirrorOptions o;
             o.plane.normal = glm::dvec3(0.0);
             o.plane.normal[int(std::clamp<int64_t>(intOf(n, "axis"), 0, 2))] = 1.0;
             o.direction = geo::MirrorDirection(std::clamp<int64_t>(intOf(n, "direction"), 0, 2));
             o.weld = flagOf(n, "weld");
             o.tolerance = numOf(n, "tolerance");
             geo::mirrorMesh(m, o);
             return m;
         }},
        {"subdivisionModifier",
         "Smooths the mesh with Catmull-Clark subdivision. Use it to turn a low-poly cage into a smooth high-poly surface.",
         {param("levels", PlugType::Int, int64_t(1), "Subdivision levels, 0 to 4. Each level about quadruples the face count; 0 passes the mesh through.", 0.0, 4.0),
          param("keepHardEdges", PlugType::Bool, false, "Keep normals split along hard edges; off smooths the shading across them."),
          param("uvs", PlugType::Enum, int64_t(1),
                "How island interiors get their uvs. fitSurface spreads them evenly over the smoothed surface, so rounded "
                "bevels never squash into slivers; blend smooths them like the points."),
          param("uvCorners", PlugType::Enum, int64_t(0), "Island corner uvs: keep holds them where the cage has them; smooth rounds them off with the border.")},
         [](const geo::Mesh& in, const Node& n) {
             geo::SubdivideOptions o;
             o.levels = int(std::clamp<int64_t>(intOf(n, "levels"), 0, 4));
             o.keepHardEdges = flagOf(n, "keepHardEdges");
             o.uvs = geo::SubdivideUvs(std::clamp<int64_t>(intOf(n, "uvs"), 0, 1));
             o.uvCorners = geo::SubdivideUvCorners(std::clamp<int64_t>(intOf(n, "uvCorners"), 0, 1));
             return o.levels == 0 ? in : geo::subdivide(in, o);
         }},
        {"bevelModifier",
         "Bevels every edge sharper than an angle so hard corners catch the light. A quick live bevel; Auto Bevel has finer "
         "edge rules and hand-picked edges.",
         {param("offset", PlugType::Float, 1.0, "Bevel width, cm.", 0.0),
          param("segments", PlugType::Int, int64_t(2), "Edges across each bevel, 1 to 32. More is rounder.", 1.0, 32.0),
          param("profile", PlugType::Float, 0.5, "Profile shape: 0.5 is a round arc, 0.25 a straight chamfer.", 0.0, 1.0),
          param("angle", PlugType::Float, 30.0, "Bevel edges whose faces meet sharper than this, degrees. Negative bevels hard edges only.", {}, 180.0)},
         [](const geo::Mesh& in, const Node& n) {
             geo::Mesh m = in;
             geo::BevelOptions o;
             o.offset = numOf(n, "offset");
             o.segments = int(std::clamp<int64_t>(intOf(n, "segments"), 1, 32));
             o.profile = numOf(n, "profile");
             geo::bevelEdges(m, sharpEdges(m, numOf(n, "angle")), o);
             return m;
         }},
        {"arrayModifier",
         "Repeats the mesh in a row, around a circle or in a grid, each copy stepped from the last. Use it for railings, "
         "stairs, tiles, gear teeth and bolts round a flange. Radial with weld on turns one modelled wedge into an exact "
         "N-fold whole.",
         {param("mode", PlugType::Enum, int64_t(0), "linear steps each copy by offset; radial turns copies about the centre; grid fills rows, columns and layers."),
          param("count", PlugType::Int, int64_t(3), "Copies including the original, 1 to 1000. In grid mode, copies along x.", 1.0, 1000.0),
          param("offset", PlugType::Vec3, glm::dvec3(110.0, 0.0, 0.0), "Linear: step from one copy to the next, in the centre's space, cm."),
          param("rotate", PlugType::Vec3, glm::dvec3(0.0), "Extra rotation per copy about the centre, degrees. A linear array with a rotation curves."),
          param("scale", PlugType::Vec3, glm::dvec3(1.0), "Scale per copy about the centre: each copy is this much the size of the last."),
          inCategory(param("axis", PlugType::Enum, int64_t(1), "The centre's axis radial copies turn about."), "Radial"),
          inCategory(param("angle", PlugType::Float, 360.0, "Degrees the copies span, or between neighbours when angleMode is step. A whole turn closes the ring."), "Radial"),
          inCategory(param("angleMode", PlugType::Enum, int64_t(0), "total spreads count copies over angle; step puts angle between neighbours."), "Radial"),
          inCategory(param("rise", PlugType::Float, 0.0, "Distance each copy climbs along the axis, cm, for spirals and screw threads."), "Radial"),
          inCategory(param("countY", PlugType::Int, int64_t(3), "Grid copies along y, 1 to 1000.", 1.0, 1000.0), "Grid"),
          inCategory(param("countZ", PlugType::Int, int64_t(1), "Grid copies along z, 1 to 1000.", 1.0, 1000.0), "Grid"),
          inCategory(param("spacing", PlugType::Vec3, glm::dvec3(110.0), "Distance between grid copies along each axis, in the centre's space, cm."), "Grid"),
          inCategory(param("center", PlugType::Enum, int64_t(0),
                           "What copies turn and scale about: the object's origin, centerPoint, or another object's transform (arrayCenter)."), "Center"),
          inCategory(param("centerPoint", PlugType::Vec3, glm::dvec3(0.0), "Centre when center is point, object space, cm."), "Center"),
          inCategory({.name = "centerMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
                      .doc = "The centre object's worldMatrix, connected by arrayCenter."}, "Center"),
          inCategory({.name = "targetMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable | AttrHidden,
                      .doc = "This mesh's world matrix when the centre object was connected."}, "Center"),
          inCategory(param("weld", PlugType::Bool, false, "Merge points where neighbouring copies meet and drop the walls they share, so the copies join into one surface."), "Weld"),
          inCategory(param("mergeEnds", PlugType::Bool, false, "Weld the last copy to the first as well. A radial array spanning a whole turn always does."), "Weld"),
          inCategory(param("tolerance", PlugType::Float, 0.01, "Points of neighbouring copies this close are welded, cm.", 0.0)
                     , "Weld")},
         [](const geo::Mesh& in, const Node& n) { return geo::arrayMesh(in, arrayOptions(n)); }},
        {"triangulateModifier",
         "Splits every face into triangles. Use it to see, or lock in, the triangulation an engine will get.",
         {},
         [](const geo::Mesh& in, const Node&) {
             geo::Mesh m = in;
             geo::triangulateFaces(m, allFaces(m));
             return m;
         }},
        {"weightedNormalsModifier",
         "Weights vertex normals by face area so big flat faces shade flat and the bevels take the curvature. Use it on "
         "bevelled hard-surface game meshes.",
         {param("mode", PlugType::Enum, int64_t(0), "What weighs a face: its area, its corner angle, or both."),
          param("dominance", PlugType::Float, 0.5, "Faces under this fraction of the heaviest face in a fan are left out; 0 keeps all.", 0.0, 1.0),
          param("keepHardEdges", PlugType::Bool, false, "Keep normals split along the mesh's hard edges. Leave off on a mesh you bake a normal map onto.")},
         [](const geo::Mesh& in, const Node& n) {
             geo::Mesh m = in;
             geo::WeightedNormalOptions o;
             o.mode = geo::WeightedNormalMode(std::clamp<int64_t>(intOf(n, "mode"), 0, 2));
             o.dominance = std::clamp(numOf(n, "dominance"), 0.0, 1.0);
             o.keepSharp = flagOf(n, "keepHardEdges");
             geo::applyWeightedNormals(m, o);
             return m;
         }},
        {"solidifyModifier",
         "Gives a surface thickness: an inner shell offset along the normals, joined to the outer one at open borders. Use it "
         "to turn planes and open shells into solid panels.",
         {param("thickness", PlugType::Float, 2.0, "Shell thickness, cm, offset inward along the normals.")},
         [](const geo::Mesh& in, const Node& n) { return solidify(in, numOf(n, "thickness")); }},
        {"decimateModifier",
         "Reduces the triangle count while keeping the shape, silhouette, UVs and normals. Use it for LODs or to lighten "
         "scans and sculpts.",
         {param("ratio", PlugType::Float, 0.5, "Fraction of triangles to keep, 0 to 1.", 0.0, 1.0),
          param("targetTriangles", PlugType::Int, int64_t(0), "Triangles to keep; above 0 it overrides ratio.", 0.0),
          param("keepBorders", PlugType::Bool, true, "Open borders stay exactly where they are."),
          param("keepHardEdges", PlugType::Bool, true, "Hard edges stay intact, as UV seams and material borders always do; off lets them collapse for a lower count."),
          param("symmetry", PlugType::Enum, int64_t(0), "off, or reduce one half and mirror it so the result stays exactly symmetric across x, y or z."),
          param("quads", PlugType::Bool, false, "Join triangle pairs back into quads where they make good ones.")},
         [](const geo::Mesh& in, const Node& n) { return decimate(in, n); }},
    };
}

std::map<std::string, std::function<void(Graph&, Node&, NodeId)>>& createHooks()
{
    static std::map<std::string, std::function<void(Graph&, Node&, NodeId)>> hooks;
    return hooks;
}

Node& modifierMesh(const std::string& name, const char* cmd)
{
    Node* n = Graph::current().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

int outAttrOf(const Node& n)
{
    if (auto* m = modifiers::asModifier(&n))
        return m->outAttr;
    if (auto* g = dynamic_cast<const GeneratorNode*>(&n))
        return g->outAttr;
    return n.type().attrIndex("outMesh");
}
} // namespace

void ModifierNode::compute(const EvalContext&)
{
    LILA_ZONE();

    mError.clear();
    const MeshPtr& in = as<MeshPtr>(inAttr);
    if (!in || !as<bool>(enabledAttr) || modifiers::bypassed(in))
    {
        setOutput(outAttr, in);
        return;
    }
    try
    {
        setOutput(outAttr, MeshPtr(std::make_shared<geo::Mesh>(apply(*modifiers::stackInput(*this, in), *this))));
    }
    catch (const std::exception& e)
    {
        log::warn("{}: {}", name(), e.what());
        mError = e.what();
        setOutput(outAttr, in);
    }
}

void MeshSourceNode::compute(const EvalContext&)
{
    setOutput(kOutMesh, as<MeshPtr>(kMesh));
}

namespace modifiers
{
bool bypassed(const MeshPtr& mesh) { return mesh && mesh->pointAttrs().has(kBypassLayer); }

bool isModifier(const Node& n) { return (n.traits() & Node::kModifier) != 0; }

void setCreateHook(const std::string& type, std::function<void(Graph&, Node& modifier, NodeId mesh)> hook)
{
    createHooks()[type] = std::move(hook);
}

void baseRemapped(Graph& g, NodeId mesh, const geo::MeshRemap& remap, const geo::Mesh& base)
{
    autoBevel::baseRemapped(g, mesh, remap, base);
    extrudeModifier::baseRemapped(g, mesh, remap, base);
    splitRingsModifier::baseRemapped(g, mesh, remap, base);
}

std::vector<NodeId> stack(Graph& g, NodeId mesh)
{
    std::vector<NodeId> out;
    PlugRef plug{mesh, MeshNode::kInMesh, -1};
    while (auto src = g.source(plug))
    {
        Node* up = g.node(src->node);
        auto* mod = asModifier(up);
        if (!mod)
            break;
        out.insert(out.begin(), up->id());
        plug = {up->id(), mod->inAttr, -1};
    }
    return out;
}

namespace
{
bool isSubdivision(const Node& n)
{
    return n.type().name == "subdivisionModifier";
}
} // namespace

bool feedsSubdivision(const ModifierNode& mod)
{
    const Graph* g = mod.graph();
    if (!g)
        return false;
    std::vector<PlugRef> todo = g->destinations({mod.id(), mod.outAttr, -1});
    std::unordered_set<NodeId> seen;
    while (!todo.empty())
    {
        const PlugRef p = todo.back();
        todo.pop_back();
        const ModifierNode* next = asModifier(g->node(p.node));
        if (!next || p.attr != next->inAttr || !seen.insert(next->id()).second)
            continue;
        if (isSubdivision(*next))
            return true;
        for (const PlugRef& d : g->destinations({next->id(), next->outAttr, -1}))
            todo.push_back(d);
    }
    return false;
}

MeshPtr stackInput(const ModifierNode& mod, const MeshPtr& in)
{
    if (!in || isSubdivision(mod) || geo::hasFitSpacing(*in) || !feedsSubdivision(mod))
        return in;
    auto m = std::make_shared<geo::Mesh>(*in);
    geo::addFitSpacing(*m);
    return m;
}

namespace
{
/** The plug at the bottom of the modifier chain, and what feeds it. */
std::pair<PlugRef, std::optional<PlugRef>> chainBase(Graph& g, NodeId mesh)
{
    PlugRef plug{mesh, MeshNode::kInMesh, -1};
    while (auto src = g.source(plug))
    {
        auto* mod = asModifier(g.node(src->node));
        if (!mod)
            return {plug, src};
        plug = {mod->id(), mod->inAttr, -1};
    }
    return {plug, std::nullopt};
}
} // namespace

PlugRef editPlug(Graph& g, NodeId mesh)
{
    if (Node* n = g.node(mesh); n && !n->type().isA("mesh"))
        if (const components::PointProvider* pp = components::pointProvider(*n); pp && pp->editPlug)
            return pp->editPlug(g, mesh);
    // An instance has no base of its own: edits land on its root's, which every member shares.
    if (NodeId root = instancing::root(g, mesh); root != mesh)
        return editPlug(g, root);
    for (NodeId id : stack(g, mesh))
        if (static_cast<ModifierNode*>(g.node(id))->editingInput())
            throw CommandError(g.node(id)->path() + " is editing its edges: apply or cancel that first");
    auto [base, src] = chainBase(g, mesh);
    if (!src)
        return base;
    Node* up = g.node(src->node);
    if (dynamic_cast<MeshSourceNode*>(up))
        return {up->id(), MeshSourceNode::kMesh, -1};

    // A generator (or anything else) feeds the chain: freeze its current result into a stored source.
    g.evaluate();
    MeshPtr frozen = std::get<MeshPtr>(g.value(*src));
    log::info("{}: baked {} into the mesh to edit it; its settings no longer apply", g.node(mesh)->path(), up->path());
    // Other stacks it feeds (references) move onto the frozen source with this one, so they keep sharing it.
    std::vector<PlugRef> shared;
    for (const PlugRef& d : g.destinations(*src))
    {
        const Node* to = g.node(d.node);
        const bool chainInput = to->type().isA("mesh") ? d.attr == MeshNode::kInMesh : asModifier(to) && d.attr == asModifier(to)->inAttr;
        if (chainInput && !(d.node == base.node && d.attr == base.attr))
            shared.push_back(d);
    }
    g.disconnect(base);
    if (base.node == mesh && shared.empty())
    {
        g.setValue(base, frozen);
        if (g.destinations(*src).empty() && !up->type().dag)
            g.deleteNode(up->id());
        return base;
    }
    Node* source = g.createNode(*types::find("meshSource"), {}, kNoNode, true);
    g.setValue({source->id(), MeshSourceNode::kMesh, -1}, frozen);
    g.connect({source->id(), MeshSourceNode::kOutMesh, -1}, base);
    for (const PlugRef& d : shared)
    {
        g.disconnect(d);
        g.connect({source->id(), MeshSourceNode::kOutMesh, -1}, d);
    }
    if (g.destinations(*src).empty() && !up->type().dag)
        g.deleteNode(up->id());
    return {source->id(), MeshSourceNode::kMesh, -1};
}

json selectionSnapshot(Graph& g, NodeId mesh)
{
    json j;
    j["mode"] = components::modeName(components::mode());
    j["vertices"] = components::selected(mesh, ComponentMode::Vertex);
    j["edges"] = components::selected(mesh, ComponentMode::Edge);
    j["faces"] = components::selected(mesh, ComponentMode::Face);
    json names = json::array();
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            names.push_back(n->path());
    j["selection"] = names;
    return j;
}

void restoreSelection(Graph& g, NodeId mesh, const json& j)
{
    if (!j.is_object())
        return;
    std::vector<NodeId> ids;
    for (const json& name : j.value("selection", json::array()))
        if (Node* n = g.find(name.get<std::string>()))
            ids.push_back(n->id());
    g.select(ids);
    for (auto [key, kind] : {std::pair{"vertices", ComponentMode::Vertex}, {"edges", ComponentMode::Edge}, {"faces", ComponentMode::Face}})
        components::select(mesh, kind, j.value(key, json::array()).get<components::Ids>());
    try
    {
        components::setMode(components::modeFromName(j.value("mode", "object")));
    }
    catch (const std::invalid_argument&)
    {
    }
}

MeshPtr cage(Graph& g, NodeId mesh)
{
    g.evaluate();
    if (Node* n = g.node(mesh); !n || !n->type().isA("mesh"))
        return nullptr;
    if (NodeId root = instancing::root(g, mesh); root != mesh)
        return cage(g, root);
    for (NodeId id : stack(g, mesh))
        if (auto* m = static_cast<ModifierNode*>(g.node(id)); m->editingInput())
            return m->as<MeshPtr>(m->inAttr);
    auto [base, src] = chainBase(g, mesh);
    if (!src)
        return std::get<MeshPtr>(g.value(base));
    return std::get<MeshPtr>(g.value(*src));
}

void registerTypes()
{
    if (types::find("meshSource"))
        return;
    NodeType source;
    source.name = "meshSource";
    source.icon = "database";
    source.doc = "Holds the base mesh under a modifier stack, where direct component edits land.";
    source.ownAttrs = {
        {.name = "mesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Base mesh under a modifier stack."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
    };
    source.create = [] { return std::make_unique<MeshSourceNode>(); };
    types::registerType(std::move(source));

    for (ModifierType& m : modifierTypes())
    {
        NodeType t;
        t.name = m.name;
        t.doc = m.doc;
        t.icon = "stack-2";
        t.ownAttrs.push_back({.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden});
        t.ownAttrs.push_back(param("enabled", PlugType::Bool, true, "Turn the modifier off without removing it."));
        t.ownAttrs.push_back({.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable,
                              .doc = "Apply the modifier when exporting."});
        for (AttrDef& p : m.params)
        {
            if (std::string(m.name) == "mirrorModifier" && p.name == "axis")
                p.enumNames = {"x", "y", "z"};
            if (std::string(m.name) == "mirrorModifier" && p.name == "direction")
                p.enumNames = {"+ to -", "- to +", "both"};
            if (std::string(m.name) == "weightedNormalsModifier" && p.name == "mode")
                p.enumNames = {"area", "angle", "areaAngle"};
            if (std::string(m.name) == "subdivisionModifier" && p.name == "uvs")
            {
                p.enumNames = {"blend", "fitSurface"};
                p.niceName = "UVs";
            }
            if (std::string(m.name) == "subdivisionModifier" && p.name == "uvCorners")
            {
                p.enumNames = {"keep", "smooth"};
                p.niceName = "UV Corners";
            }
            if (std::string(m.name) == "decimateModifier" && p.name == "symmetry")
                p.enumNames = {"off", "x", "y", "z"};
            if (std::string(m.name) == "arrayModifier")
            {
                if (p.name == "mode")
                    p.enumNames = {"linear", "radial", "grid"};
                else if (p.name == "axis")
                    p.enumNames = {"x", "y", "z"};
                else if (p.name == "angleMode")
                    p.enumNames = {"total", "step"};
                else if (p.name == "center")
                    p.enumNames = {"origin", "point", "object"};
            }
            if (p.type == PlugType::Vec3)
                for (int k = 0; k < 3; ++k)
                    p.childNames[k] = p.name + char('X' + k);
            t.ownAttrs.push_back(p);
        }
        t.ownAttrs.push_back({.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden});
        int out = int(t.ownAttrs.size()) - 1;
        ModifierNode::Apply apply = m.apply;
        t.create = [apply, out] {
            auto n = std::make_unique<ModifierNode>();
            n->apply = apply;
            n->outAttr = out;
            return n;
        };
        types::registerType(std::move(t));
    }
    autoBevel::registerTypes();
    extrudeModifier::registerTypes();
    splitRingsModifier::registerTypes();
    registerTransferModifier();
    registerBooleanModifier();
}

void registerCommands()
{
    autoBevel::registerCommands();
    splitRingsModifier::registerCommands();
    registerStackCommands();
    using T = ArgType;
    std::string kindList;
    for (const std::string& kind : kinds())
        kindList += "\n- " + kind + ": " + kindDoc(kind);

    ops::registerCommand({
        .name = "addModifier",
        .doc = "Add a modifier to the top of a mesh's stack. params sets its parameters. Returns its name. modifierKinds(docs=True) "
               "also lists kinds extensions add later.\n\nKinds:" + kindList,
        .positionalDoc = "mesh",
        .flags = {{"kind", "", T::String, nullptr, "Modifier kind, such as bevel or mirror (see Kinds)."},
                  {"params", "", T::Any, nullptr, "{param: value} to set on the new modifier."}},
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty() || !a.has("kind"))
                throw CommandError("addModifier: give a mesh and kind=");
            Graph& g = Graph::current();
            // On an instance it goes on the root's stack, which every member shares.
            Node& mesh = *g.node(instancing::root(g, modifierMesh(objs.back(), "addModifier").id()));
            const NodeType* t = types::find(a.get<std::string>("kind") + "Modifier");
            if (!t)
                throw CommandError("addModifier: unknown kind '" + a.get<std::string>("kind") + "'");

            PlugRef top{mesh.id(), MeshNode::kInMesh, -1};
            auto src = g.source(top);
            Node* mod = g.createNode(*t, {}, kNoNode, true);
            auto* mn = static_cast<ModifierNode*>(mod);
            if (auto hook = createHooks().find(t->name); hook != createHooks().end())
                hook->second(g, *mod, mesh.id());
            if (src)
            {
                g.disconnect(top);
                g.connect(*src, {mod->id(), mn->inAttr, -1});
            }
            else
            {
                // The mesh's stored base moves into a source node so it stays editable under the stack.
                Node* source = g.createNode(*types::find("meshSource"), {}, kNoNode, true);
                g.setValue({source->id(), MeshSourceNode::kMesh, -1}, g.storedValue(top));
                g.setValue(top, MeshPtr());
                g.connect({source->id(), MeshSourceNode::kOutMesh, -1}, {mod->id(), mn->inAttr, -1});
            }
            g.connect({mod->id(), mn->outAttr, -1}, top);
            // The modifiers below rerun to carry the spacing layers their first one adds for a subdivision.
            if (isSubdivision(*mod))
                for (NodeId id : stack(g, mesh.id()))
                    g.touch(id);
            if (a.has("params"))
                for (auto& [k, val] : a.flags["params"].items())
                {
                    PlugRef p = g.plug(mod->path() + "." + k);
                    AttrDef def = g.attr(p);
                    if (p.component >= 0)
                        def.type = PlugType::Float;
                    g.setValue(p, fromJson(def, val));
                }
            return mod->path();
        },
    });

    ops::registerCommand({
        .name = "arrayCenter",
        .doc = "Set what an array modifier's copies turn and scale about. Give an object to centre on its transform (its "
               "position and axes, kept live as it moves; run again after moving the arrayed mesh), pivot=True to copy the "
               "mesh's pivot into centerPoint, or neither to go back to the object's origin.",
        .positionalDoc = "modifier, [object]",
        .flags = {{"pivot", "", T::Bool, nullptr, "Centre on the arrayed mesh's current pivot."}},
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            Graph& g = Graph::current();
            Node* mod = objs.empty() ? nullptr : g.find(objs.front());
            if (!mod || mod->type().name != "arrayModifier")
                throw CommandError("arrayCenter: give an array modifier first");
            auto at = [&](const char* name) { return PlugRef{mod->id(), mod->type().attrIndex(name), -1}; };
            const NodeId mesh = meshOf(g, mod->id());
            Node* meshNode = g.node(mesh);
            if (g.source(at("centerMatrix")))
                g.disconnect(at("centerMatrix"));
            if (objs.size() > 1)
            {
                Node* center = g.find(objs.back());
                if (!center || !center->type().isA("transform"))
                    throw CommandError("arrayCenter: " + objs.back() + " is not an object");
                if (center->id() == mesh)
                    throw CommandError("arrayCenter: centre on another object; pivot=True uses the mesh's own pivot");
                g.connect({center->id(), Transform::kWorldMatrix, -1}, at("centerMatrix"), true);
                if (meshNode)
                    g.setValue(at("targetMatrix"), meshNode->as<glm::dmat4>(Transform::kWorldMatrix));
                g.setValue(at("center"), int64_t(kCenterObject));
            }
            else if (a.has("pivot") && a.get<bool>("pivot"))
            {
                if (!meshNode)
                    throw CommandError("arrayCenter: the modifier feeds no mesh");
                g.setValue(at("centerPoint"), meshNode->as<glm::dvec3>(Transform::kRotatePivot));
                g.setValue(at("center"), int64_t(kCenterPoint));
            }
            else
                g.setValue(at("center"), int64_t(kCenterOrigin));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "modifiers",
        .doc = "A mesh's modifier stack, bottom (applied first) to top.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("modifiers: no mesh");
            json out = json::array();
            for (NodeId id : stack(Graph::current(), modifierMesh(objs.back(), "modifiers").id()))
                out.push_back(Graph::current().node(id)->path());
            return out;
        },
    });

    auto unlink = [](Graph& g, Node& mod) {
        auto* mn = static_cast<ModifierNode*>(&mod);
        auto src = g.source({mod.id(), mn->inAttr, -1});
        std::vector<PlugRef> dsts = g.destinations({mod.id(), mn->outAttr, -1});
        for (const PlugRef& d : dsts)
            g.disconnect(d);
        if (src)
            for (const PlugRef& d : dsts)
                g.connect(*src, d);
        g.deleteNode(mod.id());
    };

    ops::registerCommand({
        .name = "removeModifier",
        .doc = "Remove modifiers, reconnecting the stack around them.",
        .positionalDoc = "modifiers",
        .run = [unlink](const Args& a) -> json {
            Graph& g = Graph::current();
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n || !isModifier(*n))
                    throw CommandError("removeModifier: " + name + " is not a modifier");
                unlink(g, *n);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "applyModifier",
        .doc = "Bake a modifier (and everything below it in the stack) into the base mesh.",
        .positionalDoc = "modifier",
        .run = [unlink](const Args& a) -> json {
            Graph& g = Graph::current();
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n || !isModifier(*n))
                    throw CommandError("applyModifier: " + name + " is not a modifier");
                auto* mn = static_cast<ModifierNode*>(n);
                g.evaluate();
                MeshPtr baked = n->as<MeshPtr>(mn->outAttr);
                // Walk down to the source and drop every modifier below this one along the way.
                PlugRef plug{n->id(), mn->inAttr, -1};
                std::vector<NodeId> below;
                std::optional<PlugRef> src;
                while ((src = g.source(plug)))
                {
                    auto* down = asModifier(g.node(src->node));
                    if (!down)
                        break;
                    below.push_back(down->id());
                    plug = {down->id(), down->inAttr, -1};
                }
                std::vector<PlugRef> dsts = g.destinations({n->id(), mn->outAttr, -1});
                for (const PlugRef& d : dsts)
                    g.disconnect(d);
                Node* source = src ? g.node(src->node) : nullptr;
                if (!dynamic_cast<MeshSourceNode*>(source))
                {
                    if (src)
                        g.disconnect(plug);
                    source = g.createNode(*types::find("meshSource"), {}, kNoNode, true);
                }
                g.setValue({source->id(), MeshSourceNode::kMesh, -1}, baked);
                for (const PlugRef& d : dsts)
                    g.connect({source->id(), MeshSourceNode::kOutMesh, -1}, d);
                g.deleteNode(n->id());
                for (NodeId id : below)
                    g.deleteNode(id);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "moveModifier",
        .doc = "Move a modifier up or down its stack by a number of steps (positive is up, applied later).",
        .positionalDoc = "modifier",
        .flags = {{"by", "", T::Int, 1, "Steps."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("moveModifier: no modifier");
            Node* n = g.find(objs.back());
            if (!n || !isModifier(*n))
                throw CommandError("moveModifier: " + objs.back() + " is not a modifier");
            // Find the mesh at the top of this chain.
            NodeId cur = n->id();
            PlugRef top;
            while (true)
            {
                auto dsts = g.destinations({cur, outAttrOf(*g.node(cur)), -1});
                if (dsts.empty())
                    throw CommandError("moveModifier: modifier is not connected to a mesh");
                Node* d = g.node(dsts[0].node);
                if (d->type().isA("mesh"))
                {
                    top = dsts[0];
                    break;
                }
                cur = d->id();
            }
            std::vector<NodeId> order = stack(g, top.node);
            auto it = std::find(order.begin(), order.end(), n->id());
            int from = int(it - order.begin());
            int to = std::clamp(from + a.get<int>("by"), 0, int(order.size()) - 1);
            if (from == to)
                return nullptr;
            order.erase(order.begin() + from);
            order.insert(order.begin() + to, n->id());
            // Rewire the chain in the new order.
            auto* first = static_cast<ModifierNode*>(g.node(stack(g, top.node).front()));
            auto base = g.source({first->id(), first->inAttr, -1});
            for (NodeId id : stack(g, top.node))
                g.disconnect({id, static_cast<ModifierNode*>(g.node(id))->inAttr, -1});
            g.disconnect(top);
            std::optional<PlugRef> prev = base;
            for (NodeId id : order)
            {
                auto* m = static_cast<ModifierNode*>(g.node(id));
                if (prev)
                    g.connect(*prev, {id, m->inAttr, -1});
                prev = PlugRef{id, m->outAttr, -1};
            }
            g.connect(*prev, top);
            return nullptr;
        },
    });
}
} // namespace modifiers
} // namespace lila::scene
