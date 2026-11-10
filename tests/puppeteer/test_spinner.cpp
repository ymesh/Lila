#include <doctest/doctest.h>

#include "TestUtil.h"
#include "rig/RigMath.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "extensions/ExtensionManager.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "ui/Gizmo.h"
#include "ui/Modes.h"
#include "ui/Tools.h"
#include "ui/ViewportWidget.h"

#include <QApplication>
#include <QMouseEvent>

#include <glm/gtc/matrix_transform.hpp>

using namespace lila;
using namespace lila::scene;
using namespace puptest;
using json = nlohmann::json;
using Approx = doctest::Approx;

namespace
{
glm::dvec3 vec(const std::string& n, int attr)
{
    graph().evaluate();
    return std::get<glm::dvec3>(graph().value({id(n), attr, -1}));
}
glm::dquat rot(const std::string& n) { return glm::quat_cast(puppeteer::math::rotationOf(world(n))); }
/** Degrees between two orientations. */
double turned(const glm::dquat& a, const glm::dquat& b) { return glm::degrees(2.0 * std::acos(std::min(1.0, std::abs(glm::dot(a, b))))); }

/** What ViewportWidget does for a drag: setView before each call, press, moves, release. */
void drag(ui::Gizmo& gz, QPointF from, QPointF to, Qt::MouseButton button = Qt::LeftButton, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    gz.setView(view());
    gz.hover(from);
    REQUIRE(gz.press(from, button, mods));
    for (int i = 1; i <= 4; ++i)
    {
        gz.setView(view());
        gz.drag(from + (to - from) * (i / 4.0), mods);
    }
    gz.release();
    CHECK_FALSE(gz.dragging());
}

void click(ui::Gizmo& gz, QPointF at, Qt::MouseButton button)
{
    gz.setView(view());
    REQUIRE(gz.press(at, button, Qt::NoModifier));
    gz.release();
}

std::unique_ptr<ui::Gizmo> spinner(const char* tool)
{
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    REQUIRE(gz);
    gz->setView(view());
    REQUIRE(gz->supports(tool));
    return gz;
}

QPointF polar(double r, double screenDegrees)
{
    const double a = glm::radians(screenDegrees);
    return kCenter + QPointF(r * std::cos(a), r * std::sin(a));
}
const QPointF kXZone = polar(u(110.0), -90.0);
const QPointF kYZone = polar(u(110.0), 170.0);
const QPointF kZZone = polar(u(110.0), 30.0);
const QPointF kRotateButton = polar(u(177.0), -90.0);
const QPointF kScaleButton = polar(u(177.0), -60.0);
const QPointF kRim = polar(u(190.0), -150.0);
const QPointF kSlider = polar(u(177.0), 90.0);
const QPointF kUp(0.0, -1.0);

void chain3()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("joint", {{"position", v3(0, 20, 0)}, {"name", "arm"}});
    run("joint", {{"position", v3(0, 40, 0)}, {"name", "hand"}});
    run("select", {{"args", {"arm"}}});
}

/** A left leg with an IK chain, its handle selected. */
std::string legWithIk()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"position", v3(10, 100, 0)}, {"name", "hip_l"}});
    run("joint", {{"position", v3(10, 55, 6)}, {"name", "knee_l"}});
    run("joint", {{"position", v3(10, 10, 0)}, {"name", "ankle_l"}});
    run("joint", {{"position", v3(10, 3, 12)}, {"name", "ball_l"}});
    run("joint", {{"position", v3(10, 0, 20)}, {"name", "toe_l"}});
    const std::string chain = run("puppeteerIkChain", {{"args", {"hip_l", "ankle_l"}}}).get<std::string>();
    return chain;
}
} // namespace

TEST_CASE("spinner: zones drag vertically, up increases, sideways does nothing")
{
    loadPuppeteer();
    chain3();
    auto gz = spinner("rotate");
    const glm::dquat r0 = rot("arm");
    drag(*gz, kXZone, kXZone + QPointF(80.0, 0.0));
    CHECK(turned(rot("arm"), r0) == Approx(0.0).epsilon(1e-6));
    // 60 px up at the default 0.5 degrees per pixel.
    drag(*gz, kXZone, kXZone + kUp * 60.0);
    CHECK(turned(rot("arm"), r0) == Approx(30.0).epsilon(1e-6));
    const glm::dvec3 axisX = puppeteer::math::rotationOf(world("arm"))[0];
    CHECK(glm::length(axisX - glm::dvec3(puppeteer::math::rotationOf(glm::mat4_cast(r0))[0])) < 1e-6); // turned about its own X
    // Down decreases, back to where it was; Shift and Ctrl change nothing.
    drag(*gz, kXZone, kXZone - kUp * 60.0, Qt::LeftButton, Qt::ShiftModifier | Qt::ControlModifier);
    CHECK(turned(rot("arm"), r0) == Approx(0.0).epsilon(1e-6));
    // One undo step per drag.
    run("undo");
    CHECK(turned(rot("arm"), r0) == Approx(30.0).epsilon(1e-6));
    // A right drag on a plain joint's zone does nothing, and clicks off the Spinner fall through.
    drag(*gz, kYZone, kYZone + kUp * 50.0, Qt::RightButton);
    CHECK(turned(rot("arm"), r0) == Approx(30.0).epsilon(1e-6));
    CHECK_FALSE(gz->press(QPointF(100, 100), Qt::LeftButton, Qt::NoModifier));
}

TEST_CASE("spinner: inherit rotate is the rotate button's right-click alternative")
{
    loadPuppeteer();
    chain3();
    auto gz = spinner("rotate");
    const glm::dquat hand0 = rot("hand"), arm0 = rot("arm");
    click(*gz, kRotateButton, Qt::RightButton);
    drag(*gz, kZZone, kZZone + kUp * 66.0);
    CHECK(turned(rot("arm"), arm0) == Approx(33.0).epsilon(1e-6));
    CHECK(turned(rot("hand"), hand0) == Approx(0.0).epsilon(1e-6));
    click(*gz, kRotateButton, Qt::RightButton);
    const glm::dquat hand1 = rot("hand");
    drag(*gz, kZZone, kZZone + kUp * 20.0);
    CHECK(turned(rot("hand"), hand1) == Approx(10.0).epsilon(1e-6));
}

TEST_CASE("spinner: scale is linear, toon scale compensates the other axes, the centre scales uniformly")
{
    loadPuppeteer();
    chain3();
    auto gz = spinner("scale");
    drag(*gz, kXZone, kXZone + kUp * 80.0);
    glm::dvec3 s = vec("arm", Transform::kScale);
    CHECK(s.x == Approx(1.0 + 80.0 * 0.0046));
    CHECK(s.y == Approx(1.0));
    run("undo");
    click(*gz, kScaleButton, Qt::RightButton); // toon
    drag(*gz, kXZone, kXZone + kUp * 80.0);
    s = vec("arm", Transform::kScale);
    CHECK(s.x == Approx(1.368));
    CHECK(s.y == Approx(std::pow(1.368, -1.0 / 3.0)));
    CHECK(s.z == Approx(std::pow(1.368, -1.0 / 3.0)));
    // The child keeps its size.
    CHECK(glm::length(glm::dvec3(world("hand")[0])) == Approx(1.0).epsilon(1e-6));
    click(*gz, kScaleButton, Qt::RightButton);
    run("undo");
    drag(*gz, kCenter, kCenter + kUp * 50.0);
    CHECK(vec("arm", Transform::kScale) == glm::dvec3(1.0 + 50.0 * 0.0046));
}

TEST_CASE("spinner: middle drag turns freely like a trackball, the same way every time")
{
    loadPuppeteer();
    chain3();
    auto gz = spinner("rotate");
    const glm::dquat r0 = rot("arm");
    drag(*gz, kYZone, kYZone + QPointF(40.0, 0.0), Qt::MiddleButton);
    const glm::dquat r1 = rot("arm");
    CHECK(turned(r1, r0) == Approx(20.0).epsilon(1e-6));
    // Dragging right turns about the view's up axis (world Y here).
    const glm::dquat expect = glm::angleAxis(glm::radians(20.0), glm::dvec3(0, 1, 0)) * r0;
    CHECK(turned(r1, expect) < 1e-3);
    run("undo");
    drag(*gz, kCenter, kCenter + QPointF(40.0, 0.0), Qt::MiddleButton);
    CHECK(turned(rot("arm"), r1) < 1e-3);
}

TEST_CASE("spinner: on an IK controller the centre swings the pole, right-drag turns the hand with it")
{
    loadPuppeteer();
    const std::string chain = legWithIk();
    const std::string handle = run("getAttr", {{"args", {chain + ".handle"}}}).get<std::string>();
    run("select", {{"args", {handle}}});
    auto gz = spinner("rotate");
    const glm::dquat ankle0 = rot("ankle_l");
    const glm::dvec3 knee0 = pos("knee_l");
    drag(*gz, kCenter, kCenter + kUp * 80.0);
    CHECK(attr(handle + ".poleAngle") == Approx(80.0 * 0.42));
    CHECK(glm::length(pos("knee_l") - knee0) > 1.0);
    CHECK(turned(rot("ankle_l"), ankle0) == Approx(0.0).epsilon(1e-6));
    const glm::dquat ankle1 = rot("ankle_l");
    drag(*gz, kCenter, kCenter + kUp * 40.0, Qt::RightButton);
    CHECK(attr(handle + ".poleAngle") == Approx(120.0 * 0.42));
    CHECK(turned(rot("ankle_l"), ankle1) > 5.0);
    // The rings turn the controller, and the hand (here the ankle) with it.
    const glm::dquat ankle2 = rot("ankle_l");
    drag(*gz, kXZone, kXZone + kUp * 40.0);
    CHECK(turned(rot("ankle_l"), ankle2) == Approx(20.0).epsilon(1e-3));
}

TEST_CASE("spinner: right-dragging a reverse foot's rings rolls, twists and banks it within its limits")
{
    loadPuppeteer();
    legWithIk();
    const std::string foot = run("puppeteerReverseFoot", {{"args", {"ankle_l"}}}).get<std::string>();
    run("select", {{"args", {foot}}});
    auto gz = spinner("rotate");
    drag(*gz, kXZone, kXZone + kUp * 40.0, Qt::RightButton);
    CHECK(attr(foot + ".roll") == Approx(20.0));
    drag(*gz, kYZone, kYZone - kUp * 20.0, Qt::RightButton);
    CHECK(attr(foot + ".twist") == Approx(-10.0));
    drag(*gz, kZZone, kZZone + kUp * 30.0, Qt::RightButton);
    CHECK(attr(foot + ".bank") == Approx(15.0));
    // Past the limit it stops there.
    drag(*gz, kXZone, kXZone + kUp * 400.0, Qt::RightButton);
    CHECK(attr(foot + ".roll") == Approx(attr(foot + ".rollForwardMax")));
    // The centre swings the pole with the foot left alone.
    const glm::dquat f0 = rot(foot);
    drag(*gz, kCenter, kCenter + kUp * 20.0);
    CHECK(attr(foot + ".poleAngle") == Approx(20.0 * 0.42));
    CHECK(turned(rot(foot), f0) == Approx(0.0).epsilon(1e-9));
}

TEST_CASE("spinner: right-clicking a controller locks its key, and a locked key holds until the next")
{
    loadPuppeteer();
    const std::string chain = legWithIk();
    const std::string foot = run("puppeteerReverseFoot", {{"args", {"ankle_l"}}}).get<std::string>();
    run("select", {{"args", json::array()}});
    run("currentTime", {{"args", {0.0}}});
    run("setKeyframe", {{"args", {foot + ".translate"}}, {"time", 0.0}});
    run("setKeyframe", {{"args", {foot + ".tz"}}, {"time", 1.0}, {"value", 30.0}});
    const double z0 = vec(foot, Transform::kTranslate).z;

    // A point on the drawn sole loop, as the Spinner draws it: an ellipse around the pivots.
    auto pv = [&](const char* n) { return vec(foot, graph().node(id(foot))->type().attrIndex(n)); };
    const glm::dvec3 heel = pv("heelPivot"), toe = pv("toePivot"), inner = pv("innerPivot"), outer = pv("outerPivot");
    const glm::dvec3 local((inner.x + outer.x) * 0.5 + std::abs(outer.x - inner.x) * 0.5 * 1.15, 0.0, (heel.z + toe.z) * 0.5);
    const auto px = view().project(glm::dvec3(world(foot) * glm::dvec4(local, 1.0)));
    REQUIRE(px);
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    gz->setView(view());
    CHECK(gz->wantsRightButton(*px));
    CHECK_FALSE(gz->wantsRightButton(QPointF(5, 5)));
    click(*gz, *px, Qt::RightButton);
    CHECK(attr(chain + ".lock") == Approx(1.0));

    // Locked at 0: the foot holds there until its next key.
    run("currentTime", {{"args", {0.5}}});
    CHECK(vec(foot, Transform::kTranslate).z == Approx(z0));
    run("currentTime", {{"args", {1.0}}});
    CHECK(vec(foot, Transform::kTranslate).z == Approx(30.0));
    run("currentTime", {{"args", {0.0}}});
    click(*gz, *px, Qt::RightButton);
    CHECK(attr(chain + ".lock") == Approx(0.0));
    run("currentTime", {{"args", {0.5}}});
    CHECK(vec(foot, Transform::kTranslate).z > z0 + 1.0);
}

TEST_CASE("spinner: off a key in Animate mode it is disabled")
{
    loadPuppeteer();
    chain3();
    run("setKeyframe", {{"args", {"arm.rotate"}}, {"time", 0.0}});
    REQUIRE(ui::modes::activate("puppeteerAnimate"));
    auto gz = spinner("rotate");
    run("currentTime", {{"args", {0.5}}});
    const glm::dquat r0 = rot("arm");
    drag(*gz, kXZone, kXZone + kUp * 60.0);
    CHECK(turned(rot("arm"), r0) == Approx(0.0).epsilon(1e-9));
    run("currentTime", {{"args", {0.0}}});
    run("autoKeyframe", {{"state", true}});
    drag(*gz, kXZone, kXZone + kUp * 60.0);
    CHECK(turned(rot("arm"), r0) > 1.0);
    run("autoKeyframe", {{"state", false}});
    REQUIRE(ui::modes::activate(ui::modes::kDefaultMode));
}

TEST_CASE("spinner: the breakdown slider is proportional past a dead zone and its result sticks")
{
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("setKeyframe", {{"args", {"root.tx"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"root.tx"}}, {"time", 1.0}, {"value", 4.0}});
    run("setKeyframe", {{"args", {"root.tx"}}, {"time", 2.0}, {"value", 10.0}});
    run("currentTime", {{"args", {1.0}}});
    run("select", {{"args", {"root"}}});
    auto gz = spinner("rotate");
    drag(*gz, kSlider, kSlider + QPointF(15.0, 0.0));
    CHECK(attr("root.tx") == Approx(4.0));
    drag(*gz, kSlider, kSlider + QPointF(60.0, 0.0));
    CHECK(attr("root.tx") == Approx(7.0));
    // The next drag starts from the stuck result; far right overshoots the next key.
    drag(*gz, kSlider, kSlider + QPointF(-100.0, 0.0));
    CHECK(attr("root.tx") == Approx(0.0));
}

TEST_CASE("spinner: the rim moves it, per view, and it stays in the view")
{
    loadPuppeteer();
    chain3();
    auto gz = spinner("rotate");
    drag(*gz, kRim, kRim + QPointF(-300.0, 50.0));
    auto again = spinner("rotate");
    const glm::dquat r0 = rot("arm");
    drag(*again, kXZone + QPointF(-300.0, 50.0), kXZone + QPointF(-300.0, -10.0));
    CHECK(turned(rot("arm"), r0) == Approx(30.0).epsilon(1e-6));
    // Back where it was, for the tests that follow.
    drag(*again, kRim + QPointF(-300.0, 50.0), kRim);
}

TEST_CASE("spinner: sticky aim keeps the orients along the bones while rigging")
{
    loadPuppeteer();
    chain3();
    run("puppeteerMove", {{"args", {"arm"}}, {"by", v3(10, 0, 0)}, {"stickyAim", true}});
    const glm::dvec3 bone = glm::normalize(pos("arm") - pos("root"));
    // root's aim axis (the one along the bone before) now points at the moved arm.
    const glm::dmat3 r = puppeteer::math::rotationOf(world("root"));
    double best = 0.0;
    for (int k = 0; k < 3; ++k)
        best = std::max(best, std::abs(glm::dot(r[k], bone)));
    CHECK(best == Approx(1.0).epsilon(1e-9));
    // The hand came along, and arm still aims at it.
    CHECK(glm::length(pos("hand") - glm::dvec3(10, 40, 0)) < 1e-9);
    const glm::dvec3 toHand = glm::normalize(pos("hand") - pos("arm"));
    const glm::dmat3 a = puppeteer::math::rotationOf(world("arm"));
    best = 0.0;
    for (int k = 0; k < 3; ++k)
        best = std::max(best, std::abs(glm::dot(a[k], toHand)));
    CHECK(best == Approx(1.0).epsilon(1e-9));
}

TEST_CASE("spinner: pivot moves a joint without its children; sticky aim re-aims the parent at it")
{
    loadPuppeteer();
    chain3();
    run("puppeteerMove", {{"args", {"arm"}}, {"by", v3(10, 0, 0)}, {"keepChildren", true}, {"stickyAim", true}});
    CHECK(glm::length(pos("hand") - glm::dvec3(0, 40, 0)) < 1e-9);
    CHECK(glm::length(pos("arm") - glm::dvec3(10, 20, 0)) < 1e-9);
    const glm::dvec3 toHand = glm::normalize(pos("hand") - pos("arm"));
    const glm::dmat3 a = puppeteer::math::rotationOf(world("arm"));
    double best = 0.0;
    for (int k = 0; k < 3; ++k)
        best = std::max(best, std::abs(glm::dot(a[k], toHand)));
    CHECK(best == Approx(1.0).epsilon(1e-9));
}

TEST_CASE("spinner: right-clicks over it reach it through the viewport, not the marking menu")
{
    if (!qApp)
    {
        MESSAGE("no offscreen Qt platform: skipped");
        return;
    }
    loadPuppeteer();
    chain3();
    REQUIRE(ui::gizmos::setActive("puppeteer.spinner"));
    ui::tools::setCurrent(ui::Tool::Rotate);
    {
        ui::ViewportWidget w;
        w.resize(800, 600);
        w.setView("front");
        auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent e(type, at, w.mapToGlobal(at), button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(&w, &e);
        };
        auto dragUp = [&](QPointF from, double px) {
            send(QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
            send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= 4; ++i)
                send(QEvent::MouseMove, from + kUp * (px * i / 4.0), Qt::NoButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, from + kUp * px, Qt::LeftButton, Qt::NoButton);
        };
        const glm::dquat arm0 = rot("arm"), hand0 = rot("hand");
        // Right-click on the rotate button: inherit rotate on.
        send(QEvent::MouseButtonPress, kRotateButton, Qt::RightButton, Qt::RightButton);
        send(QEvent::MouseButtonRelease, kRotateButton, Qt::RightButton, Qt::NoButton);
        dragUp(kZZone, 60.0);
        CHECK(turned(rot("arm"), arm0) == Approx(30.0).epsilon(1e-6));
        CHECK(turned(rot("hand"), hand0) == Approx(0.0).epsilon(1e-6));
        CHECK(graph().selection().size() == 1);
        send(QEvent::MouseButtonPress, kRotateButton, Qt::RightButton, Qt::RightButton);
        send(QEvent::MouseButtonRelease, kRotateButton, Qt::RightButton, Qt::NoButton);
    }
    ui::gizmos::setActive("");
    ui::tools::setCurrent(ui::Tool::Select);
}
