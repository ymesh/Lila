#include "scene/ExtrudeModifier.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/Symmetry.h"
#include "scene/AttrBuilders.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <unordered_map>

namespace lila::scene
{
using namespace attrdefs;

namespace
{
using geo::Index;

class ExtrudeModifierNode final : public ModifierNode
{
public:
    enum Attr
    {
        kInMesh,
        kEnabled,
        kExportEnabled,
        kThickness,
        kOffset,
        kIndividual,
        kKeepBack,
        kComponents,
        kMeshMatrix,
        kOutMesh,
        kLost,
    };

    ExtrudeModifierNode()
    {
        inAttr = kInMesh;
        enabledAttr = kEnabled;
        outAttr = kOutMesh;
    }

    void compute(const EvalContext& ctx) override;
};

/** Finds faces again by their centre, on a hash grid of cells a few tolerances wide. */
class CentreMatcher
{
public:
    explicit CentreMatcher(const geo::Mesh& m) : mMesh(m)
    {
        mTolerance = std::max(1e-6, glm::length(m.bounds().size()) * 1e-6);
        mCell = mTolerance * 4.0;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                mGrid[key(cellOf(m.faceCentroid(f)))].push_back(f);
    }

    Index find(const glm::dvec3& centre) const
    {
        const glm::ivec3 c = cellOf(centre);
        Index best = geo::kInvalid;
        double bestDistance = mTolerance;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz)
                    if (auto it = mGrid.find(key(c + glm::ivec3(dx, dy, dz))); it != mGrid.end())
                        for (Index f : it->second)
                            if (double d = glm::length(mMesh.faceCentroid(f) - centre); d <= bestDistance)
                            {
                                best = f;
                                bestDistance = d;
                            }
        return best;
    }

private:
    glm::ivec3 cellOf(const glm::dvec3& p) const { return glm::ivec3(glm::floor(p / mCell)); }
    static uint64_t key(const glm::ivec3& c)
    {
        return (uint64_t(uint32_t(c.x)) * 73856093ull) ^ (uint64_t(uint32_t(c.y)) * 19349663ull) ^ (uint64_t(uint32_t(c.z)) * 83492791ull);
    }

    const geo::Mesh& mMesh;
    double mTolerance = 1e-6;
    double mCell = 4e-6;
    std::unordered_map<uint64_t, std::vector<Index>> mGrid;
};

bool componentsMatch(const geo::Mesh& m, const ExtrudeComponents& c)
{
    return m.pointCapacity() == c.pointCount && m.edgeCapacity() == c.edgeCount && m.faceCapacity() == c.faceCount;
}

extrudeModifier::Resolved resolveOn(const geo::Mesh& m, const Node& n)
{
    return extrudeModifier::resolveComponents(m, dynamic_cast<const ExtrudeComponents*>(n.as<DataPtr>(ExtrudeModifierNode::kComponents).get()));
}
} // namespace

extrudeModifier::Resolved extrudeModifier::resolveComponents(const geo::Mesh& m, const ExtrudeComponents* c)
{
    LILA_ZONE();

    extrudeModifier::Resolved r;
    if (!c)
    {
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                r.ids.push_back(f);
        return r;
    }
    const bool same = componentsMatch(m, *c);
    if (c->edges)
    {
        r.kind = ComponentMode::Edge;
        geo::EdgeMatcher matcher(m);
        for (const geo::EdgeRef& ref : c->edgeRefs)
            if (Index e = matcher.find(ref, same); e != geo::kInvalid)
                r.ids.push_back(e);
            else
                ++r.lost;
    }
    else
    {
        std::optional<CentreMatcher> matcher;
        for (const ExtrudeComponents::FaceRef& ref : c->faces)
        {
            Index f = same && ref.face < m.faceCapacity() && m.faceValid(ref.face) ? ref.face : geo::kInvalid;
            if (f == geo::kInvalid)
            {
                if (!matcher)
                    matcher.emplace(m);
                f = matcher->find(ref.centre);
            }
            if (f != geo::kInvalid)
                r.ids.push_back(f);
            else
                ++r.lost;
        }
    }
    std::sort(r.ids.begin(), r.ids.end());
    r.ids.erase(std::unique(r.ids.begin(), r.ids.end()), r.ids.end());
    return r;
}

namespace
{
void ExtrudeModifierNode::compute(const EvalContext&)
{
    LILA_ZONE();

    mError.clear();
    const MeshPtr& in = as<MeshPtr>(kInMesh);
    setOutput(kOutMesh, in);
    setOutput(kLost, int64_t(0));
    if (!in || !as<bool>(kEnabled) || modifiers::bypassed(in))
        return;
    try
    {
        extrudeModifier::Resolved r = resolveOn(*in, *this);
        setOutput(kLost, int64_t(r.lost));
        if (r.ids.empty())
            return;
        geo::Mesh m = *in;
        if (r.kind == ComponentMode::Edge)
            geo::extrudeEdges(m, r.ids, geo::EdgeExtrudeOptions{as<double>(kThickness), as<double>(kOffset), nullptr});
        else
            geo::extrudeFaces(m, r.ids, geo::ExtrudeOptions{as<double>(kThickness), as<bool>(kIndividual), nullptr, as<double>(kOffset), as<bool>(kKeepBack),
                                                          glm::dmat3(as<glm::dmat4>(kMeshMatrix))});
        m.compact();
        setOutput(kOutMesh, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    }
    catch (const std::exception& e)
    {
        log::warn("{}: {}", name(), e.what());
        mError = e.what();
        setOutput(kOutMesh, in);
    }
}

json edgeRefToJson(const geo::EdgeRef& r)
{
    return json::array({r.edge, r.p0, r.p1, r.a.x, r.a.y, r.a.z, r.b.x, r.b.y, r.b.z});
}

geo::EdgeRef edgeRefFromJson(const json& j)
{
    geo::EdgeRef r;
    if (!j.is_array() || j.size() != 9)
        return r;
    r.edge = j[0].get<Index>();
    r.p0 = j[1].get<Index>();
    r.p1 = j[2].get<Index>();
    r.a = {j[3].get<double>(), j[4].get<double>(), j[5].get<double>()};
    r.b = {j[6].get<double>(), j[7].get<double>(), j[8].get<double>()};
    return r;
}

/** addModifier's hook: the new modifier takes the mesh's selected faces, or edges in edge mode, with their mirror images under symmetry. */
void takeSelection(Graph& g, Node& mod, NodeId mesh)
{
    if (DataPtr c = extrudeModifier::selectedComponents(g, mesh))
        g.setValue({mod.id(), ExtrudeModifierNode::kComponents, -1}, c);
    if (Node* n = g.node(mesh))
        g.setValue({mod.id(), ExtrudeModifierNode::kMeshMatrix, -1}, n->as<glm::dmat4>(Transform::kWorldMatrix));
}
} // namespace

DataPtr extrudeModifier::selectedComponents(Graph& g, NodeId mesh)
{
    const ComponentMode mode = components::mode();
    if (mode == ComponentMode::Object)
        return nullptr;
    const components::Ids& sel = components::selected(mesh, mode);
    MeshPtr cage = sel.empty() ? nullptr : modifiers::cage(g, mesh);
    if (!cage)
        return nullptr;
    const bool edges = mode == ComponentMode::Edge;
    std::vector<Index> ids = components::convert(*cage, mode, sel, edges ? ComponentMode::Edge : ComponentMode::Face);
    Node& n = *g.node(mesh);
    if (const int64_t axis = n.as<int64_t>(MeshNode::kSymmetry); axis >= 1 && axis <= 3)
    {
        geo::Mesh copy = *cage;
        const double tolerance = std::max(1e-4, glm::length(copy.bounds().size()) * 1e-5);
        geo::SymmetryMap map = geo::SymmetryMap::persistent(copy, int(axis - 1), tolerance, nullptr, symmetryOffset(n, int(axis - 1)));
        ids = edges ? map.expandEdges(copy, ids) : map.expandFaces(copy, ids);
    }
    return ExtrudeComponents::take(*cage, edges, ids);
}

json ExtrudeComponents::toJson() const
{
    json f = json::array(), e = json::array();
    for (const FaceRef& r : faces)
        f.push_back(json::array({r.face, r.centre.x, r.centre.y, r.centre.z}));
    for (const geo::EdgeRef& r : edgeRefs)
        e.push_back(edgeRefToJson(r));
    return {{"kind", kind()},       {"edges", edges}, {"points", pointCount}, {"edgeCount", edgeCount},
            {"faceCount", faceCount}, {"faces", f},     {"edgeRefs", e}};
}

DataPtr ExtrudeComponents::fromJson(const json& j)
{
    auto c = std::make_shared<ExtrudeComponents>();
    c->edges = j.value("edges", false);
    c->pointCount = j.value("points", Index(0));
    c->edgeCount = j.value("edgeCount", Index(0));
    c->faceCount = j.value("faceCount", Index(0));
    for (const json& r : j.value("faces", json::array()))
        if (r.is_array() && r.size() == 4)
            c->faces.push_back({r[0].get<Index>(), {r[1].get<double>(), r[2].get<double>(), r[3].get<double>()}});
    for (const json& r : j.value("edgeRefs", json::array()))
        c->edgeRefs.push_back(edgeRefFromJson(r));
    return c;
}

std::shared_ptr<ExtrudeComponents> ExtrudeComponents::take(const geo::Mesh& mesh, bool edges, const std::vector<Index>& ids)
{
    auto c = std::make_shared<ExtrudeComponents>();
    c->edges = edges;
    c->pointCount = mesh.pointCapacity();
    c->edgeCount = mesh.edgeCapacity();
    c->faceCount = mesh.faceCapacity();
    for (Index id : ids)
        if (edges && id < mesh.edgeCapacity() && mesh.edgeValid(id))
            c->edgeRefs.push_back(geo::edgeRef(mesh, id));
        else if (!edges && id < mesh.faceCapacity() && mesh.faceValid(id))
            c->faces.push_back({id, mesh.faceCentroid(id)});
    return c;
}

namespace extrudeModifier
{
bool isExtrude(const Node& n) { return dynamic_cast<const ExtrudeModifierNode*>(&n) != nullptr; }

std::optional<Resolved> resolve(Graph& g, NodeId modifier)
{
    Node* n = g.node(modifier);
    if (!n || !isExtrude(*n))
        return std::nullopt;
    g.evaluate();
    MeshPtr in = n->as<MeshPtr>(ExtrudeModifierNode::kInMesh);
    if (!in)
        return std::nullopt;
    Resolved r = resolveOn(*in, *n);
    r.input = in;
    return r;
}

void baseRemapped(Graph& g, NodeId mesh, const geo::MeshRemap& remap, const geo::Mesh& base)
{
    std::vector<NodeId> s = modifiers::stack(g, mesh);
    if (s.empty() || !isExtrude(*g.node(s.front())))
        return;
    Node& mod = *g.node(s.front());
    if (DataPtr next = remapComponents(dynamic_cast<const ExtrudeComponents*>(mod.as<DataPtr>(ExtrudeModifierNode::kComponents).get()), remap, base))
        g.setValue({mod.id(), ExtrudeModifierNode::kComponents, -1}, next);
}

DataPtr remapComponents(const ExtrudeComponents* c, const geo::MeshRemap& remap, const geo::Mesh& base)
{
    // Ids taken on some other topology were already matched by position; the remap can't say where they went.
    if (!c || c->faceCount > remap.faces.size() || c->edgeCount > remap.edges.size())
        return nullptr;
    auto next = std::make_shared<ExtrudeComponents>(*c);
    next->pointCount = base.pointCapacity();
    next->edgeCount = base.edgeCapacity();
    next->faceCount = base.faceCapacity();
    for (ExtrudeComponents::FaceRef& r : next->faces)
    {
        const Index f = r.face < remap.faces.size() ? remap.faces[r.face] : geo::kInvalid;
        // Gone: only its centre can find it again.
        if (f == geo::kInvalid || !base.faceValid(f))
            r.face = geo::kInvalid;
        else
            r = {f, base.faceCentroid(f)};
    }
    for (geo::EdgeRef& r : next->edgeRefs)
    {
        const Index e = r.edge < remap.edges.size() ? remap.edges[r.edge] : geo::kInvalid;
        if (e == geo::kInvalid || !base.edgeValid(e))
            r.edge = r.p0 = r.p1 = geo::kInvalid;
        else
            r = geo::edgeRef(base, e);
    }
    return next;
}

void registerTypes()
{
    if (types::find("extrudeModifier"))
        return;
    data::registerKind("extrudeComponents", &ExtrudeComponents::fromJson);

    NodeType t;
    t.name = "extrudeModifier";
    t.icon = "stack-2";
    t.doc = "Extrudes faces, or grows strips off edges, kept live: the faces or edges selected when it was added follow later "
            "edits under it, and its thickness and offset stay adjustable. Without a selection it extrudes every face.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        param("enabled", PlugType::Bool, true, "Turn the modifier off without removing it."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable,
         .doc = "Apply the modifier when exporting."},
        param("thickness", PlugType::Float, 10.0, "Distance along the normals, cm."),
        param("offset", PlugType::Float, 0.0,
              "Faces: inward distance of the extruded border, cm; negative grows them. Edges: outward distance from their faces, cm."),
        param("individual", PlugType::Bool, false, "Extrude each face separately instead of connected faces as regions."),
        param("keepBack", PlugType::Bool, true, "Where the faces only border open edges, as a whole open shell, keep a copy facing back at the base."),
        {.name = "components", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The faces or edges to extrude."},
        {.name = "meshMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The mesh's world matrix when the modifier was added; thickness and offset are world distances under it."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "lostComponents", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput,
         .doc = "Faces or edges no longer found on the input mesh."},
    };
    t.create = [] { return std::make_unique<ExtrudeModifierNode>(); };
    types::registerType(std::move(t));
    modifiers::setCreateHook("extrudeModifier", takeSelection);
}
} // namespace extrudeModifier
} // namespace lila::scene
