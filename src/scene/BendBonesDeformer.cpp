// The bendBones deformer: a mesh bound to chains of joints bends along the smooth curve through each chain (and its bone
// CVs), deform/BendBones.h doing the maths. Joints and CVs feed it through matrix array inputs, so posing them re-evaluates
// it; the rest pose and the binding are stored on it.

#include "scene/BendBonesDeformer.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "deform/BendBones.h"
#include "deform/Ssdr.h"
#include "curve/CurveControls.h"
#include "curve/NodeHelpers.h"
#include "curve/Tessellate.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Deformers.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Poses.h"
#include "scene/Relocate.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"
#include "scene/SplineChain.h"
#include "scene/Transform.h"
#include "scene/UnrealCheck.h"
#include "math/Unreal.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
#include <span>
#include <map>
#include <mutex>

namespace lila::scene::bendbones
{
using namespace rigutil;
namespace bb = lila::deform::bendbones;

namespace
{
constexpr const char* kType = "bendBones";
constexpr const char* kCvType = "bendBoneCV";

/** What a bind fixed: chain sizes, the joints' rest matrices, the bone CVs' rest places, and the binding. */
struct Bound
{
    std::vector<int> sizes;
    std::vector<glm::dmat4> rest;      // per joint, chains in order
    std::vector<int> cvBone;           // per CV: the global joint index of the bone's first joint
    std::vector<glm::dvec3> cvRest;    // per CV: rest position in that joint's space
    std::vector<glm::dvec3> normals;   // per joint
    glm::dmat4 meshWorld{1.0};          // the mesh's world matrix at bind: chains are in world, points in the mesh's space
    bb::Binding binding;
};

json boundJson(const Bound& b)
{
    json places = json::array();
    for (const bb::Place& p : b.binding.place)
        places.push_back({p.segment, p.fraction, p.offset.x, p.offset.y, p.offset.z});
    json cvRest = json::array(), normals = json::array();
    for (const glm::dvec3& v : b.cvRest)
        cvRest.push_back({v.x, v.y, v.z});
    for (const glm::dvec3& v : b.normals)
        normals.push_back({v.x, v.y, v.z});
    return {{"sizes", b.sizes}, {"cvBone", b.cvBone}, {"cvRest", cvRest}, {"normals", normals},
            {"offsets", b.binding.offsets}, {"chain", b.binding.chain}, {"weight", b.binding.weight}, {"place", places}};
}

Bound boundOf(const json& j)
{
    Bound b;
    b.sizes = j.value("sizes", std::vector<int>{});
    b.cvBone = j.value("cvBone", std::vector<int>{});
    for (const json& v : j.value("cvRest", json::array()))
        b.cvRest.push_back({v[0].get<double>(), v[1].get<double>(), v[2].get<double>()});
    for (const json& v : j.value("normals", json::array()))
        b.normals.push_back({v[0].get<double>(), v[1].get<double>(), v[2].get<double>()});
    b.binding.offsets = j.value("offsets", std::vector<uint32_t>{0});
    b.binding.chain = j.value("chain", std::vector<uint32_t>{});
    b.binding.weight = j.value("weight", std::vector<double>{});
    for (const json& p : j.value("place", json::array()))
        b.binding.place.push_back({.segment = p[0].get<int>(), .fraction = p[1].get<double>(), .offset = {p[2].get<double>(), p[3].get<double>(), p[4].get<double>()}});
    return b;
}

/** The chains from joint matrices and CV positions (world), split by the bind's sizes. */
std::vector<bb::Chain> chainsOf(const Bound& b, const std::vector<glm::dmat4>& joints, const std::vector<glm::dvec3>& cvs)
{
    std::vector<bb::Chain> out;
    size_t at = 0;
    std::vector<int> first;
    for (int n : b.sizes)
    {
        bb::Chain c;
        first.push_back(int(at));
        for (int k = 0; k < n && at < joints.size(); ++k, ++at)
        {
            c.position.push_back(glm::dvec3(joints[at][3]));
            c.rotation.push_back(orthonormal(glm::dmat3(joints[at])));
        }
        c.cvs.assign(c.position.size(), {});
        out.push_back(std::move(c));
    }
    for (size_t i = 0; i < cvs.size() && i < b.cvBone.size(); ++i)
        for (size_t ci = 0; ci < out.size(); ++ci)
            if (const int bone = b.cvBone[i] - first[ci]; bone >= 0 && bone + 1 < int(out[ci].position.size()))
            {
                out[ci].cvs[size_t(bone)].push_back(cvs[i]);
                break;
            }
    // CVs along a bone go in order of how far along it they sit.
    for (bb::Chain& c : out)
        for (size_t bone = 0; bone + 1 < c.position.size(); ++bone)
        {
            const glm::dvec3 a = c.position[bone], d = c.position[bone + 1] - a;
            std::sort(c.cvs[bone].begin(), c.cvs[bone].end(),
                      [&](const glm::dvec3& x, const glm::dvec3& y) { return glm::dot(x - a, d) < glm::dot(y - a, d); });
        }
    return out;
}

/** Where the bone CVs sit at rest, in world. */
std::vector<glm::dvec3> restCvs(const Bound& b)
{
    std::vector<glm::dvec3> out;
    for (size_t i = 0; i < b.cvRest.size() && i < b.cvBone.size(); ++i)
        out.push_back(glm::dvec3(b.rest[size_t(b.cvBone[i])] * glm::dvec4(b.cvRest[i], 1.0)));
    return out;
}

/** Rest and posed curves of every chain, from posed joint matrices and CV positions (world). */
std::pair<std::vector<bb::Curve>, std::vector<bb::Curve>> curvesOf(const Bound& b, const std::vector<glm::dmat4>& joints, std::vector<glm::dvec3> cvs)
{
    std::vector<glm::dvec3> cvRest = restCvs(b);
    cvs.resize(std::min(cvs.size(), cvRest.size()));
    cvRest.resize(cvs.size());
    std::pair<std::vector<bb::Curve>, std::vector<bb::Curve>> out;
    size_t at = 0;
    const std::vector<bb::Chain> restChains = chainsOf(b, b.rest, cvRest), posedChains = chainsOf(b, joints, cvs);
    for (size_t c = 0; c < restChains.size(); ++c)
    {
        const std::vector<glm::dvec3> normals(b.normals.begin() + long(at), b.normals.begin() + long(at + restChains[c].position.size()));
        at += restChains[c].position.size();
        out.first.push_back(bb::build(restChains[c], normals));
        out.second.push_back(bb::build(posedChains[c], normals));
    }
    return out;
}

/** The bound points on the posed curves, in the mesh's space at bind. */
void deformed(const Bound& b, const std::vector<bb::Curve>& rest, const std::vector<bb::Curve>& posed, const bb::Options& o, std::vector<glm::dvec3>& points)
{
    const glm::dmat4 toLocal = glm::inverse(b.meshWorld);
    bb::deformPoints(rest, posed, b.binding, o, points);
    for (glm::dvec3& p : points)
        p = glm::dvec3(toLocal * glm::dvec4(p, 1.0));
}

/**
 * The bind with painted chain weights in place of its own: each point's places on the chains it is painted to, located on
 * the rest curve where the bind had none.
 */
bb::Binding paintedBinding(const Bound& b, const SkinWeights& w, const std::vector<bb::Curve>& rest, std::span<const glm::dvec3> input)
{
    bb::Binding out;
    for (size_t p = 0; p + 1 < b.binding.offsets.size() && p < input.size(); ++p)
    {
        double total = 0.0;
        if (p < w.pointCount())
            for (uint32_t k = w.offsets[p]; k < w.offsets[p + 1]; ++k)
                total += std::max(0.0f, w.weights[k]);
        if (total <= 0.0)
        {
            for (uint32_t k = b.binding.offsets[p]; k < b.binding.offsets[p + 1]; ++k)
            {
                out.chain.push_back(b.binding.chain[k]);
                out.weight.push_back(b.binding.weight[k]);
                out.place.push_back(b.binding.place[k]);
            }
            out.offsets.push_back(uint32_t(out.chain.size()));
            continue;
        }
        for (uint32_t k = w.offsets[p]; k < w.offsets[p + 1]; ++k)
        {
            const uint32_t c = w.influences[k];
            if (w.weights[k] <= 0.0f || c >= rest.size())
                continue;
            std::optional<bb::Place> place;
            for (uint32_t e = b.binding.offsets[p]; e < b.binding.offsets[p + 1]; ++e)
                if (b.binding.chain[e] == c)
                    place = b.binding.place[e];
            if (!place)
                place = bb::locate(rest[c], glm::dvec3(b.meshWorld * glm::dvec4(input[p], 1.0)));
            out.chain.push_back(c);
            out.weight.push_back(double(w.weights[k]) / total);
            out.place.push_back(*place);
        }
        out.offsets.push_back(uint32_t(out.chain.size()));
    }
    return out;
}

class BendBonesNode : public DeformerNode
{
public:
    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        const Bound b = boundWithRest();
        if (b.sizes.empty() || b.binding.offsets.size() != points.size() + 1)
            return;
        std::vector<glm::dmat4> joints;
        for (const auto& [i, v] : elements(type().attrIndex("matrix")))
            joints.push_back(std::get<glm::dmat4>(v));
        std::vector<glm::dvec3> cvs;
        for (const auto& [i, v] : elements(type().attrIndex("cvMatrix")))
            cvs.push_back(glm::dvec3(std::get<glm::dmat4>(v)[3]));
        if (joints.size() != b.rest.size())
            return;
        const auto [rest, posed] = curvesOf(b, joints, cvs);
        Bound painted = b;
        painted.binding = binding(b, rest, points);
        deformed(painted, rest, posed, options(), points);
    }

    /** The binding the points deform by: the bind's own, or the painted chain weights' when there are any. */
    bb::Binding binding(const Bound& b, const std::vector<bb::Curve>& rest, std::span<const glm::dvec3> input) const
    {
        const DataPtr w = as<DataPtr>(type().attrIndex("chainWeights"));
        const auto* sw = dynamic_cast<const SkinWeights*>(w.get());
        if (!sw)
            return b.binding;
        std::lock_guard lock(mMutex);
        if (mPaintedFor != w || mPaintedText != mText)
        {
            mPainted = paintedBinding(b, *sw, rest, input);
            mPaintedFor = w;
            mPaintedText = mText;
        }
        return mPainted;
    }

    /** The bind with its world-space parts, which live in matrix attrs so relocating the scene moves them. */
    Bound boundWithRest() const
    {
        Bound b = bound();
        for (const auto& [i, v] : elements(type().attrIndex("restMatrix")))
            b.rest.push_back(std::get<glm::dmat4>(v));
        b.meshWorld = as<glm::dmat4>(type().attrIndex("bindMeshMatrix"));
        return b;
    }

    bb::Options options() const
    {
        return {.volume = as<double>(type().attrIndex("volume")), .evenBend = as<double>(type().attrIndex("evenBend"))};
    }

private:
    /** The stored bind, parsed once per change of its text. */
    const Bound& bound() const
    {
        const std::string& text = as<std::string>(type().attrIndex("binding"));
        std::lock_guard lock(mMutex);
        if (text != mText)
        {
            mText = text;
            mBound = text.empty() ? Bound{} : boundOf(json::parse(text, nullptr, false));
        }
        return mBound;
    }

    mutable std::mutex mMutex;
    mutable std::string mText;
    mutable Bound mBound;
    mutable DataPtr mPaintedFor;
    mutable std::string mPaintedText;
    mutable bb::Binding mPainted;
};

AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, uint32_t flags)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = flags, .doc = std::move(doc)};
}

/** A joint path, or the error a command reports. */
NodeId jointArg(Graph& g, const json& name, const char* command)
{
    Node* n = g.find(name.get<std::string>());
    if (!n || !rig::isJoint(*n))
        throw CommandError(std::string(command) + ": " + name.get<std::string>() + " is not a joint");
    return n->id();
}
/** The joints feeding a deformer's matrix array (the chains, in order) and its bone CVs. */
std::pair<std::vector<NodeId>, std::vector<NodeId>> inputsOf(Graph& g, const Node& d)
{
    std::pair<std::vector<NodeId>, std::vector<NodeId>> out;
    for (const PlugRef& e : g.elements({d.id(), d.type().attrIndex("matrix"), -1}))
        if (const auto src = g.source(e))
            out.first.push_back(src->node);
    for (const PlugRef& e : g.elements({d.id(), d.type().attrIndex("cvMatrix"), -1}))
        if (const auto src = g.source(e))
            out.second.push_back(src->node);
    return out;
}

/** A pose for the fit: every joint turned at random about its own axes, children following, CVs riding their bones. */
std::pair<std::vector<glm::dmat4>, std::vector<glm::dvec3>> randomPose(const Bound& b, double degrees, std::mt19937& rng)
{
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::vector<glm::dmat4> posed;
    size_t at = 0;
    for (int n : b.sizes)
        for (int k = 0; k < n; ++k, ++at)
        {
            const glm::dvec3 e = glm::radians(degrees) * glm::dvec3(u(rng), u(rng), u(rng));
            const glm::dmat4 turn = glm::dmat4(glm::mat3_cast(glm::dquat(e)));
            posed.push_back(k == 0 ? b.rest[at] * turn : posed[at - 1] * glm::inverse(b.rest[at - 1]) * b.rest[at] * turn);
        }
    std::vector<glm::dvec3> cvs;
    for (size_t i = 0; i < b.cvRest.size() && i < b.cvBone.size(); ++i)
        cvs.push_back(glm::dvec3(posed[size_t(b.cvBone[i])] * glm::dvec4(b.cvRest[i], 1.0)));
    return {posed, cvs};
}

/** Segment joint world matrices along each chain's curve, chains in order. */
std::vector<glm::dmat4> segmentsOf(const std::vector<bb::Curve>& curves, int perBone)
{
    std::vector<glm::dmat4> out;
    for (const bb::Curve& c : curves)
        for (double s : bb::segmentLengths(c, perBone))
            out.push_back(bb::matrixAt(c, s));
    return out;
}

size_t segmentCount(int chainSize, int perBone) { return size_t((chainSize - 1) * perBone + 1); }

/** Starting weights for the fit: each point split between the two segment joints its bind place sits between. */
void startingWeights(const Bound& b, int perBone, deform::SsdrProblem& p)
{
    std::vector<uint32_t> first;
    uint32_t total = 0;
    for (int n : b.sizes)
    {
        first.push_back(total);
        total += uint32_t(segmentCount(n, perBone));
    }
    p.offsets = {0};
    for (size_t pt = 0; pt + 1 < b.binding.offsets.size(); ++pt)
    {
        std::map<uint32_t, float> w;
        for (uint32_t k = b.binding.offsets[pt]; k < b.binding.offsets[pt + 1]; ++k)
        {
            const uint32_t c = b.binding.chain[k];
            const bb::Place& pl = b.binding.place[k];
            const double along = (double(pl.segment) + std::clamp(pl.fraction, 0.0, 1.0)) * perBone;
            const uint32_t last = first[c] + uint32_t(segmentCount(b.sizes[c], perBone)) - 1;
            const uint32_t i = std::min(first[c] + uint32_t(std::floor(along)), last);
            const double t = along - std::floor(along);
            w[i] += float(b.binding.weight[k] * (1.0 - t));
            w[std::min(i + 1, last)] += float(b.binding.weight[k] * t);
        }
        for (const auto& [i, v] : w)
            if (v > 0.0f)
            {
                p.influences.push_back(i);
                p.weights.push_back(v);
            }
        p.offsets.push_back(uint32_t(p.influences.size()));
    }
}

/** Writes a joint's local matrix as translate and rotate, keyed at time or set when there is none. */
void writeLocal(Graph& g, NodeId j, const glm::dmat4& local, std::optional<double> time, glm::dvec3& near)
{
    near = eulerNear(orthonormal(glm::dmat3(local)), RotateOrder(g.node(j)->as<int64_t>(Transform::kRotateOrder)), near);
    if (!time)
    {
        g.setValue({j, Transform::kTranslate, -1}, glm::dvec3(local[3]));
        g.setValue({j, Transform::kRotate, -1}, near);
        return;
    }
    for (int c = 0; c < 3; ++c)
        anim::setKey(g, {j, Transform::kTranslate, c}, *time, local[3][c]);
    anim::setRotationKey(g, {j, Transform::kRotate, -1}, *time, near);
}

bool moves(const std::vector<std::vector<glm::dmat4>>& frames, size_t count)
{
    for (size_t f = 1; f < count; ++f)
        for (size_t i = 0; i < frames[f].size(); ++i)
            for (int c = 0; c < 4; ++c)
                if (glm::length(frames[f][i][c] - frames[0][i][c]) > 1e-7)
                    return true;
    return false;
}

/** The deformer a command acts on: the named one, or the one in the named mesh's stack. */
Node* deformerArg(Graph& g, const Args& a, const char* command)
{
    const std::vector<std::string> objs = a.objects();
    Node* d = objs.empty() ? nullptr : g.find(objs.front());
    if (d && d->type().isA("mesh"))
        for (NodeId id : modifiers::stack(g, d->id()))
            if (g.node(id)->type().name == kType)
                d = g.node(id);
    if (!d || d->type().name != kType)
        throw CommandError(std::string(command) + ": give a bendBones deformer or a mesh it deforms");
    return d;
}

std::string bareName(const std::string& path) { return path.substr(path.find_last_of("|:") + 1); }

json toUnrealJson(const glm::dvec3& v)
{
    const math::Vector u = math::toUnreal(math::Vector(v.x, v.y, v.z));
    return {u.x, u.y, u.z};
}

/**
 * Bend bones live in Unreal: the chains by bone name, the options, and each point where it sits at bind (Unreal space)
 * with its chains' weights, painted ones included, and the bone CVs: per chain, each CV's bone (its index in the chain),
 * its rest place (Unreal space) and its node's name, the bone Unreal reads it from when posed. LilaLink binds every LOD
 * of the skeletal mesh to them and gives it the Lila Bend Bones Deformer Graph.
 */
json unrealPayload(Graph& g, Node& d, const std::string& mesh, const std::string& deformerPath)
{
    const auto& node = static_cast<const BendBonesNode&>(d);
    g.evaluate();
    const Bound bound = node.boundWithRest();
    const std::vector<NodeId> chainJoints = inputsOf(g, d).first;
    if (bound.sizes.empty() || chainJoints.size() != bound.rest.size())
        throw CommandError("unrealBendBones: " + d.path() + " has no bind");
    const MeshPtr in = d.as<MeshPtr>(DeformerNode::kInMesh);
    if (!in || bound.binding.offsets.size() != in->positions().size() + 1)
        throw CommandError("unrealBendBones: the mesh changed since it was bound; bind it again");
    const bb::Binding binding = node.binding(bound, curvesOf(bound, bound.rest, restCvs(bound)).first, in->positions());
    // The curve normals at rest go too, in world space: the twist depends on them, so Unreal can't choose its own.
    json chains = json::array(), restJoints = json::array(), restNormals = json::array();
    for (size_t at = 0; int n : bound.sizes)
    {
        json names = json::array(), rest = json::array(), normals = json::array();
        for (int k = 0; k < n; ++k, ++at)
        {
            names.push_back(bareName(g.node(chainJoints[at])->name()));
            rest.push_back(toUnrealJson(glm::dvec3(bound.rest[at][3])));
            if (at < bound.normals.size())
                normals.push_back(toUnrealJson(orthonormal(glm::dmat3(bound.rest[at])) * bound.normals[at]));
        }
        chains.push_back(names);
        restJoints.push_back(rest);
        restNormals.push_back(normals);
    }
    json cvs = json::array();
    for (size_t c = 0; c < bound.sizes.size(); ++c)
        cvs.push_back(json::array());
    const std::vector<glm::dvec3> cvRest = restCvs(bound);
    for (const PlugRef& e : g.elements({d.id(), d.type().attrIndex("cvMatrix"), -1}))
    {
        const auto src = g.source(e);
        const size_t i = size_t(e.element);
        if (!src || i >= bound.cvBone.size() || i >= cvRest.size())
            continue;
        for (size_t c = 0, first = 0; c < bound.sizes.size(); first += size_t(bound.sizes[c]), ++c)
            if (const int bone = bound.cvBone[i] - int(first); bone >= 0 && bone + 1 < bound.sizes[c])
            {
                cvs[c].push_back({{"bone", bone}, {"rest", toUnrealJson(cvRest[i])}, {"name", bareName(g.node(src->node)->name())}});
                break;
            }
    }
    json points = json::array();
    const std::span<const glm::dvec3> pos = in->positions();
    for (size_t i = 0; i + 1 < binding.offsets.size() && i < pos.size(); ++i)
    {
        json w = json::array();
        for (uint32_t e = binding.offsets[i]; e < binding.offsets[i + 1]; ++e)
            w.push_back({binding.chain[e], binding.weight[e]});
        points.push_back({{"p", toUnrealJson(glm::dvec3(bound.meshWorld * glm::dvec4(pos[i], 1.0)))}, {"w", w}});
    }
    const bb::Options o = node.options();
    json warnings = json::array();
    return {{"skeletalMesh", mesh},
            {"deformer", deformerPath},
            {"chains", chains},
            {"restJoints", restJoints},
            {"restNormals", restNormals},
            {"cvs", cvs},
            {"volume", o.volume},
            {"evenBend", o.evenBend},
            {"samplesPerSpan", 16},
            {"blend", 0.35},
            {"points", points},
            {"warnings", warnings}};
}

json bake(Graph& g, Node& d, const Args& a)
{
    LILA_ZONE();
    const auto& node = static_cast<const BendBonesNode&>(d);
    const int perBone = std::max(1, a.get<int>("segments", 3));
    g.evaluate();
    const Bound bound = node.boundWithRest();
    const auto [chainJoints, cvNodes] = inputsOf(g, d);
    if (bound.sizes.empty() || chainJoints.size() != bound.rest.size())
        throw CommandError("bakeBendBones: " + d.path() + " has no bind to bake");
    const NodeId mesh = modifiers::meshOf(g, d.id());
    if (mesh == kNoNode)
        throw CommandError("bakeBendBones: " + d.path() + " deforms no mesh");
    if (skin::find(g, mesh) != kNoNode)
        throw CommandError("bakeBendBones: " + g.node(mesh)->path() + " is already skinned; bake bend bones on a mesh they alone deform");
    const MeshPtr in = d.as<MeshPtr>(DeformerNode::kInMesh);
    if (!in || bound.binding.offsets.size() != in->positions().size() + 1)
        throw CommandError("bakeBendBones: the mesh changed since it was bound; bind it again");
    const bb::Options o = node.options();
    Bound b = bound;
    b.binding = node.binding(bound, curvesOf(bound, bound.rest, restCvs(bound)).first, in->positions());

    // Samples: the animation range, then random poses so the weights hold beyond the motion the scene happens to have.
    const anim::Playback& pb = anim::playback();
    const double fps = std::max(pb.fps, 1e-6);
    const int64_t start = a.has("start") ? a.get<int>("start") : int64_t(std::llround(pb.minTime * fps));
    const int64_t end = a.has("end") ? a.get<int>("end") : int64_t(std::llround(pb.maxTime * fps));
    std::vector<double> times;
    for (int64_t f = start; f <= end; f += std::max(1, a.get<int>("step", 1)))
        times.push_back(double(f) / fps);
    if (times.empty())
        times.push_back(g.time());

    const size_t pointCount = in->positions().size();
    std::vector<glm::dvec3> targets;
    std::vector<std::vector<glm::dmat4>> segWorld; // per sample
    std::vector<std::vector<glm::dmat4>> parentWorld; // per frame, the parent of each chain's first joint
    std::vector<NodeId> chainParent;
    for (size_t at = 0; int n : b.sizes)
    {
        chainParent.push_back(g.parent(chainJoints[at]));
        at += size_t(n);
    }
    auto sample = [&](const std::vector<glm::dmat4>& joints, const std::vector<glm::dvec3>& cvs) {
        const auto [rest, posed] = curvesOf(b, joints, cvs);
        std::vector<glm::dvec3> pts(pointCount);
        deformed(b, rest, posed, o, pts);
        targets.insert(targets.end(), pts.begin(), pts.end());
        segWorld.push_back(segmentsOf(posed, perBone));
    };
    const double saved = g.time();
    for (double t : times)
    {
        g.setTime(t);
        g.evaluate();
        std::vector<glm::dmat4> joints, parents;
        std::vector<glm::dvec3> cvs;
        for (NodeId j : chainJoints)
            joints.push_back(worldOf(*g.node(j)));
        for (NodeId c : cvNodes)
            cvs.push_back(glm::dvec3(worldOf(*g.node(c))[3]));
        for (NodeId p : chainParent)
            parents.push_back(p != kNoNode ? worldOf(*g.node(p)) : glm::dmat4(1.0));
        sample(joints, cvs);
        parentWorld.push_back(std::move(parents));
    }
    g.setTime(saved);
    g.evaluate();
    std::mt19937 rng(uint32_t(a.get<int>("seed", 1)));
    const int poses = std::max(0, a.get<int>("poses", 24));
    for (int k = 0; k < poses; ++k)
    {
        const auto [joints, cvs] = randomPose(b, a.get<double>("angle", 45.0), rng);
        sample(joints, cvs);
    }

    const std::vector<glm::dmat4> segRest = segmentsOf(curvesOf(b, b.rest, restCvs(b)).first, perBone);
    deform::SsdrProblem p;
    p.rest.assign(in->positions().begin(), in->positions().end());
    for (geo::Index f = 0; f < in->faceCapacity(); ++f)
        if (in->faceValid(f))
        {
            std::vector<int> poly;
            for (geo::Index v : in->facePoints(f))
                poly.push_back(int(v));
            p.polygons.push_back(std::move(poly));
        }
    p.frames = int(segWorld.size());
    p.targets = std::move(targets);
    p.bones = int(segRest.size());
    const glm::dmat4 toLocal = glm::inverse(b.meshWorld);
    for (const std::vector<glm::dmat4>& frame : segWorld)
        for (size_t i = 0; i < frame.size(); ++i)
            p.transforms.push_back(toLocal * frame[i] * glm::inverse(segRest[i]) * b.meshWorld);
    startingWeights(b, perBone, p);
    p.maxInfluences = std::max(1, a.get<int>("maxInfluences", 4));
    p.iterations = std::max(1, a.get<int>("iterations", 3));
    const deform::SsdrResult fit = deform::ssdrFit(p);

    // A chain of segment joints per bend-bones chain, beside the original under the same parent.
    const bool animated = moves(segWorld, times.size());
    std::vector<NodeId> segs, roots;
    json chains = json::array();
    for (size_t c = 0, first = 0, at = 0; c < b.sizes.size(); ++c)
    {
        const size_t count = segmentCount(b.sizes[c], perBone);
        const std::string base = g.node(chainJoints[at])->name() + "_bend";
        json names = json::array();
        for (size_t k = 0; k < count; ++k)
        {
            const NodeId parent = k == 0 ? chainParent[c] : segs.back();
            Node* j = g.createNode(*types::find("joint"), base + std::to_string(k + 1), parent);
            segs.push_back(j->id());
            if (k == 0)
                roots.push_back(j->id());
            names.push_back(j->path());
            glm::dvec3 near(0.0);
            auto parentAt = [&](size_t f) { return k == 0 ? parentWorld[f][c] : segWorld[f][first + k - 1]; };
            if (!animated)
            {
                writeLocal(g, j->id(), glm::inverse(parentAt(0)) * segWorld[0][first + k], std::nullopt, near);
                continue;
            }
            for (size_t f = 0; f < times.size(); ++f)
                writeLocal(g, j->id(), glm::inverse(parentAt(f)) * segWorld[f][first + k], times[f], near);
        }
        chains.push_back(names);
        first += count;
        at += size_t(b.sizes[c]);
    }

    json args = json::array();
    for (NodeId j : segs)
        args.push_back(g.node(j)->path());
    args.push_back(g.node(mesh)->path());
    const std::string skinName =
        ops::execute("skinCluster", {{"args", args}, {"bindTo", "selected"}, {"maxInfluences", p.maxInfluences}, {"name", "BendBonesSkin"}}).get<std::string>();
    const NodeId skin = skin::resolve(g, skinName, "bakeBendBones");
    g.setValue({skin, SkinClusterNode::kGeomMatrix, -1}, b.meshWorld);
    std::map<NodeId, uint32_t> influenceOf;
    for (const skin::Influence& i : skin::influences(g, skin))
        if (const auto it = std::find(segs.begin(), segs.end(), i.joint); it != segs.end())
        {
            influenceOf[i.joint] = i.index;
            g.setValue({i.link, SkinInfluenceNode::kBindPreMatrix, -1}, glm::inverse(segRest[size_t(it - segs.begin())]));
        }
    WeightTable t;
    t.points.resize(pointCount);
    for (size_t pt = 0; pt < pointCount; ++pt)
        for (uint32_t k = fit.offsets[pt]; k < fit.offsets[pt + 1]; ++k)
            t.points[pt].push_back({influenceOf[segs[fit.influences[k]]], fit.weights[k]});
    skin::setWeights(g, skin, t);
    g.setValue({d.id(), DeformerNode::kEnabled, -1}, false);

    // U1: the baked skeleton under Unreal's transform rules over the frames it was keyed on.
    uecheck::Options uo{.roots = roots, .start = times.front(), .end = times.back(), .fps = fps, .tolerance = a.get<double>("tolerance", 0.01)};
    const uecheck::Report r = uecheck::check(g, uo);
    uecheck::reportIssues(g, r);
    json mismatches = json::array();
    for (const uecheck::Mismatch& m : r.mismatches)
        mismatches.push_back({{"joint", g.node(m.joint)->path()}, {"error", m.error}, {"time", m.time}});
    return {{"joints", chains},
            {"skinCluster", g.node(skin)->path()},
            {"rmse", fit.rmse},
            {"maxError", fit.maxError},
            {"frames", int(times.size())},
            {"poses", poses},
            {"unrealCheck", {{"worst", r.worst}, {"frames", r.frames}, {"mismatches", mismatches}}}};
}

/** The chains as influences the weight tools paint: one per chain, named after its first joint, indexed by chain. */
std::vector<skin::Influence> chainInfluences(Graph& g, NodeId d)
{
    const Node& n = *g.node(d);
    const Bound b = boundOf(json::parse(n.as<std::string>(n.type().attrIndex("binding")), nullptr, false));
    const std::vector<NodeId> joints = inputsOf(g, n).first;
    std::vector<skin::Influence> out;
    size_t at = 0;
    for (size_t c = 0; c < b.sizes.size() && at < joints.size(); at += size_t(b.sizes[c]), ++c)
        out.push_back({.link = kNoNode, .joint = joints[at], .index = uint32_t(c), .name = g.node(joints[at])->name()});
    return out;
}

WeightTable bindWeights(Graph& g, NodeId d)
{
    const Node& n = *g.node(d);
    const Bound b = boundOf(json::parse(n.as<std::string>(n.type().attrIndex("binding")), nullptr, false));
    WeightTable t;
    t.points.resize(b.binding.offsets.size() - 1);
    for (size_t p = 0; p < t.points.size(); ++p)
        for (uint32_t k = b.binding.offsets[p]; k < b.binding.offsets[p + 1]; ++k)
            t.points[p].push_back({b.binding.chain[k], float(b.binding.weight[k])});
    return t;
}

void registerWeightHost()
{
    const NodeType& t = *types::find(kType);
    skin::registerWeightHost(kType, {.weights = t.attrIndex("chainWeights"),
                                     .geomMatrix = t.attrIndex("bindMeshMatrix"),
                                     .inMesh = DeformerNode::kInMesh,
                                     .outMesh = t.attrIndex("outMesh"),
                                     .influences = chainInfluences,
                                     .seed = bindWeights});
}
/** Adds a bone CV on joint's bone of a bendBones deformer, at a fraction along it, where the bone is now. */
NodeId addCv(Graph& g, Node& d, NodeId joint, double at)
{
    Bound b = boundOf(json::parse(d.as<std::string>(d.type().attrIndex("binding")), nullptr, false));
    for (const auto& [i, v] : d.elements(d.type().attrIndex("restMatrix")))
        b.rest.push_back(std::get<glm::dmat4>(v));
    // The joint's place in the bind: its index, and the next joint of its chain.
    std::vector<NodeId> joints;
    for (const PlugRef& e : g.elements({d.id(), d.type().attrIndex("matrix"), -1}))
        if (const auto src = g.source(e))
            joints.push_back(src->node);
    const auto it = std::find(joints.begin(), joints.end(), joint);
    if (it == joints.end())
        throw CommandError("bendBoneCV: " + g.node(joint)->path() + " is not in the deformer's chains");
    const int index = int(it - joints.begin());
    int start = 0;
    bool last = false;
    for (int n : b.sizes)
    {
        if (index < start + n)
        {
            last = index == start + n - 1;
            break;
        }
        start += n;
    }
    if (last)
        throw CommandError("bendBoneCV: " + g.node(joint)->path() + " ends its chain, so it has no bone after it");
    g.evaluate();
    const glm::dvec3 restPoint = glm::mix(glm::dvec3(b.rest[size_t(index)][3]), glm::dvec3(b.rest[size_t(index) + 1][3]), at);
    const glm::dvec3 nowPoint = glm::mix(glm::dvec3(worldOf(*g.node(joint))[3]), glm::dvec3(worldOf(*g.node(joints[size_t(index) + 1]))[3]), at);
    Node* cv = g.createNode(*types::find(kCvType), g.node(joint)->name() + "_cv");
    g.setParent(cv->id(), joint, false);
    const glm::dmat4 parentInv = glm::inverse(worldOf(*g.node(joint)));
    g.setValue({cv->id(), Transform::kTranslate, -1}, glm::dvec3(parentInv * glm::dvec4(nowPoint, 1.0)));
    b.cvBone.push_back(index);
    b.cvRest.push_back(glm::dvec3(glm::inverse(b.rest[size_t(index)]) * glm::dvec4(restPoint, 1.0)));
    const int cvMatrix = d.type().attrIndex("cvMatrix");
    g.connect({cv->id(), Transform::kWorldMatrix, -1}, {d.id(), cvMatrix, -1, int(b.cvBone.size() - 1)});
    g.setValue({d.id(), d.type().attrIndex("binding"), -1}, boundJson(b).dump());
    return cv->id();
}

/** The bendBones deformer whose chains include the joint, or null. */
Node* deformerOf(Graph& g, NodeId joint)
{
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().name == kType)
            for (const PlugRef& e : g.elements({id, n->type().attrIndex("matrix"), -1}))
                if (const auto src = g.source(e); src && src->node == joint)
                    return n;
    return nullptr;
}

/** The joint on the other side (arm_l to arm_r), or kNoNode for a centre joint. */
NodeId mirrorJoint(Graph& g, NodeId joint)
{
    const std::string name = g.node(joint)->name(), other = poses::mirrorNodeName(name);
    if (other == name)
        return kNoNode;
    // Same skeleton: search from the top joint down.
    NodeId top = joint;
    while (g.parent(top) != kNoNode && rig::isJoint(*g.node(g.parent(top))))
        top = g.parent(top);
    for (NodeId id : g.descendants(top))
        if (g.node(id)->name() == other && rig::isJoint(*g.node(id)))
            return id;
    return kNoNode;
}

/** A bone CV's partner on the mirrored bone: the CV at the same place in order along it. */
NodeId partnerCv(Graph& g, NodeId cv)
{
    const NodeId joint = g.parent(cv), other = joint != kNoNode ? mirrorJoint(g, joint) : kNoNode;
    if (other == kNoNode)
        return kNoNode;
    auto cvsUnder = [&](NodeId j) {
        std::vector<std::pair<double, NodeId>> out;
        for (NodeId c : g.children(j))
            if (g.node(c)->type().name == kCvType)
                out.push_back({glm::length(g.node(c)->as<glm::dvec3>(Transform::kTranslate)), c});
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto mine = cvsUnder(joint), theirs = cvsUnder(other);
    for (size_t i = 0; i < mine.size() && i < theirs.size(); ++i)
        if (mine[i].second == cv)
            return theirs[i].second;
    return kNoNode;
}

/** The hose limb a joint shapes or rides: its limb joints feed the hose, its hose joints ride the hose's chain. */
NodeId hoseOf(Graph& g, NodeId joint)
{
    const NodeId chain = spline::splineChainContaining(g, joint);
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || !n->type().isA(spline::kHoseType))
            continue;
        if (chain && spline::hoseChain(g, id) == chain)
            return id;
        for (const char* attr : {"rootMatrix", "midMatrix"})
            if (const auto src = g.source({id, n->type().attrIndex(attr), -1}); src && src->node == joint)
                return id;
    }
    return kNoNode;
}

/** A hose limb's bone CV is its bend control: the hose's arc goes through it. */
NodeId hoseCv(Graph& g, NodeId hose)
{
    const auto src = g.source({hose, g.node(hose)->type().attrIndex("bendMatrix"), -1});
    return src ? src->node : kNoNode;
}

/**
 * A bone CV on a spline chain: a point inserted into the chain's curve at the bone's middle (its shape unchanged), with
 * a control like the others. The curve's controls are rebuilt, so it refuses while any of them is keyed.
 */
NodeId addSplineCv(Graph& g, NodeId chain, NodeId joint, double at)
{
    const std::vector<NodeId> joints = spline::splineChainJoints(g, chain);
    const auto it = std::find(joints.begin(), joints.end(), joint);
    if (it == joints.end() || it + 1 == joints.end())
        throw CommandError("bendBoneCV: " + g.node(joint)->path() + " ends its spline chain, so it has no bone after it");
    Node* curve = g.find(g.node(chain)->as<std::string>(g.node(chain)->type().attrIndex("curve")));
    if (!curve)
        throw CommandError("bendBoneCV: the spline chain of " + g.node(joint)->path() + " has no curve");
    std::vector<NodeId> controls = curve::curveControlNodes(g, *curve);
    for (NodeId c : controls)
        for (int attr : {int(Transform::kTranslate), int(Transform::kRotate)})
            if (g.source({c, attr, -1}) || g.source({c, attr, 0}) || g.source({c, attr, 1}) || g.source({c, attr, 2}))
                throw CommandError("bendBoneCV: " + g.node(c)->name() + " is keyed or driven, and adding a point rebuilds the curve's controls");
    g.evaluate();
    const glm::dvec3 mid = glm::mix(glm::dvec3(worldOf(*g.node(*it))[3]), glm::dvec3(worldOf(*g.node(*(it + 1)))[3]), at);
    const glm::dvec3 local(glm::inverse(worldOf(*curve)) * glm::dvec4(mid, 1.0));
    const curve::Splines splines = curve::helpers::storedSplines(g, *curve);
    if (splines.empty())
        throw CommandError("bendBoneCV: the spline chain's curve is empty");
    const double u = curve::closestParam(splines.front(), local);
    const bool hadControls = !controls.empty();
    const bool handles = hadControls && controls.size() > splines.front().points.size();
    if (hadControls)
        ops::execute("removeCurveControls", {{"args", {curve->path()}}});
    const size_t inserted = ops::execute("insertCurvePoint", {{"args", {curve->path()}}, {"parameter", u}}).get<size_t>();
    const json made = ops::execute("curveControls", {{"args", {curve->path()}}, {"handles", handles}});
    // One control per point, in point order, unless handles have controls of their own.
    if (!handles && inserted < made.size())
        if (Node* c = g.find(made[inserted].get<std::string>()))
            return c->id();
    NodeId best = kNoNode;
    double bestD = 1e300;
    g.evaluate();
    for (const json& n : made)
        if (Node* c = g.find(n.get<std::string>()))
            if (const double d = glm::length(glm::dvec3(worldOf(*c)[3]) - mid); d < bestD)
            {
                bestD = d;
                best = c->id();
            }
    return best;
}

/** A bone CV on joint's bone, whatever kind of bend-bone chain it is in; kNoNode when it is in none. */
NodeId cvOnChain(Graph& g, NodeId joint, double at)
{
    if (Node* d = deformerOf(g, joint))
        return addCv(g, *d, joint, at);
    if (const NodeId hose = hoseOf(g, joint))
    {
        log::write(LogLevel::Info, "bendBoneCV: " + g.node(hose)->name() + " is a hose limb, which bends through its bend control");
        return hoseCv(g, hose);
    }
    if (const NodeId chain = spline::splineChainContaining(g, joint))
        return addSplineCv(g, chain, joint, at);
    return kNoNode;
}

/** Moves cv's partner to cv's position mirrored across the character's centre (its top joint's YZ plane). */
NodeId mirrorCv(Graph& g, NodeId cv, NodeId partner = kNoNode)
{
    if (partner == kNoNode)
        partner = partnerCv(g, cv);
    if (partner == kNoNode)
    {
        const std::string other = poses::mirrorNodeName(g.node(cv)->name());
        if (Node* n = other != g.node(cv)->name() ? g.find(other) : nullptr)
            partner = n->id();
    }
    if (partner == kNoNode)
        return kNoNode;
    g.evaluate();
    // The centre: the top joint above the CV, else the world origin.
    NodeId top = g.parent(cv);
    while (top != kNoNode && !rig::isJoint(*g.node(top)))
        top = g.parent(top);
    while (top != kNoNode && g.parent(top) != kNoNode && rig::isJoint(*g.node(g.parent(top))))
        top = g.parent(top);
    const glm::dmat4 centre = top != kNoNode ? worldOf(*g.node(top)) : glm::dmat4(1.0);
    glm::dvec3 local = glm::dvec3(glm::inverse(centre) * worldOf(*g.node(cv))[3]);
    local.x = -local.x;
    const glm::dvec3 world = glm::dvec3(centre * glm::dvec4(local, 1.0));
    const NodeId parent = g.parent(partner);
    const glm::dmat4 parentWorld = parent != kNoNode ? worldOf(*g.node(parent)) : glm::dmat4(1.0);
    g.setValue({partner, Transform::kTranslate, -1}, glm::dvec3(glm::inverse(parentWorld) * glm::dvec4(world, 1.0)));
    return partner;
}
} // namespace

void registerTypes()
{
    if (types::find(kType))
        return;
    const uint32_t keyable = AttrInput | AttrStorable | AttrKeyable;
    deformers::registerDeformer(
        {kType, "vector-spline",
         "Bends a mesh along the smooth curve through chains of joints (bend bones): each chain is one influence, the "
         "inside of a bend compresses evenly instead of pinching, stretching thins the mesh to keep its volume, and bone "
         "CVs reshape a bone without rebinding. Bake it to joints for Unreal.",
         {param("volume", PlugType::Float, 1.0, "0 keeps the mesh's thickness as chains stretch, 1 thins it to keep the volume.", keyable),
          param("evenBend", PlugType::Float, 1.0, "0 rides the curve, 1 spreads the inside and outside of a bend evenly.", keyable),
          param("matrix", PlugType::Matrix, glm::dmat4(1.0), "World matrices of the chains' joints, chains in order.", AttrInput | AttrHidden | AttrArray),
          param("cvMatrix", PlugType::Matrix, glm::dmat4(1.0), "World matrices of the bone CVs.", AttrInput | AttrHidden | AttrArray),
          param("binding", PlugType::String, std::string(), "The bind: chains, bone CVs and where each point sits.", AttrInput | AttrStorable | AttrHidden),
          param("restMatrix", PlugType::Matrix, glm::dmat4(1.0), "World matrices of the chains' joints at bind.", AttrInput | AttrStorable | AttrHidden | AttrArray),
          param("bindMeshMatrix", PlugType::Matrix, glm::dmat4(1.0), "The mesh's world matrix at bind.", AttrInput | AttrStorable | AttrHidden),
          param("chainWeights", PlugType::Data, DataPtr(), "Painted weights over the chains, one influence per chain; empty uses the bind.",
                AttrInput | AttrStorable | AttrHidden)},
         [] { return std::make_unique<BendBonesNode>(); }});

    NodeType cv{.name = kCvType,
                .base = types::find("transform"),
                .dag = true,
                .doc = "A bone CV of a bend bones chain: an extra point the bone's curve passes through. Move or key it to "
                       "reshape the bone; nothing is rebound.",
                .icon = "point",
                .create = types::find("transform")->create};
    types::registerType(std::move(cv));
    relocate::registerWorldAttrs(kType, {.matrices = {"restMatrix", "bindMeshMatrix"}});
    registerWeightHost();
    relocate::registerNeutral(kCvType);
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "bendBones",
        .doc = "Bind a mesh to chains of joints as bend bones: it bends along the smooth curve through each chain, each "
               "chain one influence, the joins between chains blending. Binds in the current pose. Returns the deformer.",
        .positionalDoc = "mesh, or a chain's joints then the mesh",
        .flags = {
            {"chains", "ch", T::List, nullptr, "Chains, each a list of joints from top to end ([[thigh, calf, foot], ...])."},
            {"volume", "v", T::Float, 1.0, "Thin the mesh as chains stretch to keep its volume.", {}, {}, 0.0, 1.0},
            {"evenBend", "eb", T::Float, 1.0, "Spread the inside and outside of bends evenly.", {}, {}, 0.0, 1.0},
            {"name", "n", T::String, nullptr, "Name for the deformer."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            // A mesh and chains, or (from the selection) a chain's joints top to end and the mesh.
            Node* meshNode = nullptr;
            json selected = json::array();
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "bendBones");
                if (n.type().isA("mesh") && !meshNode)
                    meshNode = &n;
                else if (rig::isJoint(n))
                    selected.push_back(n.path());
            }
            if (!meshNode)
                throw CommandError("bendBones: give a mesh");
            Node& mesh = *meshNode;
            const json chainsArg = a.has("chains") ? a.flags["chains"] : json::array({selected});
            if (!chainsArg.is_array() || chainsArg.empty() || (!a.has("chains") && selected.size() < 2))
                throw CommandError("bendBones: give chains, each a list of joints from top to end (or select a chain's joints, then the mesh)");
            g.evaluate();
            Bound b;
            std::vector<NodeId> joints;
            std::vector<bb::Curve> rest;
            for (const json& chain : chainsArg)
            {
                if (!chain.is_array() || chain.size() < 2)
                    throw CommandError("bendBones: a chain needs two joints or more");
                bb::Chain c;
                for (const json& j : chain)
                {
                    const NodeId id = jointArg(g, j, "bendBones");
                    joints.push_back(id);
                    const glm::dmat4 w = worldOf(*g.node(id));
                    b.rest.push_back(w);
                    c.position.push_back(glm::dvec3(w[3]));
                    c.rotation.push_back(orthonormal(glm::dmat3(w)));
                }
                b.sizes.push_back(int(chain.size()));
                const bb::Curve first = bb::build(c);
                const std::vector<glm::dvec3> normals = bb::restNormals(c, first);
                b.normals.insert(b.normals.end(), normals.begin(), normals.end());
                rest.push_back(bb::build(c, normals));
            }
            const MeshPtr m = mesh.as<MeshPtr>(MeshNode::kOutMesh);
            if (!m)
                throw CommandError("bendBones: " + mesh.path() + " has no mesh to bind");
            const glm::dmat4 toWorld = worldOf(mesh);
            b.meshWorld = toWorld;
            std::vector<glm::dvec3> points;
            for (const glm::dvec3& p : m->positions())
                points.push_back(glm::dvec3(toWorld * glm::dvec4(p, 1.0)));
            b.binding = bb::bindPoints(rest, points);

            const std::vector<NodeId> made = deformers::addDeformer(g, kType, {mesh.id()}, a.has("name") ? a.get<std::string>("name") : "BendBones");
            if (made.empty())
                throw CommandError("bendBones: could not add the deformer");
            Node& d = *g.node(made.front());
            const int matrix = d.type().attrIndex("matrix");
            for (size_t i = 0; i < joints.size(); ++i)
                g.connect({joints[i], Transform::kWorldMatrix, -1}, {d.id(), matrix, -1, int(i)});
            g.setValue({d.id(), d.type().attrIndex("volume"), -1}, a.get<double>("volume", 1.0));
            g.setValue({d.id(), d.type().attrIndex("evenBend"), -1}, a.get<double>("evenBend", 1.0));
            g.setValue({d.id(), d.type().attrIndex("binding"), -1}, boundJson(b).dump());
            const int restMatrix = d.type().attrIndex("restMatrix");
            for (size_t i = 0; i < b.rest.size(); ++i)
                g.setValue({d.id(), restMatrix, -1, int(i)}, b.rest[i]);
            g.setValue({d.id(), d.type().attrIndex("bindMeshMatrix"), -1}, b.meshWorld);
            g.setValue({d.id(), d.type().attrIndex("chainWeights"), -1}, DataPtr(bindWeights(g, d.id()).pack()));
            return d.path();
        },
    });
    ops::registerCommand({
        .name = "bendBoneCV",
        .doc = "Add a bone CV to a bend bones bone (or a spline chain bone, as a curve point with a control; a hose limb answers with its bend control), an extra point the curve passes through at a fraction "
               "along the bone from joint to its child. Move or key it to reshape the bone without rebinding. Returns the CV.",
        .positionalDoc = "deformer, or a joint",
        .flags = {
            {"joint", "j", T::String, nullptr, "The bone's first joint (it runs to the next joint of its chain)."},
            {"at", "at", T::Float, 0.5, "How far along the bone, 0 to 1.", {}, {}, 0.0, 1.0},
            {"mirror", "mi", T::Bool, true, "Also add one on the mirrored joint's bone (arm_l and arm_r), placed mirrored."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<std::string> objs = a.objects();
            Node* d = objs.empty() ? nullptr : g.find(objs.front());
            // A selected joint alone: its bone on whichever bendBones drives it.
            NodeId joint = kNoNode;
            const double at = std::clamp(a.get<double>("at", 0.5), 0.0, 1.0);
            if (d && rig::isJoint(*d) && !a.has("joint"))
            {
                joint = d->id();
                d = deformerOf(g, joint);
                // Spline chains and hose limbs are bend-bone chains too.
                if (!d)
                {
                    const NodeId cv = cvOnChain(g, joint, at);
                    if (cv == kNoNode)
                        throw CommandError("bendBoneCV: " + g.node(joint)->path() + " is in no bend bones, spline chain or hose limb");
                    if (a.get<bool>("mirror", true))
                        if (const NodeId other = mirrorJoint(g, joint); other != kNoNode && other != joint)
                            if (const NodeId ocv = cvOnChain(g, other, at); ocv != kNoNode)
                                mirrorCv(g, cv, ocv);
                    return g.node(cv)->path();
                }
            }
            if (!d || d->type().name != kType)
                throw CommandError("bendBoneCV: give a bendBones deformer, or select a bend bones joint");
            if (joint == kNoNode && !a.has("joint"))
                throw CommandError("bendBoneCV: give the bone's first joint");
            if (joint == kNoNode)
                joint = jointArg(g, a.flags["joint"], "bendBoneCV");
            const NodeId cv = addCv(g, *d, joint, at);
            if (a.get<bool>("mirror", true))
                if (const NodeId other = mirrorJoint(g, joint); other != kNoNode)
                    if (Node* od = deformerOf(g, other))
                    {
                        addCv(g, *od, other, at);
                        mirrorCv(g, cv);
                    }
            return g.node(cv)->path();
        },
    });
    ops::registerCommand({
        .name = "mirrorBendBoneCVs",
        .doc = "Copy bone CVs to the other side: each CV's partner (on the mirrored joint's bone, same order along it) moves to "
               "its position mirrored across the character's centre. Returns the CVs moved.",
        .positionalDoc = "cvs",
        .flags = {},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> cvs;
            for (const std::string& name : a.objects())
                if (Node* n = g.find(name); n && n->type().isA("transform"))
                    cvs.push_back(n->id());
            if (cvs.empty())
                throw CommandError("mirrorBendBoneCVs: give bone CVs");
            json out = json::array();
            for (NodeId cv : cvs)
                if (const NodeId moved = mirrorCv(g, cv); moved != kNoNode)
                    out.push_back(g.node(moved)->path());
            return out;
        },
    });
    ops::registerCommand({
        .name = "bakeBendBones",
        .doc = "Bake a bendBones deformer to plain joints for Unreal: each bone gets segment joints along its curve, keyed on "
               "every sampled frame, and the mesh is skinned to them with weights fitted (SSDR) to the bend bones' result over "
               "the animation and random poses. The deformer is turned off so the viewport shows what Unreal gets. Returns "
               "{joints, skinCluster, rmse, maxError, frames, poses, unrealCheck}: the fit error in cm and the Unreal round-trip check. "
               "A mesh skinned to spline chain or hose joints is already joints: it gets the Unreal check alone.",
        .positionalDoc = "deformer or mesh",
        .flags = {
            {"segments", "sg", T::Int, 3, "Segment joints per bone.", {}, {}, 1.0, 16.0},
            {"start", "", T::Int, nullptr, "First frame (the playback start by default)."},
            {"end", "", T::Int, nullptr, "Last frame (the playback end by default)."},
            {"step", "", T::Int, 1, "Frames between samples.", {}, {}, 1.0},
            {"poses", "", T::Int, 24, "Random poses added to the fit so the weights hold beyond the animation.", {}, {}, 0.0},
            {"angle", "", T::Float, 45.0, "Degrees the random poses turn each joint, at most.", {}, {}, 0.0, 180.0},
            {"seed", "", T::Int, 1, "Seed of the random poses."},
            {"maxInfluences", "mi", T::Int, 4, "Influences per point.", {}, {}, 1.0, 12.0},
            {"iterations", "", T::Int, 3, "Fit passes.", {}, {}, 1.0},
            {"tolerance", "", T::Float, 0.01, "cm the Unreal check allows.", {}, {}, 0.0},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const std::vector<std::string> objs = a.objects();
            Node* d = objs.empty() ? nullptr : g.find(objs.front());
            if (d && d->type().isA("mesh"))
                for (NodeId id : modifiers::stack(g, d->id()))
                    if (g.node(id)->type().name == kType)
                        d = g.node(id);
            // A mesh skinned to spline chain or hose joints is already plain joints: only the Unreal check is left to do.
            if (d && d->type().isA("mesh") && skin::find(g, d->id()) != kNoNode)
            {
                std::vector<NodeId> chained;
                for (const skin::Influence& i : skin::influences(g, skin::find(g, d->id())))
                    if (i.joint != kNoNode && spline::splineChainContaining(g, i.joint))
                        chained.push_back(i.joint);
                if (!chained.empty())
                {
                    const anim::Playback& pb = anim::playback();
                    const uecheck::ExportCheck c = uecheck::checkExport(g, {d->id()}, false, pb.minTime, pb.maxTime, pb.fps, "bakeBendBones");
                    json mismatches = json::array();
                    for (const uecheck::Mismatch& m : c.report.mismatches)
                        mismatches.push_back({{"joint", g.node(m.joint)->path()}, {"error", m.error}, {"time", m.time}});
                    log::write(LogLevel::Info, "bakeBendBones: " + d->name() + " is skinned to spline chain joints, which export as they are; checked them for Unreal");
                    return {{"joints", json::array()}, {"alreadyJoints", true}, {"unrealCheck", {{"worst", c.report.worst}, {"frames", c.report.frames}, {"mismatches", mismatches}, {"error", c.error}}}};
                }
            }
            if (!d || d->type().name != kType)
                throw CommandError("bakeBendBones: give a bendBones deformer or a mesh it deforms");
            return bake(g, *d, a);
        },
    });

    ops::registerCommand({
        .name = "unrealBendBones",
        .doc = "Bend the mesh in Unreal live as bend bones bend it here: LilaLink binds every LOD of the skeletal mesh to the "
               "chains (each vertex takes the chain weights of the point at its place, painted ones included; LOD vertices "
               "with no point there take the nearest point's) and makes "
               "the Lila Bend Bones Deformer Graph its default deformer. Run it on the mesh exported from Bake Bend Bones, "
               "whose skin stays the fallback where the deformer doesn't run. Returns {skeletalMesh, deformer, unreal}: the "
               "vertices bound, how many matched a point, the rest pose error, warnings.",
        .positionalDoc = "deformer or mesh",
        .flags = {{"skeletalMesh", "sm", ArgType::String, nullptr, "The skeletal mesh asset. Default: the mesh's mapping."},
                  {"deformer", "df", ArgType::String, "/Game/Lila/DG_LilaBendBones", "The Deformer Graph asset to use, made when missing."},
                  {"dryRun", "dry", ArgType::Bool, false, "Return what would be sent without connecting to Unreal."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            Node* d = deformerArg(g, a, "unrealBendBones");
            std::string mesh = a.get<std::string>("skeletalMesh", "");
            if (mesh.empty() && ops::find("unrealMappings"))
            {
                const NodeId target = modifiers::meshOf(g, d->id());
                auto holds = [&](NodeId root) {
                    for (NodeId n = target; n != kNoNode; n = g.parent(n))
                        if (n == root)
                            return true;
                    return false;
                };
                for (const json& m : ops::execute("unrealMappings", json::object()))
                    if (Node* mapped = g.find(m.value("node", "")); mapped && m.value("kind", "") == "skeletal" && holds(mapped->id()))
                        mesh = m.value("contentPath", "");
            }
            if (mesh.empty())
                throw CommandError("unrealBendBones: give skeletalMesh, or map the mesh's character with unrealMap kind=skeletal");
            const json payload = unrealPayload(g, *d, mesh, a.get<std::string>("deformer", "/Game/Lila/DG_LilaBendBones"));
            if (a.get<bool>("dryRun"))
                return {{"skeletalMesh", mesh}, {"deformer", payload["deformer"]}, {"payload", payload}};
            const std::string code = "import importlib, lila_link_bend_bones\nimportlib.reload(lila_link_bend_bones)\nlila_link_bend_bones.setup_and_print(" +
                                     json(payload.dump()).dump() + ")\n";
            const json r = ops::execute("unrealCall", {{"args", {"runPython"}}, {"params", {{"code", code}}}, {"timeoutMs", 300000}});
            if (!r.value("ok", false))
                throw CommandError("unrealBendBones: " + r.value("error", std::string("Unreal didn't answer")));
            json result;
            std::string log;
            for (const json& line : r.value("result", json::object()).value("log", json::array()))
            {
                const std::string text = line.value("text", "");
                if (text.rfind("LILA_RESULT:", 0) == 0)
                    result = json::parse(text.substr(12), nullptr, false);
                else
                    log += text + "\n";
            }
            if (!result.is_object())
                throw CommandError("unrealBendBones: Unreal didn't bind the mesh" + (log.empty() ? std::string() : ":\n" + log));
            if (const json errors = result.value("errors", json::array()); !errors.empty())
                throw CommandError("unrealBendBones: " + errors[0].get<std::string>());
            for (const json& w : payload["warnings"])
                result["warnings"].push_back(w);
            for (const json& w : result.value("warnings", json::array()))
                log::write(LogLevel::Warning, "unrealBendBones: " + w.get<std::string>());
            return {{"skeletalMesh", mesh}, {"deformer", payload["deformer"]}, {"unreal", result}};
        },
    });
}
} // namespace lila::scene::bendbones
