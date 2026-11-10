#include "scene/Relocate.h"

#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "geo/Mesh.h"
#include "gn/Group.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "scene/Anim.h"
#include "scene/AnimCurve.h"
#include "scene/AnimInterpolator.h"
#include "scene/Components.h"
#include "scene/Constraint.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "scene/Ik.h"
#include "scene/MeshNodes.h"
#include "scene/NodeBounds.h"
#include "scene/Poses.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/RotationCurve.h"
#include "scene/SceneData.h"
#include "scene/Simulation.h"
#include "scene/Spaces.h"
#include "scene/Take.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace lila::scene::relocate
{
using rigutil::orthonormal;

namespace
{
constexpr const char* kDisplayOriginKey = "displayOrigin";
// Axis indices of each rotate order in application order; the last is applied outermost.
constexpr int kOrderAxes[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {1, 0, 2}, {2, 1, 0}};
constexpr const char* kOrderNames[] = {"xyz", "yzx", "zxy", "xzy", "yxz", "zyx"};

Registry<Handler>& registry()
{
    static Registry<Handler> r;
    return r;
}

[[noreturn]] void refuse(const std::string& why) { throw CommandError("relocateScene: " + why + ". Nothing was changed."); }

std::string fmt(double v)
{
    std::ostringstream s;
    s.precision(6);
    s << v;
    return s.str();
}

json vecJson(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

/** Allowed error at a world position: 1e-6 cm, growing with the distance from the origin past 10 m. */
double tolerance(double magnitude) { return 1e-6 * std::max(1.0, magnitude / 1000.0); }

bool nearIdentity(const glm::dmat3& m)
{
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            if (std::abs(m[c][r] - (c == r ? 1.0 : 0.0)) > 1e-12)
                return false;
    return true;
}

double matrixError(const glm::dmat3& a, const glm::dmat3& b)
{
    double e = 0.0;
    for (int c = 0; c < 3; ++c)
        e = std::max(e, glm::length(a[c] - b[c]));
    return e;
}

/** Signed degrees m turns about one axis, or nullopt when it turns about any other. */
std::optional<double> angleAbout(const glm::dmat3& m, int axis)
{
    glm::dvec3 e(0.0);
    e[axis] = 1.0;
    if (glm::length(m * e - e) > 1e-12)
        return std::nullopt;
    const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    glm::dvec3 u(0.0);
    u[a1] = 1.0;
    const glm::dvec3 w = m * u;
    return glm::degrees(std::atan2(w[a2], w[a1]));
}

// ------------------------------------------------------------------------------------------------ edits

/**
 * Sets a plug, writing through a lock: unlocked, set and locked again, all undoable. Plans call lockNote when they
 * queue one, so the result says so.
 */
void writeThrough(Graph& g, const PlugRef& p, const PlugValue& v)
{
    if (!g.anyLocked(p))
    {
        g.setValue(p, v);
        return;
    }
    const Node* n = g.node(p.node);
    const uint8_t mask = n->lockMask(p.attr);
    const PlugRef whole{p.node, p.attr, -1, p.element};
    if (p.component >= 0)
    {
        g.setLocked(p, false);
        g.setValue(p, v);
        g.setLocked(p, true);
        return;
    }
    for (int c = 0; c < 3; ++c)
        if (mask & (1u << c))
            g.setLocked(n->type().attrs[size_t(p.attr)].type == PlugType::Vec3 ? PlugRef{p.node, p.attr, c, p.element} : whole, false);
    g.setValue(p, v);
    if (n->type().attrs[size_t(p.attr)].type != PlugType::Vec3)
        g.setLocked(whole, true);
    else
        for (int c = 0; c < 3; ++c)
            if (mask & (1u << c))
                g.setLocked({p.node, p.attr, c, p.element}, true);
}

struct Notes
{
    std::vector<std::string> lines;
    void add(std::string s)
    {
        if (std::find(lines.begin(), lines.end(), s) == lines.end())
            lines.push_back(std::move(s));
    }
};

void lockNote(Graph& g, const PlugRef& p, Notes& notes)
{
    if (g.anyLocked(p))
        notes.add(g.plugName(p) + " is locked: it was unlocked, set and locked again");
}

Edit chain(std::vector<Edit> edits)
{
    std::erase_if(edits, [](const Edit& e) { return !e; });
    if (edits.empty())
        return {};
    return [edits = std::move(edits)] {
        for (const Edit& e : edits)
            e();
    };
}

// ------------------------------------------------------------------------------------------------ curves

/** Value and the slopes either side of t on a curve made of plain cubic segments; nullopt for anything else. */
struct ValueSlope
{
    double value = 0.0, left = 0.0, right = 0.0;
};

/** Why a curve's segments are not plain cubics, or empty when they are. */
std::string notPlain(const anim::Curve& c)
{
    if (c.keys.size() < 2)
        return {};
    if (c.weighted)
        return "has weighted tangents";
    if (c.loop)
        return "loops";
    for (anim::Infinity i : {c.pre, c.post})
        if (i != anim::Infinity::Constant && i != anim::Infinity::Linear)
            return "cycles past its keys";
    for (const anim::Key& k : c.keys)
    {
        if (k.in == anim::Tangent::Step || k.out == anim::Tangent::Step)
            return "has stepped keys";
        if (anim::findInterpolator(k.interpolator))
            return "has keys with a custom interpolator";
    }
    return {};
}

ValueSlope sampleCurve(const anim::Curve& c, double t)
{
    const size_t n = c.keys.size();
    if (n == 0)
        return {};
    if (n == 1)
        return {c.keys[0].value, 0.0, 0.0};
    const anim::Key& first = c.keys.front();
    const anim::Key& last = c.keys.back();
    const double pre = c.pre == anim::Infinity::Linear ? c.inSlope(0) : 0.0;
    const double post = c.post == anim::Infinity::Linear ? c.outSlope(n - 1) : 0.0;
    if (t < first.time)
        return {first.value + (t - first.time) * pre, pre, pre};
    if (t > last.time)
        return {last.value + (t - last.time) * post, post, post};
    for (size_t i = 0; i < n; ++i)
        if (std::abs(c.keys[i].time - t) < 1e-9)
            return {c.keys[i].value, i == 0 ? pre : c.inSlope(i), i + 1 == n ? post : c.outSlope(i)};
    auto it = std::upper_bound(c.keys.begin(), c.keys.end(), t, [](double x, const anim::Key& k) { return x < k.time; });
    const size_t i = size_t(it - c.keys.begin()) - 1;
    const anim::Key& a = c.keys[i];
    const anim::Key& b = c.keys[i + 1];
    const double dt = b.time - a.time;
    const double m0 = c.outSlope(i), m1 = c.inSlope(i + 1);
    const double s = (t - a.time) / dt, s2 = s * s, s3 = s2 * s;
    const double v = (2 * s3 - 3 * s2 + 1) * a.value + (s3 - 2 * s2 + s) * dt * m0 + (-2 * s3 + 3 * s2) * b.value + (s3 - s2) * dt * m1;
    const double d = ((6 * s2 - 6 * s) * a.value + (3 * s2 - 4 * s + 1) * dt * m0 + (-6 * s2 + 6 * s) * b.value + (3 * s2 - 2 * s) * dt * m1) / dt;
    return {v, d, d};
}

/** Times to compare two versions of curves at: every key, many points between, and a stretch of each infinity. */
std::vector<double> probeTimes(std::vector<double> keys)
{
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    if (keys.empty())
        return {0.0};
    const double span = std::max(keys.back() - keys.front(), 1.0 / 30.0);
    std::vector<double> out;
    for (int i = 1; i <= 8; ++i)
    {
        out.push_back(keys.front() - span * i / 8.0);
        out.push_back(keys.back() + span * i / 8.0);
    }
    for (size_t i = 0; i < keys.size(); ++i)
    {
        out.push_back(keys[i]);
        if (i + 1 < keys.size())
            for (int s = 1; s < 24; ++s)
                out.push_back(keys[i] + (keys[i + 1] - keys[i]) * s / 24.0);
    }
    return out;
}

std::vector<double> keyTimesOf(const anim::Curve& c)
{
    std::vector<double> t;
    for (const anim::Key& k : c.keys)
        t.push_back(k.time);
    return t;
}

double valueTolerance(double v) { return 1e-7 * std::max(1.0, std::abs(v) / 1000.0); }

/** One output of an affine map over a Vec3's components, as a curve: sum of coef[k] * input k, plus b. */
struct MixInput
{
    const anim::Curve* curve = nullptr; // null: constant
    double constant = 0.0;
    double coef = 0.0;
};

double mixAt(const std::vector<MixInput>& in, double b, double t)
{
    double v = b;
    for (const MixInput& m : in)
        v += m.coef * (m.curve ? m.curve->evaluate(t) : m.constant);
    return v;
}

/**
 * The mixed curve, keyed at `times` (every input's keys, and the target curve's own) with fixed tangents; throws
 * CommandError naming label when an input's segments aren't plain cubics, which a mix can't reproduce exactly.
 */
anim::Curve mixCurves(const std::vector<MixInput>& in, double b, std::vector<double> times, const std::string& label)
{
    bool linearPre = false, linearPost = false;
    for (const MixInput& m : in)
        if (m.curve)
        {
            if (std::string why = notPlain(*m.curve); !why.empty())
                refuse("the keys on " + label + " can't be turned exactly, because a curve feeding it " + why);
            linearPre |= m.curve->pre == anim::Infinity::Linear;
            linearPost |= m.curve->post == anim::Infinity::Linear;
        }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double a, double c) { return std::abs(a - c) < 1e-9; }), times.end());
    anim::Curve out;
    out.pre = linearPre ? anim::Infinity::Linear : anim::Infinity::Constant;
    out.post = linearPost ? anim::Infinity::Linear : anim::Infinity::Constant;
    auto degrees = [](double slope) { return std::atan(slope) * 180.0 / 3.14159265358979323846; };
    for (double t : times)
    {
        double v = b, left = 0.0, right = 0.0;
        for (const MixInput& m : in)
        {
            ValueSlope s = m.curve ? sampleCurve(*m.curve, t) : ValueSlope{m.constant, 0.0, 0.0};
            v += m.coef * s.value;
            left += m.coef * s.left;
            right += m.coef * s.right;
        }
        anim::Key k;
        k.time = t;
        k.value = v;
        k.in = k.out = anim::Tangent::Fixed;
        k.inAngle = degrees(left);
        k.outAngle = degrees(right);
        out.keys.push_back(k);
    }
    return out;
}

/** Throws unless curve matches expected at every probe time. */
void checkCurve(const anim::Curve& curve, const std::function<double(double)>& expected, const std::vector<double>& keys, const std::string& label)
{
    double worst = 0.0;
    for (double t : probeTimes(keys))
    {
        const double want = expected(t);
        const double err = std::abs(curve.evaluate(t) - want);
        if (err > valueTolerance(want))
            worst = std::max(worst, err);
    }
    if (worst > 0.0)
        refuse("the keys on " + label + " can't be rewritten exactly (off by " + fmt(worst) + ")");
}

/** Every curve to compare a rotation curve rewrite at. */
void checkRotation(const anim::RotationCurve& before, const anim::RotationCurve& after, const glm::dmat3& q, const std::string& label)
{
    std::vector<double> keys;
    for (const anim::RotKey& k : before.keys)
        keys.push_back(k.time);
    double worst = 0.0;
    for (double t : probeTimes(keys))
    {
        const glm::dmat3 want = q * glm::mat3_cast(before.evaluateQuat(t));
        worst = std::max(worst, matrixError(glm::mat3_cast(after.evaluateQuat(t)), want));
    }
    if (worst > 1e-9)
        refuse("the rotation keys on " + label + " can't be turned exactly (off by " + fmt(worst) + ")");
}

// ------------------------------------------------------------------------------------------------ channels

/** The curves keying one Vec3 attr in one take: the live connections, or an inactive take's parked ones. */
struct KeySet
{
    std::string take; // empty: the live connections
    std::array<NodeId, 3> curve{};
    std::array<NodeId, 3> driver{}; // something other than a curve drives the component
    NodeId wholeCurve = kNoNode; // a rotation curve on the whole attr
    bool live() const { return take.empty(); }
    bool any() const
    {
        for (int c = 0; c < 3; ++c)
            if (curve[size_t(c)] || driver[size_t(c)])
                return true;
        return wholeCurve != kNoNode;
    }
};

/** The live key set, then one per inactive take that keys the attr. Throws when something other than curves is parked. */
std::vector<KeySet> keySets(Graph& g, NodeId node, int attr, NodeId* wholeDriver)
{
    std::vector<KeySet> sets(1);
    *wholeDriver = kNoNode;
    if (auto src = g.source({node, attr, -1}))
    {
        if (anim::isRotationCurve(*g.node(src->node)))
            sets[0].wholeCurve = src->node;
        else
            *wholeDriver = src->node;
    }
    for (int c = 0; c < 3; ++c)
        if (auto src = g.source({node, attr, c}))
            (anim::isCurve(*g.node(src->node)) ? sets[0].curve : sets[0].driver)[size_t(c)] = src->node;
    // A curve driving something else too would be rewritten twice.
    for (NodeId curve : {sets[0].wholeCurve, sets[0].curve[0], sets[0].curve[1], sets[0].curve[2]})
        if (curve && g.destinations({curve, anim::isRotationCurve(*g.node(curve)) ? int(anim::RotationCurveNode::kOutput) : int(anim::CurveNode::kOutput), -1}).size() > 1)
            refuse(g.node(curve)->path() + " keys more than one channel, so its keys can't be moved for one of them");
    for (NodeId take : anim::takes(g))
    {
        if (g.node(take)->as<bool>(anim::TakeNode::kActive))
            continue;
        KeySet s;
        s.take = g.node(take)->name();
        for (const auto& [dst, src] : anim::takeConnections(g, take))
        {
            if (dst.node != node || dst.attr != attr)
                continue;
            const Node* sn = g.node(src.node);
            if (dst.component < 0 && sn && anim::isRotationCurve(*sn))
                s.wholeCurve = src.node;
            else if (dst.component >= 0 && sn && anim::isCurve(*sn))
                s.curve[size_t(dst.component)] = src.node;
            else
                refuse(g.plugName({node, attr, dst.component}) + " is driven by something other than keys in take '" + s.take + "'");
        }
        if (s.any())
            sets.push_back(std::move(s));
    }
    return sets;
}

/** Something other than curves drives all three components, live and in every take: the attr follows it. */
NodeId drivenWhole(const std::vector<KeySet>& sets)
{
    if (sets.size() != 1 || !sets[0].driver[0] || !sets[0].driver[1] || !sets[0].driver[2])
        return kNoNode;
    return sets[0].driver[0];
}

std::string channelName(Graph& g, NodeId node, int attr, int c) { return g.plugName({node, attr, c}); }

/**
 * Plans out = A * in + b on a Vec3 attr: its stored value, the curves keying its components (in every take) and new
 * curves where a mixed component needs one. A component that only shifts may be driven by anything else, which is
 * left to bring it along; one that mixes others can't be.
 */
Edit planVec3(Graph& g, NodeId node, int attr, const glm::dmat3& A, const glm::dvec3& b, Notes& notes)
{
    auto coef = [&](int c, int k) { return A[k][c]; };
    auto shiftOnly = [&](int c) {
        for (int k = 0; k < 3; ++k)
            if (std::abs(coef(c, k) - (k == c ? 1.0 : 0.0)) > 1e-14)
                return false;
        return true;
    };
    bool anything = false;
    for (int c = 0; c < 3; ++c)
        anything |= !shiftOnly(c) || b[c] != 0.0;
    if (!anything)
        return {};
    const Node* n = g.node(node);
    const glm::dvec3 stored = n->as<glm::dvec3>(attr);
    NodeId wholeDriver = kNoNode;
    std::vector<KeySet> sets = keySets(g, node, attr, &wholeDriver);
    const std::string attrName = g.plugName({node, attr, -1});
    if (!wholeDriver)
        wholeDriver = drivenWhole(sets);
    if (wholeDriver)
    {
        notes.add(attrName + " is driven by " + g.node(wholeDriver)->path() + " and was left to follow it");
        return {};
    }
    for (const KeySet& s : sets)
        if (s.wholeCurve)
            refuse(attrName + " is keyed by a rotation curve");

    std::array<bool, 3> keyedAnywhere{};
    for (const KeySet& s : sets)
        for (int k = 0; k < 3; ++k)
            keyedAnywhere[size_t(k)] |= s.curve[size_t(k)] != kNoNode;
    const bool anyTakes = !anim::takes(g).empty();

    std::vector<Edit> edits;
    glm::dvec3 newStored = stored;
    std::array<bool, 3> writeStored{};
    for (const KeySet& s : sets)
    {
        const std::string where = s.live() ? std::string() : " in take '" + s.take + "'";
        // Read before any edit, so every plan sees the keys as they are now.
        std::array<std::optional<anim::Curve>, 3> curves;
        for (int k = 0; k < 3; ++k)
            if (NodeId cn = s.curve[size_t(k)])
                curves[size_t(k)] = anim::curveOf(*g.node(cn));
        for (int c = 0; c < 3; ++c)
        {
            const std::string label = channelName(g, node, attr, c) + where;
            const NodeId target = s.curve[size_t(c)];
            if (shiftOnly(c))
            {
                if (b[c] == 0.0)
                    continue;
                if (NodeId d = s.driver[size_t(c)])
                    notes.add(label + " is driven by " + g.node(d)->path() + " and was left to follow it");
                else if (target)
                {
                    const anim::Curve& before = *curves[size_t(c)];
                    anim::Curve out = before;
                    for (anim::Key& k : out.keys)
                        k.value += b[c];
                    const double shift = b[c];
                    checkCurve(out, [&](double t) { return before.evaluate(t) + shift; }, keyTimesOf(before), label);
                    edits.push_back([&g, target, out] { anim::setCurve(g, target, out); });
                }
                else if (s.live())
                {
                    newStored[c] = stored[c] + b[c];
                    writeStored[size_t(c)] = true;
                }
                continue;
            }
            std::vector<MixInput> in;
            std::vector<double> keys;
            if (target)
                keys = keyTimesOf(*curves[size_t(c)]);
            bool animated = false;
            for (int k = 0; k < 3; ++k)
            {
                const double a = coef(c, k);
                if (std::abs(a) < 1e-14)
                    continue;
                if (NodeId d = s.driver[size_t(k)])
                    refuse(channelName(g, node, attr, k) + where + " is driven by " + g.node(d)->path() + ", so the turn can't be applied to " +
                           attrName);
                MixInput m{.curve = nullptr, .constant = stored[k], .coef = a};
                if (curves[size_t(k)])
                {
                    m.curve = &*curves[size_t(k)];
                    animated = true;
                    for (double t : keyTimesOf(*m.curve))
                        keys.push_back(t);
                }
                else if (!s.live() && keyedAnywhere[size_t(k)])
                    refuse("take '" + s.take + "' leaves " + channelName(g, node, attr, k) +
                           " unkeyed while another take keys it, so its value there is not known");
                in.push_back(m);
            }
            if (s.driver[size_t(c)])
                refuse(label + " is driven by " + g.node(s.driver[size_t(c)])->path() + ", so the turn can't be applied to " + attrName);
            if (!animated && !target)
            {
                if (s.live())
                {
                    newStored[c] = mixAt(in, b[c], 0.0);
                    writeStored[size_t(c)] = true;
                }
                continue;
            }
            const anim::Curve out = mixCurves(in, b[c], keys, label);
            checkCurve(out, [&](double t) { return mixAt(in, b[c], t); }, keys, label);
            if (target)
                edits.push_back([&g, target, out] { anim::setCurve(g, target, out); });
            else if (s.live() && !anyTakes)
            {
                const PlugRef plug{node, attr, c};
                notes.add(label + " had no keys and was keyed so it turns with the rest of " + attrName);
                edits.push_back([&g, plug, out] { anim::setCurve(g, anim::setKey(g, plug, out.keys.front().time, out.keys.front().value), out); });
            }
            else
                refuse(label + " would need new keys to turn with the rest of " + attrName + ", which an inactive take can't be given");
        }
    }
    for (int c = 0; c < 3; ++c)
        if (writeStored[size_t(c)] && newStored[c] != stored[c])
        {
            const PlugRef plug{node, attr, c};
            lockNote(g, plug, notes);
            const double v = newStored[c];
            edits.push_back([&g, plug, v] { writeThrough(g, plug, v); });
        }
    return chain(std::move(edits));
}

/**
 * Plans rotate' = q * rotate on a transform: a rotation curve's keys turn, Euler keys shift when q turns about the
 * outermost axis of the rotate order, and a static rotate takes the new angles.
 */
Edit planRotate(Graph& g, NodeId node, const glm::dmat3& q, Notes& notes)
{
    if (nearIdentity(q))
        return {};
    const Node* n = g.node(node);
    const int attr = Transform::kRotate;
    const auto order = RotateOrder(n->as<int64_t>(Transform::kRotateOrder));
    const int outer = kOrderAxes[int(order)][2];
    const std::optional<double> turn = angleAbout(q, outer);
    const glm::dvec3 stored = n->as<glm::dvec3>(attr);
    const std::string attrName = g.plugName({node, attr, -1});
    NodeId wholeDriver = kNoNode;
    std::vector<KeySet> sets = keySets(g, node, attr, &wholeDriver);
    if (!wholeDriver)
        wholeDriver = drivenWhole(sets);
    if (wholeDriver)
    {
        notes.add(attrName + " is driven by " + g.node(wholeDriver)->path() + " and was left to follow it");
        return {};
    }
    std::vector<Edit> edits;
    for (const KeySet& s : sets)
    {
        const std::string where = s.live() ? std::string() : " in take '" + s.take + "'";
        if (s.wholeCurve)
        {
            const anim::RotationCurve before = anim::rotationCurveOf(*g.node(s.wholeCurve));
            anim::RotationCurve after = before;
            const glm::dquat qq = glm::normalize(glm::quat_cast(q));
            for (size_t i = 0; i < after.keys.size(); ++i)
            {
                anim::RotKey& k = after.keys[i];
                k.q = glm::normalize(qq * before.keys[i].q);
                const glm::dvec3 ref = i == 0 ? before.keys[0].euler : after.keys[i - 1].euler + (before.keys[i].euler - before.keys[i - 1].euler);
                k.euler = eulerNear(glm::mat3_cast(k.q), after.order, ref);
            }
            checkRotation(before, after, q, attrName + where);
            const NodeId cn = s.wholeCurve;
            edits.push_back([&g, cn, after] { anim::setRotationCurve(g, cn, after); });
            continue;
        }
        bool anyKeyed = false;
        for (int c = 0; c < 3; ++c)
            anyKeyed |= s.curve[size_t(c)] || s.driver[size_t(c)];
        if (turn)
        {
            const int c = outer;
            if (NodeId d = s.driver[size_t(c)])
            {
                notes.add(channelName(g, node, attr, c) + " is driven by " + g.node(d)->path() + where + " and was left to follow it");
                continue;
            }
            if (NodeId cn = s.curve[size_t(c)])
            {
                const anim::Curve before = anim::curveOf(*g.node(cn));
                anim::Curve after = before;
                for (anim::Key& k : after.keys)
                    k.value += *turn;
                const double d = *turn;
                checkCurve(after, [&](double t) { return before.evaluate(t) + d; }, keyTimesOf(before), channelName(g, node, attr, c) + where);
                edits.push_back([&g, cn, after] { anim::setCurve(g, cn, after); });
            }
            else if (s.live())
            {
                const PlugRef plug{node, attr, c};
                lockNote(g, plug, notes);
                const double v = stored[c] + *turn;
                edits.push_back([&g, plug, v] { writeThrough(g, plug, v); });
            }
            continue;
        }
        if (anyKeyed)
            refuse(attrName + where + " is keyed on separate X, Y and Z curves in " + kOrderNames[int(order)] +
                   " order, which this turn can't be applied to exactly. Use rotation='none', or convert its keys to a rotation curve "
                   "(convertRotationCurves) first");
        if (s.live())
        {
            const glm::dmat3 want = q * glm::dmat3(eulerToMatrix(stored, order));
            const glm::dvec3 euler = eulerNear(want, order, stored);
            if (matrixError(glm::dmat3(eulerToMatrix(euler, order)), want) > 1e-9)
                refuse("the new rotation of " + attrName + " can't be written as Euler angles exactly");
            const PlugRef plug{node, attr, -1};
            lockNote(g, plug, notes);
            edits.push_back([&g, plug, euler] { writeThrough(g, plug, euler); });
        }
    }
    return chain(std::move(edits));
}

// ------------------------------------------------------------------------------------------------ handlers

std::vector<std::string> words(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;)
        out.push_back(w);
    return out;
}

/** The plugs of one attr: its elements for an array, else the attr itself. */
std::vector<PlugRef> plugsOf(Graph& g, NodeId node, int attr)
{
    const AttrDef& def = g.node(node)->type().attrs[size_t(attr)];
    if (def.flags & AttrArray)
        return g.elements({node, attr, -1});
    return {{node, attr, -1}};
}

Edit planMatrices(const Move& mv, NodeId node, const std::vector<std::string>& names, bool inverse, Notes& notes)
{
    Graph& g = *mv.graph;
    const Node* n = g.node(node);
    const glm::dmat4 inv = glm::inverse(mv.matrix);
    std::vector<Edit> edits;
    for (const std::string& name : names)
    {
        const int attr = n->type().attrIndex(name);
        if (attr < 0)
            continue;
        for (const PlugRef& p : plugsOf(g, node, attr))
        {
            if (g.source(p))
                continue;
            const PlugValue* v = p.element >= 0 ? n->element(attr, p.element) : &n->value(attr);
            if (!v || !std::holds_alternative<glm::dmat4>(*v))
                continue;
            const glm::dmat4 m = std::get<glm::dmat4>(*v);
            const glm::dmat4 next = inverse ? m * inv : mv.matrix * m;
            lockNote(g, p, notes);
            edits.push_back([&g, p, next] { writeThrough(g, p, next); });
        }
    }
    return chain(std::move(edits));
}

Edit planPointLists(const Move& mv, NodeId node, const std::vector<std::string>& names, Notes& notes)
{
    Graph& g = *mv.graph;
    const Node* n = g.node(node);
    std::vector<Edit> edits;
    for (const std::string& name : names)
    {
        const int attr = n->type().attrIndex(name);
        if (attr < 0 || g.source({node, attr, -1}))
            continue;
        const std::string text = n->as<std::string>(attr);
        if (text.empty())
            continue;
        json pts = json::parse(text, nullptr, false);
        if (!pts.is_array())
            refuse(g.plugName({node, attr, -1}) + " holds points that can't be read");
        for (json& p : pts)
        {
            if (!p.is_array() || p.size() != 3)
                refuse(g.plugName({node, attr, -1}) + " holds points that can't be read");
            p = vecJson(mv.point({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}));
        }
        const PlugRef plug{node, attr, -1};
        lockNote(g, plug, notes);
        std::string next = pts.dump();
        edits.push_back([&g, plug, next] { writeThrough(g, plug, next); });
    }
    return chain(std::move(edits));
}

/** Unconnected curves a history node holds (left when its source curve was deleted) are moved point by point. */
Edit planWorldData(const Move& mv, NodeId node, const std::vector<std::string>& names, Notes& notes)
{
    Graph& g = *mv.graph;
    const Node* n = g.node(node);
    std::vector<Edit> edits;
    for (const std::string& name : names)
    {
        const int attr = n->type().attrIndex(name);
        if (attr < 0 || g.source({node, attr, -1}))
            continue;
        const DataPtr* d = std::get_if<DataPtr>(&n->value(attr));
        if (!d || !*d)
            continue;
        if (std::string((*d)->kind()) != "curve")
            refuse(n->path() + " holds world-space " + name + " of its own rather than from a connection");
        curve::Splines splines = curve::splinesOf(n->value(attr));
        for (curve::Spline& s : splines)
            curve::transform(s, mv.matrix);
        const PlugRef plug{node, attr, -1};
        lockNote(g, plug, notes);
        PlugValue next = curve::makeCurveData(std::move(splines));
        edits.push_back([&g, plug, next] { writeThrough(g, plug, next); });
    }
    return chain(std::move(edits));
}

/** Handlers and plans that report notes see the run's notes through this. */
Notes* gNotes = nullptr;

Notes& notes()
{
    static Notes scratch;
    return gNotes ? *gNotes : scratch;
}

Handler worldAttrsHandler(WorldAttrs w)
{
    const bool worldOutput = w.worldOutput;
    return {.plan = [w = std::move(w)](const Move& mv, NodeId node) -> Edit {
        Graph& g = *mv.graph;
        const Node* n = g.node(node);
        std::vector<Edit> edits;
        edits.push_back(planMatrices(mv, node, w.matrices, false, notes()));
        edits.push_back(planMatrices(mv, node, w.inverseMatrices, true, notes()));
        for (const std::string& name : w.points)
            if (int attr = n->type().attrIndex(name); attr >= 0)
                edits.push_back(planVec3(g, node, attr, mv.rotation(), glm::dvec3(mv.matrix[3]), notes()));
        for (const std::string& name : w.directions)
            if (int attr = n->type().attrIndex(name); attr >= 0)
                edits.push_back(planVec3(g, node, attr, mv.rotation(), glm::dvec3(0.0), notes()));
        edits.push_back(planPointLists(mv, node, w.pointLists, notes()));
        for (const std::string& name : w.channelCopies)
            if (int attr = n->type().attrIndex(name); attr >= 0)
                if (const Node* copied = g.find(n->as<std::string>(attr)); copied && mv.rootMove(copied->id()))
                    refuse(n->path() + " keeps a copy of where " + copied->path() + " stood, which moves as a root; move its parent instead");
        edits.push_back(planWorldData(mv, node, w.worldData, notes()));
        return chain(std::move(edits));
    }, .worldOutput = worldOutput};
}

/** The node a constraint drives, through its blend when it has one. */
NodeId constrainedBy(Graph& g, NodeId constraint)
{
    for (int out : {int(ConstraintNode::kConstraintTranslate), int(ConstraintNode::kConstraintRotate)})
        for (const PlugRef& dst : g.destinations({constraint, out, -1}))
        {
            const Node* d = g.node(dst.node);
            if (d && anim::isBlend(*d))
            {
                if (PlugRef m = anim::memberOf(g, dst.node); m.valid())
                    return m.node;
            }
            else if (d)
                return dst.node;
        }
    return kNoNode;
}

/**
 * Constraints work in their targets' and parent's frames, which move with the scene, except for two things: an aim
 * constraint's world up, and the copy of a relocated root's own channels (restTranslate, the aim origin and what skipped
 * axes hold), which is in world space for a root.
 */
Edit planConstraint(const Move& mv, NodeId node)
{
    Graph& g = *mv.graph;
    const Node* n = g.node(node);
    std::vector<Edit> edits;
    const bool turns = !nearIdentity(mv.rotation());
    if (std::optional<glm::dmat4> rm = mv.rootMove(constrainedBy(g, node)))
    {
        const glm::dmat3 q(*rm);
        const glm::dvec3 pivot = n->as<glm::dvec3>(ConstraintNode::kRotatePivot) + n->as<glm::dvec3>(ConstraintNode::kRotatePivotTranslate);
        if (!nearIdentity(q))
            for (int k = 0; k < 6; ++k)
                if (n->as<bool>(ConstraintNode::kSkipTranslateX + k))
                    refuse(n->path() + " skips an axis of the root it constrains, which a turn would pull apart; use rotation='none'");
        edits.push_back(planVec3(g, node, ConstraintNode::kRestTranslate, q, glm::dvec3((*rm)[3]) + q * pivot - pivot, notes()));
    }
    if (n->type().registered().name == "aimConstraint" && turns)
    {
        switch (n->as<int64_t>(ConstraintNode::kWorldUpType))
        {
        case ConstraintNode::kUpScene:
            if (glm::length(mv.rotation() * glm::dvec3(0, 1, 0) - glm::dvec3(0, 1, 0)) > 1e-12)
                refuse(n->path() + " aims with the scene's up, which this rotation would tilt; set its worldUpType to vector, or use rotation='yaw'");
            break;
        case ConstraintNode::kUpNone:
            refuse(n->path() + " aims with no up vector, which doesn't turn with the scene; give it a world up, or use rotation='none'");
        case ConstraintNode::kUpVectorOnly:
            edits.push_back(planVec3(g, node, ConstraintNode::kWorldUpVector, mv.rotation(), glm::dvec3(0.0), notes()));
            break;
        default: break;
        }
    }
    return chain(std::move(edits));
}

/** The pole is a world direction; the rest layout copies the start joint's local matrix, world space for a root joint. */
Edit planIkHandle(const Move& mv, NodeId node)
{
    Graph& g = *mv.graph;
    const Node* n = g.node(node);
    std::vector<Edit> edits;
    edits.push_back(planVec3(g, node, IkHandle::kPoleVector, mv.rotation(), glm::dvec3(0.0), notes()));
    const std::vector<NodeId> chainJoints = ik::chainOf(g, node);
    for (size_t i = 0; i < chainJoints.size(); ++i)
        if (std::optional<glm::dmat4> rm = mv.rootMove(chainJoints[i]))
        {
            const PlugRef p{node, IkHandle::slotLocal(int(i)), -1};
            const glm::dmat4 next = *rm * n->as<glm::dmat4>(p.attr);
            edits.push_back([&g, p, next] { writeThrough(g, p, next); });
        }
    return chain(std::move(edits));
}

/** A world space's frame is a world matrix; parent and node spaces are relative. */
Edit planSpaceSwitch(const Move& mv, NodeId node)
{
    Graph& g = *mv.graph;
    const Node* n = g.node(node);
    const std::vector<std::string> kinds = words(n->as<std::string>(spaces::SwitchNode::kKinds));
    std::vector<Edit> edits;
    for (int i = 0; i < int(kinds.size()); ++i)
    {
        if (kinds[size_t(i)] != "world")
            continue;
        const PlugRef p{node, spaces::SwitchNode::kOffset, -1, i};
        if (g.source(p))
            continue;
        const glm::dmat4 next = mv.matrix * n->elementOr<glm::dmat4>(spaces::SwitchNode::kOffset, i);
        lockNote(g, p, notes());
        edits.push_back([&g, p, next] { writeThrough(g, p, next); });
    }
    return chain(std::move(edits));
}

Edit noEdit(const Move&, NodeId) { return {}; }

// ------------------------------------------------------------------------------------------------ links

bool linkType(PlugType t) { return t == PlugType::Matrix || t == PlugType::Mesh || t == PlugType::Data || t == PlugType::Vec3; }

/** Nodes joined by matrix, mesh, data and vector connections, both ways. */
std::unordered_map<NodeId, std::vector<NodeId>> linkGraph(Graph& g)
{
    std::unordered_map<NodeId, std::vector<NodeId>> adj;
    for (const auto& [dst, src] : g.connections())
    {
        const Node* d = g.node(dst.node);
        if (!d || dst.attr < 0 || size_t(dst.attr) >= d->type().attrs.size() || !linkType(d->type().attrs[size_t(dst.attr)].type))
            continue;
        adj[dst.node].push_back(src.node);
        adj[src.node].push_back(dst.node);
    }
    return adj;
}

/** The DAG nodes a node links: itself when it is one, and those reached through non-DAG nodes (not through other DAG ones). */
std::vector<NodeId> linkedDag(Graph& g, const std::unordered_map<NodeId, std::vector<NodeId>>& adj, NodeId start)
{
    std::vector<NodeId> out;
    std::unordered_set<NodeId> seen{start};
    std::vector<NodeId> todo{start};
    if (g.node(start)->type().dag)
    {
        out.push_back(start);
        todo.clear();
        if (auto it = adj.find(start); it != adj.end())
            for (NodeId m : it->second)
                if (seen.insert(m).second)
                    todo.push_back(m);
    }
    while (!todo.empty())
    {
        NodeId id = todo.back();
        todo.pop_back();
        const Node* n = g.node(id);
        if (!n)
            continue;
        if (n->type().dag)
        {
            out.push_back(id);
            continue;
        }
        if (anim::isAnyCurve(*n) || anim::isTake(*n))
            continue;
        if (auto it = adj.find(id); it != adj.end())
            for (NodeId m : it->second)
                if (seen.insert(m).second)
                    todo.push_back(m);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ snapshots

struct Sample
{
    double time = 0.0;
    NodeId take = kNoNode; // an inactive take sampled with it active; kNoNode as the scene is
};

struct Snapshot
{
    struct Mesh
    {
        MeshPtr mesh;
        glm::dmat4 world;
    };
    std::vector<std::unordered_map<NodeId, glm::dmat4>> worlds;
    std::vector<std::unordered_map<NodeId, Mesh>> meshes;
    /** Per sample: the candidate roots whose local matrix lives in a space other than their parent's. */
    std::vector<std::unordered_set<NodeId>> inSpace;
};

void captureNow(Graph& g, Snapshot& s)
{
    g.evaluate();
    auto& worlds = s.worlds.emplace_back();
    auto& meshes = s.meshes.emplace_back();
    auto& inSpace = s.inSpace.emplace_back();
    for (NodeId id : g.nodes())
    {
        const Node* n = g.node(id);
        if (!n->type().dag || !n->type().isA("transform"))
            continue;
        worlds[id] = n->as<glm::dmat4>(Transform::kWorldMatrix);
        if (spaces::activeFrame(*n))
            inSpace.insert(id);
        if (n->type().isA("mesh"))
            if (const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh))
                meshes[id] = {m, worlds[id]};
    }
}

Snapshot capture(Graph& g, const std::vector<Sample>& samples)
{
    LILA_ZONE();

    Snapshot s;
    const double now = g.time();
    size_t i = 0;
    while (i < samples.size())
    {
        const NodeId take = samples[i].take;
        size_t end = i;
        while (end < samples.size() && samples[end].take == take)
            ++end;
        auto run = [&] {
            for (size_t k = i; k < end; ++k)
            {
                g.setTime(samples[k].time, false);
                captureNow(g, s);
            }
        };
        if (take)
            anim::withTake(g, take, run);
        else
            run();
        i = end;
    }
    g.setTime(now, false);
    g.evaluate();
    return s;
}

std::string sampleName(Graph& g, const Sample& s)
{
    std::string f = "frame " + fmt(anim::frameOf(s.time));
    if (s.take)
        f += " of take '" + g.node(s.take)->name() + "'";
    return f;
}

struct Mismatch
{
    std::string what;
    double error = 0.0;
};

/** Every transform and mesh point after must equal M times before (or before, for what doesn't move). */
std::optional<Mismatch> compare(Graph& g, const Snapshot& before, const Snapshot& after, const std::vector<Sample>& samples, const Move& mv,
                                const std::unordered_set<NodeId>& shaped, double* maxError, size_t* checkedPoints)
{
    *maxError = 0.0;
    *checkedPoints = 0;
    for (size_t s = 0; s < samples.size(); ++s)
    {
        for (const auto& [id, wb] : before.worlds[s])
        {
            const glm::dmat4 m = mv.moves(id) ? mv.matrix : glm::dmat4(1.0);
            auto it = after.worlds[s].find(id);
            if (it == after.worlds[s].end())
                return Mismatch{g.node(id)->path() + " is missing after the move", 0.0};
            // A world-shaped node keeps its transform relative to its moved geometry.
            const glm::dmat4 want = shaped.contains(id) ? m * wb * glm::inverse(m) : m * wb;
            const glm::dmat4& got = it->second;
            for (int c = 0; c < 3; ++c)
            {
                const double e = glm::length(glm::dvec3(got[c]) - glm::dvec3(want[c]));
                if (e > 1e-9 * std::max(1.0, glm::length(glm::dvec3(want[c]))))
                    return Mismatch{"the world axes of " + g.node(id)->path() + " at " + sampleName(g, samples[s]), e};
            }
            const double e = glm::length(glm::dvec3(got[3]) - glm::dvec3(want[3]));
            *maxError = std::max(*maxError, e);
            if (e > tolerance(glm::length(glm::dvec3(want[3]))))
                return Mismatch{"the world position of " + g.node(id)->path() + " at " + sampleName(g, samples[s]), e};
        }
        for (const auto& [id, mb] : before.meshes[s])
        {
            auto it = after.meshes[s].find(id);
            if (it == after.meshes[s].end() || it->second.mesh->pointCapacity() != mb.mesh->pointCapacity())
                return Mismatch{"the mesh of " + g.node(id)->path() + " changed shape at " + sampleName(g, samples[s]), 0.0};
            const glm::dmat4 wantWorld = (mv.moves(id) ? mv.matrix : glm::dmat4(1.0)) * mb.world;
            const glm::dmat4& gotWorld = it->second.world;
            const auto pb = mb.mesh->positions();
            const auto pa = it->second.mesh->positions();
            for (size_t p = 0; p < pb.size(); ++p)
            {
                const glm::dvec3 want(wantWorld * glm::dvec4(pb[p], 1.0));
                const glm::dvec3 got(gotWorld * glm::dvec4(pa[p], 1.0));
                const double e = glm::length(got - want);
                *maxError = std::max(*maxError, e);
                if (e > tolerance(glm::length(want)))
                    return Mismatch{"point " + std::to_string(p) + " of " + g.node(id)->path() + " at " + sampleName(g, samples[s]), e};
            }
            *checkedPoints += pb.size();
        }
    }
    return std::nullopt;
}

// ------------------------------------------------------------------------------------------------ choosing M

glm::dvec3 vec3Arg(const json& j)
{
    if (!j.is_array() || j.size() != 3)
        throw CommandError("relocateScene: expected [x, y, z], got " + j.dump());
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

std::vector<NodeId> nodesNamed(Graph& g, const std::vector<std::string>& names, const char* cmd)
{
    std::vector<NodeId> ids;
    for (const std::string& name : names)
    {
        Node* n = g.find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
        if (n->type().dag && n->type().isA("transform"))
            ids.push_back(n->id());
    }
    return ids;
}

geo::Bounds boundsOf(Graph& g, const std::vector<NodeId>& ids)
{
    geo::Bounds b;
    for (NodeId id : ids)
    {
        std::vector<NodeId> all = g.descendants(id);
        all.push_back(id);
        for (NodeId d : all)
            if (g.node(d)->type().dag)
                if (geo::Bounds w = worldBounds(g, d); w.valid())
                {
                    b.extend(w.min);
                    b.extend(w.max);
                }
    }
    return b;
}

struct Anchor
{
    glm::dvec3 point{0.0};
    NodeId object = kNoNode; // whose orientation rotation= takes
};

/** The point a `from` keyword or object name stands for, among the sources (the selection by default). */
Anchor anchorFor(Graph& g, const std::string& from, const std::vector<NodeId>& sources, const char* cmd)
{
    Anchor a;
    a.object = sources.empty() ? kNoNode : sources.front();
    if (from == "components")
    {
        const ComponentMode m = components::mode();
        if (m == ComponentMode::Object || components::selected(m).empty())
            throw CommandError(std::string(cmd) + ": from='components' needs selected components");
        a.point = symmetricSelectionCenter();
        if (a.object == kNoNode && !components::selected(m).empty())
            a.object = components::selected(m).begin()->first;
        return a;
    }
    if (from != "pivot" && from != "bottom" && from != "center")
    {
        Node* n = g.find(from);
        if (!n || !n->type().dag)
            throw CommandError(std::string(cmd) + ": from must be pivot, bottom, center, components or an object, not '" + from + "'");
        a.object = n->id();
        a.point = xformops::worldPivot(g, n->id());
        return a;
    }
    if (sources.empty())
        throw CommandError(std::string(cmd) + ": select something to relocate to, or give offset=");
    if (from == "pivot")
    {
        a.point = xformops::worldPivot(g, sources.front());
        return a;
    }
    geo::Bounds b = boundsOf(g, sources);
    if (!b.valid())
    {
        a.point = xformops::worldPivot(g, sources.front());
        return a;
    }
    a.point = b.center();
    if (from == "bottom")
    {
        const glm::dvec3 up = sceneUp();
        const int axis = std::abs(up.z) > 0.5 ? 2 : 1;
        a.point[axis] = b.min[axis];
    }
    return a;
}

/** The turn that makes an object's heading (rotation='yaw') or its whole orientation ('full') line up with the world. */
glm::dmat3 alignRotation(Graph& g, NodeId object, const std::string& mode)
{
    if (mode == "none")
        return glm::dmat3(1.0);
    if (mode != "yaw" && mode != "full")
        throw CommandError("relocateScene: rotation must be 'none', 'yaw' or 'full'");
    if (object == kNoNode)
        throw CommandError("relocateScene: rotation='" + mode + "' needs an object to take the orientation of");
    g.evaluate();
    const glm::dmat3 r = orthonormal(glm::dmat3(g.node(object)->as<glm::dmat4>(Transform::kWorldMatrix)));
    if (glm::determinant(r) < 0.0)
        throw CommandError("relocateScene: " + g.node(object)->path() + " is mirrored, so its orientation can't be taken");
    if (mode == "full")
        return glm::transpose(r);
    const glm::dvec3 up = sceneUp();
    auto flat = [&](glm::dvec3 v) { return v - glm::dot(v, up) * up; };
    glm::dvec3 ref(1, 0, 0), axis = r[0];
    if (glm::length(flat(axis)) < 0.1)
    {
        ref = std::abs(up.z) > 0.5 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
        axis = std::abs(up.z) > 0.5 ? r[1] : r[2];
    }
    axis = glm::normalize(flat(axis));
    const double angle = std::atan2(glm::dot(glm::cross(ref, axis), up), glm::dot(ref, axis));
    return glm::dmat3(glm::rotate(glm::dmat4(1.0), -angle, up));
}

/** axes="xz" keeps the others: "xyz" by default. */
glm::dvec3 maskAxes(glm::dvec3 d, const std::string& axes)
{
    for (char c : axes)
        if (c != 'x' && c != 'y' && c != 'z')
            throw CommandError("relocateScene: axes takes x, y and z, e.g. 'xz'");
    for (int i = 0; i < 3; ++i)
        if (axes.find(char('x' + i)) == std::string::npos)
            d[i] = 0.0;
    return d;
}

glm::dmat4 chooseMatrix(Graph& g, const Args& a, std::vector<NodeId> sources, glm::dvec3* anchorOut)
{
    const std::string axes = a.get<std::string>("axes", "xyz");
    if (a.has("offset"))
    {
        if (a.get<std::string>("rotation", "none") != "none")
            throw CommandError("relocateScene: rotation= turns about the from= point, so it can't be given with offset=");
        *anchorOut = glm::dvec3(0.0);
        return glm::translate(glm::dmat4(1.0), maskAxes(vec3Arg(a.flags.at("offset")), axes));
    }
    const Anchor anchor = anchorFor(g, a.get<std::string>("from", "bottom"), sources, "relocateScene");
    const glm::dvec3 to = a.has("to") ? vec3Arg(a.flags.at("to")) : glm::dvec3(0.0);
    const glm::dmat3 r = alignRotation(g, anchor.object, a.get<std::string>("rotation", "none"));
    const glm::dvec3 d = maskAxes(to - anchor.point, axes);
    *anchorOut = anchor.point;
    // Turn about the anchor, then carry it to `to` along the chosen axes.
    glm::dmat4 m(r);
    m[3] = glm::dvec4(anchor.point + d - r * anchor.point, 1.0);
    return m;
}

json matrixJson(const glm::dmat4& m)
{
    json j = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            j.push_back(m[c][r]);
    return j;
}

// ------------------------------------------------------------------------------------------------ the relocation

struct Root
{
    NodeId id = kNoNode;
    glm::dmat4 local{1.0}; // the move in the root's parent space
};

std::vector<Sample> samplesFor(Graph& g, const std::vector<NodeId>& roots)
{
    std::vector<double> times{g.time(), anim::playback().minTime, anim::playback().maxTime};
    std::vector<NodeId> keyed = roots;
    for (NodeId r : roots)
        for (NodeId d : g.descendants(r))
            keyed.push_back(d);
    std::vector<double> keys = anim::keyTimes(g, keyed);
    if (!keys.empty())
    {
        times.push_back(keys.front());
        times.push_back(keys[keys.size() / 2]);
        times.push_back(keys.back());
        // Between keys too, where only interpolation decides.
        if (keys.size() > 1)
            times.push_back(0.5 * (keys[0] + keys[1]));
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end(), [](double x, double y) { return std::abs(x - y) < 1e-9; }), times.end());
    std::vector<Sample> out;
    for (double t : times)
        out.push_back({t, kNoNode});
    for (NodeId take : anim::takes(g))
    {
        const Node* n = g.node(take);
        if (n->as<bool>(anim::TakeNode::kActive))
            continue;
        const double s = n->as<double>(anim::TakeNode::kStart), e = n->as<double>(anim::TakeNode::kEnd);
        for (double t : {s, 0.5 * (s + e), e})
            out.push_back({t, take});
    }
    return out;
}

/** Plans one root taking its move through its own channels and keys. */
Edit planRoot(Graph& g, const Root& r, Notes& notes)
{
    Node* n = g.node(r.id);
    const glm::dmat3 q(r.local);
    const glm::dvec3 t(r.local[3]);
    const bool turns = !nearIdentity(q);
    const std::string name = n->path();
    std::vector<Edit> edits;

    glm::dvec3 pivot = n->as<glm::dvec3>(Transform::kRotatePivot) + n->as<glm::dvec3>(Transform::kRotatePivotTranslate);
    if (turns)
        for (int attr : {int(Transform::kRotatePivot), int(Transform::kRotatePivotTranslate)})
            for (int c = -1; c < 3; ++c)
                if (g.source({r.id, attr, c}))
                    refuse("the pivot of " + name + " is driven, so it can't be turned");
    if (rig::isJoint(*n))
        for (int attr : {int(Joint::kSolvedRotate), int(Joint::kIkBlend)})
            if (auto src = g.source({r.id, attr, -1}); src && !g.node(src->node)->type().isA("ikHandle"))
                refuse(name + " is posed by " + g.node(src->node)->path() + ", which keeps a copy of where it stood");
    // T' = q (T + pivot) + t - pivot keeps the pivot chain landing where M puts it.
    edits.push_back(planVec3(g, r.id, Transform::kTranslate, q, t + q * pivot - pivot, notes));

    if (turns)
    {
        if (rig::isJoint(*n))
        {
            // A root joint takes the turn in its jointOrient, so its rotate keys stay as they are.
            NodeId driver = kNoNode;
            std::vector<KeySet> sets = keySets(g, r.id, Transform::kRotate, &driver);
            bool driven = driver != kNoNode;
            for (const KeySet& s : sets)
                for (int c = 0; c < 3; ++c)
                    driven |= s.driver[size_t(c)] != kNoNode;
            for (int attr : {int(Joint::kSolvedRotate), int(Joint::kIkBlend)})
                if (auto src = g.source({r.id, attr, -1}))
                    refuse(name + " is posed by " + g.node(src->node)->path() + ", so it can't be turned");
            if (driven)
                notes.add(g.plugName({r.id, Transform::kRotate, -1}) + " is driven and was left to follow its driver");
            else
            {
                for (int c = -1; c < 3; ++c)
                    if (g.source({r.id, Joint::kJointOrient, c}))
                        refuse("the jointOrient of " + name + " is driven, so it can't be turned");
                if (n->as<bool>(Joint::kSegmentScaleCompensate))
                {
                    const glm::dvec3 s = n->as<glm::dvec3>(Joint::kInverseScale);
                    if (std::abs(s.x - s.y) > 1e-12 || std::abs(s.x - s.z) > 1e-12)
                        refuse(name + " compensates an uneven parent scale, so it can't be turned");
                }
                const glm::dvec3 jo = n->as<glm::dvec3>(Joint::kJointOrient);
                const glm::dmat3 want = q * glm::dmat3(eulerToMatrix(jo, RotateOrder::XYZ));
                const glm::dvec3 next = eulerNear(want, RotateOrder::XYZ, jo);
                if (matrixError(glm::dmat3(eulerToMatrix(next, RotateOrder::XYZ)), want) > 1e-9)
                    refuse("the new jointOrient of " + name + " can't be written exactly");
                const PlugRef plug{r.id, Joint::kJointOrient, -1};
                lockNote(g, plug, notes);
                edits.push_back([&g, plug, next] { writeThrough(g, plug, next); });
            }
        }
        else
        {
            edits.push_back(planRotate(g, r.id, q, notes));
        }
    }
    return chain(std::move(edits));
}

/** Scene poses hold the roots' channels by name: they move with them. */
Edit planScenePoses(Graph& g, const std::vector<Root>& roots, Notes& notes)
{
    std::vector<poses::Pose> changed;
    for (const poses::Entry& e : poses::list({}, poses::Tier::Scene, true))
    {
        poses::Pose p = e.pose;
        bool any = false;
        for (const Root& r : roots)
        {
            const Node* n = g.node(r.id);
            const std::string key = poses::stripNamespace(n->name());
            if (!p.nodes.contains(key) || !p.nodes[key].is_object())
                continue;
            json& entry = p.nodes[key];
            const glm::dmat3 q(r.local);
            const glm::dvec3 pivot = n->as<glm::dvec3>(Transform::kRotatePivot) + n->as<glm::dvec3>(Transform::kRotatePivotTranslate);
            if (entry.contains("translate"))
            {
                const glm::dvec3 t = vec3Arg(entry["translate"]);
                entry["translate"] = vecJson(q * (t + pivot) + glm::dvec3(r.local[3]) - pivot);
                any = true;
            }
            if (!nearIdentity(q) && !rig::isJoint(*n) && entry.contains("rotate"))
            {
                const json& rj = entry["rotate"];
                const auto order = RotateOrder(n->as<int64_t>(Transform::kRotateOrder));
                glm::dquat rq = rj.size() == 4 ? glm::dquat(rj[3].get<double>(), rj[0].get<double>(), rj[1].get<double>(), rj[2].get<double>())
                                               : anim::eulerToQuat(vec3Arg(rj), order);
                rq = glm::normalize(glm::quat_cast(q) * rq);
                entry["rotate"] = json::array({rq.x, rq.y, rq.z, rq.w});
                any = true;
            }
        }
        if (any)
        {
            notes.add("scene pose '" + p.name + "' moved with its root");
            changed.push_back(std::move(p));
        }
    }
    if (changed.empty())
        return {};
    return [changed] {
        for (const poses::Pose& p : changed)
            poses::save(p, poses::Tier::Scene);
    };
}

bool isGnModifierType(const NodeType& t) { return t.name.starts_with("gn_") && t.attrIndex("groupDefinition") >= 0; }

bool readsObjects(const NodeType& t)
{
    for (const AttrDef& a : t.attrs)
        if (a.name.starts_with("obj_") && a.name.ends_with("_matrix"))
            return true;
    return false;
}

/**
 * A geometry-nodes modifier with Object Info nodes: relative ones read the object against the owner's world matrix,
 * which moves with both; absolute ones read a world transform the move changes, and are refused.
 */
Edit planGnObjects(const Move& mv, NodeId node)
{
    Graph& g = *mv.graph;
    const Node* n = g.node(node);
    gn::Group grp;
    try
    {
        grp = gn::Group::fromJson(json::parse(std::get<std::string>(g.value({node, n->type().attrIndex("groupDefinition"), -1}))));
    }
    catch (const std::exception&)
    {
        refuse("the geometry-nodes group of " + n->path() + " can't be read");
    }
    const gn::NodeDecl* decl = gn::findDecl("ObjectInfo");
    for (const gn::GNode& o : grp.nodes)
        if (decl && o.type == "ObjectInfo" && decl->propValue(o, "relativeTo") != "owner" && !decl->propValue(o, "object").empty())
            refuse("Object Info node '" + o.name + "' in " + n->path() + " reads " + decl->propValue(o, "object") +
                   "'s world transform, which the move changes; set its Relative To to owner so it reads the object against the mesh "
                   "the group runs on");
    return planMatrices(mv, node, {"ownerMatrix"}, false, notes());
}

const Handler* handlerFor(const NodeType& t)
{
    static const Handler neutral;
    static const Handler gnObjects{.plan = planGnObjects};
    if (const Handler* h = registry().find(t.registered().name))
        return h;
    if (!isGnModifierType(t.registered()))
        return nullptr;
    return readsObjects(t.registered()) ? &gnObjects : &neutral;
}

/** Input connections by destination node, with the destination attr's type. */
using Inputs = std::unordered_map<NodeId, std::vector<std::pair<NodeId, PlugType>>>;

Inputs inputsOf(Graph& g)
{
    Inputs in;
    for (const auto& [dst, src] : g.connections())
        if (const Node* d = g.node(dst.node); d && dst.attr >= 0 && size_t(dst.attr) < d->type().attrs.size())
            in[dst.node].emplace_back(src.node, d->type().attrs[size_t(dst.attr)].type);
    return in;
}

/** Whether a mesh's or curve's shape comes, through non-DAG nodes, from history that outputs world-space geometry. */
bool worldShaped(Graph& g, const Inputs& inputs, NodeId id)
{
    const Node* n = g.node(id);
    int shape = -1;
    if (n->type().isA("mesh"))
        shape = MeshNode::kInMesh;
    else if (n->type().isA("curve"))
        shape = n->type().attrIndex("create");
    if (shape < 0)
        return false;
    auto src = g.source({id, shape, -1});
    if (!src)
        return false;
    std::vector<NodeId> todo{src->node};
    std::unordered_set<NodeId> seen{src->node};
    while (!todo.empty())
    {
        const Node* m = g.node(todo.back());
        todo.pop_back();
        if (!m || m->type().dag)
            continue;
        if (const Handler* h = handlerFor(m->type()); h && h->worldOutput)
            return true;
        if (auto it = inputs.find(m->id()); it != inputs.end())
            for (const auto& [from, type] : it->second)
                if ((type == PlugType::Mesh || type == PlugType::Data) && seen.insert(from).second)
                    todo.push_back(from);
    }
    return false;
}

json relocate(const Args& a)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    UndoStack& undo = UndoStack::get();
    if (!undo.enabled() || undo.replaying())
        refuse("undo is off, so a failed check could not be rolled back");
    g.evaluate();

    std::vector<NodeId> sources = nodesNamed(g, a.objects(), "relocateScene");
    glm::dvec3 anchor(0.0);
    Move mv;
    mv.graph = &g;
    mv.matrix = chooseMatrix(g, a, sources, &anchor);
    if (std::abs(glm::determinant(glm::dmat3(mv.matrix)) - 1.0) > 1e-9 || matrixError(glm::transpose(glm::dmat3(mv.matrix)) * glm::dmat3(mv.matrix), glm::dmat3(1.0)) > 1e-9)
        refuse("the move must be a rotation and a translation");

    // What moves: every root, or the top of each named hierarchy.
    std::vector<NodeId> tops;
    std::unordered_set<NodeId> moved;
    mv.wholeScene = !a.has("nodes") && !a.get<bool>("selectedOnly");
    if (mv.wholeScene)
    {
        for (NodeId r : g.roots())
            if (const Node* n = g.node(r); n && n->type().dag && n->type().isA("transform"))
                tops.push_back(r);
        mv.moves = [&g](NodeId id) { return g.node(id) && g.node(id)->type().dag; };
    }
    else
    {
        std::vector<std::string> names;
        if (a.has("nodes"))
            for (const json& j : a.flags.at("nodes"))
                names.push_back(j.get<std::string>());
        tops = xformops::topLevel(g, a.has("nodes") ? nodesNamed(g, names, "relocateScene") : sources);
        if (tops.empty())
            throw CommandError("relocateScene: no hierarchies to move: name them with nodes=, or select them");
        for (NodeId t : tops)
        {
            moved.insert(t);
            for (NodeId d : g.descendants(t))
                moved.insert(d);
        }
        mv.moves = [&moved](NodeId id) { return moved.contains(id); };
    }

    // Every type must be known before anything is sampled or changed.
    std::map<std::string, std::string> unknown;
    for (NodeId id : g.nodes())
    {
        const std::string& type = g.node(id)->type().registered().name;
        if (!handlerFor(g.node(id)->type()) && !unknown.contains(type))
            unknown[type] = g.node(id)->path();
    }
    if (!unknown.empty())
    {
        std::string list;
        for (const auto& [type, example] : unknown)
            list += (list.empty() ? "" : ", ") + type + " (" + example + ")";
        refuse("the scene has node types that may hold world-space data relocateScene doesn't know how to move: " + list);
    }

    const std::vector<Sample> samples = samplesFor(g, tops);
    Snapshot before = capture(g, samples);

    Notes notes;
    gNotes = &notes;
    struct ResetNotes
    {
        ~ResetNotes() { gNotes = nullptr; }
    } resetNotes;

    // Meshes and curves shaped by world-space history keep their transform relative to that geometry, which follows
    // its moved inputs; their children then no longer follow them and take a move of their own.
    const Inputs inputs = inputsOf(g);
    std::unordered_set<NodeId> shaped;
    for (NodeId id : g.nodes())
        if (const Node* n = g.node(id); n->type().dag && mv.moves(id) && worldShaped(g, inputs, id))
            shaped.insert(id);
    std::vector<NodeId> candidates = tops;
    auto addCandidate = [&](NodeId id) {
        if (std::find(candidates.begin(), candidates.end(), id) == candidates.end())
            candidates.push_back(id);
    };
    for (NodeId id : shaped)
    {
        for (const PlugRef& p : {PlugRef{id, Transform::kWorldMatrix, -1}, PlugRef{id, Transform::kMatrix, -1}})
            for (const PlugRef& d : g.destinations(p))
                if (!(g.parent(d.node) == id && d.attr == Transform::kParentMatrix))
                    refuse(g.node(id)->path() + " is shaped by world-space history and " + g.node(d.node)->path() +
                           " reads its transform, which would no longer move with it");
        addCandidate(id);
        for (NodeId c : g.children(id))
            if (g.node(c)->type().isA("transform"))
                addCandidate(c);
    }
    auto target = [&](NodeId id, size_t s) {
        const glm::dmat4& w = before.worlds[s].at(id);
        if (!mv.moves(id))
            return w;
        return shaped.contains(id) ? mv.matrix * w * glm::inverse(mv.matrix) : mv.matrix * w;
    };

    // A root whose local matrix lives in a space switch's frame follows the frame, which its handler moves.
    std::vector<Root> roots;
    std::vector<std::string> followers;
    for (NodeId id : candidates)
    {
        const std::string name = g.node(id)->path();
        size_t inSpace = 0;
        for (const auto& s : before.inSpace)
            inSpace += s.contains(id);
        const bool top = std::find(tops.begin(), tops.end(), id) != tops.end();
        if (inSpace == samples.size() && top && !shaped.contains(id))
        {
            followers.push_back(name);
            continue;
        }
        if (inSpace > 0)
            refuse(name + " switches between its parent and another space over time");
        // The move in the parent's space that lands the node on its target T: L' = P'^-1 T, so X = L' L^-1.
        std::optional<glm::dmat4> first;
        for (size_t s = 0; s < samples.size(); ++s)
        {
            const NodeId p = g.parent(id);
            const glm::dmat4 parentBefore = p ? before.worlds[s].at(p) : glm::dmat4(1.0);
            const glm::dmat4 parentAfter = p ? target(p, s) : glm::dmat4(1.0);
            const glm::dmat4 local = glm::inverse(parentBefore) * before.worlds[s].at(id);
            const glm::dmat4 x = glm::inverse(parentAfter) * target(id, s) * glm::inverse(local);
            if (!first)
                first = x;
            else if (matrixError(glm::dmat3(x), glm::dmat3(*first)) > 1e-9 || glm::length(glm::dvec3(x[3] - (*first)[3])) > 1e-6)
                refuse(name + " would need a different move at different frames: its parent, or the history shaping it, is animated");
        }
        const glm::dmat3 q(*first);
        if (matrixError(glm::transpose(q) * q, glm::dmat3(1.0)) > 1e-9)
            refuse(name + " is scaled unevenly against its parent, so the move isn't rigid in its space");
        if (matrixError(q, glm::dmat3(1.0)) < 1e-12 && glm::length(glm::dvec3((*first)[3])) < 1e-9)
            continue;
        roots.push_back({id, *first});
    }
    {
        std::vector<std::string> names;
        for (NodeId id : shaped)
            names.push_back(g.node(id)->path());
        std::sort(names.begin(), names.end());
        for (const std::string& s : names)
            notes.add(s + " is shaped by world-space history, which moved its geometry; its transform stays where its geometry needs it");
    }

    std::unordered_map<NodeId, glm::dmat4> rootMoves;
    for (const Root& r : roots)
        rootMoves[r.id] = r.local;
    mv.rootMove = [&rootMoves](NodeId id) -> std::optional<glm::dmat4> {
        auto it = rootMoves.find(id);
        return it == rootMoves.end() ? std::nullopt : std::optional(it->second);
    };

    std::vector<Edit> edits;
    for (const Root& r : roots)
        edits.push_back(planRoot(g, r, notes));

    std::map<std::string, int> handled;
    const auto adj = mv.wholeScene ? std::unordered_map<NodeId, std::vector<NodeId>>{} : linkGraph(g);
    for (NodeId id : g.nodes())
    {
        const Node* n = g.node(id);
        const std::string& type = n->type().registered().name;
        const Handler* h = handlerFor(n->type());
        if (!h || !h->plan)
            continue;
        if (!mv.wholeScene)
        {
            std::vector<NodeId> linked = linkedDag(g, adj, id);
            size_t in = 0;
            std::string inside, outside;
            for (NodeId l : linked)
            {
                if (mv.moves(l))
                {
                    ++in;
                    inside = g.node(l)->path();
                }
                else
                    outside = g.node(l)->path();
            }
            if (in == 0)
                continue;
            if (in < linked.size())
                refuse(n->path() + " links " + inside + ", which moves, and " + outside + ", which doesn't; include both in nodes=");
        }
        if (Edit e = h->plan(mv, id))
        {
            edits.push_back(std::move(e));
            ++handled[type];
        }
    }
    edits.push_back(planScenePoses(g, roots, notes));
    std::erase_if(edits, [](const Edit& e) { return !e; });

    json result = {
        {"matrix", matrixJson(mv.matrix)},
        {"offset", vecJson(glm::dvec3(mv.matrix[3]))},
        {"rotation", vecJson(matrixToEuler(glm::dmat3(mv.matrix), RotateOrder::XYZ))},
        {"anchor", vecJson(anchor)},
        {"roots", json::array()},
        {"handled", handled},
    };
    for (const Root& r : roots)
        result["roots"].push_back(g.node(r.id)->path());
    if (!followers.empty())
    {
        result["following"] = followers;
        for (const std::string& f : followers)
            notes.add(f + " lives in a space switch's frame and moved with it");
    }

    if (a.get<bool>("dryRun"))
    {
        result["notes"] = notes.lines;
        result["dryRun"] = true;
        return result;
    }

    const size_t mark = undo.openEdits();
    try
    {
        Graph::Batch batch(g);
        for (const Edit& e : edits)
            e();
    }
    catch (...)
    {
        undo.rollbackTo(mark);
        throw;
    }
    sim::clearCache(g);

    Snapshot after = capture(g, samples);
    double maxError = 0.0;
    size_t points = 0;
    if (std::optional<Mismatch> bad = compare(g, before, after, samples, mv, shaped, &maxError, &points))
    {
        undo.rollbackTo(mark);
        sim::clearCache(g);
        g.evaluate();
        throw CommandError("relocateScene: the check after moving failed, so the scene was put back as it was: " + bad->what +
                           " is off by " + fmt(bad->error) + " cm");
    }
    result["notes"] = notes.lines;
    result["verified"] = {{"samples", samples.size()}, {"transforms", before.worlds.empty() ? 0 : before.worlds[0].size()},
                          {"points", points}, {"maxError", maxError}};
    return result;
}

json originJson()
{
    if (!hasDisplayOrigin())
        return nullptr;
    return vecJson(displayOrigin());
}
} // namespace

// ------------------------------------------------------------------------------------------------ public

void registerHandler(const std::string& type, Handler handler) { registry().put(type, std::move(handler)); }

void registerWorldAttrs(const std::string& type, WorldAttrs attrs) { registerHandler(type, worldAttrsHandler(std::move(attrs))); }

void registerNeutral(const std::string& type) { registry().put(type, Handler{}); }

bool handles(const std::string& type)
{
    if (registry().contains(type))
        return true;
    const NodeType* t = types::find(type);
    return t && handlerFor(*t);
}

void registerBuiltins()
{
    if (registry().contains("transform"))
        return;
    for (const char* t : {
             // Transforms and shapes: values relative to the parent, geometry in object space.
             "transform", "locator", "joint", "mesh", "lattice", "camera", "light", "imagePlane", "lodGroup", "array", "curve", "field",
             "proxySet", "turntable",
             // Rig elements: control-space or connected values; their outputs follow what they read.
             "ikChain", "reverseFoot", "rigAlign", "twistDriver", "poseDriver", "drivenChannel",
             "splineChain", "splineJoint", "hoseLimb", "hoseHand",
             // Animation.
             "animCurve", "animCurveTL", "animCurveTA", "animCurveTU", "animCurveRQ", "animLayer", "animBlend", "animBlendLinear",
             "animBlendVector", "animBlendRotation", "animBlendOffsetVector", "animBlendOffsetRotation", "take",
             // Sound: timing and files only (a cue filters events by name, not place).
             "audio", "soundCue",
             // Scene organisation, shading and baking.
             "displayLayer", "objectSet", "material", "postProcess", "textureSet", "bakeGroup", "bakeProject", "gnGroup",
             // Mesh sources and object-space modifiers and deformers.
             "meshSource", "polyCapsule", "polyCone", "polyCube", "polyCylinder", "polyDisc", "polyIcoSphere", "polyPipe", "polyPlane",
             "polyQuadSphere", "polySphere", "polyTorus", "bevelModifier", "decimateModifier", "mirrorModifier", "solidifyModifier",
             "subdivisionModifier", "triangulateModifier", "weightedNormalsModifier", "extrudeModifier", "splitRingsModifier", "autoBevelModifier", "morph",
             "morphTarget", "correction", "deltaMush", "smoothDeformer", "tension", "textureDeformer", "noiseDeformer", "meshCache"})
        registerNeutral(t);
    registerNeutral("gameExportPreset");
    // Math nodes, scale and pole vector constraints among them: they compute from connected values and hold nothing in world space.
    for (const char* t : {"addDoubleLinear", "multDoubleLinear", "blendColors", "blendTwoAttr", "blendWeighted", "choice", "clamp", "condition",
                          "multiplyDivide", "plusMinusAverage", "remapValue", "reverse", "setRange", "unitConversion", "pairBlend", "distanceBetween",
                          "vectorProduct", "closestPointOnMesh", "expression", "scaleConstraint", "poleVectorConstraint"})
        registerNeutral(t);

    // Linked only: nothing stored in world space, but what they link must move together.
    for (const char* t : {"constraint", "parentConstraint", "pointConstraint", "orientConstraint", "aimConstraint"})
        registerHandler(t, {.plan = planConstraint});
    registerHandler("surfacePin", {.plan = noEdit});
    registerWorldAttrs("pin", {.directions = {"upVector"}});
    registerHandler(spaces::kSwitchType, {.plan = planSpaceSwitch});

    registerWorldAttrs("skinCluster", {.matrices = {"geomMatrix"}});
    registerWorldAttrs("skinInfluence", {.inverseMatrices = {"bindPreMatrix"}});
    for (const char* t : {"arap", "bbw", "laplacianDeform", "rigidify", "skinSlide", "inflate", "collision", "cage", "shrinkwrap", "surfaceDeform",
                          "wrap", "rigidBind", "bendDeformer", "cast", "shearDeformer", "sineDeformer", "squashDeformer", "taperDeformer", "twistDeformer",
                          "waveDeformer", "warp"})
        registerWorldAttrs(t, {.matrices = {"geomMatrix"}});
    registerWorldAttrs("ffd", {.matrices = {"geomMatrix", "baseMatrix"}});
    registerWorldAttrs("kelvinlet", {.matrices = {"bindMatrix", "geomMatrix"}});
    registerWorldAttrs("softMod", {.matrices = {"bindMatrix", "geomMatrix"}, .points = {"falloffCenter"}});
    registerWorldAttrs("jiggle", {.directions = {"gravity"}});
    for (const char* t : {"arrayModifier", "booleanModifier", "surfaceFlowModifier", "transferAttributesModifier"})
        registerWorldAttrs(t, {.matrices = {"targetMatrix"}});
    // Curve history: world-space curves in, world-space geometry out; curves of their own (unconnected) move with the scene.
    registerNeutral("curveText");
    for (const char* t : {"curveLoft", "curveNetwork", "curvePatch", "meshBorderBlend"})
        registerWorldAttrs(t, {.worldOutput = true});
    registerWorldAttrs("bevelPlus", {.matrices = {"meshMatrix"}});
    for (const char* t : {"curveBoolean", "curveCenterLine", "insetCurve", "offsetRegion", "roof"})
        registerWorldAttrs(t, {.directions = {"normal"}, .worldOutput = true});
    for (const char* t : {"filletCurve", "fitCurve", "rebuildCurve"})
        registerWorldAttrs(t, {.worldData = {"inCurve"}, .worldOutput = true});
    registerWorldAttrs("offsetCurve", {.directions = {"normal"}, .worldData = {"inCurve"}, .worldOutput = true});
    registerWorldAttrs("projectCurve", {.directions = {"direction"}, .worldData = {"inCurve"}, .worldOutput = true});
    registerWorldAttrs("curveBlend", {.worldData = {"curveA", "curveB"}, .worldOutput = true});
    registerWorldAttrs("curveCable", {.directions = {"gravity"}, .worldOutput = true});
    registerWorldAttrs("curveStitch", {.directions = {"up"}, .worldOutput = true});
    registerWorldAttrs("curveContour", {.points = {"origin"}, .directions = {"axis"}, .worldOutput = true});
    registerWorldAttrs("curvePipe", {.pointLists = {"startLoop"}, .worldData = {"path"}, .worldOutput = true});
    registerWorldAttrs("curveRevolve", {.points = {"axisOrigin"}, .directions = {"axis"}, .worldData = {"profile"}, .worldOutput = true});
    registerWorldAttrs("curveSweep", {.directions = {"upVector"}, .worldData = {"path", "profile"}, .worldOutput = true});
    registerWorldAttrs("hairCards", {.directions = {"upVector"}, .worldData = {"inCurve", "guideCurve"}, .worldOutput = true});
    for (const char* t : {"groom", "groomSplit", "groomClump", "groomCurl", "groomWave", "groomFrizz", "groomNoise", "groomLength", "groomCut",
                          "groomFlyaways", "groomShell"})
        registerWorldAttrs(t, {.worldData = {"inCurve"}, .worldOutput = true});
    registerWorldAttrs("curveStrapPath", {.matrices = {"meshMatrices"}, .points = {"start", "end"}, .directions = {"startDir", "endDir"}, .worldOutput = true});
    registerWorldAttrs("curveStrap", {.directions = {"up"}, .worldData = {"path"}});
    registerWorldAttrs("curvePipeRoute", {.points = {"start", "end"}, .directions = {"startDir", "endDir"}, .worldOutput = true});
    registerWorldAttrs("curvePipeSupports", {.directions = {"up"}, .worldData = {"path"}, .worldOutput = true});
    registerWorldAttrs("channelModifier", {.matrices = {"targetMatrix"}, .directions = {"up"}, .worldData = {"path"}});
    // Curve-driven modifiers read world-space curves through the mesh's world matrix when they were connected.
    for (const char* t : {"curveArrayModifier", "curveDeformModifier"})
        registerWorldAttrs(t, {.matrices = {"targetMatrix"}, .directions = {"upVector"}, .worldData = {"curve", "aimCurve"}});
    registerHandler("ikHandle", {.plan = planIkHandle});
    registerWorldAttrs("looker", {.directions = {"upVector"}});
    registerWorldAttrs("distanceMeasure", {.matrices = {"startMatrix", "endMatrix"}});
    registerWorldAttrs("angleMeasure", {.matrices = {"point1Matrix", "vertexMatrix", "point2Matrix"}});
    registerWorldAttrs("radiusMeasure", {.matrices = {"meshMatrix"}, .pointLists = {"points"}});
}

glm::dvec3 displayOrigin()
{
    const json j = data::get(kDisplayOriginKey);
    if (!j.is_array() || j.size() != 3)
        return glm::dvec3(0.0);
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

bool hasDisplayOrigin()
{
    const json j = data::get(kDisplayOriginKey);
    return j.is_array() && j.size() == 3;
}

void setDisplayOrigin(const std::optional<glm::dvec3>& origin) { data::set(kDisplayOriginKey, origin ? vecJson(*origin) : json(nullptr)); }

void registerCommands()
{
    using T = ArgType;
    registerBuiltins();

    ops::registerCommand({
        .name = "relocateScene",
        .doc = "Move the whole scene (or chosen hierarchies) so a point becomes the world origin, keeping every rig, deformer and "
               "animation working.\n\n"
               "One rigid move M is applied: the roots take it through their channels and keys, and nodes that store world-space data "
               "(skin bind matrices, deformer bind matrices, world spaces, world up vectors, measures) are rewritten to match. Every "
               "transform and deformed mesh point is then checked at several frames (and in every take) against M times where it was; "
               "if anything is off by more than 1e-6 cm, or the scene holds a node type relocateScene doesn't know, nothing changes "
               "and the error says why. One undo step.",
        .positionalDoc = "objects the from= point is taken from (default: the selection)",
        .flags = {
            {"from", "", T::String, "bottom",
             "The point that moves to `to`: bottom (the bottom centre of the objects' bounds), center (their bounds centre), pivot (the "
             "first object's pivot), components (the centre of the selected components), or an object's name (its pivot).",
             {}, "relocateScene(from=\"Hero\")"},
            {"to", "", T::Vec3, nullptr, "Where the from= point lands, cm. Default the world origin."},
            {"offset", "", T::Vec3, nullptr, "Move by this world offset instead of from= and to=, cm.", {}, "relocateScene(offset=(-12000, 0, 3500))"},
            {"axes", "", T::String, "xyz", "Only move along these world axes; 'xz' keeps every height."},
            {"rotation", "", T::String, "none",
             "none; yaw: turn about the up axis so the from object faces along the world axes; full: turn so its whole orientation "
             "lines up with the world. Turns about the from= point.",
             {"none", "yaw", "full"}},
            {"nodes", "", T::List, nullptr, "Move only these hierarchies (their tops) instead of the whole scene.", {},
             "relocateScene(\"Hero\", nodes=[\"Hero\"])"},
            {"selectedOnly", "", T::Bool, false, "Move only the hierarchies of the objects given (the selection by default), as nodes= does."},
            {"dryRun", "", T::Bool, false, "Plan and check, but change nothing; returns what would happen."},
        },
        .run = [](const Args& a) -> json { return relocate(a); },
        .examples = {"relocateScene(\"Ground\", from=\"bottom\")", "relocateScene(\"Hero\", from=\"Hero\", rotation=\"yaw\", nodes=[\"Hero\"])"},
    });

    const std::vector<std::pair<std::string, std::string>> fromChoices = {
        {"bottom", "Bottom centre"}, {"center", "Centre"}, {"pivot", "Pivot"}, {"components", "Selected components"}};
    ops::options::setOverride("relocateScene", {.order = {"from", "to", "axes", "rotation", "selectedOnly", "offset"},
                                                .hidden = {"dryRun"},
                                                .labels = {{"from", "Move from"}, {"selectedOnly", "Only the selected hierarchies"}},
                                                .choices = {{"from", fromChoices}}});
    ops::options::setOverride("setDisplayOrigin", {.order = {"from", "position"}, .hidden = {"clear"}, .choices = {{"from", fromChoices}}});

    ops::registerCommand({
        .name = "setDisplayOrigin",
        .doc = "Draw the grid and world axes, and read world positions, relative to a point, without moving anything in the scene. "
               "Saved with the scene; clear=True goes back to the world origin.",
        .positionalDoc = "objects the from= point is taken from (default: the selection)",
        .flags = {
            {"position", "", T::Vec3, nullptr, "The display origin, world cm."},
            {"from", "", T::String, nullptr, "Take it from the objects instead: bottom, center, pivot, components or an object's name, as relocateScene's from=."},
            {"clear", "", T::Bool, false, "Back to the world origin."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.get<bool>("clear"))
                setDisplayOrigin(std::nullopt);
            else if (a.has("position"))
                setDisplayOrigin(vec3Arg(a.flags.at("position")));
            else if (a.has("from"))
            {
                g.evaluate();
                setDisplayOrigin(anchorFor(g, a.get<std::string>("from"), nodesNamed(g, a.objects(), "setDisplayOrigin"), "setDisplayOrigin").point);
            }
            else
                throw CommandError("setDisplayOrigin: give position=, from= or clear=True");
            return originJson();
        },
    });

    ops::registerCommand({
        .name = "displayOrigin",
        .doc = "The display origin setDisplayOrigin set, world cm; None when the grid sits at the world origin.",
        .undoable = false,
        .run = [](const Args&) -> json { return originJson(); },
    });
}
} // namespace lila::scene::relocate
