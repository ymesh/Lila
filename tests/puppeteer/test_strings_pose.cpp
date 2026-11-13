#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Gizmo.h"
#include "ui/MarkingMenu.h"
#include "ui/Menus.h"
#include "ui/Overlay.h"
#include "ui/RenderPasses.h"
#include "render/FrameState.h"

#include <QApplication>
#include "extensions/ExtensionManager.h"

#include <chrono>
#include <map>

#include "scene/Anim.h"
#include "scene/Ghosts.h"
#include "scene/Strings.h"
#include <QImage>

#include <glm/gtx/quaternion.hpp>

using namespace lila;
using namespace lila::scene;
using namespace puptest;
using json = nlohmann::json;

namespace
{
/** A stick biped in cm with a character: hips at 100, a spine, neck and head, arms out to the sides, legs down. */
void stringsBiped()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    auto j = [](const char* name, double x, double y, double z, const char* parent) {
        run("joint", {{"position", v3(x, y, z)}, {"name", name}, {"parent", parent}, {"skipSelect", true}});
    };
    j("hips", 0, 100, 0, "");
    j("spine_01", 0, 115, 0, "hips");
    j("spine_02", 0, 134, 0, "spine_01");
    j("neck", 0, 150, 0, "spine_02");
    j("head", 0, 162, 0, "neck");
    for (const char* side : {"l", "r"})
    {
        const double s = side[0] == 'l' ? 1.0 : -1.0;
        auto n = [&](const char* base) { return std::string(base) + "_" + side; };
        j(n("upperarm").c_str(), 18 * s, 140, 0, "spine_02");
        j(n("lowerarm").c_str(), 46 * s, 140, -2, n("upperarm").c_str());
        j(n("hand").c_str(), 72 * s, 140, 0, n("lowerarm").c_str());
        j(n("thigh").c_str(), 10 * s, 95, 0, "hips");
        j(n("calf").c_str(), 10 * s, 52, 4, n("thigh").c_str());
        j(n("foot").c_str(), 10 * s, 8, 0, n("calf").c_str());
    }
    run("puppeteerCharacter", {{"args", {"hips"}}, {"name", "hero"}});
}

/** Sets the Strings mode the way the arc and marking menu do, and back to Full Body when the test ends. */
struct ModeScope
{
    explicit ModeScope(const char* action) { ui::actions::run(action); }
    ~ModeScope() { ui::actions::run("puppeteer.strings.fullBody"); }
};

json vj(const glm::dvec3& v) { return v3(v.x, v.y, v.z); }

bool checked(const char* action) { return ui::actions::registry().find(action)->checked(); }

QPointF arcPoint(double r, double screenDegrees)
{
    const double a = glm::radians(screenDegrees);
    return kCenter + QPointF(r * std::cos(a), r * std::sin(a));
}

/** The middle of the arc segment k (0 Default on top, Free, Forward, Backward, Bidirectional, IK, 6 Full Body). */
QPointF modeSegment(int k) { return arcPoint(u(177.0), 228.0 - (228.0 - 162.0) / 7.0 * (k + 0.5)); }
} // namespace

TEST_CASE("stringsPose backward from the knee keeps the pinned foot and swings the body")
{
    loadPuppeteer();
    stringsBiped();
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    const glm::dmat4 foot = world("foot_l");
    const glm::dvec3 hips = pos("hips"), other = pos("foot_r");
    const json r = run("stringsPose", {{"args", {"calf_l"}}, {"mode", "backward"}, {"axis", v3(1, 0, 0)}, {"angle", 25.0}});
    CHECK(r["mode"] == "backward");
    CHECK(r["anchor"].get<std::string>().ends_with("foot_l"));
    CHECK(near(world("foot_l"), foot, 1e-5));
    CHECK(near(pos("foot_r"), other, 1e-3));
    CHECK(glm::length(pos("hips") - hips) > 5.0);
    run("undo");
    CHECK(near(pos("hips"), hips, 1e-9));
}

TEST_CASE("stringsPose IK puts the hand on the target and leaves the body")
{
    loadPuppeteer();
    stringsBiped();
    const glm::dvec3 hand = pos("hand_l"), spine = pos("spine_02");
    run("stringsPose", {{"args", {"hand_l"}}, {"mode", "ik"}, {"by", v3(-10, 15, 8)}});
    CHECK(near(pos("hand_l"), hand + glm::dvec3(-10, 15, 8), 1e-3));
    CHECK(near(pos("spine_02"), spine, 1e-9));
}

TEST_CASE("stringsPose IK on the hips crouches over locked or pinned feet")
{
    loadPuppeteer();
    stringsBiped();
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    const glm::dvec3 l = pos("foot_l"), r = pos("foot_r");
    run("stringsPose", {{"args", {"hips"}}, {"mode", "ik"}, {"by", v3(0, -15, 4)}});
    CHECK(near(pos("hips"), glm::dvec3(0, 85, 4), 1e-6));
    CHECK(near(pos("foot_l"), l, 1e-3));
    CHECK(near(pos("foot_r"), r, 1e-3));
}

TEST_CASE("stringsPose anchors hold for one pose only and holds false ignores pins")
{
    loadPuppeteer();
    stringsBiped();
    const glm::dvec3 hand = pos("hand_r");
    const json r = run("stringsPose", {{"args", {"spine_02"}}, {"mode", "backward"}, {"axis", v3(0, 0, 1)}, {"angle", 15.0}, {"anchors", {"hand_r"}}});
    CHECK(r["anchor"].get<std::string>().ends_with("hand_r"));
    CHECK(near(pos("hand_r"), hand, 1e-5));
    // Nothing was stored: the next pose has no anchor but the joint itself.
    const json next = run("stringsPose", {{"args", {"spine_02"}}, {"mode", "backward"}, {"axis", v3(0, 0, 1)}, {"angle", 5.0}});
    CHECK(next["anchor"].get<std::string>().ends_with("spine_02"));

    run("puppeteerPin", {{"args", {"foot_l"}}, {"pin", true}});
    const json free = run("stringsPose", {{"args", {"calf_l"}}, {"mode", "backward"}, {"axis", v3(1, 0, 0)}, {"angle", 10.0}, {"holds", false}});
    CHECK(free["anchor"].get<std::string>().ends_with("calf_l"));
}

TEST_CASE("stringsPose says when it falls back to Forward, and refuses joints outside a character")
{
    loadPuppeteer();
    run("newScene");
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("joint", {{"position", v3(0, 100, 0)}, {"name", "pelvis"}});
    run("joint", {{"position", v3(10, 50, 0)}, {"name", "leg_l"}, {"parent", "pelvis"}, {"skipSelect", true}});
    run("joint", {{"position", v3(-10, 50, 0)}, {"name", "leg_r"}, {"parent", "pelvis"}, {"skipSelect", true}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "loose"}, {"parent", ""}, {"skipSelect", true}});
    run("puppeteerCharacter", {{"args", {"root"}}, {"name", "hero"}});
    const json r = run("stringsPose", {{"args", {"root"}}, {"mode", "backward"}, {"axis", v3(0, 1, 0)}, {"angle", 10.0}});
    CHECK(r.contains("fallback"));
    CHECK_THROWS(run("stringsPose", {{"args", {"loose"}}, {"mode", "ik"}, {"by", v3(0, 1, 0)}}));
    CHECK_THROWS(run("stringsPose", {{"args", {"pelvis"}}, {"mode", "sideways"}, {"by", v3(0, 1, 0)}}));
}

TEST_CASE("strings modes: the actions are one exclusive choice, the U menus hold the options and every mode")
{
    loadPuppeteer();
    ModeScope scope("puppeteer.strings.backward");
    CHECK(checked("puppeteer.strings.backward"));
    CHECK_FALSE(checked("puppeteer.strings.forward"));
    ui::actions::run("puppeteer.strings.next");
    CHECK(checked("puppeteer.strings.backwardAll"));
    CHECK_FALSE(checked("puppeteer.strings.backward"));
    // The All variant toggles back and forth, and has nothing to do on IK.
    ui::actions::run("puppeteer.strings.allVariant");
    CHECK(checked("puppeteer.strings.backward"));
    ui::actions::run("puppeteer.strings.ik");
    CHECK_FALSE(ui::actions::registry().find("puppeteer.strings.allVariant")->enabled());
    const json menu = ui::markingMenus::resolve("puppeteerStrings");
    CHECK(menu["N"]["action"] == "puppeteer.anim.pin");
    CHECK(menu["W"]["action"] == "puppeteer.strings.contacts");
    CHECK(menu["E"]["action"] == "puppeteer.strings.stretch");
    CHECK(menu["S"]["action"] == "puppeteer.strings.zero");
    CHECK(menu["NE"]["action"] == "puppeteer.strings.isolate");
    const json modes = ui::markingMenus::resolve("puppeteerStringsModes");
    for (strings::Mode m : strings::kModes)
    {
        CAPTURE(strings::modeName(m));
        CHECK(modes.dump().find(std::string("puppeteer.strings.") + strings::modeName(m) + "\"") != std::string::npos);
    }
    CHECK(modes["N"]["action"] == "puppeteer.strings.backward");
    CHECK(modes["S"]["action"] == "puppeteer.strings.forward");
}

TEST_CASE("spinner: the mode arc picks the Strings mode and drags pose in it")
{
    loadPuppeteer();
    stringsBiped();
    ModeScope scope("puppeteer.strings.forward");
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    run("select", {{"args", {"calf_l"}}});
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    REQUIRE(gz);
    gz->setView(view());
    REQUIRE(gz->supports("rotate"));

    // Each segment of the arc is its mode; the right button takes Forward, Backward and Bidirectional to All and back.
    const char* actions[] = {"puppeteer.strings.default",       "puppeteer.strings.free", "puppeteer.strings.forward", "puppeteer.strings.backward",
                             "puppeteer.strings.bidirectional", "puppeteer.strings.ik",   "puppeteer.strings.fullBody"};
    for (int k : {6, 0, 5, 1, 4, 2, 3})
    {
        gz->setView(view());
        REQUIRE(gz->press(modeSegment(k), Qt::LeftButton, Qt::NoModifier));
        gz->release();
        CHECK(checked(actions[k]));
    }
    for (auto [k, all] : {std::pair{2, "puppeteer.strings.forwardAll"}, {3, "puppeteer.strings.backwardAll"}, {4, "puppeteer.strings.all"}})
    {
        gz->setView(view());
        REQUIRE(gz->press(modeSegment(k), Qt::RightButton, Qt::NoModifier));
        gz->release();
        CHECK(checked(all));
        // The left button keeps the variant; right again goes back.
        gz->setView(view());
        REQUIRE(gz->press(modeSegment(k), Qt::LeftButton, Qt::NoModifier));
        gz->release();
        CHECK(checked(all));
        gz->setView(view());
        REQUIRE(gz->press(modeSegment(k), Qt::RightButton, Qt::NoModifier));
        gz->release();
        CHECK(checked(actions[k]));
    }
    gz->setView(view());
    REQUIRE(gz->press(modeSegment(3), Qt::LeftButton, Qt::NoModifier));
    gz->release();

    // Backward: a drag on the X zone keeps the pinned foot and moves the hips.
    const glm::dmat4 foot = world("foot_l");
    const glm::dvec3 hips = pos("hips");
    const QPointF zone = arcPoint(u(110.0), -90.0);
    gz->setView(view());
    gz->hover(zone);
    REQUIRE(gz->press(zone, Qt::LeftButton, Qt::NoModifier));
    for (int i = 1; i <= 4; ++i)
    {
        gz->setView(view());
        gz->drag(zone + QPointF(0.0, -10.0 * i), Qt::NoModifier);
    }
    CHECK(gz->readout().contains("Backward"));
    CHECK(gz->readout().contains("foot_l"));
    gz->release();
    CHECK(near(world("foot_l"), foot, 1e-5));
    CHECK(glm::length(pos("hips") - hips) > 1.0);
    // One undo step for the drag.
    run("undo");
    CHECK(near(pos("hips"), hips, 1e-9));
}

TEST_CASE("stringsGlue: glued hands keep their offset, an attached prop follows the hand, unglue frees them")
{
    loadPuppeteer();
    stringsBiped();
    run("stringsGlue", {{"remove", true}});
    // Arms bent first, so both hands can go somewhere together.
    run("stringsPose", {{"args", {"hand_l"}}, {"mode", "ik"}, {"by", v3(-20, 0, 15)}});
    run("stringsPose", {{"args", {"hand_r"}}, {"mode", "ik"}, {"by", v3(20, 0, 15)}});
    run("stringsGlue", {{"args", {"hand_l", "hand_r"}}});
    const glm::dvec3 gap = pos("hand_r") - pos("hand_l");
    const glm::dvec3 spine = pos("spine_02");
    const json r = run("stringsPose", {{"args", {"hand_l"}}, {"mode", "ik"}, {"by", v3(0, 8, 5)}});
    CHECK(r["followed"].size() == 1);
    CHECK(near(pos("hand_r") - pos("hand_l"), gap, 1e-3));
    CHECK(near(pos("spine_02"), spine, 1e-9));

    // A prop attached to the hand follows it; posing the prop moves the hand with it, as glue would.
    run("spaceLocator", {{"name", "cup"}});
    run("move", {{"args", {"cup"}}, {"by", vj(pos("hand_l") + glm::dvec3(0, 0, 5))}});
    run("stringsGlue", {{"args", {"cup", "hand_l"}}, {"attach", true}});
    CHECK(run("stringsGlue", {{"query", true}}).size() == 1); // the hand left the glue for the attach
    const glm::dvec3 offset = pos("cup") - pos("hand_l");
    run("stringsPose", {{"args", {"hand_l"}}, {"mode", "ik"}, {"by", v3(5, -5, 0)}});
    CHECK(near(pos("cup") - pos("hand_l"), offset, 1e-3));
    // Attach is one-way: moving the cup leaves the hand.
    const glm::dvec3 hand = pos("hand_l");
    run("stringsPose", {{"args", {"cup"}}, {"by", v3(0, 4, 0)}});
    CHECK(near(pos("hand_l"), hand, 1e-9));

    run("stringsGlue", {{"args", {"cup"}}, {"remove", true}});
    CHECK(run("stringsGlue", {{"query", true}}).empty());
    CHECK_THROWS(run("stringsPose", {{"args", {"cup"}}, {"by", v3(0, 4, 0)}}));
    CHECK_THROWS(run("stringsGlue", {{"args", {"cup"}}}));
}

TEST_CASE("stringsGlue: a glued prop leads when it is posed, and the hand bends its arm to stay on it")
{
    loadPuppeteer();
    stringsBiped();
    run("stringsGlue", {{"remove", true}});
    run("spaceLocator", {{"name", "bar"}});
    run("move", {{"args", {"bar"}}, {"by", vj(pos("hand_l"))}});
    run("stringsGlue", {{"args", {"hand_l", "bar"}}});
    const glm::dvec3 spine = pos("spine_02");
    const json r = run("stringsPose", {{"args", {"bar"}}, {"by", v3(-8, 12, 6)}});
    CHECK(near(pos("hand_l"), pos("bar"), 1e-3));
    CHECK(near(pos("spine_02"), spine, 1e-9));
    run("stringsGlue", {{"remove", true}});
}

namespace
{
/** Rasterises overlay triangles (solid and atlas-tinted) onto img, alpha blended in draw order. */
void rasterise(QImage& img, const std::vector<render::OverlayVertex>& tris, const render::OverlayAtlas* atlas)
{
    for (size_t i = 0; i + 2 < tris.size(); i += 3)
    {
        const auto &a = tris[i], &b = tris[i + 1], &c = tris[i + 2];
        const float minX = std::floor(std::min({a.pos.x, b.pos.x, c.pos.x})), maxX = std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x}));
        const float minY = std::floor(std::min({a.pos.y, b.pos.y, c.pos.y})), maxY = std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y}));
        const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
        if (std::abs(area) < 1e-6f)
            continue;
        for (int y = std::max(0, int(minY)); y < std::min(img.height(), int(maxY) + 1); ++y)
            for (int x = std::max(0, int(minX)); x < std::min(img.width(), int(maxX) + 1); ++x)
            {
                const glm::vec2 p(x + 0.5f, y + 0.5f);
                const float w0 = ((b.pos.x - p.x) * (c.pos.y - p.y) - (b.pos.y - p.y) * (c.pos.x - p.x)) / area;
                const float w1 = ((c.pos.x - p.x) * (a.pos.y - p.y) - (c.pos.y - p.y) * (a.pos.x - p.x)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                glm::vec4 col = a.color * w0 + b.color * w1 + c.color * w2;
                if (a.uv.x >= 0.0f)
                {
                    if (!atlas || atlas->width == 0)
                        continue;
                    const glm::vec2 uv = a.uv * w0 + b.uv * w1 + c.uv * w2;
                    const int ax = std::clamp(int(uv.x * atlas->width), 0, atlas->width - 1), ay = std::clamp(int(uv.y * atlas->height), 0, atlas->height - 1);
                    col.a *= atlas->pixels[size_t(ay) * size_t(atlas->width) + size_t(ax)] / 255.0f;
                }
                const QColor under = img.pixelColor(x, y);
                const float k = col.a;
                img.setPixelColor(x, y, QColor::fromRgbF(under.redF() * (1 - k) + col.r * k, under.greenF() * (1 - k) + col.g * k,
                                                         under.blueF() * (1 - k) + col.b * k));
            }
    }
}
} // namespace

TEST_CASE("spinner draw: the Strings arc, saved for a look when LILA_SPINNER_PNG names a folder")
{
    const char* dir = std::getenv("LILA_SPINNER_PNG");
    if (!dir || !qApp)
        return;
    loadPuppeteer();
    stringsBiped();
    run("select", {{"args", {"calf_l"}}});
    for (auto [action, file] : {std::pair{"puppeteer.strings.backward", "backward"}, {"puppeteer.strings.fullBody", "fullbody"}})
    {
        ModeScope scope(action);
        std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
        gz->setView(view());
        gz->supports("rotate");
        gz->hover(modeSegment(2));
        std::vector<render::OverlayVertex> tris;
        ui::OverlayPainter p(tris, 1.0);
        gz->draw(p);
        QImage img(800, 600, QImage::Format_RGB32);
        img.fill(QColor(70, 72, 76));
        rasterise(img, tris, p.atlas().get());
        const QImage crop = img.copy(QRect(480, 130, 320, 340));
        crop.save(QString("%1/spinner_%2.png").arg(dir, file));
        crop.scaled(crop.width() / 8, crop.height() / 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .scaled(crop.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .save(QString("%1/spinner_%2_squint.png").arg(dir, file));
    }
}

TEST_CASE("stringsZero zeroes through the mode: backward keeps the pinned foot")
{
    loadPuppeteer();
    stringsBiped();
    run("stringsPose", {{"args", {"calf_l"}}, {"mode", "forward"}, {"axis", v3(1, 0, 0)}, {"angle", 35.0}});
    run("puppeteerPin", {{"args", {"foot_l"}}, {"pin", true}});
    const glm::dmat4 foot = world("foot_l");
    const glm::dvec3 hips = pos("hips");
    run("stringsZero", {{"args", {"calf_l"}}, {"mode", "backward"}});
    CHECK(near(std::get<glm::dvec3>(graph().value({id("calf_l"), Transform::kRotate, -1})), glm::dvec3(0.0), 1e-6));
    CHECK(near(world("foot_l"), foot, 1e-5));
    CHECK(glm::length(pos("hips") - hips) > 1.0);
}

TEST_CASE("stringsRestoreLengths puts stretched bones back to their bind length")
{
    loadPuppeteer();
    stringsBiped();
    const std::string mesh = run("polyCylinder", {{"name", "Body"}, {"radius", 20}, {"height", 160}})[0];
    run("skinCluster", {{"args", {"hips", mesh}}});
    const double bound = glm::length(pos("calf_l") - pos("thigh_l")), shin = glm::length(pos("foot_l") - pos("calf_l"));
    run("stringsPose", {{"args", {"hips"}}, {"mode", "ik"}, {"by", v3(0, 30, 0)}, {"stretch", 1.5}});
    run("puppeteerPin", {{"args", {"foot_l"}}, {"pin", true}});
    const glm::dvec3 footAt = pos("foot_l");
    const json st = run("stringsPose", {{"args", {"hips"}}, {"mode", "ik"}, {"by", v3(0, 20, 0)}, {"stretch", 1.5}});
    CHECK(st["error"].get<double>() < 1e-3);
    CHECK(near(pos("foot_l"), footAt, 1e-3));
    CHECK(glm::length(pos("foot_l") - pos("calf_l")) > shin + 1.0);
    const json r = run("stringsRestoreLengths", {{"args", {"calf_l", "foot_l"}}});
    CHECK(r["restored"].size() >= 1);
    CHECK(glm::length(pos("calf_l") - pos("thigh_l")) == doctest::Approx(bound));
    CHECK(glm::length(pos("foot_l") - pos("calf_l")) == doctest::Approx(shin));
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "unbound"}, {"parent", "head"}, {"skipSelect", true}});
    CHECK(run("stringsRestoreLengths", {{"args", {"unbound"}}})["skipped"].size() == 1);
}

namespace
{
void mannyCharacter()
{
    auto& em = extensions::ExtensionManager::get();
    if (!em.isLoaded("RigTemplates"))
        REQUIRE(em.load(LILA_TEST_RIG_TEMPLATES_PACKAGE) == "RigTemplates");
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"rig", false}});
    run("puppeteerCharacter", {{"args", {"root"}}, {"template", "UE5_Manny"}, {"name", "manny"}});
}
} // namespace

TEST_CASE("strings rules: Manny's template contacts hold the feet without pins, and the arm bends as its limb")
{
    mannyCharacter();
    const glm::dmat4 foot = world("foot_l");
    const glm::dvec3 other = pos("foot_r");
    const json r = run("stringsPose", {{"args", {"calf_l"}}, {"mode", "backward"}, {"axis", v3(1, 0, 0)}, {"angle", 20.0}});
    CHECK(r["anchor"].get<std::string>().ends_with("foot_l"));
    CHECK(near(world("foot_l"), foot, 1e-4));
    CHECK(near(pos("foot_r"), other, 1e-3));

    // IK on the hand bends the arm limb the template declares (clavicle to hand), never the spine.
    const glm::dvec3 hand = pos("hand_l"), chest = pos("spine_05"), shoulder = pos("upperarm_l");
    const glm::dvec3 target = shoulder + 0.8 * (hand - shoulder) + glm::dvec3(0, 0, 8);
    run("stringsPose", {{"args", {"hand_l"}}, {"mode", "ik"}, {"by", v3(target.x - hand.x, target.y - hand.y, target.z - hand.z)}});
    CHECK(near(pos("hand_l"), target, 1e-3));
    CHECK(near(pos("spine_05"), chest, 1e-6));

    // Contacts off: the knee hangs the body from itself.
    const json free = run("stringsPose", {{"args", {"calf_r"}}, {"mode", "backward"}, {"axis", v3(1, 0, 0)}, {"angle", 10.0}, {"contacts", false}});
    CHECK(free["anchor"].get<std::string>().ends_with("calf_r"));
}

TEST_CASE("strings rules: posing Manny is interactive")
{
    mannyCharacter();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i)
        run("stringsPose", {{"args", {"calf_l"}}, {"mode", "backward"}, {"axis", v3(1, 0, 0)}, {"angle", 1.0}});
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10.0;
    MESSAGE("Manny backward pose with contacts, command and write: " << ms << " ms");
    CHECK(ms < 50.0);
}

TEST_CASE("stringsRules: a character's own contacts and limbs override the template's, and reset goes back")
{
    mannyCharacter();
    json r = run("stringsRules", {{"args", {"manny"}}});
    CHECK(r["contactsFrom"] == "template");
    CHECK(r["contacts"] == json({"foot_l", "foot_r"}));
    CHECK(r["chainsFrom"] == "template");

    r = run("stringsRules", {{"args", {"pelvis"}}, {"contacts", {"hand_l"}}, {"addChain", {"upperarm_r", "hand_r"}}});
    CHECK(r["contactsFrom"] == "character");
    CHECK(r["contacts"] == json({"hand_l"}));
    bool found = false;
    for (const json& c : r["chains"])
    {
        CHECK(c.front() != "clavicle_r"); // the template's arm overlapped the new limb, so it went
        found = found || (c.front() == "upperarm_r" && c.back() == "hand_r" && c.size() == 3);
    }
    CHECK(found);

    // The hand holds now, the feet don't: Backward from the calf hangs from the hand.
    const glm::dvec3 hand = pos("hand_l");
    run("stringsPose", {{"args", {"spine_03"}}, {"mode", "backward"}, {"axis", v3(0, 0, 1)}, {"angle", 10.0}});
    CHECK(near(pos("hand_l"), hand, 1e-4));

    r = run("stringsRules", {{"args", {"manny"}}, {"reset", true}});
    CHECK(r["contactsFrom"] == "template");
    CHECK_THROWS(run("stringsRules", {{"args", {"manny"}}, {"addChain", {"hand_r", "upperarm_r"}}}));
    run("undo");
}

namespace
{
double sec(double frame) { return frame / 30.0; }

/** The walk-ish keys: the hips bob on frames 0, 10, 20, 30, 40 with the feet keyed planted. */
void keyedBiped()
{
    stringsBiped();
    run("playbackOptions", {{"fps", 30.0}});
    for (int f : {0, 10, 20, 30, 40})
    {
        run("currentTime", {{"args", {sec(f)}}});
        run("move", {{"args", {"hips"}}, {"to", v3(0, 100 - (f % 20 == 0 ? 0 : 3), 0)}});
        run("setKeyframe", {{"args", {"hips", "thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r"}}});
    }
    run("currentTime", {{"args", {sec(20)}}});
}
} // namespace

TEST_CASE("stringsPose across a range: every keyed frame moves by the drag, pinned feet stay planted on each")
{
    loadPuppeteer();
    keyedBiped();
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    std::map<int, glm::dvec3> hips, foot;
    for (int f : {0, 10, 20, 30, 40})
    {
        run("currentTime", {{"args", {sec(f)}}});
        hips[f] = pos("hips");
        foot[f] = pos("foot_l");
    }
    run("currentTime", {{"args", {sec(20)}}});
    const json r = run("stringsPose", {{"args", {"hips"}}, {"mode", "ik"}, {"by", v3(0, -10, 0)}, {"range", {sec(10), sec(30)}}});
    CHECK(r.size() == 3);
    for (int f : {10, 20, 30})
    {
        run("currentTime", {{"args", {sec(f)}}});
        CAPTURE(f);
        CHECK(near(pos("hips"), hips[f] + glm::dvec3(0, -10, 0), 1e-3));
        CHECK(near(pos("foot_l"), foot[f], 1e-3));
    }
    for (int f : {0, 40})
    {
        run("currentTime", {{"args", {sec(f)}}});
        CHECK(near(pos("hips"), hips[f], 1e-6));
    }
    run("undo");
    run("currentTime", {{"args", {sec(30)}}});
    CHECK(near(pos("hips"), hips[30], 1e-6));
}

TEST_CASE("stringsPose across a range: falloff eases out, dense keys every frame, absolute brings frames to the pose")
{
    loadPuppeteer();
    keyedBiped();
    std::map<int, glm::dvec3> hips;
    for (int f = 0; f <= 40; ++f)
    {
        run("currentTime", {{"args", {sec(f)}}});
        hips[f] = pos("hips");
    }
    run("currentTime", {{"args", {sec(20)}}});
    run("stringsPose", {{"args", {"hips"}}, {"mode", "ik"}, {"by", v3(0, 0, 12)}, {"range", {sec(20), sec(20)}}, {"falloff", 4.0}, {"dense", true}});
    double last = 13.0;
    for (int f = 20; f <= 25; ++f)
    {
        run("currentTime", {{"args", {sec(f)}}});
        const double dz = pos("hips").z - hips[f].z;
        CAPTURE(f);
        CHECK(dz <= last + 1e-9);
        last = dz;
    }
    CHECK(last == doctest::Approx(0.0).scale(1.0).epsilon(1e-6));
    run("currentTime", {{"args", {sec(20)}}});
    CHECK(pos("hips").z - hips[20].z == doctest::Approx(12.0));

    // Absolute: every keyed frame's hips go to where the drag put them now.
    run("currentTime", {{"args", {sec(10)}}});
    const glm::dvec3 target = pos("hips") + glm::dvec3(5, 0, 0);
    run("stringsPose", {{"args", {"hips"}}, {"mode", "ik"}, {"by", v3(5, 0, 0)}, {"range", {sec(0), sec(40)}}, {"relative", false}});
    for (int f : {0, 10, 30, 40})
    {
        run("currentTime", {{"args", {sec(f)}}});
        CAPTURE(f);
        CHECK(near(pos("hips"), target, 1e-3));
    }
    CHECK_THROWS(run("stringsPose", {{"args", {"hips"}}, {"mode", "ik"}, {"by", v3(1, 0, 0)}, {"range", {sec(41), sec(45)}}}));
}

TEST_CASE("timeRange: the time slider's selection and its falloff weights")
{
    run("timeRange", {{"start", sec(10)}, {"end", sec(20)}, {"falloff", 5.0}});
    const lila::scene::anim::TimeSelection& s = lila::scene::anim::timeSelection();
    CHECK(s.active);
    CHECK(lila::scene::anim::timeSelectionWeight(s, sec(15)) == doctest::Approx(1.0));
    CHECK(lila::scene::anim::timeSelectionWeight(s, sec(22)) > 0.0);
    CHECK(lila::scene::anim::timeSelectionWeight(s, sec(22)) < 1.0);
    CHECK(lila::scene::anim::timeSelectionWeight(s, sec(26)) == 0.0);
    run("timeRange", {{"clear", true}});
    CHECK_FALSE(s.active);
}

TEST_CASE("spinner: the motion trail shows the joint's frames and dragging one poses that frame")
{
    loadPuppeteer();
    keyedBiped();
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    run("select", {{"args", {"hips"}}});
    ModeScope scope("puppeteer.strings.ik");
    ui::actions::run("puppeteer.strings.trail");
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    gz->setView(view());
    REQUIRE(gz->supports("rotate"));
    {
        std::vector<render::OverlayVertex> tris;
        ui::OverlayPainter p(tris, 1.0);
        gz->draw(p);
    }
    // Frame 30's hips, where the trail draws them.
    run("currentTime", {{"args", {sec(30)}}});
    const glm::dvec3 at30 = pos("hips");
    run("currentTime", {{"args", {sec(20)}}});
    const ui::ViewInfo v = view();
    const auto px = v.project(at30);
    REQUIRE(px);
    gz->setView(view());
    CHECK(gz->hover(*px));
    REQUIRE(gz->press(*px, Qt::LeftButton, Qt::NoModifier));
    gz->setView(view());
    gz->drag(*px + QPointF(0.0, 20.0), Qt::NoModifier);
    // Frames on the bob coincide on screen (10 and 30 are the same pose): whichever is nearest is dragged.
    REQUIRE(gz->readout().startsWith("Frame "));
    const int frame = gz->readout().section(' ', 1, 1).toInt();
    CHECK((frame >= 10 && frame <= 30));
    run("currentTime", {{"args", {sec(frame)}}});
    const glm::dvec3 before = pos("hips");
    run("currentTime", {{"args", {sec(20)}}});
    gz->release();
    run("currentTime", {{"args", {sec(frame)}}});
    CHECK(pos("hips").y < before.y - 1.0);
    run("currentTime", {{"args", {sec(10)}}});
    ui::actions::run("puppeteer.strings.trail");
}

TEST_CASE("stringsSmooth evens out a jittery spine over a range and the pinned feet stay")
{
    loadPuppeteer();
    stringsBiped();
    run("playbackOptions", {{"fps", 30.0}});
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    std::map<int, double> before;
    for (int f = 0; f <= 20; ++f)
    {
        run("currentTime", {{"args", {sec(f)}}});
        run("setAttr", {{"args", {"spine_01.rotateZ", (f % 2 ? 8.0 : -8.0)}}});
        run("setKeyframe", {{"args", {"spine_01", "foot_l", "foot_r", "hips"}}});
    }
    run("currentTime", {{"args", {sec(10)}}});
    const glm::dvec3 foot = pos("foot_l");
    const json r = run("stringsSmooth", {{"args", {"spine_01"}}, {"range", {sec(5), sec(15)}}, {"strength", 1.0}, {"radius", 2}});
    CHECK(r["frames"] == 11);
    for (int f = 6; f <= 14; ++f)
    {
        run("currentTime", {{"args", {sec(f)}}});
        CAPTURE(f);
        CHECK(std::abs(attr("spine_01.rotateZ")) < 4.0);
        CHECK(near(pos("foot_l"), foot, 1e-3));
    }
    run("currentTime", {{"args", {sec(2)}}});
    CHECK(std::abs(attr("spine_01.rotateZ")) == doctest::Approx(8.0));
    run("undo");
    run("currentTime", {{"args", {sec(10)}}});
    CHECK(std::abs(attr("spine_01.rotateZ")) == doctest::Approx(8.0));
}

TEST_CASE("stringsOvershoot carries the motion on; stringsBlend goes toward another frame's pose")
{
    loadPuppeteer();
    stringsBiped();
    run("playbackOptions", {{"fps", 30.0}});
    run("currentTime", {{"args", {sec(0)}}});
    run("setKeyframe", {{"args", {"spine_02"}}});
    run("currentTime", {{"args", {sec(10)}}});
    run("setAttr", {{"args", {"spine_02.rotateZ", 20.0}}});
    run("setKeyframe", {{"args", {"spine_02"}}});
    run("stringsOvershoot", {{"args", {"spine_02"}}, {"amount", 0.5}, {"mode", "forward"}});
    CHECK(attr("spine_02.rotateZ") == doctest::Approx(30.0).epsilon(1e-4));
    run("stringsBlend", {{"args", {"spine_02"}}, {"time", sec(0)}, {"weight", 0.5}, {"mode", "forward"}});
    CHECK(attr("spine_02.rotateZ") == doctest::Approx(15.0).epsilon(1e-4));
    run("stringsBlend", {{"args", {"spine_02"}}, {"mode", "forward"}});
    CHECK(attr("spine_02.rotateZ") == doctest::Approx(0.0).scale(1.0).epsilon(1e-6));
}

TEST_CASE("stringsBreakdown blends on arcs and keeps planted feet; stringsFill keys inbetweens spaced by the curve")
{
    loadPuppeteer();
    stringsBiped();
    run("playbackOptions", {{"fps", 30.0}});
    run("stringsRules", {{"args", {"hero"}}, {"contacts", {"foot_l", "foot_r"}}});
    const json all = {"hips", "spine_01", "spine_02", "neck", "head", "upperarm_l", "lowerarm_l", "hand_l", "upperarm_r",
                      "lowerarm_r", "hand_r", "thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r"};
    run("currentTime", {{"args", {sec(0)}}});
    run("setKeyframe", {{"args", all}});
    const glm::dvec3 foot = pos("foot_l");
    // The arm swings up 90 degrees by frame 10; the hips drop with the feet held.
    run("currentTime", {{"args", {sec(10)}}});
    run("stringsPose", {{"args", {"upperarm_l"}}, {"mode", "forward"}, {"axis", v3(0, 0, 1)}, {"angle", 90.0}});
    run("stringsPose", {{"args", {"hips"}}, {"mode", "ik"}, {"by", v3(0, -10, 0)}});
    run("setKeyframe", {{"args", all}});
    run("currentTime", {{"args", {sec(5)}}});
    // Between the keys the joints interpolate on their own and the foot drifts; the breakdown blends it in world.
    const double drift = glm::length(pos("foot_l") - foot);
    run("stringsBreakdown", {{"args", {"hips"}}, {"weight", 0.5}});
    CHECK(glm::length(pos("foot_l") - foot) <= drift * 0.5 + 1e-3);
    // On an arc: the hand stays the arm's length from the shoulder, where a straight blend would cut inside.
    CHECK(glm::length(pos("hand_l") - pos("upperarm_l")) == doctest::Approx(glm::length(glm::dvec3(72, 140, 0) - glm::dvec3(18, 140, 0))).epsilon(1e-3));

    const json f = run("stringsFill", {{"args", {"hips"}}, {"range", {sec(0), sec(10)}}, {"spacing", "linear"}, {"step", 2}});
    CHECK(f["filled"] == 4);
    run("currentTime", {{"args", {sec(4)}}});
    CHECK(near(pos("foot_l"), foot, 1e-3));
    CHECK(glm::length(pos("hand_l") - pos("upperarm_l")) == doctest::Approx(54.0).epsilon(0.01));
    run("currentTime", {{"args", {sec(20)}}});
    CHECK_THROWS(run("stringsBreakdown", {{"args", {"hips"}}, {"weight", 0.5}}));
}

namespace
{
/** A stroke from hips height curving forward over the head, world points. */
json forwardCurve(double scale = 1.0)
{
    json out = json::array();
    for (int i = 0; i <= 20; ++i)
    {
        const double a = glm::radians(80.0) * i / 20.0;
        out.push_back(v3(0.0, 100.0 + scale * 62.0 * std::sin(a) / glm::radians(80.0), scale * 62.0 * (1.0 - std::cos(a)) / glm::radians(80.0)));
    }
    return out;
}
} // namespace

TEST_CASE("lineOfAction: the body bends to a stroke, feet holding, the nearest line chosen, one undo")
{
    loadPuppeteer();
    stringsBiped();
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    const glm::dvec3 foot = pos("foot_l"), hips = pos("hips"), head = pos("head");
    const json r = run("lineOfAction", {{"args", {"hero"}}, {"stroke", forwardCurve(2.0)}});
    CHECK(r["line"] == "body");
    CHECK(r["error"].get<double>() < 1.0);
    CHECK(near(pos("foot_l"), foot, 1e-3));
    CHECK(near(pos("hips"), hips, 1e-3));
    // The head leans forward along the curve and keeps its distance along the spine.
    CHECK(pos("head").z > 20.0);
    CHECK(pos("head").y < head.y - 5.0);
    run("undo");
    CHECK(near(pos("head"), head, 1e-6));

    // A named limb line; the rules list the inferred lines.
    const json rules = run("stringsRules", {{"args", {"hero"}}});
    CHECK(rules["lines"].contains("body"));
    CHECK(run("lineOfAction", {{"args", {"hero"}}, {"joints", {"upperarm_l", "hand_l"}}, {"stroke", {v3(18, 140, 0), v3(40, 120, 20), v3(50, 100, 40)}}})["joints"].size() == 3);
    CHECK(pos("hand_l").y < 135.0);
}

TEST_CASE("lineOfAction across a range: travel slides the body along a path stroke, keyed per frame")
{
    loadPuppeteer();
    keyedBiped();
    // A long, gently curving path: the spine rides a window of it that slides forward over the range.
    json stroke = json::array();
    for (int i = 0; i <= 40; ++i)
        stroke.push_back(v3(0.0, 100.0 + 2.0 * i, 0.5 * i * i / 40.0));
    run("currentTime", {{"args", {sec(20)}}});
    const json r = run("lineOfAction", {{"args", {"hero"}}, {"line", "body"}, {"stroke", stroke}, {"travel", true}, {"range", {sec(10), sec(30)}}, {"dense", true}});
    CHECK(r["frames"].size() == 21);
    run("currentTime", {{"args", {sec(10)}}});
    const double early = pos("head").z;
    run("currentTime", {{"args", {sec(30)}}});
    const double late = pos("head").z;
    CHECK(late > early + 1.0);
}

#include "ui/ViewportTool.h"
#include <QMouseEvent>
#include <QWheelEvent>

TEST_CASE("Line of Action tool: a drawn stroke bends the body, the curve stays while the selection holds and dragging it re-poses")
{
    loadPuppeteer();
    stringsBiped();
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    run("select", {{"args", {"hips"}}});
    ui::ViewInfo v = view();
    v.eye = {0.0, 120.0, 400.0};
    v.view = glm::lookAt(v.eye, glm::dvec3(0.0, 120.0, 0.0), glm::dvec3(0, 1, 0));
    ui::actions::run("puppeteer.strings.lineOfAction");
    ui::ViewportTool* tool = ui::viewportTools::active();
    REQUIRE(tool);
    auto mouse = [&](QEvent::Type type, QPointF at) {
        QMouseEvent e(type, at, at, Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        if (type == QEvent::MouseButtonPress)
            return tool->mousePress(&e, v);
        if (type == QEvent::MouseMove)
            return tool->mouseMove(&e, v);
        return tool->mouseRelease(&e, v);
    };
    // From the hips up, curving to screen right: the spine leans along it.
    const auto hipsPx = v.project(pos("hips")), headPx = v.project(pos("head"));
    REQUIRE((hipsPx && headPx));
    const double h = headPx->y() - hipsPx->y();
    CHECK(mouse(QEvent::MouseButtonPress, *hipsPx));
    for (int i = 1; i <= 20; ++i)
    {
        const double t = i / 20.0;
        mouse(QEvent::MouseMove, *hipsPx + QPointF(60.0 * t * t, h * t));
    }
    mouse(QEvent::MouseButtonRelease, *hipsPx + QPointF(60.0, h));
    const glm::dvec3 bent = pos("head");
    CHECK(bent.x > 5.0);
    CHECK(near(pos("foot_l"), glm::dvec3(10, 8, 0), 1e-3));

    // Drag the curve's end further right: the body follows.
    const QPointF end = *hipsPx + QPointF(60.0, h);
    REQUIRE(mouse(QEvent::MouseButtonPress, end));
    mouse(QEvent::MouseMove, end + QPointF(30.0, 0.0));
    mouse(QEvent::MouseButtonRelease, end + QPointF(30.0, 0.0));
    CHECK(pos("head").x > bent.x + 2.0);
    ui::viewportTools::set(nullptr);
}

TEST_CASE("lineOfAction on a bend-bones arm: the bone CV lands on the stroke between the joints")
{
    loadPuppeteer();
    stringsBiped();
    const std::string arm = run("polyCylinder", {{"name", "Arm"}, {"radius", 3}, {"height", 54}, {"subdivisionsHeight", 12}})[0];
    run("rotate", {{"args", {arm}}, {"to", v3(0, 0, 90)}});
    run("move", {{"args", {arm}}, {"to", v3(45, 140, 0)}});
    const std::string d = run("bendBones", {{"args", {arm}}, {"chains", json::array({json::array({"upperarm_l", "lowerarm_l", "hand_l"})})}}).get<std::string>();
    const std::string cv = run("bendBoneCV", {{"args", {d}}, {"joint", "upperarm_l"}, {"mirror", false}}).get<std::string>();
    // A bow up between shoulder and hand.
    json stroke = json::array();
    for (int i = 0; i <= 20; ++i)
    {
        const double t = i / 20.0;
        stroke.push_back(v3(18.0 + 54.0 * t, 140.0 + 25.0 * std::sin(glm::pi<double>() * t), 0.0));
    }
    run("lineOfAction", {{"args", {"hero"}}, {"joints", json::array({"upperarm_l", "hand_l"})}, {"stroke", stroke}});
    CHECK(pos(cv).y > 150.0);
    CHECK(pos("lowerarm_l").y > 150.0);
}

TEST_CASE("spinner IK drag: the wheel sets how many bones bend, shown in the readout")
{
    loadPuppeteer();
    stringsBiped();
    ModeScope scope("puppeteer.strings.ik");
    ui::actions::run("puppeteer.spinner.move");
    run("select", {{"args", {"hand_l"}}});
    const glm::dvec3 spine = pos("spine_02");
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    REQUIRE(gz);
    gz->setView(view());
    gz->hover(kCenter);
    REQUIRE(gz->press(kCenter, Qt::LeftButton, Qt::NoModifier));
    gz->setView(view());
    gz->drag(kCenter + QPointF(0.0, 20.0), Qt::NoModifier);
    CHECK(gz->readout().contains("chain 3"));
    CHECK(gz->wheel(1));
    CHECK(gz->readout().contains("chain 4"));
    CHECK(glm::length(pos("spine_02") - spine) > 1e-3); // four bones reach into the spine
    CHECK(gz->wheel(-2));
    CHECK(gz->readout().contains("chain 2"));
    gz->release();
    CHECK(near(pos("spine_02"), spine, 1e-6));
    CHECK(run("stringsPose", {{"args", {"hand_l"}}, {"mode", "ik"}, {"by", v3(0, 5, 0)}, {"chainLength", 3}})["chain"].size() == 3);
    ui::actions::run("puppeteer.spinner.rotate");
}

TEST_CASE("stringsZero relativeTo: the spine zeroes and the head keeps its place; forward says it zeroed backward")
{
    loadPuppeteer();
    stringsBiped();
    run("rotate", {{"args", {"spine_01"}}, {"to", v3(0, 0, 20)}});
    run("rotate", {{"args", {"spine_02"}}, {"to", v3(0, 0, 15)}});
    const glm::dvec3 head = pos("head");
    const json r = run("stringsZero", {{"args", {"spine_01", "spine_02"}}, {"mode", "forward"}, {"relativeTo", {"head"}}});
    REQUIRE(r.size() == 2);
    CHECK(r[0].contains("fallback"));
    CHECK(near(pos("head"), head, 1e-3));
    const json rot = run("getAttr", {{"args", {"spine_02.rotate"}}});
    CHECK(std::abs(rot[2].get<double>()) < 1e-3);
}

TEST_CASE("stringsBreakdown parts and favour: an arm held at its own weight, extremities lagging the body")
{
    loadPuppeteer();
    stringsBiped();
    run("playbackOptions", {{"fps", 30.0}});
    const json all = {"hips", "spine_01", "spine_02", "neck", "head", "upperarm_l", "lowerarm_l", "hand_l"};
    run("currentTime", {{"args", {sec(0)}}});
    run("setKeyframe", {{"args", all}});
    run("currentTime", {{"args", {sec(10)}}});
    for (const char* j : {"spine_01", "upperarm_l", "lowerarm_l"})
        run("rotate", {{"args", {j}}, {"to", v3(0, 0, 30)}});
    run("setKeyframe", {{"args", all}});
    run("currentTime", {{"args", {sec(0)}}});
    const json armBefore = run("getAttr", {{"args", {"upperarm_l.rotate"}}});
    run("stringsBreakdown", {{"args", {"hips"}}, {"weight", 1.0}, {"parts", {{"upperarm_l", 0.0}}}});
    CHECK(run("getAttr", {{"args", {"upperarm_l.rotate"}}})[2].get<double>() == doctest::Approx(armBefore[2].get<double>()).epsilon(1e-6));
    CHECK(run("getAttr", {{"args", {"spine_01.rotate"}}})[2].get<double>() == doctest::Approx(30.0).epsilon(1e-3));
    run("undo");
    run("stringsBreakdown", {{"args", {"hips"}}, {"weight", 1.0}, {"favour", 1.0}});
    const double spine = run("getAttr", {{"args", {"spine_01.rotate"}}})[2].get<double>();
    const double fore = run("getAttr", {{"args", {"lowerarm_l.rotate"}}})[2].get<double>();
    CHECK(fore < spine - 1.0);
}

#include "scene/AnimLayer.h"

TEST_CASE("stringsPose across a range onto a layer: the base keeps its keys, the layer's weight takes the edit away")
{
    loadPuppeteer();
    keyedBiped();
    std::map<int, glm::dvec3> hips;
    for (int f : {10, 20, 30})
    {
        run("currentTime", {{"args", {sec(f)}}});
        hips[f] = pos("hips");
    }
    run("currentTime", {{"args", {sec(20)}}});
    run("stringsPose", {{"args", {"hips"}}, {"mode", "forward"}, {"by", v3(0, 0, 8)}, {"range", {sec(10), sec(30)}}, {"layer", "StringsEdit"}});
    CHECK(anim::activeLayer(graph()) == kNoNode);
    for (int f : {10, 20, 30})
    {
        run("currentTime", {{"args", {sec(f)}}});
        CAPTURE(f);
        CHECK(near(pos("hips"), hips[f] + glm::dvec3(0, 0, 8), 1e-3));
    }
    run("setAttr", {{"args", {"StringsEdit.weight", 0.0}}});
    for (int f : {10, 20, 30})
    {
        run("currentTime", {{"args", {sec(f)}}});
        CHECK(near(pos("hips"), hips[f], 1e-3));
    }
}

TEST_CASE("Line of Action tool: Shift + drag picks a path between two joints, Ctrl + wheel turns the plane")
{
    loadPuppeteer();
    stringsBiped();
    run("select", {{"args", {"hips"}}});
    ui::ViewInfo v = view();
    v.eye = {0.0, 120.0, 400.0};
    v.view = glm::lookAt(v.eye, glm::dvec3(0.0, 120.0, 0.0), glm::dvec3(0, 1, 0));
    ui::actions::run("puppeteer.strings.lineOfAction");
    ui::ViewportTool* tool = ui::viewportTools::active();
    REQUIRE(tool);
    auto mouse = [&](QEvent::Type type, QPointF at, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QMouseEvent e(type, at, at, Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, mods);
        if (type == QEvent::MouseButtonPress)
            return tool->mousePress(&e, v);
        if (type == QEvent::MouseMove)
            return tool->mouseMove(&e, v);
        return tool->mouseRelease(&e, v);
    };
    const QPointF hand = *v.project(pos("hand_l")), shoulder = *v.project(pos("upperarm_l"));
    // Picked from the hand up to the shoulder: the path still runs top first.
    mouse(QEvent::MouseButtonPress, hand, Qt::ShiftModifier);
    mouse(QEvent::MouseMove, (hand + shoulder) / 2.0, Qt::ShiftModifier);
    mouse(QEvent::MouseButtonRelease, shoulder, Qt::ShiftModifier);
    const glm::dvec3 spine = pos("spine_02");
    // A stroke from the shoulder bowing down: the arm bends to it, the spine stays.
    mouse(QEvent::MouseButtonPress, shoulder);
    for (int i = 1; i <= 20; ++i)
    {
        const double t = i / 20.0;
        mouse(QEvent::MouseMove, shoulder + QPointF((hand.x() - shoulder.x()) * t, 60.0 * std::sin(glm::pi<double>() * t)));
    }
    mouse(QEvent::MouseButtonRelease, hand);
    CHECK(pos("lowerarm_l").y < 135.0);
    CHECK(near(pos("spine_02"), spine, 1e-6));

    QWheelEvent w(QPointF(400, 300), QPointF(400, 300), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    CHECK(tool->wheel(&w, v));
    ui::viewportTools::set(nullptr);
}

TEST_CASE("stringsPose preview: ghosts and positions follow the drag across a range, nothing is keyed or left changed")
{
    loadPuppeteer();
    keyedBiped();
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    run("ghosting", {{"enable", true}, {"mode", "keys"}, {"before", 1}, {"after", 1}, {"show", "skeleton"}});
    run("select", {{"args", {"hips"}}});
    std::map<int, glm::dvec3> hips;
    for (int f : {10, 20, 30})
    {
        run("currentTime", {{"args", {sec(f)}}});
        hips[f] = pos("hips");
    }
    run("currentTime", {{"args", {sec(20)}}});
    const glm::dvec3 spine = pos("spine_02");
    auto ghostHips = [](double t) {
        for (const ghosts::Ghost& g : ghosts::cache().ghosts(ghosts::Settings::fromPrefs()))
            if (std::abs(g.frame->time - t) < 1e-6 && !g.frame->bones.empty())
                return g.frame->bones.front().first;
        return glm::dvec3(1e9);
    };
    CHECK(near(ghostHips(sec(10)), hips[10], 1e-6));

    const json r = run("stringsPose", {{"args", {"hips"}}, {"mode", "ik"}, {"by", v3(0, -10, 0)}, {"range", {sec(10), sec(30)}}, {"preview", true},
                                       {"show", {sec(15)}}});
    std::map<int, glm::dvec3> at;
    for (const json& f : r)
        at[int(std::lround(f["time"].get<double>() * 30.0))] = {f["at"][0][0].get<double>(), f["at"][0][1].get<double>(), f["at"][0][2].get<double>()};
    REQUIRE(at.contains(15));
    CHECK(near(at[10], hips[10] + glm::dvec3(0, -10, 0), 1e-3));
    CHECK(near(at[30], hips[30] + glm::dvec3(0, -10, 0), 1e-3));
    CHECK(ghosts::cache().previewing());
    CHECK(near(ghostHips(sec(10)), hips[10] + glm::dvec3(0, -10, 0), 1e-3));
    // Nothing changed: the frame, the unkeyed spine, the keys.
    CHECK(near(pos("hips"), hips[20], 1e-9));
    CHECK(near(pos("spine_02"), spine, 1e-9));
    run("currentTime", {{"args", {sec(10)}}});
    CHECK(near(pos("hips"), hips[10], 1e-9));
    run("currentTime", {{"args", {sec(20)}}});
    ghosts::cache().clearPreview();
    CHECK(near(ghostHips(sec(10)), hips[10], 1e-6));
    run("ghosting", {{"enable", false}});
}

TEST_CASE("spinner: a range drag previews on the ghosts while dragging and keys every frame on release")
{
    loadPuppeteer();
    keyedBiped();
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    run("ghosting", {{"enable", true}, {"mode", "keys"}, {"before", 1}, {"after", 1}, {"show", "skeleton"}});
    run("select", {{"args", {"hips"}}});
    ModeScope scope("puppeteer.strings.ik");
    run("currentTime", {{"args", {sec(10)}}});
    const glm::dvec3 hips10 = pos("hips");
    run("currentTime", {{"args", {sec(20)}}});
    run("timeRange", {{"start", sec(10)}, {"end", sec(30)}});
    ghosts::cache().ghosts(ghosts::Settings::fromPrefs());
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    ui::actions::run("puppeteer.spinner.move");
    const QPointF zone = arcPoint(u(110.0), -90.0);
    gz->setView(view());
    gz->hover(zone);
    REQUIRE(gz->press(zone, Qt::LeftButton, Qt::NoModifier));
    for (int i = 1; i <= 3; ++i)
    {
        gz->setView(view());
        gz->drag(zone + QPointF(0.0, 15.0 * i), Qt::NoModifier);
    }
    CHECK(ghosts::cache().previewing());
    glm::dvec3 ghost(1e9);
    for (const ghosts::Ghost& g : ghosts::cache().ghosts(ghosts::Settings::fromPrefs()))
        if (std::abs(g.frame->time - sec(10)) < 1e-6)
            ghost = g.frame->bones.front().first;
    CHECK(glm::length(ghost - hips10) > 1.0);
    CHECK(glm::length(ghost - hips10) < 100.0);
    gz->release();
    CHECK_FALSE(ghosts::cache().previewing());
    run("currentTime", {{"args", {sec(10)}}});
    CHECK(near(pos("hips"), ghost, 1e-4));
    run("timeRange", {{"clear", true}});
    run("ghosting", {{"enable", false}});
    ui::actions::run("puppeteer.spinner.rotate");
}

TEST_CASE("stringsRetime: dense motion resampled along the warp, the ends and every frame outside identical")
{
    loadPuppeteer();
    stringsBiped();
    run("playbackOptions", {{"fps", 30.0}});
    // Dense motion: the hips keyed on every frame, rising 1 cm a frame.
    for (int f = 0; f <= 40; ++f)
    {
        run("currentTime", {{"args", {sec(f)}}});
        run("move", {{"args", {"hips"}}, {"to", v3(0, 100 + f, 0)}});
        run("setKeyframe", {{"args", {"hips"}}});
    }
    std::map<int, glm::dvec3> before;
    for (int f = 0; f <= 40; ++f)
    {
        run("currentTime", {{"args", {sec(f)}}});
        before[f] = pos("hips");
    }
    const json r = run("stringsRetime", {{"args", {"hero"}}, {"range", {sec(10), sec(30)}}, {"easeIn", 1.0}, {"easeOut", 1.0}});
    CHECK(r["frames"].get<int>() == 21);
    CHECK(r["kept"].get<int>() == 0);
    for (int f = 0; f <= 40; ++f)
    {
        run("currentTime", {{"args", {sec(f)}}});
        CAPTURE(f);
        if (f <= 10 || f >= 30)
            CHECK(near(pos("hips"), before[f], 1e-9));
    }
    // Eased both ends: slow near the ends, the middle frame where it was (symmetric), faster through it.
    auto y = [&](int f) {
        run("currentTime", {{"args", {sec(f)}}});
        return pos("hips").y;
    };
    CHECK(y(11) - y(10) < 0.5);
    CHECK(y(20) == doctest::Approx(before[20].y).epsilon(1e-6));
    CHECK(y(21) - y(20) > 1.2);
    CHECK(y(30) - y(29) < 0.5);
    run("undo");
    CHECK(y(11) == doctest::Approx(before[11].y));

    // Warp points: the first half of the range plays the first quarter of the motion.
    run("stringsRetime", {{"args", {"hips"}}, {"range", {sec(10), sec(30)}}, {"warp", {{0.5, 0.25}}}});
    CHECK(y(20) == doctest::Approx(before[15].y).epsilon(1e-6));
    CHECK_THROWS(run("stringsRetime", {{"args", {"hips"}}, {"range", {sec(10), sec(30)}}, {"warp", {{0.5, 0.6}, {0.6, 0.4}}}}));
}

TEST_CASE("stringsRetime on sparse keys: the frames out to the neighbouring keys keep their pose")
{
    loadPuppeteer();
    keyedBiped();
    std::map<int, glm::dvec3> before;
    for (int f = 0; f <= 40; ++f)
    {
        run("currentTime", {{"args", {sec(f)}}});
        before[f] = pos("hips");
    }
    const json r = run("stringsRetime", {{"args", {"hips"}}, {"range", {sec(14), sec(26)}}, {"easeIn", 0.5}});
    CHECK(r["kept"].get<int>() == 6);
    for (int f = 0; f <= 40; ++f)
    {
        if (f > 14 && f < 26)
            continue;
        run("currentTime", {{"args", {sec(f)}}});
        CAPTURE(f);
        CHECK(near(pos("hips"), before[f], 1e-6));
    }
    // The range shows in the time slider's menu with a range selected.
    bool listed = false;
    for (const ui::menus::Item* item : ui::menus::itemsIn(ui::menus::kTimeSliderRange))
        listed = listed || item->action == "puppeteer.strings.retime";
    CHECK(listed);
}

TEST_CASE("stringsBreakdownParts: a brushed part takes its share of the slider's weight, cleared parts follow again")
{
    loadPuppeteer();
    stringsBiped();
    run("playbackOptions", {{"fps", 30.0}});
    const json all = {"hips", "spine_01", "spine_02", "neck", "head", "upperarm_l", "lowerarm_l", "hand_l"};
    run("currentTime", {{"args", {sec(0)}}});
    run("setKeyframe", {{"args", all}});
    run("currentTime", {{"args", {sec(10)}}});
    for (const char* j : {"spine_01", "upperarm_l"})
        run("rotate", {{"args", {j}}, {"to", v3(0, 0, 30)}});
    run("setKeyframe", {{"args", all}});
    run("currentTime", {{"args", {sec(0)}}});
    const json parts = run("stringsBreakdownParts", {{"args", {"upperarm_l"}}, {"factor", 0.5}});
    REQUIRE(parts.size() == 1);
    run("stringsBreakdown", {{"args", {"hips"}}, {"weight", 1.0}});
    CHECK(run("getAttr", {{"args", {"upperarm_l.rotate"}}})[2].get<double>() == doctest::Approx(15.0).epsilon(1e-3));
    CHECK(run("getAttr", {{"args", {"spine_01.rotate"}}})[2].get<double>() == doctest::Approx(30.0).epsilon(1e-3));
    run("undo");
    // brushed False ignores them; remove with no joints clears every part.
    run("stringsBreakdown", {{"args", {"hips"}}, {"weight", 1.0}, {"brushed", false}});
    CHECK(run("getAttr", {{"args", {"upperarm_l.rotate"}}})[2].get<double>() == doctest::Approx(30.0).epsilon(1e-3));
    run("undo");
    CHECK(run("stringsBreakdownParts", {{"remove", true}}).empty());
    CHECK(ui::actions::registry().find("puppeteer.strings.breakdownParts"));
}

TEST_CASE("spinner breakdown in a Strings mode: dragging down lags the extremities, and their arcs draw while dragging")
{
    loadPuppeteer();
    stringsBiped();
    run("playbackOptions", {{"fps", 30.0}});
    run("stringsRules", {{"args", {"hero"}}, {"addLine", {"arm", "upperarm_l", "hand_l"}}});
    const json all = {"hips", "spine_01", "spine_02", "neck", "head", "upperarm_l", "lowerarm_l", "hand_l"};
    run("currentTime", {{"args", {sec(0)}}});
    run("setKeyframe", {{"args", all}});
    run("currentTime", {{"args", {sec(10)}}});
    for (const char* j : {"spine_01", "upperarm_l", "lowerarm_l"})
        run("rotate", {{"args", {j}}, {"to", v3(0, 0, 30)}});
    run("setKeyframe", {{"args", all}});
    run("currentTime", {{"args", {sec(0)}}});
    run("select", {{"args", {"spine_01"}}});
    ModeScope scope("puppeteer.strings.fullBody");
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    auto drawn = [&] {
        std::vector<render::OverlayVertex> tris;
        ui::OverlayPainter p(tris, 1.0);
        gz->setView(view());
        gz->draw(p);
        return tris.size();
    };
    const size_t idle = drawn();
    const QPointF slider = arcPoint(u(177.0), 90.0);
    gz->setView(view());
    gz->hover(slider);
    REQUIRE(gz->press(slider, Qt::LeftButton, Qt::NoModifier));
    gz->setView(view());
    gz->drag(slider + QPointF(100.0, 0.0), Qt::NoModifier);
    const double spineFull = run("getAttr", {{"args", {"spine_01.rotate"}}})[2].get<double>();
    const double foreFull = run("getAttr", {{"args", {"lowerarm_l.rotate"}}})[2].get<double>();
    CHECK(drawn() > idle + 20);
    gz->setView(view());
    gz->drag(slider + QPointF(100.0, 100.0), Qt::NoModifier);
    CHECK(gz->readout().contains("lag"));
    const double fore = run("getAttr", {{"args", {"lowerarm_l.rotate"}}})[2].get<double>();
    CHECK(fore < foreFull - 1.0);
    gz->release();
    CHECK(spineFull == doctest::Approx(30.0).epsilon(1e-3));
    CHECK(run("getAttr", {{"args", {"lowerarm_l.rotate"}}})[2].get<double>() == doctest::Approx(fore).epsilon(1e-6));
    run("undo");
    CHECK(run("getAttr", {{"args", {"spine_01.rotate"}}})[2].get<double>() == doctest::Approx(0.0).scale(1.0).epsilon(1e-6));
}

#include "ui/Controls.h"
#include "ui/Panels.h"
#include <QLabel>
#include <QMouseEvent>
#include <QToolButton>

namespace
{
void mouse(QWidget* w, QEvent::Type type, QPointF at)
{
    QMouseEvent e(type, at, w->mapToGlobal(at), Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                  Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

void clickOn(QWidget* w, QPointF at)
{
    mouse(w, QEvent::MouseButtonPress, at);
    mouse(w, QEvent::MouseButtonRelease, at);
}

void dragOn(QWidget* w, QPointF from, QPointF to)
{
    mouse(w, QEvent::MouseButtonPress, from);
    mouse(w, QEvent::MouseMove, (from + to) * 0.5);
    mouse(w, QEvent::MouseMove, to);
    mouse(w, QEvent::MouseButtonRelease, to);
}

void pickLayer(QWidget* editor, const char* label)
{
    for (QToolButton* b : editor->findChildren<QToolButton*>())
        if (b->text() == label)
            b->click();
}
} // namespace

TEST_CASE("Strings Rules editor: contacts, limbs and lines toggle on the joint graph, and a test drag poses then puts back")
{
    loadPuppeteer();
    stringsBiped();
    run("select", {{"args", {"hips"}}});
    const ui::panels::Panel* panel = ui::panels::registry().find("puppeteerStringsRules");
    REQUIRE(panel);
    std::unique_ptr<QWidget> editor(panel->create(nullptr));
    editor->resize(420, 620);
    editor->show();
    QApplication::processEvents();
    QWidget* canvas = editor->findChild<QWidget*>("stringsRulesCanvas");
    REQUIRE(canvas);
    CHECK(canvas->property("jointCount").toInt() == 17);
    auto at = [&](const char* joint) { return canvas->property("joints").toMap()[joint].toPointF(); };
    auto rules = [] { return run("stringsRules", {{"args", {"hero"}}}); };

    // Contacts: a click toggles the joint.
    const size_t contacts = rules()["contacts"].size();
    clickOn(canvas, at("hand_l"));
    CHECK(rules()["contacts"].size() == contacts + 1);
    clickOn(canvas, at("hand_l"));
    CHECK(rules()["contacts"].size() == contacts);

    // Limbs: a drag from a joint to one below it adds one (either direction), a click on it removes it.
    pickLayer(editor.get(), "Limbs");
    const size_t limbs = rules()["chains"].size();
    dragOn(canvas, at("hand_r"), at("upperarm_r"));
    REQUIRE(rules()["chains"].size() == limbs + 1);
    CHECK(rules()["chains"].back().front() == "upperarm_r");
    CHECK(canvas->property("limbCount").toInt() == int(limbs + 1));
    clickOn(canvas, at("lowerarm_r"));
    CHECK(rules()["chains"].size() == limbs);

    // Lines: the same gestures.
    pickLayer(editor.get(), "Lines");
    dragOn(canvas, at("spine_01"), at("head"));
    CHECK(rules()["lines"].contains("head"));
    clickOn(canvas, at("neck"));
    CHECK_FALSE(rules()["lines"].contains("head"));

    // Test: IK drags the hand while the mouse is down; release puts every joint back with nothing on the undo stack.
    ModeScope scope("puppeteer.strings.ik");
    pickLayer(editor.get(), "Test");
    const glm::dvec3 hand = pos("hand_l");
    const json undo = run("undoInfo")["undo"];
    mouse(canvas, QEvent::MouseButtonPress, at("hand_l"));
    mouse(canvas, QEvent::MouseMove, at("hand_l") + QPointF(0.0, -40.0));
    CHECK(pos("hand_l").y > hand.y + 5.0);
    const QLabel* status = editor->findChild<QLabel*>("stringsRulesStatus");
    REQUIRE(status);
    CHECK(status->text().startsWith("Test IK"));
    mouse(canvas, QEvent::MouseButtonRelease, at("hand_l") + QPointF(0.0, -40.0));
    CHECK(near(pos("hand_l"), hand, 1e-9));
    CHECK(run("undoInfo")["undo"] == undo);
}

#include <filesystem>
#include <fstream>

TEST_CASE("stringsRuleWeb and unrealStringsRig: the rule web by bone name, written to a file and sent with a control per line end")
{
    loadPuppeteer();
    stringsBiped();
    run("stringsRules", {{"args", {"hero"}}, {"contacts", {"foot_l", "foot_r"}}, {"addChain", {"upperarm_l", "hand_l"}}});
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "lila_rule_web_test.json";
    const json web = run("stringsRuleWeb", {{"args", {"hand_l"}}, {"file", file.string()}});
    CHECK(web["version"] == 2);
    CHECK(web["hips"] == "hips");
    CHECK(web["contacts"] == json({"foot_l", "foot_r"}));
    CHECK(web["chains"].back() == json({"upperarm_l", "lowerarm_l", "hand_l"}));
    CHECK(web["bones"].size() == 17);
    // Unreal solves the same problem: the bones in Strings' order (parents first) and its length scale.
    CHECK(web["bones"][0]["name"] == "hips");
    CHECK(web["lengthScale"].get<double>() > 1.0);
    for (const json& b : web["bones"])
        if (b["name"] == "lowerarm_l")
            CHECK(b["parent"] == "upperarm_l");
    std::ifstream in(file);
    REQUIRE(in);
    CHECK(json::parse(in) == web);
    in.close();
    std::filesystem::remove(file);

    const json sent = run("unrealStringsRig", {{"args", {"hero"}}, {"skeletalMesh", "/Game/Hero/SK_Hero"}, {"dryRun", true}});
    CHECK(sent["rig"] == "/Game/Hero/CR_Hero_Strings");
    CHECK(sent["payload"]["ruleWeb"] == web);
    const json controls = sent["payload"]["controls"];
    CHECK(controls.front() == "hips");
    CHECK(std::find(controls.begin(), controls.end(), json("hand_l")) != controls.end());
    CHECK_THROWS(run("unrealStringsRig", {{"args", {"hero"}}, {"dryRun", true}}));
}

TEST_CASE("stringsRuleWeb: pins, IK chains with their lock and solve, and glue among the skeleton's bones go to Unreal")
{
    loadPuppeteer();
    stringsBiped();
    run("stringsGlue", {{"remove", true}});
    run("puppeteerPin", {{"args", {"hand_r"}}, {"pin", true}});
    const json chain = run("ikChain", {{"args", {"upperarm_l", "hand_l"}}});
    run("ikLock", {{"args", {chain}}, {"state", true}});
    run("stringsGlue", {{"args", {"hand_l", "hand_r"}}});
    run("spaceLocator", {{"name", "cup"}});
    run("stringsGlue", {{"args", {"foot_l", "cup"}}, {"attach", true}});
    const json web = run("stringsRuleWeb", {{"args", {"hero"}}});
    CHECK(web["pins"] == json({"hand_r"}));
    REQUIRE(web["locks"].size() == 1);
    const json lock = web["locks"][0];
    CHECK(lock["bone"] == "hand_l");
    CHECK(lock["locked"] == true);
    CHECK(lock["solver"] == "twoBone");
    CHECK(lock["alignWeight"] == 1.0);
    CHECK(lock["chain"] == json({"upperarm_l", "lowerarm_l", "hand_l"}));
    CHECK(lock["slots"].size() == 3);
    CHECK(lock.contains("pole"));
    CHECK(!lock.contains("unsolved"));
    // A joint limit and preferred angle go with the chain's slots.
    run("ikLimit", {{"args", {"lowerarm_l"}}, {"type", "hinge"}, {"axis", "z"}, {"min", -30.0}, {"max", 30.0}, {"preferredAngle", v3(0, 0, 20)}});
    run("ikHandle", {{"args", run("ls", {{"type", "ikHandle"}})}, {"refresh", true}});
    const json slot = run("stringsRuleWeb", {{"args", {"hero"}}})["locks"][0]["slots"][1];
    CHECK(slot["limit"]["kind"] == "hinge");
    CHECK(slot["limit"]["max"].get<double>() == doctest::Approx(glm::radians(30.0)));
    CHECK(slot.contains("preferred"));
    // The glue between the hands goes by name; the prop attached to a foot goes as a frame of its own, and leads as attach's last.
    REQUIRE(web["glue"].size() == 2);
    CHECK(web["glue"][0] == json({{"members", {"hand_l", "hand_r"}}, {"attach", false}, {"lead", 0}}));
    CHECK(web["glue"][1]["lead"] == 1);
    CHECK(web["glue"][1]["members"][0] == "foot_l");
    CHECK(web["glue"][1]["members"][1]["prop"].get<std::string>().ends_with("cup"));
    CHECK(web["glue"][1]["members"][1]["location"].size() == 3);
    CHECK(web["basis"]["up"].size() == 3);

    const json sent = run("unrealStringsRig", {{"args", {"hero"}}, {"skeletalMesh", "/Game/Hero/SK_Hero"}, {"dryRun", true}});
    const json controls = sent["payload"]["controls"];
    for (const char* b : {"hand_r", "hand_l"})
        CHECK(std::find(controls.begin(), controls.end(), json(b)) != controls.end());

    run("ikLock", {{"args", {chain}}, {"state", false}});
    CHECK(run("stringsRuleWeb", {{"args", {"hero"}}})["locks"][0]["locked"] == false);
    // A keyed lock goes as its on and off steps, for the rig's Lock channel in a Level Sequence.
    run("ikLock", {{"args", {chain}}, {"time", 0.0}, {"state", true}});
    run("ikLock", {{"args", {chain}}, {"time", 1.0}, {"state", false}});
    const json keys = run("stringsRuleWeb", {{"args", {"hero"}}})["locks"][0]["keys"];
    REQUIRE(keys.size() == 2);
    CHECK(keys[0][1] == true);
    CHECK(keys[1][1] == false);
    CHECK(keys[1][0].get<double>() == doctest::Approx(1.0));
    // Glue is kept by joint name for the session: free everything, with nothing selected, so later tests start clean.
    run("select", {{"clear", true}});
    run("stringsGlue", {{"remove", true}});
    CHECK(run("stringsGlue", {{"query", true}}).empty());
    run("puppeteerPin", {{"args", {"hand_r"}}, {"pin", false}});
}

TEST_CASE("unrealStringsRig: the rule web carries isolate and modes, and the controls start in the mode and Isolate Rotation given")
{
    loadPuppeteer();
    stringsBiped();
    run("stringsRules", {{"args", {"hero"}}, {"contacts", {"foot_l", "foot_r"}}, {"isolate", {"head", "hand_l"}}, {"defaultMode", {"neck", "backward"}}});
    const json web = run("stringsRuleWeb", {{"args", {"hero"}}});
    CHECK(web["isolate"] == json({"head", "hand_l"}));
    CHECK(web["modes"] == json({{"neck", "backward"}}));

    // Unset, the controls start in the current Strings mode with the Isolate Rotation setting.
    const json plain = run("unrealStringsRig", {{"args", {"hero"}}, {"skeletalMesh", "/Game/Hero/SK_Hero"}, {"dryRun", true}});
    CHECK(plain["payload"]["mode"].is_string());
    CHECK(lila::scene::strings::modeFromName(plain["payload"]["mode"].get<std::string>()).has_value());
    CHECK(plain["payload"]["isolate"].is_boolean());

    for (const lila::scene::strings::Mode m : lila::scene::strings::kModes)
    {
        const std::string name = lila::scene::strings::modeName(m);
        const json sent = run("unrealStringsRig", {{"args", {"hero"}}, {"skeletalMesh", "/Game/Hero/SK_Hero"}, {"mode", name}, {"isolate", true}, {"dryRun", true}});
        CHECK(sent["payload"]["mode"] == name);
        CHECK(sent["payload"]["isolate"] == true);
        CHECK(sent["payload"]["ruleWeb"] == web);
    }
    CHECK_THROWS(run("unrealStringsRig", {{"args", {"hero"}}, {"skeletalMesh", "/Game/Hero/SK_Hero"}, {"mode", "sideways"}, {"dryRun", true}}));
}

namespace
{
/** Hips walking forward 2 cm a frame from frame 0 to 40, keyed every 10, the legs keyed with them. */
void walkingBiped()
{
    stringsBiped();
    run("playbackOptions", {{"fps", 30.0}, {"minTime", 0.0}, {"maxTime", sec(120)}});
    for (int f : {0, 10, 20, 30, 40})
    {
        run("currentTime", {{"args", {sec(f)}}});
        run("move", {{"args", {"hips"}}, {"to", v3(0, 100, 2.0 * f)}});
        run("setKeyframe", {{"args", {"hips", "thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r", "spine_01"}}});
    }
    run("currentTime", {{"args", {sec(20)}}});
}

glm::dvec3 hipsAt(int f)
{
    run("currentTime", {{"args", {sec(f)}}});
    return pos("hips");
}
} // namespace

TEST_CASE("timeRange: falloff types, a falloff per side, every; the frames a range edit keys")
{
    namespace an = lila::scene::anim;
    run("timeRange", {{"start", sec(10)}, {"end", sec(20)}, {"falloffBefore", 4.0}, {"falloffAfter", 9.0}, {"falloffType", "linear"}, {"every", 2}});
    const an::TimeSelection& s = an::timeSelection();
    CHECK(s.before() == 4.0);
    CHECK(s.after() == 9.0);
    // Linear: frame 22 is two of the ten steps out past the end.
    CHECK(an::timeSelectionWeight(s, sec(22)) == doctest::Approx(0.8));
    CHECK(an::timeSelectionWeight(s, sec(8)) == doctest::Approx(0.6));
    CHECK(an::timeSelectionWeight(s, sec(5)) == 0.0);
    CHECK(an::timeSelectionWeight(s, sec(29)) > 0.0);
    CHECK(an::timeSelectionWeight(s, sec(30)) == 0.0);
    for (an::RangeFalloff f : {an::RangeFalloff::Smooth, an::RangeFalloff::Linear, an::RangeFalloff::Sharp, an::RangeFalloff::Soft, an::RangeFalloff::Constant})
    {
        CAPTURE(an::rangeFalloffName(f));
        CHECK(an::rangeFalloffWeight(f, 1.0) == doctest::Approx(1.0));
        CHECK(an::rangeFalloffWeight(f, 0.0) == doctest::Approx(0.0));
        CHECK(an::rangeFalloffFromName(an::rangeFalloffName(f)) == f);
    }
    // Sharp drops faster than linear, soft slower, constant holds.
    CHECK(an::rangeFalloffWeight(an::RangeFalloff::Sharp, 0.5) < 0.5);
    CHECK(an::rangeFalloffWeight(an::RangeFalloff::Soft, 0.5) > 0.5);
    CHECK(an::rangeFalloffWeight(an::RangeFalloff::Constant, 0.1) == 1.0);
    // Every 2 from the falloff's start, the range's ends, the last eased frames and the keys given.
    const std::vector<double> frames = an::timeSelectionFrames(s, {sec(13)});
    std::vector<int> fr;
    for (double t : frames)
        fr.push_back(int(std::lround(an::frameOf(t))));
    CHECK(fr.front() == 6);
    CHECK(fr.back() == 29);
    for (int must : {10, 13, 20, 29})
        CHECK(std::find(fr.begin(), fr.end(), must) != fr.end());
    CHECK(run("timeRange", {{"falloff", 3.0}})["falloffAfter"] == 3.0);
    CHECK_THROWS(run("timeRange", {{"falloffType", "wobbly"}}));
    run("timeRange", {{"clear", true}, {"every", 0}, {"falloffType", "smooth"}, {"falloff", 0.0}});
}

TEST_CASE("stringsPose across a range: pivot swings the hips' path about where they are now, local turns them in place")
{
    loadPuppeteer();
    walkingBiped();
    std::map<int, glm::dvec3> before;
    for (int f : {0, 10, 30, 40})
        before[f] = hipsAt(f);
    run("currentTime", {{"args", {sec(20)}}});
    const glm::dvec3 now = pos("hips");
    const json r = run("stringsPose", {{"args", {"hips"}}, {"mode", "forwardAll"}, {"axis", v3(0, 1, 0)}, {"angle", 90.0}, {"range", {sec(0), sec(40)}},
                                       {"rotationType", "pivot"}});
    CHECK(r.size() == 5);
    for (int f : {0, 10, 30, 40})
    {
        CAPTURE(f);
        const glm::dvec3 d = before[f] - now;
        // A quarter turn about Y: z becomes x.
        CHECK(near(hipsAt(f), now + glm::dvec3(d.z, d.y, -d.x), 1e-4));
    }
    run("undo");
    run("currentTime", {{"args", {sec(20)}}});
    run("stringsPose", {{"args", {"hips"}}, {"mode", "forwardAll"}, {"axis", v3(0, 1, 0)}, {"angle", 90.0}, {"range", {sec(0), sec(40)}},
                        {"rotationType", "local"}});
    for (int f : {0, 40})
        CHECK(near(hipsAt(f), before[f], 1e-6));
    // A mid joint has no place of its own to swing: pivot turns it in place and says so.
    run("undo");
    const json mid = run("stringsPose", {{"args", {"spine_01"}}, {"mode", "forward"}, {"axis", v3(1, 0, 0)}, {"angle", 20.0}, {"range", {sec(0), sec(40)}},
                                         {"rotationType", "pivot"}});
    CHECK(mid[0]["pivot"] == "in place");
}

TEST_CASE("stringsPose across a range: every n keys between the keys, eased by the falloff type, and leaves the frames past it alone")
{
    loadPuppeteer();
    walkingBiped();
    std::map<int, glm::dvec3> before;
    for (int f = 0; f <= 40; ++f)
        before[f] = hipsAt(f);
    run("currentTime", {{"args", {sec(20)}}});
    run("stringsPose", {{"args", {"hips"}}, {"mode", "ik"}, {"by", v3(0, 0, 10)}, {"range", {sec(18), sec(22)}}, {"falloff", 6.0}, {"falloffType", "linear"},
                        {"every", 2}});
    // Linear: frame 26 is four of the seven steps out, so it takes three sevenths of the move.
    CHECK(hipsAt(26).z - before[26].z == doctest::Approx(10.0 * 3.0 / 7.0).epsilon(1e-4));
    CHECK(hipsAt(20).z - before[20].z == doctest::Approx(10.0).epsilon(1e-6));
    // Frames past the falloff keep their pose: the edges were keyed.
    for (int f : {0, 10, 30, 40})
    {
        CAPTURE(f);
        CHECK(near(hipsAt(f), before[f], 1e-6));
    }
}

TEST_CASE("stringsPose preview: solved in memory it matches what the keys give, and 120 dense frames stay interactive")
{
    loadPuppeteer();
    stringsBiped();
    run("stringsGlue", {{"remove", true}});
    run("playbackOptions", {{"fps", 30.0}, {"minTime", 0.0}, {"maxTime", sec(130)}});
    for (int f = 0; f <= 120; ++f)
    {
        run("currentTime", {{"args", {sec(f)}}});
        run("move", {{"args", {"hips"}}, {"to", v3(0, 100 - 3.0 * std::sin(f * 0.3), 1.5 * f)}});
        run("setKeyframe", {{"args", {"hips", "thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r", "spine_01", "spine_02"}}});
    }
    run("puppeteerPin", {{"args", {"foot_l"}}, {"pin", true}});
    run("currentTime", {{"args", {sec(60)}}});
    json call = {{"args", {"hips"}}, {"mode", "ik"}, {"by", v3(0, -6, 2)}, {"range", {sec(0), sec(120)}}, {"every", 1}, {"preview", true}};
    auto t0 = std::chrono::steady_clock::now();
    run("stringsPose", call);
    const double first = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    json r;
    // The fastest of several updates: other processes (a parallel test run) only ever slow some of them down, while a
    // real regression (the frames solved one after another, or re-evaluating the scene) slows every one.
    double later = 1e30, mean = 0.0;
    for (int i = 1; i <= 7; ++i)
    {
        call["by"] = v3(0, -6.0 - i, 2);
        t0 = std::chrono::steady_clock::now();
        r = run("stringsPose", call);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        later = std::min(later, ms);
        mean += ms / 7.0;
    }
    MESSAGE("range preview of 121 frames: first " << first << " ms, then " << later << " ms (fastest), " << mean << " ms (mean) per drag update");
    CHECK(r.size() == 121);
    CHECK(later < 100.0);
    std::map<int, glm::dvec3> previewed;
    for (const json& f : r)
        previewed[int(std::lround(f["time"].get<double>() * 30.0))] = {f["at"][0][0].get<double>(), f["at"][0][1].get<double>(), f["at"][0][2].get<double>()};
    call.erase("preview");
    run("stringsPose", call);
    for (int f : {0, 30, 60, 90, 120})
    {
        CAPTURE(f);
        CHECK(near(hipsAt(f), previewed[f], 1e-4));
    }
}

TEST_CASE("stringsTween: the range goes onto the inbetween of the poses either side, eased out across the falloff")
{
    loadPuppeteer();
    walkingBiped();
    // A bump in the middle of a straight walk: frame 20 lifted.
    run("currentTime", {{"args", {sec(20)}}});
    run("move", {{"args", {"hips"}}, {"to", v3(0, 112, 40)}});
    run("setKeyframe", {{"args", {"hips"}}});
    const json r = run("stringsTween", {{"args", {"hips"}}, {"range", {sec(20), sec(20)}}, {"falloff", 9.0}, {"amount", 1.0}});
    CHECK(r["tweened"].get<int>() >= 1);
    // The keys around frame 20 lie between frames 10 and 30's poses: the bump is gone at full weight.
    CHECK(hipsAt(20).y == doctest::Approx(100.0).epsilon(1e-6));
    run("undo");
    CHECK(hipsAt(20).y == doctest::Approx(112.0).epsilon(1e-9));
    // Half way: half the bump.
    run("currentTime", {{"args", {sec(20)}}});
    run("stringsTween", {{"args", {"hips"}}, {"range", {sec(20), sec(20)}}, {"falloff", 9.0}, {"amount", 0.5}});
    CHECK(hipsAt(20).y == doctest::Approx(106.0).epsilon(1e-6));
    // Preview: the frame shows it, nothing is keyed.
    run("undo");
    run("currentTime", {{"args", {sec(20)}}});
    run("timeRange", {{"start", sec(20)}, {"end", sec(20)}, {"falloff", 9.0}});
    run("stringsTween", {{"args", {"hips"}}, {"amount", 1.0}, {"preview", true}});
    CHECK(pos("hips").y == doctest::Approx(100.0).epsilon(1e-6));
    hipsAt(19);
    CHECK(hipsAt(20).y == doctest::Approx(112.0).epsilon(1e-9));
    run("timeRange", {{"clear", true}, {"falloff", 0.0}});
}

TEST_CASE("stringsMirror: the arm takes the other side's pose through the mode, pinned feet stay")
{
    loadPuppeteer();
    stringsBiped();
    run("rotate", {{"args", {"upperarm_r"}}, {"to", v3(0, 0, -40)}});
    run("rotate", {{"args", {"lowerarm_r"}}, {"to", v3(0, 30, 0)}});
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    const glm::dvec3 feet = pos("foot_l");
    const glm::dvec3 hand = pos("hand_l");
    const json r = run("stringsMirror", {{"args", {"upperarm_l", "lowerarm_l"}}, {"mode", "forward"}});
    CHECK(r["mode"] == "forward");
    // Joints keep the other side's rotation, the way mirrored joints behave.
    CHECK(near(graph().node(id("upperarm_l"))->as<glm::dvec3>(Transform::kRotate), glm::dvec3(0, 0, -40), 1e-6));
    CHECK(near(graph().node(id("lowerarm_l"))->as<glm::dvec3>(Transform::kRotate), glm::dvec3(0, 30, 0), 1e-6));
    CHECK(glm::length(pos("hand_l") - hand) > 5.0);
    CHECK(near(pos("foot_l"), feet, 1e-6));
    run("undo");
    CHECK(near(pos("hand_l"), hand, 1e-9));
}

TEST_CASE("curveRangeEdit: an offset eases across the falloff and only the range's keys move")
{
    loadPuppeteer();
    walkingBiped();
    std::map<int, glm::dvec3> before;
    for (int f = 0; f <= 40; ++f)
        before[f] = hipsAt(f);
    run("currentTime", {{"args", {sec(20)}}});
    const json r = run("curveRangeEdit", {{"channels", {"hips.translateY"}}, {"offset", -8.0}, {"range", {sec(20), sec(20)}}, {"falloff", 10.0}});
    CHECK(r["channels"].size() == 1);
    CHECK(hipsAt(20).y - before[20].y == doctest::Approx(-8.0));
    // Frame 10, a key, sits in the smooth falloff.
    const double w10 = lila::scene::anim::rangeFalloffWeight(lila::scene::anim::RangeFalloff::Smooth, 1.0 - 10.0 / 11.0);
    CHECK(hipsAt(10).y - before[10].y == doctest::Approx(-8.0 * w10).epsilon(1e-6));
    CHECK(near(hipsAt(0), before[0], 1e-9));
    CHECK(near(hipsAt(20) - glm::dvec3(0, hipsAt(20).y, 0), before[20] - glm::dvec3(0, before[20].y, 0), 1e-9));
    run("undo");
    CHECK(near(hipsAt(20), before[20], 1e-9));
    CHECK_THROWS(run("curveRangeEdit", {{"channels", {"hips.translate"}}, {"offset", 1.0}, {"range", {sec(20), sec(20)}}}));
}

TEST_CASE("curveRangeEdit: a tween puts every frame of the range on the line between the values either side")
{
    loadPuppeteer();
    walkingBiped();
    run("currentTime", {{"args", {sec(20)}}});
    run("move", {{"args", {"hips"}}, {"to", v3(0, 112, 40)}});
    run("setKeyframe", {{"args", {"hips"}}});
    const double y14 = hipsAt(14).y, y26 = hipsAt(26).y;
    run("currentTime", {{"args", {sec(20)}}});
    run("curveRangeEdit", {{"channels", {"hips.translateY"}}, {"tween", 1.0}, {"range", {sec(15), sec(25)}}, {"every", 1}});
    for (int f : {15, 20, 25})
    {
        CAPTURE(f);
        CHECK(hipsAt(f).y == doctest::Approx(y14 + (y26 - y14) * (f - 14) / 12.0).epsilon(1e-6));
    }
    CHECK(hipsAt(14).y == doctest::Approx(y14).epsilon(1e-9));
}

TEST_CASE("curveRangeEdit: pivot turns the hips so their whole path swings about where they are now")
{
    loadPuppeteer();
    walkingBiped();
    const glm::dvec3 end = hipsAt(40);
    run("currentTime", {{"args", {sec(20)}}});
    const glm::dvec3 now = pos("hips");
    const json p = run("curveRangeEdit", {{"channels", {"hips.rotateY"}}, {"offset", 90.0}, {"range", {sec(0), sec(40)}}, {"rotationType", "pivot"}});
    CHECK(p["pivot"].size() == 1);
    CHECK(p["channels"].empty());
    CHECK(glm::length(hipsAt(40) - now) == doctest::Approx(glm::length(end - now)).epsilon(1e-4));
    CHECK(glm::length(hipsAt(40) - end) > 10.0);
}

TEST_CASE("ghosts across a range: spread over it and its falloff, at most sixteen, fading with the weight")
{
    loadPuppeteer();
    walkingBiped();
    run("ghosting", {{"enable", true}, {"mode", "frames"}, {"step", 1}, {"show", "skeleton"}, {"range", true}});
    run("select", {{"args", {"hips"}}});
    run("timeRange", {{"start", sec(10)}, {"end", sec(30)}, {"falloff", 5.0}});
    run("currentTime", {{"args", {sec(20)}}});
    const auto times = ghosts::cache().shownTimes(ghosts::Settings::fromPrefs());
    CHECK(times.size() <= size_t(ghosts::kMaxRangeGhosts));
    CHECK(times.size() >= 10);
    double lo = 1e9, hi = -1e9;
    for (const auto& [t, offset] : times)
    {
        lo = std::min(lo, t);
        hi = std::max(hi, t);
        CHECK((offset < 0) == (t < sec(20)));
    }
    CHECK(lila::scene::anim::frameOf(lo) <= 6.0);
    CHECK(lila::scene::anim::frameOf(hi) >= 34.0);
    bool faded = false;
    for (const ghosts::Ghost& g : ghosts::cache().ghosts(ghosts::Settings::fromPrefs()))
        faded = faded || g.weight < 1.0;
    CHECK(faded);
    run("timeRange", {{"clear", true}, {"falloff", 0.0}});
    const ghosts::Settings around = ghosts::Settings::fromPrefs();
    CHECK(ghosts::cache().shownTimes(around).size() == size_t(around.before + around.after));
    run("ghosting", {{"enable", false}, {"mode", "keys"}});
}

TEST_CASE("stringsRules: isolate and default modes per joint, which Default and Isolate Rotation pose through")
{
    loadPuppeteer();
    stringsBiped();
    json rules = run("stringsRules", {{"args", {"hero"}}});
    CHECK(rules["isolateFrom"] == "inferred");
    CHECK(std::find(rules["isolate"].begin(), rules["isolate"].end(), json("head")) != rules["isolate"].end());
    rules = run("stringsRules", {{"args", {"hero"}}, {"defaultMode", {"calf_l", "backward"}}, {"isolate", {"head"}}});
    CHECK(rules["modes"]["calf_l"] == "backward");
    CHECK(rules["isolate"] == json::array({"head"}));
    CHECK_THROWS(run("stringsRules", {{"args", {"hero"}}, {"defaultMode", {"calf_l", "sideways"}}}));

    // Default: the knee's rule poses it Backward, so the pinned foot stays.
    run("puppeteerPin", {{"args", {"foot_l", "foot_r"}}, {"pin", true}});
    const glm::dmat4 foot = world("foot_l");
    const json r = run("stringsPose", {{"args", {"calf_l"}}, {"mode", "default"}, {"axis", v3(1, 0, 0)}, {"angle", 20.0}});
    CHECK(r["resolved"] == "backward");
    CHECK(near(world("foot_l"), foot, 1e-5));
    run("undo");

    // Isolate Rotation keeps the head level as the spine turns Forward.
    const glm::dmat4 head = world("head");
    ui::actions::run("puppeteer.strings.isolate");
    CHECK(checked("puppeteer.strings.isolate"));
    run("stringsPose", {{"args", {"spine_01"}}, {"mode", "forward"}, {"axis", v3(1, 0, 0)}, {"angle", 25.0}});
    const glm::dmat3 a = glm::dmat3(world("head")), b = glm::dmat3(head);
    CHECK(glm::length(a[0] - b[0]) + glm::length(a[1] - b[1]) + glm::length(a[2] - b[2]) < 1e-6);
    ui::actions::run("puppeteer.strings.isolate");
    CHECK_FALSE(checked("puppeteer.strings.isolate"));
    run("stringsRules", {{"args", {"hero"}}, {"reset", true}});
}

TEST_CASE("curve overlay draw: the range's ramps and its row, saved for a look when LILA_SPINNER_PNG names a folder")
{
    const char* dir = std::getenv("LILA_SPINNER_PNG");
    if (!dir || !qApp)
        return;
    loadPuppeteer();
    walkingBiped();
    run("select", {{"args", {"hips"}}});
    run("timeRange", {{"start", sec(15)}, {"end", sec(25)}, {"falloffBefore", 6.0}, {"falloffAfter", 12.0}, {"falloffType", "smooth"}, {"every", 1}});
    QWidget widget;
    widget.resize(1100, 640);
    ui::actions::run("puppeteer.curveOverlay");
    render::FrameState frame;
    const ui::ViewInfo v = [] {
        ui::ViewInfo info = view();
        info.width = 1100;
        info.height = 640;
        return info;
    }();
    std::vector<render::OverlayVertex> tris;
    ui::OverlayPainter p(tris, 1.0);
    ui::renderPasses::Context c{.frame = frame, .painter = p, .view = v, .widget = &widget};
    ui::renderPasses::run(ui::renderPasses::Stage::Scene, c);
    tris.clear();
    ui::renderPasses::run(ui::renderPasses::Stage::Overlay, c);
    CHECK(!tris.empty());
    QImage img(1100, 640, QImage::Format_RGB32);
    img.fill(QColor(70, 72, 76));
    rasterise(img, tris, p.atlas().get());
    img.save(QString("%1/overlay_range.png").arg(dir));
    ui::actions::run("puppeteer.curveOverlay");
    run("timeRange", {{"clear", true}, {"falloff", 0.0}, {"every", 0}});
}
