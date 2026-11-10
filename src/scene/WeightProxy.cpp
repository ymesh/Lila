#include "scene/WeightProxy.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Bvh.h"
#include "geo/CrossSection.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "retopo/Retopo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"
#include "scene/Transform.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace lila::scene::weightproxy
{
namespace
{
using rigutil::worldOf;

constexpr const char* kTargetsAttr = "weightProxyFor";
constexpr const char* kTargetsKind = "weightProxyTargets";

/** The render meshes a proxy stands in for: live ids, or names read from a file until they resolve. */
struct Targets final : Data
{
    mutable std::vector<NodeId> ids;
    std::vector<std::string> names;

    const char* kind() const override { return kTargetsKind; }
    size_t bytes() const override { return 32 + names.size() * 32; }
    json toJson() const override
    {
        json arr = json::array();
        for (size_t k = 0; k < std::max(ids.size(), names.size()); ++k)
        {
            const Node* n = k < ids.size() ? Graph::current().node(ids[k]) : nullptr;
            arr.push_back(n ? n->path() : k < names.size() ? names[k] : std::string());
        }
        return {{"kind", kTargetsKind}, {"meshes", arr}};
    }
    static DataPtr fromJson(const json& j)
    {
        auto t = std::make_shared<Targets>();
        if (j.contains("meshes"))
            for (const json& m : j["meshes"])
                t->names.push_back(m.get<std::string>());
        return t;
    }
    std::vector<NodeId> resolve(Graph& g) const
    {
        std::vector<NodeId> out;
        ids.resize(std::max(ids.size(), names.size()), kNoNode);
        for (size_t k = 0; k < ids.size(); ++k)
        {
            if (ids[k] == kNoNode && k < names.size())
                if (const Node* n = g.find(names[k]))
                    ids[k] = n->id();
            if (g.node(ids[k]))
                out.push_back(ids[k]);
        }
        return out;
    }
};

const Targets* targetsData(Graph& g, NodeId proxy)
{
    const Node* n = g.node(proxy);
    if (!n)
        return nullptr;
    // User attrs live on the node's own copy of its type.
    const int attr = n->type().attrIndex(kTargetsAttr);
    if (attr < 0)
        return nullptr;
    return dynamic_cast<const Targets*>(n->as<DataPtr>(attr).get());
}

/** A skin's undeformed points placed where they were bound. */
geo::Mesh bindWorld(Graph& g, NodeId skin)
{
    MeshPtr m = skin::inputMesh(g, skin);
    if (!m)
        throw CommandError("weight proxy: a skinned mesh has no geometry");
    geo::Mesh w = *m;
    const glm::dmat4 geom = g.node(skin)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix);
    for (geo::Index p = 0; p < w.pointCapacity(); ++p)
        if (w.pointValid(p))
            w.setPosition(p, glm::dvec3(geom * glm::dvec4(m->position(p), 1.0)));
    return w;
}

/** The mesh as weights see it: its bind shape when skinned, else as shown, in world space. */
geo::Mesh worldShape(Graph& g, NodeId mesh)
{
    if (NodeId sk = skin::find(g, mesh); sk != kNoNode)
        return bindWorld(g, sk);
    g.evaluate();
    MeshPtr m = g.node(mesh)->as<MeshPtr>(MeshNode::kOutMesh);
    if (!m)
        throw CommandError("weight proxy: " + g.node(mesh)->path() + " has no geometry");
    geo::Mesh w = *m;
    const glm::dmat4 world = worldOf(*g.node(mesh));
    for (geo::Index p = 0; p < w.pointCapacity(); ++p)
        if (w.pointValid(p))
            w.setPosition(p, glm::dvec3(world * glm::dvec4(m->position(p), 1.0)));
    return w;
}

void append(geo::Mesh& into, const geo::Mesh& m)
{
    std::unordered_map<geo::Index, geo::Index> map;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            map[p] = into.addPoint(m.position(p));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<geo::Index> pts;
        for (geo::Index p : m.facePoints(f))
            pts.push_back(map.at(p));
        into.addFace(pts);
    }
}

/** A joint the proxy binds to, where it was bound, and its parent among them. */
struct Bone
{
    NodeId joint = kNoNode;
    glm::dvec3 pos{0.0};
    int parent = -1;
    std::vector<int> children;
};

std::vector<Bone> bonesOf(Graph& g, const std::vector<NodeId>& joints, const std::unordered_map<NodeId, glm::dmat4>& bindPre)
{
    std::vector<Bone> bones;
    std::unordered_map<NodeId, int> index;
    for (NodeId j : joints)
    {
        Bone b;
        b.joint = j;
        auto it = bindPre.find(j);
        b.pos = it != bindPre.end() ? glm::dvec3(glm::inverse(it->second)[3]) : glm::dvec3(worldOf(*g.node(j))[3]);
        index[j] = int(bones.size());
        bones.push_back(b);
    }
    for (Bone& b : bones)
        for (NodeId p = g.parent(b.joint); p != kNoNode; p = g.parent(p))
            if (auto it = index.find(p); it != index.end())
            {
                b.parent = it->second;
                break;
            }
    for (size_t k = 0; k < bones.size(); ++k)
        if (bones[k].parent >= 0)
            bones[size_t(bones[k].parent)].children.push_back(int(k));
    return bones;
}

double segmentDistance(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b, glm::dvec3* closest = nullptr)
{
    const glm::dvec3 d = b - a;
    const double len2 = glm::dot(d, d);
    const double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, d) / len2, 0.0, 1.0) : 0.0;
    const glm::dvec3 q = a + d * t;
    if (closest)
        *closest = q;
    return glm::length(p - q);
}

/** The bone (child index, from its parent) nearest p, and the closest point on it; -1 without bones. */
int nearestBone(const std::vector<Bone>& bones, const glm::dvec3& p, glm::dvec3* closest = nullptr)
{
    int best = -1;
    double bestD = 1e300;
    for (size_t k = 0; k < bones.size(); ++k)
    {
        if (bones[k].parent < 0)
            continue;
        glm::dvec3 q;
        const double d = segmentDistance(p, bones[size_t(bones[k].parent)].pos, bones[k].pos, &q);
        if (d < bestD)
        {
            bestD = d;
            best = int(k);
            if (closest)
                *closest = q;
        }
    }
    if (best < 0 && !bones.empty())
    {
        // A lone joint: everything is nearest to it.
        if (closest)
            *closest = bones[0].pos;
        return 0;
    }
    return best;
}

/**
 * Keeps the faces that look away from the nearest bone: of a thick shell (a coat's outside and its lining) only the
 * outside stays, so both layers later take weights from one sheet.
 */
void keepOuterSide(geo::Mesh& m, const std::vector<Bone>& bones)
{
    std::vector<geo::Index> drop;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        glm::dvec3 q;
        const glm::dvec3 c = m.faceCentroid(f);
        if (nearestBone(bones, c, &q) < 0)
            continue;
        // Clearly facing away: a hem or cuff edge, side-on to the bone, goes with the inside.
        const glm::dvec3 out = c - q;
        if (glm::length(out) < 1e-9 || glm::dot(m.faceNormal(f), glm::normalize(out)) < 0.2)
            drop.push_back(f);
    }
    if (drop.size() < size_t(m.numFaces()))
        geo::deleteFaces(m, drop);
    m.compact();
}

/** A cut plane and the bones whose part of the surface it may cut. */
struct LoopPlane
{
    glm::dvec3 origin, normal;
    std::vector<int> bones;
};

std::vector<LoopPlane> loopPlanes(const std::vector<Bone>& bones, int between)
{
    std::vector<LoopPlane> out;
    for (size_t k = 0; k < bones.size(); ++k)
    {
        const Bone& b = bones[k];
        const bool hasParent = b.parent >= 0;
        if (hasParent)
        {
            const glm::dvec3 a = bones[size_t(b.parent)].pos;
            const glm::dvec3 d = b.pos - a;
            if (glm::length(d) < 1e-9)
                continue;
            for (int s = 1; s <= between; ++s)
                out.push_back({a + d * (double(s) / double(between + 1)), glm::normalize(d), {int(k)}});
        }
        // The joint's own loop, on the bisector of the bones meeting there. Branch joints are left to the poles.
        if (b.children.size() > 1)
            continue;
        glm::dvec3 n(0.0);
        LoopPlane lp{b.pos, {}, {}};
        if (hasParent)
        {
            n += glm::normalize(b.pos - bones[size_t(b.parent)].pos);
            lp.bones.push_back(int(k));
        }
        if (b.children.size() == 1)
        {
            const glm::dvec3 d = bones[size_t(b.children[0])].pos - b.pos;
            if (glm::length(d) > 1e-9)
            {
                n += glm::normalize(d);
                lp.bones.push_back(b.children[0]);
            }
        }
        if (glm::length(n) < 1e-9 || lp.bones.empty())
            continue;
        lp.normal = glm::normalize(n);
        out.push_back(lp);
    }
    return out;
}

/** The parts of each plane's cut through the sheet that lie on its bones, as polylines the remesh lands loops on. */
std::vector<std::vector<glm::dvec3>> loopGuides(const geo::Mesh& sheet, const std::vector<Bone>& bones, int between)
{
    std::vector<std::vector<glm::dvec3>> guides;
    const geo::SectionMesh sm{&sheet, glm::dmat4(1.0)};
    for (const LoopPlane& lp : loopPlanes(bones, between))
        for (const geo::SectionChain& chain : geo::sectionChains(std::span(&sm, 1), lp.origin, lp.normal))
        {
            std::vector<glm::dvec3> run;
            auto flush = [&] {
                if (run.size() >= 3)
                    guides.push_back(run);
                run.clear();
            };
            for (const glm::dvec3& p : chain.points)
            {
                const int nb = nearestBone(bones, p);
                if (std::find(lp.bones.begin(), lp.bones.end(), nb) != lp.bones.end())
                    run.push_back(p);
                else
                    flush();
            }
            if (chain.closed && run.size() == chain.points.size() && !run.empty())
                run.push_back(run.front());
            flush();
        }
    return guides;
}

/** Median bone length over the loops it carries: the edge length that puts loopsPerBone loops on a bone. */
double edgeLengthFor(const std::vector<Bone>& bones, int between, double fallback)
{
    std::vector<double> lens;
    for (const Bone& b : bones)
        if (b.parent >= 0)
            lens.push_back(glm::length(b.pos - bones[size_t(b.parent)].pos) / double(between + 1));
    if (lens.empty())
        return fallback;
    std::nth_element(lens.begin(), lens.begin() + lens.size() / 2, lens.end());
    return lens[lens.size() / 2];
}

/** Median length around the sheet at each bone's middle, counting only the part on that bone; 0 when none is cut. */
double medianGirth(const geo::Mesh& sheet, const std::vector<Bone>& bones)
{
    std::vector<double> girths;
    const geo::SectionMesh sm{&sheet, glm::dmat4(1.0)};
    for (size_t k = 0; k < bones.size(); ++k)
    {
        if (bones[k].parent < 0)
            continue;
        const glm::dvec3 a = bones[size_t(bones[k].parent)].pos, d = bones[k].pos - a;
        if (glm::length(d) < 1e-9)
            continue;
        double girth = 0.0;
        for (const geo::SectionChain& chain : geo::sectionChains(std::span(&sm, 1), a + d * 0.5, glm::normalize(d)))
            for (size_t i = 1; i < chain.points.size(); ++i)
                if (nearestBone(bones, chain.points[i]) == int(k) && nearestBone(bones, chain.points[i - 1]) == int(k))
                    girth += glm::length(chain.points[i] - chain.points[i - 1]);
        if (girth > 0.0)
            girths.push_back(girth);
    }
    if (girths.empty())
        return 0.0;
    std::nth_element(girths.begin(), girths.begin() + girths.size() / 2, girths.end());
    return girths[girths.size() / 2];
}

Node& meshArg(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

NodeId proxyArg(Graph& g, const Args& a, const char* cmd)
{
    auto objs = a.objects();
    if (objs.empty())
        throw CommandError(std::string(cmd) + ": give a weight proxy or one of its render meshes");
    Node& n = meshArg(g, objs.back(), cmd);
    if (targetsData(g, n.id()))
        return n.id();
    if (NodeId p = proxyOf(g, n.id()); p != kNoNode)
        return p;
    throw CommandError(std::string(cmd) + ": " + n.path() + " is not a weight proxy and has none");
}

/** The mesh's skin, binding it to the proxy's joints (with the proxy's bind) when it has none. */
NodeId ensureSkin(Graph& g, NodeId mesh, NodeId like)
{
    if (NodeId sk = skin::find(g, mesh); sk != kNoNode)
        return sk;
    json args = json::array();
    for (const skin::Influence& i : skin::influences(g, like))
        if (i.joint != kNoNode)
            args.push_back(g.node(i.joint)->path());
    args.push_back(g.node(mesh)->path());
    ops::execute("skinCluster", {{"args", args}, {"bindTo", "selected"},
                                 {"maxInfluences", g.node(like)->as<int64_t>(SkinClusterNode::kMaxInfluences)}});
    return skin::find(g, mesh);
}
} // namespace

std::vector<NodeId> targetsOf(Graph& g, NodeId proxy)
{
    const Targets* t = targetsData(g, proxy);
    return t ? t->resolve(g) : std::vector<NodeId>{};
}

NodeId proxyOf(Graph& g, NodeId mesh)
{
    for (NodeId id : g.nodes())
        if (const Targets* t = targetsData(g, id))
        {
            const auto ids = t->resolve(g);
            if (std::find(ids.begin(), ids.end(), mesh) != ids.end())
                return id;
        }
    return kNoNode;
}

size_t transfer(Graph& g, const std::vector<NodeId>& fromSkins, NodeId toSkin)
{
    LILA_ZONE();

    // Joints first: adding an influence changes the destination's influence list.
    struct Source
    {
        geo::Mesh world;
        std::unique_ptr<geo::MeshBvh> bvh;
        WeightTable table;
        std::unordered_map<uint32_t, uint32_t> remap;
    };
    std::vector<Source> sources;
    for (NodeId from : fromSkins)
    {
        Source s;
        for (const skin::Influence& i : skin::influences(g, from))
        {
            if (i.joint == kNoNode)
                continue;
            const bool had = skin::indexOf(skin::influences(g, toSkin), i.name) >= 0;
            const uint32_t to = skin::addInfluence(g, toSkin, i.joint);
            if (!had)
                for (const skin::Influence& d : skin::influences(g, toSkin))
                    if (d.index == to)
                        g.setValue({d.link, SkinInfluenceNode::kBindPreMatrix}, g.node(i.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix));
            s.remap[i.index] = to;
        }
        s.world = bindWorld(g, from);
        s.bvh = std::make_unique<geo::MeshBvh>(s.world);
        s.table = skin::table(g, from);
        sources.push_back(std::move(s));
    }
    g.evaluate();
    const geo::Mesh dst = bindWorld(g, toSkin);
    skin::WeightEdit edit(g, toSkin);
    WeightTable& t = edit.table();
    const auto& locked = edit.locked();
    auto isLocked = [&](uint32_t i) { return i < locked.size() && locked[i]; };
    std::vector<std::vector<WeightTable::Entry>> result(t.points.size());
    std::vector<std::vector<uint32_t>> autos(t.points.size());
    std::vector<char> written(t.points.size(), 0);
    tbb::parallel_for(geo::Index(0), std::min<geo::Index>(dst.pointCapacity(), geo::Index(t.points.size())), [&](geo::Index p) {
        if (!dst.pointValid(p))
            return;
        const Source* best = nullptr;
        geo::BvhClosest bestHit;
        double bestD = 1e300;
        for (const Source& s : sources)
        {
            geo::BvhClosest hit;
            if (!s.bvh->closestPoint(dst.position(p), hit))
                continue;
            if (const double d = glm::length(hit.point - dst.position(p)); d < bestD)
            {
                bestD = d;
                best = &s;
                bestHit = hit;
            }
        }
        if (!best)
            return;
        std::unordered_map<uint32_t, float> acc;
        std::unordered_set<uint32_t> automatic;
        const auto& tris = best->bvh->triangles();
        for (int k = 0; k < 3; ++k)
        {
            const geo::Index sp = best->world.cornerPoint(tris.corners[size_t(bestHit.triangle) * 3 + k]);
            if (sp >= best->table.points.size())
                continue;
            for (auto& [i, w] : best->table.points[sp])
                if (auto it = best->remap.find(i); it != best->remap.end() && !isLocked(it->second))
                {
                    acc[it->second] += w * float(bestHit.bary[k]);
                    if (bestHit.bary[k] > 1e-6 && best->table.isAutomatic(sp, i))
                        automatic.insert(it->second);
                }
        }
        float lockedSum = 0.0f, free = 0.0f;
        auto& out = result[p];
        for (auto& [i, w] : t.points[p])
            if (isLocked(i))
            {
                out.push_back({i, w});
                lockedSum += w;
            }
        for (auto& [i, w] : acc)
            free += w;
        if (free <= 0.0f)
        {
            out = t.points[p];
            return;
        }
        const float s = std::max(0.0f, 1.0f - lockedSum) / free;
        for (auto& [i, w] : acc)
            if (w * s > 0.0f)
                out.push_back({i, w * s});
        std::sort(out.begin(), out.end());
        autos[p].assign(automatic.begin(), automatic.end());
        written[p] = 1;
    });
    size_t count = 0;
    for (size_t p = 0; p < t.points.size(); ++p)
    {
        if (!written[p])
            continue;
        t.points[p] = std::move(result[p]);
        t.markPointAuthored(p);
        for (uint32_t i : autos[p])
            t.markAutomatic(p, i);
        t.prune(p, edit.maxInfluences(), 1e-4f, locked);
        ++count;
    }
    edit.commit();
    return count;
}

NodeId create(Graph& g, const std::vector<NodeId>& meshes, const Options& o)
{
    LILA_ZONE();

    if (meshes.empty())
        throw CommandError("createWeightProxy: give the render meshes");
    // Joints and where they were bound: from the render meshes' skins, else the joints given.
    std::vector<NodeId> joints;
    std::unordered_map<NodeId, glm::dmat4> bindPre;
    std::vector<NodeId> skins;
    for (NodeId m : meshes)
        if (NodeId sk = skin::find(g, m); sk != kNoNode)
        {
            skins.push_back(sk);
            for (const skin::Influence& i : skin::influences(g, sk))
                if (i.joint != kNoNode && !bindPre.contains(i.joint))
                {
                    joints.push_back(i.joint);
                    bindPre[i.joint] = g.node(i.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix);
                }
        }
    for (NodeId j : o.joints)
        if (std::find(joints.begin(), joints.end(), j) == joints.end())
            joints.push_back(j);
    if (joints.empty())
        throw CommandError("createWeightProxy: the meshes aren't skinned; give joints= to bind the proxy to");

    geo::Mesh fused;
    for (NodeId m : meshes)
        append(fused, worldShape(g, m));
    const std::vector<Bone> bones = bonesOf(g, joints, bindPre);

    // Fused into one closed solid, which fills buttons, cuffs and the gap between a coat and its lining.
    retopo::VoxelOptions vo;
    vo.resolution = std::clamp(o.resolution, 16, 1024);
    vo.project = false;
    geo::Mesh sheet = retopo::voxelRemesh(fused, vo);
    keepOuterSide(sheet, bones);
    if (sheet.numFaces() == 0)
        throw CommandError("createWeightProxy: nothing of the meshes faces away from the joints; check the joints");

    retopo::QuadRemeshOptions qo;
    qo.targetEdgeLength = edgeLengthFor(bones, std::max(0, o.loopsPerBone), glm::length(sheet.bounds().size()) / 40.0);
    if (const double girth = medianGirth(sheet, bones); girth > 0.0 && o.edgesAround > 0)
        // A little under the girth's share: the remesh rounds a loop's count down as often as up.
        qo.targetEdgeLength = std::min(qo.targetEdgeLength, 0.85 * girth / double(o.edgesAround));
    qo.adaptivity = 0.0;
    qo.jointDensity = 0.0;
    qo.featureGuides = loopGuides(sheet, bones, std::max(0, o.loopsPerBone));
    // Off the UI thread, cancellable, and bounded: a shape the remesh struggles with fails with a reason, not a freeze.
    qo.timeout = 120.0;
    retopo::QuadRemeshResult r;
    {
        progress::Scope scope("Weight proxy", 1.0, progress::Cancellable);
        progress::runOnWorker([&] { r = retopo::quadRemesh(sheet, qo); });
    }
    if (!r.ok())
        throw CommandError("createWeightProxy: " + r.error + (r.error.rfind("gave up", 0) == 0 ? "; try a lower resolution or fewer loops" : ""));
    for (const std::string& note : r.notes)
        log::warn("createWeightProxy: {}", note);

    // Beside the first render mesh in the outliner.
    const NodeId first = meshes.front();
    const NodeId parent = g.parent(first);
    const std::string name = o.name.empty() ? g.node(first)->name() + "_weightProxy" : o.name;
    Node* proxy = g.createNode(*types::find("mesh"), name, parent, true);
    const NodeId id = proxy->id();
    const auto& siblings = parent != kNoNode ? g.children(parent) : g.roots();
    const auto at = std::find(siblings.begin(), siblings.end(), first);
    g.setParent(id, parent, false, at != siblings.end() ? size_t(at - siblings.begin()) + 1 : SIZE_MAX);
    // Points are in world space at the bind, so the proxy stays at the origin whatever its parent does.
    Transform::setLocalMatrix(g, id, glm::inverse(parent != kNoNode ? worldOf(*g.node(parent)) : glm::dmat4(1.0)));
    g.setValue({id, MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
    auto targets = std::make_shared<Targets>();
    for (NodeId m : meshes)
    {
        targets->ids.push_back(m);
        targets->names.push_back(g.node(m)->path());
    }
    const int attr = g.addAttr(id, {.name = kTargetsAttr, .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrHidden,
                                    .doc = "The render meshes this weight proxy stands in for."});
    g.setValue({id, attr, -1}, DataPtr(targets));

    json args = json::array();
    for (NodeId j : joints)
        args.push_back(g.node(j)->path());
    args.push_back(g.node(id)->path());
    const int64_t maxInf = skins.empty() ? 4 : g.node(skins.front())->as<int64_t>(SkinClusterNode::kMaxInfluences);
    ops::execute("skinCluster", {{"args", args}, {"bindTo", "selected"}, {"maxInfluences", maxInf}, {"name", name + "Skin"}});
    const NodeId proxySkin = skin::find(g, id);
    g.setValue({proxySkin, SkinClusterNode::kGeomMatrix}, worldOf(*g.node(id)));
    for (const skin::Influence& i : skin::influences(g, proxySkin))
        if (auto it = bindPre.find(i.joint); it != bindPre.end())
            g.setValue({i.link, SkinInfluenceNode::kBindPreMatrix}, it->second);
    if (!skins.empty())
        transfer(g, skins, proxySkin);
    skin::reportAutomatic(g);
    log::info("createWeightProxy {}: {} faces, {} loop guides, edge {:.2f} cm, layout {}", name, g.node(id)->as<MeshPtr>(MeshNode::kInMesh)->numFaces(),
              qo.featureGuides.size(), qo.targetEdgeLength, r.layout);
    return id;
}

void registerCommands()
{
    data::registerKind(kTargetsKind, &Targets::fromJson);
    using T = ArgType;

    ops::registerCommand({
        .name = "createWeightProxy",
        .doc = "Make a light, single-sided mesh to paint weights on in place of the render meshes (a coat with its lining, "
               "buttons and cuffs). Its edge loops sit at the joints with loopsPerBone more between each pair, it is bound to "
               "the same joints and takes their weights, and it goes beside the first mesh. Push its weights back with "
               "pushProxyWeights. Returns the proxy.",
        .positionalDoc = "meshes...",
        .flags = {
            {"loopsPerBone", "lpb", T::Int, 3, "Edge loops between each joint's loop and the next, at least.", {}, {}, 0.0, 20.0},
            {"edgesAround", "ea", T::Int, 8, "Edges around a limb, at least; more makes the loops closer too.", {}, {}, 0.0, 64.0},
            {"resolution", "res", T::Int, 128, "Voxels along the longest side when fusing the meshes; lower fills more detail.", {}, {}, 16.0, 1024.0},
            {"joints", "j", T::List, nullptr, "Joints to bind to when the meshes aren't skinned."},
            {"name", "n", T::String, nullptr, "Name of the proxy."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> meshes;
            Options o;
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (n && rig::isJoint(*n))
                    o.joints.push_back(n->id());
                else
                    meshes.push_back(meshArg(g, name, "createWeightProxy").id());
            }
            if (a.has("joints"))
                for (const json& j : a.flags["joints"])
                {
                    Node* n = g.find(j.get<std::string>());
                    if (!n || !rig::isJoint(*n))
                        throw CommandError("createWeightProxy: " + j.get<std::string>() + " is not a joint");
                    o.joints.push_back(n->id());
                }
            o.loopsPerBone = a.get<int>("loopsPerBone", 3);
            o.edgesAround = a.get<int>("edgesAround", 8);
            o.resolution = a.get<int>("resolution", 128);
            o.name = a.get<std::string>("name", "");
            return g.node(create(g, meshes, o))->path();
        },
    });

    ops::registerCommand({
        .name = "pushProxyWeights",
        .doc = "Copy a weight proxy's weights onto its render meshes: each point takes the weights of the closest point on the "
               "proxy, so a coat's outside and lining get the same. Locked joints on the render meshes keep theirs. Give the "
               "proxy or one of its meshes. Returns the points written per mesh.",
        .positionalDoc = "proxy",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const NodeId proxy = proxyArg(g, a, "pushProxyWeights");
            const NodeId proxySkin = skin::find(g, proxy);
            if (proxySkin == kNoNode)
                throw CommandError("pushProxyWeights: the proxy isn't skinned");
            json out = json::object();
            for (NodeId m : targetsOf(g, proxy))
                out[g.node(m)->path()] = transfer(g, {proxySkin}, ensureSkin(g, m, proxySkin));
            skin::reportAutomatic(g);
            return out;
        },
    });

    ops::registerCommand({
        .name = "pullProxyWeights",
        .doc = "Copy the render meshes' weights onto their weight proxy, each proxy point from the closest render surface. "
               "Locked joints on the proxy keep theirs. Returns the points written.",
        .positionalDoc = "proxy",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const NodeId proxy = proxyArg(g, a, "pullProxyWeights");
            std::vector<NodeId> skins;
            for (NodeId m : targetsOf(g, proxy))
                if (NodeId sk = skin::find(g, m); sk != kNoNode)
                    skins.push_back(sk);
            if (skins.empty())
                throw CommandError("pullProxyWeights: none of the render meshes is skinned");
            const NodeId proxySkin = skin::find(g, proxy);
            if (proxySkin == kNoNode)
                throw CommandError("pullProxyWeights: the proxy isn't skinned");
            const size_t n = transfer(g, skins, proxySkin);
            skin::reportAutomatic(g);
            return n;
        },
    });

    ops::registerCommand({
        .name = "weightProxyInfo",
        .doc = "A weight proxy's render meshes, or a render mesh's proxy: {proxy, meshes}; null when neither.",
        .positionalDoc = "mesh",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("weightProxyInfo: give a mesh");
            Node& n = meshArg(g, objs.back(), "weightProxyInfo");
            NodeId proxy = targetsData(g, n.id()) ? n.id() : proxyOf(g, n.id());
            if (proxy == kNoNode)
                return nullptr;
            json meshes = json::array();
            for (NodeId m : targetsOf(g, proxy))
                meshes.push_back(g.node(m)->path());
            return {{"proxy", g.node(proxy)->path()}, {"meshes", meshes}};
        },
    });
}
} // namespace lila::scene::weightproxy
