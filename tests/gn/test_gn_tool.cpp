#include <doctest/doctest.h>

#include "TestUtil.h"
#include "geo/Primitives.h"
#include "gn/Exec.h"
#include "gn/Gn.h"
#include "gn/Tool.h"
#include "scene/Components.h"
#include "scene/Graph.h"

using namespace lila;
using namespace gntest;
using json = nlohmann::json;

namespace tool_tests
{
using N = gn::GNode;
using L = gn::GLink;

gn::Group groupOf(std::vector<N> nodes, std::vector<L> links, std::vector<gn::GInput> inputs = {})
{
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = std::move(links);
    g.inputs = std::move(inputs);
    return g;
}

/** Moves the selected points up by 10. */
gn::Group liftSelected()
{
    return groupOf({N{"S", "ToolSelection", {}, {}}, N{"P", "SetPosition", {}, {{"offset", json::array({0, 10, 0})}}}},
                   {L{"GroupInput", "geometry", "P", "geometry"}, L{"S", "selection", "P", "selection"}, L{"P", "geometry", "GroupOutput", "geometry"}});
}
} // namespace tool_tests

using namespace tool_tests;

TEST_CASE("tool: selection reaches the group and everything counts in object mode")
{
    geo::Mesh cube = geo::makeCube();
    std::array<std::vector<uint32_t>, 3> sel = {std::vector<uint32_t>{0, 1}, {}, {}};
    gn::ToolResult r = gn::runTool(liftSelected(), cube, &sel, {}, {});
    REQUIRE(r.mesh.numPoints() == cube.numPoints());
    int lifted = 0;
    for (geo::Index p = 0; p < cube.pointCapacity(); ++p)
        lifted += r.mesh.position(p).y > cube.position(p).y + 5.0;
    CHECK(lifted == 2);
    CHECK(r.mesh.position(0).y == doctest::Approx(cube.position(0).y + 10.0));
    CHECK(r.mesh.pointAttrs().find(gn::toolSelectionAttr(0)) == nullptr);

    gn::ToolResult all = gn::runTool(liftSelected(), cube, nullptr, {}, {});
    for (geo::Index p = 0; p < cube.pointCapacity(); ++p)
        CHECK(all.mesh.position(p).y == doctest::Approx(cube.position(p).y + 10.0));

    // Outside a tool the Selection node reads as everything too.
    gn::ExecEnv env;
    env.input = gn::meshGeometry(cube);
    gn::GeometryPtr out = gn::execute(*gn::compile(liftSelected()), env);
    CHECK(out->mesh->position(3).y == doctest::Approx(cube.position(3).y + 10.0));
}

TEST_CASE("tool: set selection writes the selection back")
{
    geo::Mesh cube = geo::makeCube();
    gn::Group g = groupOf({N{"I", "Index", {}, {}}, N{"C", "Compare", {}, {{"dataType", "int"}, {"operation", "equal"}, {"b", 2}}},
                           N{"W", "ToolSetSelection", {}, {{"domain", "face"}}}},
                          {L{"GroupInput", "geometry", "W", "geometry"}, L{"I", "value", "C", "a"}, L{"C", "result", "W", "selection"},
                           L{"W", "geometry", "GroupOutput", "geometry"}});
    gn::ToolResult r = gn::runTool(g, cube, nullptr, {}, {});
    REQUIRE(r.selection[2]);
    CHECK(*r.selection[2] == std::vector<uint32_t>{2});
    CHECK(!r.selection[0]);
    CHECK(r.mesh.faceAttrs().find(gn::toolSetSelectionAttr(2)) == nullptr);
}

TEST_CASE("tool: mouse, view and active element reach their nodes")
{
    geo::Mesh cube = geo::makeCube();
    gn::Group g = groupOf({N{"M", "MousePosition", {}, {}}, N{"A", "ActiveElement", {}, {{"domain", "point"}}}, N{"V", "CombineXYZ", {}, {}},
                           N{"P", "SetPosition", {}, {}}},
                          {L{"GroupInput", "geometry", "P", "geometry"}, L{"M", "x", "V", "x"}, L{"M", "regionHeight", "V", "y"}, L{"A", "index", "V", "z"},
                           L{"V", "vector", "P", "offset"}, L{"P", "geometry", "GroupOutput", "geometry"}});
    gn::ToolContext ctx;
    ctx.mouse = {12, 34};
    ctx.region = {800, 600};
    ctx.active = {5, -1, -1};
    gn::ToolResult r = gn::runTool(g, cube, nullptr, ctx, {});
    glm::dvec3 d = r.mesh.position(0) - cube.position(0);
    CHECK(d.x == doctest::Approx(12.0));
    CHECK(d.y == doctest::Approx(600.0));
    CHECK(d.z == doctest::Approx(5.0));
}

TEST_CASE("gizmo: gizmo nodes record the group input they drive")
{
    gn::GInput height{.name = "height", .type = gn::SocketType::Float, .defaultValue = 4.0};
    gn::GInput turn{.name = "turn", .type = gn::SocketType::Float, .defaultValue = 90.0, .subtype = "angle"};
    gn::GInput place{.name = "place", .type = gn::SocketType::Matrix, .defaultValue = nullptr};
    gn::Group g = groupOf({N{"Lin", "LinearGizmo", {}, {{"position", json::array({0, 5, 0})}, {"direction", json::array({0, 0, 2})}, {"style", "box"}}},
                           N{"Dial", "DialGizmo", {}, {{"radius", 2.0}}}, N{"Free", "LinearGizmo", {}, {}}, N{"Move", "TransformGizmo", {}, {}}},
                          {L{"GroupInput", "geometry", "GroupOutput", "geometry"}, L{"GroupInput", "height", "Lin", "value"},
                           L{"GroupInput", "turn", "Dial", "value"}, L{"GroupInput", "place", "Move", "value"}},
                          {height, turn, place});
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    CHECK(plan->gizmoSteps.size() == 4);

    glm::dmat4 m(1.0);
    m[3] = glm::dvec4(1, 2, 3, 1);
    json mj = json::array();
    for (int k = 0; k < 16; ++k)
        mj.push_back(m[k / 4][k % 4]);
    gn::ToolResult r = gn::runTool(g, geo::makeCube(), nullptr, {}, {{"height", 7.0}, {"place", mj}});
    // The unlinked gizmo has nothing to drive, so it records nothing.
    REQUIRE(r.gizmos.size() == 3);
    const gn::GizmoSpec& lin = r.gizmos[0];
    CHECK(lin.kind == gn::GizmoSpec::Kind::Linear);
    CHECK(lin.param == "height");
    CHECK(lin.value == doctest::Approx(7.0));
    CHECK(lin.position.y == doctest::Approx(5.0));
    CHECK(lin.direction.z == doctest::Approx(2.0));
    CHECK(lin.style == "box");
    const gn::GizmoSpec& dial = r.gizmos[1];
    CHECK(dial.kind == gn::GizmoSpec::Kind::Dial);
    CHECK(dial.subtype == "angle");
    CHECK(dial.value == doctest::Approx(3.14159265358979 / 2.0));
    CHECK(dial.radius == doctest::Approx(2.0));
    const gn::GizmoSpec& move = r.gizmos[2];
    CHECK(move.kind == gn::GizmoSpec::Kind::Transform);
    CHECK(move.position.z == doctest::Approx(3.0));

    // Without a collector the gizmo steps are skipped and the result is unchanged.
    gn::ExecEnv env;
    env.input = gn::meshGeometry(geo::makeCube());
    CHECK(gn::execute(*plan, env)->mesh->numPoints() == 8);
}

TEST_CASE("tool: the tool flag saves with the group")
{
    gn::Group g = gn::Group::makeDefault("Flagged");
    CHECK(!gn::Group::fromJson(g.toJson()).tool);
    g.tool = true;
    CHECK(g.toJson().value("tool", false));
    CHECK(gn::Group::fromJson(g.toJson()).tool);
}

TEST_CASE("tool: gnRunTool edits the selected faces of a scene mesh, undoably")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "ToolCube"}})[0].get<std::string>();
    run("gnBuildGroup", args({json::parse(R"({
      "name": "Lift",
      "nodes": [
        {"name": "Sel", "type": "ToolSelection"},
        {"name": "Move", "type": "SetPosition", "values": {"offset": [0, 10, 0]}},
        {"name": "Keep", "type": "ToolSetSelection", "values": {"domain": "face"}}
      ],
      "links": ["GroupInput.geometry -> Move.geometry", "Sel.selection -> Move.selection", "Move.geometry -> Keep.geometry",
                "Sel.selection -> Keep.selection", "Keep.geometry -> GroupOutput.geometry"]
    })")}));
    run("gnSetTool", args({"Lift"}));
    CHECK(gn::toolGroups(scene::Graph::current()) == std::vector<std::string>{"Lift"});

    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.find(cube);
    REQUIRE(n);
    auto ySum = [&] {
        g.evaluate();
        scene::MeshPtr m = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
        double sum = 0.0;
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            sum += m->position(p).y;
        return sum;
    };
    const double before = ySum();
    run("select", args({cube}));
    scene::components::setMode(scene::ComponentMode::Face);
    scene::components::select(n->id(), scene::ComponentMode::Face, {0});
    json edited = run("gnRunTool", args({"Lift"}));
    CHECK(edited == json::array({cube}));
    CHECK(ySum() == doctest::Approx(before + 40.0));
    CHECK(scene::components::selected(n->id(), scene::ComponentMode::Face) == scene::components::Ids{0});
    run("undo");
    CHECK(ySum() == doctest::Approx(before));
    scene::components::setMode(scene::ComponentMode::Object);
}
