#include "layout/SoftNet.h"
#include "layout/SoftNetBind.h"

#include "core/Profile.h"
#include "deform/SoftNet.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "layout/LayoutNode.h"
#include "layout/Path.h"
#include "layout/Resolve.h"
#include "ops/Command.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Node.h"
#include "scene/RigUtil.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace lila::layout
{
namespace
{
using namespace lila::scene;
using namespace lila::scene::rigutil;
namespace sn = lila::deform::softnet;

/** A list of matrices handed between Soft Net nodes: knot frames, or the bound meshes' world matrices. */
struct Matrices final : Data
{
    std::vector<glm::dmat4> m;
    const char* kind() const override { return "softNetMatrices"; }
    json toJson() const override { return {{"count", m.size()}}; }
    size_t bytes() const override { return m.size() * sizeof(glm::dmat4) + 32; }
};

json pointJson(const sn::SurfacePoint& p) { return {p.points[0], p.points[1], p.points[2], p.bary.x, p.bary.y, p.bary.z}; }

sn::SurfacePoint pointFrom(const json& j)
{
    sn::SurfacePoint p;
    if (j.is_array() && j.size() >= 6)
    {
        p.points = {j[0].get<uint32_t>(), j[1].get<uint32_t>(), j[2].get<uint32_t>()};
        p.bary = {j[3].get<double>(), j[4].get<double>(), j[5].get<double>()};
    }
    return p;
}

json netVecJson(const glm::dvec3& v) { return {v.x, v.y, v.z}; }
glm::dvec3 netVecFrom(const json& j) { return j.is_array() && j.size() == 3 ? glm::dvec3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>()) : glm::dvec3(0.0); }

std::vector<glm::dvec3> pointsOf(const geo::Mesh& m) { return {m.positions().begin(), m.positions().end()}; }

bool nearIdentity(const glm::dmat4& m)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(m[c][r] - (c == r ? 1.0 : 0.0)) > 1e-12)
                return false;
    return true;
}

std::map<int, glm::dmat4> elementMatrices(const Node& n, int attr)
{
    std::map<int, glm::dmat4> out;
    for (const auto& [i, v] : n.elements(attr))
        if (const auto* m = std::get_if<glm::dmat4>(&v))
            out[i] = *m;
    return out;
}

const Matrices* matricesOf(const Node& n, int attr)
{
    const auto* p = std::get_if<DataPtr>(&n.value(attr));
    return p && *p ? dynamic_cast<const Matrices*>(p->get()) : nullptr;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Bindings

json NetBind::toJson() const
{
    json k = json::array();
    for (const sn::KnotRig& r : knots)
    {
        json along = json::array(), dirs = json::array(), blend = json::array();
        for (const sn::SurfacePoint& p : r.along)
            along.push_back(pointJson(p));
        for (const glm::dvec3& d : r.restDirections)
            dirs.push_back(netVecJson(d));
        for (const auto& [j, w] : r.blend)
            blend.push_back({j, w});
        k.push_back({{"at", pointJson(r.at)},
                     {"along", along},
                     {"dirs", dirs},
                     {"blend", blend},
                     {"degree", r.degree},
                     {"origin", netVecJson(r.origin)},
                     {"rest", {netVecJson(r.rest[0]), netVecJson(r.rest[1]), netVecJson(r.rest[2])}}});
    }
    return {{"knots", k}};
}

NetBind NetBind::fromJson(const std::string& text)
{
    NetBind b;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object())
        return b;
    for (const json& k : j.value("knots", json::array()))
    {
        sn::KnotRig r;
        r.at = pointFrom(k.value("at", json()));
        for (const json& p : k.value("along", json::array()))
            r.along.push_back(pointFrom(p));
        for (const json& d : k.value("dirs", json::array()))
            r.restDirections.push_back(netVecFrom(d));
        for (const json& w : k.value("blend", json::array()))
            if (w.is_array() && w.size() == 2)
                r.blend.emplace_back(w[0].get<int>(), w[1].get<double>());
        r.degree = k.value("degree", 0);
        r.origin = netVecFrom(k.value("origin", json()));
        const json rest = k.value("rest", json::array());
        if (rest.size() == 3)
            r.rest = glm::dmat3(netVecFrom(rest[0]), netVecFrom(rest[1]), netVecFrom(rest[2]));
        b.knots.push_back(std::move(r));
    }
    return b;
}

json MeshBind::toJson() const
{
    json s = json::array(), r = json::array(), t = json::array();
    for (const sn::Sample& x : samples)
    {
        json p = pointJson(x.at);
        p.push_back(x.run);
        p.push_back(x.knot);
        p.push_back(x.t);
        s.push_back(p);
    }
    for (const sn::Run& x : runs)
        r.push_back({x.knot[0], x.knot[1], x.tangent[0], x.tangent[1]});
    for (size_t i = 0; i < tangentRest.size(); ++i)
        t.push_back({tangentKnot[i], tangentRest[i].x, tangentRest[i].y, tangentRest[i].z});
    json out = {{"points", points}, {"knots", knots}, {"edge", edge}, {"autoReach", autoReach}, {"samples", s}, {"runs", r}, {"tangents", t}};
    if (!lines.empty())
    {
        json l = json::array();
        for (const std::vector<sn::SurfacePoint>& line : lines)
        {
            json pts = json::array();
            for (const sn::SurfacePoint& p : line)
                pts.push_back(pointJson(p));
            l.push_back(pts);
        }
        out["lines"] = l;
    }
    return out;
}

MeshBind MeshBind::fromJson(const std::string& text)
{
    MeshBind b;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object())
        return b;
    b.points = j.value("points", size_t(0));
    b.knots = j.value("knots", size_t(0));
    b.edge = j.value("edge", 1.0);
    b.autoReach = j.value("autoReach", 1.0);
    for (const json& s : j.value("samples", json::array()))
        if (s.is_array() && s.size() == 9)
            b.samples.push_back({.at = pointFrom(s), .run = s[6].get<int>(), .knot = s[7].get<int>(), .t = s[8].get<double>()});
    for (const json& r : j.value("runs", json::array()))
        if (r.is_array() && r.size() == 4)
            b.runs.push_back({.knot = {r[0].get<int>(), r[1].get<int>()}, .tangent = {r[2].get<int>(), r[3].get<int>()}});
    for (const json& t : j.value("tangents", json::array()))
        if (t.is_array() && t.size() == 4)
        {
            b.tangentKnot.push_back(t[0].get<int>());
            b.tangentRest.push_back({t[1].get<double>(), t[2].get<double>(), t[3].get<double>()});
        }
    for (const json& l : j.value("lines", json::array()))
    {
        std::vector<sn::SurfacePoint>& line = b.lines.emplace_back();
        for (const json& p : l)
            if (p.is_array() && p.size() >= 6)
                line.push_back(pointFrom(p));
    }
    return b;
}

namespace
{
struct TableKey
{
    size_t bind = 0;
    double reach = 0.0, stiffness = 0.0;
    uint64_t topology = 0;
    size_t points = 0;
    bool operator==(const TableKey&) const = default;
};

std::mutex gTablesMutex;
/** Most recent first. */
std::vector<std::pair<TableKey, std::shared_future<InfluencePtr>>> gTables;
constexpr size_t kKeptTables = 6;
} // namespace

std::shared_future<InfluencePtr> influenceTable(const geo::Mesh& topology, const std::string& bindText, double reach, double stiffness)
{
    LILA_ZONE();
    const TableKey key{std::hash<std::string>{}(bindText), reach, stiffness, topology.topologyId(), topology.pointCapacity()};
    std::lock_guard lock(gTablesMutex);
    for (size_t i = 0; i < gTables.size(); ++i)
        if (gTables[i].first == key)
        {
            std::rotate(gTables.begin(), gTables.begin() + std::ptrdiff_t(i), gTables.begin() + std::ptrdiff_t(i) + 1);
            return gTables.front().second;
        }
    auto mesh = std::make_shared<const geo::Mesh>(topology);
    std::shared_future<InfluencePtr> built = std::async(std::launch::async, [mesh, bindText, reach, stiffness]() -> InfluencePtr {
                                                 LILA_ZONE_N("softNet table build");
                                                 const MeshBind b = MeshBind::fromJson(bindText);
                                                 if (b.points != mesh->pointCapacity())
                                                     return std::make_shared<const sn::InfluenceTable>();
                                                 return std::make_shared<const sn::InfluenceTable>(sn::buildInfluence(
                                                     *mesh, b.samples, b.runs, b.knots, b.tangentRest.size(), b.edge, reach > 0.0 ? reach : b.autoReach, stiffness));
                                             }).share();
    gTables.insert(gTables.begin(), {key, built});
    if (gTables.size() > kKeptTables)
        gTables.resize(kKeptTables);
    return built;
}

std::vector<glm::dvec3> dofMoves(const MeshBind& b, std::span<const glm::dmat4> frames, const std::map<int, glm::dmat4>& knots,
                                 const std::map<int, glm::dmat4>& tangents, const glm::dmat4& toMesh)
{
    std::vector<glm::dvec3> moves(sn::dofCount(b.knots, b.tangentRest.size()), glm::dvec3(0.0));
    auto frameOf = [&](size_t k) { return toMesh * (k < frames.size() ? frames[k] : glm::dmat4(1.0)); };
    auto controlOf = [&](int k) {
        const auto it = knots.find(k);
        return it != knots.end() ? it->second : glm::dmat4(1.0);
    };
    for (size_t k = 0; k < b.knots; ++k)
    {
        const glm::dmat4 f = frameOf(k);
        moves[k] = glm::dvec3((f * controlOf(int(k)))[3]) - glm::dvec3(f[3]);
    }
    for (size_t t = 0; t < b.tangentRest.size(); ++t)
    {
        const int k = b.tangentKnot[t];
        if (k < 0 || size_t(k) >= b.knots)
            continue;
        const glm::dmat4 f = frameOf(size_t(k));
        const auto it = tangents.find(int(t));
        const glm::dvec3 offset = it != tangents.end() ? glm::dvec3(it->second[3]) : glm::dvec3(0.0);
        const glm::dvec3 rest = b.tangentRest[t];
        moves[b.knots + t] = glm::dvec3(f * controlOf(k) * glm::dvec4(rest + offset, 1.0)) - glm::dvec3(f * glm::dvec4(rest, 1.0));
    }
    return moves;
}

namespace
{
// ---------------------------------------------------------------------------------------------------------------------
// Nodes

class KnotsNode;
std::mutex gHubsMutex;
std::set<KnotsNode*> gHubs;

/**
 * The net's knots group (softNetKnots): reads the surface under the net and gives every knot its frame there, in the
 * body's object space. It stands where the body mesh stands, its world matrix kept equal to the mesh's after every
 * evaluation (syncSpaces), so the knots under it follow the mesh's transform without anything on the mesh feeding the net.
 */
class KnotsNode final : public Transform
{
public:
    KnotsNode()
    {
        std::lock_guard lock(gHubsMutex);
        gHubs.insert(this);
    }
    ~KnotsNode() override
    {
        std::lock_guard lock(gHubsMutex);
        gHubs.erase(this);
    }

    void compute(const EvalContext& ctx) override
    {
        LILA_ZONE();
        const NodeType& t = type();
        {
            std::lock_guard lock(mMutex);
            if (mWorlds.empty())
                mWorlds = {as<glm::dmat4>(t.attrIndex("meshWorld"))};
            setOutput(kParentMatrix, mWorlds.front());
            if (!mSpaces || mSpaces->m != mWorlds)
            {
                auto s = std::make_shared<Matrices>();
                s->m = mWorlds;
                mSpaces = s;
            }
        }
        Transform::compute(ctx);
        setOutput(t.attrIndex("spaces"), DataPtr(mSpaces));

        const std::string& text = as<std::string>(t.attrIndex("binding"));
        if (text != mText)
        {
            mText = text;
            mBind = NetBind::fromJson(text);
        }
        const MeshPtr in = as<MeshPtr>(t.attrIndex("inMesh"));
        std::vector<glm::dmat4> frames;
        if (in)
        {
            const std::vector<glm::dvec3> pts = pointsOf(*in);
            bool fits = true;
            for (const sn::KnotRig& k : mBind.knots)
                for (uint32_t p : k.at.points)
                    fits = fits && p < pts.size();
            if (fits)
                frames = sn::knotFrames(mBind.knots, pts);
        }
        // The same data while the frames are unchanged, so the deformers reading them see no new input.
        if (!mFrames || mFrames->m != frames)
        {
            auto f = std::make_shared<Matrices>();
            f->m = std::move(frames);
            mFrames = f;
        }
        setOutput(t.attrIndex("frames"), DataPtr(mFrames));
    }

    /** Sets the bound meshes' world matrices (the body's first); true when they changed. */
    bool setWorlds(std::vector<glm::dmat4> worlds)
    {
        std::lock_guard lock(mMutex);
        if (worlds == mWorlds)
            return false;
        mWorlds = std::move(worlds);
        return true;
    }

private:
    std::mutex mMutex;
    std::vector<glm::dmat4> mWorlds;
    std::shared_ptr<Matrices> mSpaces;
    std::shared_ptr<Matrices> mFrames;
    std::string mText;
    NetBind mBind;
};

/** A knot control: its parentMatrix is the knots group's times the knot's frame, so translate, rotate and scale are in the frame. */
class KnotNode final : public Locator
{
public:
    void compute(const EvalContext& ctx) override
    {
        const NodeType& t = type();
        const auto* p = std::get_if<DataPtr>(&value(t.attrIndex("frames")));
        const auto* f = p && *p ? dynamic_cast<const Matrices*>(p->get()) : nullptr;
        const int64_t i = as<int64_t>(t.attrIndex("index"));
        if (f && i >= 0 && size_t(i) < f->m.size())
            setOutput(kParentMatrix, as<glm::dmat4>(kParentMatrix) * f->m[size_t(i)]);
        Transform::compute(ctx);
    }
};

class SoftNetNode final : public DeformerNode
{
public:
    bool followsJoints() const override { return false; }

    void deform(const geo::Mesh& in, std::vector<glm::dvec3>& points) const override
    {
        LILA_ZONE();
        const NodeType& t = type();
        // Controls at rest leave the surface exactly as it came in.
        std::map<int, glm::dmat4> knots, tangents;
        for (const auto& [i, m] : elementMatrices(*this, t.attrIndex("knotMatrix")))
            if (!nearIdentity(m))
                knots[i] = m;
        for (const auto& [i, m] : elementMatrices(*this, t.attrIndex("tangentMatrix")))
            if (!nearIdentity(m))
                tangents[i] = m;
        std::shared_future<InfluencePtr> pending;
        std::shared_ptr<const MeshBind> bind;
        {
            std::lock_guard lock(mMutex);
            const std::string& text = as<std::string>(t.attrIndex("binding"));
            const double reach = as<double>(t.attrIndex("reach")), stiffness = as<double>(t.attrIndex("stiffness"));
            if (text != mText || reach != mReach || stiffness != mStiffness || in.topologyId() != mTopology)
            {
                mText = text;
                mReach = reach;
                mStiffness = stiffness;
                mTopology = in.topologyId();
                mBind = std::make_shared<const MeshBind>(MeshBind::fromJson(text));
                mTable = mBind->points == points.size() ? influenceTable(in, text, reach, stiffness) : std::shared_future<InfluencePtr>();
            }
            if (mBind->points != points.size())
                throw std::runtime_error("the mesh changed since the Soft Net was bound; run softNetRebind");
            pending = mTable;
            bind = mBind;
        }
        // At rest the table is only started, so it builds while nothing waits on it.
        if (knots.empty() && tangents.empty())
            return;
        if (!pending.valid())
            return;
        InfluencePtr table;
        {
            LILA_ZONE_N("softNet wait for table");
            table = pending.get();
        }
        if (!table || table->points() == 0)
            return;
        const Matrices* frames = matricesOf(*this, t.attrIndex("frames"));
        if (!frames || frames->m.size() < bind->knots)
            return;
        // A mesh other than the body has the body's frames carried into its own object space.
        glm::dmat4 toMesh(1.0);
        const int64_t slot = as<int64_t>(t.attrIndex("meshIndex"));
        if (const Matrices* spaces = matricesOf(*this, t.attrIndex("spaces")); slot > 0 && spaces && size_t(slot) < spaces->m.size())
            toMesh = glm::inverse(spaces->m[size_t(slot)]) * spaces->m[0];
        const std::vector<glm::dvec3> moves = dofMoves(*bind, frames->m, knots, tangents, toMesh);
        std::vector<glm::dvec3> moved(points.size(), glm::dvec3(0.0));
        table->apply(moves, moved);
        for (size_t i = 0; i < points.size(); ++i)
            points[i] += moved[i];
    }

private:
    mutable std::mutex mMutex;
    mutable std::string mText;
    mutable double mReach = -1.0, mStiffness = -1.0;
    mutable uint64_t mTopology = 0;
    mutable std::shared_ptr<const MeshBind> mBind = std::make_shared<const MeshBind>();
    mutable std::shared_future<InfluencePtr> mTable;
};

/** Keeps every knots group's world matrix on its body mesh's, and the bound meshes' matrices current, after each evaluation. */
void syncSpaces(Graph& g, bool after)
{
    if (!after)
        return;
    std::vector<KnotsNode*> hubs;
    {
        std::lock_guard lock(gHubsMutex);
        hubs.assign(gHubs.begin(), gHubs.end());
    }
    bool changed = false;
    for (KnotsNode* h : hubs)
    {
        if (g.node(h->id()) != h)
            continue;
        const std::vector<NodeId> meshes = netMeshes(g, h->id());
        if (meshes.empty())
            continue;
        std::vector<glm::dmat4> worlds;
        for (NodeId m : meshes)
            worlds.push_back(std::get<glm::dmat4>(g.value({m, Transform::kWorldMatrix, -1})));
        if (h->setWorlds(std::move(worlds)))
        {
            g.touch(h->id());
            changed = true;
        }
    }
    if (changed)
        g.evaluate();
}
} // namespace

std::vector<NodeId> netDeformers(Graph& g, NodeId knotsNode)
{
    std::vector<std::pair<int64_t, NodeId>> found;
    const Node* hub = g.node(knotsNode);
    if (!hub)
        return {};
    for (const PlugRef& d : g.destinations({knotsNode, hub->type().attrIndex("frames"), -1}))
        if (const Node* n = g.node(d.node); n && n->type().name == kSoftNetType)
            found.emplace_back(n->as<int64_t>(n->type().attrIndex("meshIndex")), d.node);
    std::sort(found.begin(), found.end());
    std::vector<NodeId> out;
    for (const auto& [slot, id] : found)
        if (std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    return out;
}

std::vector<NodeId> netMeshes(Graph& g, NodeId knotsNode)
{
    std::vector<NodeId> out;
    for (NodeId d : netDeformers(g, knotsNode))
        if (const NodeId m = modifiers::meshOf(g, d); m != kNoNode)
            out.push_back(m);
    return out;
}

NodeId netOf(Graph& g, NodeId node)
{
    const Node* n = g.node(node);
    if (!n)
        return kNoNode;
    if (n->type().name == kSoftNetKnotsType)
        return node;
    if (n->type().name == kSoftNetKnotType || n->type().name == kSoftNetTangentType)
    {
        for (NodeId p = g.parent(node); p != kNoNode; p = g.parent(p))
            if (g.node(p)->type().name == kSoftNetKnotsType)
                return p;
        return kNoNode;
    }
    if (n->type().name == kSoftNetType)
    {
        if (const auto src = g.source({node, n->type().attrIndex("frames"), -1}))
            return src->node;
        return kNoNode;
    }
    if (n->type().isA("mesh"))
        for (NodeId d : modifiers::stack(g, node))
            if (g.node(d)->type().name == kSoftNetType)
                return netOf(g, d);
    return kNoNode;
}

void registerSoftNetTypes()
{
    if (types::find(kSoftNetType))
        return;
    const uint32_t keyable = AttrInput | AttrStorable | AttrKeyable;
    const uint32_t hiddenArray = AttrInput | AttrHidden | AttrArray;
    deformers::registerDeformer(
        {kSoftNetType, "topology-ring",
         "Soft Net: a Panel Layout's lines as Bezier curves that shape the surface after the skin. Each line sample holds to "
         "the mesh as it is (never cut); move, turn or scale a knot, or pull one of its tangent handles, and the lines through "
         "it follow, the surface following the lines and fading over the reach.",
         {{.name = "reach", .type = PlugType::Float, .defaultValue = 0.0, .flags = keyable,
           .doc = "How far from a line the surface follows it, cm; 0 is a sixth of the mesh's size.", .minValue = 0.0},
          {.name = "stiffness", .type = PlugType::Float, .defaultValue = 1.0, .flags = keyable,
           .doc = "How closely the surface meets the lines against keeping smooth.", .minValue = 0.0},
          {.name = "frames", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden,
           .doc = "The knots' frames on the surface under the net, in the body's object space, from the net's knots group."},
          {.name = "spaces", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden,
           .doc = "The bound meshes' world matrices, body first, from the net's knots group."},
          {.name = "meshIndex", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable | AttrHidden,
           .doc = "This mesh's place in the net: 0 the body the knots ride, others the garments and props bound with it."},
          {.name = "knotMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = hiddenArray,
           .doc = "Each knot control's local matrix: its move, turn and scale in its frame."},
          {.name = "tangentMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = hiddenArray,
           .doc = "Each tangent handle's local matrix: how far it has moved from its rest place in its knot's space."},
          {.name = "binding", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
           .doc = "The bind: every line sample's triangle and weights, its line and place along it, and the tangent handles' rests."}},
         [] { return std::make_unique<SoftNetNode>(); }});

    NodeType hub;
    hub.name = kSoftNetKnotsType;
    hub.icon = "topology-ring";
    hub.base = types::find("transform");
    hub.dag = true;
    hub.doc = "A Soft Net's knots: gives each knot its frame on the surface under the net and stands where the body mesh "
              "stands, so the knots follow the mesh. showAllKnots shows the knots inside lines as well as junctions and ends.";
    hub.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden,
         .doc = "The body's surface under the net, which the knot frames ride."},
        {.name = "binding", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Each knot's place on the surface, the lines through it and the junctions it turns with."},
        {.name = "meshWorld", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The body's world matrix when bound; the live one replaces it at the first evaluation."},
        {.name = "layout", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The Panel Layout the net was made from, which softNetRebind reads again."},
        {.name = "spacing", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Distance between line samples, cm; 0 is twice the layout's."},
        {.name = "maxDistance", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "How far a garment or prop may lie from a line and still be held by it, cm; 0 is half the reach."},
        {.name = "showAllKnots", .type = PlugType::Bool, .defaultValue = false, .flags = AttrInput | AttrStorable | AttrChannelBox,
         .doc = "Show every knot; off shows junctions (3 or more lines) and line ends only."},
        {.name = "frames", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "Each knot's frame on the surface, in the body's object space."},
        {.name = "spaces", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "The bound meshes' world matrices, body first."},
    };
    hub.create = [] { return std::make_unique<KnotsNode>(); };
    types::registerType(std::move(hub));

    NodeType knot;
    knot.name = kSoftNetKnotType;
    knot.icon = "circle-dot";
    knot.base = types::find("locator");
    knot.dag = true;
    knot.doc = "A Soft Net knot control: translate, rotate and scale move the lines through it in its frame on the surface "
               "(z the surface normal, x fitted to the lines).";
    knot.ownAttrs = {
        {.name = "frames", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden, .doc = "The net's knot frames."},
        {.name = "index", .type = PlugType::Int, .defaultValue = int64_t(-1), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "This knot's place in the net."},
        {.name = "junction", .type = PlugType::Int, .defaultValue = int64_t(-1), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The Panel Layout junction this knot sits on; rebinding keeps the knot while the junction lasts."},
        {.name = "degree", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Lines through the knot."},
    };
    knot.create = [] { return std::make_unique<KnotNode>(); };
    types::registerType(std::move(knot));

    NodeType tangent;
    tangent.name = kSoftNetTangentType;
    tangent.icon = "vector-bezier";
    tangent.base = types::find("locator");
    tangent.dag = true;
    tangent.doc = "A Soft Net tangent handle: shapes the line leaving its knot, as a Bezier handle does. At rest it sits a third "
                  "of the way along the line; translate moves it from there.";
    tangent.ownAttrs = {
        {.name = "run", .type = PlugType::Int, .defaultValue = int64_t(-1), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The Panel Layout run whose end this handle shapes."},
        {.name = "end", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "Which end of the run: 0 its start, 1 its end."},
    };
    tangent.create = [] { return std::make_unique<Locator>(); };
    types::registerType(std::move(tangent));

    Graph::addEvaluateHook(syncSpaces);
}

void registerSoftNetCommands()
{
    registerSoftNetBindCommands();
    registerSoftNetPoseCommands();
    registerSoftNetLayoutsCommands();
    registerSoftNetExportCommands();
}
} // namespace lila::layout
