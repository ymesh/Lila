#include "scene/Strings.h"

#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/Skin.h"
#include "scene/RigUtil.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <stdexcept>

namespace lila::scene::strings
{
using namespace rigutil;

namespace
{
glm::dmat4 parentWorld(const fbik::Problem& p, const std::vector<glm::dmat4>& w, int i)
{
    const fbik::Bone& b = p.bones[size_t(i)];
    return b.parent < 0 ? b.parentWorld : w[size_t(b.parent)];
}

glm::dvec3 lengthening(const fbik::Problem& p, const Pose& s, int i)
{
    return (s.stretch[size_t(i)] - 1.0) * glm::dvec3(p.bones[size_t(i)].before[3]);
}

/**
 * Sets bone i's rotation (and with translate its offset) so its world becomes target, its parent where w has it. A
 * bone's world is parent * T(offset + lengthening) * before * R * after.
 */
void place(const fbik::Problem& p, Pose& s, const std::vector<glm::dmat4>& w, int i, const glm::dmat4& target, bool translate)
{
    const fbik::Bone& b = p.bones[size_t(i)];
    const glm::dmat4 m = glm::inverse(parentWorld(p, w, i)) * target * glm::inverse(b.after);
    const glm::dmat3 r = glm::inverse(glm::dmat3(b.before)) * glm::dmat3(m);
    s.rotation[size_t(i)] = glm::normalize(glm::quat_cast(orthonormal(r)));
    if (translate)
        s.offset[size_t(i)] = glm::dvec3(m[3]) - glm::dvec3(b.before[3]) - lengthening(p, s, i);
}

glm::dmat4 turnedAbout(const glm::dmat4& world, const glm::dquat& q)
{
    const glm::dvec3 c(world[3]);
    return glm::translate(glm::dmat4(1.0), c) * glm::dmat4(glm::mat3_cast(q)) * glm::translate(glm::dmat4(1.0), -c) * world;
}

glm::dmat4 shifted(const glm::dmat4& world, const glm::dvec3& by) { return glm::translate(glm::dmat4(1.0), by) * world; }

double holdError(const std::vector<glm::dmat4>& w, const std::vector<Hold>& holds)
{
    double e = 0.0;
    for (const Hold& h : holds)
        e = std::max(e, glm::length(glm::dvec3(w[size_t(h.bone)][3]) - glm::dvec3(h.world[3])));
    return e;
}

/** The holds at or below bone, in their given order. */
std::vector<Hold> holdsUnder(const fbik::Problem& p, const std::vector<Hold>& holds, int bone)
{
    std::vector<Hold> out;
    for (const Hold& h : holds)
        if (isUnder(p, h.bone, bone))
            out.push_back(h);
    return out;
}

void forwardTurn(const fbik::Problem& p, Pose& s, int g, const glm::dquat& q)
{
    const std::vector<glm::dmat4> w = worlds(p, s);
    place(p, s, w, g, turnedAbout(w[size_t(g)], q), false);
}

glm::dquat turnBetween(const glm::dvec3& from, const glm::dvec3& to)
{
    const glm::dvec3 a = glm::normalize(from), b = glm::normalize(to);
    const double c = glm::dot(a, b);
    if (c > 1.0 - 1e-12)
        return glm::dquat(1.0, 0.0, 0.0, 0.0);
    glm::dvec3 axis = glm::cross(a, b);
    if (glm::length(axis) < 1e-9)
        axis = glm::abs(a.x) < 0.9 ? glm::cross(a, glm::dvec3(1.0, 0.0, 0.0)) : glm::cross(a, glm::dvec3(0.0, 1.0, 0.0));
    return glm::angleAxis(std::acos(std::clamp(c, -1.0, 1.0)), glm::normalize(axis));
}

/**
 * Two-bone start for a limb (upper, lower, end): the elbow placed by the law of cosines in the limb's current bend
 * plane, so the solve that follows starts near the answer. Damped least squares alone stalls on a nearly straight limb
 * pulled inward.
 */
void twoBoneStart(const fbik::Problem& p, Pose& s, int upper, int lower, int end, const glm::dvec3& target)
{
    std::vector<glm::dmat4> w = worlds(p, s);
    const glm::dvec3 a(w[size_t(upper)][3]), b(w[size_t(lower)][3]), c(w[size_t(end)][3]);
    const double l1 = glm::length(b - a), l2 = glm::length(c - b);
    if (l1 < 1e-9 || l2 < 1e-9 || glm::length(target - a) < 1e-9)
        return;
    const glm::dvec3 dir = glm::normalize(target - a);
    const double d = std::clamp(glm::length(target - a), std::abs(l1 - l2) + 1e-6, l1 + l2 - 1e-6);
    glm::dvec3 bend = (b - a) - glm::dot(b - a, dir) * dir;
    if (glm::length(bend) < 1e-6 * l1)
    {
        const glm::dvec3 reach = glm::normalize(c - a);
        bend = (b - a) - glm::dot(b - a, reach) * reach;
        bend -= glm::dot(bend, dir) * dir;
        if (glm::length(bend) < 1e-9)
            return;
    }
    bend = glm::normalize(bend);
    const double angle = std::acos(std::clamp((l1 * l1 + d * d - l2 * l2) / (2.0 * l1 * d), -1.0, 1.0));
    const glm::dvec3 elbow = a + l1 * (std::cos(angle) * dir + std::sin(angle) * bend);
    place(p, s, w, upper, turnedAbout(w[size_t(upper)], turnBetween(b - a, elbow - a)), false);
    w = worlds(p, s);
    const glm::dvec3 b2(w[size_t(lower)][3]), c2(w[size_t(end)][3]);
    place(p, s, w, lower, turnedAbout(w[size_t(lower)], turnBetween(c2 - b2, a + d * dir - b2)), false);
}

/** Full body: every solved bone may turn and the hips move; the grabbed joint's target and the holds are pins. */
Result fullBody(const fbik::Problem& p, const Request& r)
{
    fbik::Problem q = p;
    const std::vector<glm::dmat4> w = worlds(p, startPose(p));
    const glm::dmat4& g = w[size_t(r.grabbed)];
    fbik::Effector e{.bone = r.grabbed, .position = glm::dvec3(g[3]) + r.move.value_or(glm::dvec3(0.0))};
    if (r.turn)
        e.rotation = glm::normalize(glm::quat_cast(orthonormal(glm::dmat3(turnedAbout(g, *r.turn)))));
    q.effectors = {e};
    for (const Hold& h : r.holds)
        if (h.bone != r.grabbed)
            q.effectors.push_back({.bone = h.bone,
                                   .position = glm::dvec3(h.world[3]),
                                   .rotation = h.rotation ? std::optional(glm::normalize(glm::quat_cast(orthonormal(glm::dmat3(h.world)))))
                                                          : std::nullopt});
    if (r.hips >= 0)
        q.bones[size_t(r.hips)].translate = true;
    if (r.maxStretch > 1.0)
        for (fbik::Bone& b : q.bones)
            b.maxStretch = r.maxStretch;
    const fbik::Solution sol = fbik::solve(q);
    Result out;
    out.pose = {sol.rotation, sol.offset, sol.stretch.empty() ? std::vector<double>(p.bones.size(), 1.0) : sol.stretch};
    out.error = sol.error;
    return out;
}
} // namespace

const char* modeName(Mode m)
{
    switch (m)
    {
    case Mode::Forward: return "forward";
    case Mode::Backward: return "backward";
    case Mode::Ik: return "ik";
    case Mode::FullBody: return "fullBody";
    }
    return "forward";
}

std::optional<Mode> modeFromName(std::string_view name)
{
    for (Mode m : {Mode::Forward, Mode::Backward, Mode::Ik, Mode::FullBody})
        if (name == modeName(m))
            return m;
    return std::nullopt;
}

Pose startPose(const fbik::Problem& p)
{
    Pose s;
    for (const fbik::Bone& b : p.bones)
        s.rotation.push_back(b.rotation);
    s.offset.assign(p.bones.size(), glm::dvec3(0.0));
    s.stretch.assign(p.bones.size(), 1.0);
    return s;
}

std::vector<glm::dmat4> worlds(const fbik::Problem& p, const Pose& s) { return fbik::forward(p, s.rotation, s.offset, s.stretch); }

bool isUnder(const fbik::Problem& p, int bone, int ancestor)
{
    for (int b = bone; b >= 0; b = p.bones[size_t(b)].parent)
        if (b == ancestor)
            return true;
    return false;
}

std::vector<int> limbOf(const fbik::Problem& p, int bone, int hips, int stop, int length, const Rules* rules)
{
    std::vector<int> out;
    if (length > 0)
    {
        for (int b = bone; b >= 0 && b != hips && b != stop && int(out.size()) < length; b = p.bones[size_t(b)].parent)
            if (p.bones[size_t(b)].solve)
                out.push_back(b);
        return out;
    }
    if (rules)
        for (const std::vector<int>& chain : rules->chains)
            if (const auto at = std::find(chain.begin(), chain.end(), bone); at != chain.end())
            {
                for (auto it = std::make_reverse_iterator(at + 1); it != chain.rend() && *it != hips && *it != stop; ++it)
                {
                    if (p.bones[size_t(*it)].solve)
                        out.push_back(*it);
                    if (length > 0 && int(out.size()) >= length)
                        break;
                }
                return out;
            }

    const size_t n = p.bones.size();
    // A limb continues through a joint with one child carrying two or more joints below it: twist joints and corrective
    // helpers (a root with leaves) don't count.
    std::vector<int> height(n, 0);
    for (size_t i = n; i-- > 0;)
        if (const int parent = p.bones[i].parent; parent >= 0)
            height[size_t(parent)] = std::max(height[size_t(parent)], height[i] + 1);
    std::vector<int> limbs(n, 0);
    for (size_t i = 0; i < n; ++i)
        if (const int parent = p.bones[i].parent; parent >= 0 && height[i] >= 2)
            ++limbs[size_t(parent)];

    for (int b = bone; b >= 0 && b != hips && b != stop; b = p.bones[size_t(b)].parent)
    {
        if (b != bone && limbs[size_t(b)] >= 2)
            break;
        if (p.bones[size_t(b)].solve)
            out.push_back(b);
        if (length > 0 && int(out.size()) >= length)
            break;
    }
    return out;
}

double holdLimbs(const fbik::Problem& p, Pose& s, const std::vector<Hold>& holds, int hips, double maxStretch,
                 const std::vector<std::pair<int, glm::dvec3>>& soft, const Rules* rules)
{
    if (holds.empty())
        return 0.0;
    std::vector<bool> bend(p.bones.size(), false);
    for (const Hold& h : holds)
        for (int b : limbOf(p, h.bone, hips, -1, 0, rules))
            bend[size_t(b)] = true;
    if (std::find(bend.begin(), bend.end(), true) == bend.end())
        return holdError(worlds(p, s), holds);
    for (const Hold& h : holds)
        if (const std::vector<int> limb = limbOf(p, h.bone, hips, -1, 0, rules); limb.size() >= 3 && limb[0] == h.bone)
            twoBoneStart(p, s, limb[2], limb[1], limb[0], glm::dvec3(h.world[3]));

    // The pose so far becomes the problem's: offsets and stretches folded into each bone, its rotation the reference.
    fbik::Problem q = p;
    q.effectors.clear();
    for (size_t i = 0; i < q.bones.size(); ++i)
    {
        fbik::Bone& b = q.bones[i];
        b.before = glm::translate(glm::dmat4(1.0), s.offset[i] + lengthening(p, s, int(i))) * b.before;
        b.rotation = b.reference = s.rotation[i];
        b.translate = false;
        b.offset = glm::dvec3(0.0);
        b.solve = bend[i];
        const bool limbTop = b.parent < 0 || !bend[size_t(b.parent)];
        b.maxStretch = bend[i] && !limbTop ? std::max(1.0, maxStretch) : 1.0;
    }
    for (const Hold& h : holds)
        q.effectors.push_back({.bone = h.bone,
                               .position = glm::dvec3(h.world[3]),
                               .rotation = h.rotation ? std::optional(glm::normalize(glm::quat_cast(orthonormal(glm::dmat3(h.world))))) : std::nullopt});
    for (const auto& [bone, at] : soft)
        if (bend[size_t(bone)])
            q.effectors.push_back({.bone = bone, .position = at, .soft = true});
    const fbik::Solution sol = fbik::solve(q);
    for (size_t i = 0; i < q.bones.size(); ++i)
        if (bend[i])
        {
            s.rotation[i] = sol.rotation[i];
            if (i < sol.stretch.size())
                s.stretch[i] *= sol.stretch[i];
        }
    return holdError(worlds(p, s), holds);
}

Result solve(const fbik::Problem& p, const Request& r)
{
    LILA_ZONE();
    if (r.grabbed < 0 || size_t(r.grabbed) >= p.bones.size())
        throw std::invalid_argument("strings: the grabbed joint is not in the skeleton");
    if (r.mode == Mode::FullBody)
        return fullBody(p, r);

    Result out;
    out.pose = startPose(p);
    Pose& s = out.pose;
    const int g = r.grabbed;
    const std::vector<glm::dmat4> start = worlds(p, s);
    const bool belowHips = r.hips >= 0 && isUnder(p, g, r.hips) && g != r.hips;

    if (r.mode == Mode::Forward)
    {
        if (r.turn)
            forwardTurn(p, s, g, *r.turn);
        if (r.move)
            place(p, s, start, g, shifted(start[size_t(g)], *r.move), true);
        return out;
    }

    if (r.mode == Mode::Backward)
    {
        if (r.hips >= 0 && !isUnder(p, g, r.hips))
        {
            out.handled = false;
            return out;
        }
        std::vector<Hold> others = r.holds;
        if (r.turn)
        {
            forwardTurn(p, s, g, *r.turn);
            if (belowHips)
            {
                // What stays still: the first hold below the grabbed joint, else the joint itself, so the body hangs from it.
                const std::vector<Hold> below = holdsUnder(p, r.holds, g);
                out.anchor = below.empty() ? g : below.front().bone;
                const std::vector<glm::dmat4> turned = worlds(p, s);
                const glm::dmat4 undo = start[size_t(out.anchor)] * glm::inverse(turned[size_t(out.anchor)]);
                place(p, s, turned, r.hips, undo * turned[size_t(r.hips)], true);
                std::erase_if(others, [&](const Hold& h) { return h.bone == out.anchor; });
            }
        }
        std::vector<std::pair<int, glm::dvec3>> soft;
        if (r.move && r.hips >= 0)
        {
            // The body is carried by the grabbed joint; the limbs that hold bend under it.
            const std::vector<glm::dmat4> w = worlds(p, s);
            place(p, s, w, r.hips, shifted(w[size_t(r.hips)], *r.move), true);
            soft.push_back({g, glm::dvec3(start[size_t(g)][3]) + *r.move});
        }
        out.error = holdLimbs(p, s, others, r.hips, r.maxStretch, soft, r.rules);
        return out;
    }

    // IK.
    if (r.hips >= 0 && !isUnder(p, g, r.hips))
    {
        out.handled = false;
        return out;
    }
    if (r.turn)
    {
        // Turning an effector turns it in place; holds under it bend their limbs back, the hips holding the feet.
        forwardTurn(p, s, g, *r.turn);
        out.error = holdLimbs(p, s, holdsUnder(p, r.holds, g), g == r.hips ? r.hips : g, r.maxStretch, {}, r.rules);
    }
    if (r.move)
    {
        if (g == r.hips)
        {
            const std::vector<glm::dmat4> w = worlds(p, s);
            place(p, s, w, g, shifted(w[size_t(g)], *r.move), true);
            out.error = holdLimbs(p, s, r.holds, r.hips, r.maxStretch, {}, r.rules);
        }
        else
        {
            const std::vector<glm::dmat4> w = worlds(p, s);
            Hold target{.bone = g, .world = shifted(w[size_t(g)], *r.move)};
            // The chain: the limb above the grabbed joint, or the bones asked for.
            std::vector<Hold> holds = {target};
            for (const Hold& h : r.holds)
                if (h.bone != g)
                    holds.push_back(h);
            if (r.chainLength > 0)
            {
                // A set length runs through branches: bend only those bones, the rest held.
                fbik::Problem q = p;
                std::vector<int> chain;
                for (int b = g; b >= 0 && int(chain.size()) < r.chainLength + 1; b = p.bones[size_t(b)].parent)
                    if (p.bones[size_t(b)].solve)
                        chain.push_back(b);
                for (size_t i = 0; i < q.bones.size(); ++i)
                {
                    q.bones[i].solve = std::find(chain.begin(), chain.end(), int(i)) != chain.end();
                    q.bones[i].translate = false;
                    q.bones[i].reference = q.bones[i].rotation = s.rotation[i];
                }
                q.effectors = {{.bone = g, .position = glm::dvec3(target.world[3]),
                                .rotation = glm::normalize(glm::quat_cast(orthonormal(glm::dmat3(target.world))))}};
                const fbik::Solution sol = fbik::solve(q);
                for (int b : chain)
                    s.rotation[size_t(b)] = sol.rotation[size_t(b)];
                out.error = holdLimbs(p, s, std::vector<Hold>(holds.begin() + 1, holds.end()), r.hips, r.maxStretch, {}, r.rules);
                out.error = std::max(out.error, glm::length(glm::dvec3(worlds(p, s)[size_t(g)][3]) - glm::dvec3(target.world[3])));
            }
            else
                out.error = holdLimbs(p, s, holds, r.hips, r.maxStretch, {}, r.rules);
        }
    }
    return out;
}

int Skeleton::boneOf(NodeId joint) const
{
    const auto it = std::find(joints.begin(), joints.end(), joint);
    return it == joints.end() ? -1 : int(it - joints.begin());
}

Skeleton snapshot(Graph& g, NodeId root, const std::vector<std::pair<NodeId, double>>& stiffness)
{
    Skeleton k;
    fbik::PoseOptions opts;
    opts.moveRoot = true;
    opts.stiffness = stiffness;
    k.problem = fbik::problemFor(g, root, opts, k.joints);
    for (size_t i = 0; i < k.problem.bones.size(); ++i)
        if (k.problem.bones[i].translate)
        {
            k.hips = int(i);
            k.problem.bones[i].translate = false;
        }
    return k;
}

std::vector<glm::dvec3> trail(Graph& g, NodeId node, const std::vector<double>& times)
{
    LILA_ZONE();
    for (const auto& [dst, src] : g.connections())
        if (g.isHeld(dst))
            return {};
    const double now = g.time();
    std::vector<glm::dvec3> out;
    out.reserve(times.size());
    for (double t : times)
    {
        g.setTime(t, false);
        g.evaluateUpstream({node});
        out.push_back(glm::dvec3(g.node(node)->as<glm::dmat4>(Transform::kWorldMatrix)[3]));
    }
    g.setTime(now, false);
    g.evaluateUpstream({node});
    return out;
}

std::optional<glm::dmat4> bindWorld(Graph& g, NodeId joint)
{
    for (NodeId id : g.nodes())
        if (const Node* n = g.node(id); n && n->type().isA("skinInfluence"))
            if (const auto src = g.source({id, SkinInfluenceNode::kMatrix, -1}); src && src->node == joint)
                return glm::inverse(n->as<glm::dmat4>(SkinInfluenceNode::kBindPreMatrix));
    return std::nullopt;
}

std::vector<std::string> apply(Graph& g, const Skeleton& k, const Pose& s)
{
    std::vector<std::string> plugs;
    for (size_t i = 0; i < k.joints.size(); ++i)
    {
        Node& j = *g.node(k.joints[i]);
        const fbik::Bone& b = k.problem.bones[i];
        if (glm::abs(glm::dot(s.rotation[i], b.rotation)) < 1.0 - 1e-14)
        {
            const auto order = RotateOrder(j.as<int64_t>(Transform::kRotateOrder));
            const glm::dvec3 now = j.as<glm::dvec3>(Transform::kRotate);
            const glm::dvec3 rotate = xformops::closestEuler(glm::mat3_cast(s.rotation[i]), order, now);
            if (glm::length(rotate - now) > 1e-9)
            {
                g.setValue({k.joints[i], Transform::kRotate}, clean(rotate));
                plugs.push_back(j.path() + ".rotate");
            }
        }
        const glm::dvec3 move = s.offset[i] + (s.stretch[i] - 1.0) * glm::dvec3(b.before[3]);
        if (glm::length(move) > 1e-9)
        {
            g.setValue({k.joints[i], Transform::kTranslate}, j.as<glm::dvec3>(Transform::kTranslate) + move);
            plugs.push_back(j.path() + ".translate");
        }
    }
    return plugs;
}

double strokeLength(const std::vector<glm::dvec3>& stroke)
{
    double l = 0.0;
    for (size_t i = 1; i < stroke.size(); ++i)
        l += glm::length(stroke[i] - stroke[i - 1]);
    return l;
}

namespace
{
/** The point at arc length s along a polyline, clamped to its ends. */
glm::dvec3 alongStroke(const std::vector<glm::dvec3>& q, double s)
{
    if (s <= 0.0)
        return q.front();
    for (size_t i = 1; i < q.size(); ++i)
    {
        const double l = glm::length(q[i] - q[i - 1]);
        if (s <= l && l > 0.0)
            return glm::mix(q[i - 1], q[i], s / l);
        s -= l;
    }
    return q.back();
}
} // namespace

glm::dvec3 conformAt(const std::vector<glm::dvec3>& path, std::vector<glm::dvec3> stroke, double window, double fraction)
{
    if (path.empty())
        return glm::dvec3(0.0);
    if (stroke.size() < 2)
        return path.front();
    if (glm::length(stroke.back() - path.front()) < glm::length(stroke.front() - path.front()))
        std::reverse(stroke.begin(), stroke.end());
    double l = 0.0;
    for (size_t i = 1; i < path.size(); ++i)
        l += glm::length(path[i] - path[i - 1]);
    const double m = strokeLength(stroke), s = fraction * l;
    if (window >= 0.0)
        return alongStroke(stroke, window + s);
    if (m <= 1e-9 || l <= 1e-9)
        return path.front();
    return path.front() + (alongStroke(stroke, fraction * m) - stroke.front()) * (l / m);
}

double pathFraction(const std::vector<glm::dvec3>& path, const glm::dvec3& p)
{
    double total = 0.0, best = 1e300, at = 0.0;
    for (size_t i = 1; i < path.size(); ++i)
    {
        const glm::dvec3 d = path[i] - path[i - 1];
        const double len = glm::length(d);
        const double t = len > 1e-12 ? std::clamp(glm::dot(p - path[i - 1], d) / (len * len), 0.0, 1.0) : 0.0;
        if (const double e = glm::length(path[i - 1] + t * d - p); e < best)
        {
            best = e;
            at = total + t * len;
        }
        total += len;
    }
    return total > 1e-12 ? at / total : 0.0;
}

std::vector<glm::dvec3> conformTargets(const std::vector<glm::dvec3>& path, std::vector<glm::dvec3> stroke, double window)
{
    if (path.empty() || stroke.size() < 2)
        return path;
    std::vector<double> along{0.0};
    for (size_t i = 1; i < path.size(); ++i)
        along.push_back(along.back() + glm::length(path[i] - path[i - 1]));
    std::vector<glm::dvec3> out;
    for (double s : along)
        out.push_back(conformAt(path, stroke, window, along.back() > 1e-12 ? s / along.back() : 0.0));
    return out;
}

Result conform(const fbik::Problem& p, const std::vector<int>& path, const std::vector<glm::dvec3>& targets, const std::vector<Hold>& holds, int hips,
               double maxStretch)
{
    LILA_ZONE();
    fbik::Problem q = p;
    q.effectors.clear();
    for (size_t i = 0; i < path.size() && i < targets.size(); ++i)
        q.effectors.push_back({.bone = path[i], .position = targets[i]});
    for (const Hold& h : holds)
        if (std::find(path.begin(), path.end(), h.bone) == path.end())
            q.effectors.push_back({.bone = h.bone,
                                   .position = glm::dvec3(h.world[3]),
                                   .rotation = h.rotation ? std::optional(glm::normalize(glm::quat_cast(orthonormal(glm::dmat3(h.world))))) : std::nullopt});
    if (hips >= 0)
        q.bones[size_t(hips)].translate = true;
    // With its first joint fixed, the path bends alone: nothing above or beside it turns to help.
    else if (!path.empty())
        for (size_t b = 0; b < q.bones.size(); ++b)
            if (!isUnder(p, int(b), path.front()))
                q.bones[b].solve = false;
    if (maxStretch > 1.0)
        for (fbik::Bone& b : q.bones)
            b.maxStretch = maxStretch;
    const fbik::Solution sol = fbik::solve(q);
    Result out;
    out.pose = {sol.rotation, sol.offset, sol.stretch.empty() ? std::vector<double>(p.bones.size(), 1.0) : sol.stretch};
    out.error = sol.error;
    // The solve shares its last iterations between the path and the holds; the holds' limbs then close what is left exactly.
    std::vector<Hold> kept;
    for (const Hold& h : holds)
        if (std::find(path.begin(), path.end(), h.bone) == path.end())
            kept.push_back(h);
    if (!kept.empty())
        holdLimbs(p, out.pose, kept, hips >= 0 ? hips : (path.empty() ? -1 : path.front()), maxStretch);
    return out;
}
} // namespace lila::scene::strings
