#include "retarget/SceneRetarget.h"

#include "retarget/Semantic.h"

#include "core/Profile.h"
#include "math/MathCore.h"
#include "scene/Anim.h"
#include "scene/AnimCurve.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/Relocate.h"
#include "scene/Rig.h"
#include "scene/RigElements.h"
#include "scene/RotationCurve.h"
#include "scene/Take.h"
#include "scene/Transform.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <set>
#include <stdexcept>

namespace lila::retarget
{
using namespace scene;

namespace
{
enum RigAttr
{
    kRigSkeleton,
    kRigDefinition,
};
enum RetargeterAttr
{
    kSourceSkeleton,
    kTargetSkeleton,
    kSettings,
    kSourcePose,
    kTargetPose,
};

bool isType(const Node* n, const char* type) { return n && n->type().name == type; }

NodeId sourceNode(Graph& g, NodeId node, int attr)
{
    const std::optional<PlugRef> src = g.source({node, attr, -1});
    return src ? src->node : kNoNode;
}

json readJson(const Node& n, int attr)
{
    const std::string& text = n.as<std::string>(attr);
    if (text.empty())
        return json();
    json j = json::parse(text, nullptr, false);
    return j.is_discarded() ? json() : j;
}

json poseJson(const Skeleton& s, const Pose& p)
{
    json rot = json::object(), tr = json::object();
    for (size_t i = 0; i < s.size(); ++i)
    {
        const glm::dquat& q = p.rotation[i];
        rot[s.names[i]] = json::array({q.x, q.y, q.z, q.w});
        tr[s.names[i]] = json::array({p.translate[i].x, p.translate[i].y, p.translate[i].z});
    }
    return {{"poseRotation", rot}, {"poseTranslate", tr}};
}
} // namespace

int SceneSkeleton::indexOf(NodeId joint) const
{
    auto it = std::find(joints.begin(), joints.end(), joint);
    return it == joints.end() ? -1 : int(it - joints.begin());
}

SceneSkeleton sceneSkeleton(Graph& g, NodeId joint)
{
    const Node* n = g.node(joint);
    if (!n || !rig::isJoint(*n))
        throw std::invalid_argument((n ? n->path() : std::string("a node")) + " is not a joint");
    NodeId top = joint;
    while (g.parent(top) != kNoNode && rig::isJoint(*g.node(g.parent(top))))
        top = g.parent(top);
    g.evaluate();

    SceneSkeleton out;
    out.top = top;
    std::vector<std::pair<NodeId, int>> stack{{top, -1}};
    std::set<std::string> seen;
    while (!stack.empty())
    {
        const auto [id, parent] = stack.back();
        stack.pop_back();
        const Node& j = *g.node(id);
        glm::dmat4 before, after;
        Transform::splitLocal(j, before, after);
        const glm::dmat4 pre = glm::translate(glm::dmat4(1.0), -j.as<glm::dvec3>(Transform::kTranslate)) * before;
        std::string name = bareName(j.name());
        if (!seen.insert(name).second)
            name = g.fullPath(id);
        const int self = out.skeleton.add(name, parent, pre, after);
        if (parent < 0)
            out.skeleton.parentWorld[size_t(self)] = j.as<glm::dmat4>(Transform::kParentMatrix);
        out.joints.push_back(id);
        const std::vector<NodeId>& kids = g.children(id);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            if (const Node* c = g.node(*it); c && rig::isJoint(*c))
                stack.push_back({*it, self});
    }
    return out;
}

Pose currentPose(Graph& g, const SceneSkeleton& s)
{
    g.evaluate();
    Pose p;
    for (NodeId id : s.joints)
    {
        const Node& j = *g.node(id);
        p.rotation.push_back(glm::normalize(glm::quat_cast(glm::dmat3(static_cast<const Transform&>(j).rotation()))));
        p.translate.push_back(j.as<glm::dvec3>(Transform::kTranslate));
    }
    return p;
}

Frames sample(Graph& g, const SceneSkeleton& s, double start, double end, double fps)
{
    LILA_ZONE();
    Frames out;
    out.fps = fps > 0.0 ? fps : 30.0;
    const long long frames = std::max<long long>(0, std::llround((end - start) * out.fps));
    const double was = g.time();
    out.world.reserve(size_t(frames + 1));
    std::vector<std::pair<std::string, PlugRef>> intents;
    for (const anim::AnimatedPlug& ap : anim::animatedPlugs(g, s.top))
        if (const std::string name = g.attr(ap.plug).name; name.starts_with("plant") && ap.plug.component < 0)
            intents.push_back({name, ap.plug});
    for (long long f = 0; f <= frames; ++f)
    {
        g.setTime(start + double(f) / out.fps, false);
        g.evaluateUpstream(s.joints);
        std::vector<glm::dmat4> w;
        w.reserve(s.joints.size());
        for (NodeId id : s.joints)
            w.push_back(g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix));
        out.world.push_back(std::move(w));
        for (const auto& [name, plug] : intents)
        {
            const PlugValue v = g.value(plug);
            out.channels[name].push_back(std::holds_alternative<double>(v) ? std::get<double>(v)
                                         : std::holds_alternative<bool>(v) ? double(std::get<bool>(v))
                                                                           : 0.0);
        }
    }
    g.setTime(was);
    g.evaluate();
    return out;
}

std::optional<std::pair<double, double>> keyedRange(Graph& g, const SceneSkeleton& s)
{
    const std::vector<double> t = anim::keyTimes(g, s.joints);
    if (t.size() < 2)
        return std::nullopt;
    return std::make_pair(t.front(), t.back());
}

// Rig nodes ----------------------------------------------------------------------------------------------------------

NodeId rigNodeFor(Graph& g, NodeId top)
{
    for (NodeId id : g.nodes())
        if (isType(g.node(id), kRigType) && sourceNode(g, id, kRigSkeleton) == top)
            return id;
    return kNoNode;
}

NodeId rigSkeletonTop(Graph& g, NodeId rigNode) { return sourceNode(g, rigNode, kRigSkeleton); }

Rig rigOf(Graph& g, NodeId rigNode)
{
    const Node* n = g.node(rigNode);
    if (!isType(n, kRigType))
        throw std::invalid_argument("not a retarget rig");
    const json j = readJson(*n, kRigDefinition);
    return j.is_object() ? Rig::fromJson(j) : Rig{};
}

void setRig(Graph& g, NodeId rigNode, const Rig& rig) { g.setValue({rigNode, kRigDefinition}, rig.toJson().dump()); }

NodeId characterizeSkeleton(Graph& g, NodeId joint, const std::string& name)
{
    const SceneSkeleton s = sceneSkeleton(g, joint);
    CharacterizeOptions o;
    const math::AxisBasis& basis = math::axisBasis();
    o.up = basis.up;
    o.forward = basis.forward;
    o.skip.resize(s.joints.size());
    for (size_t i = 0; i < s.joints.size(); ++i)
        o.skip[i] = !rigel::isDeform(*g.node(s.joints[i]));
    const Rig rig = characterize(s.skeleton, currentPose(g, s), o);
    NodeId id = rigNodeFor(g, s.top);
    if (id == kNoNode)
    {
        Node* n = g.createNode(*types::find(kRigType), name.empty() ? g.node(s.top)->name() + "_retargetRig" : name);
        id = n->id();
        g.connect({s.top, Transform::kWorldMatrix, -1}, {id, kRigSkeleton, -1});
    }
    setRig(g, id, rig);
    return id;
}

NodeId ensureRig(Graph& g, NodeId top)
{
    const NodeId id = rigNodeFor(g, top);
    return id != kNoNode ? id : characterizeSkeleton(g, top);
}

// Retargeter nodes ---------------------------------------------------------------------------------------------------

NodeId retargeterFor(Graph& g, NodeId sourceTop, NodeId targetTop)
{
    for (NodeId id : g.nodes())
        if (isType(g.node(id), kRetargeterType) && sourceNode(g, id, kSourceSkeleton) == sourceTop && sourceNode(g, id, kTargetSkeleton) == targetTop)
            return id;
    return kNoNode;
}

NodeId sourceTopOf(Graph& g, NodeId r) { return sourceNode(g, r, kSourceSkeleton); }
NodeId targetTopOf(Graph& g, NodeId r) { return sourceNode(g, r, kTargetSkeleton); }

Settings settingsOf(Graph& g, NodeId r)
{
    const Node* n = g.node(r);
    if (!isType(n, kRetargeterType))
        throw std::invalid_argument("not a retargeter");
    const json j = readJson(*n, kSettings);
    return j.is_object() ? Settings::fromJson(j) : Settings{};
}

void setSettings(Graph& g, NodeId r, const Settings& s) { g.setValue({r, kSettings}, s.toJson().dump()); }

Rig pairedRig(Graph& g, NodeId r, bool target)
{
    const NodeId top = target ? targetTopOf(g, r) : sourceTopOf(g, r);
    if (top == kNoNode)
        throw std::invalid_argument(std::string("the retargeter has no ") + (target ? "target" : "source") + " skeleton");
    const NodeId rn = rigNodeFor(g, top);
    if (rn == kNoNode)
        throw std::invalid_argument(g.node(top)->path() + " has no retarget rig; characterize it first");
    Rig rig = rigOf(g, rn);
    const json pose = readJson(*g.node(r), target ? kTargetPose : kSourcePose);
    if (pose.is_object())
    {
        const Rig overrides = Rig::fromJson(json{{"chains", json::array()}, {"poseRotation", pose.value("poseRotation", json::object())},
                                                 {"poseTranslate", pose.value("poseTranslate", json::object())}});
        for (const auto& [k, v] : overrides.poseRotation)
            rig.poseRotation[k] = v;
        for (const auto& [k, v] : overrides.poseTranslate)
            rig.poseTranslate[k] = v;
    }
    return rig;
}

void setPairedPose(Graph& g, NodeId r, bool target, const Skeleton& s, const Pose& pose)
{
    g.setValue({r, target ? kTargetPose : kSourcePose}, poseJson(s, pose).dump());
}

void alignPairedPose(Graph& g, NodeId r)
{
    const SceneSkeleton src = sceneSkeleton(g, sourceTopOf(g, r)), tgt = sceneSkeleton(g, targetTopOf(g, r));
    const Pose aligned = alignRetargetPose(src.skeleton, pairedRig(g, r, false), currentPose(g, src), tgt.skeleton, pairedRig(g, r, true),
                                           currentPose(g, tgt), settingsOf(g, r));
    setPairedPose(g, r, true, tgt.skeleton, aligned);
}

NodeId createRetargeter(Graph& g, NodeId sourceTop, NodeId targetTop, const std::string& name, bool align)
{
    if (sourceTop == targetTop)
        throw std::invalid_argument("the source and target are the same skeleton");
    const NodeId sr = ensureRig(g, sourceTop), tr = ensureRig(g, targetTop);
    Node* n = g.createNode(*types::find(kRetargeterType),
                           name.empty() ? g.node(sourceTop)->name() + "_to_" + g.node(targetTop)->name() : name);
    const NodeId id = n->id();
    g.connect({sourceTop, Transform::kWorldMatrix, -1}, {id, kSourceSkeleton, -1});
    g.connect({targetTop, Transform::kWorldMatrix, -1}, {id, kTargetSkeleton, -1});
    setSettings(g, id, autoMap(rigOf(g, sr), rigOf(g, tr)));
    if (align)
        alignPairedPose(g, id);
    return id;
}

// Keying -------------------------------------------------------------------------------------------------------------

void keyJoint(Graph& g, NodeId joint, const std::vector<double>& times, const std::vector<glm::dvec3>* translate, const std::vector<glm::dvec3>* euler)
{
    if (times.empty())
        return;
    if (translate)
        for (int k = 0; k < 3; ++k)
        {
            anim::Curve c;
            for (size_t f = 0; f < times.size(); ++f)
                c.keys.push_back({.time = times[f], .value = (*translate)[f][k], .in = anim::Tangent::Linear, .out = anim::Tangent::Linear});
            const PlugRef plug{joint, Transform::kTranslate, k};
            anim::setCurve(g, anim::setKey(g, plug, times.front(), c.keys.front().value), c);
        }
    if (!euler)
        return;
    const PlugRef rotate{joint, Transform::kRotate, -1};
    if (anim::keysAsRotation(g, rotate))
    {
        const NodeId curve = anim::setRotationKey(g, rotate, times.front(), euler->front());
        anim::RotationCurve rc = anim::rotationCurveOf(*g.node(curve));
        rc.keys.clear();
        for (size_t f = 0; f < times.size(); ++f)
        {
            anim::RotKey k = rc.makeKey(times[f], (*euler)[f]);
            k.in = k.out = anim::Tangent::Linear;
            rc.keys.push_back(k);
        }
        anim::setRotationCurve(g, curve, rc);
        return;
    }
    for (int k = 0; k < 3; ++k)
    {
        anim::Curve c;
        for (size_t f = 0; f < times.size(); ++f)
            c.keys.push_back({.time = times[f], .value = (*euler)[f][k], .in = anim::Tangent::Linear, .out = anim::Tangent::Linear});
        const PlugRef plug{joint, Transform::kRotate, k};
        anim::setCurve(g, anim::setKey(g, plug, times.front(), c.keys.front().value), c);
    }
}

namespace
{
NodeId takeFor(Graph& g, NodeId top, const std::string& name)
{
    for (NodeId t : anim::takes(g))
        if (g.node(t)->name() == name && sourceNode(g, t, anim::TakeNode::kRoot) == top)
            return t;
    return kNoNode;
}

bool animated(Graph& g, const std::vector<NodeId>& joints)
{
    for (NodeId j : joints)
        if (!anim::animatedPlugs(g, j).empty())
            return true;
    return false;
}

/** Makes `name` the target's active take, replacing one of that name; existing animation first goes to its own take. */
std::string prepareTake(Graph& g, const SceneSkeleton& tgt, const std::string& name, double start, double end, double fps)
{
    if (const NodeId old = takeFor(g, tgt.top, name); old != kNoNode)
        anim::deleteTake(g, old);
    const std::string root = g.path(tgt.top);
    bool hasTake = false;
    for (NodeId t : anim::takes(g))
        hasTake = hasTake || sourceNode(g, t, anim::TakeNode::kRoot) == tgt.top;
    if (!hasTake && animated(g, tgt.joints))
    {
        const auto range = keyedRange(g, tgt);
        anim::createTake(g, g.node(tgt.top)->name() + "_original",
                         {.root = root, .start = range ? range->first : start, .end = range ? range->second : end, .fps = fps});
    }
    const NodeId take = anim::createTake(g, name, {.root = root, .start = start, .end = end, .fps = fps});
    anim::setActiveTake(g, take);
    return g.node(take)->name();
}

/** Copies animated attributes (not translate, rotate or scale) from one node to another, by name or as mapped. */
int copyCurves(Graph& g, NodeId from, NodeId to, const Settings& s)
{
    int copied = 0;
    const Node& dst = *g.node(to);
    for (const anim::AnimatedPlug& ap : anim::animatedPlugs(g, from))
    {
        const std::string attr = g.attr(ap.plug).name;
        if (attr == "translate" || attr == "rotate" || attr == "scale" || ap.plug.attr == Transform::kTranslate || ap.plug.attr == Transform::kRotate ||
            ap.plug.attr == Transform::kScale || !anim::isCurve(*g.node(ap.curve)))
            continue;
        std::string want;
        for (const CurveMap& m : s.curves)
            if (m.source == attr)
                want = m.target;
        if (want.empty() && s.copyCurves)
            want = attr;
        const int a = want.empty() ? -1 : dst.type().attrIndex(want);
        if (a < 0)
            continue;
        const anim::Curve c = anim::curveOf(*g.node(ap.curve));
        if (c.keys.empty())
            continue;
        const PlugRef plug{to, a, ap.plug.component};
        try
        {
            anim::setCurve(g, anim::setKey(g, plug, c.keys.front().time, c.keys.front().value), c);
            ++copied;
        }
        catch (const std::invalid_argument&)
        {
            // Driven by something other than a curve: leave it.
        }
    }
    return copied;
}
} // namespace

json RunReport::toJson() const
{
    json p = json::object();
    for (const auto& [chain, count] : planted)
        p[chain] = count;
    return {{"take", take},       {"mode", mode}, {"frames", frames},   {"start", start},           {"end", end}, {"fps", fps}, {"scale", scale},
            {"ikError", ikError}, {"notes", notes}, {"plantedFrames", p}, {"curvesCopied", curvesCopied}};
}

namespace
{
/** Keys a result onto the target, into opts.take when it names one, and fills in the report. */
void keyInto(Graph& g, const SceneSkeleton& tgt, const Rig& tgtRig, const Pose& tgtNow, const Result& res, const Settings& settings, NodeId sTop,
             const RunOptions& opts, RunReport& rep)
{
    rep.frames = int(res.poses.size());
    rep.scale = res.scale;
    rep.ikError = res.ikError;
    rep.notes = res.notes;
    if (res.poses.empty())
        return;

    std::vector<double> times(res.poses.size());
    for (size_t f = 0; f < times.size(); ++f)
        times[f] = rep.start + double(f) / rep.fps;

    if (!opts.take.empty())
        rep.take = prepareTake(g, tgt, opts.take, rep.start, rep.end, rep.fps);

    // Keys go on plain curves, never an active animation layer's.
    std::vector<NodeId> layers;
    for (NodeId l : anim::layers(g))
        if (const int a = g.node(l)->type().attrIndex("active"); a >= 0 && g.node(l)->as<bool>(a))
        {
            layers.push_back(l);
            g.setValue({l, a, -1}, false);
        }
    for (size_t i = 0; i < tgt.joints.size(); ++i)
    {
        const NodeId id = tgt.joints[i];
        const Node& j = *g.node(id);
        const bool wasAnimated = !anim::animatedPlugs(g, id).empty();
        bool turns = wasAnimated, moves = false;
        for (const Pose& p : res.poses)
        {
            turns = turns || std::abs(glm::dot(p.rotation[i], tgtNow.rotation[i])) < 1.0 - 1e-12;
            moves = moves || glm::length(p.translate[i] - tgtNow.translate[i]) > 1e-6;
        }
        if (!turns && !moves)
            continue;
        const auto order = RotateOrder(j.as<int64_t>(Transform::kRotateOrder));
        std::vector<glm::dvec3> euler, translate;
        glm::dvec3 prev = j.as<glm::dvec3>(Transform::kRotate);
        for (const Pose& p : res.poses)
        {
            prev = xformops::closestEuler(glm::mat3_cast(p.rotation[i]), order, prev);
            euler.push_back(prev);
            translate.push_back(p.translate[i]);
        }
        try
        {
            keyJoint(g, id, times, moves ? &translate : nullptr, &euler);
        }
        catch (const std::invalid_argument& e)
        {
            rep.notes.push_back(j.path() + ": " + e.what());
        }
    }
    for (NodeId l : layers)
        g.setValue({l, g.node(l)->type().attrIndex("active"), -1}, true);

    rep.curvesCopied = sTop == kNoNode ? 0 : copyCurves(g, sTop, tgt.top, settings);

    for (const auto& [chainName, planted] : res.planted)
    {
        int count = 0;
        for (bool p : planted)
            count += p ? 1 : 0;
        rep.planted.push_back({chainName, count});
        if (!opts.lockChains || count == 0)
            continue;
        const Chain* c = tgtRig.chain(chainName);
        const int goal = c && !c->goal.empty() ? tgt.skeleton.find(c->goal) : -1;
        const NodeId ikChain = goal >= 0 ? rigel::chainForEffector(g, tgt.joints[size_t(goal)]) : kNoNode;
        if (ikChain == kNoNode)
            continue;
        for (size_t f = 0; f < planted.size(); ++f)
            if (planted[f] != (f > 0 && planted[f - 1]))
            {
                g.setTime(times[f]);
                g.evaluate();
                rigel::setLock(g, ikChain, times[f], planted[f]);
            }
    }
}
} // namespace

RunReport keyResult(Graph& g, const SceneSkeleton& tgt, const Rig& tgtRig, const Pose& tgtNow, const Result& res, double start, double fps,
                    const RunOptions& opts)
{
    RunReport rep;
    rep.mode = "fk";
    rep.fps = fps;
    rep.start = start;
    rep.end = start + double(std::max<size_t>(res.poses.size(), 1) - 1) / fps;
    keyInto(g, tgt, tgtRig, tgtNow, res, Settings{.copyCurves = false}, kNoNode, opts, rep);
    return rep;
}

RunReport run(Graph& g, NodeId r, const RunOptions& opts)
{
    LILA_ZONE();
    const NodeId sTop = sourceTopOf(g, r), tTop = targetTopOf(g, r);
    if (sTop == kNoNode || tTop == kNoNode)
        throw std::invalid_argument("the retargeter needs both a source and a target skeleton");
    const SceneSkeleton src = sceneSkeleton(g, sTop), tgt = sceneSkeleton(g, tTop);
    const Settings settings = settingsOf(g, r);
    const Rig srcRig = pairedRig(g, r, false), tgtRig = pairedRig(g, r, true);

    RunReport rep;
    const anim::Playback& pb = anim::playback();
    const auto keyed = keyedRange(g, src);
    rep.start = opts.start ? *opts.start : keyed ? keyed->first : pb.minTime;
    rep.end = opts.end ? *opts.end : keyed ? keyed->second : pb.maxTime;
    rep.fps = opts.fps > 0.0 ? opts.fps : pb.fps;
    if (rep.end < rep.start)
        throw std::invalid_argument("the end is before the start");

    const Pose tgtNow = currentPose(g, tgt);
    const Frames frames = sample(g, src, rep.start, rep.end, rep.fps);
    auto legs = [](const Rig& r) {
        int n = 0;
        for (const Chain& c : r.chains)
            n += c.role == Role::Leg && !c.goal.empty() ? 1 : 0;
        return n;
    };
    if (opts.mode != "auto" && opts.mode != "fk" && opts.mode != "semantic")
        throw std::invalid_argument("mode is auto, fk or semantic");
    const bool semantic = opts.mode == "semantic" || (opts.mode == "auto" && legs(srcRig) > 0 && legs(srcRig) != legs(tgtRig));
    rep.mode = semantic ? "semantic" : "fk";
    Result res;
    if (semantic)
    {
        PlantSettings plant;
        for (const ChainMap& m : settings.chains)
            if (const Chain* c = tgtRig.chain(m.target); c && c->role == Role::Leg)
                plant = m.plant;
        const semantic::Clip clip = semantic::encode(src.skeleton, srcRig, currentPose(g, src), frames, plant);
        res = semantic::apply(clip, tgt.skeleton, tgtRig, tgtNow, {.gait = semantic::gaitFromName(opts.gait), .secondary = opts.secondary, .armsAsForelegs = opts.armsAsForelegs});
    }
    else
    {
        const Retargeter rt(src.skeleton, srcRig, currentPose(g, src), tgt.skeleton, tgtRig, tgtNow, settings);
        res = rt.clip(frames);
    }
    keyInto(g, tgt, tgtRig, tgtNow, res, settings, sTop, opts, rep);
    return rep;
}

RunReport applySemantic(Graph& g, const semantic::Clip& clip, NodeId target, const RunOptions& opts)
{
    const SceneSkeleton tgt = sceneSkeleton(g, target);
    const Rig rig = rigOf(g, ensureRig(g, tgt.top));
    const Pose now = currentPose(g, tgt);
    RunReport rep;
    rep.mode = "semantic";
    rep.fps = clip.fps;
    rep.start = opts.start.value_or(0.0);
    rep.end = rep.start + double(std::max(clip.frames - 1, 0)) / clip.fps;
    const Result res = semantic::apply(clip, tgt.skeleton, rig, now, {.gait = semantic::gaitFromName(opts.gait)});
    keyInto(g, tgt, rig, now, res, Settings{.copyCurves = false}, kNoNode, opts, rep);
    return rep;
}

semantic::Clip encodeSkeleton(Graph& g, NodeId source, const RunOptions& opts)
{
    const SceneSkeleton src = sceneSkeleton(g, source);
    const Rig rig = rigOf(g, ensureRig(g, src.top));
    const anim::Playback& pb = anim::playback();
    const auto keyed = keyedRange(g, src);
    const double start = opts.start ? *opts.start : keyed ? keyed->first : pb.minTime;
    const double end = opts.end ? *opts.end : keyed ? keyed->second : pb.maxTime;
    return semantic::encode(src.skeleton, rig, currentPose(g, src), sample(g, src, start, end, opts.fps > 0.0 ? opts.fps : pb.fps));
}

void registerTypes()
{
    if (types::find(kRigType))
        return;
    const uint32_t stored = AttrInput | AttrStorable;
    NodeType rig;
    rig.name = kRigType;
    rig.icon = "skeleton";
    rig.doc = "What each part of a skeleton is for retargeting: the pelvis, chains with roles and sides, the retarget pose.";
    rig.ownAttrs = {
        {.name = "skeleton", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
         .doc = "Connected from the skeleton's top joint."},
        {.name = "definition", .type = PlugType::String, .defaultValue = std::string(), .flags = stored | AttrHidden,
         .doc = "The rig as JSON (editRetargetRig)."},
    };
    rig.create = [] { return std::make_unique<Node>(); };
    types::registerType(std::move(rig));

    NodeType rt;
    rt.name = kRetargeterType;
    rt.icon = "retarget";
    rt.doc = "Retargets one skeleton's motion onto another: chain mapping, root, FK, IK and the retarget poses of the pair.";
    rt.ownAttrs = {
        {.name = "sourceSkeleton", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
         .doc = "Connected from the source's top joint."},
        {.name = "targetSkeleton", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
         .doc = "Connected from the target's top joint."},
        {.name = "settings", .type = PlugType::String, .defaultValue = std::string(), .flags = stored | AttrHidden,
         .doc = "Settings as JSON (editRetargeter)."},
        {.name = "sourcePose", .type = PlugType::String, .defaultValue = std::string(), .flags = stored | AttrHidden,
         .doc = "The source's retarget pose for this pair, over its rig's."},
        {.name = "targetPose", .type = PlugType::String, .defaultValue = std::string(), .flags = stored | AttrHidden,
         .doc = "The target's retarget pose for this pair, over its rig's."},
    };
    rt.create = [] { return std::make_unique<Node>(); };
    types::registerType(std::move(rt));
    // Poses are joint-local and top joints keep their current place, so moving the scene leaves both right.
    relocate::registerNeutral(kRigType);
    relocate::registerNeutral(kRetargeterType);
}
} // namespace lila::retarget
