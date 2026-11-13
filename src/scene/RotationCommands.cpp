#include "scene/Anim.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Profile.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/RigUtil.h"
#include "scene/RotationCurve.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace lila::scene::anim
{
namespace
{

using rigutil::guarded;

bool isTransform(const Node& n) { return n.type().isA("transform"); }

RotateOrder orderOf(const Node& n)
{
    return RotateOrder(std::clamp<int64_t>(n.as<int64_t>(Transform::kRotateOrder), 0, 5));
}

/** The animCurveRQ driving the node's rotate, or kNoNode. */
NodeId rotationSource(Graph& g, NodeId node)
{
    auto s = g.source({node, Transform::kRotate, -1});
    if (!s)
        return kNoNode;
    Node* c = g.node(s->node);
    return c && isRotationCurve(*c) ? c->id() : kNoNode;
}

std::string curveName(Graph& g, NodeId node, const std::string& channel)
{
    std::string name = g.node(node)->name() + "_" + channel;
    std::replace(name.begin(), name.end(), ':', '_');
    return name;
}

/** Curve nodes named by the args (curves, plugs or nodes, default the selection), each once. */
std::vector<NodeId> curvesFrom(Graph& g, const Args& a)
{
    std::vector<NodeId> out;
    auto add = [&](NodeId c) {
        if (c && std::find(out.begin(), out.end(), c) == out.end())
            out.push_back(c);
    };
    std::vector<std::string> attrs;
    if (a.has("attribute"))
    {
        const json& v = a.flags["attribute"];
        if (v.is_string())
            attrs.push_back(v.get<std::string>());
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    attrs.push_back(x.get<std::string>());
    }
    auto addPlug = [&](const PlugRef& p) {
        add(curveFor(g, p));
        if (p.component < 0 && g.attr(p).type == PlugType::Vec3)
            for (int c = 0; c < 3; ++c)
                add(curveFor(g, {p.node, p.attr, c}));
    };
    for (const std::string& obj : a.objects())
    {
        if (obj.find('.') != std::string::npos)
        {
            addPlug(guarded([&] { return g.plug(obj); }));
            continue;
        }
        Node* n = g.find(obj);
        if (!n)
            throw CommandError("no node named '" + obj + "'");
        if (isAnyCurve(*n))
            add(n->id());
        else if (!attrs.empty())
            for (const std::string& at : attrs)
                addPlug(guarded([&] { return g.plug(obj + "." + at); }));
        else
            for (const AnimatedPlug& ap : animatedPlugs(g, n->id()))
                add(ap.curve);
    }
    return out;
}

/** Transform nodes named by the args, default the selection. */
std::vector<NodeId> transformsFrom(Graph& g, const Args& a, const char* command)
{
    std::vector<NodeId> out;
    for (const std::string& obj : a.objects())
    {
        Node* n = g.find(obj);
        if (!n)
            throw CommandError("no node named '" + obj + "'");
        if (!isTransform(*n))
            throw CommandError(std::string(command) + ": " + obj + " is not a transform");
        out.push_back(n->id());
    }
    return out;
}
} // namespace

bool keysAsRotation(Graph& g, const PlugRef& plug)
{
    Node* n = g.node(plug.node);
    if (!n || plug.attr != Transform::kRotate || !isTransform(*n))
        return false;
    // Keys on a layer go through its blend (setLayeredRotationKey). Asking routes the key that follows, which may make the layer.
    if (routeKey(g, plug))
        return false;
    if (auto s = g.source({plug.node, plug.attr, -1}))
    {
        Node* c = g.node(s->node);
        return c && isRotationCurve(*c);
    }
    for (int c = 0; c < 3; ++c)
        if (g.source({plug.node, plug.attr, c}))
            return false;
    if (routeKey(g, plug))
        return false;
    return RotationInterpolation(n->as<int64_t>(Transform::kRotationInterpolation)) != RotationInterpolation::Euler;
}

NodeId setRotationKey(Graph& g, const PlugRef& rotate, double time, const glm::dvec3& euler, std::optional<Tangent> in,
                      std::optional<Tangent> out)
{
    LILA_ZONE();

    Node* n = g.node(rotate.node);
    if (!n || !isTransform(*n) || rotate.attr != Transform::kRotate)
        throw std::invalid_argument("rotation keys go on a transform's rotate");
    PlugRef whole{rotate.node, rotate.attr, -1};
    NodeId curve = rotationSource(g, rotate.node);
    RotationCurve rc;
    if (!curve)
    {
        if (g.source(whole))
            throw std::invalid_argument(g.plugName(whole) + " is driven by a connection");
        for (int c = 0; c < 3; ++c)
            if (g.source({whole.node, whole.attr, c}))
                throw std::invalid_argument(g.plugName({whole.node, whole.attr, c}) + " has Euler curves; convertRotationCurves first");
        curve = g.createNode(*types::find("animCurveRQ"), curveName(g, rotate.node, "rotate"), kNoNode, false)->id();
        auto mode = RotationInterpolation(n->as<int64_t>(Transform::kRotationInterpolation));
        rc.interp = mode == RotationInterpolation::Slerp ? RotInterp::Slerp : RotInterp::Smooth;
        rc.order = orderOf(*n);
    }
    else
    {
        rc = rotationCurveOf(*g.node(curve));
        rc.reorder(orderOf(*n));
    }
    size_t i = rc.insert(rc.makeKey(time, euler));
    if (in)
        rc.keys[i].in = *in;
    if (out)
        rc.keys[i].out = *out;
    setRotationCurve(g, curve, rc);
    if (!g.source(whole))
        g.connect({curve, RotationCurveNode::kOutput, -1}, whole);
    return curve;
}

std::vector<double> curveKeyTimes(const Node& curve)
{
    std::vector<double> out;
    try
    {
        if (isRotationCurve(curve))
            for (const RotKey& k : rotationCurveOf(curve).keys)
                out.push_back(k.time);
        else if (isCurve(curve))
            for (const Key& k : curveOf(curve).keys)
                out.push_back(k.time);
    }
    catch (const std::exception&)
    {
        out.clear();
    }
    return out;
}

double curveValueAt(const Node& curve, int component, double t)
{
    std::vector<Modifier> mods = modifiersOf(curve);
    if (isRotationCurve(curve))
        return evaluateModified(rotationCurveOf(curve), mods, t)[std::clamp(component, 0, 2)];
    return evaluateModified(curveOf(curve), mods, t);
}

bool convertRotationCurves(Graph& g, NodeId node, RotationInterpolation to)
{
    LILA_ZONE();

    Node* n = g.node(node);
    if (!n || !isTransform(*n))
        throw std::invalid_argument("convertRotationCurves works on transforms");
    bool changed = false;
    NodeId rq = rotationSource(g, node);
    if (to == RotationInterpolation::Euler)
    {
        if (rq)
        {
            std::array<Curve, 3> curves = eulerFromRotation(rotationCurveOf(*g.node(rq)));
            g.disconnect({node, Transform::kRotate, -1});
            g.deleteNode(rq);
            const AttrDef& a = n->type().attrs[Transform::kRotate];
            for (int c = 0; c < 3; ++c)
            {
                NodeId id = g.createNode(*types::find("animCurveTA"), curveName(g, node, a.childNames[size_t(c)]), kNoNode, false)->id();
                setCurve(g, id, curves[size_t(c)]);
                g.connect({id, CurveNode::kOutput, -1}, {node, Transform::kRotate, c});
            }
            changed = true;
        }
    }
    else
    {
        RotInterp ri = to == RotationInterpolation::Slerp ? RotInterp::Slerp : RotInterp::Smooth;
        if (rq)
        {
            RotationCurve rc = rotationCurveOf(*g.node(rq));
            if (rc.interp != ri)
            {
                rc.interp = ri;
                setRotationCurve(g, rq, rc);
                changed = true;
            }
        }
        else
        {
            std::array<NodeId, 3> ids{};
            std::array<Curve, 3> curves;
            std::array<const Curve*, 3> xyz{};
            bool any = false;
            for (int c = 0; c < 3; ++c)
            {
                auto s = g.source({node, Transform::kRotate, c});
                if (!s)
                    continue;
                Node* cn = g.node(s->node);
                if (!cn || !isCurve(*cn))
                    throw std::invalid_argument(g.plugName({node, Transform::kRotate, c}) + " is driven by something other than a curve");
                ids[size_t(c)] = cn->id();
                curves[size_t(c)] = curveOf(*cn);
                xyz[size_t(c)] = &curves[size_t(c)];
                any = true;
            }
            if (any)
            {
                glm::dvec3 rest = n->as<glm::dvec3>(Transform::kRotate);
                RotationCurve rc = rotationFromEuler(xyz, rest, orderOf(*n), ri);
                for (int c = 0; c < 3; ++c)
                    if (ids[size_t(c)])
                    {
                        g.disconnect({node, Transform::kRotate, c});
                        g.deleteNode(ids[size_t(c)]);
                    }
                NodeId id = g.createNode(*types::find("animCurveRQ"), curveName(g, node, "rotate"), kNoNode, false)->id();
                setRotationCurve(g, id, rc);
                g.connect({id, RotationCurveNode::kOutput, -1}, {node, Transform::kRotate, -1});
                changed = true;
            }
        }
    }
    if (n->as<int64_t>(Transform::kRotationInterpolation) != int64_t(to))
    {
        g.setValue({node, Transform::kRotationInterpolation}, int64_t(to));
        changed = true;
    }
    return changed;
}

double setCurvesLoop(Graph& g, const std::vector<NodeId>& curves, bool on, std::optional<double> loopTime)
{
    LILA_ZONE();

    double latest = std::numeric_limits<double>::quiet_NaN();
    auto note = [&](double t) { latest = std::isnan(latest) ? t : std::max(latest, t); };
    for (NodeId id : curves)
    {
        Node* c = g.node(id);
        if (!c)
            continue;
        if (isCurve(*c))
        {
            Curve cv = curveOf(*c);
            if (on)
            {
                if (loopTime && !cv.keys.empty() && *loopTime > cv.keys.back().time + kTimeEps)
                {
                    Key k = cv.keys.front();
                    k.time = *loopTime;
                    cv.keys.push_back(k);
                }
                if (cv.keys.size() < 2)
                    continue;
                cv.loop = true;
                cv.post = Infinity::Cycle;
                note(cv.keys.back().time);
            }
            else
            {
                if (!cv.loop)
                    continue;
                cv.loop = false;
                if (cv.post == Infinity::Cycle)
                    cv.post = Infinity::Constant;
            }
            setCurve(g, id, cv);
        }
        else if (isRotationCurve(*c))
        {
            RotationCurve rc = rotationCurveOf(*c);
            if (on)
            {
                if (loopTime && !rc.keys.empty() && *loopTime > rc.keys.back().time + kTimeEps)
                {
                    RotKey k = rc.keys.front();
                    k.time = *loopTime;
                    rc.keys.push_back(k);
                }
                if (rc.keys.size() < 2)
                    continue;
                rc.loop = true;
                rc.post = Infinity::Cycle;
                note(rc.keys.back().time);
            }
            else
            {
                if (!rc.loop)
                    continue;
                rc.loop = false;
                if (rc.post == Infinity::Cycle || rc.post == Infinity::CycleRelative)
                    rc.post = Infinity::Constant;
            }
            setRotationCurve(g, id, rc);
        }
    }
    return latest;
}

bool curveLoops(const Node& curve)
{
    if (isCurve(curve))
        return curve.as<bool>(CurveNode::kLoop);
    if (isRotationCurve(curve))
        return curve.as<bool>(RotationCurveNode::kLoop);
    return false;
}

std::vector<double> loopKeyTimes(Graph& g, const std::vector<NodeId>& nodes)
{
    std::vector<double> out;
    for (NodeId id : nodes)
    {
        Node* n = g.node(id);
        if (!n)
            continue;
        std::vector<NodeId> curves;
        if (isAnyCurve(*n))
            curves.push_back(id);
        for (const AnimatedPlug& ap : animatedPlugs(g, id))
            curves.push_back(ap.curve);
        for (NodeId c : curves)
            if (Node* cn = g.node(c); cn && curveLoops(*cn))
                if (std::vector<double> times = curveKeyTimes(*cn); times.size() > 1)
                    out.push_back(times.back());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return std::abs(a - b) <= kTimeEps; }), out.end());
    return out;
}

void registerRotationCommands()
{
    using T = ArgType;
    registerRotationCurveType();

    Graph::current().setDrivenVecSetHook([](Graph& g, const PlugRef& dst, const PlugRef& src, const glm::dvec3& v) {
        Node* s = g.node(src.node);
        if (!s || !isRotationCurve(*s))
            return Graph::DrivenEdit::Reject;
        // As scalar curves: interactive drags hold, and the committing set keys.
        if (!UndoStack::get().enabled())
            return Graph::DrivenEdit::Hold;
        const bool toStatic = editSetsStatic(g, dst);
        if (!toStatic && !autoKey())
            return Graph::DrivenEdit::Hold;
        if (toStatic || routeKey(g, dst) != kNoNode)
        {
            // A layer takes the edit (a router's track): each component goes through it, the others settling after.
            for (int pass = 0; pass < 2; ++pass)
                for (int c = 0; c < 3; ++c)
                    setKey(g, {dst.node, dst.attr, c}, g.time(), v[c]);
        }
        else
            setRotationKey(g, dst, g.time(), v);
        if (!toStatic)
            notifyAutoKey(g, dst, g.time());
        return Graph::DrivenEdit::Handled;
    });

    // A rotation curve's Euler output follows its transform's rotateOrder; loads and undo replay carry both already.
    static bool listening = false;
    if (!std::exchange(listening, true))
        Graph::current().addListener(
        [](Change c, NodeId id) {
            Graph& g = Graph::current();
            if (c != Change::Value || g.batching() || UndoStack::get().replaying())
                return;
            Node* n = g.node(id);
            if (!n || !isTransform(*n))
                return;
            NodeId rq = rotationSource(g, id);
            if (!rq)
                return;
            RotationCurve rc = rotationCurveOf(*g.node(rq));
            if (rc.order == orderOf(*n))
                return;
            rc.reorder(orderOf(*n));
            setRotationCurve(g, rq, rc);
        },
        true);

    const FlagDef attrFlag{"attribute", "at", T::Any, nullptr, "Attribute name or list of names."};

    ops::registerCommand({
        .name = "convertRotationCurves",
        .doc = "Convert the rotate animation of transforms (default the selection) to `to`, keeping the key times and the "
               "pose at every key: euler (three curves) or one rotation curve interpolated by slerp or smooth. Also sets "
               "rotationInterpolation, so later keys follow. Returns the number of nodes changed.",
        .positionalDoc = "objects",
        .flags = {{"to", "to", T::String, "smooth", "euler | slerp | smooth", {"euler", "slerp", "smooth"}}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::string to = a.get<std::string>("to", "smooth");
            RotationInterpolation mode;
            if (to == "euler")
                mode = RotationInterpolation::Euler;
            else if (to == "slerp")
                mode = RotationInterpolation::Slerp;
            else if (to == "smooth")
                mode = RotationInterpolation::Smooth;
            else
                throw CommandError("convertRotationCurves: to is euler, slerp or smooth");
            int count = 0;
            for (NodeId id : transformsFrom(g, a, "convertRotationCurves"))
                count += guarded([&] { return convertRotationCurves(g, id, mode); }) ? 1 : 0;
            return count;
        },
    });

    ops::registerCommand({
        .name = "eulerFilter",
        .doc = "Euler filter the rotate curves of transforms (default the selection): each key keeps its rotation but "
               "takes the angles closest to the previous key's, removing flips and 360-degree jumps. Rotation curves "
               "need none. Returns the number of keys changed.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            int changed = 0;
            for (NodeId id : transformsFrom(g, a, "eulerFilter"))
            {
                Node* n = g.node(id);
                std::array<NodeId, 3> ids{};
                std::array<Curve, 3> curves;
                std::array<Curve*, 3> xyz{};
                for (int c = 0; c < 3; ++c)
                    if (auto s = g.source({id, Transform::kRotate, c}))
                        if (Node* cn = g.node(s->node); cn && isCurve(*cn))
                        {
                            ids[size_t(c)] = cn->id();
                            curves[size_t(c)] = curveOf(*cn);
                            xyz[size_t(c)] = &curves[size_t(c)];
                        }
                if (!ids[0] && !ids[1] && !ids[2])
                    continue;
                int n0 = eulerFilter(xyz, n->as<glm::dvec3>(Transform::kRotate), orderOf(*n));
                if (n0 == 0)
                    continue;
                changed += n0;
                for (int c = 0; c < 3; ++c)
                    if (ids[size_t(c)])
                        setCurve(g, ids[size_t(c)], curves[size_t(c)]);
            }
            return changed;
        },
    });

    ops::registerCommand({
        .name = "setLoopKey",
        .doc = "Make the last key of each curve a loop key: it mirrors the first key's value and tangents (live, as the "
               "first key changes) and the curve cycles after it. time adds the loop key there first. Objects may be "
               "nodes, plugs or curves (default the selection). snapRange moves the playback end to the loop key. "
               "Returns the loop key time, or None.",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            {"time", "t", T::Float, nullptr, "Seconds: add the loop key here when it is past the last key."},
            {"snapRange", "sr", T::Bool, true, "Move the playback range end to the loop key."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::optional<double> at;
            if (a.has("time"))
                at = a.get<double>("time");
            double t = setCurvesLoop(g, curvesFrom(g, a), true, at);
            if (std::isnan(t))
                return nullptr;
            if (a.get<bool>("snapRange", true))
            {
                Playback& pb = playback();
                pb.maxTime = std::max(t, pb.minTime);
                pb.animEnd = std::max(pb.animEnd, pb.maxTime);
                playbackChanged();
            }
            return t;
        },
    });

    ops::registerCommand({
        .name = "clearLoopKey",
        .doc = "Turn the loop key off on curves (nodes, plugs or curves, default the selection): the last key is an "
               "ordinary key again and cycling post infinity goes back to constant. Returns the number of curves changed.",
        .positionalDoc = "objects",
        .flags = {attrFlag},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<NodeId> looping;
            for (NodeId c : curvesFrom(g, a))
                if (curveLoops(*g.node(c)))
                    looping.push_back(c);
            setCurvesLoop(g, looping, false);
            return looping.size();
        },
    });
}
} // namespace lila::scene::anim
