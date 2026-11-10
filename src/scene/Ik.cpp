#include "scene/Ik.h"

#include "core/Profile.h"
#include "scene/AttrBuilders.h"
#include "scene/IkSolver.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <set>

namespace lila::scene
{
using namespace rigutil;
using namespace attrdefs;

namespace
{
constexpr const char* kTwoBone = "ik2BoneSolver";
constexpr const char* kFabrik = "ikFabrikSolver";
constexpr const char* kSpline = "ikSplineSolver";

glm::dvec3 translationOf(const glm::dmat4& m) { return glm::dvec3(m[3]); }

glm::dmat3 axisAngle(const glm::dvec3& axis, double radians) { return glm::mat3_cast(glm::angleAxis(radians, glm::normalize(axis))); }

/** Shortest rotation taking direction a onto b (both unit). */
glm::dmat3 arc(const glm::dvec3& a, const glm::dvec3& b)
{
    double c = std::clamp(glm::dot(a, b), -1.0, 1.0);
    glm::dvec3 axis = glm::cross(a, b);
    if (glm::length(axis) < 1e-12)
    {
        if (c > 0.0)
            return glm::dmat3(1.0);
        glm::dvec3 any = std::abs(a.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
        return axisAngle(glm::cross(a, any), std::numbers::pi);
    }
    return axisAngle(axis, std::atan2(glm::length(axis), c));
}

glm::dvec3 perpendicular(const glm::dvec3& v, const glm::dvec3& axis)
{
    glm::dvec3 p = v - glm::dot(v, axis) * axis;
    return glm::length(p) > 1e-9 ? glm::normalize(p) : glm::dvec3(0.0);
}

/** The bend direction around dir: toward the pole when there is one, else fkBend, rolled by twist about dir. */
glm::dvec3 bendDirection(const ik::Problem& p, const glm::dvec3& root, const glm::dvec3& dir, const glm::dvec3& fkBend)
{
    glm::dvec3 u = p.pole ? perpendicular(*p.pole - root, dir) : glm::dvec3(0.0);
    if (u == glm::dvec3(0.0))
        u = perpendicular(fkBend, dir);
    if (u == glm::dvec3(0.0))
        u = perpendicular(std::abs(dir.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0), dir);
    if (p.twist != 0.0)
        u = axisAngle(dir, p.twist) * u;
    return u;
}

/** Solved world rotations (one per joint but the end) as rotations relative to each parent's solved frame. */
ik::Solution toLocal(const ik::Problem& p, const std::vector<glm::dmat3>& world)
{
    ik::Solution out;
    glm::dmat3 parent = orthonormal(glm::dmat3(p.parentWorld));
    for (const glm::dmat3& w : world)
    {
        out.push_back(glm::normalize(glm::quat_cast(glm::transpose(parent) * w)));
        parent = w;
    }
    return out;
}

/** Turns each joint by the shortest arc from where its bone points after its parent turned to where it should point. */
std::vector<glm::dmat3> followPositions(const std::vector<glm::dmat4>& world, const std::vector<glm::dvec3>& pos)
{
    std::vector<glm::dmat3> out(world.size() - 1);
    glm::dmat3 inherited(1.0);
    for (size_t i = 0; i + 1 < world.size(); ++i)
    {
        glm::dvec3 fkDir = translationOf(world[i + 1]) - translationOf(world[i]);
        glm::dvec3 newDir = pos[i + 1] - pos[i];
        glm::dmat3 turn = inherited;
        if (glm::length(fkDir) > 1e-12 && glm::length(newDir) > 1e-12)
            turn = arc(glm::normalize(inherited * glm::normalize(fkDir)), glm::normalize(newDir)) * inherited;
        out[i] = turn * orthonormal(glm::dmat3(world[i]));
        inherited = turn;
    }
    return out;
}

glm::dquat quatOf(const glm::dmat3& m) { return glm::normalize(glm::quat_cast(m)); }

bool limited(const ik::Problem& p)
{
    return std::any_of(p.limits.begin(), p.limits.end(), [](const ik::JointLimit& l) { return l.limits(); });
}

/** A local rotation held to the joint's limit, after its stiffness eased it back toward where it started. */
glm::dmat3 limitLocal(const ik::JointLimit& l, const glm::dmat3& local, const glm::dmat3& start)
{
    glm::dquat q = quatOf(local);
    if (l.stiffness > 0.0)
        q = glm::slerp(quatOf(start), q, 1.0 - std::clamp(l.stiffness, 0.0, 1.0));
    if (l.kind != ik::JointLimit::Kind::None)
        q = l.frame * ik::clampRotate(l, glm::inverse(l.frame) * q * glm::inverse(l.post)) * l.post;
    return glm::mat3_cast(q);
}

/** Per joint but the end: the bone's unit direction in the joint's own frame, its length, and its starting local rotation. */
struct Bones
{
    std::vector<glm::dvec3> dir;
    std::vector<double> len;
    std::vector<glm::dmat3> start;
};

Bones bonesOf(const ik::Problem& p)
{
    Bones b;
    glm::dmat3 parent = orthonormal(glm::dmat3(p.parentWorld));
    for (size_t i = 0; i + 1 < p.world.size(); ++i)
    {
        const glm::dmat3 r = orthonormal(glm::dmat3(p.world[i]));
        const glm::dvec3 d = translationOf(p.world[i + 1]) - translationOf(p.world[i]);
        const double l = glm::length(d);
        b.dir.push_back(l > 1e-12 ? glm::transpose(r) * (d / l) : glm::dvec3(1, 0, 0));
        b.len.push_back(l);
        b.start.push_back(glm::transpose(parent) * r);
        parent = r;
    }
    return b;
}

/** Turns each joint from its start toward the next position, held to its limits; pos gets where the joints land. */
std::vector<glm::dmat3> forwardLimited(const ik::Problem& p, const Bones& b, std::vector<glm::dvec3>& pos)
{
    std::vector<glm::dmat3> world(b.len.size());
    glm::dmat3 parent = orthonormal(glm::dmat3(p.parentWorld));
    for (size_t i = 0; i < b.len.size(); ++i)
    {
        glm::dmat3 r = parent * b.start[i];
        if (const glm::dvec3 want = pos[i + 1] - pos[i]; glm::length(want) > 1e-12)
            r = arc(r * b.dir[i], glm::normalize(want)) * r;
        glm::dmat3 local = glm::transpose(parent) * r;
        if (i < p.limits.size())
            local = limitLocal(p.limits[i], local, b.start[i]);
        world[i] = parent * local;
        pos[i + 1] = pos[i] + world[i] * b.dir[i] * b.len[i];
        parent = world[i];
    }
    return world;
}

/**
 * Cyclic coordinate descent from a limited pose: each joint, end to root, turns the end toward the target as far as its
 * limit allows (a stiff joint less). Reaches where FABRIK's shortest arcs stall, such as a hinged knee whose hip has to
 * swing the hinge's plane onto the target.
 */
std::vector<glm::dmat3> refineLimited(const ik::Problem& p, const Bones& b, const std::vector<glm::dmat3>& world)
{
    const size_t m = b.len.size();
    const glm::dmat3 root = orthonormal(glm::dmat3(p.parentWorld));
    std::vector<glm::dmat3> local(m), w(m);
    for (size_t i = 0; i < m; ++i)
        local[i] = glm::transpose(i == 0 ? root : world[i - 1]) * world[i];
    std::vector<glm::dvec3> pos(m + 1);
    auto pose = [&] {
        glm::dmat3 parent = root;
        pos[0] = translationOf(p.world[0]);
        for (size_t i = 0; i < m; ++i)
        {
            w[i] = parent * local[i];
            pos[i + 1] = pos[i] + w[i] * b.dir[i] * b.len[i];
            parent = w[i];
        }
        return glm::length(pos[m] - p.target);
    };
    double bestError = pose();
    std::vector<glm::dmat3> best = w;
    for (int it = 0; it < 256 && bestError > 1e-9; ++it)
    {
        for (size_t i = m; i-- > 0;)
        {
            pose();
            const glm::dvec3 toEnd = pos[m] - pos[i], toTarget = p.target - pos[i];
            if (glm::length(toEnd) < 1e-12 || glm::length(toTarget) < 1e-12)
                continue;
            glm::dquat turn = quatOf(arc(glm::normalize(toEnd), glm::normalize(toTarget)));
            const ik::JointLimit* l = i < p.limits.size() ? &p.limits[i] : nullptr;
            if (l && l->stiffness > 0.0)
                turn = glm::slerp(glm::dquat(1.0, 0.0, 0.0, 0.0), turn, 1.0 - std::clamp(l->stiffness, 0.0, 1.0));
            glm::dquat q = quatOf(glm::transpose(i == 0 ? root : w[i - 1]) * glm::mat3_cast(turn) * w[i]);
            if (l && l->kind != ik::JointLimit::Kind::None)
                q = l->frame * ik::clampRotate(*l, glm::inverse(l->frame) * q * glm::inverse(l->post)) * l->post;
            local[i] = glm::mat3_cast(q);
        }
        const double e = pose();        if (e >= bestError - 1e-15)
            break;
        bestError = e;
        best = w;
    }
    return best;
}

/**
 * Refines a free solution (world rotations per joint but the end) under the limits: FABRIK passes whose forward half
 * turns joints only as far as their limits allow. Keeps the closest pose found.
 */
std::vector<glm::dmat3> solveLimited(const ik::Problem& p, const std::vector<glm::dmat3>& free)
{
    const Bones b = bonesOf(p);
    const size_t n = p.world.size();
    std::vector<glm::dvec3> pos(n);
    pos[0] = translationOf(p.world[0]);
    for (size_t i = 0; i + 1 < n; ++i)
        pos[i + 1] = pos[i] + free[i] * b.dir[i] * b.len[i];
    std::vector<glm::dmat3> best = forwardLimited(p, b, pos);
    double bestError = glm::length(pos[n - 1] - p.target);
    auto safeDir = [](const glm::dvec3& v) { return glm::length(v) > 1e-12 ? glm::normalize(v) : glm::dvec3(1, 0, 0); };
    for (int it = 0, stalled = 0; it < p.maxIterations && bestError > p.tolerance && stalled < 8; ++it)
    {
        pos[n - 1] = p.target;
        for (size_t i = n - 1; i-- > 0;)
            pos[i] = pos[i + 1] + safeDir(pos[i] - pos[i + 1]) * b.len[i];
        pos[0] = translationOf(p.world[0]);
        std::vector<glm::dmat3> world = forwardLimited(p, b, pos);
        if (const double e = glm::length(pos[n - 1] - p.target); e < bestError - 1e-12)
        {
            best = std::move(world);
            bestError = e;
            stalled = 0;
        }
        else
            ++stalled;
    }
    return bestError > p.tolerance ? refineLimited(p, b, best) : best;
}

/** Analytic two-bone solve (law of cosines), bending toward the pole; exact whenever the target is in reach. */
ik::Solution solveTwoBone(const ik::Problem& p)
{
    LILA_ZONE();

    const glm::dvec3 p0 = translationOf(p.world[0]), p1 = translationOf(p.world[1]), p2 = translationOf(p.world[2]);
    const double a = glm::length(p1 - p0), b = glm::length(p2 - p1);
    const glm::dvec3 toTarget = p.target - p0, fkToEnd = p2 - p0;
    const double d = glm::length(toTarget);
    std::vector<glm::dmat3> world = {orthonormal(glm::dmat3(p.world[0])), orthonormal(glm::dmat3(p.world[1]))};
    if (d < 1e-12 || a < 1e-12 || b < 1e-12 || glm::length(fkToEnd) < 1e-12)
        return toLocal(p, world);

    glm::dvec3 dir = toTarget / d;
    glm::dvec3 fkDir = glm::normalize(fkToEnd);
    // An elbow on the target line has no bend to keep there; carry the FK chain's own bend over instead.
    glm::dvec3 u = bendDirection(p, p0, dir, perpendicular(p1 - p0, dir) == glm::dvec3(0.0) ? arc(fkDir, dir) * (p1 - p0) : p1 - p0);
    glm::dvec3 fkU = perpendicular(p1 - p0, fkDir);
    if (fkU == glm::dvec3(0.0))
        // A straight FK chain has no bend plane; take the one that turns the chain least.
        fkU = perpendicular(arc(dir, fkDir) * u, fkDir);
    glm::dvec3 n = glm::cross(dir, u);
    glm::dmat3 align = glm::dmat3(dir, u, n) * glm::transpose(glm::dmat3(fkDir, fkU, glm::cross(fkDir, fkU)));

    double reach = std::clamp(d, std::abs(a - b), a + b);
    double angleNew = std::acos(std::clamp((a * a + reach * reach - b * b) / (2.0 * a * reach), -1.0, 1.0));
    glm::dvec3 upper = glm::normalize(p1 - p0);
    double angleFk = std::atan2(glm::dot(upper, fkU), glm::dot(upper, fkDir));
    glm::dmat3 turn0 = axisAngle(n, angleNew - angleFk) * align;

    glm::dvec3 elbow = p0 + turn0 * (p1 - p0);
    glm::dvec3 want = p0 + dir * reach - elbow;
    glm::dmat3 turn1 = turn0;
    if (glm::length(want) > 1e-12)
        turn1 = arc(glm::normalize(turn0 * (p2 - p1)), glm::normalize(want)) * turn0;
    world = {turn0 * world[0], turn1 * world[1]};
    return toLocal(p, limited(p) ? solveLimited(p, world) : world);
}

/** Forward And Backward Reaching IK over any number of bones, then swung about the root-end axis toward the pole. */
ik::Solution solveFabrik(const ik::Problem& p)
{
    LILA_ZONE();

    size_t n = p.world.size();
    std::vector<glm::dvec3> pos(n);
    std::vector<double> len(n - 1);
    double reach = 0.0;
    for (size_t i = 0; i < n; ++i)
        pos[i] = translationOf(p.world[i]);
    for (size_t i = 0; i + 1 < n; ++i)
        reach += len[i] = glm::length(pos[i + 1] - pos[i]);
    const glm::dvec3 root = pos[0];
    auto safeDir = [](const glm::dvec3& v) { return glm::length(v) > 1e-12 ? glm::normalize(v) : glm::dvec3(1, 0, 0); };

    if (glm::length(p.target - root) >= reach)
    {
        glm::dvec3 dir = safeDir(p.target - root);
        for (size_t i = 0; i + 1 < n; ++i)
            pos[i + 1] = pos[i] + dir * len[i];
    }
    else
    {
        // Joints all on the target line would stay there; aim the chain's end at the target first so its bend survives.
        const glm::dvec3 line = safeDir(p.target - root);
        bool onLine = true;
        for (size_t i = 1; i + 1 < n && onLine; ++i)
            onLine = glm::length((pos[i] - root) - glm::dot(pos[i] - root, line) * line) < 1e-9;
        if (onLine && n > 2 && glm::length(pos[n - 1] - root) > 1e-12)
        {
            const glm::dmat3 aim = arc(glm::normalize(pos[n - 1] - root), line);
            for (size_t i = 1; i < n; ++i)
                pos[i] = root + aim * (pos[i] - root);
        }
        for (int it = 0; it < p.maxIterations && glm::length(pos[n - 1] - p.target) > p.tolerance; ++it)
        {
            pos[n - 1] = p.target;
            for (size_t i = n - 1; i-- > 0;)
                pos[i] = pos[i + 1] + safeDir(pos[i] - pos[i + 1]) * len[i];
            pos[0] = root;
            for (size_t i = 0; i + 1 < n; ++i)
                pos[i + 1] = pos[i] + safeDir(pos[i + 1] - pos[i]) * len[i];
        }
    }

    glm::dvec3 axis = pos[n - 1] - root;
    if (n > 2 && glm::length(axis) > 1e-12 && (p.pole || p.twist != 0.0))
    {
        axis = glm::normalize(axis);
        glm::dvec3 mid(0.0);
        for (size_t i = 1; i + 1 < n; ++i)
            mid += pos[i] - root;
        glm::dvec3 bend = perpendicular(mid, axis);
        if (bend != glm::dvec3(0.0))
        {
            glm::dvec3 want = bendDirection(p, root, axis, bend);
            glm::dmat3 swing = axisAngle(axis, std::atan2(glm::dot(glm::cross(bend, want), axis), glm::dot(bend, want)));
            for (size_t i = 1; i + 1 < n; ++i)
                pos[i] = root + swing * (pos[i] - root);
        }
    }
    std::vector<glm::dmat3> world = followPositions(p.world, pos);
    return toLocal(p, limited(p) ? solveLimited(p, world) : world);
}

/** The point at arc length s along a polyline (cumulative lengths in cum), continuing straight past its ends. */
glm::dvec3 alongPolyline(const std::vector<glm::dvec3>& pts, const std::vector<double>& cum, double s)
{
    if (pts.size() == 1)
        return pts[0];
    size_t k = size_t(std::upper_bound(cum.begin(), cum.end(), s) - cum.begin());
    k = std::clamp<size_t>(k, 1, pts.size() - 1);
    const double seg = cum[k] - cum[k - 1];
    const double t = seg > 1e-12 ? (s - cum[k - 1]) / seg : 0.0;
    return pts[k - 1] + (pts[k] - pts[k - 1]) * t;
}

/**
 * Spline IK: the chain laid along a curve from the start joint, each joint where the curve is a bone's length (chord)
 * from the one before, or with stretch every bone scaled so the chain spans the curve. Bones turn by the shortest arc
 * from their rest direction, then roll about themselves by roll plus the twist spread from start to end.
 */
ik::Solution solveSpline(const ik::Problem& p)
{
    LILA_ZONE();

    const size_t n = p.world.size();
    if (p.curve.size() < 2)
        return solveFabrik(p);
    std::vector<double> cum(p.curve.size(), 0.0);
    for (size_t i = 1; i < p.curve.size(); ++i)
        cum[i] = cum[i - 1] + glm::length(p.curve[i] - p.curve[i - 1]);
    std::vector<double> len(n - 1);
    double chain = 0.0;
    for (size_t i = 0; i + 1 < n; ++i)
        chain += len[i] = glm::length(translationOf(p.world[i + 1]) - translationOf(p.world[i]));

    std::vector<glm::dvec3> pos(n);
    pos[0] = translationOf(p.world[0]);
    if (p.fitCurve && chain > 1e-12)
    {
        double s = 0.0;
        for (size_t i = 0; i + 1 < n; ++i)
            pos[i + 1] = alongPolyline(p.curve, cum, s += len[i] * cum.back() / chain);
    }
    else
    {
        // March along the curve to where it leaves a sphere of the bone's length around the joint before.
        double s = 0.0;
        const double step = std::max(cum.back() / double(p.curve.size() * 4), 1e-6);
        for (size_t i = 0; i + 1 < n; ++i)
        {
            double lo = s, hi = s;
            while (glm::length(alongPolyline(p.curve, cum, hi) - pos[i]) < len[i] && hi - s < len[i] * 4.0 + cum.back())
            {
                lo = hi;
                hi += step;
            }
            for (int it = 0; it < 60; ++it)
            {
                const double mid = 0.5 * (lo + hi);
                (glm::length(alongPolyline(p.curve, cum, mid) - pos[i]) < len[i] ? lo : hi) = mid;
            }
            s = hi;
            const glm::dvec3 d = alongPolyline(p.curve, cum, s) - pos[i];
            pos[i + 1] = glm::length(d) > 1e-12 ? pos[i] + glm::normalize(d) * len[i] : pos[i];
        }
    }

    std::vector<glm::dmat3> world = followPositions(p.world, pos);
    for (size_t i = 0; i + 1 < n; ++i)
    {
        const glm::dvec3 bone = pos[i + 1] - pos[i];
        const double share = n > 2 ? double(i) / double(n - 2) : 1.0;
        const double angle = p.roll + p.twist * share;
        if (std::abs(angle) > 1e-15 && glm::length(bone) > 1e-12)
            world[i] = axisAngle(bone, angle) * world[i];
    }
    return toLocal(p, world);
}

AttrDef vec3Attr(std::string name, std::string shortName, glm::dvec3 def, uint32_t flags, std::string doc)
{
    AttrDef a{.name = name, .shortName = shortName, .type = PlugType::Vec3, .defaultValue = def, .flags = flags, .doc = std::move(doc)};
    for (int i = 0; i < 3; ++i)
    {
        a.childNames[i] = name + char('X' + i);
        a.childShort[i] = shortName.empty() ? "" : shortName + char('x' + i);
    }
    return a;
}

NodeType makeHandleType(const NodeType& base)
{
    const uint32_t hiddenStored = AttrInput | AttrStorable | AttrHidden;
    const uint32_t keyable = AttrInput | AttrStorable | AttrKeyable;
    NodeType t;
    t.name = "ikHandle";
    t.icon = "target-arrow";
    t.base = &base;
    t.dag = true;
    t.doc = "Moves a joint chain's end to the handle's position by solving the joints' rotations (IK).";
    t.ownAttrs = {
        {.name = "ikSolver", .shortName = "ikso", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "Solver id (ik2BoneSolver, ikFabrikSolver, or an extension's). Empty picks 2-bone for three joints, else FABRIK."},
        vec3Attr("poleVector", "pv", glm::dvec3(0.0), keyable, "Direction from the start joint the chain bends toward. Zero keeps the rest bend plane."),
        {.name = "poleAngle", .shortName = "pa", .type = PlugType::Float, .defaultValue = 0.0, .flags = keyable,
         .doc = "Roll of the bend plane about the start-to-handle axis, degrees."},
        {.name = "ikBlend", .shortName = "ikb", .type = PlugType::Float, .defaultValue = 1.0, .flags = keyable,
         .doc = "0 leaves the chain on its FK rotate, 1 is fully solved.", .minValue = 0.0, .maxValue = 1.0},
        {.name = "jointCount", .shortName = "jc", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = hiddenStored,
         .doc = "Joints in the chain, end effector included."},
        {.name = "chainParentMatrix", .shortName = "cpm", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
         .flags = AttrInput | AttrHidden, .doc = "World matrix of the start joint's parent."},
        {.name = "endEffector", .shortName = "ee", .type = PlugType::String, .defaultValue = std::string(), .flags = hiddenStored,
         .doc = "Name of the chain's last joint."},
        {.name = "softness", .shortName = "sft", .type = PlugType::Float, .defaultValue = 0.0, .flags = keyable,
         .doc = "Soft IK, cm: over this much of the chain's reach the end eases toward full extension instead of snapping straight.",
         .minValue = 0.0},
        {.name = "stretch", .shortName = "str", .type = PlugType::Bool, .defaultValue = false, .flags = keyable,
         .doc = "Lengthen the bones to reach a handle beyond the chain's length. ikSplineSolver: spread the joints over the whole curve."},
        {.name = "maxStretch", .shortName = "mxs", .type = PlugType::Float, .defaultValue = 1.5, .flags = keyable,
         .doc = "The longest the chain stretches, as a multiple of its length.", .minValue = 1.0},
        {.name = "stretchFactor", .shortName = "sfc", .type = PlugType::Float, .defaultValue = 1.0, .flags = AttrOutput | AttrHidden,
         .doc = "What the bones' lengths are multiplied by, blended by ikBlend; drives the joints' ikStretch."},
        {.name = "inCurve", .shortName = "ic", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden,
         .doc = "ikSplineSolver: the curve the chain follows, as world points (a curveToPoints node's output)."},
        {.name = "roll", .shortName = "rol", .type = PlugType::Float, .defaultValue = 0.0, .flags = keyable,
         .doc = "ikSplineSolver: turn of the whole chain about its bones, degrees. poleAngle twists the end past the start."},
    };
    for (int i = 0; i < IkHandle::kMaxJoints; ++i)
    {
        std::string n = std::to_string(i);
        t.ownAttrs.push_back({.name = "restLocal" + n, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = hiddenStored,
                              .doc = "The joint's FK local matrix at rest."});
        t.ownAttrs.push_back({.name = "orient" + n, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = hiddenStored,
                              .doc = "The joint's jointOrient."});
        t.ownAttrs.push_back({.name = "axis" + n, .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = hiddenStored,
                              .doc = "The joint's rotateAxis."});
        t.ownAttrs.push_back({.name = "rotateOrder" + n, .type = PlugType::Int, .defaultValue = int64_t(0), .flags = hiddenStored});
        t.ownAttrs.push_back(vec3Attr("restRotate" + n, "", glm::dvec3(0.0), hiddenStored, "Rest rotate, the Euler branch solutions stay near."));
        t.ownAttrs.push_back(vec3Attr("solvedRotate" + n, "", glm::dvec3(0.0), AttrOutput | AttrHidden, "Solved rotate for the joint."));
        t.ownAttrs.push_back({.name = "limitType" + n, .type = PlugType::Int, .defaultValue = int64_t(0), .flags = hiddenStored,
                              .doc = "The joint's ikLimit."});
        t.ownAttrs.push_back(vec3Attr("limitAxis" + n, "", glm::dvec3(0, 0, 1), hiddenStored, "The joint's hinge axis."));
        t.ownAttrs.push_back({.name = "limitMin" + n, .type = PlugType::Float, .defaultValue = -180.0, .flags = hiddenStored});
        t.ownAttrs.push_back({.name = "limitMax" + n, .type = PlugType::Float, .defaultValue = 180.0, .flags = hiddenStored});
        t.ownAttrs.push_back({.name = "limitCone" + n, .type = PlugType::Float, .defaultValue = 180.0, .flags = hiddenStored});
        t.ownAttrs.push_back({.name = "stiffness" + n, .type = PlugType::Float, .defaultValue = 0.0, .flags = hiddenStored});
        t.ownAttrs.push_back(vec3Attr("preferredAngle" + n, "", glm::dvec3(0.0), hiddenStored, "The joint's preferredAngle."));
    }
    t.create = [] { return std::make_unique<IkHandle>(); };
    return t;
}

std::string defaultSolver(int joints) { return joints == 3 ? kTwoBone : kFabrik; }

/** Joints from start down to end, start first; empty when end is not below start through joints. */
std::vector<NodeId> chainBetween(Graph& g, NodeId start, NodeId end)
{
    std::vector<NodeId> chain;
    for (NodeId j = end; j != kNoNode && rig::isJoint(*g.node(j)); j = g.parent(j))
    {
        chain.push_back(j);
        if (j == start)
        {
            std::reverse(chain.begin(), chain.end());
            return chain;
        }
    }
    return {};
}

/** The rest pole: the chain's average bend away from the start-to-end line, so a new handle doesn't move anything. */
glm::dvec3 restPole(Graph& g, const std::vector<NodeId>& chain)
{
    g.evaluate();
    glm::dvec3 root = translationOf(worldOf(*g.node(chain.front())));
    glm::dvec3 end = translationOf(worldOf(*g.node(chain.back())));
    if (glm::length(end - root) < 1e-12)
        return glm::dvec3(0.0);
    glm::dvec3 axis = glm::normalize(end - root);
    glm::dvec3 mid(0.0);
    for (size_t i = 1; i + 1 < chain.size(); ++i)
        mid += translationOf(worldOf(*g.node(chain[i]))) - root;
    return clean(perpendicular(mid, axis));
}
} // namespace

ik::JointLimit IkHandle::slotLimit(int i) const
{
    ik::JointLimit l;
    l.kind = ik::JointLimit::Kind(std::clamp<int64_t>(as<int64_t>(slotLimitType(i)), 0, 2));
    l.axis = as<glm::dvec3>(slotLimitAxis(i));
    l.minAngle = glm::radians(as<double>(slotLimitMin(i)));
    l.maxAngle = glm::radians(as<double>(slotLimitMax(i)));
    l.cone = glm::radians(as<double>(slotCone(i)));
    l.stiffness = std::clamp(as<double>(slotStiffness(i)), 0.0, 1.0);
    const auto order = RotateOrder(std::clamp<int64_t>(as<int64_t>(slotOrder(i)), 0, 5));
    if (const glm::dvec3 pref = as<glm::dvec3>(slotPreferred(i)); pref != glm::dvec3(0.0))
        l.preferred = glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(pref, order))));
    l.frame = glm::normalize(glm::quat_cast(glm::dmat3(as<glm::dmat4>(slotOrient(i)))));
    l.post = glm::normalize(glm::quat_cast(glm::dmat3(as<glm::dmat4>(slotAxis(i)))));
    // The child's translate is in the joint's frame after rotate and rotateAxis.
    if (const glm::dvec3 child(as<glm::dmat4>(slotLocal(i + 1))[3]); glm::length(child) > 1e-12)
        l.bone = glm::normalize(glm::dmat3(as<glm::dmat4>(slotAxis(i))) * child);
    return l;
}

void IkHandle::compute(const EvalContext& ctx)
{
    LILA_ZONE();

    Transform::compute(ctx);
    int count = int(std::clamp<int64_t>(as<int64_t>(kJointCount), 0, kMaxJoints));
    if (count < 2)
        return;

    ik::Problem p;
    p.parentWorld = as<glm::dmat4>(kChainParentMatrix);
    glm::dmat4 w = p.parentWorld;
    for (int i = 0; i < count; ++i)
        p.world.push_back(w = w * as<glm::dmat4>(slotLocal(i)));
    p.target = glm::dvec3(as<glm::dmat4>(kWorldMatrix)[3]);
    glm::dvec3 pv = as<glm::dvec3>(kPoleVector);
    if (glm::length(pv) > 1e-12)
        p.pole = translationOf(p.world[0]) + pv;
    p.twist = glm::radians(as<double>(kPoleAngle));
    if (const auto* c = dynamic_cast<const ik::CurvePoints*>(as<DataPtr>(kInCurve).get()))
        p.curve = c->points;
    p.roll = glm::radians(as<double>(kRoll));
    p.fitCurve = as<bool>(kStretch);
    p.tolerance = 1e-9;
    p.maxIterations = 256;
    p.softness = std::max(0.0, as<double>(kSoftness));
    p.maxStretch = as<bool>(kStretch) ? std::max(1.0, as<double>(kMaxStretch)) : 1.0;
    for (int i = 0; i + 1 < count; ++i)
        p.limits.push_back(slotLimit(i));

    std::string id = as<std::string>(kSolver);
    if (id.empty())
        id = defaultSolver(count);
    const ik::Solver* solver = ik::findSolver(id);
    if (!solver || count < solver->minJoints || (solver->maxJoints > 0 && count > solver->maxJoints))
        solver = ik::findSolver(kFabrik);
    if (!solver)
        return;
    // A spline chain stretches along its curve, not toward the handle.
    if (solver->name == kSpline)
        p.maxStretch = 1.0;
    const ik::Result result = ik::solve(*solver, p);
    const ik::Solution& sol = result.rotation;
    setOutput(kStretchFactor, 1.0 + (result.stretch - 1.0) * std::clamp(as<double>(kIkBlend), 0.0, 1.0));

    for (int i = 0; i + 1 < count && size_t(i) < sol.size(); ++i)
    {
        // World = parent * orient * R * axis, and the solution is parent^-1 * world, so R = orient^-1 * solution * axis^-1.
        glm::dmat3 orient(as<glm::dmat4>(slotOrient(i))), axis(as<glm::dmat4>(slotAxis(i)));
        glm::dmat3 r = glm::transpose(orient) * glm::mat3_cast(sol[size_t(i)]) * glm::transpose(axis);
        auto order = RotateOrder(std::clamp<int64_t>(as<int64_t>(slotOrder(i)), 0, 5));
        setOutput(slotSolved(i), xformops::closestEuler(r, order, as<glm::dvec3>(slotRest(i))));
    }
}

namespace ik
{
glm::dquat clampRotate(const JointLimit& l, const glm::dquat& rotate)
{
    const glm::dquat r = rotate.w < 0.0 ? -rotate : rotate;
    const glm::dvec3 v(r.x, r.y, r.z);
    switch (l.kind)
    {
    case JointLimit::Kind::Hinge:
    {
        const glm::dvec3 a = glm::length(l.axis) > 1e-12 ? glm::normalize(l.axis) : glm::dvec3(0, 0, 1);
        const double angle = 2.0 * std::atan2(glm::dot(v, a), r.w);
        return glm::angleAxis(std::clamp(angle, std::min(l.minAngle, l.maxAngle), std::max(l.minAngle, l.maxAngle)), a);
    }
    case JointLimit::Kind::Cone:
    {
        const glm::dvec3 b = glm::length(l.bone) > 1e-12 ? glm::normalize(l.bone) : glm::dvec3(1, 0, 0);
        // rotate = swing * twist, the twist about the bone: only the swing moves where the bone points.
        const double along = glm::dot(v, b);
        glm::dquat twist(1.0, 0.0, 0.0, 0.0);
        if (std::abs(along) + std::abs(r.w) > 1e-12)
            twist = glm::normalize(glm::dquat(r.w, b.x * along, b.y * along, b.z * along));
        glm::dquat swing = r * glm::conjugate(twist);
        if (swing.w < 0.0)
            swing = -swing;
        const double cone = std::max(l.cone, 0.0);
        const glm::dvec3 axis(swing.x, swing.y, swing.z);
        if (2.0 * std::acos(std::clamp(swing.w, -1.0, 1.0)) <= cone || glm::length(axis) < 1e-12)
            return rotate;
        return glm::angleAxis(cone, glm::normalize(axis)) * twist;
    }
    case JointLimit::Kind::None: break;
    }
    return rotate;
}

Result solve(const Solver& solver, const Problem& in)
{
    LILA_ZONE();

    Problem p = in;
    Result out;
    const size_t n = p.world.size();
    if (n < 2)
        return out;

    // Rebuilt from the locals: preferred angles replace the starting rotations and stretch lengthens translates.
    std::vector<glm::dmat4> local(n);
    for (size_t i = 0; i < n; ++i)
        local[i] = glm::inverse(i == 0 ? p.parentWorld : p.world[i - 1]) * p.world[i];
    bool rebuild = false;
    for (size_t i = 0; i + 1 < n && i < p.limits.size(); ++i)
        if (const JointLimit& l = p.limits[i]; l.preferred)
        {
            const glm::dmat3 m(local[i]);
            const glm::dmat3 rot = orthonormal(m);
            const glm::dmat3 turned = glm::mat3_cast(l.frame * *l.preferred * l.post) * (glm::transpose(rot) * m);
            for (int c = 0; c < 3; ++c)
                local[i][c] = glm::dvec4(turned[c], 0.0);
            rebuild = true;
        }
    auto build = [&] {
        glm::dmat4 w = p.parentWorld;
        for (size_t i = 0; i < n; ++i)
            p.world[i] = w = w * local[i];
    };
    if (rebuild)
        build();

    const glm::dvec3 root = translationOf(p.world[0]);
    double reach = 0.0;
    for (size_t i = 0; i + 1 < n; ++i)
        reach += glm::length(translationOf(p.world[i + 1]) - translationOf(p.world[i]));
    const double d = glm::length(p.target - root);
    double soft = std::min(d, reach);
    if (p.softness > 0.0 && reach > 1e-12)
    {
        const double ds = std::min(p.softness, reach), da = reach - ds;
        soft = d > da ? da + ds * (1.0 - std::exp(-(d - da) / ds)) : d;
    }
    if (p.maxStretch > 1.0 && soft > 1e-12 && d > soft)
    {
        out.stretch = std::min(d / soft, p.maxStretch);
        for (size_t i = 1; i < n; ++i)
            local[i][3] = glm::dvec4(glm::dvec3(local[i][3]) * out.stretch, 1.0);
        build();
    }
    if (p.softness > 0.0 && d > 1e-12)
        p.target = root + (p.target - root) * (std::min(d, soft * out.stretch) / d);
    p.softness = 0.0;
    p.maxStretch = 1.0;

    out.rotation = solver.solve(p);
    for (size_t i = 0; i < out.rotation.size() && i < p.limits.size(); ++i)
        if (const JointLimit& l = p.limits[i]; l.kind != JointLimit::Kind::None)
            out.rotation[i] = glm::normalize(l.frame * clampRotate(l, glm::inverse(l.frame) * out.rotation[i] * glm::inverse(l.post)) * l.post);
    return out;
}

JointLimit limitOf(Graph& g, NodeId joint)
{
    JointLimit l;
    const Node& j = *g.node(joint);
    const auto* xf = static_cast<const Transform*>(&j);
    l.kind = JointLimit::Kind(std::clamp<int64_t>(j.as<int64_t>(Joint::kLimitType), 0, 2));
    l.axis = glm::dvec3(0.0);
    l.axis[int(std::clamp<int64_t>(j.as<int64_t>(Joint::kLimitAxis), 0, 2))] = 1.0;
    l.minAngle = glm::radians(j.as<double>(Joint::kLimitMin));
    l.maxAngle = glm::radians(j.as<double>(Joint::kLimitMax));
    l.cone = glm::radians(j.as<double>(Joint::kConeAngle));
    l.stiffness = std::clamp(j.as<double>(Joint::kStiffness), 0.0, 1.0);
    if (const glm::dvec3 pref = j.as<glm::dvec3>(Joint::kPreferredAngle); pref != glm::dvec3(0.0))
        l.preferred = glm::normalize(glm::quat_cast(glm::dmat3(eulerToMatrix(pref, RotateOrder(j.as<int64_t>(Transform::kRotateOrder))))));
    l.frame = glm::normalize(glm::quat_cast(glm::dmat3(xf->preRotation())));
    l.post = glm::normalize(glm::quat_cast(glm::dmat3(xf->rotateAxisMatrix())));
    for (NodeId c : g.children(joint))
        if (rig::isJoint(*g.node(c)))
        {
            if (const glm::dvec3 t = g.node(c)->as<glm::dvec3>(Transform::kTranslate); glm::length(t) > 1e-12)
                l.bone = glm::normalize(glm::dmat3(xf->rotateAxisMatrix()) * t);
            break;
        }
    return l;
}

std::vector<NodeId> chainOf(Graph& g, NodeId handle)
{
    Node* h = g.node(handle);
    if (!h || !h->type().isA("ikHandle"))
        return {};
    int count = int(std::clamp<int64_t>(h->as<int64_t>(IkHandle::kJointCount), 0, IkHandle::kMaxJoints));
    std::vector<NodeId> chain;
    for (int i = 0; i + 1 < count; ++i)
    {
        std::vector<PlugRef> dst = g.destinations({handle, IkHandle::slotSolved(i), -1});
        if (dst.empty())
            return {};
        chain.push_back(dst.front().node);
    }
    if (chain.empty())
        return {};
    // The effector is stored by name and told apart from same-named nodes by being the chain's last child.
    Node* end = nullptr;
    for (NodeId id : g.findAll(h->as<std::string>(IkHandle::kEndEffector)))
        if (g.parent(id) == chain.back())
            end = g.node(id);
    if (!end)
    {
        // Renamed: the child whose offset matches the stored rest offset.
        glm::dvec3 want = translationOf(h->as<glm::dmat4>(IkHandle::slotLocal(count - 1)));
        end = nullptr;
        double best = 1e300;
        for (NodeId c : g.children(chain.back()))
            if (rig::isJoint(*g.node(c)))
                if (double d = glm::length(g.node(c)->as<glm::dvec3>(Transform::kTranslate) - want); d < best)
                    best = d, end = g.node(c);
    }
    if (!end)
        return {};
    chain.push_back(end->id());
    return chain;
}

std::vector<NodeId> handlesOf(Graph& g, NodeId joint)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
        if (g.node(id)->type().isA("ikHandle"))
        {
            auto chain = chainOf(g, id);
            if (std::find(chain.begin(), chain.end(), joint) != chain.end())
                out.push_back(id);
        }
    return out;
}

void refresh(Graph& g, NodeId handle)
{
    std::vector<NodeId> chain = chainOf(g, handle);
    if (chain.size() < 2)
        return;
    g.setValue({handle, IkHandle::kJointCount}, int64_t(chain.size()));
    g.setValue({handle, IkHandle::kEndEffector}, g.node(chain.back())->name());
    for (size_t i = 0; i < chain.size(); ++i)
    {
        Node* j = g.node(chain[i]);
        const auto* xf = static_cast<const Transform*>(j);
        int k = int(i);
        // FK only: whatever IK currently blends in must not feed back into the rest layout.
        auto order = RotateOrder(j->as<int64_t>(Transform::kRotateOrder));
        glm::dmat4 local = Transform::localMatrix(*j, eulerToMatrix(j->as<glm::dvec3>(Transform::kRotate), order));
        g.setValue({handle, IkHandle::slotLocal(k)}, local);
        g.setValue({handle, IkHandle::slotOrient(k)}, xf->preRotation());
        g.setValue({handle, IkHandle::slotAxis(k)}, xf->rotateAxisMatrix());
        g.setValue({handle, IkHandle::slotOrder(k)}, j->as<int64_t>(Transform::kRotateOrder));
        g.setValue({handle, IkHandle::slotRest(k)}, j->as<glm::dvec3>(Transform::kRotate));
        glm::dvec3 axis(0.0);
        axis[int(std::clamp<int64_t>(j->as<int64_t>(Joint::kLimitAxis), 0, 2))] = 1.0;
        g.setValue({handle, IkHandle::slotLimitType(k)}, j->as<int64_t>(Joint::kLimitType));
        g.setValue({handle, IkHandle::slotLimitAxis(k)}, axis);
        g.setValue({handle, IkHandle::slotLimitMin(k)}, j->as<double>(Joint::kLimitMin));
        g.setValue({handle, IkHandle::slotLimitMax(k)}, j->as<double>(Joint::kLimitMax));
        g.setValue({handle, IkHandle::slotCone(k)}, j->as<double>(Joint::kConeAngle));
        g.setValue({handle, IkHandle::slotStiffness(k)}, j->as<double>(Joint::kStiffness));
        g.setValue({handle, IkHandle::slotPreferred(k)}, j->as<glm::dvec3>(Joint::kPreferredAngle));
    }
}
} // namespace ik

namespace
{
Node& jointArg(Graph& g, const std::string& name)
{
    Node& n = nodeNamed(name, "ikHandle");
    if (!rig::isJoint(n))
        throw CommandError("ikHandle: " + name + " is not a joint");
    (void)g;
    return n;
}

json createHandle(const Args& a)
{
    Graph& g = graph();
    std::string startName = a.get<std::string>("startJoint"), endName = a.get<std::string>("endEffector");
    if (startName.empty() || endName.empty())
    {
        std::vector<std::string> joints;
        for (NodeId id : g.selection())
            if (rig::isJoint(*g.node(id)))
                joints.push_back(g.node(id)->path());
        if (joints.size() < 2)
            throw CommandError("ikHandle: give startJoint and endEffector, or select the start and end joints");
        startName = startName.empty() ? joints.front() : startName;
        endName = endName.empty() ? joints.back() : endName;
    }
    NodeId start = jointArg(g, startName).id(), end = jointArg(g, endName).id();
    std::vector<NodeId> chain = chainBetween(g, start, end);
    if (chain.size() < 2)
        throw CommandError("ikHandle: " + endName + " is not a joint below " + startName);
    if (chain.size() > size_t(IkHandle::kMaxJoints))
        throw CommandError("ikHandle: chains are limited to " + std::to_string(IkHandle::kMaxJoints) + " joints");
    for (size_t i = 0; i + 1 < chain.size(); ++i)
        if (g.source({chain[i], Joint::kSolvedRotate, -1}))
            throw CommandError("ikHandle: " + g.node(chain[i])->path() + " is already solved by another handle");

    std::string solver = a.get<std::string>("solver");
    if (!solver.empty())
    {
        const ik::Solver* s = ik::findSolver(solver);
        if (!s)
            throw CommandError("ikHandle: no IK solver named '" + solver + "'");
        int n = int(chain.size());
        if (n < s->minJoints || (s->maxJoints > 0 && n > s->maxJoints))
            throw CommandError("ikHandle: " + solver + " does not take a chain of " + std::to_string(n) + " joints");
    }

    g.evaluate();
    glm::dvec3 target = translationOf(worldOf(*g.node(end)));
    Node* h = guarded([&] { return g.createNode(*types::find("ikHandle"), a.get<std::string>("name", "ikHandle"), kNoNode); });
    NodeId id = h->id();
    g.setValue({id, Transform::kTranslate}, target);
    g.setValue({id, IkHandle::kSolver}, solver);
    g.setValue({id, IkHandle::kPoleVector}, a.has("poleVector") ? vec3Of(a.flags["poleVector"]) : restPole(g, chain));
    g.setValue({id, IkHandle::kPoleAngle}, a.get<double>("poleAngle", 0.0));
    g.setValue({id, IkHandle::kJointCount}, int64_t(chain.size()));
    g.setValue({id, IkHandle::kEndEffector}, g.node(end)->name());
    if (NodeId parent = g.parent(start); parent != kNoNode)
        g.connect({parent, Transform::kWorldMatrix, -1}, {id, IkHandle::kChainParentMatrix, -1});
    for (size_t i = 0; i + 1 < chain.size(); ++i)
    {
        g.connect({id, IkHandle::slotSolved(int(i)), -1}, {chain[i], Joint::kSolvedRotate, -1});
        g.connect({id, IkHandle::kIkBlend, -1}, {chain[i], Joint::kIkBlend, -1});
    }
    for (size_t i = 1; i < chain.size(); ++i)
        if (!g.source({chain[i], Joint::kIkStretch, -1}))
            g.connect({id, IkHandle::kStretchFactor, -1}, {chain[i], Joint::kIkStretch, -1});
    ik::refresh(g, id);
    g.select({id});
    return g.node(id)->path();
}

json limitCmd(const Args& a)
{
    Graph& g = graph();
    std::vector<NodeId> joints;
    for (const std::string& name : a.objects())
        joints.push_back(jointArg(g, name).id());
    if (joints.empty())
        for (NodeId id : g.selection())
            if (rig::isJoint(*g.node(id)))
                joints.push_back(id);
    if (joints.empty())
        throw CommandError("ikLimit: give or select joints");
    auto pick = [&](const char* flag, std::initializer_list<const char*> names) -> std::optional<int64_t> {
        if (!a.has(flag))
            return std::nullopt;
        const std::string v = a.get<std::string>(flag);
        int64_t i = 0;
        for (const char* n : names)
        {
            if (v == n)
                return i;
            ++i;
        }
        throw CommandError("ikLimit: unknown " + std::string(flag) + " '" + v + "'");
    };
    const std::optional<int64_t> type = pick("type", {"none", "hinge", "cone"});
    const std::optional<int64_t> axis = pick("axis", {"x", "y", "z"});
    json out = json::array();
    for (NodeId j : joints)
    {
        if (type)
            g.setValue({j, Joint::kLimitType}, *type);
        if (axis)
            g.setValue({j, Joint::kLimitAxis}, *axis);
        for (auto [flag, attr] : {std::pair{"min", int(Joint::kLimitMin)}, std::pair{"max", int(Joint::kLimitMax)},
                                  std::pair{"cone", int(Joint::kConeAngle)}, std::pair{"stiffness", int(Joint::kStiffness)}})
            if (a.has(flag))
                g.setValue({j, attr}, a.get<double>(flag));
        if (a.has("preferredAngle"))
            g.setValue({j, Joint::kPreferredAngle}, vec3Of(a.flags["preferredAngle"]));
        out.push_back(g.node(j)->path());
    }
    std::set<NodeId> handles;
    for (NodeId j : joints)
        for (NodeId h : ik::handlesOf(g, j))
            handles.insert(h);
    for (NodeId h : handles)
        ik::refresh(g, h);
    return out;
}

void registerIkCommandsImpl()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "ikLimit",
        .doc = "Set how far IK may turn joints (default the selection): a hinge about one axis within min..max degrees, or a "
               "cone the bone swings within; plus stiffness and the preferred angle. Only the flags given change. The IK "
               "handles solving the joints pick the limits up at once. Returns the joints.",
        .positionalDoc = "joints",
        .flags = {
            {"type", "t", T::String, nullptr, "none, hinge or cone.", {"none", "hinge", "cone"}},
            {"axis", "ax", T::String, nullptr, "The hinge's axis in rotate space: x, y or z.", {"x", "y", "z"}},
            {"min", "mn", T::Float, nullptr, "Smallest hinge rotate, degrees."},
            {"max", "mx", T::Float, nullptr, "Largest hinge rotate, degrees."},
            {"cone", "cn", T::Float, nullptr, "How far the bone may swing from rotate zero, degrees."},
            {"stiffness", "st", T::Float, nullptr, "0 turns freely, toward 1 the joint resists turning.", {}, {}, 0.0, 1.0},
            {"preferredAngle", "pa", T::Vec3, nullptr, "Rotate IK starts from, degrees; picks the bend of a straight chain."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            return limitCmd(a);
        },
    });
    ops::registerCommand({
        .name = "ikHandle",
        .doc = "Create an IK handle that solves the chain from startJoint down to endEffector (or the first and last "
               "selected joints); the handle starts on the end joint, so nothing moves. The joints keep their keyable "
               "rotate as FK and the handle's ikBlend crossfades to the solution. With refresh=True, re-reads the rest "
               "layout of the given handles' chains after joints were edited. Returns the handle.",
        .positionalDoc = "handles (refresh only)",
        .flags = {
            nodeFlag({"startJoint", "sj", T::String, nullptr, "First joint of the chain."}, "joint"),
            nodeFlag({"endEffector", "ee", T::String, nullptr, "Last joint of the chain; the one placed on the handle."}, "joint"),
            {"solver", "sol", T::String, "", "IK solver id; empty picks ik2BoneSolver for three joints, else ikFabrikSolver.",
             {"", kTwoBone, kFabrik}},
            {"name", "n", T::String, nullptr, "Handle name."},
            {"poleVector", "pv", T::Vec3, nullptr, "Bend direction from the start joint; defaults to the chain's current bend."},
            {"poleAngle", "pa", T::Float, nullptr, "Roll of the bend plane, degrees."},
            {"refresh", "rf", T::Bool, false, "Re-read the rest layout of the given handles' chains."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            if (!a.get<bool>("refresh"))
                return createHandle(a);
            Graph& g = graph();
            json out = json::array();
            for (const std::string& name : a.objects())
            {
                Node& h = nodeNamed(name, "ikHandle");
                if (!h.type().isA("ikHandle"))
                    throw CommandError("ikHandle: " + name + " is not an ikHandle");
                ik::refresh(g, h.id());
                out.push_back(name);
            }
            return out;
        },
    });
}
} // namespace

void registerIkCommands()
{
    if (ops::find("ikHandle"))
        return;
    registerIkCommandsImpl();
}

void registerIkTypes()
{
    if (!ik::findSolver(kTwoBone))
        ik::registerSolver({.name = kTwoBone, .doc = "Exact two-bone solve for limbs (three joints), bending toward the pole vector.",
                            .minJoints = 3, .maxJoints = 3, .solve = solveTwoBone});
    if (!ik::findSolver(kSpline))
        ik::registerSolver({.name = kSpline,
                            .doc = "Lays the chain along the handle's inCurve, bones keeping their length (or stretched over the curve), rolled "
                                   "by roll and twisted toward the end by poleAngle.",
                            .minJoints = 2, .solve = solveSpline});
    if (!ik::findSolver(kFabrik))
        ik::registerSolver({.name = kFabrik, .doc = "Iterative FABRIK solve for chains of any length; the bend swings toward the pole vector.",
                            .minJoints = 2, .solve = solveFabrik});
    if (types::find("ikHandle"))
        return;
    const NodeType* transform = types::find("transform");
    if (!transform)
        throw std::logic_error("registerIkTypes needs the built-in types first");
    types::registerType(makeHandleType(*transform));
}
} // namespace lila::scene
