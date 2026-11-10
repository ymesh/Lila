// What the Spinner draws, rasterised from its overlay triangles, and the tool it starts in.

#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "extensions/ExtensionManager.h"
#include "render/FrameState.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Gizmo.h"
#include "ui/Modes.h"
#include "ui/Overlay.h"
#include "ui/Tools.h"

#include <QApplication>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <map>

using namespace lila;
using namespace lila::scene;
using namespace puptest;
using json = nlohmann::json;
using Approx = doctest::Approx;

namespace
{

double cross(glm::vec2 a, glm::vec2 b, glm::vec2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }

/** The colour the last solid triangle covering p was filled with; nullopt for none. */
std::optional<glm::vec4> colorAt(const std::vector<render::OverlayVertex>& tris, glm::vec2 p)
{
    std::optional<glm::vec4> out;
    for (size_t i = 0; i + 2 < tris.size(); i += 3)
    {
        const auto& a = tris[i];
        const auto& b = tris[i + 1];
        const auto& c = tris[i + 2];
        if (a.uv.x >= 0.0f || a.color.a < 0.5f)
            continue;
        const double d1 = cross(a.pos, b.pos, p), d2 = cross(b.pos, c.pos, p), d3 = cross(c.pos, a.pos, p);
        const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
        if (!(neg && pos))
            out = a.color;
    }
    return out;
}

/** 0 red, 1 green, 2 blue, -1 grey or nothing. */
int hueOf(const std::optional<glm::vec4>& c)
{
    if (!c)
        return -1;
    const glm::vec3 v(*c);
    const float hi = std::max({v.r, v.g, v.b}), lo = std::min({v.r, v.g, v.b});
    if (hi - lo < 0.1f)
        return -1;
    return v.r == hi ? 0 : v.g == hi ? 1 : 2;
}

std::vector<render::OverlayVertex> drawSpinner(ui::Gizmo& gz)
{
    std::vector<render::OverlayVertex> out;
    ui::OverlayPainter p(out, 1.0);
    gz.setView(view("drawTest"));
    gz.draw(p);
    return out;
}

/** Samples the zone ring at radius r every 2 degrees: the zone each angle shows, and each zone's mean direction. */
void checkZones(const std::vector<render::OverlayVertex>& tris, double r)
{
    std::map<int, glm::dvec2> sum;
    for (int deg = -180; deg < 180; deg += 2)
    {
        const double a = glm::radians(double(deg));
        const glm::vec2 p(float(kCenter.x() + r * std::cos(a)), float(kCenter.y() + r * std::sin(a)));
        const int hue = hueOf(colorAt(tris, p));
        // Screen degrees, y down: X red on top (-150..-30), Z blue lower right (-30..90), Y green lower left (90..210).
        const double s = deg < -150 ? deg + 360.0 : double(deg);
        const int want = s > -150 && s < -30 ? 0 : s > -30 && s < 90 ? 2 : 1;
        const bool nearEdge = std::abs(s + 150) < 5 || std::abs(s + 30) < 5 || std::abs(s - 90) < 5 || std::abs(s - 210) < 5;
        if (nearEdge)
            continue;
        CAPTURE(deg);
        CHECK(hue == want);
        if (hue >= 0)
            sum[hue] += glm::dvec2(std::cos(a), std::sin(a));
    }
    REQUIRE(sum.size() == 3);
    auto centroid = [&](int hue) { return glm::degrees(std::atan2(sum[hue].y, sum[hue].x)); };
    CHECK(centroid(0) == Approx(-90.0).epsilon(0.02));
    CHECK(centroid(2) == Approx(30.0).epsilon(0.02));
    CHECK(std::abs(centroid(1) - 150.0) < 2.0);
}
} // namespace

TEST_CASE("spinner draw: the three zones tile the disc, red on top, blue lower right, green lower left")
{
    if (!qApp)
    {
        MESSAGE("no offscreen Qt platform: skipped");
        return;
    }
    loadPuppeteer();
    run("newScene");
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "j"}});
    run("select", {{"args", {"j"}}});
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    REQUIRE(gz);
    gz->setView(view("drawTest"));
    // Rotate on a plain joint: full wedges.
    REQUIRE(gz->supports("rotate"));
    checkZones(drawSpinner(*gz), u(60.0));
    checkZones(drawSpinner(*gz), u(120.0));
    // Scale: rings around a centre disc.
    REQUIRE(gz->supports("scale"));
    checkZones(drawSpinner(*gz), u(110.0));
    gz->supports("rotate");
}

TEST_CASE("spinner draw: hit testing agrees with the drawing")
{
    if (!qApp)
    {
        MESSAGE("no offscreen Qt platform: skipped");
        return;
    }
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "j"}});
    run("select", {{"args", {"j"}}});
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    gz->setView(view("drawTest"));
    REQUIRE(gz->supports("rotate"));
    const std::vector<render::OverlayVertex> tris = drawSpinner(*gz);
    // Pressing where a colour is drawn turns about that colour's axis (local axes: the joint's own, identity here).
    for (int deg : {-90, 150, 30, -120, 60, 200})
    {
        CAPTURE(deg);
        const double a = glm::radians(double(deg));
        const QPointF at = kCenter + QPointF(u(110.0) * std::cos(a), u(110.0) * std::sin(a));
        const int hue = hueOf(colorAt(tris, glm::vec2(float(at.x()), float(at.y()))));
        REQUIRE(hue >= 0);
        const glm::dvec3 r0 = std::get<glm::dvec3>(Graph::current().value(Graph::current().plug("j.rotate")));
        gz->setView(view("drawTest"));
        REQUIRE(gz->press(at, Qt::LeftButton, Qt::NoModifier));
        gz->drag(at + QPointF(0.0, -20.0), Qt::NoModifier);
        gz->release();
        Graph::current().evaluate();
        const glm::dvec3 r1 = std::get<glm::dvec3>(Graph::current().value(Graph::current().plug("j.rotate")));
        const glm::dvec3 d = r1 - r0;
        int axis = 0;
        for (int k = 1; k < 3; ++k)
            if (std::abs(d[k]) > std::abs(d[axis]))
                axis = k;
        CHECK(axis == hue);
        CHECK(std::abs(d[axis]) == Approx(10.0).epsilon(1e-6));
        run("undo");
    }
}

TEST_CASE("spinner: entering Animate starts in Rotate, and moves track the cursor on screen")
{
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "j"}});
    run("select", {{"args", {"j"}}});
    REQUIRE(ui::actions::find("tool.move"));
    ui::tools::setCurrent(ui::Tool::Move);
    REQUIRE(ui::modes::activate("puppeteerAnimate"));
    CHECK(ui::tools::current() == ui::Tool::Rotate);
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    gz->setView(view("drawTest"));
    REQUIRE(gz->supports(ui::tools::name(ui::tools::current())));
    const QPointF xZone = kCenter + QPointF(0.0, -u(110.0));
    REQUIRE(gz->press(xZone, Qt::LeftButton, Qt::NoModifier));
    gz->drag(xZone + QPointF(0.0, -80.0), Qt::NoModifier);
    gz->release();
    Graph& g = Graph::current();
    g.evaluate();
    CHECK(std::get<glm::dvec3>(g.value(g.plug("j.rotate"))).x == Approx(40.0));
    CHECK(std::get<glm::dvec3>(g.value(g.plug("j.translate"))) == glm::dvec3(0.0));
    REQUIRE(ui::modes::activate(ui::modes::kDefaultMode));

    // Move: an 80 px drag moves the joint 80 px on screen along the axis's projection.
    REQUIRE(gz->supports("move"));
    const auto before = view("drawTest").project(glm::dvec3(0.0));
    REQUIRE(gz->press(xZone, Qt::LeftButton, Qt::NoModifier));
    gz->drag(xZone + QPointF(0.0, -80.0), Qt::NoModifier);
    gz->release();
    g.evaluate();
    const glm::dvec3 t = std::get<glm::dvec3>(g.value(g.plug("j.translate")));
    const auto after = view("drawTest").project(t);
    REQUIRE(before);
    REQUIRE(after);
    CHECK(std::hypot(after->x() - before->x(), after->y() - before->y()) == Approx(80.0).epsilon(0.01));
    gz->supports("rotate");
    ui::tools::setCurrent(ui::Tool::Select);
}

TEST_CASE("spinner: holding Summon brings it to the cursor, and it stays there")
{
    if (!qApp)
    {
        MESSAGE("no offscreen Qt platform: skipped");
        return;
    }
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "j"}});
    run("select", {{"args", {"j"}}});
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    gz->setView(view("summonTest"));
    REQUIRE(gz->supports("rotate"));
    auto summonTo = [&](QPointF at) {
        REQUIRE(ui::actions::run("puppeteer.spinner.summonDown"));
        gz->setView(view("summonTest"));
        gz->hover(at);
        std::vector<render::OverlayVertex> tris;
        ui::OverlayPainter p(tris, 1.0);
        gz->draw(p);
        REQUIRE(ui::actions::run("puppeteer.spinner.summonUp"));
    };
    const QPointF to(300.0, 260.0);
    summonTo(to);
    gz->setView(view("summonTest"));
    // The X zone is now above the new centre.
    const QPointF xZone = to + QPointF(0.0, -u(110.0));
    REQUIRE(gz->press(xZone, Qt::LeftButton, Qt::NoModifier));
    gz->drag(xZone + QPointF(0.0, -20.0), Qt::NoModifier);
    gz->release();
    Graph& g = Graph::current();
    g.evaluate();
    CHECK(std::get<glm::dvec3>(g.value(g.plug("j.rotate"))).x == Approx(10.0));
    summonTo(kCenter);
}

// Writes the Spinner's triangles and the overlay atlas for each state to LILA_SPINNER_DUMP, for looking at offline.
TEST_CASE("spinner draw: dump states for inspection" * doctest::skip(std::getenv("LILA_SPINNER_DUMP") == nullptr))
{
    REQUIRE(qApp);
    const std::string dir = std::getenv("LILA_SPINNER_DUMP");
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "j"}});
    run("select", {{"args", {"j"}}});
    std::unique_ptr<ui::Gizmo> gz = ui::gizmos::create("puppeteer.spinner");
    auto dump = [&](const std::string& name) {
        std::vector<render::OverlayVertex> tris;
        ui::OverlayPainter p(tris, 1.0);
        gz->setView(view("drawTest"));
        gz->draw(p);
        const auto atlas = p.atlas();
        std::FILE* f = std::fopen((dir + "/" + name + ".bin").c_str(), "wb");
        REQUIRE(f);
        const int32_t head[3] = {atlas ? atlas->width : 0, atlas ? atlas->height : 0, int32_t(tris.size())};
        std::fwrite(head, sizeof(head), 1, f);
        if (atlas)
            std::fwrite(atlas->pixels.data(), 1, atlas->pixels.size(), f);
        std::fwrite(tris.data(), sizeof(render::OverlayVertex), tris.size(), f);
        std::fclose(f);
    };
    REQUIRE(gz->supports("rotate"));
    dump("rotate");
    gz->hover(kCenter + QPointF(0.0, -u(110.0)));
    dump("rotate_hoverX");
    gz->hover(kCenter + QPointF(u(177.0), 0.0));
    dump("rotate_hoverSpace");
    gz->hover(QPointF(5.0, 5.0));
    REQUIRE(gz->supports("move"));
    dump("move");
    REQUIRE(gz->supports("scale"));
    dump("scale");
    gz->supports("rotate");
}
