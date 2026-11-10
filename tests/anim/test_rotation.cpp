#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/RotationCurve.h"

#include <filesystem>

using namespace lila;
using namespace lila::scene;
using namespace animtest;
using json = nlohmann::json;

namespace
{
/** Angle in degrees between two rotations. */
double angleBetween(const glm::dquat& a, const glm::dquat& b)
{
    return glm::degrees(glm::length(anim::quatLog(glm::inverse(a) * b)));
}

glm::dquat poseOf(const glm::dvec3& euler, RotateOrder order) { return anim::eulerToQuat(euler, order); }

anim::RotationCurve twoKeys(anim::RotInterp interp, const glm::dvec3& a, const glm::dvec3& b)
{
    anim::RotationCurve rc;
    rc.interp = interp;
    rc.keys = {rc.makeKey(0.0, a), rc.makeKey(1.0, b)};
    return rc;
}

/** A joint with a rotation curve: three keys on rotateY and Z. */
void keyedJoint()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"name", "j"}});
    run("currentTime", {{"args", {0.0}}});
    run("setKeyframe", {{"args", {"j.rotate"}}});
    run("currentTime", {{"args", {1.0}}});
    run("setAttr", {{"args", {"j.rotate", v3(0, 90, 30)}}});
    run("setKeyframe", {{"args", {"j.rotate"}}});
    run("currentTime", {{"args", {2.0}}});
    run("setAttr", {{"args", {"j.rotate", v3(45, 20, -60)}}});
    run("setKeyframe", {{"args", {"j.rotate"}}});
}

glm::dvec3 rotatePlugValue(const char* node)
{
    Graph& g = Graph::current();
    return std::get<glm::dvec3>(g.value(g.plug(std::string(node) + ".rotate")));
}
} // namespace

TEST_CASE("slerp rotation curves match glm's slerp")
{
    glm::dvec3 a(10, 20, 30), b(-40, 80, 120);
    anim::RotationCurve rc = twoKeys(anim::RotInterp::Slerp, a, b);
    glm::dquat qa = poseOf(a, RotateOrder::XYZ), qb = poseOf(b, RotateOrder::XYZ);
    for (double t : {0.0, 0.1, 0.37, 0.5, 0.81, 1.0})
        CHECK(angleBetween(rc.evaluateQuat(t), glm::slerp(qa, glm::dot(qa, qb) < 0 ? -qb : qb, t)) < 1e-6);
}

TEST_CASE("smooth with linear tangents is the slerp; with auto it eases and passes through every key")
{
    glm::dvec3 a(0, 0, 0), b(30, 60, -20), c(-10, 100, 45);
    anim::RotationCurve lin = twoKeys(anim::RotInterp::Smooth, a, b);
    for (anim::RotKey& k : lin.keys)
        k.in = k.out = anim::Tangent::Linear;
    anim::RotationCurve sl = twoKeys(anim::RotInterp::Slerp, a, b);
    for (double t = 0.0; t <= 1.0; t += 0.05)
        CHECK(angleBetween(lin.evaluateQuat(t), sl.evaluateQuat(t)) < 1e-6);

    anim::RotationCurve sm;
    sm.keys = {sm.makeKey(0.0, a), sm.makeKey(1.0, b), sm.makeKey(2.0, c)};
    for (const anim::RotKey& k : sm.keys)
        CHECK(angleBetween(sm.evaluateQuat(k.time), k.q) < 1e-9);
    // Auto: flat at the ends, so it eases out of the first key.
    CHECK(glm::length(sm.outVelocity(0)) == doctest::Approx(0.0));
    double early = angleBetween(sm.evaluateQuat(0.0), sm.evaluateQuat(0.05));
    double late = angleBetween(sm.evaluateQuat(0.45), sm.evaluateQuat(0.5));
    CHECK(early < late);
    // C1 through the middle key: the angular speed either side agrees.
    double h = 1e-4;
    double before = angleBetween(sm.evaluateQuat(1.0 - h), sm.evaluateQuat(1.0));
    double after = angleBetween(sm.evaluateQuat(1.0), sm.evaluateQuat(1.0 + h));
    CHECK(before == doctest::Approx(after).epsilon(1e-3));
    // Step holds.
    sm.keys[0].out = anim::Tangent::Step;
    CHECK(angleBetween(sm.evaluateQuat(0.7), sm.keys[0].q) < 1e-9);
}

TEST_CASE("rotation curve output is continuous Euler, keeps typed values and extra turns")
{
    anim::RotationCurve rc;
    rc.order = RotateOrder::ZXY;
    rc.keys = {rc.makeKey(0.0, {0, 0, 0}), rc.makeKey(1.0, {170, 10, 0}), rc.makeKey(2.0, {200, 40, 30})};
    CHECK(rc.evaluate(1.0).x == doctest::Approx(170.0));
    CHECK(rc.evaluate(2.0).z == doctest::Approx(30.0));
    glm::dvec3 prev = rc.evaluate(0.0);
    for (double t = 0.01; t <= 2.0; t += 0.01)
    {
        glm::dvec3 e = rc.evaluate(t);
        CAPTURE(t);
        CHECK(glm::length(e - prev) < 90.0);
        // The Euler output rebuilds the quaternion exactly.
        CHECK(angleBetween(poseOf(e, rc.order), rc.evaluateQuat(t)) < 1e-6);
        prev = e;
    }
    // Two full turns between the last keys: the path really spins rather than taking the short way.
    anim::RotationCurve spin;
    spin.interp = anim::RotInterp::Slerp;
    spin.keys = {spin.makeKey(0.0, {0, 0, 0}), spin.makeKey(1.0, {0, 0, 720})};
    CHECK(spin.evaluate(0.25).z == doctest::Approx(180.0));
    CHECK(spin.evaluate(0.5).z == doctest::Approx(360.0));
}

TEST_CASE("Euler and rotation curves convert both ways keeping the pose at every key")
{
    std::array<anim::Curve, 3> xyz;
    xyz[0].keys = {{.time = 0.0, .value = 0.0}, {.time = 1.0, .value = 90.0}, {.time = 2.0, .value = 180.0}};
    xyz[1].keys = {{.time = 0.0, .value = 10.0}, {.time = 1.5, .value = -45.0}};
    anim::RotationCurve rc = anim::rotationFromEuler({&xyz[0], &xyz[1], nullptr}, {0, 0, 30}, RotateOrder::YZX, anim::RotInterp::Smooth);
    REQUIRE(rc.keys.size() == 4);
    for (const anim::RotKey& k : rc.keys)
    {
        glm::dvec3 e(xyz[0].evaluate(k.time), xyz[1].evaluate(k.time), 30.0);
        CHECK(angleBetween(k.q, poseOf(e, RotateOrder::YZX)) < 1e-6);
    }
    std::array<anim::Curve, 3> back = anim::eulerFromRotation(rc);
    for (size_t i = 0; i < rc.keys.size(); ++i)
    {
        glm::dvec3 e(back[0].keys[i].value, back[1].keys[i].value, back[2].keys[i].value);
        CHECK(back[0].keys[i].time == doctest::Approx(rc.keys[i].time));
        CHECK(angleBetween(poseOf(e, RotateOrder::YZX), rc.keys[i].q) < 1e-6);
    }
}

TEST_CASE("Euler filter removes flips and wraps without changing the pose")
{
    std::array<anim::Curve, 3> c;
    c[0].keys = {{.time = 0.0, .value = 0.0}, {.time = 1.0, .value = 170.0}, {.time = 2.0, .value = -170.0}};
    c[1].keys = {{.time = 0.0, .value = 0.0}, {.time = 1.0, .value = 0.0}, {.time = 2.0, .value = 0.0}};
    c[2].keys = {{.time = 0.0, .value = 0.0}, {.time = 1.0, .value = 0.0}, {.time = 2.0, .value = 0.0}};
    std::array<anim::Curve, 3> before = c;
    CHECK(anim::eulerFilter({&c[0], &c[1], &c[2]}, glm::dvec3(0.0), RotateOrder::XYZ) > 0);
    CHECK(c[0].keys[2].value == doctest::Approx(190.0));
    for (size_t i = 0; i < 3; ++i)
    {
        glm::dvec3 a(before[0].keys[i].value, before[1].keys[i].value, before[2].keys[i].value);
        glm::dvec3 b(c[0].keys[i].value, c[1].keys[i].value, c[2].keys[i].value);
        CHECK(angleBetween(poseOf(a, RotateOrder::XYZ), poseOf(b, RotateOrder::XYZ)) < 1e-6);
    }
}

TEST_CASE("loop key mirrors the first key, live, and cycles")
{
    anim::Curve c;
    c.keys = {{.time = 0.0, .value = 0.0}, {.time = 0.3, .value = 4.0}, {.time = 0.6, .value = -6.0}, {.time = 1.0, .value = 3.0}};
    c.loop = true;
    CHECK(c.evaluate(1.0) == doctest::Approx(0.0));
    CHECK(c.evaluate(1.25) == doctest::Approx(c.evaluate(0.25)));
    // Seamless: the slope arriving at the loop key equals the slope leaving the first.
    double h = 1e-6;
    double arriving = (c.evaluate(1.0) - c.evaluate(1.0 - h)) / h, leaving = (c.evaluate(h) - c.evaluate(0.0)) / h;
    CHECK(std::abs(leaving) > 1.0);
    CHECK(arriving == doctest::Approx(leaving).epsilon(1e-3));
    c.keys[0].value = 2.0;
    CHECK(c.evaluate(1.0) == doctest::Approx(2.0));

    anim::RotationCurve rc;
    rc.keys = {rc.makeKey(0.0, {0, 10, 0}), rc.makeKey(1.0, {0, 80, 0}), rc.makeKey(2.0, {50, 50, 50})};
    rc.loop = true;
    CHECK(angleBetween(rc.evaluateQuat(2.0), rc.keys[0].q) < 1e-9);
    CHECK(angleBetween(rc.evaluateQuat(2.3), rc.evaluateQuat(0.3)) < 1e-9);
}

TEST_CASE("joints key into a rotation curve that commands edit")
{
    keyedJoint();
    Graph& g = Graph::current();
    CHECK(run("nodeType", {{"args", {"j_rotate"}}}) == "animCurveRQ");
    CHECK(run("keyframe", {{"args", {"j.rotateY"}}, {"query", true}}) == json({0.0, 1.0, 2.0}));
    CHECK(run("keyframe", {{"args", {"j.rotateY"}}, {"query", true}, {"valueChange", true}}) == json({0.0, 90.0, 20.0}));
    run("currentTime", {{"args", {1.0}}});
    CHECK(rotatePlugValue("j").y == doctest::Approx(90.0));
    CHECK(rotatePlugValue("j").z == doctest::Approx(30.0));

    // One axis's value change rewrites that key's quaternion; a time change moves the whole key.
    run("keyframe", {{"args", {"j.rotateZ"}}, {"time", 1.0}, {"valueChange", 10.0}, {"relative", true}});
    CHECK(rotatePlugValue("j").z == doctest::Approx(40.0));
    run("keyframe", {{"args", {"j.rotateX"}}, {"time", 1.0}, {"timeChange", 1.5}});
    CHECK(run("keyframe", {{"args", {"j"}}, {"attribute", "rotateZ"}, {"query", true}}) == json({0.0, 1.5, 2.0}));

    // Tangents and infinity.
    run("keyTangent", {{"args", {"j.rotateY"}}, {"inTangentType", "linear"}, {"outTangentType", "linear"}});
    json tq = run("keyTangent", {{"args", {"j.rotateY"}}, {"query", true}});
    CHECK(tq[0]["out"] == "linear");
    run("keyTangent", {{"args", {"j.rotateY"}}, {"time", 0.0}, {"outAngle", 30.0}});
    tq = run("keyTangent", {{"args", {"j.rotateY"}}, {"time", 0.0}, {"query", true}});
    CHECK(tq[0]["outAngle"].get<double>() == doctest::Approx(30.0).epsilon(1e-2));
    run("setInfinity", {{"args", {"j"}}, {"postInfinite", "cycle"}});
    CHECK(run("setInfinity", {{"args", {"j"}}, {"query", true}}).size() == 1);

    // Auto key through a whole-rotate set, and the component set from the channel box.
    run("autoKeyframe", {{"state", true}});
    run("currentTime", {{"args", {0.5}}});
    run("setAttr", {{"args", {"j.rotate", v3(5, 6, 7)}}});
    run("setAttr", {{"args", {"j.rotateX", 9.0}}});
    run("autoKeyframe", {{"state", false}});
    CHECK(rotatePlugValue("j").x == doctest::Approx(9.0));
    CHECK(rotatePlugValue("j").z == doctest::Approx(7.0));
    CHECK(run("keyframe", {{"args", {"j.rotateX"}}, {"query", true}, {"keyCount", true}}) == 4);

    // Copy and paste the whole rotation.
    run("joint", {{"name", "k"}});
    run("copyKey", {{"args", {"j"}}, {"attribute", "rotate"}});
    run("pasteKey", {{"args", {"k"}}, {"attribute", "rotate"}, {"time", 0.0}});
    CHECK(run("nodeType", {{"args", {"k_rotate"}}}) == "animCurveRQ");
    run("currentTime", {{"args", {0.5}}});
    CHECK(rotatePlugValue("k").x == doctest::Approx(9.0));

    // findKeyframe and cutKey.
    CHECK(run("findKeyframe", {{"args", {"j"}}, {"which", "last"}}).get<double>() == doctest::Approx(2.0));
    CHECK(run("cutKey", {{"args", {"j"}}, {"time", 0.5}, {"clear", true}}) == 1);
    CHECK(anim::keyTimes(g, {g.find("j")->id()}).size() == 3);
    CHECK(run("cutKey", {{"args", {"j"}}, {"clear", true}}) == 3);
    CHECK_FALSE(run("objExists", {{"args", {"j_rotate"}}}).get<bool>());
}

TEST_CASE("rotateOrder changes keep the pose; undo restores both")
{
    keyedJoint();
    run("currentTime", {{"args", {1.0}}});
    Graph& g = Graph::current();
    g.evaluate();
    glm::dmat4 before = g.find("j")->as<glm::dmat4>(Transform::kWorldMatrix);
    run("setAttr", {{"args", {"j.rotateOrder", 4}}});
    g.evaluate();
    glm::dmat4 after = g.find("j")->as<glm::dmat4>(Transform::kWorldMatrix);
    for (int c = 0; c < 3; ++c)
        CHECK(glm::length(before[c] - after[c]) < 1e-6);
    CHECK(g.find("j_rotate")->as<int64_t>(anim::RotationCurveNode::kRotateOrder) == 4);
    run("undo");
    CHECK(g.find("j_rotate")->as<int64_t>(anim::RotationCurveNode::kRotateOrder) == 0);
}

TEST_CASE("convertRotationCurves goes both ways keeping key times and poses; eulerFilter on Euler curves")
{
    keyedJoint();
    Graph& g = Graph::current();
    std::vector<glm::dvec3> poses;
    for (double t : {0.0, 1.0, 2.0})
    {
        run("currentTime", {{"args", {t}}});
        poses.push_back(rotatePlugValue("j"));
    }
    CHECK(run("convertRotationCurves", {{"args", {"j"}}, {"to", "euler"}}) == 1);
    CHECK(run("nodeType", {{"args", {"j_rotateX"}}}) == "animCurveTA");
    CHECK(g.find("j")->as<int64_t>(Transform::kRotationInterpolation) == 0);
    CHECK(run("keyframe", {{"args", {"j.rotateZ"}}, {"query", true}}) == json({0.0, 1.0, 2.0}));
    for (int i = 0; i < 3; ++i)
    {
        run("currentTime", {{"args", {double(i)}}});
        CHECK(angleBetween(poseOf(rotatePlugValue("j"), RotateOrder::XYZ), poseOf(poses[size_t(i)], RotateOrder::XYZ)) < 1e-6);
    }
    CHECK(run("eulerFilter", {{"args", {"j"}}}) == 0);
    CHECK(run("convertRotationCurves", {{"args", {"j"}}, {"to", "slerp"}}) == 1);
    CHECK(run("nodeType", {{"args", {"j_rotate"}}}) == "animCurveRQ");
    CHECK(g.find("j_rotate")->as<int64_t>(anim::RotationCurveNode::kInterpolation) == int64_t(anim::RotInterp::Slerp));
    for (int i = 0; i < 3; ++i)
    {
        run("currentTime", {{"args", {double(i)}}});
        CHECK(angleBetween(poseOf(rotatePlugValue("j"), RotateOrder::XYZ), poseOf(poses[size_t(i)], RotateOrder::XYZ)) < 1e-6);
    }
    run("undo");
    run("undo");
    CHECK(run("nodeType", {{"args", {"j_rotate"}}}) == "animCurveRQ");
}

TEST_CASE("setLoopKey and clearLoopKey, on scalar and rotation curves, snap the range")
{
    keyedJoint();
    run("setKeyframe", {{"args", {"j.tx"}}, {"time", 0.0}, {"value", 1.0}});
    run("setKeyframe", {{"args", {"j.tx"}}, {"time", 1.0}, {"value", 4.0}});
    CHECK(run("setLoopKey", {{"args", {"j"}}, {"time", 3.0}}).get<double>() == doctest::Approx(3.0));
    CHECK(anim::playback().maxTime == doctest::Approx(3.0));
    Graph& g = Graph::current();
    CHECK(anim::loopKeyTimes(g, {g.find("j")->id()}) == std::vector<double>{3.0});
    run("currentTime", {{"args", {3.0}}});
    CHECK(rotatePlugValue("j").y == doctest::Approx(0.0));
    CHECK(run("getAttr", {{"args", {"j.tx"}}}).get<double>() == doctest::Approx(1.0));
    run("currentTime", {{"args", {3.5}}});
    double cycled = run("getAttr", {{"args", {"j.tx"}}}).get<double>();
    run("currentTime", {{"args", {0.5}}});
    CHECK(run("getAttr", {{"args", {"j.tx"}}}).get<double>() == doctest::Approx(cycled));
    // Live: editing the first key moves the loop key with it.
    run("keyframe", {{"args", {"j.tx"}}, {"time", 0.0}, {"valueChange", 2.0}});
    run("currentTime", {{"args", {3.0}}});
    CHECK(run("getAttr", {{"args", {"j.tx"}}}).get<double>() == doctest::Approx(2.0));
    CHECK(run("clearLoopKey", {{"args", {"j"}}}) == 2);
    CHECK(anim::loopKeyTimes(g, {g.find("j")->id()}).empty());
}

TEST_CASE("rotation curves save and load in both scene formats")
{
    for (const char* file : {"rotation.lila", "rotation.lilaa"})
    {
        CAPTURE(file);
        keyedJoint();
        run("setLoopKey", {{"args", {"j"}}, {"snapRange", false}});
        run("currentTime", {{"args", {0.7}}});
        glm::dvec3 before = rotatePlugValue("j");
        std::string path = tempPath(file);
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        run("currentTime", {{"args", {0.0}}});
        run("currentTime", {{"args", {0.7}}});
        CHECK(run("nodeType", {{"args", {"j_rotate"}}}) == "animCurveRQ");
        CHECK(glm::length(rotatePlugValue("j") - before) < 1e-6);
        CHECK(anim::curveLoops(*Graph::current().find("j_rotate")));
    }
}

TEST_CASE("eulerKeysFor writes the rotation curve's own keys where Euler curves can follow it, frames only where needed")
{
    std::vector<double> frames;
    for (int f = 0; f <= 48; ++f)
        frames.push_back(f / 24.0);
    anim::RotationCurve one;
    one.keys = {one.makeKey(0.0, {0, 0, 0}), one.makeKey(1.0, {0, 70, 0}), one.makeKey(2.0, {0, -35, 0})};
    std::array<anim::Curve, 3> a = anim::eulerKeysFor(one, frames);
    CHECK(a[1].keys.size() == 3);

    anim::RotationCurve two;
    two.keys = {two.makeKey(0.0, {0, 0, 0}), two.makeKey(1.0, {60, 70, 0}), two.makeKey(2.0, {-30, -35, 80})};
    std::array<anim::Curve, 3> b = anim::eulerKeysFor(two, frames, 0.01);
    CHECK(b[0].keys.size() < frames.size());
    for (double f : frames)
    {
        glm::dvec3 e(b[0].evaluate(f), b[1].evaluate(f), b[2].evaluate(f));
        CHECK(angleBetween(poseOf(e, RotateOrder::XYZ), two.evaluateQuat(f)) < 0.011);
    }
}

TEST_CASE("setting rotationInterpolation converts the keyed rotate curves, and undo puts them back")
{
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    for (auto [t, ry] : {std::pair{0.0, 0.0}, {10.0, 90.0}, {20.0, 180.0}})
    {
        run("currentTime", {{"args", {t}}});
        run("setAttr", {{"args", {"A.rotate", json::array({30.0, ry, 0.0})}}});
        run("setKeyframe", {{"args", {"A.rotate"}}});
    }
    REQUIRE(run("nodeType", {{"args", {"A_rotateX"}}}) == "animCurveTA");
    run("setAttr", {{"args", {"A.rotationInterpolation", 1}}});
    CHECK(run("nodeType", {{"args", {"A_rotate"}}}) == "animCurveRQ");
    CHECK(Graph::current().find("A")->as<int64_t>(Transform::kRotationInterpolation) == 1);
    run("undo");
    CHECK(run("nodeType", {{"args", {"A_rotateX"}}}) == "animCurveTA");
    CHECK(Graph::current().find("A")->as<int64_t>(Transform::kRotationInterpolation) == 0);
}
