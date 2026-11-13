#include "AnimOps.h"
#include "Tracks.h"

#include "rig/Rig.h"

#include <lila/Command.h>
#include <lila/Node.h>
#include <lila/Transform.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace puppeteer::anim
{
namespace
{
namespace sa = lila::scene::anim;

bool isScalarCurve(const Node& n) { return sa::isCurve(n); }
bool isRotCurve(const Node& n) { return sa::isRotationCurve(n); }

/** Curves feeding plug, through layer blends (both inputs, or only inputA of control tracks) down to the base animation. */
void curvesDriving(Graph& g, const PlugRef& plug, std::set<NodeId>& seen, std::vector<NodeId>& out, bool withTracks, int depth = 0)
{
    if (depth > 64)
        return;
    auto visit = [&](const PlugRef& p) {
        auto src = g.source(p);
        if (!src)
            return;
        Node* n = g.node(src->node);
        if (!n)
            return;
        if (sa::isAnyCurve(*n))
        {
            if (seen.insert(n->id()).second)
                out.push_back(n->id());
        }
        else if (sa::isBlend(*n))
        {
            const bool vec = g.attr({n->id(), sa::BlendNode::kInputA, -1}).type == PlugType::Vec3;
            const bool skipB = !withTracks && isTrack(g, sa::layerOf(g, n->id()));
            for (int input : {int(sa::BlendNode::kInputA), int(sa::BlendNode::kInputB)})
            {
                if (skipB && input == sa::BlendNode::kInputB)
                    continue;
                curvesDriving(g, {n->id(), input, -1}, seen, out, withTracks, depth + 1);
                if (vec)
                    for (int c = 0; c < 3; ++c)
                        curvesDriving(g, {n->id(), input, c}, seen, out, withTracks, depth + 1);
            }
        }
    };
    visit(plug);
}

RotateOrder orderOf(const Node& n)
{
    const int a = n.type().attrIndex("rotateOrder");
    return a >= 0 ? RotateOrder(n.as<int64_t>(a)) : RotateOrder::XYZ;
}

glm::dquat quatFromJson(const json& r)
{
    return glm::normalize(glm::dquat(r[3].get<double>(), r[0].get<double>(), r[1].get<double>(), r[2].get<double>()));
}

glm::dquat rotationOf(const glm::dmat4& m)
{
    glm::dmat3 r(m);
    for (int i = 0; i < 3; ++i)
        r[i] /= std::max(glm::length(r[i]), 1e-300);
    return glm::normalize(glm::quat_cast(r));
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool isJoint(const Node& n) { return n.type().isA("joint"); }
} // namespace

Character resolveCharacter(Graph& g, NodeId node)
{
    if (!g.node(node))
        throw std::invalid_argument("no such node");
    if (const NodeId c = puppeteer::characterFor(g, node))
        if (Node* root = named(g, g.node(c)->as<std::string>(g.node(c)->type().attrIndex("root"))))
        {
            Character ch = poses::characterOf(g, root->id());
            ch.id = poseLibraryOf(g, c);
            if (const std::string& t = g.node(c)->as<std::string>(g.node(c)->type().attrIndex("templateId")); !t.empty())
                ch.templateId = t;
            return ch;
        }
    Character ch = poses::characterOf(g, node);
    if (!ch.root)
        throw std::invalid_argument(g.node(node)->path() + " is not part of a character");
    return ch;
}

std::vector<NodeId> characterCurves(Graph& g, const Character& ch, bool withTracks)
{
    std::set<NodeId> seen;
    std::vector<NodeId> out;
    for (NodeId id : ch.nodes)
    {
        Node* n = g.node(id);
        if (!n)
            continue;
        const auto& attrs = n->type().attrs;
        for (int i = 0; i < int(attrs.size()); ++i)
        {
            if (!(attrs[size_t(i)].flags & AttrKeyable))
                continue;
            curvesDriving(g, {id, i, -1}, seen, out, withTracks);
            if (attrs[size_t(i)].type == PlugType::Vec3)
                for (int c = 0; c < 3; ++c)
                    curvesDriving(g, {id, i, c}, seen, out, withTracks);
        }
    }
    return out;
}

std::vector<double> keyTimesOf(Graph& g, const std::vector<NodeId>& curves)
{
    std::vector<double> out;
    for (NodeId c : curves)
        if (Node* n = g.node(c))
            for (double t : sa::curveKeyTimes(*n))
                out.push_back(t);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return sameTime(a, b); }), out.end());
    return out;
}

double loopTimeOf(Graph& g, const std::vector<NodeId>& curves)
{
    double t = std::numeric_limits<double>::quiet_NaN();
    for (NodeId c : curves)
        if (Node* n = g.node(c); n && sa::curveLoops(*n))
        {
            const std::vector<double> k = sa::curveKeyTimes(*n);
            if (!k.empty() && (std::isnan(t) || k.back() > t))
                t = k.back();
        }
    return t;
}

void warpCurves(Graph& g, const std::vector<NodeId>& curves, const TimeWarp& w)
{
    if (w.knots.empty())
        return;
    for (NodeId c : curves)
    {
        Node* n = g.node(c);
        if (!n)
            continue;
        if (isRotCurve(*n))
        {
            sa::RotationCurve rc = sa::rotationCurveOf(*n);
            bool changed = false;
            for (sa::RotKey& k : rc.keys)
            {
                const double t = w.map(k.time);
                changed |= !sameTime(t, k.time);
                k.time = t;
            }
            if (changed)
                sa::setRotationCurve(g, c, rc);
        }
        else if (isScalarCurve(*n))
        {
            sa::Curve cv = sa::curveOf(*n);
            bool changed = false;
            for (sa::Key& k : cv.keys)
            {
                const double t = w.map(k.time);
                changed |= !sameTime(t, k.time);
                k.time = t;
            }
            if (changed)
                sa::setCurve(g, c, cv);
        }
    }
}

void removeKeysAt(Graph& g, const std::vector<NodeId>& curves, double t)
{
    for (NodeId c : curves)
        if (g.node(c))
            sa::removeKeys(g, c, t - kTimeEps, t + kTimeEps);
}

json poseAt(Graph& g, const std::vector<NodeId>& nodes, double t)
{
    json out = json::object();
    for (NodeId id : nodes)
    {
        Node* n = g.node(id);
        if (!n)
            continue;
        json vals = json::object();
        for (const sa::AnimatedPlug& ap : sa::animatedPlugs(g, id))
        {
            const AttrDef& a = n->type().attrs[size_t(ap.plug.attr)];
            const Node& curve = *g.node(ap.curve);
            const double v = sa::curveValueAt(curve, std::max(ap.plug.component, 0), t);
            if (ap.plug.component < 0)
            {
                vals[a.name] = v;
                continue;
            }
            if (!vals.contains(a.name))
            {
                json arr = json::array();
                for (int c = 0; c < 3; ++c)
                {
                    const PlugValue pv = g.value(sa::keyInput(g, {id, ap.plug.attr, c}));
                    arr.push_back(std::holds_alternative<double>(pv) ? std::get<double>(pv) : 0.0);
                }
                vals[a.name] = arr;
            }
            vals[a.name][size_t(ap.plug.component)] = v;
        }
        if (!vals.empty())
            out[poses::stripNamespace(n->name())] = vals;
    }
    return out;
}

KeyWriter::KeyWriter(Graph& g, const std::vector<NodeId>& scope, WriteMode mode) : mGraph(g), mMode(mode)
{
    for (NodeId id : scope)
        if (Node* n = g.node(id))
            mByName.emplace(poses::stripNamespace(n->name()), id);
}

void KeyWriter::scalar(const PlugRef& plug, double t, double v)
{
    Graph& g = mGraph;
    const NodeId curve = sa::curveFor(g, plug);
    if (!curve && !mMode.keyMissing)
    {
        if (!g.source(plug) && !g.isLocked(plug))
            g.setValue(plug, v);
        return;
    }
    if (mMode.absolute && curve)
    {
        sa::setKey(g, plug, t, v);
        ++mKeys;
        return;
    }
    if (curve && isScalarCurve(*g.node(curve)))
    {
        auto it = mScalars.find(curve);
        if (it == mScalars.end())
            it = mScalars.emplace(curve, sa::curveOf(*g.node(curve))).first;
        sa::Curve& c = it->second;
        if (int k = c.find(t, kTimeEps); k >= 0)
            c.keys[size_t(k)].value = v;
        else
            c.insert({.time = t, .value = v});
        ++mKeys;
        return;
    }
    if (curve)
        return;
    // A layered channel with nothing keyed on its layer yet: v is layer-local, so it keys that input itself.
    if (const PlugRef in = sa::keyInput(g, plug); in.node != plug.node && !mMode.absolute)
    {
        sa::setKey(g, in, t, v);
        ++mKeys;
        return;
    }
    try
    {
        sa::setKey(g, plug, t, v);
        ++mKeys;
    }
    catch (const std::invalid_argument&)
    {
        // Driven by something that isn't a curve (a constraint, an expression): left alone.
    }
}

void KeyWriter::rotation(NodeId node, int attr, double t, glm::dvec3 euler, const json& raw)
{
    Graph& g = mGraph;
    const NodeId curve = sa::curveFor(g, {node, attr, 0});
    const RotateOrder order = orderOf(*g.node(node));
    if (curve && isRotCurve(*g.node(curve)) && !mMode.absolute)
    {
        auto it = mRotations.find(curve);
        if (it == mRotations.end())
            it = mRotations.emplace(curve, sa::rotationCurveOf(*g.node(curve))).first;
        sa::RotationCurve& rc = it->second;
        if (raw.size() == 4)
            euler = eulerNear(glm::mat3_cast(quatFromJson(raw)), rc.order, rc.keys.empty() ? glm::dvec3(0.0) : rc.evaluate(t));
        if (int k = rc.find(t, kTimeEps); k >= 0)
            rc.setEuler(size_t(k), euler);
        else
            rc.insert(rc.makeKey(t, euler));
        mKeys += 3;
        return;
    }
    if (raw.size() == 4)
    {
        glm::dvec3 ref(0.0);
        for (int c = 0; c < 3; ++c)
            if (NodeId cc = sa::curveFor(g, {node, attr, c}))
                ref[c] = sa::curveValueAt(*g.node(cc), c, t);
            else if (const PlugValue pv = g.value({node, attr, c}); std::holds_alternative<double>(pv))
                ref[c] = std::get<double>(pv);
        euler = eulerNear(glm::mat3_cast(quatFromJson(raw)), order, ref);
    }
    if (!curve && mMode.keyMissing && sa::keysAsRotation(g, {node, attr, 0}))
    {
        try
        {
            sa::setRotationKey(g, {node, attr, -1}, t, euler);
            mKeys += 3;
        }
        catch (const std::invalid_argument&)
        {
        }
        return;
    }
    for (int c = 0; c < 3; ++c)
        scalar({node, attr, c}, t, euler[c]);
}

void KeyWriter::write(const json& pose, double t, const std::vector<std::string>& channels)
{
    for (const auto& [name, vals] : pose.items())
    {
        auto found = mByName.find(poses::stripNamespace(name));
        if (found == mByName.end() || !vals.is_object())
            continue;
        const NodeId id = found->second;
        const Node& n = *mGraph.node(id);
        for (const auto& [attrName, v] : vals.items())
        {
            if (!poses::channelWanted(channels, attrName))
                continue;
            const int ai = n.type().attrIndex(attrName);
            if (ai < 0 || !(n.type().attrs[size_t(ai)].flags & AttrKeyable))
                continue;
            const AttrDef& a = n.type().attrs[size_t(ai)];
            if (a.type == PlugType::Vec3)
            {
                if (!v.is_array() || v.size() < 3)
                    continue;
                if (attrName == "rotate")
                    rotation(id, ai, t, {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()}, v);
                else
                    for (int c = 0; c < 3; ++c)
                        scalar({id, ai, c}, t, v[size_t(c)].get<double>());
            }
            else if (v.is_number() || v.is_boolean())
                scalar({id, ai, -1}, t, v.is_boolean() ? (v.get<bool>() ? 1.0 : 0.0) : v.get<double>());
        }
    }
}

int KeyWriter::commit()
{
    for (auto& [id, c] : mScalars)
        if (mGraph.node(id))
            sa::setCurve(mGraph, id, c);
    for (auto& [id, rc] : mRotations)
        if (mGraph.node(id))
            sa::setRotationCurve(mGraph, id, rc);
    mScalars.clear();
    mRotations.clear();
    return mKeys;
}

void adoptIntoTake(Graph& g, const Character& ch)
{
    for (NodeId t : sa::takes(g))
        if (auto src = g.source({t, sa::TakeNode::kRoot, -1}); !src || src->node == ch.root)
            return;
    if (keyTimesOf(g, characterCurves(g, ch)).empty())
        return;
    const sa::Playback& pb = sa::playback();
    sa::createTake(g, "Take1", {.root = g.node(ch.root)->path(), .start = pb.minTime, .end = pb.maxTime, .fps = pb.fps});
}

poses::Pose libraryPose(const Character& ch, const std::string& name)
{
    auto e = poses::find(ch.id, name);
    if (!e)
        throw std::invalid_argument("the character " + ch.id + " has no '" + name + "' pose; save one with savePose");
    return e->pose;
}

json mirrorPose(Graph& g, const Character& ch, const json& pose, char axis)
{
    poses::Pose p;
    p.nodes = pose;
    return poses::mirrored(g, p, ch.nodes, axis).nodes;
}

namespace
{
NodeId marksNode(Graph& g, const Character& ch)
{
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().name == kMarksType && named(g, n->as<std::string>(n->type().attrIndex("character"))) == g.node(ch.root))
            return id;
    return kNoNode;
}
} // namespace

Marks marksOf(Graph& g, const Character& ch)
{
    Marks m;
    const NodeId id = marksNode(g, ch);
    if (!id)
        return m;
    const Node& n = *g.node(id);
    const json j = json::parse(n.as<std::string>(n.type().attrIndex("marks")), nullptr, false);
    if (!j.is_array())
        return m;
    for (const json& e : j)
        if (e.is_array() && e.size() == 2 && e[0].is_number() && e[1].is_string())
        {
            try
            {
                m.set(e[0].get<double>(), markFromName(e[1].get<std::string>()));
            }
            catch (const std::invalid_argument&)
            {
            }
        }
    return m;
}

void setMarks(Graph& g, const Character& ch, const Marks& marks)
{
    NodeId id = marksNode(g, ch);
    if (!id && marks.at.empty())
        return;
    if (!id)
    {
        const NodeType* t = types::find(kMarksType);
        if (!t)
            throw std::invalid_argument("key marks are unavailable");
        Node* n = g.createNode(*t, g.node(ch.root)->name() + "_keyMarks");
        id = n->id();
        g.setValue({id, n->type().attrIndex("character"), -1}, g.path(ch.root));
    }
    json j = json::array();
    for (const auto& [t, m] : marks.at)
        j.push_back(json::array({t, markName(m)}));
    const Node& n = *g.node(id);
    const int a = n.type().attrIndex("marks");
    if (n.as<std::string>(a) != j.dump())
        g.setValue({id, a, -1}, j.dump());
}

std::string labelFromName(const std::string& rawName)
{
    static const std::set<std::string> ue = {
        "root",      "pelvis",     "spine_01",   "spine_02",   "spine_03",  "spine_04",  "spine_05",   "neck_01",    "neck_02",
        "head",      "clavicle_l", "clavicle_r", "upperarm_l", "upperarm_r", "lowerarm_l", "lowerarm_r", "hand_l",   "hand_r",
        "thigh_l",   "thigh_r",    "calf_l",     "calf_r",     "foot_l",    "foot_r",    "ball_l",     "ball_r"};
    static const std::map<std::string, std::string> aliases = {
        {"hips", "pelvis"},          {"spine", "spine_01"},        {"spine1", "spine_02"},       {"spine2", "spine_03"},
        {"spine3", "spine_04"},      {"neck", "neck_01"},          {"neck1", "neck_02"},         {"leftshoulder", "clavicle_l"},
        {"rightshoulder", "clavicle_r"}, {"leftarm", "upperarm_l"}, {"rightarm", "upperarm_r"},  {"leftforearm", "lowerarm_l"},
        {"rightforearm", "lowerarm_r"}, {"lefthand", "hand_l"},    {"righthand", "hand_r"},      {"leftupleg", "thigh_l"},
        {"rightupleg", "thigh_r"},   {"leftleg", "calf_l"},        {"rightleg", "calf_r"},       {"leftfoot", "foot_l"},
        {"rightfoot", "foot_r"},     {"lefttoebase", "ball_l"},    {"righttoebase", "ball_r"},   {"l_thigh", "thigh_l"},
        {"r_thigh", "thigh_r"},      {"l_calf", "calf_l"},         {"r_calf", "calf_r"},         {"l_foot", "foot_l"},
        {"r_foot", "foot_r"}};
    const std::string n = lower(poses::stripNamespace(rawName));
    if (ue.contains(n))
        return n;
    if (auto it = aliases.find(n); it != aliases.end())
        return it->second;
    return {};
}

std::map<NodeId, std::string> jointLabels(Graph& g, const Character& ch)
{
    std::map<std::string, std::string> fromTemplate;
    if (!ch.templateId.empty() && ops::find("rigTemplateInfo"))
    {
        try
        {
            const json t = ops::execute("rigTemplateInfo", {{"args", {ch.templateId}}});
            for (const json& m : t.value("markers", json::array()))
                if (!m.value("label", std::string()).empty())
                    fromTemplate[m.value("joint", m.value("name", std::string()))] = m["label"].get<std::string>();
        }
        catch (const std::exception&)
        {
        }
    }
    std::map<NodeId, std::string> out;
    for (NodeId id : ch.nodes)
    {
        Node* n = g.node(id);
        if (!n || !isJoint(*n))
            continue;
        const std::string name = poses::stripNamespace(n->name());
        std::string label;
        if (auto it = fromTemplate.find(name); it != fromTemplate.end())
            label = it->second;
        else
            // A built skeleton may carry a prefix in front of the template's joint names.
            for (const auto& [joint, l] : fromTemplate)
                if (!joint.empty() && name.size() > joint.size() && name.compare(name.size() - joint.size(), joint.size(), joint) == 0)
                    label = l;
        if (label.empty())
            label = labelFromName(name);
        if (!label.empty())
            out[id] = label;
    }
    return out;
}

namespace
{
/** Joints under root, parents first. */
void jointsDepthFirst(Graph& g, NodeId id, std::vector<NodeId>& out)
{
    if (Node* n = g.node(id); n && isJoint(*n))
        out.push_back(id);
    for (NodeId c : g.children(id))
        jointsDepthFirst(g, c, out);
}

glm::dmat4 worldOf(Graph& g, NodeId id)
{
    const Node& n = *g.node(id);
    const int a = n.type().attrIndex("worldMatrix");
    return a >= 0 ? std::get<glm::dmat4>(g.value({id, a, -1})) : glm::dmat4(1.0);
}

/** The joints the generator keys: everything it moves. Others follow their parents. */
bool walkKeyed(const std::string& label)
{
    static const std::set<std::string> keyed = {"pelvis", "head", "thigh_l", "thigh_r", "calf_l", "calf_r", "foot_l", "foot_r",
                                                "ball_l", "ball_r", "upperarm_l", "upperarm_r", "lowerarm_l", "lowerarm_r"};
    return keyed.contains(label) || label.rfind("spine_", 0) == 0;
}
} // namespace

WalkResult generateWalk(Graph& g, const Character& ch, const WalkParams& params, const std::string& takeName, const std::string& restPose)
{
    const std::map<NodeId, std::string> labels = jointLabels(g, ch);
    const std::string rootName = g.node(ch.root)->path();
    const double frames = params.frames();
    const double fps = params.fps;

    adoptIntoTake(g, ch);
    const NodeId take = sa::createTake(g, takeName.empty() ? (params.run ? "Run" : "Walk") : takeName,
                                       {.root = rootName, .start = 0.0, .end = frames / fps, .fps = fps, .loop = true});
    sa::setActiveTake(g, take);

    std::string poseName = restPose;
    if (poseName.empty())
        for (const char* n : {"ref", "bind"})
            if (poses::find(ch.id, n))
            {
                poseName = n;
                break;
            }
    if (!poseName.empty())
    {
        poses::ApplyOptions opts;
        opts.scope = ch.nodes;
        opts.nameSpace = ch.nameSpace;
        poses::apply(g, libraryPose(ch, poseName), opts);
    }
    g.evaluate();

    std::vector<NodeId> joints;
    jointsDepthFirst(g, ch.root, joints);
    std::map<NodeId, int> index;
    std::vector<RigJoint> rest;
    for (NodeId j : joints)
    {
        index[j] = int(rest.size());
        const glm::dmat4 w = worldOf(g, j);
        RigJoint r;
        auto l = labels.find(j);
        r.label = l == labels.end() ? std::string() : l->second;
        auto p = index.find(g.parent(j));
        r.parent = p == index.end() ? -1 : p->second;
        r.position = glm::dvec3(w[3]);
        r.rotation = rotationOf(w);
        rest.push_back(r);
    }
    WalkSolver solver(rest, params);

    struct Keyed
    {
        NodeId node;
        int index;
        glm::dquat pre, post; // the local rotation is pre * R * post
        RotateOrder order;
        glm::dvec3 euler;
        int rotate;
        bool translate;
        glm::dvec3 restTranslate;
        glm::dmat4 parentInverse;
    };
    std::vector<Keyed> keyed;
    for (NodeId j : joints)
    {
        const RigJoint& r = rest[size_t(index[j])];
        if (!walkKeyed(r.label))
            continue;
        const Node& n = *g.node(j);
        Keyed k{.node = j, .index = index[j], .order = orderOf(n)};
        k.rotate = n.type().attrIndex("rotate");
        const int ra = n.type().attrIndex("rotateAxis");
        k.post = ra >= 0 ? rotationOf(eulerToMatrix(n.as<glm::dvec3>(ra), RotateOrder::XYZ)) : glm::dquat(1, 0, 0, 0);
        k.pre = glm::normalize(rotationOf(Transform::localMatrix(n, glm::dmat4(1.0))) * glm::conjugate(k.post));
        k.euler = n.as<glm::dvec3>(k.rotate);
        k.translate = r.label == "pelvis";
        k.restTranslate = n.as<glm::dvec3>(n.type().attrIndex("translate"));
        k.parentInverse = glm::inverse(g.parent(j) ? worldOf(g, g.parent(j)) : glm::dmat4(1.0));
        keyed.push_back(k);
    }

    WalkResult res;
    res.take = g.node(take)->path();
    res.frames = int(frames);
    for (const Keyed& k : keyed)
        res.joints.push_back(g.node(k.node)->path());
    const int translate = Transform::kTranslate;
    for (int f : walkKeyFrames(params))
    {
        const WalkPose pose = solver.pose(double(f) / frames);
        const double t = double(f) / fps;
        for (Keyed& k : keyed)
        {
            const int pi = rest[size_t(k.index)].parent;
            const glm::dquat parentWorld = pi >= 0 ? pose.rotation[size_t(pi)] : rotationOf(glm::inverse(k.parentInverse));
            const glm::dquat local = glm::conjugate(parentWorld) * pose.rotation[size_t(k.index)];
            const glm::dquat r = glm::normalize(glm::conjugate(k.pre) * local * glm::conjugate(k.post));
            k.euler = eulerNear(glm::mat3_cast(r), k.order, k.euler);
            if (sa::keysAsRotation(g, {k.node, k.rotate, 0}))
                sa::setRotationKey(g, {k.node, k.rotate, -1}, t, k.euler);
            else
                for (int c = 0; c < 3; ++c)
                    sa::setKey(g, {k.node, k.rotate, c}, t, k.euler[c]);
            res.keys += 3;
            if (k.translate)
            {
                const glm::dvec3 moved = pose.position[size_t(k.index)] - rest[size_t(k.index)].position;
                const glm::dvec3 tr = k.restTranslate + glm::dvec3(k.parentInverse * glm::dvec4(moved, 0.0));
                for (int c = 0; c < 3; ++c)
                    sa::setKey(g, {k.node, translate, c}, t, tr[c]);
                res.keys += 3;
            }
        }
    }
    ops::execute("editTake", {{"args", {res.take}}, {"loop", true}});
    return res;
}
} // namespace puppeteer::anim
