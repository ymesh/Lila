#include <doctest/doctest.h>

#include "TestUtil.h"

#include "curve/CurveNodes.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/GeometrySnap.h"
#include "ui/Tools.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;
using namespace testutil;
namespace geometrySnap = lila::ui::geometrySnap;

namespace
{
void registerCurves()
{
    static bool done = false;
    if (done)
        return;
    curve::registerTypes();
    curve::registerCommands();
    done = true;
}

/** Looking down -Z, 1 px per cm, world origin at pixel (50, 50). */
ui::ViewInfo snapTopView()
{
    ui::ViewInfo v;
    v.proj = glm::ortho(-50.0, 50.0, -50.0, 50.0, -100.0, 100.0);
    v.width = v.height = 100.0;
    v.ortho = true;
    return v;
}

std::optional<D3> snap(const std::string& curve, QPointF px, unsigned snaps, const std::vector<uint32_t>& moving = {})
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    ui::ViewInfo view = snapTopView();
    geometrySnap::Nearest n(px, view, 5.0);
    geometrySnap::addCurve(n, g, g.find(curve)->id(), snaps, moving);
    return n.hit();
}

void checkHit(const std::optional<D3>& got, const D3& want)
{
    REQUIRE(got);
    CHECK(glm::distance(*got, want) == doctest::Approx(0.0).epsilon(1e-6));
}
} // namespace

TEST_CASE("V and C snap to curve points and along curves")
{
    registerCurves();
    run("newScene");
    std::string c = run("curve", {{"type", "poly"}, {"points", {{0, 0, 0}, {20, 0, 0}, {20, 20, 0}}}});

    SUBCASE("V finds the nearest curve point, C a point along the curve")
    {
        checkHit(snap(c, QPointF(72, 51), ui::SnapVertex), D3(20, 0, 0));
        CHECK_FALSE(snap(c, QPointF(60, 52), ui::SnapVertex));
        checkHit(snap(c, QPointF(60, 52), ui::SnapEdge), D3(10, 0, 0));
        checkHit(snap(c, QPointF(71, 62), ui::SnapEdge), D3(20, 12, 0));
    }
    SUBCASE("targets follow the curve's transform")
    {
        run("move", {{"args", {c}}, {"by", {5, 0, 0}}});
        checkHit(snap(c, QPointF(76, 50), ui::SnapVertex), D3(25, 0, 0));
        checkHit(snap(c, QPointF(65, 53), ui::SnapEdge), D3(15, 0, 0));
    }
    SUBCASE("points being moved are skipped, and so is the shape they reshape")
    {
        std::vector<uint32_t> moving{curve::componentId(0, 1, curve::PointRole::Point)};
        CHECK_FALSE(snap(c, QPointF(71, 50), ui::SnapVertex, moving));
        checkHit(snap(c, QPointF(51, 50), ui::SnapVertex, moving), D3(0, 0, 0));
        CHECK_FALSE(snap(c, QPointF(60, 52), ui::SnapEdge, moving));
    }
    SUBCASE("hidden curves are not targets")
    {
        run("hide", {{"args", {c}}});
        CHECK_FALSE(snap(c, QPointF(72, 51), ui::SnapVertex));
        CHECK_FALSE(snap(c, QPointF(60, 52), ui::SnapEdge));
    }
}

TEST_CASE("V snaps to the centers of locators and joints")
{
    run("newScene");
    std::string loc = run("spaceLocator", {{"position", {10, 5, 0}}, {"skipSelect", true}});
    std::string jnt = run("joint", {{"position", {-20, 0, 0}}, {"parent", ""}, {"skipSelect", true}});
    std::string cube = run("polyCube", json::object())[0];
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    ui::ViewInfo view = snapTopView();
    auto at = [&](const std::string& node, QPointF px, unsigned snaps) {
        geometrySnap::Nearest n(px, view, 5.0);
        bool point = geometrySnap::addPointObject(n, g, g.find(node)->id(), snaps);
        return std::pair{point, n.hit()};
    };

    auto [isLoc, locHit] = at(loc, QPointF(62, 56), ui::SnapVertex);
    CHECK(isLoc);
    checkHit(locHit, D3(10, 5, 0));
    auto [isJoint, jointHit] = at(jnt, QPointF(31, 50), ui::SnapVertex);
    CHECK(isJoint);
    checkHit(jointHit, D3(-20, 0, 0));
    CHECK_FALSE(at(loc, QPointF(62, 56), ui::SnapEdge).second);
    CHECK_FALSE(at(cube, QPointF(50, 50), ui::SnapVertex).first);

    run("hide", {{"args", {loc}}});
    g.evaluate();
    CHECK_FALSE(at(loc, QPointF(62, 56), ui::SnapVertex).second);
}
