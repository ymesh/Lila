#include <doctest/doctest.h>

#include "TestUtil.h"
#include "hand/HandMath.h"

#include "curve/CurveNodes.h"

#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <random>

using namespace puppeteer::hand;
using namespace puptest;
using Approx = doctest::Approx;

namespace
{
bool sameAngles(const Angles& a, const Angles& b, double eps = 1e-6)
{
    return std::abs(a.bend - b.bend) < eps && std::abs(a.splay - b.splay) < eps && std::abs(a.twist - b.twist) < eps;
}

/** A hand with joints in world space and identity rest rotations; mirror puts it on the right, x flipped. */
std::vector<SkelJoint> handSkeleton(bool right, bool named = true)
{
    const double sx = right ? -1.0 : 1.0;
    const std::string s = right ? "_r" : "_l";
    std::vector<SkelJoint> js;
    auto add = [&](const std::string& name, int parent, glm::dvec3 p) {
        js.push_back({named ? name + s : "b" + std::to_string(js.size()), parent, {p.x * sx, p.y, p.z}, glm::dmat3(1.0)});
        return int(js.size()) - 1;
    };
    const int root = add("root", -1, {0, 0, 0});
    const int shoulder = add("upperarm", root, {15, 140, 0});
    const int elbow = add("lowerarm", shoulder, {30, 120, 0});
    const int hand = add("hand", elbow, {50, 100, 0});
    const char* names[4] = {"index", "middle", "ring", "pinky"};
    const double zs[4] = {3, 1, -1, -3}, lens[4] = {1.0, 1.1, 1.0, 0.8};
    for (int f = 0; f < 4; ++f)
    {
        int p = hand;
        glm::dvec3 at(58, 100, zs[f]);
        const double seg[4] = {3.0, 2.5, 2.0, 1.0};
        for (int k = 0; k < 4; ++k)
        {
            p = add(std::string(names[f]) + "_0" + std::to_string(k + 1), p, at);
            at += glm::dvec3(seg[k] * lens[f], 0, 0);
        }
    }
    int p = hand;
    glm::dvec3 at(52, 99, 3.5);
    const glm::dvec3 dir = glm::normalize(glm::dvec3(1, -0.2, 1));
    for (int k = 0; k < 4; ++k)
    {
        p = add("thumb_0" + std::to_string(k + 1), p, at);
        at += dir * (k == 0 ? 3.0 : 2.4);
    }
    return js;
}

/** Forward kinematics over a skeleton whose rest rotations are identity: world positions with a local rotation per joint. */
std::vector<glm::dvec3> fk(const std::vector<SkelJoint>& js, const std::map<int, glm::dmat3>& local)
{
    std::vector<glm::dmat3> rot(js.size(), glm::dmat3(1.0));
    std::vector<glm::dvec3> pos(js.size());
    for (size_t i = 0; i < js.size(); ++i)
    {
        const int p = js[i].parent;
        const glm::dmat3 l = local.count(int(i)) ? local.at(int(i)) : glm::dmat3(1.0);
        if (p < 0)
        {
            rot[i] = l;
            pos[i] = js[i].position;
            continue;
        }
        rot[i] = rot[size_t(p)] * l;
        pos[i] = pos[size_t(p)] + rot[size_t(p)] * (js[i].position - js[size_t(p)].position);
    }
    return pos;
}

/** Locals putting a pose on a detected hand, through its frames. */
std::map<int, glm::dmat3> posed(const std::vector<SkelJoint>& js, const HandRig& h, const Pose& p)
{
    std::map<int, glm::dmat3> out;
    for (const FingerRig& f : h.fingers)
    {
        if (!p.fingers[int(f.role)])
            continue;
        const FingerFrames ff = fingerFrames(js, h, f);
        if (f.meta >= 0)
            out[f.meta] = localFor(*ff.meta, p.fingers[int(f.role)]->meta);
        const std::vector<Angles> a = toJoints(p.fingers[int(f.role)]->joints, f.joints.size());
        for (size_t k = 0; k < f.joints.size(); ++k)
            out[f.joints[k]] = localFor(ff.joints[k], a[k]);
    }
    return out;
}
} // namespace

TEST_CASE("hands: resampling keeps each channel's total and is exact for three joints")
{
    const std::vector<double> v = {90, 105, 75};
    CHECK(resample(v, 3) == v);
    for (size_t m : {1, 2, 4, 5})
    {
        const std::vector<double> r = resample(v, m);
        double sum = 0;
        for (double x : r)
            sum += x;
        CHECK(sum == Approx(270.0));
    }
    const std::array<Angles, kCanon> c = {Angles{40, 5, 2}, Angles{60, 0, 0}, Angles{30, 0, 0}};
    const std::array<Angles, kCanon> back = fromJoints(toJoints(c, 3));
    for (int j = 0; j < kCanon; ++j)
        CHECK(sameAngles(back[j], c[j]));
    // Two joints carry all the curl, the splay on the knuckle.
    const std::vector<Angles> two = toJoints(c, 2);
    CHECK(two[0].bend + two[1].bend == Approx(130.0));
    CHECK(two[0].splay == Approx(5.0));
}

TEST_CASE("hands: a curl spreads over the joints by tip and fits back exactly")
{
    for (Role r : {Role::Index, Role::Pinky, Role::Thumb})
        for (double curl : {-0.2, 0.0, 0.3, 0.8, 1.0})
            for (double tip : {-1.0, -0.4, 0.0, 0.5, 1.0})
            {
                const FingerShape s{curl, tip, 7.0, -3.0};
                const std::array<Angles, kCanon> a = anglesFor(s, r);
                double c = 0, t = 0;
                fitCurl(a, r, c, t);
                CHECK(c == Approx(curl));
                if (std::abs(curl) > 1e-9)
                    CHECK(t == Approx(tip));
            }
    // A fist bends every joint fully; a hook (tip 1) leaves the knuckle straight; a table top (tip -1) only the knuckle.
    const std::array<Angles, kCanon> fist = anglesFor(FingerShape{1.0}, Role::Index);
    CHECK(fist[0].bend == Approx(90.0));
    CHECK(fist[1].bend == Approx(105.0));
    CHECK(fist[2].bend == Approx(75.0));
    CHECK(anglesFor(FingerShape{1.0, 1.0}, Role::Index)[0].bend == Approx(0.0));
    CHECK(anglesFor(FingerShape{1.0, -1.0}, Role::Index)[1].bend == Approx(0.0));
}

TEST_CASE("hands: limits hold every joint in its range")
{
    FingerPose f;
    f.joints = {Angles{200, 90, 90}, Angles{-90, 30, 30}, Angles{300, 0, 0}};
    f.meta = {90, 90, 90};
    const FingerPose c = clamp(f, Role::Index);
    const std::array<Limit, kCanon> l = canonLimits(Role::Index);
    for (int j = 0; j < kCanon; ++j)
    {
        CHECK(c.joints[j].bend <= l[j].bend.hi);
        CHECK(c.joints[j].bend >= l[j].bend.lo);
        CHECK(std::abs(c.joints[j].splay) <= std::max(std::abs(l[j].splay.lo), l[j].splay.hi));
    }
    CHECK(c.meta.bend == Approx(metaLimit().bend.hi));
    // A full curl never needs clamping, and a curl past it stops at the joint's end.
    const Pose fist = setCurl(poseFrom(Shape{}), 1.0);
    CHECK(fist.fingers[1]->joints[1].bend == Approx(105.0));
    const Pose over = setCurl(poseFrom(Shape{}), 2.0);
    CHECK(over.fingers[1]->joints[1].bend == Approx(115.0));
    // Resampled limits keep the same reach over fewer joints.
    const std::vector<Limit> two = jointLimits(Role::Index, 2);
    CHECK(two[0].bend.hi + two[1].bend.hi == Approx(95.0 + 115.0 + 90.0));
}

TEST_CASE("hands: hand level values set what they measure and keep the rest of the pose")
{
    Pose p = poseFrom(findLibrary("relaxed")->shape);
    p.fingers[2]->joints[0].twist = 4.0; // a hand keyed twist the shape knows nothing of
    const Pose curled = setCurl(p, 0.6);
    CHECK(measureCurl(curled) == Approx(0.6));
    CHECK(curled.fingers[2]->joints[0].twist == Approx(4.0));
    // The cascade of a relaxed hand survives: the pinky stays more curled than the index.
    CHECK(measureFingerCurl(curled, Role::Pinky) > measureFingerCurl(curled, Role::Index));
    CHECK(measureSpread(setSpread(p, 0.8)) == Approx(0.8));
    CHECK(measureThumb(setThumb(p, -0.7)) == Approx(-0.7));
    CHECK(measureCup(setCup(p, 0.5, true), true) == Approx(0.5));
    CHECK(measureCup(setCup(p, 0.5, false), false) == Approx(0.5));
    CHECK(measureFingerCurl(setFingerCurl(p, Role::Ring, 0.9), Role::Ring) == Approx(0.9));
    CHECK(measureFingerSpread(setFingerSpread(p, Role::Index, 0.5), Role::Index) == Approx(0.5));
    // Masked edits leave the other fingers as they were.
    Mask onlyIndex{};
    onlyIndex[1] = true;
    const Pose one = setCurl(p, 1.0, onlyIndex);
    CHECK(one.fingers[2] == p.fingers[2]);
    CHECK(measureFingerCurl(one, Role::Index) == Approx(1.0));
}

TEST_CASE("hands: blending is linear per joint and respects the finger mask")
{
    const Pose a = poseFrom(findLibrary("flat")->shape), b = poseFrom(findLibrary("fist")->shape);
    const Pose half = mix(a, b, 0.5);
    CHECK(half.fingers[1]->joints[1].bend == Approx(0.5 * b.fingers[1]->joints[1].bend));
    CHECK(mix(a, b, 0.0).fingers[1] == a.fingers[1]);
    CHECK(mix(a, b, 1.0).fingers[3] == b.fingers[3]);
    Mask thumbOnly{};
    thumbOnly[0] = true;
    const Pose t = mix(a, b, 1.0, thumbOnly);
    CHECK(t.fingers[0] == b.fingers[0]);
    CHECK(t.fingers[1] == a.fingers[1]);
    // Relax drags toward the relaxed pose, and negative toward tense.
    const Pose relaxed = poseFrom(findLibrary("relaxed")->shape);
    CHECK(relax(a, 1.0).fingers[4] == relaxed.fingers[4]);
    CHECK(measureCurl(relax(a, -1.0)) < 0.0);
    // Every built-in pose is within limits.
    for (const Library& l : library())
    {
        const Pose p = poseFrom(l.shape);
        for (int r = 0; r < kRoles; ++r)
            CHECK(clamp(*p.fingers[r], Role(r)) == *p.fingers[r]);
    }
}

TEST_CASE("hands: a joint frame turns angles into a rotation and back")
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (int i = 0; i < 50; ++i)
    {
        const glm::dmat3 parent = eulerXYZ({u(rng) * 3, u(rng) * 1.4, u(rng) * 3});
        const glm::dmat3 rest = parent * eulerXYZ({u(rng) * 0.4, u(rng) * 0.4, u(rng) * 0.4});
        const glm::dvec3 straight = glm::normalize(glm::dvec3(u(rng), u(rng), u(rng)));
        const glm::dvec3 bone = glm::normalize(straight + 0.3 * glm::dvec3(u(rng), u(rng), u(rng)));
        const glm::dvec3 axis = glm::normalize(glm::cross(straight, glm::dvec3(u(rng), u(rng), u(rng))));
        const Frame f = makeFrame(parent, rest, straight, bone, axis, glm::dvec3(u(rng), u(rng), u(rng)), false);
        const Angles a{u(rng) * 80, u(rng) * 20, u(rng) * 15};
        CHECK(sameAngles(anglesOf(f, localFor(f, a)), a, 1e-7));
        // The rest pose reads as the bone's own bend from straight, untwisted.
        const Angles r = anglesOf(f, glm::transpose(parent) * rest);
        CHECK(sameAngles(anglesOf(f, localFor(f, r)), r, 1e-7));
        CHECK(r.twist == Approx(0.0).epsilon(1e-6));
    }
}

TEST_CASE("hands: detection reads hands, sides, roles, metacarpals and tips from names")
{
    std::vector<SkelJoint> js = handSkeleton(false);
    const std::vector<SkelJoint> right = handSkeleton(true);
    const int offset = int(js.size());
    for (SkelJoint j : right)
    {
        if (j.parent >= 0)
            j.parent += offset;
        else
            j.parent = 0;
        js.push_back(j);
    }
    const std::vector<HandRig> hands = detectHands(js);
    REQUIRE(hands.size() == 2);
    CHECK(hands[0].side == Side::Left);
    CHECK(hands[1].side == Side::Right);
    CHECK(hands[0].sideFrom == "name");
    for (const HandRig& h : hands)
    {
        REQUIRE(h.fingers.size() == 5);
        for (int r = 0; r < kRoles; ++r)
        {
            CHECK(h.fingers[r].role == Role(r));
            CHECK(h.fingers[r].roleFrom == "name");
            CHECK(h.fingers[r].joints.size() == 3);
            CHECK(h.fingers[r].tip >= 0);
            CHECK(h.fingers[r].meta == -1);
        }
        CHECK(glm::dot(h.palm, glm::dvec3(0, -1, 0)) > 0.9);
    }

    CHECK(sideFromName("LeftHand") == Side::Left);
    CHECK(sideFromName("hand_r") == Side::Right);
    CHECK(sideFromName("Bip01 L Hand") == Side::Left);
    CHECK(sideFromName("R_wrist") == Side::Right);
    CHECK(sideFromName("handL") == Side::Left);
    CHECK(sideFromName("palm") == Side::Center);
    CHECK(roleFromJointName("LeftHandIndex2") == Role::Index);
    CHECK(roleFromJointName("Bip01 R Finger0") == Role::Thumb);
    CHECK(roleFromJointName("Bip01 R Finger31") == Role::Ring);
    CHECK(!roleFromJointName("hand_l").has_value());
}

TEST_CASE("hands: detection by layout finds an unnamed hand, its thumb and its fingers")
{
    std::vector<SkelJoint> js = handSkeleton(false, false);
    const std::vector<HandRig> hands = detectHands(js);
    REQUIRE(hands.size() == 1);
    CHECK(hands[0].hand == 3);
    CHECK(hands[0].sideFrom == "position");
    CHECK(hands[0].side == Side::Left);
    REQUIRE(hands[0].fingers.size() == 5);
    CHECK(hands[0].fingers[0].role == Role::Thumb);
    CHECK(hands[0].fingers[0].roleFrom == "layout");
    // Ordered from the thumb side: the finger at z = 3 is the index.
    CHECK(js[size_t(hands[0].fingers[1].joints[0])].position.z == Approx(3.0));
    CHECK(js[size_t(hands[0].fingers[4].joints[0])].position.z == Approx(-3.0));

    // A metacarpal: a joint near the wrist before the knuckle.
    std::vector<SkelJoint> m = handSkeleton(false);
    const int hand = 3;
    const auto index = std::find_if(m.begin(), m.end(), [](const SkelJoint& j) { return j.name == "index_01_l"; }) - m.begin();
    m.push_back({"index_metacarpal_l", hand, {53, 100, 2}, glm::dmat3(1.0)});
    m[size_t(index)].parent = int(m.size()) - 1;
    const std::vector<HandRig> withMeta = detectHands(m);
    REQUIRE(withMeta.size() == 1);
    CHECK(withMeta[0].fingers[1].meta >= 0);
    CHECK(withMeta[0].fingers[1].joints.size() == 3);
}

TEST_CASE("hands: one pose on both hands mirrors exactly, and curls toward the palm")
{
    const std::vector<SkelJoint> l = handSkeleton(false), r = handSkeleton(true);
    const HandRig hl = detectHands(l).front(), hr = detectHands(r).front();
    for (const Library& lib : library())
    {
        const Pose p = poseFrom(lib.shape);
        const std::vector<glm::dvec3> a = fk(l, posed(l, hl, p)), b = fk(r, posed(r, hr, p));
        for (size_t i = 0; i < a.size(); ++i)
        {
            CHECK(b[i].x == Approx(-a[i].x).epsilon(1e-9));
            CHECK(b[i].y == Approx(a[i].y).epsilon(1e-9));
            CHECK(b[i].z == Approx(a[i].z).epsilon(1e-9));
        }
    }
    // A curl brings the index tip down toward the palm (palm down here); spread fans index toward the thumb (+z).
    const int tip = hl.fingers[1].tip;
    const std::vector<glm::dvec3> rest = fk(l, {});
    const std::vector<glm::dvec3> curled = fk(l, posed(l, hl, setCurl(poseFrom(Shape{}), 0.5)));
    CHECK(curled[size_t(tip)].y < rest[size_t(tip)].y - 2.0);
    const std::vector<glm::dvec3> spread = fk(l, posed(l, hl, setSpread(poseFrom(Shape{}), 1.0)));
    CHECK(spread[size_t(tip)].z > rest[size_t(tip)].z + 1.0);
    CHECK(spread[size_t(hl.fingers[4].tip)].z < rest[size_t(hl.fingers[4].tip)].z - 1.0);
    // The thumb across the palm moves its tip toward the little finger side (-z).
    const std::vector<glm::dvec3> across = fk(l, posed(l, hl, setThumb(poseFrom(Shape{}), 1.0)));
    CHECK(across[size_t(hl.fingers[0].tip)].z < rest[size_t(hl.fingers[0].tip)].z - 1.0);
}

TEST_CASE("hands: wrap stops each joint as its bone touches and closes the rest")
{
    const std::vector<Angles> from(3), to = {Angles{90}, Angles{100}, Angles{80}};
    // The first bone touches once its joint passes 30 degrees; the second once its joint passes 50.
    auto contact = [](const std::vector<Angles>& a) -> int {
        if (a[0].bend > 30.0)
            return 0;
        if (a[1].bend > 50.0)
            return 1;
        return -1;
    };
    const std::vector<Angles> got = wrap(from, to, contact);
    CHECK(got[0].bend == Approx(30.0).epsilon(0.02));
    CHECK(got[1].bend == Approx(50.0).epsilon(0.03));
    CHECK(got[2].bend == Approx(80.0));
    CHECK(contact(got) == -1);
    // Nothing in the way: all the way closed.
    CHECK(wrap(from, to, [](const std::vector<Angles>&) { return -1; }) == to);
}

// ---------------------------------------------------------------------------------------------------------------------
// The extension on a Manny character.

namespace
{
void mannyWithHands()
{
    auto& em = lila::extensions::ExtensionManager::get();
    if (!em.isLoaded("RigTemplates"))
        REQUIRE(em.load(LILA_TEST_RIG_TEMPLATES_PACKAGE) == "RigTemplates");
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"rig", false}});
    run("puppeteerCharacter", {{"args", {"root"}}, {"template", "UE5_Manny"}, {"name", "manny"}});
}

json handInfo(const char* node) { return run("puppeteerHandInfo", {{"args", {node}}}).at(0); }
glm::dvec3 rotateOf(const std::string& joint)
{
    const auto v = std::get<glm::dvec3>(graph().value(graph().plug(joint + ".rotate")));
    return v;
}
} // namespace

TEST_CASE("hands: a Manny character gets both hands set up with every finger")
{
    mannyWithHands();
    const json info = run("puppeteerHandInfo", {{"args", {"manny"}}});
    REQUIRE(info.size() == 2);
    CHECK(info[0]["side"] == "left");
    CHECK(info[1]["side"] == "right");
    for (const json& h : info)
    {
        REQUIRE(h["fingers"].size() == 5);
        CHECK(h["fingers"][0]["role"] == "thumb");
        CHECK(h["fingers"][0]["metacarpal"] == "");
        for (int f = 1; f < 5; ++f)
        {
            CHECK(h["fingers"][f]["roleFrom"] == "name");
            CHECK(h["fingers"][f]["metacarpal"].get<std::string>().find("metacarpal") != std::string::npos);
            CHECK(h["fingers"][f]["joints"].size() == 3);
        }
        // Manny's fingers rest nearly straight, any curl toward the palm.
        CHECK(h["shape"]["curl"].get<double>() > -0.05);
        CHECK(h["shape"]["curl"].get<double>() < 0.4);
    }
    CHECK(handInfo("index_02_l")["name"].get<std::string>().ends_with("hand_l_hand"));
}

TEST_CASE("hands: poses land on the joints, mirror across, blend and key")
{
    mannyWithHands();
    const glm::dvec3 tipRest = pos("index_03_l"), palmSide = pos("middle_metacarpal_l");
    run("puppeteerHandPose", {{"args", {"hand_l"}}, {"pose", "fist"}});
    const json fist = handInfo("hand_l");
    CHECK(fist["shape"]["curl"].get<double>() == Approx(1.0).epsilon(0.02));
    CHECK(glm::length(pos("index_03_l") - palmSide) < glm::length(tipRest - palmSide));
    // The right hand is untouched until mirrored.
    CHECK(handInfo("hand_r")["shape"]["curl"].get<double>() < 0.4);
    run("puppeteerHandMirror", {{"args", {"hand_l"}}});
    const json right = handInfo("hand_r");
    CHECK(right["shape"]["curl"].get<double>() == Approx(1.0).epsilon(0.02));
    // Mirrored in world: the right index tip is the left one across the character's centre.
    const glm::dvec3 lt = pos("index_03_l"), rt = pos("index_03_r");
    CHECK(rt.x == Approx(-lt.x).epsilon(0.01));
    CHECK(rt.y == Approx(lt.y).epsilon(0.01));
    CHECK(rt.z == Approx(lt.z).epsilon(0.01));

    // Undo puts the right hand back.
    run("undo");
    CHECK(handInfo("hand_r")["shape"]["curl"].get<double>() < 0.4);

    // Half way from the fist to flat, the index only.
    run("puppeteerHandPose", {{"args", {"hand_l"}}, {"pose", "flat"}, {"weight", 0.5}, {"fingers", {"index"}}});
    const json half = handInfo("hand_l");
    CHECK(half["shape"]["fingers"]["index"]["curl"].get<double>() == Approx(0.5).epsilon(0.03));
    CHECK(half["shape"]["fingers"]["middle"]["curl"].get<double>() == Approx(1.0).epsilon(0.02));

    // Blend between two poses.
    run("puppeteerHandPose", {{"args", {"hand_r"}}, {"pose", "flat"}, {"blendTo", "fist"}, {"mix", 0.25}});
    CHECK(handInfo("hand_r")["shape"]["curl"].get<double>() == Approx(0.25).epsilon(0.03));

    // Keys: plain rotate keys on the finger joints.
    run("currentTime", {{"args", {0.0}}});
    run("puppeteerHandPose", {{"args", {"hand_l"}}, {"pose", "point"}, {"key", true}});
    const json times = run("keyframe", {{"args", {"index_02_l"}}, {"query", true}});
    CHECK(!times.empty());
    run("currentTime", {{"args", {1.0}}});
    run("puppeteerHandKey", {{"args", {"hand_l"}}});
    CHECK(run("keyframe", {{"args", {"pinky_03_l.rotateZ"}}, {"query", true}}).size() >= 1);
}

TEST_CASE("hands: shape values set through the command read back")
{
    mannyWithHands();
    run("puppeteerHandShape", {{"args", {"hand_l"}}, {"curl", 0.6}, {"spread", 0.4}, {"thumb", 0.5}, {"cup", 0.3}});
    const json s = handInfo("hand_l")["shape"];
    CHECK(s["curl"].get<double>() == Approx(0.6).epsilon(0.02));
    CHECK(s["spread"].get<double>() == Approx(0.4).epsilon(0.02));
    CHECK(s["thumb"].get<double>() == Approx(0.5).epsilon(0.02));
    CHECK(s["cup"].get<double>() == Approx(0.3).epsilon(0.02));
    run("puppeteerHandShape", {{"args", {"hand_l"}}, {"fingerCurl", {{"index", 0.0}}}});
    CHECK(handInfo("hand_l")["shape"]["fingers"]["index"]["curl"].get<double>() == Approx(0.0).epsilon(0.02));
    run("puppeteerHandShape", {{"args", {"manny"}}, {"side", "both"}, {"curl", 0.2}, {"add", true}});
    CHECK(handInfo("hand_r")["shape"]["curl"].get<double>() > 0.2);
}

TEST_CASE("hands: saved poses apply to any hand and undo")
{
    mannyWithHands();
    run("puppeteerHandPose", {{"args", {"hand_l"}}, {"pose", "ok"}});
    run("puppeteerHandSavePose", {{"args", {"hand_l"}}, {"name", "test ok"}, {"fingers", {"index", "thumb"}}});
    bool found = false;
    for (const json& p : run("puppeteerHandPoses"))
        found |= p["name"] == "test ok";
    CHECK(found);
    run("puppeteerHandPose", {{"args", {"hand_r"}}, {"pose", "test ok"}});
    const json r = handInfo("hand_r")["pose"], l = handInfo("hand_l")["pose"];
    CHECK(r["index"]["joints"][1][0].get<double>() == Approx(l["index"]["joints"][1][0].get<double>()).epsilon(0.01));
    run("puppeteerHandDeletePose", {{"args", {"test ok"}}});
    CHECK_THROWS(run("puppeteerHandPose", {{"args", {"hand_r"}}, {"pose", "test ok"}}));
    run("undo");
    CHECK_NOTHROW(run("puppeteerHandPose", {{"args", {"hand_r"}}, {"pose", "test ok"}}));
    run("puppeteerHandDeletePose", {{"args", {"test ok"}}});
}

TEST_CASE("hands: grip closes the fingers onto a cylinder until they touch")
{
    mannyWithHands();
    // A bar where the grip pose's fingers wrap, along the knuckles; then the hand opened flat around it.
    run("puppeteerHandPose", {{"args", {"hand_r"}}, {"pose", "grip"}});
    glm::dvec3 centre(0.0);
    for (const char* j : {"middle_01_r", "middle_02_r", "middle_03_r", "index_02_r", "ring_02_r"})
        centre += pos(j) / 5.0;
    const glm::dvec3 axis = glm::normalize(pos("index_01_r") - pos("pinky_01_r"));
    run("puppeteerHandPose", {{"args", {"hand_r"}}, {"pose", "flat"}});
    run("polyCylinder", {{"radius", 1.4}, {"height", 20.0}, {"name", "bar"}});
    // The cylinder's height runs along y: turn y onto the line of the knuckles.
    const glm::dvec3 x = glm::normalize(glm::dvec3(axis.y, -axis.x, 0.0)), z = glm::cross(x, axis);
    graph().setValue(graph().plug("bar.rotate"), lila::scene::eulerNear(glm::dmat3(x, axis, z), lila::scene::RotateOrder::XYZ, glm::dvec3(0.0)));
    graph().setValue(graph().plug("bar.translate"), centre);
    const json r = run("puppeteerHandGrip", {{"args", {"hand_r"}}, {"target", "bar"}, {"thickness", 0.8}}).at(0)["fingers"];
    for (const char* f : {"index", "middle", "ring"})
    {
        CHECK(!r[f]["startedInside"].get<bool>());
        CHECK(r[f]["touching"].get<bool>());
        CHECK(r[f]["curl"].get<double>() > 0.3);
        CHECK(r[f]["curl"].get<double>() < 1.0);
    }
    // Undo opens the hand again.
    run("undo");
    CHECK(handInfo("hand_r")["shape"]["curl"].get<double>() == Approx(0.0).epsilon(0.02));
}

TEST_CASE("hands: a finger that starts in what it grips opens out and closes back onto its surface")
{
    mannyWithHands();
    // A bar straight through the flat fingers' middle bones.
    run("puppeteerHandPose", {{"args", {"hand_r"}}, {"pose", "flat"}});
    const glm::dvec3 centre = (pos("index_02_r") + pos("middle_02_r") + pos("ring_02_r")) / 3.0;
    const glm::dvec3 axis = glm::normalize(pos("index_01_r") - pos("pinky_01_r"));
    const double radius = 1.4, thickness = 0.8;
    run("polyCylinder", {{"radius", radius}, {"height", 20.0}, {"name", "bar"}});
    const glm::dvec3 x = glm::normalize(glm::dvec3(axis.y, -axis.x, 0.0)), z = glm::cross(x, axis);
    graph().setValue(graph().plug("bar.rotate"), lila::scene::eulerNear(glm::dmat3(x, axis, z), lila::scene::RotateOrder::XYZ, glm::dvec3(0.0)));
    graph().setValue(graph().plug("bar.translate"), centre);
    auto fromAxis = [&](const std::string& j) {
        const glm::dvec3 d = pos(j) - centre;
        return glm::length(d - axis * glm::dot(d, axis));
    };
    REQUIRE(fromAxis("middle_02_r") < radius);

    const json r = run("puppeteerHandGrip", {{"args", {"hand_r"}}, {"target", "bar"}, {"thickness", thickness}}).at(0)["fingers"];
    for (const char* f : {"index", "middle", "ring"})
    {
        CAPTURE(f);
        CHECK(r[f]["startedInside"].get<bool>());
        CHECK_FALSE(r[f]["stayed"].get<bool>());
        CHECK(r[f]["touching"].get<bool>());
        // Out of the bar, and on it: the closest joint sits about a finger's thickness off its surface.
        double closest = 1e9;
        for (const char* k : {"_01_r", "_02_r", "_03_r"})
            closest = std::min(closest, fromAxis(std::string(f) + k));
        CHECK(closest > radius);
        CHECK(closest < radius + thickness + 1.0);
    }
}

TEST_CASE("hands: rubber hose fingers pose through their hose, and a plain named rig gets its hand")
{
    lila::curve::registerTypes();
    if (!lila::ops::find("curve"))
        lila::curve::registerCommands();
    mannyWithHands();
    const json made = run("hoseFingers", {{"args", {"index_01_l", "middle_01_l", "ring_01_l", "pinky_01_l"}}});
    REQUIRE(made.size() == 5);
    run("puppeteerHandSetup", {{"args", {"manny"}}});
    const json left = handInfo("hand_l");
    int hoses = 0;
    for (const json& f : left["fingers"])
        hoses += f["hose"].get<std::string>().empty() ? 0 : 1;
    CHECK(hoses == 4);
    // A partial curl moves the hose tip the way the jointed right index moves, mirrored (roughly: the hose curls about its base joint's own axis).
    const glm::dvec3 hoseRest = pos("index_03_l"), jointRest = pos("index_03_r");
    run("puppeteerHandShape", {{"args", {"manny"}}, {"side", "both"}, {"fingerCurl", {{"index", 0.3}}}});
    const glm::dvec3 hoseMove = pos("index_03_l") - hoseRest, jointMove = pos("index_03_r") - jointRest;
    CHECK(glm::dot(glm::normalize(hoseMove), glm::normalize(glm::dvec3(-jointMove.x, jointMove.y, jointMove.z))) > 0.2);
    run("puppeteerHandPose", {{"args", {"hand_l"}}, {"pose", "fist"}});
    const json fist = handInfo("hand_l")["shape"];
    CHECK(fist["fingers"]["index"]["curl"].get<double>() == Approx(1.0).epsilon(0.02));
    CHECK(attr(made[1].get<std::string>() + ".curl") > 0.1);
    run("puppeteerHandShape", {{"args", {"hand_l"}}, {"fingerCurl", {{"middle", 0.3}}}});
    CHECK(handInfo("hand_l")["shape"]["fingers"]["middle"]["curl"].get<double>() == Approx(0.3).epsilon(0.02));

    // A rig named the Mixamo way, its own character: one left hand, five fingers, end joints left alone.
    run("newScene");
    auto joint = [](const std::string& name, glm::dvec3 p) { run("joint", {{"position", v3(p.x, p.y, p.z)}, {"name", name}}); };
    joint("Hips", {0, 100, 0});
    joint("LeftArm", {15, 140, 0});
    joint("LeftForeArm", {30, 120, 0});
    joint("LeftHand", {50, 100, 0});
    const char* names[5] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
    const double zs[5] = {5, 3, 1, -1, -3};
    for (int f = 0; f < 5; ++f)
    {
        run("select", {{"args", {"LeftHand"}}});
        for (int k = 1; k <= 4; ++k)
            joint(std::string("LeftHand") + names[f] + std::to_string(k), {(f == 0 ? 52 : 58) + 2.5 * k, 100, zs[f]});
    }
    run("puppeteerCharacter", {{"args", {"Hips"}}, {"name", "mixamo"}});
    const json info = run("puppeteerHandInfo", {{"args", {"mixamo"}}});
    REQUIRE(info.size() == 1);
    CHECK(info[0]["side"] == "left");
    REQUIRE(info[0]["fingers"].size() == 5);
    CHECK(info[0]["fingers"][1]["joints"].size() == 3);
    CHECK(info[0]["fingers"][1]["tip"].get<std::string>().ends_with("LeftHandIndex4"));
    run("puppeteerHandPose", {{"args", {"mixamo"}}, {"pose", "point"}});
    CHECK(handInfo("LeftHand")["shape"]["fingers"]["middle"]["curl"].get<double>() == Approx(1.0).epsilon(0.02));
}
