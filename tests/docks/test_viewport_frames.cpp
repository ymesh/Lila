#include <doctest/doctest.h>

#include "gn/Gn.h"
#include "gn/InstanceView.h"
#include "ops/Command.h"
#include "render/FrameState.h"
#include "scene/Graph.h"
#include "scene/SceneCommands.h"
#include "ui/ViewportWidget.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
size_t drawsOf(const render::FrameState& s, scene::NodeId id)
{
    size_t n = 0;
    for (const render::MeshDraw& d : s.meshes)
        n += d.node == uint64_t(id);
    return n;
}
} // namespace

TEST_CASE("viewport: a modified mesh's cage wire is laid on only once something shows it")
{
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    ops::execute("newScene", json::object());
    const std::string cube = ops::execute("polyCube", {{"name", "Caged"}})[0];
    ops::execute("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    ops::execute("select", {{"args", json::array()}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const scene::NodeId id = g.find(cube)->id();

    ui::ViewportWidget view;
    view.resize(400, 300);
    // Shaded, nothing selected: the surface alone, no wire laid onto the subdivided result.
    const size_t unselected = drawsOf(view.liveFrame(), id);
    CHECK(unselected == 1);

    // Selecting it shows the cage wire, so the next frame builds it.
    ops::execute("select", {{"args", {cube}}});
    const size_t selected = drawsOf(view.liveFrame(), id);
    CHECK(selected > unselected);
}

TEST_CASE("viewport: a geometry-nodes generator on an empty mesh draws its instances")
{
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    gn::registerTypes();
    gn::registerCommands();
    ops::execute("newScene", json::object());
    // The way Buildings makes its cells: an empty mesh whose modifier makes geometry beside instances.
    const std::string mesh = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", "Cell"}}).get<std::string>();
    auto gnArgs = [](std::initializer_list<json> a) { return json{{"args", json(a)}}; };
    ops::execute("gnCreateGroup", gnArgs({"Gen"}));
    const std::string grid = ops::execute("gnAddNode", gnArgs({"Gen", "MeshGrid"})).get<std::string>();
    const std::string iop = ops::execute("gnAddNode", gnArgs({"Gen", "InstanceOnPoints"})).get<std::string>();
    const std::string cube = ops::execute("gnAddNode", gnArgs({"Gen", "MeshCube"})).get<std::string>();
    const std::string join = ops::execute("gnAddNode", gnArgs({"Gen", "JoinGeometry"})).get<std::string>();
    ops::execute("gnConnect", gnArgs({"Gen", grid + ".mesh", iop + ".points"}));
    ops::execute("gnConnect", gnArgs({"Gen", cube + ".mesh", iop + ".instance"}));
    ops::execute("gnConnect", gnArgs({"Gen", grid + ".mesh", join + ".geometry"}));
    ops::execute("gnConnect", gnArgs({"Gen", iop + ".instances", join + ".geometry"}));
    ops::execute("gnConnect", gnArgs({"Gen", join + ".geometry", "GroupOutput.geometry"}));
    ops::execute("gnApply", gnArgs({mesh, "Gen"}));
    ops::execute("select", {{"args", json::array()}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const scene::NodeId id = g.find(mesh)->id();
    REQUIRE(gn::instanceViewFor(g, id));

    ui::ViewportWidget view;
    view.resize(400, 300);
    const render::FrameState frame = view.liveFrame();
    size_t placements = 0;
    for (const render::MeshDraw& d : frame.meshes)
        if (d.node == uint64_t(id) && d.instances)
            placements += d.instances->size();
    // The grid's points each carry a cube, drawn instanced next to the grid itself.
    CHECK(drawsOf(frame, id) >= 2);
    CHECK(placements >= 4);
}
