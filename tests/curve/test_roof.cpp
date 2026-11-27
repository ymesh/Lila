#include <doctest/doctest.h>

#include "TestUtil.h"

#include "curve/Roof.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;
using namespace curvetest;

namespace
{
curve::Spline footprint(std::vector<D3> pts)
{
    curve::Spline s;
    s.kind = curve::Kind::Poly;
    s.closed = true;
    s.points = std::move(pts);
    s.normalize();
    return s;
}

curve::Spline rectFootprint(double w, double d) { return footprint({D3(0, 0, 0), D3(w, 0, 0), D3(w, 0, -d), D3(0, 0, -d)}); }

int faces(const geo::Mesh& m, int part)
{
    auto p = m.faceAttrs().get<int32_t>("part");
    int c = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && f < p.size() && p[f] == part)
            ++c;
    return c;
}

int open(const geo::Mesh& m)
{
    int c = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        c += m.edgeValid(e) && m.isBoundaryEdge(e) ? 1 : 0;
    return c;
}

/** Every roof face slopes up away from its eave: normals point up and out. */
void checkUp(const geo::Mesh& m)
{
    auto p = m.faceAttrs().get<int32_t>("part");
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && p[f] == 0)
            CHECK(m.faceNormal(f).y > 0.1);
}
} // namespace

TEST_CASE("roof: hip and gable roofs over a rectangle")
{
    curve::RoofOptions o;
    o.pitch = 45.0;
    curve::RoofResult r = curve::roof({rectFootprint(20, 10)}, o);
    CHECK(r.warnings.empty());
    // Two trapezoids and two hip triangles meeting at a ridge 5 up.
    CHECK(faces(r.mesh, 0) == 4);
    CHECK(r.mesh.bounds().max.y == doctest::Approx(5.0));
    CHECK(r.mesh.bounds().min.y == doctest::Approx(0.0));
    CHECK(open(r.mesh) == 4);
    checkUp(r.mesh);
    CHECK(r.mesh.cornerAttrs().has(geo::attr::uvName(0)));

    o.gableEnds = true;
    r = curve::roof({rectFootprint(20, 10)}, o);
    CHECK(faces(r.mesh, 0) == 2);
    CHECK(faces(r.mesh, 1) == 2);
    CHECK(r.mesh.bounds().max.y == doctest::Approx(5.0));
    // The gables stand straight up from the short sides.
    auto part = r.mesh.faceAttrs().get<int32_t>("part");
    for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
        if (r.mesh.faceValid(f) && part[f] == 1)
            CHECK(std::abs(r.mesh.faceNormal(f).y) < 1e-9);
    checkUp(r.mesh);

    // Gable edges by index, a ridge height, an overhang.
    o.gableEnds = false;
    o.gables = {1};
    o.height = 8.0;
    o.overhang = 1.0;
    r = curve::roof({rectFootprint(20, 10)}, o);
    CHECK(faces(r.mesh, 1) == 1);
    CHECK(r.mesh.bounds().max.y == doctest::Approx(8.0));
    CHECK(r.mesh.bounds().min.x == doctest::Approx(-1.0));
    CHECK(r.mesh.bounds().min.y < 0.0);
}

TEST_CASE("roof: L, courtyard and a thick slab")
{
    curve::RoofOptions o;
    o.pitch = 30.0;
    curve::Spline l = footprint({D3(0, 0, 0), D3(20, 0, 0), D3(20, 0, -8), D3(8, 0, -8), D3(8, 0, -20), D3(0, 0, -20)});
    curve::RoofResult r = curve::roof({l}, o);
    CHECK(r.warnings.empty());
    CHECK(faces(r.mesh, 0) == 6);
    CHECK(r.mesh.bounds().max.y == doctest::Approx(4.0 * std::tan(glm::radians(30.0))));
    checkUp(r.mesh);

    curve::Spline outer = rectFootprint(30, 30), court = footprint({D3(10, 0, -10), D3(10, 0, -20), D3(20, 0, -20), D3(20, 0, -10)});
    r = curve::roof({outer, court}, o);
    CHECK(r.warnings.empty());
    CHECK(faces(r.mesh, 0) == 8);
    CHECK(r.mesh.bounds().max.y == doctest::Approx(5.0 * std::tan(glm::radians(30.0))));

    o.thickness = 0.5;
    o.overhang = 2.0;
    r = curve::roof({rectFootprint(20, 10)}, o);
    CHECK(open(r.mesh) == 0);
    CHECK(r.mesh.validate().empty());
    CHECK(faces(r.mesh, 2) == 4);
    CHECK(faces(r.mesh, 3) == 4);
}

TEST_CASE("inset and centre lines on the skeleton")
{
    // An L inset by 1 stays one loop; a dumbbell inset past its neck splits in two.
    curve::Spline l = footprint({D3(0, 0, 0), D3(20, 0, 0), D3(20, 0, -8), D3(8, 0, -8), D3(8, 0, -20), D3(0, 0, -20)});
    curve::SkeletonCurvesResult r = curve::insetCurves({l}, 1.0, 1, D3(0), 0.01);
    REQUIRE(r.curves.size() == 1);
    CHECK(r.curves[0].size() == 6);
    curve::Spline bell = footprint({D3(0, 0, 0), D3(6, 0, 0), D3(6, 0, -2), D3(10, 0, -2), D3(10, 0, 0), D3(16, 0, 0), D3(16, 0, -6), D3(10, 0, -6),
                                    D3(10, 0, -4), D3(6, 0, -4), D3(6, 0, -6), D3(0, 0, -6)});
    r = curve::insetCurves({bell}, 1.5, 1, D3(0), 0.01);
    CHECK(r.curves.size() == 2);
    // At 0.5 one loop; at 1 the neck closes exactly, so two; at 1.5 two.
    r = curve::insetCurves({bell}, 1.5, 3, D3(0), 0.01);
    CHECK(r.curves.size() == 5);
    r = curve::insetCurves({bell}, 5.0, 1, D3(0), 0.01);
    CHECK(r.curves.empty());
    CHECK(!r.warnings.empty());
    // A long rectangle's centre line is its ridge.
    r = curve::centerLines({rectFootprint(20, 6)}, D3(0), 0.01);
    REQUIRE(r.curves.size() == 1);
    CHECK(curve::length(r.curves[0]) == doctest::Approx(14.0));
}

TEST_CASE("roof, insetCurve and curveCenterLine commands are live")
{
    run("newScene");
    std::string f = run("curve", {{"type", "poly"},
                                  {"closed", true},
                                  {"points", json::array({json::array({0, 0, 0}), json::array({20, 0, 0}), json::array({20, 0, -10}), json::array({0, 0, -10})})},
                                  {"name", "Footprint"}});
    json r = run("roof", {{"args", {f}}, {"pitch", 45.0}, {"gableEnds", true}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::MeshPtr m = g.find(r[0].get<std::string>())->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    REQUIRE(m);
    CHECK(m->bounds().max.y == doctest::Approx(5.0));
    run("move", {{"args", {f}}, {"by", {0, 100, 0}}});
    g.evaluate();
    m = g.find(r[0].get<std::string>())->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    CHECK(m->bounds().max.y == doctest::Approx(105.0));
    json in = run("insetCurve", {{"args", {f}}, {"distance", 1.0}, {"steps", 2}});
    CHECK(run("curveData", {{"args", {in[0]}}}).size() == 2);
    json c = run("curveCenterLine", {{"args", {f}}});
    CHECK(run("curveData", {{"args", {c[0]}}}).size() == 1);
}

TEST_CASE("roof: complex footprint with gables, overhang and thickness")
{
    curve::Spline f = footprint({D3(0, 0, 0), D3(30, 0, 0), D3(30, 0, -10), D3(22, 0, -10), D3(22, 0, -24), D3(12, 0, -24), D3(12, 0, -10),
                                 D3(0, 0, -10)});
    curve::RoofOptions o;
    o.pitch = 35.0;
    o.gableEnds = true;
    o.overhang = 0.8;
    o.thickness = 0.3;
    curve::RoofResult r = curve::roof({f}, o);
    CHECK(r.warnings.empty());
    CHECK(r.mesh.validate().empty());
    // The left end and the wing's end are gabled; the right end is not shorter than the wall beside it, so it hips.
    CHECK(faces(r.mesh, 1) == 2);
    checkUp(r.mesh);
}

TEST_CASE("roof: gable ends over an irregular footprint stay over the footprint")
{
    // A warehouse lot from real map data: one street side is a straight line in three pieces, the last shorter than its neighbours. It is
    // no gable end: a still edge in line with a moving one sends their corner off along the line.
    const std::vector<glm::dvec2> ring = {{262963.12566250208, 635720.43529067282}, {264155.07367990603, 635104.31100167369},
                                          {264694.73687898059, 634825.35621507734}, {264699.35019978386, 635605.56442395842},
                                          {264221.19960842817, 635608.39169755683}, {264026.83020637278, 635929.79858141229},
                                          {263306.3990770806, 635925.7517459665},   {263150.47653497668, 636227.93815907836},
                                          {262556.71762608667, 635930.50977280119}};
    const double y = 3543.351313;
    std::vector<D3> pts;
    for (const glm::dvec2& q : ring)
        pts.push_back(D3(q.x, y, q.y));
    curve::RoofOptions o;
    o.pitch = 12.0;
    o.overhang = 40.0;
    o.gableEnds = true;
    curve::RoofResult r = curve::roof({footprint(pts)}, o);
    REQUIRE(r.mesh.numFaces() > 0);
    CHECK(r.warnings.empty());
    const geo::Bounds b = r.mesh.bounds();
    CHECK(b.min.x > 262556.7 - 200.0);
    CHECK(b.max.x < 264699.4 + 200.0);
    CHECK(b.min.z > 634825.4 - 200.0);
    CHECK(b.max.z < 636227.9 + 200.0);
    CHECK(b.max.y < y + 400.0);
    checkUp(r.mesh);

    // Asked for by index, that piece's gable would do the same: the roof is hipped and says so.
    o.gableEnds = false;
    o.gables = {1};
    r = curve::roof({footprint(pts)}, o);
    REQUIRE(r.mesh.numFaces() > 0);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("hipped") != std::string::npos);
    CHECK(r.mesh.bounds().max.y < y + 400.0);
    CHECK(faces(r.mesh, 1) == 0);
}

TEST_CASE("polyInsetRegion insets a flat region as one, splitting where it pinches")
{
    run("newScene");
    // A 3 x 1 strip of 10 cm quads with the middle one narrowed: inset as one region.
    std::string plane = run("polyPlane", {{"width", 30}, {"depth", 10}, {"subdivisionsX", 3}, {"subdivisionsZ", 1}})[0];
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::MeshPtr m = g.find(plane)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    json all = json::array();
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        if (m->faceValid(f))
            all.push_back(f);
    REQUIRE(all.size() == 3);
    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {plane}}, {"faces", all}});
    run("polyInsetRegion", {{"distance", 2.0}});
    g.evaluate();
    m = g.find(plane)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    // A band face per outline edge (eight: the long sides keep their middle points) and one inner face 26 x 6.
    CHECK(m->numFaces() == 9);
    CHECK(open(*m) == 8);
    double inner = 0.0;
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        if (m->faceValid(f) && std::abs(m->faceArea(f) - 156.0) < 1e-6)
            inner = m->faceArea(f);
    CHECK(inner == doctest::Approx(156.0));
    CHECK(m->cornerAttrs().has(geo::attr::uvName(0)));
    run("undo");
    g.evaluate();
    CHECK(g.find(plane)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->numFaces() == 3);
    // Past half the depth the inset closes into a ridge: no inner face left.
    run("selectComponents", {{"args", {plane}}, {"faces", all}});
    run("polyInsetRegion", {{"distance", 6.0}});
    g.evaluate();
    m = g.find(plane)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    CHECK(m->numFaces() == 8);
    CHECK(m->validate().empty());
}
