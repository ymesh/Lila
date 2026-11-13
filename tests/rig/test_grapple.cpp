#include "TestUtil.h"

#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/FullBodyIk.h"
#include "scene/Grapple.h"
#include "scene/Simulation.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <string>

using namespace rigtest;
using namespace lila::scene;

namespace grappletest
{
/**
 * A stick biped under its own root joint, UE-named: pelvis at 100, three spine joints, neck, head, arms out to the
 * sides, legs down. offset moves the whole body; facing -1 turns it to face -Z. Returns the root's path.
 */
std::string biped(const std::string& rootName, glm::dvec3 offset, double facing = 1.0)
{
    auto at = [&](double x, double y, double z) { return v3(offset.x + x * facing, offset.y + y, offset.z + z * facing); };
    const std::string root = run("joint", {{"position", at(0, 0, 0)}, {"name", rootName}, {"parent", ""}, {"skipSelect", true}}).get<std::string>();
    std::map<std::string, std::string> path{{"root", graph().find(root)->path()}};
    auto j = [&](const char* name, double x, double y, double z, const char* parent) {
        const std::string made = run("joint", {{"position", at(x, y, z)}, {"name", name}, {"parent", path.at(parent)}, {"skipSelect", true}});
        path[name] = graph().find(path.at(parent) + "|" + made) ? path.at(parent) + "|" + made : made;
    };
    j("pelvis", 0, 100, 0, "root");
    j("spine_01", 0, 110, 0, "pelvis");
    j("spine_02", 0, 122, 0, "spine_01");
    j("spine_03", 0, 134, 0, "spine_02");
    j("neck_01", 0, 148, 0, "spine_03");
    j("head", 0, 160, 0, "neck_01");
    for (const char* side : {"l", "r"})
    {
        const double s = side[0] == 'l' ? 1.0 : -1.0;
        const std::string up = std::string("upperarm_") + side, low = std::string("lowerarm_") + side, hand = std::string("hand_") + side;
        const std::string thigh = std::string("thigh_") + side, calf = std::string("calf_") + side, foot = std::string("foot_") + side;
        j(up.c_str(), 18 * s, 140, 0, "spine_03");
        j(low.c_str(), 46 * s, 140, -2, up.c_str());
        j(hand.c_str(), 72 * s, 140, 0, low.c_str());
        j(thigh.c_str(), 10 * s, 95, 0, "pelvis");
        j(calf.c_str(), 10 * s, 52, 2, thigh.c_str());
        j(foot.c_str(), 10 * s, 8, 0, calf.c_str());
    }
    return path.at("root");
}

std::string under(const std::string& root, const std::string& joint)
{
    for (NodeId d : graph().descendants(graph().find(root)->id()))
        if (graph().node(d)->name() == joint)
            return graph().path(d);
    FAIL("no joint " << joint << " under " << root);
    return {};
}

void twoBipeds()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 1.0}, {"fps", 30.0}});
    biped("a_root", {0, 0, 0});
    biped("b_root", {0, 0, 70}, -1.0);
}

/** The skeleton under root as a figure at its current pose, the way the grapple builds one. */
grapple::Figure figureOf(const std::string& root, std::vector<NodeId>& joints, double mass = 70.0)
{
    Graph& g = graph();
    fbik::PoseOptions opts;
    grapple::Figure f;
    f.problem = fbik::problemFor(g, g.find(root)->id(), opts, joints);
    f.problem.bones.front().solve = false;
    f.mass = mass;
    const grapple::FigureSetup setup = grapple::fitFigure(g, g.find(root)->id());
    for (const grapple::FigureSetup::Body& b : setup.bodies)
        if (auto it = std::find(joints.begin(), joints.end(), b.joint); it != joints.end())
            f.capsules.push_back({int(it - joints.begin()), b.a, b.b, b.radius});
    return f;
}

int boneOf(const std::vector<NodeId>& joints, const std::string& path)
{
    const NodeId id = graph().find(path)->id();
    return int(std::find(joints.begin(), joints.end(), id) - joints.begin());
}

std::vector<glm::dmat4> posed(const grapple::Figure& f, const fbik::Solution& s) { return fbik::forward(f.problem, s.rotation, s.offset, s.stretch); }
std::vector<glm::dmat4> animatedOf(const grapple::Figure& f)
{
    std::vector<glm::dquat> r;
    for (const fbik::Bone& b : f.problem.bones)
        r.push_back(b.rotation);
    return fbik::forward(f.problem, r, std::vector<glm::dvec3>(r.size(), glm::dvec3(0.0)));
}

glm::dvec3 at(const glm::dmat4& m, const glm::dvec3& p = glm::dvec3(0.0)) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

struct Pair
{
    std::vector<grapple::Figure> figures;
    std::vector<NodeId> ja, jb;
    grapple::Link link;
};

/** A's right hand to a point on B's left forearm. */
Pair grabPair(double massA = 70.0, double massB = 70.0)
{
    twoBipeds();
    Pair p;
    p.figures.push_back(figureOf("a_root", p.ja, massA));
    p.figures.push_back(figureOf("b_root", p.jb, massB));
    p.link.source = 0;
    p.link.sourceBone = boneOf(p.ja, under("a_root", "hand_r"));
    p.link.target = 1;
    p.link.targetBone = boneOf(p.jb, under("b_root", "lowerarm_l"));
    p.link.targetPoint = glm::dvec3(8.0, 0.0, 0.0);
    p.link.share = grapple::shareByMass(massA, massB);
    return p;
}
} // namespace grappletest

using namespace grappletest;

TEST_CASE("grapple: segment closest points and ray capsules")
{
    const auto [p, q] = grapple::closestPoints({0, 0, 0}, {10, 0, 0}, {5, 5, -5}, {5, 5, 5});
    CHECK(glm::length(p - glm::dvec3(5, 0, 0)) < 1e-9);
    CHECK(glm::length(q - glm::dvec3(5, 5, 0)) < 1e-9);
    const auto t = grapple::rayCapsule({5, 20, 0}, {0, -1, 0}, {0, 0, 0}, {10, 0, 0}, 2.0);
    REQUIRE(t);
    CHECK(*t == doctest::Approx(18.0));
    CHECK_FALSE(grapple::rayCapsule({5, 20, 10}, {0, -1, 0}, {0, 0, 0}, {10, 0, 0}, 2.0));
    CHECK(grapple::shareByMass(100.0, 100.0) == doctest::Approx(0.5));
    CHECK(grapple::shareByMass(150.0, 50.0) == doctest::Approx(0.75));
}

TEST_CASE("grapple: two characters converge on a contact, each closing its share")
{
    for (const auto [ma, mb] : {std::pair{70.0, 70.0}, std::pair{210.0, 70.0}})
    {
        Pair p = grabPair(ma, mb);
        grapple::Options o;
        o.autoPose = false;
        o.collide = false;
        const grapple::Result r = grapple::solve(p.figures, {p.link}, o);
        CHECK(r.contactError < 0.05);
        const auto a0 = animatedOf(p.figures[0]), b0 = animatedOf(p.figures[1]);
        const auto a1 = posed(p.figures[0], r.poses[0]), b1 = posed(p.figures[1], r.poses[1]);
        const glm::dvec3 hand0 = at(a0[size_t(p.link.sourceBone)]), hand1 = at(a1[size_t(p.link.sourceBone)]);
        const glm::dvec3 arm0 = at(b0[size_t(p.link.targetBone)], p.link.targetPoint), arm1 = at(b1[size_t(p.link.targetBone)], p.link.targetPoint);
        CHECK(glm::length(hand1 - arm1) < 0.05);
        const double gap = glm::length(arm0 - hand0);
        REQUIRE(gap > 20.0);
        // The heavier side moves less: each closes its share of the gap.
        const double handMoved = glm::length(hand1 - hand0), armMoved = glm::length(arm1 - arm0);
        const double share = grapple::shareByMass(ma, mb);
        CHECK(armMoved / (handMoved + armMoved) == doctest::Approx(share).epsilon(0.08));
    }
}

TEST_CASE("grapple: lead and weight decide who moves and how far")
{
    Pair p = grabPair();
    grapple::Options o;
    o.autoPose = false;
    o.collide = false;
    const auto a0 = animatedOf(p.figures[0]), b0 = animatedOf(p.figures[1]);
    const glm::dvec3 hand0 = at(a0[size_t(p.link.sourceBone)]), arm0 = at(b0[size_t(p.link.targetBone)], p.link.targetPoint);

    SUBCASE("the source leads: it stays, the target is brought to it")
    {
        p.link.share = 1.0;
        const grapple::Result r = grapple::solve(p.figures, {p.link}, o);
        CHECK(r.contactError < 0.05);
        CHECK(glm::length(at(posed(p.figures[0], r.poses[0])[size_t(p.link.sourceBone)]) - hand0) < 0.05);
    }
    SUBCASE("half weight closes half the gap")
    {
        p.link.weight = 0.5;
        const grapple::Result r = grapple::solve(p.figures, {p.link}, o);
        const glm::dvec3 hand = at(posed(p.figures[0], r.poses[0])[size_t(p.link.sourceBone)]);
        const glm::dvec3 arm = at(posed(p.figures[1], r.poses[1])[size_t(p.link.targetBone)], p.link.targetPoint);
        CHECK(glm::length(arm - hand) == doctest::Approx(glm::length(arm0 - hand0) * 0.5).epsilon(0.03));
    }
    SUBCASE("weight 0 leaves both exactly as animated")
    {
        p.link.weight = 0.0;
        const grapple::Result r = grapple::solve(p.figures, {p.link}, o);
        for (size_t i = 0; i < 2; ++i)
        {
            const auto anim = animatedOf(p.figures[i]), now = posed(p.figures[i], r.poses[i]);
            for (size_t k = 0; k < anim.size(); ++k)
                CHECK(glm::length(at(anim[k]) - at(now[k])) < 1e-9);
        }
    }
}

TEST_CASE("grapple: a held rotation is met against the target")
{
    Pair p = grabPair();
    p.link.rotation = glm::angleAxis(glm::radians(60.0), glm::normalize(glm::dvec3(0.3, 1.0, 0.2)));
    grapple::Options o;
    o.autoPose = false;
    o.collide = false;
    const grapple::Result r = grapple::solve(p.figures, {p.link}, o);
    const auto a1 = posed(p.figures[0], r.poses[0]), b1 = posed(p.figures[1], r.poses[1]);
    auto rot = [](const glm::dmat4& m) { return glm::normalize(glm::quat_cast(glm::dmat3(m))); };
    const glm::dquat rel = glm::conjugate(rot(b1[size_t(p.link.targetBone)])) * rot(a1[size_t(p.link.sourceBone)]);
    CHECK(std::abs(glm::dot(rel, *p.link.rotation)) > std::cos(glm::radians(2.0) / 2.0));
    CHECK(r.contactError < 0.1);
}

TEST_CASE("grapple: blending a contact in has no pops")
{
    Pair p = grabPair();
    grapple::Options o;
    o.collide = false;
    o.autoPose = false;
    std::vector<std::vector<glm::dmat4>> poses;
    for (int step = 0; step <= 50; ++step)
    {
        grapple::Link l = p.link;
        l.weight = double(step) / 50.0;
        const grapple::Result r = grapple::solve(p.figures, {l}, o);
        poses.push_back(posed(p.figures[0], r.poses[0]));
    }
    const double gap = glm::length(at(animatedOf(p.figures[1])[size_t(p.link.targetBone)], p.link.targetPoint) -
                                   at(animatedOf(p.figures[0])[size_t(p.link.sourceBone)]));
    // Every joint moves smoothly with the weight: no step is far beyond an even share of the whole motion.
    double worst = 0.0;
    for (size_t s = 1; s < poses.size(); ++s)
        for (size_t k = 0; k < poses[s].size(); ++k)
            worst = std::max(worst, glm::length(at(poses[s][k]) - at(poses[s - 1][k])));
    CHECK(worst < gap / 50.0 * 3.0);
}

TEST_CASE("grapple: characters are kept from passing through each other")
{
    twoBipeds();
    // B stands where A is, half a body over: their torsos overlap.
    run("setAttr", {{"args", {"b_root.translate", v3(12, 0, 6)}}});
    Pair p;
    p.figures.push_back(figureOf("a_root", p.ja));
    p.figures.push_back(figureOf("b_root", p.jb));
    for (auto& f : p.figures)
        f.problem.bones[1].translate = true;
    grapple::Options o;
    o.autoPose = false;
    o.iterations = 16;
    o.margin = 0.0;
    // One round with nothing to solve only measures the overlap as animated.
    grapple::Options measure = o;
    measure.iterations = 1;
    const grapple::Result first = grapple::solve(p.figures, {}, measure);
    const grapple::Result r = grapple::solve(p.figures, {}, o);
    CHECK(r.penetration < first.penetration * 0.25);
    CHECK(r.penetration < 1.0);
}

TEST_CASE("grapple: contacts bake to keys that follow the other character and blend in without a pop")
{
    twoBipeds();
    Graph& g = graph();
    const std::string hand = under("a_root", "hand_r"), arm = under("b_root", "lowerarm_l");
    // B sways side to side; the contact must ride it.
    key("b_root.tx", 0.0, 0.0);
    key("b_root.tx", 0.5, 15.0);
    key("b_root.tx", 1.0, 0.0);
    const json made = run("grappleContact", {{"args", {hand, arm}}, {"kind", "hold"}, {"time", 10.0 / 30.0}, {"blend", 6.0 / 30.0}});
    const std::string contact = made["contact"].get<std::string>(), gr = made["grapple"].get<std::string>();
    CHECK(made["fitted"].size() == 2);
    const json baked = run("bakeGrapple", {{"args", {gr}}});
    CHECK(baked["contactError"].get<double>() < 0.1);
    const std::string layer = baked["layer"].get<std::string>();
    REQUIRE(g.find(layer));

    std::vector<glm::dvec3> handAt;
    double worstHeld = 0.0;
    for (int f = 0; f <= 30; ++f)
    {
        g.setTime(f / 30.0);
        g.evaluate();
        handAt.push_back(pos(hand));
        if (f >= 10)
        {
            const auto [s, t] = grapple::contactPoints(g, g.find(contact)->id());
            worstHeld = std::max(worstHeld, glm::length(s - t));
        }
    }
    CHECK(worstHeld < 0.5);
    // Before the blend the hand is where the animation has it, unmoved by the layer.
    run("setAttr", {{"args", {layer + ".mute", true}}});
    g.setTime(2.0 / 30.0);
    g.evaluate();
    const glm::dvec3 free = pos(hand);
    run("setAttr", {{"args", {layer + ".mute", false}}});
    CHECK(glm::length(handAt[2] - free) < 1e-3);
    // No pop: the hand's frame-to-frame step across the blend stays near the step it averages over it.
    const double travel = glm::length(handAt[10] - handAt[4]);
    for (int f = 1; f <= 30; ++f)
        CHECK(glm::length(handAt[size_t(f)] - handAt[size_t(f - 1)]) < travel / 6.0 * 2.5 + 2.0);

    // Baking again replaces the layer; undo brings the first back.
    const json again = run("bakeGrapple", {{"args", {gr}}});
    CHECK(again["layer"].get<std::string>() == layer);
    CHECK(anim::layers(g).size() == 1);
}

TEST_CASE("grapple: commands key contacts, set figures and report")
{
    twoBipeds();
    const std::string hand = under("a_root", "hand_l"), neck = under("b_root", "neck_01");
    const json made = run("grappleContact", {{"args", {hand, neck}}, {"kind", "lift"}, {"blend", 0.0}});
    const std::string contact = made["contact"].get<std::string>();
    CHECK(made["lead"] == "source");
    json info = run("grappleInfo", {{"args", {contact}}});
    REQUIRE(info["figures"].size() == 2);
    // Lift frees the lifted character's feet.
    for (const json& f : info["figures"])
        CHECK(f["plant"].get<bool>() == (f["root"].get<std::string>().find("a_root") != std::string::npos));
    CHECK(info["contacts"].size() == 1);
    CHECK(info["contactError"].get<double>() < 0.1);

    run("grappleFigure", {{"args", {under("a_root", "pelvis")}}, {"mass", 120.0}});
    info = run("grappleInfo", {{"args", {contact}}});
    CHECK(info["figures"][0]["mass"].get<double>() == doctest::Approx(120.0));

    run("grappleKey", {{"args", {contact}}, {"on", false}, {"time", 0.5}, {"blend", 0.1}});
    Graph& g = graph();
    g.setTime(0.5);
    g.evaluate();
    CHECK(g.find(contact)->as<double>(g.find(contact)->type().attrIndex("weight")) == doctest::Approx(0.0));
    g.setTime(0.3);
    g.evaluate();
    CHECK(g.find(contact)->as<double>(g.find(contact)->type().attrIndex("weight")) == doctest::Approx(1.0));
}

TEST_CASE("grapple: every weight converges with the feet planted and the bodies colliding")
{
    Pair p = grabPair();
    for (auto* f : {&p.figures[0], &p.figures[1]})
        for (size_t k = 0; k < f->problem.bones.size(); ++k)
            if (graph().node((f == &p.figures[0] ? p.ja : p.jb)[k])->name().find("foot") != std::string::npos)
                f->holds.push_back(int(k));
    grapple::Options o;
    o.autoPose = false;
    for (double w : {0.01, 0.07, 0.2, 0.5, 1.0})
        for (bool collide : {false, true})
        {
            grapple::Link l = p.link;
            l.weight = w;
            o.collide = collide;
            const grapple::Result r = grapple::solve(p.figures, {l}, o);
            CHECK(r.contactError < 0.05);
            CHECK(r.iterations <= 3);
        }
}

TEST_CASE("grapple: an aim turns the held axis onto the target in every style")
{
    grapple::Aim a;
    a.holder = glm::translate(glm::dmat4(1.0), glm::dvec3(30.0, 120.0, 20.0));
    // A barrel 6 cm above the pivot, pointing +Z.
    a.axis = glm::translate(glm::dmat4(1.0), glm::dvec3(30.0, 126.0, 60.0)) * glm::dmat4(glm::mat3_cast(glm::angleAxis(-glm::half_pi<double>(), glm::dvec3(0, 1, 0))));
    a.pivot = glm::dvec3(30.0, 120.0, 0.0);
    a.target = glm::dvec3(-200.0, 180.0, 600.0);
    a.anchor = glm::dvec3(18.0, 140.0, 0.0);
    a.reach = 55.0;
    a.drop = 10.0;
    for (grapple::AimStyle s : {grapple::AimStyle::Turn, grapple::AimStyle::Shoulder, grapple::AimStyle::Extend, grapple::AimStyle::Guard})
    {
        a.style = s;
        const glm::dmat4 h = grapple::aimHolder(a);
        CHECK(grapple::aimError(a, h) < 1e-3);
        CHECK(grapple::aimError(a, a.holder) > 10.0);
        const glm::dvec3 pivot = at(h * glm::inverse(a.holder), a.pivot);
        if (s == grapple::AimStyle::Turn)
            CHECK(glm::length(pivot - a.pivot) < 1e-9);
        if (s == grapple::AimStyle::Shoulder)
            CHECK(glm::length(pivot - a.anchor) < 1e-9);
        if (s == grapple::AimStyle::Extend)
            CHECK(glm::length(pivot - a.anchor) == doctest::Approx(55.0));
        if (s == grapple::AimStyle::Guard)
            CHECK(pivot.y < a.anchor.y + (a.target.y - a.anchor.y) * 55.0 / glm::length(a.target - a.anchor) - 9.0);
    }
}

TEST_CASE("grapple: a figure's own effectors solve it with no contact")
{
    twoBipeds();
    std::vector<NodeId> ja;
    grapple::Figure f = figureOf("a_root", ja);
    const int hand = boneOf(ja, under("a_root", "hand_r"));
    const glm::dvec3 was = at(animatedOf(f)[size_t(hand)]);
    const glm::dvec3 want = was + glm::dvec3(5.0, 8.0, 12.0);
    f.problem.effectors.push_back({.bone = hand, .position = want});
    grapple::Options o;
    o.collide = false;
    o.autoPose = false;
    const grapple::Result r = grapple::solve({f}, {}, o);
    REQUIRE(r.poses.size() == 1);
    CHECK(glm::length(at(posed(f, r.poses[0])[size_t(hand)]) - want) < 0.05);
}

TEST_CASE("grapple: a frame contact holds the source's grip on a moving transform riding the target")
{
    twoBipeds();
    Graph& g = graph();
    const std::string handR = under("a_root", "hand_r"), handL = under("a_root", "hand_l");
    // A point riding the right hand 20 cm in front of it; the left hand's grip held there.
    run("spaceLocator", {{"name", "grip"}});
    run("parent", {{"args", {"grip", handR}}});
    const std::string gripPath = g.path(g.find("grip")->id());
    run("setAttr", {{"args", {gripPath + ".translate", 0.0, 0.0, 20.0}}});
    const std::string grapple = run("grappleAim", {{"args", {handR, gripPath}}, {"axis", gripPath}, {"blend", 0.0}}).at("grapple");
    // Aims are not what this checks: switch it off and make the contact by hand.
    for (NodeId a : grapple::aimsOf(g, g.find(grapple)->id()))
        run("setAttr", {{"args", {g.path(a) + ".enabled", false}}});
    const std::string c = run("grappleContact", {{"args", {handL, handR}}, {"blend", 0.0}, {"grapple", grapple}}).at("contact");
    run("setAttr", {{"args", {c + ".frame", gripPath}}});
    const glm::dmat4 offset = glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, -3.0, 0.0));
    g.setValue(g.plug(c + ".gripOffset"), offset);
    run("bakeGrapple", {{"args", {grapple}}});
    g.evaluate();
    const glm::dmat4 left = world(handL);
    const glm::dvec3 gripAt = at(left, glm::dvec3(offset[3]));
    CHECK(glm::length(gripAt - pos(gripPath)) < 0.5);
    CHECK(grapple::contactFollowsFrame(g, g.find(c)->id()));
    // Both arms shared the reach: the right hand came toward the left too.
    CHECK(pos(handR).x > -60.0);
}
