#include "TestUtil.h"

#include "ops/Undo.h"
#include "scene/Strings.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <chrono>

using namespace rigtest;
using namespace lila::scene;

namespace
{
/** A stick biped in cm: hips at 100, three spine joints, neck and head, arms out to the sides, legs down. */
void stringsRigBiped()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    auto j = [](const char* name, double x, double y, double z, const char* parent) {
        run("joint", {{"position", v3(x, y, z)}, {"name", name}, {"parent", parent}, {"skipSelect", true}});
    };
    j("hips", 0, 100, 0, "");
    j("spine_01", 0, 110, 0, "hips");
    j("spine_02", 0, 122, 0, "spine_01");
    j("spine_03", 0, 134, 0, "spine_02");
    j("neck", 0, 150, 0, "spine_03");
    j("head", 0, 162, 0, "neck");
    for (const char* side : {"l", "r"})
    {
        const double s = side[0] == 'l' ? 1.0 : -1.0;
        auto n = [&](const char* base) { return std::string(base) + "_" + side; };
        j(n("upperarm").c_str(), 18 * s, 140, 0, "spine_03");
        j(n("lowerarm").c_str(), 46 * s, 140, -2, n("upperarm").c_str());
        j(n("hand").c_str(), 72 * s, 140, 0, n("lowerarm").c_str());
        j(n("thigh").c_str(), 10 * s, 95, 0, "hips");
        j(n("calf").c_str(), 10 * s, 52, 4, n("thigh").c_str());
        j(n("foot").c_str(), 10 * s, 8, 0, n("calf").c_str());
    }
}

strings::Skeleton snap() { return strings::snapshot(graph(), id("hips")); }

int bone(const strings::Skeleton& k, const char* name) { return k.boneOf(id(name)); }

strings::Hold holdAt(const strings::Skeleton& k, const char* name)
{
    const int b = bone(k, name);
    return {.bone = b, .world = strings::worlds(k.problem, strings::startPose(k.problem))[size_t(b)]};
}

/** Solves r on the biped and writes the result, returning it. */
strings::Result pose(strings::Request r)
{
    const strings::Skeleton k = snap();
    r.hips = k.hips;
    const strings::Result out = strings::solve(k.problem, r);
    strings::apply(graph(), k, out.pose);
    return out;
}

glm::dquat turnX(double degrees) { return glm::angleAxis(glm::radians(degrees), glm::dvec3(1.0, 0.0, 0.0)); }

void checkStringsMatrix(const glm::dmat4& a, const glm::dmat4& b, double eps = 1e-5)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 3; ++r)
            CHECK(a[c][r] == doctest::Approx(b[c][r]).epsilon(eps).scale(1.0));
}

double localAngle(const glm::dmat4& before, const glm::dmat4& after)
{
    return glm::degrees(glm::angle(glm::normalize(glm::quat_cast(glm::dmat3(after) * glm::transpose(glm::dmat3(before))))));
}
} // namespace

TEST_CASE("strings: mode names round trip")
{
    for (strings::Mode m : {strings::Mode::Forward, strings::Mode::Backward, strings::Mode::Ik, strings::Mode::FullBody})
        CHECK(strings::modeFromName(strings::modeName(m)) == m);
    CHECK(!strings::modeFromName("sideways"));
}

TEST_CASE("strings: limbs run up to the branch")
{
    stringsRigBiped();
    const strings::Skeleton k = snap();
    CHECK(k.hips == bone(k, "hips"));
    const std::vector<int> leg = strings::limbOf(k.problem, bone(k, "foot_l"), k.hips);
    CHECK(leg == std::vector<int>{bone(k, "foot_l"), bone(k, "calf_l"), bone(k, "thigh_l")});
    const std::vector<int> arm = strings::limbOf(k.problem, bone(k, "hand_r"), k.hips);
    CHECK(arm == std::vector<int>{bone(k, "hand_r"), bone(k, "lowerarm_r"), bone(k, "upperarm_r")});
    CHECK(strings::limbOf(k.problem, bone(k, "hand_r"), k.hips, -1, 2).size() == 2);
}

TEST_CASE("strings: forward turns the joint and carries its children")
{
    stringsRigBiped();
    const glm::dmat4 hips = world("hips"), foot = world("foot_l");
    pose({.mode = strings::Mode::Forward, .grabbed = snap().boneOf(id("calf_l")), .turn = turnX(30.0)});
    checkStringsMatrix(world("hips"), hips);
    CHECK(glm::length(pos("foot_l") - glm::dvec3(foot[3])) > 5.0);
}

TEST_CASE("strings: backward from the knee keeps the planted foot and swings the body")
{
    stringsRigBiped();
    const strings::Skeleton k = snap();
    const glm::dmat4 foot = world("foot_l"), calf = world("calf_l"), hips = world("hips"), other = world("foot_r");
    const glm::dmat4 calfLocal = node("calf_l").as<glm::dmat4>(Transform::kMatrix);
    const strings::Result r = pose({.mode = strings::Mode::Backward,
                                    .grabbed = bone(k, "calf_l"),
                                    .turn = turnX(25.0),
                                    .holds = {holdAt(k, "foot_l"), holdAt(k, "foot_r")}});
    CHECK(r.anchor == bone(k, "foot_l"));
    checkStringsMatrix(world("foot_l"), foot);
    checkStringsMatrix(world("calf_l"), calf);
    // The other foot was held too: its limb bent to keep it.
    CHECK(glm::length(pos("foot_r") - glm::dvec3(other[3])) < 1e-3);
    CHECK(glm::length(pos("hips") - glm::dvec3(hips[3])) > 5.0);
    // The knee bent by what was dragged, as Forward would have.
    CHECK(localAngle(calfLocal, node("calf_l").as<glm::dmat4>(Transform::kMatrix)) == doctest::Approx(25.0).epsilon(1e-6));
}

TEST_CASE("strings: backward with nothing held hangs the body from the grabbed joint")
{
    stringsRigBiped();
    const strings::Skeleton k = snap();
    const glm::dmat4 head = world("head"), neck = world("neck"), spine = world("spine_01");
    const strings::Result r = pose({.mode = strings::Mode::Backward, .grabbed = bone(k, "neck"), .turn = turnX(20.0)});
    CHECK(r.anchor == bone(k, "neck"));
    checkStringsMatrix(world("head"), head);
    checkStringsMatrix(world("neck"), neck);
    CHECK(glm::length(pos("spine_01") - glm::dvec3(spine[3])) > 1.0);
}

TEST_CASE("strings: backward above the hips is not handled")
{
    run("newScene");
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("joint", {{"position", v3(0, 100, 0)}, {"name", "pelvis"}});
    run("joint", {{"position", v3(10, 50, 0)}, {"name", "leg"}, {"parent", "pelvis"}, {"skipSelect", true}});
    run("joint", {{"position", v3(-10, 50, 0)}, {"name", "leg2"}, {"parent", "pelvis"}, {"skipSelect", true}});
    const strings::Skeleton k = strings::snapshot(graph(), id("root"));
    CHECK(k.hips == k.boneOf(id("pelvis")));
    strings::Request r{.mode = strings::Mode::Backward, .grabbed = k.boneOf(id("root")), .turn = turnX(10.0), .hips = k.hips};
    CHECK(!strings::solve(k.problem, r).handled);
}

TEST_CASE("strings: IK moves the hand by bending its arm alone")
{
    stringsRigBiped();
    const strings::Skeleton k = snap();
    const glm::dvec3 target = pos("hand_l") + glm::dvec3(-10.0, 15.0, 8.0);
    const glm::dmat4 hand = world("hand_l"), spine = world("spine_03"), other = world("hand_r");
    const strings::Result r = pose({.mode = strings::Mode::Ik, .grabbed = bone(k, "hand_l"), .move = target - pos("hand_l")});
    CHECK(r.error < 1e-3);
    CHECK(glm::length(pos("hand_l") - target) < 1e-3);
    // The hand keeps its world orientation; nothing above the arm moves.
    for (int c = 0; c < 3; ++c)
        CHECK(glm::length(glm::dvec3(world("hand_l")[c]) - glm::dvec3(hand[c])) < 1e-3);
    checkStringsMatrix(world("spine_03"), spine);
    checkStringsMatrix(world("hand_r"), other);
}

TEST_CASE("strings: IK on the hips moves the body and the planted feet hold")
{
    stringsRigBiped();
    const strings::Skeleton k = snap();
    const glm::dmat4 l = world("foot_l"), rf = world("foot_r");
    const glm::dvec3 hips = pos("hips");
    const strings::Result r = pose({.mode = strings::Mode::Ik,
                                    .grabbed = bone(k, "hips"),
                                    .move = glm::dvec3(0.0, -12.0, 3.0),
                                    .holds = {holdAt(k, "foot_l"), holdAt(k, "foot_r")}});
    CHECK(r.error < 1e-3);
    CHECK(glm::length(pos("hips") - (hips + glm::dvec3(0.0, -12.0, 3.0))) < 1e-6);
    CHECK(glm::length(pos("foot_l") - glm::dvec3(l[3])) < 1e-3);
    CHECK(glm::length(pos("foot_r") - glm::dvec3(rf[3])) < 1e-3);
}

TEST_CASE("strings: IK turning the hips keeps the feet")
{
    stringsRigBiped();
    const strings::Skeleton k = snap();
    const glm::dmat4 l = world("foot_l");
    pose({.mode = strings::Mode::Ik,
          .grabbed = bone(k, "hips"),
          .turn = glm::angleAxis(glm::radians(15.0), glm::dvec3(0.0, 1.0, 0.0)),
          .holds = {holdAt(k, "foot_l"), holdAt(k, "foot_r")}});
    CHECK(glm::length(pos("foot_l") - glm::dvec3(l[3])) < 1e-3);
}

TEST_CASE("strings: full body puts the hand on the target and the feet stay")
{
    stringsRigBiped();
    const strings::Skeleton k = snap();
    const glm::dvec3 target = pos("hand_l") + glm::dvec3(10.0, -45.0, 20.0);
    const glm::dmat4 l = world("foot_l");
    const strings::Result r = pose({.mode = strings::Mode::FullBody,
                                    .grabbed = bone(k, "hand_l"),
                                    .move = target - pos("hand_l"),
                                    .holds = {holdAt(k, "foot_l"), holdAt(k, "foot_r")}});
    CHECK(r.error < 0.05);
    CHECK(glm::length(pos("hand_l") - target) < 0.05);
    CHECK(glm::length(pos("foot_l") - glm::dvec3(l[3])) < 0.05);
}

TEST_CASE("strings: the solve leaves the scene's nodes alone and undo restores the pose")
{
    stringsRigBiped();
    const size_t nodes = graph().nodes().size();
    const glm::dmat4 hips = world("hips");
    {
        UndoChunk chunk("strings");
        const strings::Skeleton k = snap();
        pose({.mode = strings::Mode::Backward, .grabbed = bone(k, "calf_l"), .turn = turnX(25.0), .holds = {holdAt(k, "foot_l")}});
    }
    CHECK(graph().nodes().size() == nodes);
    CHECK(glm::length(pos("hips") - glm::dvec3(hips[3])) > 1.0);
    run("undo");
    checkStringsMatrix(world("hips"), hips, 1e-12);
}

TEST_CASE("strings: a solve on the biped is interactive")
{
    stringsRigBiped();
    const strings::Skeleton k = snap();
    strings::Request r{.mode = strings::Mode::Backward, .grabbed = bone(k, "calf_l"), .turn = turnX(25.0),
                       .holds = {holdAt(k, "foot_l"), holdAt(k, "foot_r")}, .hips = k.hips};
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i)
        strings::solve(k.problem, r);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20.0;
    MESSAGE("backward solve " << ms << " ms");
    CHECK(ms < 20.0);
}

TEST_CASE("strings: holdLimbs brings a hand to a reachable target")
{
    stringsRigBiped();
    const strings::Skeleton k = snap();
    strings::Pose s = strings::startPose(k.problem);
    const int hand = bone(k, "hand_r");
    glm::dmat4 target = strings::worlds(k.problem, s)[size_t(hand)];
    target[3] += glm::dvec4(12.0, 10.0, 15.0, 0.0);
    const double e = strings::holdLimbs(k.problem, s, {{.bone = hand, .world = target}}, k.hips);
    MESSAGE("error " << e);
    CHECK(e < 1e-3);
}

TEST_CASE("strings: IK pulls a nearly straight arm inward")
{
    stringsRigBiped();
    const strings::Skeleton k = snap();
    const glm::dvec3 target = pos("hand_l") + glm::dvec3(-12.0, 10.0, 15.0);
    const strings::Result r = pose({.mode = strings::Mode::Ik, .grabbed = bone(k, "hand_l"), .move = glm::dvec3(-12.0, 10.0, 15.0)});
    MESSAGE("error " << r.error);
    CHECK(glm::length(pos("hand_l") - target) < 1e-3);
}

TEST_CASE("line of action: the spine takes a stroke's curve, scaled to its length, while the feet hold")
{
    stringsRigBiped();
    const strings::Skeleton k = snap();
    const std::vector<int> path = {bone(k, "hips"), bone(k, "spine_01"), bone(k, "spine_02"), bone(k, "spine_03"), bone(k, "neck"), bone(k, "head")};
    const std::vector<glm::dmat4> w0 = strings::worlds(k.problem, strings::startPose(k.problem));
    std::vector<glm::dvec3> points;
    for (int b : path)
        points.push_back(glm::dvec3(w0[size_t(b)][3]));
    // A C curve leaning forward, longer than the spine: it is scaled to the spine.
    std::vector<glm::dvec3> stroke;
    for (int i = 0; i <= 40; ++i)
    {
        const double a = glm::radians(90.0) * i / 40.0;
        stroke.push_back(glm::dvec3(0.0, 300.0 + 80.0 * std::sin(a), 80.0 * (1.0 - std::cos(a))));
    }
    std::reverse(stroke.begin(), stroke.end()); // drawn from the head end: turned round to start at the hips
    const std::vector<glm::dvec3> targets = strings::conformTargets(points, stroke);
    REQUIRE(targets.size() == points.size());
    CHECK(glm::length(targets.front() - points.front()) < 1e-9);
    // Scaled to the spine's length: the targets' arc spans what the joints do.
    CHECK(strings::strokeLength(targets) == doctest::Approx(strings::strokeLength(points)).epsilon(0.05));
    CHECK(targets.back().z > 20.0);

    const strings::Result r = strings::conform(k.problem, path, targets, {holdAt(k, "foot_l"), holdAt(k, "foot_r")}, k.hips);
    const std::vector<glm::dmat4> w1 = strings::worlds(k.problem, r.pose);
    double worst = 0.0;
    for (size_t i = 0; i < path.size(); ++i)
        worst = std::max(worst, glm::length(glm::dvec3(w1[size_t(path[i])][3]) - targets[i]));
    CHECK(worst < 2.0);
    for (const char* foot : {"foot_l", "foot_r"})
        CHECK(glm::length(glm::dvec3(w1[size_t(bone(k, foot))][3] - w0[size_t(bone(k, foot))][3])) < 1e-3);

    // A path stroke: the path lies on it unscaled, its first joint at the window's start.
    const std::vector<glm::dvec3> along = strings::conformTargets(points, stroke, 10.0);
    CHECK(glm::length(along.front() - strings::conformAt(points, stroke, 10.0, 0.0)) < 1e-9);
    CHECK(strings::pathFraction(points, points.back()) == doctest::Approx(1.0));
    CHECK(strings::pathFraction(points, glm::dvec3(5.0, 122.0, 0.0)) == doctest::Approx(22.0 / 62.0).epsilon(1e-6));
}
