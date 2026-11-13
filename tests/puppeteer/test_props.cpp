#include "TestUtil.h"
#include "prop/PropMath.h"

#include "scene/Anim.h"
#include "scene/Grapple.h"
#include "scene/Pin.h"

#include <glm/gtc/matrix_transform.hpp>

#include <functional>

using namespace puppeteer::prop;
using namespace puptest;
using Approx = doctest::Approx;

namespace proptest
{
/** Points over a box's surface: corners, edge and face samples, as a mesh's points. */
void box(std::vector<glm::dvec3>& out, glm::dvec3 centre, glm::dvec3 size)
{
    // About a centimetre apart, as a prop's surface is sampled.
    const glm::ivec3 n = glm::max(glm::ivec3(glm::ceil(size)), glm::ivec3(4));
    for (int i = 0; i <= n.x; ++i)
        for (int j = 0; j <= n.y; ++j)
            for (int k = 0; k <= n.z; ++k)
                if (i == 0 || i == n.x || j == 0 || j == n.y || k == 0 || k == n.z)
                    out.push_back(centre + size * (glm::dvec3(i, j, k) / glm::dvec3(n) - 0.5));
}

/** A sword along +Y: pommel at 0, handle to 20, guard at 21, blade to 100. */
std::vector<glm::dvec3> swordPoints(double handle = 20.0)
{
    std::vector<glm::dvec3> p;
    box(p, {0, handle / 2, 0}, {2.6, handle, 2.6});
    box(p, {0, handle + 1, 0}, {18, 2, 3});
    box(p, {0, handle + 2 + 40, 0}, {5, 80, 0.8});
    return p;
}

/** A pistol along +Z: slide at the top, grip below toward the back. */
std::vector<glm::dvec3> pistolPoints()
{
    std::vector<glm::dvec3> p;
    box(p, {0, 0, 0}, {3, 3.5, 19});
    box(p, {0, -7, -6}, {2.8, 11, 4.5});
    return p;
}

/** A rifle along +Z: stock at the back, pistol grip, magazine, barrel to the front. */
std::vector<glm::dvec3> riflePoints()
{
    std::vector<glm::dvec3> p;
    box(p, {0, 0, 0}, {4, 7, 60});
    box(p, {0, 1, 45}, {2, 2, 30});
    box(p, {0, -3, -40}, {4, 12, 22});
    box(p, {0, -8, -16}, {3, 10, 4});
    box(p, {0, -10, 2}, {3, 14, 5});
    return p;
}

const PointDef* find(const Fit& f, Role r)
{
    for (const PointDef& d : f.points)
        if (d.role == r)
            return &d;
    return nullptr;
}
glm::dvec3 origin(const PointDef& d) { return glm::dvec3(d.frame[3]); }
glm::dvec3 xAxis(const PointDef& d) { return glm::dvec3(d.frame[0]); }

// Scene helpers -------------------------------------------------------------------------------------------------------

void manny()
{
    auto& em = lila::extensions::ExtensionManager::get();
    if (!em.isLoaded("RigTemplates"))
        REQUIRE(em.load(LILA_TEST_RIG_TEMPLATES_PACKAGE) == "RigTemplates");
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 10.0 / 30.0}, {"fps", 30.0}});
    run("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"rig", false}});
    run("puppeteerCharacter", {{"args", {"root"}}, {"template", "UE5_Manny"}, {"name", "manny"}});
}

std::string cube(const std::string& name, glm::dvec3 at, glm::dvec3 size)
{
    run("polyCube", {{"name", name}, {"width", size.x}, {"height", size.y}, {"depth", size.z}, {"position", v3(at.x, at.y, at.z)}});
    return name;
}

/** A prop mesh group made of boxes, far from the character. */
std::string group(const std::string& name, const std::vector<std::string>& parts)
{
    json args = json::array();
    for (const std::string& p : parts)
        args.push_back(p);
    run("group", {{"args", args}, {"name", name}});
    return name;
}

std::string sword()
{
    const glm::dvec3 o(200, 0, 0);
    return group("sword", {cube("sword_handle", o + glm::dvec3(0, 12, 0), {2.6, 24, 2.6}), cube("sword_guard", o + glm::dvec3(0, 25, 0), {18, 2, 3}),
                           cube("sword_blade", o + glm::dvec3(0, 66, 0), {5, 80, 0.8})});
}
std::string pistol()
{
    const glm::dvec3 o(-200, 100, 0);
    return group("pistol", {cube("pistol_slide", o, {3, 3.5, 19}), cube("pistol_grip", o + glm::dvec3(0, -7, -6), {2.8, 11, 4.5})});
}
std::string rifle()
{
    const glm::dvec3 o(0, 100, 200);
    return group("rifle", {cube("rifle_body", o, {4, 7, 60}), cube("rifle_barrel", o + glm::dvec3(0, 1, 45), {2, 2, 30}),
                           cube("rifle_stock", o + glm::dvec3(0, -3, -40), {4, 12, 22}), cube("rifle_pgrip", o + glm::dvec3(0, -8, -16), {3, 10, 4}),
                           cube("rifle_mag", o + glm::dvec3(0, -10, 2), {3, 14, 5})});
}

std::string pointNamed(const json& prop, const std::string& role)
{
    for (const json& p : prop.at("points"))
        if (p.at("role") == role)
            return p.at("point");
    FAIL("no " << role << " point");
    return {};
}

std::string jointPath(const std::string& bare)
{
    for (scene::NodeId id : graph().nodes())
        if (graph().node(id)->name() == bare)
            return graph().path(id);
    FAIL("no joint " << bare);
    return {};
}

void at(double seconds)
{
    graph().setTime(seconds, false);
    graph().evaluate();
}
} // namespace proptest

using namespace proptest;

TEST_CASE("props: a sword's handle, guard and tip are found from its mesh")
{
    for (const bool flip : {false, true})
    {
        std::vector<glm::dvec3> pts = swordPoints();
        if (flip)
            for (glm::dvec3& p : pts)
                p = glm::dvec3(p.x, 100.0 - p.y, p.z);
        const Fit f = fitPoints(pts, {0, 1, 0}, {Shape::Blade, true});
        CHECK(f.from == "mesh");
        const PointDef *main = find(f, Role::Main), *off = find(f, Role::Off), *tip = find(f, Role::Tip);
        REQUIRE(main);
        REQUIRE(off);
        REQUIRE(tip);
        const double dir = flip ? -1.0 : 1.0;
        // Along the blade toward the tip, both grips on the handle, main by the guard.
        CHECK(glm::dot(xAxis(*main), glm::dvec3(0, dir, 0)) > 0.99);
        const double mainY = flip ? 100.0 - origin(*main).y : origin(*main).y, offY = flip ? 100.0 - origin(*off).y : origin(*off).y;
        CHECK(mainY > 10.0);
        CHECK(mainY < 20.0);
        CHECK(offY < mainY - 5.0);
        CHECK(offY > 0.0);
        CHECK((flip ? 100.0 - origin(*tip).y : origin(*tip).y) > 95.0);
        CHECK(main->radius == Approx(1.3 * std::sqrt(2.0)).epsilon(0.05));
    }
    // One-handed: the pommel is an alternate grip, not an off hand.
    const Fit one = fitPoints(swordPoints(), {0, 1, 0}, {Shape::Blade, false});
    CHECK(find(one, Role::Off) == nullptr);
    CHECK(find(one, Role::Alt) != nullptr);
}

TEST_CASE("props: a pistol's grip hangs below the slide and its muzzle points forward")
{
    const Fit f = fitPoints(pistolPoints(), {0, 1, 0}, {Shape::Firearm});
    const PointDef *main = find(f, Role::Main), *muzzle = find(f, Role::Muzzle);
    REQUIRE(main);
    REQUIRE(muzzle);
    CHECK(origin(*main).y < -4.0);
    CHECK(origin(*main).z < -3.0);
    // The handle runs up toward the slide; the palm faces forward.
    CHECK(xAxis(*main).y > 0.9);
    CHECK(glm::dvec3(main->frame[2]).z > 0.9);
    CHECK(origin(*muzzle).z == Approx(9.5).epsilon(0.02));
    CHECK(glm::dot(xAxis(*muzzle), glm::dvec3(0, 0, 1)) > 0.999);
}

TEST_CASE("props: a rifle has its stock behind, the off grip ahead of the main and the muzzle in front")
{
    const Fit f = fitPoints(riflePoints(), {0, 1, 0}, {Shape::Firearm});
    const PointDef *main = find(f, Role::Main), *off = find(f, Role::Off), *stock = find(f, Role::Stock), *muzzle = find(f, Role::Muzzle);
    REQUIRE(main);
    REQUIRE(off);
    REQUIRE(stock);
    REQUIRE(muzzle);
    CHECK(origin(*stock).z < -45.0);
    CHECK(origin(*main).z < -5.0);
    CHECK(origin(*main).z > -30.0);
    CHECK(origin(*off).z > origin(*main).z + 15.0);
    CHECK(origin(*muzzle).z > 55.0);
    CHECK(glm::dot(xAxis(*muzzle), glm::dvec3(0, 0, 1)) > 0.999);
    // Palm up under the handguard.
    CHECK(glm::dvec3(off->frame[2]).y > 0.9);
}

TEST_CASE("props: no mesh still gives points, and says so")
{
    const Fit f = fitPoints({}, {0, 1, 0}, {Shape::Firearm, true});
    CHECK(f.from != "mesh");
    CHECK(find(f, Role::Main) != nullptr);
    CHECK(find(f, Role::Off) != nullptr);
}

TEST_CASE("props: a grip frame sits the same in mirrored hands")
{
    HandShape r;
    r.forward = {1, 0, 0};
    r.thumbSide = {0, 0, 1};
    r.palm = {0, -1, 0};
    r.knuckles = {9, 0, 0};
    HandShape l = r;
    // The left hand is the right mirrored across x = 0 (in its own joint's space the thumb flips side).
    l.thumbSide = {0, 0, -1};
    const glm::dmat4 gr = gripFrame(r, 1.5), gl = gripFrame(l, 1.5);
    CHECK(glm::length(glm::dvec3(gr[3]) - glm::dvec3(9 * 0.85, -(0.25 * 9 + 1.5), 0)) < 1e-9);
    CHECK(glm::length(glm::dvec3(gr[3]) - glm::dvec3(gl[3])) < 1e-9);
    // Thumb toward +X of the frame, palm toward +Z, for both.
    CHECK(glm::dvec3(gr[0]) == glm::dvec3(0, 0, 1));
    CHECK(glm::dvec3(gl[0]) == glm::dvec3(0, 0, -1));
    CHECK(glm::dvec3(gr[2]) == glm::dvec3(0, -1, 0));
    CHECK(glm::dvec3(gl[2]) == glm::dvec3(0, -1, 0));
    CHECK(glm::determinant(glm::dmat3(gr)) == Approx(1.0));
    CHECK(glm::determinant(glm::dmat3(gl)) == Approx(1.0));
}

TEST_CASE("props: presets resolve by id and by common names")
{
    REQUIRE(presetFor("rifle"));
    CHECK(std::string(presetFor("rifle")->id) == "rifle");
    CHECK(std::string(presetFor("bazooka")->id) == "launcher");
    CHECK(std::string(presetFor("greatsword")->id) == "sword2h");
    CHECK(!presetFor("toaster"));
    for (const Preset& p : presets())
    {
        CHECK(presetFor(p.id) == &p);
        REQUIRE(matchingPreset(p.settings));
        CHECK(matchingPreset(p.settings)->settings == p.settings);
    }
}

/** A cylinder-ish column of boxes along +Y: radius r from y0 to y1, as a mesh's points. */
void column(std::vector<glm::dvec3>& out, double y0, double y1, double r)
{
    box(out, {0, 0.5 * (y0 + y1), 0}, {2 * r, y1 - y0, 2 * r});
}

TEST_CASE("props: the shape is read from the points alone")
{
    CHECK(classify(swordPoints()) == Shape::Blade);
    CHECK(classify(pistolPoints()) == Shape::Firearm);
    CHECK(classify(riflePoints()) == Shape::Firearm);
    // A bottle: a narrow neck on a wide body.
    std::vector<glm::dvec3> bottle;
    column(bottle, 0, 22, 4.0);
    column(bottle, 22, 32, 1.3);
    CHECK(classify(bottle) == Shape::Haft);
    // An axe: its head hangs off one side at one end, which is not a firearm's grip.
    std::vector<glm::dvec3> axe;
    column(axe, 0, 70, 1.6);
    box(axe, {6, 64, 0}, {14, 12, 2});
    CHECK(classify(axe) == Shape::Haft);
    // A staff, a short chair leg, a shield, a box.
    std::vector<glm::dvec3> staff, leg, shield, crate;
    column(staff, 0, 160, 1.8);
    column(leg, 0, 45, 1.8);
    box(shield, {0, 0, 0}, {60, 80, 4});
    box(crate, {0, 0, 0}, {20, 18, 16});
    CHECK(classify(staff) == Shape::Pole);
    CHECK(classify(leg) == Shape::Other);
    CHECK(classify(shield) == Shape::Plate);
    CHECK(classify(crate) == Shape::Other);
    // A bow: a long limb bent away from its tips.
    std::vector<glm::dvec3> bow;
    for (int i = 0; i <= 20; ++i)
    {
        const double y = -60.0 + 6.0 * i, z = 18.0 * (1.0 - (y / 60.0) * (y / 60.0));
        box(bow, {0, y, z}, {2.5, 6.5, 2.5});
    }
    CHECK(classify(bow) == Shape::Bow);
}

TEST_CASE("props: an unknown shape still gets grips, a tip and settings that follow from it")
{
    // A bottle held by its neck, a second grip on its body with its own radius.
    std::vector<glm::dvec3> bottle;
    column(bottle, 0, 22, 4.0);
    column(bottle, 22, 32, 1.3);
    const Fit f = fitPoints(bottle);
    CHECK(f.shape == Shape::Haft);
    const PointDef *main = find(f, Role::Main), *alt = find(f, Role::Alt), *tip = find(f, Role::Tip);
    REQUIRE(main);
    REQUIRE(alt);
    REQUIRE(tip);
    CHECK(origin(*main).y > 22.0);
    CHECK(main->radius < 2.5);
    CHECK(alt->radius > 4.0);
    CHECK(origin(*tip).y < 1.0);
    CHECK_FALSE(f.suggested.twoHanded);
    // A long gun read from its shape: two hands, shouldered, trigger finger.
    const Fit r = fitPoints(riflePoints());
    CHECK(r.shape == Shape::Firearm);
    CHECK(r.suggested.twoHanded);
    CHECK(r.suggested.aim == AimStyle::Shoulder);
    CHECK(r.suggested.trigger);
    CHECK(find(r, Role::Stock) != nullptr);
    // A pistol: one hand, held out.
    const Fit pf = fitPoints(pistolPoints());
    CHECK_FALSE(pf.suggested.twoHanded);
    CHECK(pf.suggested.aim == AimStyle::Extend);
    // A staff: two hands, guard.
    std::vector<glm::dvec3> staff;
    column(staff, 0, 160, 1.8);
    const Fit sf = fitPoints(staff);
    CHECK(sf.suggested.twoHanded);
    CHECK(find(sf, Role::Off) != nullptr);
}

// On Manny ------------------------------------------------------------------------------------------------------------

TEST_CASE("props: a greatsword held in both hands, the arms sharing the reach")
{
    manny();
    sword();
    const json p = run("puppeteerPropSetup", {{"args", {"sword"}}, {"preset", "sword2h"}});
    CHECK(p.at("twoHanded") == true);
    const std::string mainPt = pointNamed(p, "main"), offPt = pointNamed(p, "off");
    const std::string handR = jointPath("hand_r"), handL = jointPath("hand_l");
    const glm::dvec3 rBefore = pos(handR), lBefore = pos(handL);
    const json h = run("puppeteerPropHold", {{"args", {"sword"}}, {"time", 0.0}, {"blend", 0.0}});
    CHECK(h.at("main") == handR);
    CHECK(h.at("off") == handL);
    REQUIRE(h.at("solve").is_object());
    CHECK(h.at("solve").at("contactError").get<double>() < 0.5);
    at(5.0 / 30.0);
    // The sword rides the right hand: its main grip is where the hand's grip frame is.
    const glm::dmat4 hr = world(handR);
    const glm::dvec3 mainInHand = glm::dvec3(glm::inverse(hr) * glm::dvec4(pos(mainPt), 1.0));
    CHECK(glm::length(mainInHand) > 3.0);
    CHECK(glm::length(mainInHand) < 15.0);
    // The left hand reached its grip, and both hands moved to meet (neither arm did it all).
    const json info = run("grappleInfo", {{"args", {"props_grapple"}}});
    CHECK(info.at("contactError").get<double>() < 0.5);
    const double movedR = glm::length(pos(handR) - rBefore), movedL = glm::length(pos(handL) - lBefore);
    CHECK(movedR > 2.0);
    CHECK(std::abs(glm::length(pos(offPt) - pos(handL)) - glm::length(pos(mainPt) - pos(handR))) < 1.0);
    CHECK(movedL > 2.0);
    // Holding it with one hand lets the left go.
    run("puppeteerPropHold", {{"args", {"sword"}}, {"hands", 1}, {"time", 5.0 / 30.0}, {"blend", 3.0 / 30.0}});
    at(10.0 / 30.0);
    CHECK(run("puppeteerPropInfo", {{"args", {"sword"}}}).at("holder") == handR);
    const json listed367 = run("grappleInfo", {{"args", {"props_grapple"}}});
    for (const json& c : listed367.at("contacts"))
        CHECK(c.at("weight").get<double>() == Approx(0.0));
}

TEST_CASE("props: handing a pistol from the right hand to the left never pops")
{
    manny();
    pistol();
    run("puppeteerPropSetup", {{"args", {"pistol"}}, {"preset", "pistol"}});
    run("puppeteerPropHold", {{"args", {"pistol"}}, {"time", 0.0}, {"blend", 0.0}, {"solve", false}});
    // The right hand lifts toward the left between frames 0 and 4, then the pistol changes hands at frame 4.
    const std::string handR = jointPath("hand_r"), handL = jointPath("hand_l");
    const json held = run("puppeteerPropHold", {{"args", {"pistol"}}, {"hand", "left"}, {"time", 4.0 / 30.0}, {"blend", 4.0 / 30.0}, {"solve", false}});
    CHECK(held.at("main") == handL);
    // The group's origin is far from its meshes, so its grip is what is followed.
    const std::string grip = pointNamed(held, "main");
    std::vector<glm::dvec3> path;
    for (int f = 0; f <= 10; ++f)
    {
        at(f / 30.0);
        path.push_back(pos(grip));
    }
    // At the switch the pistol is where it was; it then travels to the left hand over the blend.
    CHECK(glm::length(path[4] - path[3]) < 1e-6);
    const double travel = glm::length(path[8] - path[4]);
    CHECK(travel > 10.0);
    double worst = 0.0;
    for (size_t f = 4; f < 8; ++f)
        worst = std::max(worst, glm::length(path[f + 1] - path[f]));
    // An eased move: no step much over an even share of the way.
    CHECK(worst < travel * 0.6);
    at(8.0 / 30.0);
    CHECK(run("puppeteerPropInfo", {{"args", {"pistol"}}}).at("holder") == handL);
    const json holds = run("puppeteerPropInfo", {{"args", {"pistol"}}}).at("holds");
    REQUIRE(holds.size() >= 3);
    CHECK(holds.back().at("side") == "left");
    // The time slider and the Stacker see the handoff as keys on the pistol's space channel.
    const scene::NodeId curve = scene::anim::curveFor(graph(), {id("pistol"), scene::Transform::kSpace, -1});
    CHECK(curve != scene::kNoNode);
    // Holstered at frame 8, it settles on the thigh.
    const json hol = run("puppeteerPropHolster", {{"args", {"pistol"}}, {"time", 8.0 / 30.0}, {"blend", 2.0 / 30.0}, {"solve", false}});
    at(10.0 / 30.0);
    CHECK(glm::length(pos(pointNamed(hol, "main")) - pos(hol.at("holster"))) < 1e-3);
}

TEST_CASE("props: a rifle aimed at a target, shouldered, the body following")
{
    manny();
    rifle();
    run("puppeteerPropSetup", {{"args", {"rifle"}}, {"preset", "rifle"}});
    run("puppeteerPropHold", {{"args", {"rifle"}}, {"time", 0.0}, {"blend", 0.0}, {"solve", false}});
    run("spaceLocator", {{"name", "enemy"}});
    run("setAttr", {{"args", {"enemy.translate", 150.0, 140.0, 600.0}}});
    const json aimed = run("puppeteerPropAim", {{"args", {"rifle"}}, {"target", "enemy"}, {"time", 0.0}, {"blend", 0.0}});
    CHECK(aimed.at("style") == "shoulder");
    at(5.0 / 30.0);
    const json p = run("puppeteerPropInfo", {{"args", {"rifle"}}});
    const std::string muzzle = pointNamed(p, "muzzle"), stock = pointNamed(p, "stock");
    const glm::dmat4 m = world(muzzle);
    const glm::dvec3 toTarget = glm::normalize(pos("enemy") - glm::dvec3(m[3]));
    const double deg = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(glm::dvec3(m[0])), toTarget), -1.0, 1.0)));
    CHECK(deg < 2.0);
    INFO("solve error " << run("grappleInfo", {{"args", {"props_grapple"}}}).dump());
    // The off hand still holds its grip, as far from it as the right hand is from the main grip.
    const double offGap = glm::length(pos(pointNamed(p, "off")) - pos(jointPath("hand_l")));
    const double mainGap = glm::length(pos(pointNamed(p, "main")) - pos(jointPath("hand_r")));
    CHECK(std::abs(offGap - mainGap) < 1.0);
    // The stock sits at the shoulder pocket.
    CHECK(glm::length(pos(stock) - pos(jointPath("propShoulder_r"))) < 2.0);
    // Weight 0 later lets the aim go.
    run("puppeteerPropAim", {{"args", {"rifle"}}, {"off", true}, {"time", 5.0 / 30.0}, {"blend", 2.0 / 30.0}});
    at(10.0 / 30.0);
    const json info = run("grappleInfo", {{"args", {"props_grapple"}}});
    REQUIRE(info.at("aims").size() == 1);
    CHECK(info.at("aims").at(0).at("weight").get<double>() == Approx(0.0));
}

TEST_CASE("props: Unreal export attaches to hand_r by default and keys ik bones only when asked")
{
    manny();
    rifle();
    run("puppeteerPropSetup", {{"args", {"rifle"}}, {"preset", "rifle"}});
    run("puppeteerPropHold", {{"args", {"rifle"}}, {"time", 0.0}, {"blend", 0.0}, {"solve", false}});
    const std::string handR = jointPath("hand_r"), handL = jointPath("hand_l"), gun = jointPath("ik_hand_gun");
    const json u = run("puppeteerPropUnreal", {{"args", {"rifle"}}});
    CHECK(u.at("bone") == "hand_r");
    CHECK(u.at("socket") == "weapon_r");
    CHECK(u.at("strayCm").get<double>() < 1e-6);
    const scene::Node& r = *graph().find("rifle");
    CHECK(scene::pins::socketOf(r) == "weapon_r");
    CHECK(scene::pins::socketBoneOf(r) == "hand_r");
    at(0.0);
    const glm::dmat4 rel = glm::inverse(world(handR)) * world("rifle");
    CHECK(near(scene::pins::socketOffsetOf(r), rel, 1e-6));
    // Nothing touches the ik bones unless asked.
    CHECK(scene::anim::animatedPlugs(graph(), id(gun)).empty());
    const json k = run("puppeteerPropUnreal", {{"args", {"rifle"}}, {"ikBones", true}});
    CHECK(k.at("ikFrames") == 11);
    for (int f : {0, 5, 10})
    {
        at(f / 30.0);
        CHECK(glm::length(pos(gun) - pos(handR)) < 1e-3);
        CHECK(glm::length(pos(jointPath("ik_hand_l")) - pos(handL)) < 1e-3);
        CHECK(glm::length(pos(jointPath("ik_hand_r")) - pos(handR)) < 1e-3);
        const glm::dmat3 a(world(gun)), b(world(handR));
        CHECK(glm::length(a[0] - b[0]) + glm::length(a[1] - b[1]) < 1e-4);
    }
    // Bone names that aren't there: refused, and the default export never needs them.
    CHECK_THROWS(run("puppeteerPropUnreal", {{"args", {"rifle"}}, {"ikBones", true}, {"gunBone", "weapon_ik"}}));
}

TEST_CASE("props: the Unreal socket holds the settled grip, not the frame the hold eases in from")
{
    manny();
    rifle();
    run("puppeteerPropSetup", {{"args", {"rifle"}}, {"preset", "rifle"}});
    run("puppeteerPropHold", {{"args", {"rifle"}}, {"time", 0.0}, {"blend", 6.0 / 30.0}, {"solve", false}});
    const json u = run("puppeteerPropUnreal", {{"args", {"rifle"}}});
    const scene::Node& r = *graph().find("rifle");
    at(10.0 / 30.0);
    const glm::dmat4 settled = glm::inverse(world(jointPath("hand_r"))) * world("rifle");
    CHECK(near(scene::pins::socketOffsetOf(r), settled, 1e-6));
    CHECK(u.at("strayCm").get<double>() > 1.0);
}

TEST_CASE("props: a greatsword held in guard toward a target")
{
    manny();
    sword();
    const json p = run("puppeteerPropSetup", {{"args", {"sword"}}, {"preset", "sword2h"}});
    run("puppeteerPropHold", {{"args", {"sword"}}, {"time", 0.0}, {"blend", 0.0}, {"solve", false}});
    run("spaceLocator", {{"name", "foe"}});
    run("setAttr", {{"args", {"foe.translate", 0.0, 150.0, 300.0}}});
    const json a = run("puppeteerPropAim", {{"args", {"sword"}}, {"target", "foe"}, {"time", 0.0}, {"blend", 0.0}});
    CHECK(a.at("style") == "guard");
    at(5.0 / 30.0);
    const std::string tip = pointNamed(p, "tip");
    const glm::dmat4 m = world(tip);
    const glm::dvec3 to = glm::normalize(pos("foe") - glm::dvec3(m[3]));
    CHECK(glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(glm::dvec3(m[0])), to), -1.0, 1.0))) < 2.0);
    CHECK(std::abs(glm::length(pos(pointNamed(p, "off")) - pos(jointPath("hand_l"))) - glm::length(pos(pointNamed(p, "main")) - pos(jointPath("hand_r")))) < 1.0);
}

TEST_CASE("props: guard aim after a solved hold over a second")
{
    manny();
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 1.0}, {"fps", 30.0}});
    sword();
    const json p = run("puppeteerPropSetup", {{"args", {"sword"}}, {"preset", "sword2h"}});
    run("puppeteerPropHold", {{"args", {"sword"}}, {"time", 0.0}, {"blend", 0.0}});
    run("spaceLocator", {{"name", "foe"}});
    run("setAttr", {{"args", {"foe.translate", 0.0, 150.0, 300.0}}});
    const json a = run("puppeteerPropAim", {{"args", {"sword"}}, {"target", "foe"}, {"time", 10.0 / 30.0}, {"blend", 5.0 / 30.0}});
    CHECK(a.at("style") == "guard");
    at(20.0 / 30.0);
    const std::string tip = pointNamed(p, "tip");
    const glm::dmat4 m = world(tip);
    const glm::dvec3 to = glm::normalize(pos("foe") - glm::dvec3(m[3]));
    CHECK(glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(glm::dvec3(m[0])), to), -1.0, 1.0))) < 2.0);
    CHECK(std::abs(glm::length(pos(pointNamed(p, "off")) - pos(jointPath("hand_l"))) - glm::length(pos(pointNamed(p, "main")) - pos(jointPath("hand_r")))) < 1.0);
}

TEST_CASE("props: the off hand holds its grip all through a guard aim's blend")
{
    manny();
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 1.0}, {"fps", 30.0}});
    sword();
    run("puppeteerPropSetup", {{"args", {"sword"}}, {"preset", "sword2h"}});
    const json h = run("puppeteerPropHold", {{"args", {"sword"}}, {"time", 0.0}, {"blend", 0.0}});
    run("spaceLocator", {{"name", "foe"}});
    run("setAttr", {{"args", {"foe.translate", 0.0, 150.0, 300.0}}});
    run("puppeteerPropAim", {{"args", {"sword"}}, {"target", "foe"}, {"time", 8.0 / 30.0}, {"blend", 12.0 / 30.0}});
    const scene::NodeId contact = id(h.at("contact").get<std::string>());
    const std::string handR = jointPath("hand_r");
    double worst = 0.0;
    std::vector<double> steps;
    glm::dvec3 last(0.0);
    for (int f = 4; f <= 24; ++f)
    {
        at(f / 30.0);
        const auto [hand, grip] = scene::grapple::contactPoints(graph(), contact);
        worst = std::max(worst, glm::length(hand - grip));
        if (f > 4)
            steps.push_back(glm::length(pos(handR) - last));
        last = pos(handR);
    }
    MESSAGE("off hand gap through the blend: " << worst << " cm");
    CHECK(worst < 0.1);
    // The aimed hand eases out of the shared pose: its first step is a small share of its largest, no pop.
    const double largest = *std::max_element(steps.begin(), steps.end());
    const auto first = std::find_if(steps.begin(), steps.end(), [](double s) { return s > 1e-6; });
    REQUIRE(first != steps.end());
    CHECK(*first < 0.3 * largest);
}

TEST_CASE("props: every finger of every holding hand closes onto the handle")
{
    struct Case
    {
        const char* kind;
        std::function<std::string()> make;
    };
    const Case cases[] = {
        {"sword2h", [] { return sword(); }},
        {"sword1h",
         [] {
             return group("sword", {cube("short_handle", {200, 6, 0}, {2.6, 12, 2.6}), cube("short_guard", {200, 13, 0}, {14, 2, 3}),
                                    cube("short_blade", {200, 49, 0}, {4.5, 70, 0.8})});
         }},
        {"pistol", [] { return pistol(); }},
        {"rifle", [] { return rifle(); }},
    };
    for (const Case& c : cases)
    {
        CAPTURE(c.kind);
        manny();
        const std::string prop = c.make();
        json meshes = json::array();
        for (scene::NodeId d : graph().descendants(id(prop)))
            if (graph().node(d)->type().attrIndex("outMesh") >= 0)
                meshes.push_back(graph().path(d));
        run("puppeteerPropSetup", {{"args", {prop}}, {"preset", c.kind}});
        const json h = run("puppeteerPropHold", {{"args", {prop}}, {"time", 0.0}, {"blend", 0.0}});
        at(2.0 / 30.0);
        for (const char* side : {"main", "off"})
        {
            if (!h.contains(side))
                continue;
            const std::string hand = graph().node(id(h.at(side).get<std::string>()))->name() + "_hand";
            CAPTURE(side);
            run("puppeteerHandPose", {{"args", {hand}}, {"pose", "flat"}, {"key", false}});
            const json r = run("puppeteerHandGrip", {{"args", {hand}}, {"target", meshes}, {"key", false}}).at(0).at("fingers");
            CHECK(r.size() == 5);
            for (const auto& [finger, f] : r.items())
            {
                CAPTURE(finger);
                CHECK_FALSE(f.at("stayed").get<bool>());
                CHECK(f.at("touching").get<bool>());
            }
        }
    }
}

TEST_CASE("props: any object becomes a prop, its settings read from its shape")
{
    manny();
    rifle();
    const json r = run("puppeteerPropSetup", {{"args", {"rifle"}}});
    CHECK(r.at("fit").at("shape") == "firearm");
    CHECK(r.at("preset").is_null());
    CHECK(r.at("twoHanded") == true);
    CHECK(r.at("aimStyle") == "shoulder");
    CHECK(r.at("trigger") == true);
    CHECK(r.at("shape") == "auto");
    // A preset fills the settings; the prop then matches it.
    const json p = run("puppeteerPropSetup", {{"args", {"rifle"}}, {"preset", "launcher"}});
    CHECK(p.at("preset") == "launcher");
    CHECK(p.at("holsterSpot") == "back");
    // Any setting is the prop's own: changing one leaves the preset behind.
    run("setAttr", {{"args", {"rifle_prop.holsterSpot", 3}}});
    CHECK(run("puppeteerPropInfo", {{"args", {"rifle"}}}).at("preset").is_null());
    CHECK(run("puppeteerPropInfo", {{"args", {"rifle"}}}).at("holsterSpot") == "thighRight");
}

TEST_CASE("props: points are added, moved, turned and removed, and a refit keeps the ones moved by hand")
{
    manny();
    std::vector<std::string> parts{cube("guitar_neck", {0, 100, 300}, {5, 60, 3}), cube("guitar_body", {0, 55, 300}, {34, 40, 9})};
    group("guitar", parts);
    const json p = run("puppeteerPropSetup", {{"args", {"guitar"}}});
    const size_t fitted = p.at("points").size();
    REQUIRE(fitted >= 2);
    const json added = run("puppeteerPropPoint", {{"args", {"guitar"}}, {"role", "off"}, {"name", "strum"}, {"position", v3(0, 60, 306)}});
    const std::string strum = added.at("point");
    CHECK(glm::length(pos(strum) - glm::dvec3(0, 60, 306)) < 1e-6);
    // Moved and turned by command.
    run("puppeteerPropPoint", {{"args", {strum}}, {"position", v3(2, 62, 306)}, {"aim", v3(1, 0, 0)}, {"palm", v3(0, 0, -1)}, {"radius", 3.0}});
    CHECK(glm::length(pos(strum) - glm::dvec3(2, 62, 306)) < 1e-6);
    const glm::dmat4 m = world(strum);
    CHECK(glm::dot(glm::normalize(glm::dvec3(m[0])), glm::dvec3(1, 0, 0)) > 0.9999);
    CHECK(glm::dot(glm::normalize(glm::dvec3(m[2])), glm::dvec3(0, 0, -1)) > 0.9999);
    // A fitted point moved by hand stays where it was put through a refit; the added one stays too.
    const std::string mainPt = pointNamed(p, "main");
    run("puppeteerPropPoint", {{"args", {mainPt}}, {"position", v3(0, 120, 300)}});
    run("puppeteerPropFit", {{"args", {"guitar"}}});
    CHECK(glm::length(pos(mainPt) - glm::dvec3(0, 120, 300)) < 1e-6);
    CHECK(graph().find(strum) != nullptr);
    CHECK(run("puppeteerPropInfo", {{"args", {"guitar"}}}).at("points").size() == fitted + 1);
    // The added grip holds a hand like any other.
    const json h = run("puppeteerPropHold", {{"args", {"guitar"}}, {"point", "strum"}, {"time", 0.0}, {"blend", 0.0}, {"solve", false}});
    CHECK(h.at("mainPoint") == strum);
    run("puppeteerPropPoint", {{"args", {strum}}, {"remove", true}});
    CHECK(graph().find(strum) == nullptr);
    run("undo");
    CHECK(graph().find(strum) != nullptr);
}

// Handoffs -------------------------------------------------------------------------------------------------------------

namespace proptest
{
/** Manny with two seconds of playback, holding a prop in the right hand from frame 0. */
void holding(const std::string& prop, const char* preset = nullptr)
{
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 2.0}, {"fps", 30.0}});
    json call = {{"args", {prop}}};
    if (preset)
        call["preset"] = preset;
    run("puppeteerPropSetup", call);
    run("puppeteerPropHold", {{"args", {prop}}, {"hand", "right"}, {"hands", 1}, {"character", "manny"}, {"time", 0.0}, {"blend", 0.0}, {"solve", false}});
}

std::string ball() { return group("ball", {cube("ball_mesh", {200, 0, 0}, {8, 8, 8})}); }

/** A second Manny, bob, standing `ahead` cm in front of the first and facing it. */
void bob(double ahead)
{
    run("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"rig", false}, {"prefix", "bob_"}});
    run("puppeteerCharacter", {{"args", {"bob_root"}}, {"template", "UE5_Manny"}, {"name", "bob"}});
    run("setAttr", {{"args", {"bob_root.translate", 0.0, 0.0, ahead}}});
    run("setAttr", {{"args", {"bob_root.rotate", 0.0, 180.0, 0.0}}});
}

/** The contact on the prop grapple holding source onto frame (any target), else none. */
scene::NodeId contactOf(const std::string& source, const std::string& frame)
{
    for (scene::NodeId c : scene::grapple::contactsOf(graph(), id("props_grapple")))
    {
        const scene::Node& n = *graph().node(c);
        const std::string s = std::get<std::string>(n.value(n.type().attrIndex("source")));
        if (graph().find(s) && graph().find(s)->id() == id(source) && std::get<std::string>(n.value(n.type().attrIndex("frame"))) == frame)
            return c;
    }
    return scene::kNoNode;
}

double angleDeg(const glm::dvec3& a, const glm::dvec3& b)
{
    return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0)));
}
} // namespace proptest

TEST_CASE("props: planted on its tip, it leans as asked and stays when the hand lets go")
{
    manny();
    sword();
    holding("sword", "sword1h");
    const json info = run("puppeteerPropInfo", {{"args", {"sword"}}});
    const std::string tip = pointNamed(info, "tip"), mainPt = pointNamed(info, "main");
    const json pl = run("puppeteerPropPlant", {{"args", {"sword"}}, {"time", 10.0 / 30.0}, {"blend", 9.0 / 30.0}, {"lean", 10.0}});
    CHECK(pl.at("surface") == "ground");
    std::vector<glm::dvec3> path;
    for (int f = 0; f <= 40; ++f)
    {
        at(f / 30.0);
        path.push_back(pos(tip));
    }
    // No pop where it leaves the hand, an eased start, and still once down.
    CHECK(glm::length(path[10] - path[9]) < 1e-6);
    double largest = 0.0;
    for (int f = 10; f < 19; ++f)
        largest = std::max(largest, glm::length(path[f + 1] - path[f]));
    CHECK(glm::length(path[11] - path[10]) < 0.3 * largest);
    for (int f = 20; f < 40; ++f)
        CHECK(glm::length(path[f + 1] - path[f]) < 1e-6);
    at(1.0);
    MESSAGE("planted tip height " << pos(tip).y << " cm, lean " << angleDeg(pos(mainPt) - pos(tip), {0, 1, 0}) << " deg");
    CHECK(std::abs(pos(tip).y) < 0.6);
    CHECK(angleDeg(pos(mainPt) - pos(tip), {0, 1, 0}) == Approx(10.0).epsilon(0.05));
    CHECK(run("puppeteerPropInfo", {{"args", {"sword"}}}).at("holder").is_null());
    CHECK(run("puppeteerPropInfo", {{"args", {"sword"}}}).at("holds").back().at("kind") == "world");
    // The hand carried it down: held on its grip in the world through the carry, let go after.
    const scene::NodeId pin = contactOf(jointPath("hand_r"), mainPt);
    REQUIRE(pin != scene::kNoNode);
    at(15.0 / 30.0);
    const auto [hand, grip] = scene::grapple::contactPoints(graph(), pin);
    MESSAGE("hand to grip mid-carry: " << glm::length(hand - grip) << " cm");
    CHECK(glm::length(hand - grip) < 1.0);
    at(35.0 / 30.0);
    CHECK(attr(graph().path(pin) + ".weight") == Approx(0.0));
    run("undo");
    CHECK(run("puppeteerPropInfo", {{"args", {"sword"}}}).at("holds").back().at("kind") == "hand");
}

TEST_CASE("props: a keyed throw flies a ballistic arc with its spin and lands on the ground")
{
    manny();
    ball();
    holding("ball");
    const double t0 = 6.0 / 30.0;
    at(t0);
    const glm::dvec3 c0 = pos("ball_mesh");
    const glm::dmat3 r0(world("ball_mesh"));
    const json th = run("puppeteerPropThrow", {{"args", {"ball"}}, {"time", t0}, {"speed", 400.0}, {"angle", 45.0}, {"direction", v3(0, 0, 1)},
                                               {"spin", 360.0}, {"spinAxis", v3(1, 0, 0)}, {"solve", false}});
    CHECK(th.at("launchFrom") == "speed");
    CHECK(th.at("landed") == true);
    CHECK(th.at("surface") == "ground");
    at(t0);
    CHECK(glm::length(pos("ball_mesh") - c0) < 1e-6);
    const glm::dvec3 v = glm::dvec3(0, std::sin(glm::radians(45.0)), std::cos(glm::radians(45.0))) * 400.0;
    const double tau = 6.0 / 30.0;
    at(t0 + tau);
    const glm::dvec3 want = c0 + v * tau + 0.5 * glm::dvec3(0, -981.0, 0) * tau * tau;
    MESSAGE("arc off the ballistic path: " << glm::length(pos("ball_mesh") - want) << " cm");
    CHECK(glm::length(pos("ball_mesh") - want) < 1e-6);
    // Turned 72 degrees about x after a fifth of a second at 360 a second.
    const glm::dmat3 d = glm::dmat3(world("ball_mesh")) * glm::transpose(r0);
    CHECK(glm::degrees(std::acos(std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0) / 2.0, -1.0, 1.0))) == Approx(72.0).epsilon(1e-6));
    // Lying on the ground once it has landed, and still.
    at(2.0);
    MESSAGE("landed ball centre height " << pos("ball_mesh").y);
    CHECK(pos("ball_mesh").y == Approx(4.05).epsilon(0.01));
    at(1.9);
    const glm::dvec3 a = pos("ball_mesh");
    at(2.0);
    CHECK(glm::length(pos("ball_mesh") - a) < 1e-6);
    // The arc's path dragged through another point keys it again through there.
    const std::string arc = th.at("arc");
    run("puppeteerPropArc", {{"args", {arc}}, {"through", v3(0, 200, 150)}, {"at", t0 + 0.5}, {"solve", false}});
    at(t0 + 0.5);
    CHECK(glm::length(pos("ball_mesh") - glm::dvec3(0, 200, 150)) < 1e-6);
    // A hand that doesn't move at the release: thrown forward at a default speed, which it says.
    CHECK(run("puppeteerPropThrow", {{"args", {"ball"}}, {"time", t0}, {"solve", false}}).at("launchFrom") == "default");
}

TEST_CASE("props: a physics throw bounces and comes to rest on the ground, baked to keys")
{
    manny();
    ball();
    holding("ball");
    const json th = run("puppeteerPropThrow",
                        {{"args", {"ball"}}, {"time", 0.2}, {"speed", 300.0}, {"angle", 30.0}, {"direction", v3(0, 0, 1)}, {"physics", true}, {"solve", false}});
    CHECK(th.at("how") == "physics");
    CHECK(th.at("landed") == true);
    CHECK(th.at("frames").get<int>() > 10);
    at(th.at("end").get<double>());
    const glm::dvec3 rest = pos("ball_mesh");
    MESSAGE("ball at rest: " << rest.x << ", " << rest.y << ", " << rest.z << " after " << th.at("frames").get<int>() << " frames");
    CHECK(rest.y == Approx(4.0).epsilon(0.1));
    CHECK(rest.z > 20.0);
    at(2.0);
    CHECK(glm::length(pos("ball_mesh") - rest) < 0.05);
}

TEST_CASE("props: a catch times itself to where the arc meets the hand, which reaches to take it")
{
    manny();
    ball();
    holding("ball");
    const double t0 = 6.0 / 30.0;
    const json th = run("puppeteerPropThrow", {{"args", {"ball"}}, {"time", t0}, {"speed", 100.0}, {"angle", 80.0}, {"solve", false}});
    // Through where the left hand is, 0.6 s after the release.
    at(0.0);
    const glm::dvec3 left = pos(jointPath("hand_l"));
    run("puppeteerPropArc", {{"args", {th.at("arc")}}, {"through", v3(left.x, left.y, left.z)}, {"at", t0 + 0.6}, {"solve", false}});
    const json c = run("puppeteerPropCatch", {{"args", {"ball"}}, {"hand", "either"}});
    MESSAGE("caught at frame " << c.at("frame").get<double>() << ", " << c.at("gap").get<double>() << " cm from the hand");
    CHECK(c.at("hand") == jointPath("hand_l"));
    CHECK(c.at("reachable") == true);
    CHECK(std::abs(c.at("time").get<double>() - (t0 + 0.6)) < 3.0 / 30.0);
    const double tc = c.at("time").get<double>();
    // In the hand from the catch on, and the hand met it there.
    at(tc + 0.5);
    CHECK(run("puppeteerPropInfo", {{"args", {"ball"}}}).at("holder") == jointPath("hand_l"));
    const glm::dmat4 rel0 = glm::inverse(world(jointPath("hand_l"))) * world("ball");
    at(tc + 0.8);
    CHECK(near(glm::inverse(world(jointPath("hand_l"))) * world("ball"), rel0, 1e-6));
    const scene::NodeId pin = contactOf(jointPath("hand_l"), pointNamed(run("puppeteerPropInfo", {{"args", {"ball"}}}), "main"));
    REQUIRE(pin != scene::kNoNode);
    at(tc);
    const auto [hand, grip] = scene::grapple::contactPoints(graph(), pin);
    MESSAGE("hand to grip at the catch: " << glm::length(hand - grip) << " cm");
    CHECK(glm::length(hand - grip) < 1.0);
    CHECK(run("puppeteerPropInfo", {{"args", {"ball"}}}).at("holds").back().at("side") == "left");
}

TEST_CASE("props: editing a caught throw's arc catches it again on the new arc, with no jump")
{
    manny();
    ball();
    holding("ball");
    const double t0 = 6.0 / 30.0;
    const json th = run("puppeteerPropThrow", {{"args", {"ball"}}, {"time", t0}, {"speed", 100.0}, {"angle", 80.0}, {"solve", false}});
    at(0.0);
    const glm::dvec3 left = pos(jointPath("hand_l"));
    run("puppeteerPropArc", {{"args", {th.at("arc")}}, {"through", v3(left.x, left.y, left.z)}, {"at", t0 + 0.6}, {"solve", false}});
    const json c = run("puppeteerPropCatch", {{"args", {"ball"}}, {"hand", "left"}});
    const double before = c.at("time").get<double>();
    // A later, higher arc through the same hand.
    const json e = run("puppeteerPropArc", {{"args", {th.at("arc")}}, {"through", v3(left.x, left.y, left.z)}, {"at", t0 + 0.9}, {"solve", false}});
    REQUIRE(e.contains("caughtAt"));
    const double after = e.at("caughtAt").get<double>();
    MESSAGE("caught at frame " << before * 30.0 << ", then " << after * 30.0 << " after the edit");
    CHECK(after > before + 4.0 / 30.0);
    CHECK(std::abs(after - (t0 + 0.9)) < 3.0 / 30.0);
    // One catch, on the new time, and the hand takes it there.
    int catches = 0;
    const json info = run("puppeteerPropInfo", {{"args", {"ball"}}});
    for (const json& ev : info.at("events"))
        catches += ev.value("kind", std::string()) == "catch";
    CHECK(catches == 1);
    at(after + 0.5);
    CHECK(run("puppeteerPropInfo", {{"args", {"ball"}}}).at("holder") == jointPath("hand_l"));
    // No jump: frame to frame steps around the catch stay close to the arc's own speed.
    double worst = 0.0, typical = 0.0;
    for (int f = -3; f <= 3; ++f)
    {
        at(after + (f - 1) / 30.0);
        const glm::dvec3 a = pos("ball_mesh");
        at(after + f / 30.0);
        const double step = glm::length(pos("ball_mesh") - a);
        worst = std::max(worst, step);
        if (f == -3)
            typical = step;
    }
    MESSAGE("largest step around the catch " << worst << " cm, arc step " << typical << " cm");
    CHECK(worst < 3.0 * typical + 1.0);
    // Undo puts the first catch back.
    run("undo", {});
    bool old = false;
    const json undone = run("puppeteerPropInfo", {{"args", {"ball"}}});
    for (const json& ev : undone.at("events"))
        old = old || (ev.value("kind", std::string()) == "catch" && std::abs(ev.at("time").get<double>() - before) < 0.5 / 30.0);
    CHECK(old);
}

TEST_CASE("props: another character catches what one throws")
{
    manny();
    bob(220.0);
    ball();
    holding("ball");
    const double t0 = 6.0 / 30.0;
    const json th =
        run("puppeteerPropThrow", {{"args", {"ball"}}, {"time", t0}, {"speed", 300.0}, {"angle", 40.0}, {"character", "manny"}, {"solve", false}});
    at(0.0);
    const glm::dvec3 target = pos("bob_hand_r");
    run("puppeteerPropArc", {{"args", {th.at("arc")}}, {"through", v3(target.x, target.y, target.z)}, {"at", t0 + 0.7}, {"solve", false}});
    const json c = run("puppeteerPropCatch", {{"args", {"ball"}}, {"character", "bob"}});
    MESSAGE("bob catches at frame " << c.at("frame").get<double>() << " with " << c.at("hand").get<std::string>());
    CHECK(c.at("hand") == graph().path(id("bob_hand_r")));
    at(c.at("time").get<double>() + 0.3);
    CHECK(run("puppeteerPropInfo", {{"args", {"ball"}}}).at("holder") == graph().path(id("bob_hand_r")));
}

TEST_CASE("props: put on a table it rides the table, and the hand reaches to take it back")
{
    manny();
    ball();
    holding("ball");
    at(0.0);
    const glm::dvec3 h = pos(jointPath("hand_r"));
    cube("table", {h.x, h.y - 30.0, h.z}, {60, 4, 60});
    const double top = h.y - 28.0;
    const json pl = run("puppeteerPropPlace", {{"args", {"ball"}}, {"on", "table"}, {"time", 0.2}, {"blend", 0.3}, {"solve", false}});
    CHECK(pl.at("on") == graph().path(id("table")));
    at(1.0);
    MESSAGE("ball on the table: " << pos("ball_mesh").y - top << " cm above its top");
    CHECK(pos("ball_mesh").y == Approx(top + 4.05).epsilon(0.002));
    CHECK(run("puppeteerPropInfo", {{"args", {"ball"}}}).at("holds").back().at("space") == graph().path(id("table")));
    // The table moves: the ball goes with it.
    const glm::dvec3 before = pos("ball_mesh");
    run("setAttr", {{"args", {"table.translateX", h.x + 50.0}}});
    at(1.0);
    CHECK(glm::length(pos("ball_mesh") - (before + glm::dvec3(50, 0, 0))) < 1e-6);
    run("setAttr", {{"args", {"table.translateX", h.x}}});
    // Taken back: the hand reaches for it on the table over a third of a second.
    const json take = run("puppeteerPropHold", {{"args", {"ball"}}, {"hand", "right"}, {"hands", 1}, {"time", 1.2}, {"reach", 0.3}});
    at(1.5);
    CHECK(run("puppeteerPropInfo", {{"args", {"ball"}}}).at("holder") == jointPath("hand_r"));
    const scene::NodeId pin = contactOf(jointPath("hand_r"), take.at("mainPoint"));
    REQUIRE(pin != scene::kNoNode);
    at(1.2);
    const auto [hand, grip] = scene::grapple::contactPoints(graph(), pin);
    MESSAGE("hand to grip as it takes it: " << glm::length(hand - grip) << " cm");
    CHECK(glm::length(hand - grip) < 1.0);
    CHECK(run("puppeteerPropInfo", {{"args", {"ball"}}}).at("holds").back().at("kind") == "hand");
}

TEST_CASE("props: hung from a hook by its grip, it hangs below it")
{
    manny();
    sword();
    holding("sword", "sword1h");
    cube("rack", {0, 150, 80}, {80, 10, 4});
    run("spaceLocator", {{"name", "hook"}});
    run("setAttr", {{"args", {"hook.translate", 10.0, 150.0, 76.0}}});
    run("parent", {{"args", {"hook", "rack"}}});
    run("puppeteerPropPlace", {{"args", {"sword"}}, {"on", "rack"}, {"hook", "hook"}, {"time", 0.2}, {"blend", 0.3}, {"solve", false}});
    at(1.0);
    const json info = run("puppeteerPropInfo", {{"args", {"sword"}}});
    CHECK(glm::length(pos(pointNamed(info, "main")) - pos("hook")) < 1e-6);
    CHECK(angleDeg(pos(pointNamed(info, "tip")) - pos("hook"), {0, -1, 0}) < 3.0);
}

TEST_CASE("props: passed to another character, both hands share it and the receiver keeps it")
{
    manny();
    bob(70.0);
    sword();
    holding("sword", "sword1h");
    const std::string giver = jointPath("hand_r"), receiver = graph().path(id("bob_hand_r"));
    at(0.0);
    const glm::dvec3 g0 = pos(giver), r0 = pos(receiver);
    const json ps = run("puppeteerPropPass", {{"args", {"sword"}}, {"to", "bob"}, {"time", 0.2}, {"duration", 0.6}});
    const double handover = ps.at("handover");
    CHECK(handover == Approx(0.5));
    at(handover);
    const auto [gh, ggrip] = scene::grapple::contactPoints(graph(), id(ps.at("contact").get<std::string>()));
    const std::string rp = ps.at("receiverPoint");
    const glm::dmat4 held = glm::inverse(world(receiver)) * world(rp);
    MESSAGE("at the handover: giver " << glm::length(gh - ggrip) << " cm from its grip; hands moved " << glm::length(pos(giver) - g0) << " and "
                                      << glm::length(pos(receiver) - r0) << " cm");
    CHECK(glm::length(gh - ggrip) < 1.0);
    // The receiver's hand has it at its grip from the handover on.
    at(handover + 0.4);
    CHECK(near(glm::inverse(world(receiver)) * world(rp), held, 1e-6));
    at(handover);
    // Both arms reach: neither hand stays put.
    CHECK(glm::length(pos(giver) - g0) > 2.0);
    CHECK(glm::length(pos(receiver) - r0) > 2.0);
    // No pop anywhere in the exchange: every step of the grip is in line with the steps either side of it.
    std::vector<glm::dvec3> path;
    for (int f = 3; f <= 28; ++f)
    {
        at(f / 30.0);
        path.push_back(pos(ps.at("receiverPoint").get<std::string>()));
    }
    std::vector<double> steps;
    for (size_t i = 0; i + 1 < path.size(); ++i)
        steps.push_back(glm::length(path[i + 1] - path[i]));
    double worst = 0.0;
    for (size_t i = 1; i + 1 < steps.size(); ++i)
        worst = std::max(worst, steps[i] / (std::max(steps[i - 1], steps[i + 1]) + 0.5));
    MESSAGE("largest step against its neighbours: " << worst << "x");
    CHECK(worst < 2.0);
    at(1.5);
    CHECK(run("puppeteerPropInfo", {{"args", {"sword"}}}).at("holder") == receiver);
}

TEST_CASE("props: a prop that leaves the hand goes to Unreal as baked motion on a bone of its own")
{
    manny();
    ball();
    holding("ball");
    run("puppeteerPropThrow", {{"args", {"ball"}}, {"time", 0.3}, {"speed", 300.0}, {"angle", 40.0}, {"solve", false}});
    const json u = run("puppeteerPropUnreal", {{"args", {"ball"}}});
    CHECK(u.at("mode") == "baked");
    CHECK(u.at("bone") == "ball_motion");
    const scene::Node& r = *graph().find("ball");
    CHECK(scene::pins::socketOf(r) == "ball_motion");
    CHECK(scene::pins::socketBoneOf(r).empty());
    const std::string joint = u.at("joint");
    double worst = 0.0;
    for (int f = 0; f <= 60; f += 3)
    {
        at(f / 30.0);
        worst = std::max(worst, glm::length(pos(joint) - pos("ball")));
        const glm::dmat3 a(world(joint)), b(world("ball"));
        CHECK(glm::length(a[0] - glm::normalize(b[0])) + glm::length(a[1] - glm::normalize(b[1])) < 1e-4);
    }
    MESSAGE("baked bone off the prop: " << worst << " cm");
    CHECK(worst < 1e-4);
    // Held all through: still a socket on the hand.
    run("undo");
    run("undo");
    CHECK(run("puppeteerPropUnreal", {{"args", {"ball"}}}).at("mode") == "socket");
}
