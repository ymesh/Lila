#include <doctest/doctest.h>

#include "TestUtil.h"

#include "curve/Cable.h"
#include "curve/Contour.h"
#include "curve/CurveNodes.h"
#include "curve/SurfaceConform.h"
#include "curve/Tessellate.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <cmath>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;
using namespace curvetest;

namespace
{
curve::Splines splinesOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    return curve::splinesOf(n->value(curve::CurveShape::kWorldSpace));
}

double polyLength(const std::vector<D3>& pts)
{
    double l = 0.0;
    for (size_t i = 1; i < pts.size(); ++i)
        l += glm::distance(pts[i - 1], pts[i]);
    return l;
}
} // namespace

TEST_CASE("catenary: exact length, pinned ends, symmetric sag and straight fallbacks")
{
    const D3 a(0, 100, 0), b(200, 100, 0), down(0, -1, 0);
    std::vector<D3> c = curve::catenary(a, b, down, 220.0, 400);
    REQUIRE(c.size() == 401);
    CHECK(glm::distance(c.front(), a) < 1e-9);
    CHECK(glm::distance(c.back(), b) < 1e-9);
    CHECK(polyLength(c) == doctest::Approx(220.0).epsilon(1e-4));
    // Level ends: lowest in the middle, mirror symmetric, never above the chord.
    CHECK(c[200].x == doctest::Approx(100.0).epsilon(1e-6));
    CHECK(c[100].y == doctest::Approx(c[300].y).epsilon(1e-9));
    for (const D3& p : c)
        CHECK(p.y <= 100.0 + 1e-9);
    // sinh(z) / z = 1.1 gives z = 0.7634, so k = 131.0 cm and the sag k (cosh z - 1) = 40.06 cm.
    double sag = curve::sagOf(c, down);
    CHECK(sag == doctest::Approx(100.0 - c[200].y));
    CHECK(sag == doctest::Approx(40.06).epsilon(0.002));

    // Uneven ends still hang between them with the asked length.
    std::vector<D3> slope = curve::catenary(D3(0), D3(100, 60, 30), down, 160.0, 400);
    CHECK(polyLength(slope) == doctest::Approx(160.0).epsilon(1e-4));
    CHECK(glm::distance(slope.back(), D3(100, 60, 30)) < 1e-9);

    // Z-up gravity sags along -Z.
    std::vector<D3> zup = curve::catenary(D3(0), D3(100, 0, 0), D3(0, 0, -9.8), 120.0, 64);
    CHECK(zup[32].z < -10.0);
    CHECK(std::abs(zup[32].y) < 1e-9);

    // No slack, or ends straight above each other: a straight line, no NaN.
    for (const auto& line : {curve::catenary(a, b, down, 200.0, 8), curve::catenary(D3(0), D3(0, 50, 0), down, 80.0, 8)})
        for (const D3& p : line)
        {
            CHECK(std::isfinite(p.x));
            CHECK(std::isfinite(p.y));
        }
    CHECK(curve::catenary(a, b, down, 200.0, 8)[4].y == doctest::Approx(100.0));
}

TEST_CASE("sag depth mode finds the length that dips exactly that far")
{
    const D3 a(0), b(300, 40, 0), down(0, -1, 0);
    double len = curve::lengthForSag(a, b, down, 35.0);
    CHECK(len > glm::distance(a, b));
    CHECK(curve::sagOf(curve::catenary(a, b, down, len, 256), down) == doctest::Approx(35.0).epsilon(1e-3));
    CHECK(curve::lengthForSag(a, b, down, 0.0) == doctest::Approx(glm::distance(a, b)));
}

TEST_CASE("makeCables: spans, bundles with seeded variation, warnings for degenerate anchors")
{
    curve::CableOptions o;
    o.slack = 0.05;
    curve::CableResult r = curve::makeCables({D3(0), D3(100, 0, 0), D3(100, 0, 100)}, false, o);
    CHECK(r.cables.size() == 2);
    CHECK(r.warnings.empty());
    for (const curve::Spline& s : r.cables)
    {
        CHECK(s.kind == curve::Kind::Bezier);
        CHECK_FALSE(s.closed);
        CHECK(curve::length(s) == doctest::Approx(105.0).epsilon(0.01));
    }
    CHECK(glm::distance(r.cables[0].points.front(), D3(0)) < 1e-6);
    CHECK(glm::distance(r.cables[1].points.back(), D3(100, 0, 100)) < 1e-6);
    CHECK(curve::makeCables({D3(0), D3(100, 0, 0), D3(100, 0, 100)}, true, o).cables.size() == 3);

    o.count = 5;
    o.spread = 3.0;
    o.seed = 7;
    curve::CableResult bundle = curve::makeCables({D3(0), D3(200, 0, 0)}, false, o);
    REQUIRE(bundle.cables.size() == 5);
    for (const curve::Spline& s : bundle.cables)
    {
        CHECK(glm::distance(s.points.front(), D3(0)) <= 3.0 + 1e-9);
        CHECK(glm::distance(s.points.back(), D3(200, 0, 0)) <= 3.0 + 1e-9);
    }
    CHECK(glm::distance(bundle.cables[0].points.front(), bundle.cables[1].points.front()) > 1e-6);
    // The same seed gives the same bundle; another seed a different one.
    curve::CableResult again = curve::makeCables({D3(0), D3(200, 0, 0)}, false, o);
    CHECK(glm::distance(again.cables[3].points[1], bundle.cables[3].points[1]) < 1e-12);
    o.seed = 8;
    CHECK(glm::distance(curve::makeCables({D3(0), D3(200, 0, 0)}, false, o).cables[3].points.front(), bundle.cables[3].points.front()) > 1e-6);

    CHECK_FALSE(curve::makeCables({D3(0)}, false, {}).warnings.empty());
    curve::CableResult same = curve::makeCables({D3(0), D3(0), D3(10, 0, 0)}, false, {});
    CHECK(same.cables.size() == 1);
    CHECK_FALSE(same.warnings.empty());
}

TEST_CASE("drape settles a cable onto a surface it would pass through")
{
    // A ground plane at y = 0 under a long, slack cable from 20 cm up.
    geo::Mesh ground = geo::makePlane({400.0, 400.0, 4, 4});
    geo::MeshBvh bvh(ground);
    curve::CableOptions o;
    o.slack = 0.3;
    o.samples = 48;
    o.clearance = 1.0;
    o.closest = [&](const D3& p) -> std::optional<curve::SurfacePoint> {
        geo::BvhClosest c;
        if (!bvh.closestPoint(p, c))
            return std::nullopt;
        return curve::SurfacePoint{c.point, ground.faceNormal(c.face)};
    };
    curve::CableResult hanging = curve::makeCables({D3(-100, 20, 0), D3(100, 20, 0)}, false, o);
    REQUIRE(hanging.cables.size() == 1);
    double lowest = 1e9;
    for (const D3& p : curve::tessellate(hanging.cables[0]).points)
        lowest = std::min(lowest, p.y);
    CHECK(lowest < -10.0);

    o.drape = true;
    curve::CableResult draped = curve::makeCables({D3(-100, 20, 0), D3(100, 20, 0)}, false, o);
    REQUIRE(draped.cables.size() == 1);
    lowest = 1e9;
    for (const D3& p : curve::tessellate(draped.cables[0]).points)
    {
        CHECK(std::isfinite(p.y));
        lowest = std::min(lowest, p.y);
    }
    CHECK(lowest > 0.5);
    CHECK(lowest < 2.0);
}

TEST_CASE("sectionCurves and contour slice meshes into oriented curves")
{
    geo::Mesh torus = geo::makeTorus({50.0, 10.0, 24, 12});
    geo::SectionMesh sm{&torus, glm::dmat4(1.0)};
    curve::SliceOptions so;
    so.tolerance = 0.0;
    curve::Splines cut = curve::sectionCurves({&sm, 1}, D3(0), D3(0, 1, 0), so);
    REQUIRE(cut.size() == 2);
    for (const curve::Spline& s : cut)
    {
        CHECK(s.closed);
        CHECK(s.kind == curve::Kind::Poly);
        // Counter-clockwise about +Y.
        D3 newell(0.0);
        for (size_t i = 0; i < s.size(); ++i)
            newell += glm::cross(s.points[i], s.points[(i + 1) % s.size()]);
        CHECK(newell.y > 0.0);
        for (const D3& p : s.points)
            CHECK(std::abs(p.y) < 1e-9);
    }

    // An open plane cut through its middle stays one open chain, start lower along the plane's axis.
    geo::Mesh plane = geo::makePlane({100.0, 100.0, 5, 5});
    geo::SectionMesh pm{&plane, glm::dmat4(1.0)};
    curve::Splines open = curve::sectionCurves({&pm, 1}, D3(0), D3(1, 0, 0), {});
    REQUIRE(open.size() == 1);
    CHECK_FALSE(open[0].closed);
    // Collinear crossings simplify down to the two ends.
    CHECK(open[0].size() == 2);
    curve::SliceOptions bridged;
    bridged.bridge = true;
    CHECK(curve::sectionCurves({&pm, 1}, D3(0), D3(1, 0, 0), bridged).empty());

    // A torus 20 cm thick, contoured every 5 cm along Y: planes at -5, 0 and 5, two loops each.
    curve::ContourOptions co;
    co.spacing = 5.0;
    co.slice.tolerance = 0.01;
    co.slice.smooth = true;
    curve::ContourResult r = curve::contour({&sm, 1}, co);
    CHECK(r.planes.size() == 3);
    CHECK(r.curves.size() == 6);
    for (const curve::Spline& s : r.curves)
        CHECK(s.kind == curve::Kind::Bezier);
    co.count = 4;
    CHECK(curve::contour({&sm, 1}, co).planes.size() == 4);
    co.count = 0;
    co.spacing = 1e-5;
    curve::ContourResult tooMany = curve::contour({&sm, 1}, co);
    CHECK(tooMany.curves.empty());
    CHECK_FALSE(tooMany.warnings.empty());
}

TEST_CASE("conformToSurface puts points and handles on the surface")
{
    geo::Mesh plane = geo::makePlane({100.0, 100.0, 2, 2});
    geo::MeshBvh bvh(plane);
    curve::Spline s;
    s.kind = curve::Kind::Bezier;
    s.points = {D3(-20, 7, 0), D3(20, -4, 5)};
    s.handleL = {D3(-30, 7, 0), D3(10, 10, 5)};
    s.handleR = {D3(-10, 12, 0), D3(30, -4, 5)};
    s.typeL = s.typeR = {curve::HandleType::Free, curve::HandleType::Free};
    const double before = glm::distance(s.handleR[0], s.points[0]);
    curve::conformToSurface(s, plane, bvh, 2.0);
    for (size_t i = 0; i < s.size(); ++i)
    {
        CHECK(s.points[i].y == doctest::Approx(2.0));
        CHECK(s.handleL[i].y == doctest::Approx(2.0));
        CHECK(s.handleR[i].y == doctest::Approx(2.0));
    }
    CHECK(glm::distance(s.handleR[0], s.points[0]) == doctest::Approx(before));
}

TEST_CASE("conformCurve keeps a curve live on its surface until released")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 200}, {"depth", 200}})[0];
    std::string c = run("curve", {{"type", "poly"}, {"points", pts({D3(-20, 15, 0), D3(0, -6, 4), D3(20, 30, 0)})}, {"name", "OnPlane"}});
    run("conformCurve", {{"args", {c}}, {"mesh", plane}, {"offset", 1.5}});
    for (const curve::Splines s = splinesOf(c); const D3& p : s[0].points) // s keeps the splines alive for the loop
        CHECK(p.y == doctest::Approx(1.5));
    // Moving the surface carries the curve; moving a point slides it along.
    run("move", {{"args", {plane}}, {"by", {0, 10, 0}}});
    for (const curve::Splines s = splinesOf(c); const D3& p : s[0].points) // s keeps the splines alive for the loop
        CHECK(p.y == doctest::Approx(11.5));
    run("setAttr", {{"args", {c + ".surfaceOffset", 0.0}}});
    for (const curve::Splines s = splinesOf(c); const D3& p : s[0].points) // s keeps the splines alive for the loop
        CHECK(p.y == doctest::Approx(10.0));
    // Undo takes the offset change back.
    run("undo");
    CHECK(splinesOf(c)[0].points[0].y == doctest::Approx(11.5));

    run("conformCurve", {{"args", {c}}, {"release", true}});
    run("move", {{"args", {plane}}, {"by", {0, 10, 0}}});
    for (const curve::Splines s = splinesOf(c); const D3& p : s[0].points) // s keeps the splines alive for the loop
        CHECK(p.y == doctest::Approx(11.5));
    CHECK_THROWS_AS(run("conformCurve", {{"args", {c}}}), CommandError);
}

TEST_CASE("contour and section commands are live history")
{
    run("newScene");
    json torus = run("polyTorus", {{"radius", 50}, {"sectionRadius", 10}});
    std::string t = torus[0];
    json made = run("contour", {{"args", {t}}, {"spacing", 5.0}});
    REQUIRE(made.size() == 2);
    CHECK(splinesOf(made[0]).size() == 6);
    run("setAttr", {{"args", {made[1].get<std::string>() + ".count", 1}}});
    CHECK(splinesOf(made[0]).size() == 2);
    // The curves follow the mesh.
    run("move", {{"args", {t}}, {"by", {0, 100, 0}}});
    CHECK(splinesOf(made[0])[0].points[0].y == doctest::Approx(100.0).epsilon(1e-6));

    json sec = run("section", {{"args", {t}}, {"point", {0, 100, 0}}, {"normal", {1, 0, 0}}});
    CHECK(splinesOf(sec[0]).size() == 2);
    json baked = run("section", {{"args", {t}}, {"point", {0, 100, 0}}, {"normal", {0, 0, 1}}, {"constructionHistory", false}});
    CHECK(baked[1].is_null());
    CHECK(splinesOf(baked[0]).size() == 2);
    CHECK_THROWS_AS(run("section", {{"args", {t}}, {"normal", {0, 0, 0}}}), CommandError);
    run("select", {{"args", json::array()}});
    CHECK_THROWS_AS(run("contour"), CommandError);
}

TEST_CASE("cable command: live anchors and slack, bundles, pipe")
{
    run("newScene");
    json made = run("cable", {{"points", pts({D3(0, 100, 0), D3(200, 100, 0)})}, {"slack", 0.1}});
    REQUIRE(made.size() == 1);
    std::string cable = made[0][0], node = made[0][1], anchors = made[0][2];
    curve::Splines s = splinesOf(cable);
    REQUIRE(s.size() == 1);
    CHECK(curve::length(s[0]) == doctest::Approx(220.0).epsilon(0.01));

    run("setAttr", {{"args", {node + ".slack", 0.2}}});
    CHECK(curve::length(splinesOf(cable)[0]) == doctest::Approx(240.0).epsilon(0.01));
    run("setAttr", {{"args", {node + ".count", 3}}});
    CHECK(splinesOf(cable).size() == 3);
    run("setAttr", {{"args", {node + ".count", 1}}});

    // Moving the anchors moves the cable's ends.
    run("move", {{"args", {anchors}}, {"by", {0, 50, 0}}});
    CHECK(splinesOf(cable)[0].points.front().y == doctest::Approx(150.0));

    // Sag depth mode.
    run("setAttr", {{"args", {node + ".sagMode", 1}}});
    run("setAttr", {{"args", {node + ".sag", 30.0}}});
    curve::Spline hung = splinesOf(cable)[0];
    double lowest = 1e9;
    for (const D3& p : curve::tessellate(hung).points)
        lowest = std::min(lowest, p.y);
    CHECK(lowest == doctest::Approx(120.0).epsilon(0.01));

    json piped = run("cable", {{"points", pts({D3(0), D3(100, 0, 0), D3(100, 0, 100)})}, {"pipe", true}, {"pipeRadius", 1.0}});
    REQUIRE(piped[0][3].is_string());
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::MeshPtr m = g.find(piped[0][3].get<std::string>())->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(m);
    CHECK(m->numFaces() > 0);

    CHECK_THROWS_AS(run("cable", {{"points", pts({D3(0)})}}), CommandError);
}
