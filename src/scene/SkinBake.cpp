#include "scene/SkinBake.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "deform/Ssdr.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Anim.h"
#include "scene/Deformers.h"
#include "scene/Drivers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Morphs.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/RotationCurve.h"
#include "scene/Simulation.h"
#include "scene/SkinMethods.h"
#include "scene/Take.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace lila::scene::skinbake
{
using namespace rigutil;

namespace
{
/** Influence ids of fitted helper bones, until applying gives them real ones. */
constexpr uint32_t kHelperBase = 1u << 30;

uint64_t splitmix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/** Seeded uniform in [-1, 1]. */
double unit(uint64_t seed, uint64_t a, uint64_t b) { return double(splitmix(seed ^ splitmix(a * 0x100000001b3ull + b)) >> 11) * (2.0 / 9007199254740992.0) - 1.0; }

bool rotateDriven(Graph& g, NodeId j)
{
    for (int c = -1; c < 3; ++c)
        if (g.source({j, Transform::kRotate, c}))
            return true;
    return false;
}

RotateOrder orderOf(const Node& n) { return RotateOrder(n.as<int64_t>(Transform::kRotateOrder)); }

/** The joint's rotate turned by degrees about its own local axis. */
glm::dvec3 turned(const Node& j, const glm::dvec3& rest, int axis, double degrees)
{
    glm::dvec3 a(0.0);
    a[axis] = 1.0;
    const glm::dquat q = anim::eulerToQuat(rest, orderOf(j)) * glm::angleAxis(glm::radians(degrees), a);
    return eulerNear(glm::mat3_cast(q), orderOf(j), rest);
}

/** Top joint above every given one. */
NodeId skeletonRoot(Graph& g, const std::vector<NodeId>& joints)
{
    NodeId top = kNoNode;
    for (NodeId j : joints)
    {
        NodeId t = j;
        while (g.parent(t) != kNoNode && rig::isJoint(*g.node(g.parent(t))))
            t = g.parent(t);
        if (top == kNoNode)
            top = t;
    }
    return top;
}

/** Deformers after the skin in its mesh's stack, applied first first. */
std::vector<NodeId> deformersAfter(Graph& g, NodeId skin)
{
    std::vector<NodeId> out;
    NodeId mesh = skin::meshOf(g, skin);
    if (mesh == kNoNode)
        return out;
    std::vector<NodeId> st = modifiers::stack(g, mesh);
    auto it = std::find(st.begin(), st.end(), skin);
    if (it == st.end())
        return out;
    for (++it; it != st.end(); ++it)
        if (Node* n = g.node(*it); n && deformers::isDeformer(*n))
            out.push_back(*it);
    return out;
}

bool active(const Node& n)
{
    const auto& m = static_cast<const ModifierNode&>(n);
    return m.as<bool>(m.enabledAttr) && modifiers::exportEnabled(m);
}

/** Everything a fit or a morph bake needs, captured per sample. Positions are the mesh's object space. */
struct Samples
{
    std::vector<glm::dvec3> rest;
    std::vector<std::vector<int>> polygons;
    std::vector<skin::Influence> bones;
    std::map<uint32_t, uint32_t> boneOf; // influence id -> bone
    int frames = 0;
    std::vector<glm::dvec3> targets;        // frames * points
    std::vector<glm::dmat4> palettes;       // frames * bones
    std::vector<std::vector<glm::dvec3>> inputs; // per frame: the skin's input, when kept
    std::vector<double> times;
};

/**
 * Poses the rig and moves time for sampling, with undo off, and puts everything back when it goes: joint rotations,
 * deformers it turned off, the time.
 */
class Sampler
{
public:
    Sampler(Graph& g, NodeId skin, bool keepSimulated) : mGraph(g), mSkin(skin), mTime(g.time())
    {
        mMesh = skin::meshOf(g, skin);
        if (mMesh == kNoNode)
            throw CommandError(g.node(skin)->name() + " feeds no mesh");
        g.evaluate();
        MeshPtr in = skin::inputMesh(g, skin);
        if (!in)
            throw CommandError(g.node(skin)->name() + ": the mesh has no geometry");
        s.rest.assign(in->positions().begin(), in->positions().end());
        for (geo::Index f = 0; f < in->faceCapacity(); ++f)
            if (in->faceValid(f))
            {
                std::vector<int> poly;
                for (geo::Index p : in->facePoints(f))
                    poly.push_back(int(p));
                s.polygons.push_back(std::move(poly));
            }
        for (const skin::Influence& inf : skin::influences(g, skin))
            if (inf.joint != kNoNode)
            {
                s.boneOf[inf.index] = uint32_t(s.bones.size());
                s.bones.push_back(inf);
            }
        if (s.bones.empty())
            throw CommandError(g.node(skin)->name() + " has no joints");

        UndoStack& undo = UndoStack::get();
        mUndo = undo.enabled();
        undo.setEnabled(false);
        if (!keepSimulated)
            for (NodeId d : deformersAfter(g, skin))
                if (Node* n = g.node(d); n && sim::isSimulation(*n) && active(*n))
                {
                    const auto& m = static_cast<const ModifierNode&>(*n);
                    g.setValue({d, m.enabledAttr, -1}, false);
                    mDisabled.push_back({d, m.enabledAttr});
                }
    }

    ~Sampler()
    {
        try
        {
            for (auto& [j, e] : mRotates)
                if (mGraph.node(j))
                    mGraph.setValue({j, Transform::kRotate, -1}, e);
            for (auto& [d, a] : mDisabled)
                if (mGraph.node(d))
                    mGraph.setValue({d, a, -1}, true);
            mGraph.setTime(mTime);
            mGraph.evaluate();
        }
        catch (const std::exception& e)
        {
            log::warn("restoring the rig after sampling: {}", e.what());
        }
        UndoStack::get().setEnabled(mUndo);
    }

    Samples s;
    bool keepInputs = false;

    /** Sets the pose; joints posed before and not in this one go back to their rotation from before sampling. */
    void pose(const Pose& p)
    {
        for (auto& [j, e] : mRotates)
            if (std::none_of(p.begin(), p.end(), [&](const auto& x) { return x.first == j; }))
                mGraph.setValue({j, Transform::kRotate, -1}, e);
        for (auto& [j, e] : p)
        {
            if (!mRotates.contains(j))
                mRotates[j] = mGraph.node(j)->as<glm::dvec3>(Transform::kRotate);
            mGraph.setValue({j, Transform::kRotate, -1}, e);
        }
    }

    void at(double seconds) { mGraph.setTime(seconds); }

    void capture()
    {
        LILA_ZONE();

        mGraph.evaluate();
        MeshPtr out = mGraph.node(mMesh)->as<MeshPtr>(MeshNode::kOutMesh);
        const size_t n = s.rest.size();
        if (!out || out->pointCapacity() != n)
            throw CommandError(mGraph.node(mMesh)->name() + ": something after the skin changes the point count, so it can't be fitted");
        s.targets.insert(s.targets.end(), out->positions().begin(), out->positions().end());
        const glm::dmat4 geom = mGraph.node(mSkin)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix);
        const glm::dmat4 geomInv = glm::inverse(geom);
        for (const skin::Influence& b : s.bones)
        {
            const glm::dmat4 bindPre = mGraph.node(b.link)->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix);
            s.palettes.push_back(geomInv * worldOf(*mGraph.node(b.joint)) * bindPre * geom);
        }
        if (keepInputs)
        {
            MeshPtr in = skin::inputMesh(mGraph, mSkin);
            s.inputs.emplace_back(in->positions().begin(), in->positions().end());
        }
        s.times.push_back(mGraph.time());
        ++s.frames;
    }

private:
    Graph& mGraph;
    NodeId mSkin, mMesh = kNoNode;
    double mTime;
    bool mUndo = true;
    std::map<NodeId, glm::dvec3> mRotates;
    std::vector<std::pair<NodeId, int>> mDisabled;
};

double fps() { return std::max(anim::playback().fps, 1e-6); }

/** Points of the skin's input weighted like the skin, CSR over bones (not influence ids). */
void boneWeights(const Samples& s, const WeightTable& t, std::vector<uint32_t>& offsets, std::vector<uint32_t>& influences, std::vector<float>& weights)
{
    offsets.assign(1, 0);
    influences.clear();
    weights.clear();
    for (size_t p = 0; p < s.rest.size(); ++p)
    {
        if (p < t.points.size())
            for (auto& [i, w] : t.points[p])
                if (auto it = s.boneOf.find(i); it != s.boneOf.end() && w > 0.0f)
                {
                    influences.push_back(it->second);
                    weights.push_back(w);
                }
        offsets.push_back(uint32_t(influences.size()));
    }
}

/** M_p of linear skinning at sample f, normalized like the skin does. */
glm::dmat4 blended(const Samples& s, const WeightTable& t, size_t p, int f)
{
    glm::dmat4 m(0.0);
    double total = 0.0;
    if (p < t.points.size())
        for (auto& [i, w] : t.points[p])
            if (auto it = s.boneOf.find(i); it != s.boneOf.end())
            {
                m += s.palettes[size_t(f) * s.bones.size() + it->second] * double(w);
                total += w;
            }
    return total > 0.0 ? m * (1.0 / total) : glm::dmat4(1.0);
}

NodeId skinOfMesh(Graph& g, const std::string& name, const char* cmd) { return skin::resolve(g, name, cmd); }

RomOptions romFrom(const Args& a)
{
    RomOptions o;
    o.angle = a.get<double>("angle", 45.0);
    o.randomPoses = a.get<int>("randomPoses", 20);
    o.seed = uint64_t(a.get<int>("seed", 1));
    o.axes = a.get<std::string>("axes", "xyz");
    o.sweep = a.get<bool>("sweep", true);
    return o;
}

std::vector<FlagDef> romFlags(int randomDefault)
{
    using T = ArgType;
    return {
        {"angle", "", T::Float, 45.0, "Degrees each joint turns either way about its local axes.", {}, {}, 0.0, 180.0},
        {"axes", "", T::String, "xyz", "Local axes to turn about."},
        {"sweep", "", T::Bool, true, "Poses turning one joint about one axis at a time, each way."},
        {"randomPoses", "", T::Int, randomDefault, "Poses turning every joint at once by a random amount within angle.", {}, {}, 0.0},
        {"seed", "", T::Int, 1, "Seed of the random poses."},
    };
}

/** Joint and keys for a fitted helper bone, under the skeleton's root, keyed at the sampled times. */
NodeId makeHelper(Graph& g, NodeId mesh, NodeId root, const glm::dmat4& geom, const glm::dvec3& pivot, const std::vector<glm::dmat4>& motion,
                  const std::vector<double>& times, int index)
{
    const glm::dmat4 restWorld = glm::translate(glm::dmat4(1.0), glm::dvec3(geom * glm::dvec4(pivot, 1.0)));
    Node* j = g.createNode(*types::find("joint"), g.node(mesh)->name() + "_helper" + std::to_string(index + 1), root);
    const NodeId id = j->id();
    const double saved = g.time();
    glm::dvec3 near(0.0);
    for (size_t k = 0; k < times.size(); ++k)
    {
        g.setTime(times[k]);
        g.evaluate();
        const glm::dmat4 parentWorld = root != kNoNode ? worldOf(*g.node(root)) : glm::dmat4(1.0);
        const glm::dmat4 world = geom * motion[k] * glm::inverse(geom) * restWorld;
        const glm::dmat4 local = glm::inverse(parentWorld) * world;
        near = eulerNear(glm::dmat3(local), orderOf(*j), near);
        for (int c = 0; c < 3; ++c)
            anim::setKey(g, {id, Transform::kTranslate, c}, times[k], local[3][c]);
        anim::setRotationKey(g, {id, Transform::kRotate, -1}, times[k], near);
    }
    g.setTime(saved);
    g.evaluate();
    return id;
}

/** Puts fitted weights on the skin: helpers become keyed joints and influences, the method goes linear. */
json applyFit(Graph& g, NodeId skin, const FitResult& r, bool disableAfter)
{
    NodeId mesh = skin::meshOf(g, skin);
    const glm::dmat4 geom = g.node(skin)->as<glm::dmat4>(SkinClusterNode::kGeomMatrix);
    std::vector<NodeId> joints;
    for (const skin::Influence& i : skin::influences(g, skin))
        if (i.joint != kNoNode)
            joints.push_back(i.joint);
    const NodeId root = skeletonRoot(g, joints);
    std::map<uint32_t, uint32_t> helperId;
    json helpers = json::array();
    for (size_t h = 0; h < r.helperPivots.size(); ++h)
    {
        NodeId j = makeHelper(g, mesh, root, geom, r.helperPivots[h], r.helperTransforms[h], r.times, int(h));
        const uint32_t id = skin::addInfluence(g, skin, j);
        for (const skin::Influence& i : skin::influences(g, skin))
            if (i.joint == j)
                g.setValue({i.link, SkinInfluenceNode::kBindPreMatrix, -1},
                           glm::inverse(glm::translate(glm::dmat4(1.0), glm::dvec3(geom * glm::dvec4(r.helperPivots[h], 1.0)))));
        helperId[kHelperBase + uint32_t(h)] = id;
        helpers.push_back(g.node(j)->path());
    }
    WeightTable t = r.weights;
    for (auto& pt : t.points)
        for (auto& [i, w] : pt)
            if (auto it = helperId.find(i); it != helperId.end())
                i = it->second;
    skin::setWeights(g, skin, t);
    g.setValue({skin, SkinClusterNode::kSkinningMethod, -1}, int64_t(SkinClusterNode::kLinear));
    json disabled = json::array();
    if (disableAfter)
        for (NodeId d : deformersAfter(g, skin))
            if (Node* n = g.node(d); n && active(*n))
            {
                g.setValue({d, static_cast<ModifierNode*>(n)->enabledAttr, -1}, false);
                disabled.push_back(n->name());
            }
    return {{"helpers", helpers}, {"disabled", disabled}};
}
} // namespace

// ------------------------------------------------------------------------------------------------ poses

std::vector<Pose> romPoses(Graph& g, const std::vector<NodeId>& joints, const RomOptions& o, std::vector<PoseInfo>* info)
{
    std::vector<Pose> out;
    if (info)
        info->clear();
    std::vector<int> axes;
    for (char c : o.axes)
        if (c >= 'x' && c <= 'z' && std::find(axes.begin(), axes.end(), c - 'x') == axes.end())
            axes.push_back(c - 'x');
    if (o.sweep)
        for (NodeId j : joints)
        {
            const Node& n = *g.node(j);
            const glm::dvec3 rest = n.as<glm::dvec3>(Transform::kRotate);
            for (int axis : axes)
                for (int sign : {1, -1})
                {
                    out.push_back({{j, turned(n, rest, axis, o.angle * sign)}});
                    if (info)
                        info->push_back({j, axis, sign});
                }
        }
    for (int k = 0; k < o.randomPoses; ++k)
    {
        Pose p;
        for (size_t ji = 0; ji < joints.size(); ++ji)
        {
            const Node& n = *g.node(joints[ji]);
            const glm::dvec3 rest = n.as<glm::dvec3>(Transform::kRotate);
            glm::dvec3 e(0.0);
            for (int axis : axes)
                e[axis] = o.angle * unit(o.seed, uint64_t(k) * 4096 + ji, uint64_t(axis));
            const glm::dquat q = anim::eulerToQuat(rest, orderOf(n)) * anim::eulerToQuat(e, RotateOrder::XYZ);
            p.push_back({joints[ji], eulerNear(glm::mat3_cast(q), orderOf(n), rest)});
        }
        out.push_back(std::move(p));
        if (info)
            info->push_back({});
    }
    return out;
}

std::vector<NodeId> poseableJoints(Graph& g, NodeId skin)
{
    std::vector<NodeId> out;
    for (const skin::Influence& i : skin::influences(g, skin))
        if (i.joint != kNoNode && !rotateDriven(g, i.joint) && !g.isLocked({i.joint, Transform::kRotate, -1}))
            out.push_back(i.joint);
    return out;
}

// ------------------------------------------------------------------------------------------------ fit

FitResult fitSkinWeights(Graph& g, NodeId skin, const FitOptions& o)
{
    LILA_ZONE();

    if (o.helpers > 0 && !o.animation)
        throw CommandError("bakeSkinWeights: helper joints are keyed over the animation, so they need source=\"animation\"");
    const std::vector<NodeId> joints = o.animation ? std::vector<NodeId>{} : poseableJoints(g, skin);
    std::vector<Pose> poses;
    if (!o.animation)
    {
        poses = romPoses(g, joints, o.rom);
        if (poses.empty())
            throw CommandError(g.node(skin)->name() + ": no joint can be posed for a range of motion (every one is keyed or driven); use source=\"animation\"");
    }

    Sampler sampler(g, skin, o.simulated || o.animation);
    if (o.animation)
    {
        const anim::Playback& pb = anim::playback();
        const int64_t start = o.start.value_or(int64_t(std::llround(pb.minTime * fps())));
        const int64_t end = o.end.value_or(int64_t(std::llround(pb.maxTime * fps())));
        for (int64_t f = start; f <= end; f += std::max(1, o.step))
        {
            sampler.at(double(f) / fps());
            sampler.capture();
        }
    }
    else
    {
        sampler.capture();
        for (const Pose& p : poses)
        {
            sampler.pose(p);
            sampler.capture();
        }
    }
    const Samples& s = sampler.s;
    if (s.frames < 2)
        throw CommandError("bakeSkinWeights: needs at least two samples");

    const WeightTable current = skin::table(g, skin);
    deform::SsdrProblem p;
    p.rest = s.rest;
    p.polygons = s.polygons;
    p.frames = s.frames;
    p.targets = s.targets;
    p.bones = int(s.bones.size());
    p.transforms = s.palettes;
    boneWeights(s, current, p.offsets, p.influences, p.weights);
    p.helpers = o.helpers;
    p.maxInfluences = o.maxInfluences > 0 ? o.maxInfluences : int(g.node(skin)->as<int64_t>(SkinClusterNode::kMaxInfluences));
    p.iterations = std::max(1, o.iterations);
    p.smoothness = o.smoothness;

    FitResult r;
    {
        std::vector<double> before = deform::skinningError(p, p.offsets, p.influences, p.weights, p.transforms, p.bones);
        double sum = 0.0;
        for (double e : before)
            sum += e * e;
        r.linearRmse = std::sqrt(sum / std::max<size_t>(1, before.size()));
    }
    deform::SsdrResult fit = deform::ssdrFit(p);
    r.rmse = fit.rmse;
    r.maxError = fit.maxError;
    r.frames = s.frames;
    r.times = s.times;
    r.weights.points.resize(s.rest.size());
    for (size_t pt = 0; pt < s.rest.size(); ++pt)
        for (uint32_t k = fit.offsets[pt]; k < fit.offsets[pt + 1]; ++k)
        {
            const uint32_t b = fit.influences[k];
            const uint32_t id = b < s.bones.size() ? s.bones[b].index : kHelperBase + (b - uint32_t(s.bones.size()));
            r.weights.points[pt].push_back({id, fit.weights[k]});
        }
    r.helperPivots = fit.helperPivots;
    const size_t h = fit.helperPivots.size();
    r.helperTransforms.assign(h, std::vector<glm::dmat4>(size_t(s.frames)));
    for (size_t k = 0; k < h; ++k)
        for (int f = 0; f < s.frames; ++f)
            r.helperTransforms[k][size_t(f)] = fit.helperTransforms[size_t(f) * h + k];
    return r;
}

bool needsRoute(Graph& g, NodeId skin)
{
    const int64_t m = g.node(skin)->as<int64_t>(SkinClusterNode::kSkinningMethod);
    if (m == SkinClusterNode::kCentresOfRotation || m == SkinClusterNode::kDirectDeltaMush)
        return true;
    for (NodeId d : deformersAfter(g, skin))
        if (const auto* n = dynamic_cast<const DeformerNode*>(g.node(d)); active(*g.node(d)) && (!n || n->followsJoints()))
            return true;
    return false;
}

std::optional<WeightTable> exportWeights(Graph& g, NodeId skin, std::string* note)
{
    if (!needsRoute(g, skin))
        return std::nullopt;
    MeshPtr in = skin::inputMesh(g, skin);
    const auto* stored = dynamic_cast<const SkinWeights*>(g.node(skin)->as<DataPtr>(SkinClusterNode::kExportWeights).get());
    if (stored && in && stored->pointCount() == in->pointCapacity())
    {
        if (note)
            *note = "exported the linear weights stored by bakeSkinWeights keep=true";
        return WeightTable::from(stored, in->pointCapacity());
    }
    // Exports run on every save of a mapped asset, so a fit stands until something it read changes.
    struct Cached
    {
        uint64_t key = 0;
        std::vector<std::shared_ptr<const void>> held; // keeps the hashed addresses from being reused
        FitResult fit;
    };
    uint64_t key = sim::hashBytes(&skin, sizeof skin);
    std::vector<std::shared_ptr<const void>> held;
    auto hashNode = [&](NodeId id) {
        const Node& n = *g.node(id);
        for (size_t a = 0; a < n.type().attrs.size(); ++a)
        {
            if (n.type().attrs[a].flags & AttrOutput)
                continue;
            const PlugValue& v = n.value(int(a));
            if (const auto* d = std::get_if<DataPtr>(&v))
            {
                const void* ptr = d->get();
                key = sim::hashBytes(&ptr, sizeof ptr, key);
                held.push_back(*d);
            }
            else if (const auto* m = std::get_if<MeshPtr>(&v))
            {
                const void* ptr = m->get();
                key = sim::hashBytes(&ptr, sizeof ptr, key);
                held.push_back(*m);
            }
            else
            {
                const std::string j = toJson(v).dump();
                key = sim::hashBytes(j.data(), j.size(), key);
            }
        }
    };
    hashNode(skin);
    for (NodeId d : deformersAfter(g, skin))
        hashNode(d);
    for (const skin::Influence& i : skin::influences(g, skin))
    {
        hashNode(i.link);
        if (i.joint != kNoNode)
            hashNode(i.joint);
    }
    static std::map<NodeId, Cached> fits;
    auto it = fits.find(skin);
    if (it == fits.end() || it->second.key != key)
        it = fits.insert_or_assign(skin, Cached{key, std::move(held), fitSkinWeights(g, skin, {})}).first;
    const FitResult& r = it->second.fit;
    if (note)
        *note = std::format("fitted linear weights to its deformation over {} range-of-motion poses (RMS error {:.3g}, worst point {:.3g})", r.frames,
                            r.rmse, r.maxError);
    return r.weights;
}

// ------------------------------------------------------------------------------------------------ commands

void registerCommands()
{
    using T = ArgType;

    std::vector<FlagDef> fitFlags = romFlags(20);
    fitFlags.insert(fitFlags.end(), {
        {"source", "", T::String, "rangeOfMotion", "rangeOfMotion poses the joints; animation samples the scene's animation.", {"rangeOfMotion", "animation"}},
        {"start", "", T::Int, nullptr, "animation: first frame (the playback start by default)."},
        {"end", "", T::Int, nullptr, "animation: last frame (the playback end by default)."},
        {"step", "", T::Int, 1, "animation: frames between samples.", {}, {}, 1.0},
        {"helpers", "", T::Int, 0, "animation: extra joints solved with the weights and keyed over the frames (for jiggle and other motion joints can't follow).", {}, {}, 0.0},
        {"maxInfluences", "mi", T::Int, 0, "Influences per point; 0 uses the skin's.", {}, {}, 0.0},
        {"iterations", "", T::Int, 3, "Solver passes.", {}, {}, 1.0},
        {"keep", "", T::Bool, false, "Store the weights for export only and leave the scene as it is."},
    });
    ops::registerCommand({
        .name = "bakeSkinWeights",
        .doc = "Fit linear skin weights (SSDR, Dem Bones) to everything the mesh's stack does: centres of rotation and direct delta "
               "mush skinning, delta mush, wrap and other deformers after the skin. The skin goes linear and the deformers it replaces "
               "are turned off, so the viewport shows what Unreal will get; keep=true stores the weights for export instead. Returns "
               "{rmse, maxError, linearRmse, frames, helpers, disabled}.",
        .positionalDoc = "mesh",
        .flags = fitFlags,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("bakeSkinWeights: give a skinned mesh");
            const NodeId skin = skinOfMesh(g, objs.back(), "bakeSkinWeights");
            FitOptions o;
            o.animation = a.get<std::string>("source") == "animation";
            o.rom = romFrom(a);
            if (a.has("start"))
                o.start = a.get<int>("start");
            if (a.has("end"))
                o.end = a.get<int>("end");
            o.step = a.get<int>("step");
            o.helpers = std::max(0, a.get<int>("helpers"));
            o.maxInfluences = a.get<int>("maxInfluences");
            o.iterations = a.get<int>("iterations");
            const bool keep = a.get<bool>("keep");
            if (keep && o.helpers > 0)
                throw CommandError("bakeSkinWeights: keep=true can't add helper joints");
            FitResult r = fitSkinWeights(g, skin, o);
            json out = {{"rmse", r.rmse}, {"maxError", r.maxError}, {"linearRmse", r.linearRmse}, {"frames", r.frames}};
            if (keep)
            {
                g.setValue({skin, SkinClusterNode::kExportWeights, -1}, DataPtr(r.weights.pack()));
                out["helpers"] = json::array();
                out["disabled"] = json::array();
                return out;
            }
            out.update(applyFit(g, skin, r, true));
            return out;
        },
    });

    std::vector<FlagDef> morphFlags = {
        {"angle", "", T::Float, 45.0, "Degrees each joint turns either way about its local axes; each pose gets a corrective.", {}, {}, 0.0, 180.0},
        {"axes", "", T::String, "xyz", "Local axes to turn about."},
        {"falloff", "", T::String, "smooth", "How each corrective's weight fades toward rest: linear | smooth | smoother", {"linear", "smooth", "smoother"}},
        {"tolerance", "", T::Float, 1e-4, "Point offsets smaller than this (cm) are left out; a pose with none gets no corrective.", {}, {}, 0.0},
    };
    ops::registerCommand({
        .name = "bakeCorrectiveMorphs",
        .doc = "Bake what the stack adds on top of linear skinning into corrective morph targets: each joint is turned about each "
               "local axis both ways, the difference between the stack and linear skinning is taken back through the skin, and a pose "
               "driver reading the joint drives the target (1 at the pose, 0 at rest). The skin goes linear and the deformers it "
               "replaces are turned off. Returns {targets, drivers, disabled, residual}.",
        .positionalDoc = "mesh",
        .flags = morphFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("bakeCorrectiveMorphs: give a skinned mesh");
            const NodeId skin = skinOfMesh(g, objs.back(), "bakeCorrectiveMorphs");
            const NodeId mesh = skin::meshOf(g, skin);
            RomOptions rom;
            rom.angle = a.get<double>("angle");
            rom.axes = a.get<std::string>("axes");
            rom.randomPoses = 0;
            const std::vector<NodeId> joints = poseableJoints(g, skin);
            std::vector<PoseInfo> info;
            const std::vector<Pose> poses = romPoses(g, joints, rom, &info);
            if (poses.empty())
                throw CommandError("bakeCorrectiveMorphs: no joint can be posed (every one is keyed or driven)");
            const WeightTable t = skin::table(g, skin);
            const double tol = std::max(0.0, a.get<double>("tolerance"));

            std::vector<std::shared_ptr<MorphDeltas>> deltas;
            {
                Sampler sampler(g, skin, false);
                sampler.keepInputs = true;
                for (const Pose& p : poses)
                {
                    sampler.pose(p);
                    sampler.capture();
                }
                const Samples& s = sampler.s;
                const size_t n = s.rest.size();
                for (int f = 0; f < s.frames; ++f)
                {
                    auto d = std::make_shared<MorphDeltas>();
                    for (size_t pt = 0; pt < n; ++pt)
                    {
                        const glm::dmat4 m = blended(s, t, pt, f);
                        const glm::dvec3 lin = glm::dvec3(m * glm::dvec4(s.inputs[size_t(f)][pt], 1.0));
                        const glm::dvec3 r = s.targets[size_t(f) * n + pt] - lin;
                        if (glm::length(r) <= tol)
                            continue;
                        const glm::dmat3 lin3(m);
                        if (std::abs(glm::determinant(lin3)) < 1e-12)
                            continue;
                        d->indices.push_back(uint32_t(pt));
                        d->deltas.push_back(glm::inverse(lin3) * r);
                    }
                    deltas.push_back(std::move(d));
                }
            }

            json disabled = applyFit(g, skin, {.weights = t}, true)["disabled"];
            NodeId morph = morphs::find(g, mesh);
            if (morph == kNoNode)
                morph = morphs::create(g, mesh, "Morph");
            const std::string fall = a.get<std::string>("falloff");
            const drivers::Falloff falloff = fall == "linear" ? drivers::Falloff::Linear : fall == "smoother" ? drivers::Falloff::Smoother : drivers::Falloff::Smooth;
            json targets = json::array(), made = json::array();
            for (size_t k = 0; k < poses.size(); ++k)
            {
                if (deltas[k]->indices.empty())
                    continue;
                const NodeId j = info[k].joint;
                const glm::dvec3 rest = g.node(j)->as<glm::dvec3>(Transform::kRotate);
                const std::string name = std::format("{}_{}{}", g.node(j)->name(), char('x' + info[k].axis), info[k].sign > 0 ? "Pos" : "Neg");
                // The driver takes its target from the pose it is made in.
                {
                    UndoStack& undo = UndoStack::get();
                    const bool was = undo.enabled();
                    undo.setEnabled(false);
                    g.setValue({j, Transform::kRotate, -1}, poses[k][0].second);
                    undo.setEnabled(was);
                }
                NodeId d = drivers::createPoseDriver(g, j, name + "Driver", 0, rom.angle, 1.0, falloff, rest);
                {
                    UndoStack& undo = UndoStack::get();
                    const bool was = undo.enabled();
                    undo.setEnabled(false);
                    g.setValue({j, Transform::kRotate, -1}, rest);
                    undo.setEnabled(was);
                }
                NodeId target = morphs::addTarget(g, morph, name, deltas[k], 0.0);
                g.connect({d, drivers::kOutput, -1}, {target, morphs::kWeight, -1}, true);
                targets.push_back(g.node(target)->name());
                made.push_back(g.node(d)->name());
            }
            g.evaluate();
            return {{"targets", targets}, {"drivers", made}, {"disabled", disabled}};
        },
    });

    ops::registerCommand({
        .name = "skinningMethod",
        .doc = "Set how skinned meshes blend their joints: linear (what Unreal runs), dualQuaternion, centresOfRotation (Le and Hodgins: "
               "keeps volume at bends and twists without dual quaternion bulges) or directDeltaMush (Le and Lewis: delta mush smoothing "
               "in one pass). The last three export as fitted linear weights.",
        .positionalDoc = "meshes",
        .flags = {{"method", "m", T::String, "linear", "linear | dualQuaternion | centresOfRotation | directDeltaMush",
                   {"linear", "dualQuaternion", "centresOfRotation", "directDeltaMush"}}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const int64_t method = skin::methodOf(a.get<std::string>("method"), "skinningMethod");
            json out = json::array();
            for (const std::string& name : a.objects())
            {
                if (Node* n = g.find(name); n && rig::isJoint(*n))
                    continue;
                const NodeId skin = skin::resolve(g, name, "skinningMethod");
                g.setValue({skin, SkinClusterNode::kSkinningMethod, -1}, method);
                out.push_back(g.node(skin)->name());
            }
            if (out.empty())
                throw CommandError("skinningMethod: give or select skinned meshes");
            return out;
        },
    });

    std::vector<FlagDef> takeFlags = romFlags(0);
    takeFlags.push_back({"name", "n", T::String, nullptr, "Name of the take; <root>_ROM by default."});
    ops::registerCommand({
        .name = "rangeOfMotion",
        .doc = "Make a range-of-motion take: one pose per frame turning the skin's joints (or the given joints) about their local "
               "axes, for fitting and for Unreal ML Deformer training. Joints whose rotation is keyed or driven are left out. "
               "Activates the take. Returns {take, start, end, poses, joints}.",
        .positionalDoc = "mesh or joints",
        .flags = takeFlags,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            std::vector<NodeId> joints;
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "rangeOfMotion");
                if (rig::isJoint(n))
                {
                    if (!rotateDriven(g, n.id()))
                        joints.push_back(n.id());
                }
                else
                    for (NodeId j : poseableJoints(g, skin::resolve(g, name, "rangeOfMotion")))
                        if (std::find(joints.begin(), joints.end(), j) == joints.end())
                            joints.push_back(j);
            }
            if (joints.empty())
                throw CommandError("rangeOfMotion: give a skinned mesh or joints whose rotation isn't keyed or driven");
            RomOptions o = romFrom(a);
            if (!a.has("randomPoses") && !o.sweep)
                o.randomPoses = 100;
            std::vector<Pose> poses = romPoses(g, joints, o);
            const NodeId root = skeletonRoot(g, joints);
            const std::string rootPath = g.path(root);

            // A take's first take adopts what is animated now, so that animation gets a take of its own first.
            bool hasTake = false;
            for (NodeId t : anim::takes(g))
            {
                const std::string& r = g.node(t)->as<std::string>(anim::TakeNode::kRoot);
                hasTake = hasTake || r.empty() || r == rootPath;
            }
            anim::TakeSettings ts;
            ts.root = rootPath;
            ts.fps = fps();
            ts.start = 0.0;
            ts.end = double(poses.size()) / fps();
            if (!hasTake)
                anim::createTake(g, g.node(root)->name() + "_Animation", ts);
            const std::string name = a.has("name") ? a.get<std::string>("name") : g.node(root)->name() + "_ROM";
            const NodeId take = anim::createTake(g, name, ts);
            anim::setActiveTake(g, take);

            std::map<NodeId, glm::dvec3> rest;
            for (NodeId j : joints)
                rest[j] = g.node(j)->as<glm::dvec3>(Transform::kRotate);
            for (size_t f = 0; f <= poses.size(); ++f)
            {
                const double time = double(f) / fps();
                for (NodeId j : joints)
                {
                    glm::dvec3 e = rest[j];
                    if (f > 0)
                        for (auto& [pj, pe] : poses[f - 1])
                            if (pj == j)
                                e = pe;
                    anim::setRotationKey(g, {j, Transform::kRotate, -1}, time, e);
                }
            }
            anim::playback().minTime = 0.0;
            anim::playback().maxTime = ts.end;
            anim::playbackChanged();
            g.evaluate();
            json js = json::array();
            for (NodeId j : joints)
                js.push_back(g.node(j)->path());
            return {{"take", g.node(take)->name()}, {"start", 0}, {"end", int64_t(poses.size())}, {"poses", poses.size()}, {"joints", js}};
        },
    });
}
} // namespace lila::scene::skinbake
