#include "geo/Mesh.h"
#include "layout/LayoutNode.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <doctest/doctest.h>

using namespace lila;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

void reset()
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
}

scene::MeshPtr outMesh(const std::string& path)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(path);
    REQUIRE(n);
    return n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

double lowest(const geo::Mesh& m) { return m.bounds().min.y; }

json nodeLine(double x0, double z0, double x1, double z1, int n = 120)
{
    json pts = json::array();
    for (int i = 0; i <= n; ++i)
    {
        const double t = double(i) / n;
        pts.push_back({x0 + (x1 - x0) * t, 0.0, z0 + (z1 - z0) * t});
    }
    return pts;
}
} // namespace

TEST_CASE("panelLayout: geometry lines cut and sink, sculpt lines only sink, undo puts it back")
{
    reset();
    const std::string mesh = run("polyPlane", {{"name", "Hull"}, {"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 10}, {"subdivisionsZ", 10}})[0];
    const std::string layout = run("panelLayout", {{"args", {mesh}}, {"mirror", "off"}})[0];
    const scene::MeshPtr before = outMesh(mesh);
    REQUIRE(before);
    const auto faces = before->numFaces();

    const json r = run("panelLayoutStroke", {{"args", {layout}}, {"points", nodeLine(-40, 3, 40, 3)}, {"tolerance", 1.0}});
    CHECK(r["changed"].get<bool>());
    scene::MeshPtr cut = outMesh(mesh);
    CHECK(cut->numFaces() > faces);
    CHECK(lowest(*cut) == doctest::Approx(-0.2).epsilon(0.05));

    run("panelLayoutLineSet", {{"args", {layout}}, {"set", "Major"}, {"output", "sculpt"}, {"depth", 0.5}, {"width", 30.0}});
    scene::MeshPtr sunk = outMesh(mesh);
    CHECK(sunk->numFaces() == faces);
    CHECK(lowest(*sunk) < -0.05);

    const json info = run("panelLayoutInfo", {{"args", {layout}}});
    CHECK(info["runs"].size() == 1);
    CHECK(info["sets"].size() == 2);

    UndoStack::get().undo();
    UndoStack::get().undo();
    CHECK(outMesh(mesh)->numFaces() == faces);
    CHECK(lowest(*outMesh(mesh)) == doctest::Approx(0.0));
}

TEST_CASE("panelLayout: a box of lines makes a panel the query sees")
{
    reset();
    const std::string mesh = run("polyPlane", {{"name", "Plate"}, {"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    const std::string layout = run("panelLayout", {{"args", {mesh}}, {"mirror", "off"}})[0];
    json pts = nodeLine(-20, -20, 20, -20, 60);
    for (const json& p : nodeLine(20, -20, 20, 20, 60))
        pts.push_back(p);
    for (const json& p : nodeLine(20, 20, -20, 20, 60))
        pts.push_back(p);
    for (const json& p : nodeLine(-20, 20, -20, -20.4, 60))
        pts.push_back(p);
    run("panelLayoutStroke", {{"args", {layout}}, {"points", pts}, {"tolerance", 1.0}});
    const json info = run("panelLayoutInfo", {{"args", {layout}}});
    CHECK(info["panels"].get<int>() == 2);
    const json q = run("panelLayoutQuery", {{"args", {layout}}, {"points", {{0, 0, 0}, {40, 0, 40}, {20.0, 0, 0}}}});
    REQUIRE(q.size() == 3);
    CHECK(q[0]["panel"] != q[1]["panel"]);
    CHECK(q[2]["distance"].get<double>() < 0.05);
}

TEST_CASE("panelLayout: layouts mirror on X unless told otherwise")
{
    reset();
    const std::string mesh = run("polyPlane", {{"name", "Shield"}, {"width", 100.0}, {"depth", 100.0}})[0];
    const std::string layout = run("panelLayout", {{"args", {mesh}}})[0];
    CHECK(run("panelLayoutInfo", {{"args", {layout}}})["mirror"] == "x");
}

TEST_CASE("panelLayout: mirror and line set commands")
{
    reset();
    const std::string mesh = run("polyPlane", {{"name", "Wing"}, {"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 20}, {"subdivisionsZ", 20}})[0];
    const std::string layout = run("panelLayout", {{"args", {mesh}}, {"mirror", "x"}})[0];
    run("panelLayoutLineSet", {{"args", {layout}}, {"action", "add"}, {"set", "Vents"}, {"output", "none"}});
    run("panelLayoutStroke", {{"args", {layout}}, {"points", nodeLine(10, 0, 40, 0)}, {"tolerance", 1.0}});
    json info = run("panelLayoutInfo", {{"args", {layout}}});
    CHECK(info["mirror"] == "x");
    REQUIRE(info["runs"].size() == 2);
    CHECK(info["runs"][0]["output"] == "none");
    // Lines that only divide panels leave the mesh alone.
    CHECK(lowest(*outMesh(mesh)) == doctest::Approx(0.0));

    run("panelLayoutSetRuns", {{"args", {layout}}, {"runs", {info["runs"][0]["id"]}}, {"set", "Major"}});
    CHECK(lowest(*outMesh(mesh)) < -0.1);
    CHECK_THROWS_AS(run("panelLayoutLineSet", {{"args", {layout}}, {"set", "Nope"}}), CommandError);
}

TEST_CASE("panelLayout: writeAttributes publishes panel attributes for geometry nodes")
{
    reset();
    const std::string mesh = run("polyPlane", {{"name", "Slab"}, {"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 10}, {"subdivisionsZ", 10}})[0];
    const std::string layout = run("panelLayout", {{"args", {mesh}}, {"mirror", "off"}})[0];
    run("panelLayoutLineSet", {{"args", {layout}}, {"set", "Major"}, {"output", "none"}});
    run("panelLayoutStroke", {{"args", {layout}}, {"points", nodeLine(-50, 0, 50, 0)}, {"tolerance", 1.0}});
    CHECK_FALSE(outMesh(mesh)->pointAttrs().has("panelId"));
    run("setAttr", {{"args", {layout + ".writeAttributes", true}}});
    const scene::MeshPtr m = outMesh(mesh);
    REQUIRE(m->pointAttrs().has("panelId"));
    const auto ids = m->pointAttrs().get<int32_t>("panelId");
    int north = -2, south = -2;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
    {
        if (m->position(p).z > 20.0)
            north = ids[p];
        if (m->position(p).z < -20.0)
            south = ids[p];
    }
    CHECK(north >= 0);
    CHECK(south >= 0);
    CHECK(north != south);
}
