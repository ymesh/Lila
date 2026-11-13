#include <doctest/doctest.h>

#include "TestUtil.h"

#include "curve/CurveNodes.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <functional>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;
using namespace curvetest;

namespace
{
/** A 30 cm plane of 3x3 quads: points at -15, -5, 5 and 15 on X and Z. */
std::string gridPlane()
{
    run("newScene");
    return run("polyPlane", {{"width", 30}, {"depth", 30}, {"subdivisionsX", 3}, {"subdivisionsZ", 3}})[0];
}

geo::Mesh meshCopy(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    return *n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

D3 translateOf(const std::string& name)
{
    scene::Node* n = scene::Graph::current().find(name);
    REQUIRE(n);
    return n->as<D3>(scene::Transform::kTranslate);
}
} // namespace

TEST_CASE("polyEdgeToCurve: open chain, closed loop, separate chains")
{
    std::string plane = gridPlane();
    json front = edgesWhere(plane, [](const D3& a, const D3& b) { return near(a.z, -15) && near(b.z, -15); });
    REQUIRE(front.size() == 3);
    run("selectComponents", {{"args", {plane}}, {"edges", front}});
    json made = run("polyEdgeToCurve");
    REQUIRE(made.size() == 1);
    json c = curveJson(made[0]);
    CHECK(c[0]["type"] == "bezier");
    CHECK(c[0]["closed"] == false);
    CHECK(c[0]["points"].size() == 4);
    for (const json& p : c[0]["points"])
        CHECK(p[2].get<double>() == doctest::Approx(-15.0));
    // The curve sits at the chain's centre.
    CHECK(translateOf(made[0]).z == doctest::Approx(-15.0));
    CHECK(scene::components::mode() == scene::ComponentMode::Object);
    CHECK(scene::Graph::current().selection().size() == 1);

    json border = edgesWhere(plane, [](const D3& a, const D3& b) {
        auto edge = [](double v) { return near(std::abs(v), 15); };
        return (edge(a.x) && edge(b.x) && near(a.x, b.x)) || (edge(a.z) && edge(b.z) && near(a.z, b.z));
    });
    REQUIRE(border.size() == 12);
    run("selectComponents", {{"args", {plane}}, {"edges", border}});
    made = run("polyEdgeToCurve");
    REQUIRE(made.size() == 1);
    c = curveJson(made[0]);
    CHECK(c[0]["closed"] == true);
    CHECK(c[0]["points"].size() == 12);
    // Bezier corners: the plane's four corners keep vector handles, the straight runs between them are smooth.
    int vectors = 0;
    for (size_t i = 0; i < 12; ++i)
    {
        const json& p = c[0]["points"][i];
        bool corner = near(std::abs(p[0].get<double>()), 15) && near(std::abs(p[2].get<double>()), 15);
        CHECK(c[0]["typeL"][i] == (corner ? "vector" : "auto"));
        vectors += c[0]["typeL"][i] == "vector";
    }
    CHECK(vectors == 4);

    json two = edgesWhere(plane, [](const D3& a, const D3& b) { return near(std::abs(a.z), 15) && near(a.z, b.z); });
    run("selectComponents", {{"args", {plane}}, {"edges", two}});
    CHECK(run("polyEdgeToCurve").size() == 2);
}

TEST_CASE("polyEdgeToCurve: face border, branching, kinds")
{
    std::string plane = gridPlane();
    geo::Mesh m = meshCopy(plane);
    json centre = json::array();
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && glm::length(m.faceCentroid(f)) < 1e-6)
            centre.push_back(f);
    REQUIRE(centre.size() == 1);
    run("selectComponents", {{"args", {plane}}, {"faces", centre}});
    run("selectMode", {{"args", {"face"}}});
    json made = run("polyEdgeToCurve");
    REQUIRE(made.size() == 1);
    json c = curveJson(made[0]);
    CHECK(c[0]["closed"] == true);
    CHECK(c[0]["points"].size() == 4);
    for (const json& t : c[0]["typeL"])
        CHECK(t == "vector");
    // A square is a square: every point of the curve lies on the face's border.
    for (const json& p : c[0]["points"])
        CHECK((near(std::abs(p[0].get<double>()), 5) && near(std::abs(p[2].get<double>()), 5)));

    // A cross: four chains meeting at a branching point.
    json cross = edgesWhere(plane, [](const D3& a, const D3& b) { return (near(a.x, -5) && near(b.x, -5)) || (near(a.z, -5) && near(b.z, -5)); });
    REQUIRE(cross.size() == 6);
    run("selectMode", {{"args", {"edge"}}});
    run("selectComponents", {{"args", {plane}}, {"edges", cross}});
    made = run("polyEdgeToCurve");
    CHECK(made.size() == 4);

    json border = edgesWhere(plane, [](const D3& a, const D3& b) { return near(std::abs(a.z), 15) && near(a.z, b.z); });
    json poly = run("polyEdgeToCurve", {{"args", {plane}}, {"edges", border}, {"type", "poly"}});
    CHECK(curveJson(poly[0])[0]["points"].size() == 4);
    json fitted = run("polyEdgeToCurve", {{"args", {plane}}, {"edges", border}, {"type", "poly"}, {"smooth", true}});
    CHECK(curveJson(fitted[0])[0]["points"].size() == 2);
    json nurbs = run("polyEdgeToCurve", {{"args", {plane}}, {"faces", centre}, {"type", "nurbs"}, {"degree", 3}});
    c = curveJson(nurbs[0]);
    CHECK(c[0]["type"] == "nurbs");
    CHECK(c[0]["closed"] == true);
    // Exact through the corners: the length of the square's border.
    CHECK(c[0]["length"].get<double>() == doctest::Approx(40.0).epsilon(1e-4));
    json open = run("polyEdgeToCurve", {{"args", {plane}}, {"edges", cross}, {"type", "nurbs"}, {"smooth", true}, {"angle", 180.0}});
    CHECK(open.size() == 4);
}

TEST_CASE("polyEdgeToCurve: nurbs corners and undo")
{
    std::string plane = gridPlane();
    // An L: three edges along the front, three up the right side.
    json ell = edgesWhere(plane, [](const D3& a, const D3& b) { return (near(a.z, -15) && near(b.z, -15)) || (near(a.x, 15) && near(b.x, 15)); });
    REQUIRE(ell.size() == 6);
    json made = run("polyEdgeToCurve", {{"args", {plane}}, {"edges", ell}, {"type", "nurbs"}});
    REQUIRE(made.size() == 1);
    json c = curveJson(made[0]);
    CHECK(c[0]["closed"] == false);
    CHECK(c[0]["length"].get<double>() == doctest::Approx(60.0).epsilon(1e-4));

    run("selectMode", {{"args", {"edge"}}});
    run("selectComponents", {{"args", {plane}}, {"edges", ell}});
    std::string name = run("polyEdgeToCurve")[0];
    CHECK(run("objExists", {{"args", {name}}}).get<bool>());
    run("undo");
    CHECK(!run("objExists", {{"args", {name}}}).get<bool>());
    CHECK(scene::components::mode() == scene::ComponentMode::Edge);
    run("redo");
    CHECK(run("objExists", {{"args", {name}}}).get<bool>());
}

TEST_CASE("polyEdgeToCurve: history follows the mesh")
{
    std::string plane = gridPlane();
    json front = edgesWhere(plane, [](const D3& a, const D3& b) { return near(a.z, -15) && near(b.z, -15); });
    json made = run("polyEdgeToCurve", {{"args", {plane}}, {"edges", front}, {"constructionHistory", true}});
    REQUIRE(made.size() == 1);
    CHECK(curveJson(made[0])[0]["points"][0][2].get<double>() == doctest::Approx(-15.0));
    run("move", {{"args", {plane}}, {"by", {0, 7, 0}}});
    const json listed169 = curveJson(made[0]);
    for (const json& p : listed169[0]["points"])
        CHECK(p[1].get<double>() == doctest::Approx(7.0));
    // Without history the curve stays put.
    json baked = run("polyEdgeToCurve", {{"args", {plane}}, {"edges", front}});
    run("move", {{"args", {plane}}, {"by", {0, 3, 0}}});
    const json listed174 = curveJson(baked[0]);
    for (const json& p : listed174[0]["points"])
        CHECK(p[1].get<double>() == doctest::Approx(7.0));
}
