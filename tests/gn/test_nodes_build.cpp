#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "gn/Exec.h"
#include "gn/Gn.h"

using namespace lila;
using json = nlohmann::json;

namespace build_tests
{
using N = gn::GNode;
using L = gn::GLink;

gn::GeometryPtr evalNodes(std::vector<N> nodes, std::vector<L> links)
{
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = std::move(links);
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    return gn::execute(*plan, {});
}

N node(const char* name, const char* type, json values = json::object()) { return N{name, type, {}, std::move(values)}; }

/** src's output into op's input, op's output to the group output. */
gn::GeometryPtr chain(N src, const char* srcOut, N op, const char* opIn, const char* opOut)
{
    std::string s = src.name, o = op.name;
    return evalNodes({std::move(src), std::move(op)}, {L{s, srcOut, o, opIn}, L{o, opOut, "GroupOutput", "geometry"}});
}

bool hasUv(const geo::Mesh& m) { return m.cornerAttrs().has("uv0"); }

double maxX(const gn::CurveSet& cs)
{
    double v = -1e18;
    for (const auto& p : cs.positions)
        v = std::max(v, p.x);
    return v;
}

TEST_CASE("revolve spins a profile into a surface")
{
    // A vertical line 50 off the Y axis: a 16-sided open tube 100 tall.
    auto line = node("Ln", "CurveLine", {{"start", json::array({50, 0, 0})}, {"end", json::array({50, 100, 0})}});
    auto tube = chain(line, "curve", node("Rv", "Revolve"), "curve", "mesh");
    REQUIRE(tube->mesh);
    CHECK(tube->mesh->numFaces() == 16);
    CHECK(hasUv(*tube->mesh));
    CHECK(tube->mesh->bounds().max.x == doctest::Approx(50.0));
    CHECK(tube->mesh->bounds().max.y == doctest::Approx(100.0));
    CHECK(tube->mesh->bounds().min.z == doctest::Approx(-50.0).epsilon(0.01));

    // Half a turn with 8 segments.
    auto half = chain(line, "curve", node("Rv", "Revolve", {{"angle", 180.0}, {"segments", 8}}), "curve", "mesh");
    REQUIRE(half->mesh);
    CHECK(half->mesh->numFaces() == 8);
    CHECK(half->mesh->bounds().min.x == doctest::Approx(-50.0));

    auto none = evalNodes({node("Rv", "Revolve")}, {L{"Rv", "mesh", "GroupOutput", "geometry"}});
    CHECK_FALSE(none->mesh);
}

TEST_CASE("loft runs a surface through ordered curves")
{
    // Two circles, one raised 100: a tube between them, 4 rows high.
    auto low = node("C1", "CurveCircle", {{"resolution", 12}});
    auto high = node("C2", "CurveCircle", {{"resolution", 12}, {"radius", 25.0}});
    auto lift = node("Tr", "TransformGeometry", {{"translation", json::array({0, 100, 0})}});
    auto join = node("J", "JoinGeometry");
    auto loft = node("Lo", "Loft", {{"spansBetween", 4}, {"smooth", false}});
    auto m = evalNodes({low, high, lift, join, loft}, {L{"C2", "curve", "Tr", "geometry"}, L{"C1", "curve", "J", "geometry"}, L{"Tr", "geometry", "J", "geometry"},
                                                     L{"J", "geometry", "Lo", "curve"}, L{"Lo", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(m->mesh);
    CHECK(m->mesh->numFaces() == 12 * 4);
    CHECK(hasUv(*m->mesh));
    CHECK(m->mesh->bounds().max.y == doctest::Approx(100.0));
    CHECK(m->mesh->bounds().max.x == doctest::Approx(50.0));

    auto capped = evalNodes({low, high, lift, join, node("Lo", "Loft", {{"spansBetween", 1}, {"caps", true}})},
                            {L{"C2", "curve", "Tr", "geometry"}, L{"C1", "curve", "J", "geometry"}, L{"Tr", "geometry", "J", "geometry"},
                             L{"J", "geometry", "Lo", "curve"}, L{"Lo", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(capped->mesh);
    CHECK(capped->mesh->numFaces() > 12);

    // One curve has nothing to loft to.
    auto single = chain(low, "curve", node("Lo", "Loft"), "curve", "mesh");
    CHECK_FALSE(single->mesh);
}

TEST_CASE("straight skeleton of a rectangle: arcs and a roof")
{
    auto rect = node("Q", "CurveQuadrilateral", {{"width", 200.0}, {"height", 100.0}});
    auto sk = chain(rect, "curve", node("S", "StraightSkeleton"), "curve", "skeleton");
    REQUIRE(sk->curves);
    // Four hip arcs and the ridge.
    CHECK(sk->curves->numCurves() == 5);
    for (const auto& p : sk->curves->positions)
        CHECK(p.y == doctest::Approx(0.0));

    auto lifted = chain(rect, "curve", node("S", "StraightSkeleton", {{"lift", true}, {"pitch", 45.0}}), "curve", "skeleton");
    double top = -1e9;
    for (const auto& p : lifted->curves->positions)
        top = std::max(top, p.y);
    CHECK(top == doctest::Approx(50.0).epsilon(0.01));

    auto hip = chain(rect, "curve", node("S", "StraightSkeleton", {{"pitch", 45.0}}), "curve", "roof");
    REQUIRE(hip->mesh);
    CHECK(hip->mesh->numFaces() >= 4);
    CHECK(hip->mesh->bounds().max.y == doctest::Approx(50.0).epsilon(0.01));
    CHECK(hasUv(*hip->mesh));

    auto gable = chain(rect, "curve", node("S", "StraightSkeleton", {{"roof", "gable"}, {"pitch", 45.0}}), "curve", "roof");
    REQUIRE(gable->mesh);
    CHECK(gable->mesh->bounds().max.y == doctest::Approx(50.0).epsilon(0.01));
    CHECK(gable->mesh->faceAttrs().has("part"));
}
TEST_CASE("polygon offset grows, shrinks, merges and splits regions")
{
    auto rect = node("Q", "CurveQuadrilateral", {{"width", 100.0}, {"height", 100.0}});
    auto grown = chain(rect, "curve", node("O", "PolygonOffset", {{"distance", 10.0}}), "curve", "curve");
    REQUIRE(grown->curves);
    REQUIRE(grown->curves->numCurves() == 1);
    CHECK(maxX(*grown->curves) == doctest::Approx(60.0));
    CHECK(grown->curves->numPoints() == 4);

    auto rounded = chain(rect, "curve", node("O", "PolygonOffset", {{"distance", 10.0}, {"join", "round"}}), "curve", "curve");
    REQUIRE(rounded->curves);
    CHECK(maxX(*rounded->curves) == doctest::Approx(60.0).epsilon(0.01));

    auto shrunk = chain(rect, "curve", node("O", "PolygonOffset", {{"distance", -10.0}}), "curve", "curve");
    REQUIRE(shrunk->curves);
    CHECK(maxX(*shrunk->curves) == doctest::Approx(40.0));

    auto steps = chain(rect, "curve", node("O", "PolygonOffset", {{"distance", -30.0}, {"steps", 3}}), "curve", "curve");
    REQUIRE(steps->curves);
    CHECK(steps->curves->numCurves() == 3);

    auto gone = chain(rect, "curve", node("O", "PolygonOffset", {{"distance", -60.0}}), "curve", "curve");
    CHECK((!gone->curves || gone->curves->numCurves() == 0));

    // Two squares 10 apart merge when grown by 10.
    auto right = node("Tr", "TransformGeometry", {{"translation", json::array({110, 0, 0})}});
    auto pair = evalNodes({rect, right, node("J", "JoinGeometry"), node("O", "PolygonOffset", {{"distance", 10.0}})},
                          {L{"Q", "curve", "Tr", "geometry"}, L{"Q", "curve", "J", "geometry"}, L{"Tr", "geometry", "J", "geometry"},
                           L{"J", "geometry", "O", "curve"}, L{"O", "curve", "GroupOutput", "geometry"}});
    REQUIRE(pair->curves);
    CHECK(pair->curves->numCurves() == 1);

    // A dumbbell (two squares joined by a thin bar) splits when shrunk past the bar's half width.
    auto bar = node("B", "CurveQuadrilateral", {{"width", 60.0}, {"height", 10.0}});
    auto barAt = node("Tb", "TransformGeometry", {{"translation", json::array({55, 0, 0})}});
    auto un = node("U", "PolygonOffset", {{"distance", 0.001}});
    auto split = node("O", "PolygonOffset", {{"distance", -8.0}});
    auto dumbbell = evalNodes({rect, right, bar, barAt, node("J", "JoinGeometry"), un, split},
                              {L{"Q", "curve", "Tr", "geometry"}, L{"Q", "curve", "J", "geometry"}, L{"Tr", "geometry", "J", "geometry"},
                               L{"B", "curve", "Tb", "geometry"}, L{"Tb", "geometry", "J", "geometry"}, L{"J", "geometry", "U", "curve"},
                               L{"U", "curve", "O", "curve"}, L{"O", "curve", "GroupOutput", "geometry"}});
    REQUIRE(dumbbell->curves);
    CHECK(dumbbell->curves->numCurves() == 2);
}

TEST_CASE("mesh tiler along curves and over a grid")
{
    // A 20 cm module along a 100 cm line: five copies end to end.
    auto module = node("M", "MeshCube", {{"size", json::array({20, 10, 10})}});
    auto line = node("Ln", "CurveLine", {{"end", json::array({100, 0, 0})}});
    auto fit = evalNodes({module, line, node("T", "MeshTiler")},
                         {L{"M", "mesh", "T", "module"}, L{"Ln", "curve", "T", "target"}, L{"T", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(fit->mesh);
    CHECK(fit->mesh->bounds().min.x == doctest::Approx(0.0).epsilon(0.01));
    CHECK(fit->mesh->bounds().max.x == doctest::Approx(100.0).epsilon(0.01));
    CHECK(fit->mesh->numFaces() == 5 * 6);
    CHECK(fit->mesh->numPoints() == 5 * 8);

    auto stretch = evalNodes({module, line, node("T", "MeshTiler", {{"fit", "stretch"}, {"count", 2}})},
                             {L{"M", "mesh", "T", "module"}, L{"Ln", "curve", "T", "target"}, L{"T", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(stretch->mesh);
    CHECK(stretch->mesh->numFaces() == 2 * 6);
    CHECK(stretch->mesh->bounds().max.x == doctest::Approx(100.0).epsilon(0.01));

    // Over the default grid's 4 quads: one module per quad, standing on it.
    auto grid = node("G", "MeshGrid");
    auto surf = evalNodes({module, grid, node("T", "MeshTiler", {{"fit", "stretch"}})},
                          {L{"M", "mesh", "T", "module"}, L{"G", "mesh", "T", "target"}, L{"T", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(surf->mesh);
    CHECK(surf->mesh->numFaces() == 4 * 6);
    CHECK(surf->mesh->bounds().min.y == doctest::Approx(0.0).epsilon(0.01));
    CHECK(surf->mesh->bounds().max.y == doctest::Approx(10.0).epsilon(0.01));
    CHECK(surf->mesh->bounds().max.x == doctest::Approx(50.0).epsilon(0.01));
}

TEST_CASE("cable hangs between anchors")
{
    auto line = node("Ln", "CurveLine", {{"end", json::array({200, 0, 0})}});
    auto cable = evalNodes({line, node("C", "Cable", {{"mode", "sag"}, {"sag", 30.0}})},
                           {L{"Ln", "curve", "C", "anchors"}, L{"C", "curve", "GroupOutput", "geometry"}});
    REQUIRE(cable->curves);
    CHECK(cable->curves->numCurves() == 1);
    gn::CurveSet e = cable->curves->evaluated();
    double low = 1e9;
    for (const auto& p : e.positions)
        low = std::min(low, p.y);
    CHECK(low == doctest::Approx(-30.0).epsilon(0.03));

    auto tube = evalNodes({line, node("C", "Cable", {{"count", 3}})}, {L{"Ln", "curve", "C", "anchors"}, L{"C", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(tube->mesh);
    CHECK(tube->mesh->numFaces() > 3 * 8);

    auto curves3 = evalNodes({line, node("C", "Cable", {{"count", 3}})}, {L{"Ln", "curve", "C", "anchors"}, L{"C", "curve", "GroupOutput", "geometry"}});
    CHECK(curves3->curves->numCurves() == 3);
}

TEST_CASE("rock from a seed")
{
    auto a = evalNodes({node("R", "Rock", {{"seed", 3}})}, {L{"R", "mesh", "GroupOutput", "geometry"}});
    auto b = evalNodes({node("R", "Rock", {{"seed", 3}})}, {L{"R", "mesh", "GroupOutput", "geometry"}});
    auto c = evalNodes({node("R", "Rock", {{"seed", 4}})}, {L{"R", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(a->mesh);
    CHECK(a->mesh->numFaces() > 500);
    CHECK(a->mesh->validate().empty());
    CHECK(hasUv(*a->mesh));
    geo::Bounds bb = a->mesh->bounds();
    CHECK(bb.min.y == doctest::Approx(0.0));
    CHECK(bb.size().x == doctest::Approx(100.0));
    CHECK(bb.size().y == doctest::Approx(70.0));
    CHECK(bb.size().z == doctest::Approx(80.0));
    CHECK(a->mesh->positions()[5] == b->mesh->positions()[5]);
    CHECK(a->mesh->positions()[5] != c->mesh->positions()[5]);

    auto smooth = evalNodes({node("R", "Rock", {{"faceting", 0.0}, {"cuts", 0}, {"erosion", 0.0}, {"detail", 3}})}, {L{"R", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(smooth->mesh);
    CHECK(smooth->mesh->numPoints() == 162);
}
} // namespace build_tests
