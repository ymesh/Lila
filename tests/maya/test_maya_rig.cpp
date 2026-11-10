#include <doctest/doctest.h>

#include "deform/Deform.h"
#include "fbx/Fbx.h"
#include "fbx/FbxAxes.h"
#include "geo/Mesh.h"
#include "io/References.h"
#include "maya/Maya.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MathNodes.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <map>

/*
 * Every expected value here comes from the rig's own definition: the values the .ma file stores, the maths of each node
 * written out by hand, or a property the rig must hold (an IK end on its target, a constraint's offset kept). Nothing is
 * recorded from Maya.
 */

using namespace lila;
using namespace lila::scene;
namespace fs = std::filesystem;

namespace
{
fs::path rigFixture(const std::string& name) { return fs::path(LILA_MAYA_TEST_DATA) / "rig" / name; }

maya::ImportResult importRig(const std::string& name, const std::string& upAxis = "auto")
{
    io::refs::clear();
    Graph& g = Graph::current();
    g.clear();
    maya::ImportResult r = maya::importMaya(g, rigFixture(name + ".ma"), {.upAxis = upAxis});
    if (!r.ok)
        MESSAGE(r.error);
    REQUIRE(r.ok);
    return r;
}

/** The Maya to Lila basis a file read as Z up gets (identity for Y up), as the importer builds it. */
struct Axes
{
    glm::dmat3 c3{1.0};
    fbx::AxisMap map;

    static Axes zUp()
    {
        Axes a;
        a.c3 = glm::transpose(fbx::profileBasis(fbx::AxisProfile::Ue5, math::upAxis()));
        a.map = fbx::axisMap(a.c3);
        return a;
    }
};

/** A Maya translate, rotate or scale channel as the Lila plug it lands on and the factor its value takes. */
std::pair<std::string, double> lilaChannel(const std::string& plug, const Axes& ax)
{
    static const char* kGroups[3] = {"translate", "rotate", "scale"};
    const size_t dot = plug.find('.');
    const std::string node = plug.substr(0, dot), attr = plug.substr(dot + 1);
    for (int g = 0; g < 3; ++g)
        for (int a = 0; a < 3; ++a)
            if (attr == std::string(kGroups[g]) + char('X' + a))
                return {node + "." + kGroups[g] + char('X' + ax.map.to[a]), ax.map.factor(g, a)};
    return {plug, 1.0};
}

// ---------------------------------------------------------------------------------------------------- Maya-space maths

glm::dmat4 T(const glm::dvec3& t) { return glm::translate(glm::dmat4(1.0), t); }
glm::dmat4 S(const glm::dvec3& s) { return glm::scale(glm::dmat4(1.0), s); }

/** Maya's Euler rotation in degrees; order 0..5 is xyz, yzx, zxy, xzy, yxz, zyx, its first axis applied first. */
glm::dmat4 R(const glm::dvec3& deg, int order = 0)
{
    static const int kSeq[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {1, 0, 2}, {2, 1, 0}};
    glm::dmat4 m(1.0);
    for (int i : kSeq[order])
    {
        glm::dvec3 axis(0.0);
        axis[i] = 1.0;
        m = glm::rotate(glm::dmat4(1.0), glm::radians(deg[i]), axis) * m;
    }
    return m;
}

glm::dvec3 pos(const glm::dmat4& m) { return glm::dvec3(m[3]); }
glm::dvec3 xform(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }
glm::dmat3 rot(const glm::dmat4& m)
{
    glm::dmat3 r(m);
    for (int c = 0; c < 3; ++c)
        r[c] = glm::normalize(r[c]);
    return r;
}
glm::dquat quat(const glm::dmat4& m) { return glm::normalize(glm::quat_cast(rot(m))); }
glm::dmat4 frame(const glm::dmat3& r, const glm::dvec3& t)
{
    glm::dmat4 m(r);
    m[3] = glm::dvec4(t, 1.0);
    return m;
}

/** Weighted quaternion average, each aligned to the first's hemisphere. */
glm::dquat average(const std::vector<std::pair<glm::dquat, double>>& qs)
{
    glm::dquat sum(0.0, 0.0, 0.0, 0.0);
    for (const auto& [q, w] : qs)
        sum += (glm::dot(q, qs.front().first) < 0.0 ? -q : q) * w;
    return glm::normalize(sum);
}

double matrixError(const glm::dmat4& a, const glm::dmat4& b)
{
    double e = 0.0;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            e = std::max(e, std::abs(a[c][r] - b[c][r]));
    return e;
}

std::string str(const glm::dmat4& m)
{
    std::string s;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            s += std::format("{:.4f} ", m[c][r]);
    return s;
}

std::string str(const glm::dvec3& v) { return std::format("({:.4f} {:.4f} {:.4f})", v.x, v.y, v.z); }

using Pose = std::vector<std::pair<std::string, double>>;

/**
 * An imported fixture, driven and read in Maya's space and units whatever axis it was read as. Drivers go back to the
 * values the file stores before each pose sets its own.
 */
struct Rig
{
    Axes ax;
    glm::dmat4 c4{1.0};
    std::map<std::string, double> rest;
    maya::ImportResult result;

    Rig(const std::string& name, bool zUp, std::initializer_list<std::string> drivers)
    {
        result = importRig(name, zUp ? "z" : "auto");
        if (zUp)
            ax = Axes::zUp();
        c4 = glm::dmat4(ax.c3);
        Graph::current().evaluate();
        for (const std::string& d : drivers)
            rest[d] = get(d);
    }

    void pose(const Pose& p)
    {
        for (const auto& [plug, v] : rest)
            set(plug, v);
        for (const auto& [plug, v] : p)
            set(plug, v);
        Graph::current().evaluate();
    }

    void set(const std::string& mayaPlug, double value) const
    {
        Graph& g = Graph::current();
        auto [plug, factor] = lilaChannel(mayaPlug, ax);
        PlugRef p = g.plug(plug);
        const AttrDef& def = g.attr(p);
        value *= factor;
        if (p.component >= 0 || def.type == PlugType::Float)
            g.setValue(p, value);
        else if (def.type == PlugType::Bool)
            g.setValue(p, value != 0.0);
        else
            g.setValue(p, int64_t(std::llround(value)));
    }

    /** A plug's current value in Maya's units. */
    double get(const std::string& mayaPlug) const
    {
        Graph& g = Graph::current();
        auto [plug, factor] = lilaChannel(mayaPlug, ax);
        const PlugValue v = g.value(g.plug(plug));
        double d = 0.0;
        if (const double* f = std::get_if<double>(&v))
            d = *f;
        else if (const int64_t* i = std::get_if<int64_t>(&v))
            d = double(*i);
        else if (const bool* b = std::get_if<bool>(&v))
            d = *b ? 1.0 : 0.0;
        else
            FAIL("not a number: " << mayaPlug);
        return d / factor;
    }

    glm::dvec3 vec(const std::string& node, const std::string& group) const
    {
        return {get(node + "." + group + "X"), get(node + "." + group + "Y"), get(node + "." + group + "Z")};
    }

    /** A plain transform's local matrix from its own channels (no pivots), for drivers and nodes nothing drives. */
    glm::dmat4 trs(const std::string& node, int order = 0) const
    {
        return T(vec(node, "translate")) * R(vec(node, "rotate"), order) * S(vec(node, "scale"));
    }

    Node* node(const std::string& name) const
    {
        Node* n = Graph::current().find(name);
        INFO(name);
        REQUIRE(n);
        return n;
    }

    glm::dmat4 world(const std::string& name) const
    {
        const glm::dmat4 m = std::get<glm::dmat4>(Graph::current().value({node(name)->id(), Transform::kWorldMatrix, -1}));
        return glm::transpose(c4) * m * c4;
    }

    std::vector<glm::dvec3> pointsOf(const std::string& mesh, bool deformed) const
    {
        Graph& g = Graph::current();
        Node* n = node(mesh);
        PlugRef plug{n->id(), MeshNode::kOutMesh, -1};
        if (const std::vector<NodeId> stack = deformers::list(g, n->id()); !deformed && !stack.empty())
            plug = {stack.front(), DeformerNode::kInMesh, -1};
        MeshPtr m = std::get<MeshPtr>(g.value(plug));
        REQUIRE(m);
        const glm::dmat4 w = world(mesh);
        std::vector<glm::dvec3> out(size_t(m->pointCapacity()));
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = xform(w, glm::transpose(ax.c3) * m->position(geo::Index(i)));
        return out;
    }

    /** The mesh's deformed points, world space. */
    std::vector<glm::dvec3> points(const std::string& mesh) const { return pointsOf(mesh, true); }
    /** The mesh's points before its deformers, world space. */
    std::vector<glm::dvec3> basePoints(const std::string& mesh) const { return pointsOf(mesh, false); }

    void expect(const std::string& name, const glm::dmat4& want, double tol = 1e-4) const
    {
        const glm::dmat4 got = world(name);
        INFO(name);
        INFO("got  " << str(got));
        INFO("want " << str(want));
        CHECK(matrixError(got, want) < tol);
    }

    void expectAt(const std::string& name, const glm::dvec3& want, double tol = 1e-4) const
    {
        const glm::dvec3 got = pos(world(name));
        INFO(name << " at " << str(got) << ", want " << str(want));
        CHECK(glm::length(got - want) < tol);
    }

    void expectPoints(const std::string& mesh, const std::vector<glm::dvec3>& want, double tol = 1e-4) const
    {
        const std::vector<glm::dvec3> got = points(mesh);
        REQUIRE(got.size() == want.size());
        double worst = 0.0;
        size_t at = 0;
        for (size_t i = 0; i < got.size(); ++i)
            if (const double d = glm::length(got[i] - want[i]); d > worst)
                worst = d, at = i;
        INFO(mesh << ": worst point " << at << " off by " << worst << ", got " << str(got[at]) << " want " << str(want[at]));
        CHECK(worst < tol);
    }

    void dump() const
    {
        for (const std::string& s : result.skipped)
            MESSAGE("skipped: " << s);
        for (const std::string& s : result.warnings)
            MESSAGE("warning: " << s);
    }
};

bool mentions(const std::vector<std::string>& lines, std::string_view what)
{
    for (const std::string& s : lines)
        if (s.find(what) != std::string::npos)
            return true;
    return false;
}

// ---------------------------------------------------------------------------------------------------- utility nodes

glm::dvec3 setRange(const glm::dvec3& v, const glm::dvec3& oldMin, const glm::dvec3& oldMax, const glm::dvec3& lo, const glm::dvec3& hi)
{
    glm::dvec3 out;
    for (int k = 0; k < 3; ++k)
        out[k] = lo[k] + std::clamp((v[k] - oldMin[k]) / (oldMax[k] - oldMin[k]), 0.0, 1.0) * (hi[k] - lo[k]);
    return out;
}

/** The closest point on the surface of a box (half extents half) placed by m. */
glm::dvec3 closestOnBox(const glm::dmat4& m, const glm::dvec3& half, const glm::dvec3& p)
{
    const glm::dvec3 l = xform(glm::inverse(m), p);
    glm::dvec3 c = glm::clamp(l, -half, half);
    if (c == l)
    {
        int k = 0;
        for (int a = 1; a < 3; ++a)
            if (half[a] - std::abs(l[a]) < half[k] - std::abs(l[k]))
                k = a;
        c[k] = l[k] < 0.0 ? -half[k] : half[k];
    }
    return xform(m, c);
}

void checkUtility(bool zUp)
{
    Rig rig("rig_utility", zUp,
            {"ctl.translateX", "ctl.translateY", "ctl.translateZ", "ctl.rotateX", "ctl.rotateY", "ctl.rotateZ", "ctl.a", "ctl.b", "ctl.ang", "ctl.sel"});
    rig.dump();
    CHECK(rig.result.skipped.empty());
    const std::vector<Pose> poses = {
        {},
        {{"ctl.translateX", 1.5}, {"ctl.translateY", -3}, {"ctl.a", 0.75}, {"ctl.b", 0.2}, {"ctl.sel", 1}, {"ctl.ang", -40}},
        {{"ctl.translateX", -0.4}, {"ctl.translateZ", 7}, {"ctl.rotateX", -60}, {"ctl.rotateY", 95}, {"ctl.a", 2.5}, {"ctl.b", 0.9}},
    };
    for (size_t i = 0; i < poses.size(); ++i)
    {
        INFO("pose " << i);
        rig.pose(poses[i]);
        const glm::dvec3 t = rig.vec("ctl", "translate"), r = rig.vec("ctl", "rotate");
        const double a = rig.get("ctl.a"), b = rig.get("ctl.b"), ang = rig.get("ctl.ang"), sel = rig.get("ctl.sel");
        const glm::dmat4 ctlW = T(t) * R(r);
        auto out = [&](int n, const glm::dvec3& tt, const glm::dvec3& rr = glm::dvec3(0.0), const glm::dvec3& ss = glm::dvec3(1.0)) {
            rig.expect(std::format("out{}", n), T(tt) * R(rr) * S(ss));
        };

        out(1, t * glm::dvec3(2, 3, -1));
        out(2, {a / 4.0, b * b, 0});
        out(3, t + glm::dvec3(1) + glm::dvec3(0, t.x, 0), {}, {a - b - 0.125, (t.x + 3.0) / 2.0, 2.5});
        out(4, a > 0.5 ? t : glm::dvec3(-1, -2, -3));
        out(5, glm::dvec3(1) - t);
        out(6, glm::clamp(t, glm::dvec3(-1), glm::dvec3(1, 2, 3)));
        out(7, setRange(t, {-2, -2, 0}, {2, 4, 6}, {0, 10, -1}, {1, 20, 1}));
        {
            // The ramp runs linearly through (0, 0), (0.5, 1) and (1, 0.2) over the input range 0..2, onto -1..3.
            const double s = std::clamp(t.x / 2.0, 0.0, 1.0);
            const double v = s < 0.5 ? s * 2.0 : 1.0 + (s - 0.5) * 2.0 * (0.2 - 1.0);
            out(8, {-1.0 + v * 4.0, 0, 0});
        }
        out(9, t * b + glm::dvec3(5, 6, 7) * (1.0 - b));
        out(10, {t.x * (1.0 - b) + t.y * b, 0, 0});
        out(11, {glm::length(t - glm::dvec3(-2, 2, 4)), 0, 0});
        {
            const glm::dquat q = glm::slerp(quat(R(r)), quat(R({30, 0, 45})), b);
            rig.expect("out12", T(glm::mix(t, glm::dvec3(4, 5, 6), b)) * glm::dmat4(glm::mat3_cast(q)));
        }
        out(13, {sel == 0.0 ? t.x : t.y, 0, 0});
        out(14, glm::cross(t, glm::dvec3(0, 1, 0)));
        out(15, xform(ctlW, {1, 0, 0}));
        // Angles reach doubles in degrees through the file's unit conversions, and come back as degrees.
        out(16, {r.x, 0, 0}, {0, 0, 2.0 * ang});
        out(17, {}, {0, r.x + r.y, 0});
        out(18, {}, {90.0 * b, 0, 0});
        // closestPointOnMesh's inputMatrix is left at identity, so it works on the box's own points, not where it is placed.
        out(19, closestOnBox(glm::dmat4(1.0), {1.5, 1, 2}, t));
    }
}

// ---------------------------------------------------------------------------------------------------- matrix nodes

struct Decomposed
{
    glm::dvec3 t, s;
    glm::dquat q;
};

Decomposed decompose(const glm::dmat4& m)
{
    Decomposed d{.t = pos(m)};
    for (int c = 0; c < 3; ++c)
        d.s[c] = glm::length(glm::dvec3(m[c]));
    d.q = quat(m);
    return d;
}

glm::dmat4 compose(const Decomposed& d) { return T(d.t) * glm::dmat4(glm::mat3_cast(d.q)) * S(d.s); }

void checkMatrix(bool zUp)
{
    Rig rig("rig_matrix", zUp,
            {"ctl.translateX", "ctl.translateY", "ctl.translateZ", "ctl.rotateX", "ctl.rotateY", "ctl.rotateZ", "ctl.scaleY", "ctl.b", "ctl.sel",
             "ctlB.translateX", "ctlB.rotateZ", "holder.rotateY"});
    rig.dump();
    CHECK(rig.result.skipped.empty());
    const std::vector<Pose> poses = {
        {},
        {{"ctl.translateX", -2}, {"ctl.rotateY", -70}, {"ctl.scaleY", 1.5}, {"ctl.b", 0.8}, {"ctl.sel", 1}, {"ctlB.translateX", 2}},
        {{"ctl.translateZ", -4}, {"ctl.rotateX", 120}, {"ctl.b", 0.0}, {"ctlB.rotateZ", 60}, {"holder.rotateY", 45}},
    };
    for (size_t i = 0; i < poses.size(); ++i)
    {
        INFO("pose " << i);
        rig.pose(poses[i]);
        const glm::dmat4 ctl = rig.trs("ctl"), ctlB = rig.trs("ctlB"), base = rig.trs("base");
        const double b = rig.get("ctl.b");

        rig.expect("follow1", ctl * T({1, 2, 3}));
        // Decomposed into its rotate order and recomposed, the matrix comes back.
        rig.expect("follow2", ctl);
        rig.expect("follow3", glm::inverse(T(rig.vec("ctl", "translate")) * R(rig.vec("ctl", "rotate"), 3) * S({1, 2, 1})));
        {
            // Each target in turn: translate and scale blend linearly, rotation by slerp, each by its weight.
            Decomposed d = decompose(ctl);
            const Decomposed tb = decompose(ctlB), tbase = decompose(base);
            d.t = glm::mix(d.t, tb.t, b);
            d.q = glm::slerp(d.q, tb.q, b);
            d.s = glm::mix(d.s, tb.s, b);
            d.t = glm::mix(d.t, tbase.t, 0.5);
            d.s = glm::mix(d.s, tbase.s, 0.5);
            rig.expect("follow4", compose(d));
        }
        rig.expect("follow5", T(pos(ctl)));
        {
            // Z aims at ctlB from base; Y lines up with ctl's Y as near as Z allows.
            const glm::dvec3 z = glm::normalize(pos(ctlB) - pos(base));
            const glm::dvec3 up = glm::normalize(glm::dvec3(ctl[1]));
            const glm::dvec3 y = glm::normalize(up - glm::dot(up, z) * z);
            rig.expect("follow6", frame(glm::dmat3(glm::cross(y, z), y, z), pos(base)));
        }
        rig.expect("follow7", ctl * 0.25 + ctlB * 0.75);
        {
            glm::dmat4 m(1.0);
            m[0][0] = b;
            m[3][0] = rig.get("ctl.translateX");
            m[3][1] = rig.get("ctl.translateY");
            rig.expect("follow8", m);
        }
        rig.expect("follow9", rig.get("ctl.sel") == 0.0 ? ctl : ctlB);
        // ctl's world over the holder's inverse as the offset parent matrix: the holder cancels out.
        rig.expect("follow10", ctl * T({0, 1, 0}));
    }
}

// ---------------------------------------------------------------------------------------------------- constraints

void checkConstraints(bool zUp)
{
    Rig rig("rig_constraints", zUp,
            {"tgtA.translateX", "tgtA.translateY", "tgtA.translateZ", "tgtA.rotateX", "tgtA.rotateY", "tgtA.rotateZ", "tgtA.scaleX",
             "tgtB.translateX", "tgtB.translateZ", "tgtB.rotateY", "tgtB.scaleZ", "upObj.rotateX", "grp6.translateX", "grp6.rotateY", "ctl.blend"});
    rig.dump();
    CHECK(rig.result.skipped.empty());
    const std::vector<Pose> poses = {
        {},
        {{"tgtA.translateX", 4}, {"tgtA.rotateY", 80}, {"tgtB.translateZ", -3}, {"upObj.rotateX", 30}, {"grp6.rotateY", -40}},
        {{"tgtA.rotateX", -50}, {"tgtA.scaleX", 2}, {"tgtB.rotateY", 120}, {"tgtB.scaleZ", 0.5}, {"grp6.translateX", 3}, {"ctl.blend", 0.5}},
        {{"tgtA.translateY", -2}, {"tgtB.translateX", 6}, {"ctl.blend", 1.0}},
    };
    rig.pose({});
    const glm::dmat4 a0 = rig.trs("tgtA"), b0 = rig.trs("tgtB"), g60 = rig.trs("grp6");
    // Where the .ma places each constrained object at rest, which the maintained offsets keep.
    const glm::dvec3 pivot1(0, 0, 1);
    const glm::dmat4 obj1Rest = T({0, 1, 0}), jointCRest = T({2, 0, 0}) * R({0, 0, 45}), obj6Rest = g60 * T({1, 1, 1});

    for (size_t i = 0; i < poses.size(); ++i)
    {
        INFO("pose " << i);
        rig.pose(poses[i]);
        const glm::dmat4 a = rig.trs("tgtA"), b = rig.trs("tgtB"), up = rig.trs("upObj");
        const glm::dmat3 ra = rot(a) * glm::transpose(rot(a0)), rb = rot(b) * glm::transpose(rot(b0));

        {
            // The set driven key takes tgtA's weight from 1 to 0 as blend goes 0 to 1; tgtB weighs 0.25. Each target carries
            // the rotate pivot and the rotation from rest; the pivots average by weight, the rotations by weighted quaternion.
            const double blend = rig.get("ctl.blend");
            const double wa = 1.0 - blend, wb = 0.25;
            const glm::dvec3 pa = xform(a * glm::inverse(a0) * obj1Rest, pivot1), pb = xform(b * glm::inverse(b0) * obj1Rest, pivot1);
            const glm::dvec3 p = (pa * wa + pb * wb) / (wa + wb);
            const glm::dquat q = wa > 0.0 ? average({{glm::quat_cast(ra), wa}, {glm::quat_cast(rb), wb}}) : glm::quat_cast(rb);
            rig.expect("obj1", T(p) * glm::dmat4(glm::mat3_cast(q)) * T(-pivot1));
        }
        // Point constraint with an offset, Y skipped.
        rig.expect("obj2", T({pos(a).x + 1.0, 5.0, pos(a).z}));
        {
            // One offset for the blended orientation, in the joint's own space.
            const glm::dquat blend0 = average({{quat(a0), 1.0}, {quat(b0), 1.0}});
            const glm::dquat blend = average({{quat(a), 1.0}, {quat(b), 1.0}});
            const glm::dquat offset = glm::inverse(blend0) * quat(jointCRest);
            rig.expect("jointC", T({2, 0, 0}) * glm::dmat4(glm::mat3_cast(blend * offset)));
        }
        {
            // Y aims at tgtB, X toward upObj's Z, then the 10 degree offset about X in the object's own space.
            const glm::dvec3 at(3, 3, -2);
            const glm::dvec3 y = glm::normalize(pos(b) - at);
            const glm::dvec3 u = glm::normalize(rot(up) * glm::dvec3(0, 0, 1));
            const glm::dvec3 x = glm::normalize(u - glm::dot(u, y) * y);
            rig.expect("obj4", frame(glm::dmat3(x, y, glm::cross(x, y)), at) * R({10, 0, 0}));
        }
        // The scales blend multiplicatively: equal weights give the geometric mean.
        rig.expect("obj5", S(glm::sqrt(rig.vec("tgtA", "scale") * rig.vec("tgtB", "scale"))));
        // One target, its offset held while the group above moves.
        rig.expect("obj6", frame(ra * rot(obj6Rest), xform(a * glm::inverse(a0), pos(obj6Rest))));
        // No offset kept, a 90 degree offset about Y in the object's own space.
        rig.expect("obj7", frame(rot(a) * glm::dmat3(R({0, 90, 0})), {0, 4, 0}));
    }
}

// ---------------------------------------------------------------------------------------------------- IK

/** A two bone chain solved to target: bones keep their lengths, the end reaches the target or the chain points at it straight. */
void checkLimb(const Rig& rig, const std::array<std::string, 3>& joints, const glm::dvec3& target, double l1, double l2, double tol = 1e-3)
{
    const glm::dvec3 root = pos(rig.world(joints[0])), mid = pos(rig.world(joints[1])), end = pos(rig.world(joints[2]));
    INFO(joints[0] << " chain to " << str(target));
    CHECK(glm::length(mid - root) == doctest::Approx(l1).epsilon(1e-6));
    CHECK(glm::length(end - mid) == doctest::Approx(l2).epsilon(1e-6));
    const double d = glm::length(target - root);
    const glm::dvec3 want = d <= l1 + l2 ? target : root + (target - root) * ((l1 + l2) / d);
    INFO("end at " << str(end) << ", want " << str(want));
    CHECK(glm::length(end - want) < tol);
    // Every joint's X runs down its bone.
    CHECK(glm::dot(glm::normalize(glm::dvec3(rig.world(joints[0])[0])), glm::normalize(mid - root)) == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(glm::dot(glm::normalize(glm::dvec3(rig.world(joints[1])[0])), glm::normalize(end - mid)) == doctest::Approx(1.0).epsilon(1e-6));
}

/** The signed angle, about the root to target axis, from the pole's direction to the middle joint's. */
double bendAngle(const glm::dvec3& root, const glm::dvec3& target, const glm::dvec3& pole, const glm::dvec3& mid)
{
    const glm::dvec3 axis = glm::normalize(target - root);
    auto across = [&](const glm::dvec3& p) {
        const glm::dvec3 v = p - root;
        return glm::normalize(v - glm::dot(v, axis) * axis);
    };
    const glm::dvec3 a = across(pole), b = across(mid);
    return glm::degrees(std::atan2(glm::dot(glm::cross(a, b), axis), glm::dot(a, b)));
}

/** The bend plane's normal in a joint's own frame: the solve turns the chain with the plane, so this never changes. */
glm::dvec3 planeInJoint(const Rig& rig, const std::array<std::string, 3>& joints, int which)
{
    const glm::dvec3 root = pos(rig.world(joints[0])), mid = pos(rig.world(joints[1])), end = pos(rig.world(joints[2]));
    return glm::transpose(rot(rig.world(joints[size_t(which)]))) * glm::normalize(glm::cross(mid - root, end - root));
}

void checkIk(bool zUp)
{
    Rig rig("rig_ik", zUp,
            {"footCtl.translateX", "footCtl.translateY", "footCtl.translateZ", "pvCtl.translateX", "pvCtl.translateZ", "footCtl.twist",
             "handCtl.translateX", "handCtl.translateY", "handCtl.translateZ", "stretchCtl.translateY", "stretchCtl.translateZ"});
    rig.dump();
    CHECK(rig.result.skipped.empty());
    const std::vector<Pose> poses = {
        {},
        {{"footCtl.translateY", 3}, {"footCtl.translateZ", 2}},
        {{"footCtl.translateX", 2}, {"footCtl.translateY", 1}, {"pvCtl.translateX", 6}},
        {{"footCtl.translateY", 4}, {"footCtl.twist", 30}},
        {{"footCtl.translateY", -3}},
        {{"handCtl.translateX", 10}, {"handCtl.translateY", 11}, {"handCtl.translateZ", 1}},
        {{"handCtl.translateX", 11}, {"handCtl.translateY", 8.5}, {"handCtl.translateZ", -1}},
        {{"stretchCtl.translateY", 2}, {"stretchCtl.translateZ", 1}},
        {{"stretchCtl.translateY", -4}, {"stretchCtl.translateZ", 2}},
    };
    const std::array<std::string, 3> leg{"hip", "knee", "ankle"}, arm{"shoulder", "elbow", "wrist"},
        stretch{"stretchHip", "stretchKnee", "stretchAnkle"};
    // The joints where the .ma builds them.
    const double legL1 = glm::length(glm::dvec3(0, -5, 1)), legL2 = glm::length(glm::dvec3(0, -5, -1));
    const double armL1 = glm::length(glm::dvec3(3, 0, -1)), armL2 = glm::length(glm::dvec3(3, 0, 1));

    rig.pose({});
    rig.expectAt("hip", {0, 10, 0});
    rig.expectAt("knee", {0, 5, 1});
    rig.expectAt("ankle", {0, 0, 0});
    rig.expectAt("shoulder", {6, 10, 0});
    rig.expectAt("elbow", {9, 10, -1});
    rig.expectAt("wrist", {12, 10, 0});
    rig.expectAt("stretchKnee", {20, 5, 1});
    rig.expectAt("stretchAnkle", {20, 0, 0});
    const glm::dvec3 legPlane[2] = {planeInJoint(rig, leg, 0), planeInJoint(rig, leg, 1)};
    const glm::dvec3 armPlane[2] = {planeInJoint(rig, arm, 0), planeInJoint(rig, arm, 1)};

    for (size_t i = 0; i < poses.size(); ++i)
    {
        INFO("pose " << i);
        rig.pose(poses[i]);

        // Rotate plane: the knee bends toward the pole vector's control, turned by twist about the hip to foot axis.
        const glm::dvec3 foot = pos(rig.world("footCtl"));
        checkLimb(rig, leg, foot, legL1, legL2);
        const glm::dvec3 hip = pos(rig.world("hip"));
        if (glm::length(foot - hip) < legL1 + legL2 - 1e-6)
        {
            CHECK(std::abs(bendAngle(hip, foot, pos(rig.world("pvCtl")), pos(rig.world("knee")))) ==
                  doctest::Approx(std::abs(rig.get("footCtl.twist"))).epsilon(1e-6).scale(1.0));
            for (int j = 0; j < 2; ++j)
                CHECK(std::abs(glm::dot(planeInJoint(rig, leg, j), legPlane[j])) == doctest::Approx(1.0).epsilon(1e-6));
        }

        // Single chain: the wrist reaches the hand control, the elbow bending in the arm's own plane.
        checkLimb(rig, arm, pos(rig.world("handCtl")), armL1, armL2);
        for (int j = 0; j < 2; ++j)
            CHECK(std::abs(glm::dot(planeInJoint(rig, arm, j), armPlane[j])) == doctest::Approx(1.0).epsilon(1e-3));

        // Stretchy leg: past its length the bones grow by distance over length, so the ankle always reaches.
        const glm::dvec3 target = pos(rig.world("stretchCtl")), root(20, 10, 0);
        const double ratio = std::max(1.0, glm::length(target - root) / (legL1 + legL2));
        checkLimb(rig, stretch, target, legL1 * ratio, legL2 * ratio);
    }
}

// ---------------------------------------------------------------------------------------------------- set driven keys

void checkSdk(bool zUp)
{
    Rig rig("rig_sdk", zUp, {"ctl.translateX", "ctl.translateY", "ctl.translateZ", "ctl.rotateX", "ctl.curl"});
    rig.dump();
    CHECK(rig.result.skipped.empty());

    // j1.translateY sums a linear curve on ctl.translateX (0 -> 0, 2 -> 4, held past its ends) and a spline curve on
    // ctl.translateZ (0 -> 0, 1 -> -1, 3 -> 2, continued linearly past its ends). j2.rotateZ sums a linear curve on
    // ctl.rotateX (0 -> 0, 90 -> -45, held) and a flat-tangent curve on curl (0 -> 0, 10 -> 60, held). j1.rotateX follows a
    // clamped curve on ctl.translateY (-1 -> -30, 1 -> 30).
    auto at = [&](const Pose& p) {
        rig.pose(p);
        return glm::dvec3(rig.get("j1.translateY"), rig.get("j2.rotateZ"), rig.get("j1.rotateX"));
    };
    auto exact = [&](const Pose& p, const glm::dvec3& want) {
        const glm::dvec3 got = at(p);
        INFO("got " << str(got) << ", want " << str(want));
        CHECK(glm::length(got - want) < 1e-6);
    };
    exact({}, {0, 0, 0});
    exact({{"ctl.translateX", 1}}, {2, 0, 0});
    exact({{"ctl.translateX", 3}}, {4, 0, 0});
    exact({{"ctl.translateZ", 1}}, {-1, 0, 0});
    exact({{"ctl.translateZ", 3}}, {2, 0, 0});
    // Flat tangents meet halfway at half the change.
    exact({{"ctl.rotateX", 45}, {"ctl.curl", 5}}, {0, -22.5 + 30.0, 0});
    exact({{"ctl.rotateX", 120}, {"ctl.curl", 12}}, {0, -45.0 + 60.0, 0});

    // Between spline keys the curve stays near its keys; past them it runs on in straight lines.
    const double mid = at({{"ctl.translateX", 3}, {"ctl.translateZ", 0.5}}).x - 4.0;
    CHECK(mid < 0.0);
    CHECK(mid > -1.25);
    const double later = at({{"ctl.translateZ", 2}}).x;
    CHECK(later > -1.25);
    CHECK(later < 2.0);
    const double before[3] = {at({}).x, at({{"ctl.translateZ", -1}}).x, at({{"ctl.translateZ", -2}}).x};
    CHECK(before[1] > 0.0);
    CHECK(before[2] - before[1] == doctest::Approx(before[1] - before[0]));
    const double after[3] = {at({{"ctl.translateZ", 3}}).x, at({{"ctl.translateZ", 4}}).x, at({{"ctl.translateZ", 5}}).x};
    CHECK(after[2] - after[1] == doctest::Approx(after[1] - after[0]));

    // Two clamped keys mirror about their midpoint.
    const double up = at({{"ctl.translateY", 0.4}}).z, down = at({{"ctl.translateY", -0.4}}).z;
    CHECK(up > 0.0);
    CHECK(up < 30.0);
    CHECK(down == doctest::Approx(-up));

    // The stacked keys in one pose, and the tip riding the chain.
    const glm::dvec3 got = at({{"ctl.translateZ", -1}, {"ctl.rotateX", 120}, {"ctl.curl", 12}, {"ctl.translateY", 0.4}});
    CHECK(got.x == doctest::Approx(before[1]));
    CHECK(got.y == doctest::Approx(15.0));
    CHECK(got.z == doctest::Approx(up));
    rig.expect("tip", rig.world("j2") * T({0, 2, 0}));
}

// ---------------------------------------------------------------------------------------------------- deformers

/** Points moved by a cluster handle: m carries them from rest, scaled by their weights. */
std::vector<glm::dvec3> clustered(std::vector<glm::dvec3> pts, const glm::dmat4& m, const std::map<size_t, double>& weights)
{
    for (const auto& [i, w] : weights)
        pts[i] = glm::mix(pts[i], xform(m, pts[i]), w);
    return pts;
}

std::map<size_t, double> weightsOf(size_t first, size_t last, double w = 1.0)
{
    std::map<size_t, double> out;
    for (size_t i = first; i <= last; ++i)
        out[i] = w;
    return out;
}

/** A nonlinear deformer's effect, given as its handle-space function, placed by the handle. */
template <class F>
std::vector<glm::dvec3> nonlinear(std::vector<glm::dvec3> pts, const glm::dmat4& handle, F f)
{
    for (glm::dvec3& p : pts)
        p = xform(handle, f(xform(glm::inverse(handle), p)));
    return pts;
}

void checkDeformers()
{
    Rig rig("rig_deformers", false,
            {"cluHandle.translateY", "cluHandle.rotateZ", "cluBHandle.translateX", "latLattice.translateY", "latLattice.scaleX", "bndHandle.rotateY",
             "twstHandle.translateY", "sqshHandle.scaleY", "sneHandle.rotateZ", "wveHandle.translateX", "flrHandle.translateY", "softHandle.translateY"});
    rig.dump();
    CHECK(rig.result.skipped.empty());

    rig.pose({});
    std::map<std::string, glm::dmat4> rest;
    for (const char* h : {"cluHandle", "cluBHandle", "latLattice"})
        rest[h] = rig.world(h);
    const std::vector<glm::dvec3> latRest = rig.points("latCube");
    // At rest the clusters, nonlinears and softMod leave the meshes as they are.
    for (const char* m : {"clusterPlane", "clusterPlaneB", "softPlane"})
        rig.expectPoints(m, rig.basePoints(m));

    const Pose posed = {{"cluHandle.translateY", 2},   {"cluHandle.rotateZ", 20},     {"cluBHandle.translateX", -1}, {"latLattice.translateY", 0.5},
                        {"latLattice.scaleX", 1.4},    {"bndHandle.rotateY", 30},     {"twstHandle.translateY", 0.3}, {"sqshHandle.scaleY", 1.3},
                        {"sneHandle.rotateZ", 15},     {"wveHandle.translateX", 0.4}, {"flrHandle.translateY", 0.25}, {"softHandle.translateY", 1}};
    for (const Pose& p : {Pose{}, posed})
    {
        INFO(std::string(p.empty() ? "rest" : "posed"));
        rig.pose(p);
        auto moved = [&](const char* h) { return rig.world(h) * glm::inverse(rest.at(h)); };

        // Points 0 to 12 weigh 1, but 5 to 9 weigh 0.5 and move half as far.
        std::map<size_t, double> w = weightsOf(0, 12);
        for (size_t i = 5; i <= 9; ++i)
            w[i] = 0.5;
        rig.expectPoints("clusterPlane", clustered(rig.basePoints("clusterPlane"), moved("cluHandle"), w));
        rig.expectPoints("clusterPlaneB", clustered(rig.basePoints("clusterPlaneB"), moved("cluBHandle"), weightsOf(0, 4)));

        // A lattice deforms in its own space, so moving it carries the shape with it.
        {
            std::vector<glm::dvec3> want = latRest;
            for (glm::dvec3& q : want)
                q = xform(moved("latLattice"), q);
            rig.expectPoints("latCube", want);
        }

        // The nonlinears as Lila's deformers with the .ma's settings: Maya's bend curvature is per unit along the handle,
        // its twist turns the other way, its squash factor of -0.4 shortens to 1 / 1.4, its wave offset starts a quarter
        // wave in, and its flare's X and Z agree.
        rig.expectPoints("bendCube", nonlinear(rig.basePoints("bendCube"), rig.world("bndHandle"),
                                               [](const glm::dvec3& q) { return deform::bend(q, {.angle = 60.0 * 2.0}); }));
        rig.expectPoints("twistCube", nonlinear(rig.basePoints("twistCube"), rig.world("twstHandle"), [](const glm::dvec3& q) {
                             return deform::twist(q, {.startAngle = 30.0, .endAngle = -90.0});
                         }));
        {
            const double sy = 1.0 / 1.4;
            const double expand = (1.0 / sy - 1.0) / (1.0 / std::sqrt(sy) - 1.0);
            rig.expectPoints("squashCube", nonlinear(rig.basePoints("squashCube"), rig.world("sqshHandle"), [&](const glm::dvec3& q) {
                                 return deform::squash(q, {.factor = sy - 1.0, .expand = expand, .maxExpandPos = 0.5});
                             }));
        }
        rig.expectPoints("sinePlane", nonlinear(rig.basePoints("sinePlane"), rig.world("sneHandle"), [](const glm::dvec3& q) {
                             return deform::sine(q, {.amplitude = 0.3, .wavelength = 1.5});
                         }));
        rig.expectPoints("wavePlane", nonlinear(rig.basePoints("wavePlane"), rig.world("wveHandle"), [](const glm::dvec3& q) {
                             return deform::wave(q, {.amplitude = 0.2, .wavelength = 0.8, .offset = -0.25});
                         }));
        rig.expectPoints("flareCube", nonlinear(rig.basePoints("flareCube"), rig.world("flrHandle"), [](const glm::dvec3& q) {
                             return deform::taper(q, {.startScale = 1.5, .endScale = 0.5});
                         }));

        // The .ma leaves the softMod's falloffCenter at the origin, more than its 1.5 radius from every point of the
        // plane, so moving its handle moves nothing.
        rig.expectPoints("softPlane", rig.basePoints("softPlane"));
    }
}

void checkSurfaceDeformers()
{
    Rig rig("rig_surface_deformers", false, {"mid.rotateZ", "driverJoint.rotateZ", "driverJoint.translateY"});
    rig.dump();

    rig.pose({});
    const glm::dmat4 mid0 = rig.world("mid"), driver0 = rig.world("driverJoint");
    // Delta mush and wrap bind at rest, so the rest pose is the meshes as stored.
    rig.expectPoints("tube", rig.basePoints("tube"));
    rig.expectPoints("ball", rig.basePoints("ball"));
    const std::vector<glm::dvec3> tube0 = rig.basePoints("tube"), ball0 = rig.basePoints("ball");

    rig.pose({{"mid.rotateZ", 60}, {"driverJoint.rotateZ", 25}, {"driverJoint.translateY", 0.5}});
    // Away from the bend the mushed tube keeps its shape: the base stays, the top turns with mid.
    {
        const std::vector<glm::dvec3> got = rig.points("tube");
        const glm::dmat4 turn = rig.world("mid") * glm::inverse(mid0);
        double worstBase = 0.0, worstTop = 0.0;
        for (size_t i = 0; i < got.size(); ++i)
        {
            if (tube0[i].y <= 1.0)
                worstBase = std::max(worstBase, glm::length(got[i] - tube0[i]));
            else if (tube0[i].y >= 5.0)
                worstTop = std::max(worstTop, glm::length(got[i] - xform(turn, tube0[i])));
        }
        CHECK(worstBase < 0.15);
        CHECK(worstTop < 0.25);
    }
    // The wrap's driver moves rigidly with its one joint, so the ball does too.
    {
        std::vector<glm::dvec3> want = ball0;
        for (glm::dvec3& p : want)
            p = xform(rig.world("driverJoint") * glm::inverse(driver0), p);
        rig.expectPoints("ball", want, 1e-3);
    }
}

// ---------------------------------------------------------------------------------------------------- pins

void checkPins()
{
    Rig rig("rig_pins", false,
            {"pinPlane.translateX", "pinPlane.rotateX", "pinPlane.rotateY", "pinPlane.scaleZ", "sheetBendHandle.translateY", "sheetBendHandle.rotateX"});
    rig.dump();
    CHECK(rig.result.skipped.empty());

    rig.pose({});
    const glm::dmat4 bend0 = rig.world("sheetBendHandle");
    const std::vector<glm::dvec3> sheet0 = rig.basePoints("proxSheet");
    // The proximityPin's input matrix as the .ma stores it: rotated (0.2, 0.4, 0.1) radians, xyz, at (0.6, 0.3, 9.1).
    const glm::dmat4 pinRest = T({0.6, 0.3, 9.1}) * R(glm::degrees(glm::dvec3(0.2, 0.4, 0.1)));
    rig.expect("pinnedC", pinRest);

    const std::vector<Pose> poses = {
        {},
        {{"pinPlane.translateX", -2}, {"pinPlane.rotateX", 30}, {"pinPlane.rotateY", 50}, {"pinPlane.scaleZ", 1.5}},
        {{"sheetBendHandle.translateY", 1.5}, {"sheetBendHandle.rotateX", 25}},
    };
    for (size_t i = 0; i < poses.size(); ++i)
    {
        INFO("pose " << i);
        rig.pose(poses[i]);
        const glm::dmat4 plane = rig.trs("pinPlane");
        // A 4 by 4 plane: u runs along +X, v along -Z.
        auto onPlane = [&](double u, double v) { return xform(plane, {(u - 0.5) * 4.0, 0.0, (0.5 - v) * 4.0}); };
        const glm::dvec3 normal = glm::normalize(glm::transpose(glm::inverse(glm::dmat3(plane))) * glm::dvec3(0, 1, 0));
        const glm::dvec3 du = glm::normalize(glm::dvec3(plane[0]));

        // The follicle sits on the surface at its UV with one axis along the normal.
        {
            const glm::dmat4 f = rig.world("fol");
            rig.expectAt("fol", onPlane(0.3, 0.7));
            double best = 0.0;
            for (int c = 0; c < 3; ++c)
                best = std::max(best, std::abs(glm::dot(glm::normalize(glm::dvec3(f[c])), normal)));
            CHECK(best == doctest::Approx(1.0).epsilon(1e-6));
        }
        // uvPin: Y is the normal, X the U tangent.
        for (const auto& [pin, uv] : {std::pair{"pinnedA", glm::dvec2(0.6, 0.25)}, std::pair{"pinnedB", glm::dvec2(0.9, 0.9)}})
        {
            const glm::dmat4 m = rig.world(pin);
            rig.expectAt(pin, onPlane(uv.x, uv.y));
            INFO(pin);
            CHECK(glm::dot(glm::normalize(glm::dvec3(m[1])), normal) == doctest::Approx(1.0).epsilon(1e-6));
            CHECK(glm::dot(glm::normalize(glm::dvec3(m[0])), du) == doctest::Approx(1.0).epsilon(1e-6));
        }

        // proximityPin: the point bound under the pin rides the sheet as the cluster bends it, the pin held off the
        // surface along its normal. Lila carries the frame on a triangle where Maya smooths it, so this is close, not exact.
        {
            const glm::dvec3 bound(0.6, 0.0, 9.1);
            std::array<size_t, 4> quad{};
            const glm::dvec3 lo(0, 0, 9), corner[4] = {lo, lo + glm::dvec3(1, 0, 0), lo + glm::dvec3(1, 0, 1), lo + glm::dvec3(0, 0, 1)};
            for (int c = 0; c < 4; ++c)
                for (size_t k = 0; k < sheet0.size(); ++k)
                    if (glm::length(sheet0[k] - corner[c]) < 1e-6)
                        quad[size_t(c)] = k;
            const glm::dmat4 bend = rig.world("sheetBendHandle") * glm::inverse(bend0);
            const std::vector<glm::dvec3> sheet = clustered(sheet0, bend, weightsOf(0, 9));
            const double s = bound.x - lo.x, t = bound.z - lo.z;
            const glm::dvec3 p = glm::mix(glm::mix(sheet[quad[0]], sheet[quad[1]], s), glm::mix(sheet[quad[3]], sheet[quad[2]], s), t);
            glm::dvec3 n = glm::normalize(glm::cross(sheet[quad[3]] - sheet[quad[1]], sheet[quad[2]] - sheet[quad[0]]));
            if (n.y < 0.0)
                n = -n;
            rig.expectAt("pinnedC", p + n * 0.3, 0.06);
        }
    }
}

// ---------------------------------------------------------------------------------------------------- combined

void checkCombined(bool zUp)
{
    Rig rig("rig_combined", zUp,
            {"footCtl.translateX", "footCtl.translateY", "footCtl.translateZ", "footCtl.rotateZ", "footCtl.footRoll", "footCtl.toeTap", "footCtl.toeCurl",
             "kneeCtl.translateX", "hipCtl.translateY", "padLatticeLattice.translateZ", "padLatticeLattice.scaleX"});
    rig.dump();
    CHECK(rig.result.skipped.empty());

    rig.pose({});
    // The joints where the .ma builds them, and the rest the constrained controls keep their offsets from.
    rig.expectAt("hip", {0, 10, 0});
    rig.expectAt("knee", {0, 5.5, 0.8});
    rig.expectAt("ankle", {0, 1, 0});
    rig.expectAt("ball", {0, 0, 2});
    rig.expectAt("toe", {0, 0, 4});
    rig.expect("kneeFollow", T({1, 5.5, 0.8}));
    const glm::dmat4 knee0 = rig.world("knee"), kneeFollow0 = rig.world("kneeFollow"), cluster0 = rig.world("padClusterHandle");
    const glm::dmat4 lattice0 = rig.trs("padLatticeLattice");
    const std::vector<glm::dvec3> pad0 = rig.basePoints("kneePad");
    rig.expectPoints("kneePad", pad0);
    const double l1 = glm::length(glm::dvec3(0, -4.5, 0.8)), l2 = glm::length(glm::dvec3(0, -4.5, -0.8));
    const double l3 = glm::length(glm::dvec3(0, -1, 2)), l4 = 2.0;

    const std::vector<Pose> poses = {
        {},
        {{"footCtl.translateY", 2}, {"footCtl.translateZ", 1}, {"footCtl.footRoll", -20}},
        {{"footCtl.footRoll", 15}, {"footCtl.toeTap", 25}, {"footCtl.toeCurl", 4}},
        {{"footCtl.footRoll", 50}, {"footCtl.rotateZ", 20}, {"kneeCtl.translateX", 4}, {"footCtl.toeCurl", 10}},
        {{"footCtl.translateX", 1.5}, {"footCtl.translateY", 3}, {"hipCtl.translateY", -1}, {"padLatticeLattice.translateZ", 0.5},
         {"padLatticeLattice.scaleX", 1.3}},
    };
    for (size_t i = 0; i < poses.size(); ++i)
    {
        INFO("pose " << i);
        rig.pose(poses[i]);

        // The reverse foot: footRoll below 0 tips the heel, up to 30 rolls the ball, past that the toe; toeTap lifts the
        // toe the other way.
        const double roll = rig.get("footCtl.footRoll");
        const double ballRoll = std::clamp(roll, 0.0, 30.0);
        auto pivot = [&](const char* n, double rx) { return T(rig.vec(n, "translate")) * R({rx, 0, 0}); };
        const glm::dmat4 foot = T(rig.vec("footCtl", "translate")) * R({0, 0, rig.get("footCtl.rotateZ")});
        const glm::dmat4 heel = foot * pivot("heelPivot", std::clamp(roll, -90.0, 0.0));
        const glm::dmat4 toePivot = heel * pivot("toePivot", std::clamp(roll - ballRoll, 0.0, 90.0));
        const glm::dmat4 ballPivot = toePivot * pivot("ballPivot", ballRoll);
        const glm::dmat4 tapPivot = toePivot * pivot("toeTapPivot", -rig.get("footCtl.toeTap"));
        auto handle = [&](const glm::dmat4& parent, const char* h) { return xform(parent, rig.vec(h, "translate")); };

        // The hip follows its control; the leg reaches the ball pivot's handle, the knee toward kneeCtl.
        const glm::dvec3 hip = rig.vec("hipCtl", "translate");
        rig.expectAt("hip", hip);
        const glm::dvec3 legTarget = handle(ballPivot, "legIk");
        checkLimb(rig, {"hip", "knee", "ankle"}, legTarget, l1, l2);
        if (glm::length(legTarget - hip) < l1 + l2 - 1e-6)
            CHECK(bendAngle(hip, legTarget, pos(rig.world("kneeCtl")), pos(rig.world("knee"))) == doctest::Approx(0.0).scale(1.0));

        // Single bone chains point at their handles.
        const glm::dvec3 ankle = pos(rig.world("ankle")), ball = pos(rig.world("ball"));
        rig.expectAt("ball", ankle + glm::normalize(handle(toePivot, "ballIk") - ankle) * l3, 1e-3);
        rig.expectAt("toe", ball + glm::normalize(handle(tapPivot, "toeIk") - ball) * l4, 1e-3);

        // The toe's stacked keys: toeCurl 0 to 10 turns it 0 to 40, footCtl.rotateZ 0 to 30 turns it 0 to -15.
        CHECK(rig.get("toe.rotateZ") == doctest::Approx(4.0 * rig.get("footCtl.toeCurl") - 0.5 * rig.get("footCtl.rotateZ")));

        // kneeFollow keeps its offset from the knee, the cluster handle its offset from kneeFollow.
        const glm::dmat4 kneeFollow = rig.world("knee") * glm::inverse(knee0) * kneeFollow0;
        rig.expect("kneeFollow", kneeFollow);
        const glm::dmat4 cluster = kneeFollow * glm::inverse(kneeFollow0) * cluster0;
        rig.expect("padClusterHandle", cluster);

        // The pad: its clustered points carried by the handle, then the lattice's move carries the points still inside
        // its base (a flat box, 0.001 thick); its points are as built, so inside it is affine.
        std::vector<glm::dvec3> want = clustered(pad0, cluster * glm::inverse(cluster0), [] {
            std::map<size_t, double> w = weightsOf(6, 8);
            w.merge(weightsOf(11, 13));
            return w;
        }());
        const glm::dmat4 lattice = rig.trs("padLatticeLattice") * glm::inverse(lattice0);
        const glm::dmat4 baseInv = glm::inverse(rig.trs("padLatticeBase"));
        for (glm::dvec3& p : want)
            if (const glm::dvec3 q = xform(baseInv, p); glm::all(glm::lessThanEqual(glm::abs(q), glm::dvec3(0.5 + 1e-9))))
                p = xform(lattice, p);
        rig.expectPoints("kneePad", want);
    }
}
} // namespace

TEST_CASE("maya rig: utility nodes compute their documented maths")
{
    checkUtility(false);
}

TEST_CASE("maya rig: matrix nodes and offset parent matrices")
{
    checkMatrix(false);
}

TEST_CASE("maya rig: constraints keep their offsets, weights and targets")
{
    checkConstraints(false);
}

TEST_CASE("maya rig: IK handles with pole vectors, twist and stretch")
{
    checkIk(false);
}

TEST_CASE("maya rig: single and stacked set driven keys")
{
    checkSdk(false);
}

TEST_CASE("maya rig: cluster, lattice, nonlinear and softMod deformers")
{
    checkDeformers();
}

TEST_CASE("maya rig: delta mush and wrap")
{
    checkSurfaceDeformers();
}

TEST_CASE("maya rig: follicles, uvPins and proximityPins")
{
    checkPins();
}

TEST_CASE("maya rig: a leg with IK, a reverse foot, constraints, a cluster, a lattice and stacked set driven keys")
{
    checkCombined(false);
}

TEST_CASE("maya rig: networks, constraints and the leg rig hold across an axis change")
{
    // Read as Z up, every vector, matrix and channel goes through the axis change, and the rigs still hold in Maya's space.
    SUBCASE("utility") { checkUtility(true); }
    SUBCASE("matrix") { checkMatrix(true); }
    SUBCASE("constraints") { checkConstraints(true); }
    SUBCASE("sdk") { checkSdk(true); }
    SUBCASE("combined") { checkCombined(true); }
}

TEST_CASE("maya rig: what stays unconverted is reported")
{
    maya::ImportResult r = importRig("rig_unsupported");
    for (const std::string& s : r.skipped)
        MESSAGE("skipped: " << s);
    CHECK(mentions(r.skipped, "expression 'wobble'"));
    CHECK(mentions(r.skipped, "ikSplineSolver"));
    CHECK(mentions(r.skipped, "motionPath"));
    CHECK(mentions(r.skipped, "curveInfo"));
    CHECK(mentions(r.skipped, "wire 'sheetWire'"));
    CHECK(mentions(r.skipped, "ctl.label"));
    // A texture deformer with a file texture converts; Lila's sampling is its own, so only its settings are checked.
    Graph& g = Graph::current();
    Node* td = g.find("bumpDeform");
    REQUIRE(td);
    CHECK(td->type().name == "textureDeformer");
    CHECK(td->as<std::string>(td->type().attrIndex("image")).ends_with("front.png"));
    CHECK(td->as<double>(td->type().attrIndex("strength")) == doctest::Approx(0.5));
    CHECK(td->as<double>(td->type().attrIndex("midLevel")) == doctest::Approx(0.5));
    // The addDL nodes the motion path made come across as Lila math nodes, though what drives them doesn't.
    CHECK_FALSE(mentions(r.skipped, "addDL node"));
}

TEST_CASE("maya rig: decompose and compose round trip with shear")
{
    mathnodes::Decomposed d;
    d.translate = {1, 2, 3};
    d.rotation = glm::dmat3(eulerToMatrix({10, 20, 30}, RotateOrder::ZXY));
    d.scale = {1.5, 0.5, 2};
    d.shear = {0.2, -0.1, 0.3};
    const mathnodes::Decomposed back = mathnodes::decompose(mathnodes::compose(d));
    CHECK(glm::length(back.translate - d.translate) < 1e-12);
    CHECK(glm::length(back.scale - d.scale) < 1e-12);
    CHECK(glm::length(back.shear - d.shear) < 1e-12);
    for (int c = 0; c < 3; ++c)
        CHECK(glm::length(back.rotation[c] - d.rotation[c]) < 1e-12);
}
