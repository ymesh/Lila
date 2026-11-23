#include <doctest/doctest.h>

#include "TestUtil.h"
#include "core/MainQueue.h"
#include "scene/Diagnostics.h"
#include "scene/FloatPrecision.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

using namespace lila;
using namespace lila::scene;
using namespace testutil;

namespace
{
bool warned(const std::string& name)
{
    mainq::drain();
    Graph& g = Graph::current();
    const Node* n = g.find(name);
    if (!n)
        return false;
    for (const diagnostics::Issue* i : diagnostics::issuesOf(n->id()))
        if (i->source == "floatPrecision")
            return true;
    return false;
}
} // namespace

TEST_CASE("float precision: a mesh scaled to planet size is warned about, and tiles fix it")
{
    run("newScene");
    run("polySphere", {{"name", "Planet"}, {"radius", 100.0}});
    CHECK_FALSE(warned("Planet"));
    run("setAttr", {{"args", {"Planet.scale", nlohmann::json::array({1.0e5, 1.0e5, 1.0e5})}}});
    REQUIRE(warned("Planet"));
    Graph& g = Graph::current();
    const double before = floatPrecision::reach(g, g.find("Planet")->id());
    CHECK(before == doctest::Approx(1.0e7).epsilon(0.01));

    CHECK_THROWS(run("tileMesh", {{"args", {"Planet"}}, {"tileSize", 1.0e5}, {"maxTiles", 100}}));

    const nlohmann::json tiles = run("tileMesh", {{"args", {"Planet"}}, {"tileSize", floatPrecision::kDefaultTileSize * 2.0}});
    REQUIRE(tiles.size() > 4);
    CHECK_FALSE(g.find("Planet"));
    for (const auto& t : tiles)
    {
        const NodeId id = g.find(t.get<std::string>())->id();
        CHECK(floatPrecision::reach(g, id) < floatPrecision::kWarnReach);
        CHECK_FALSE(warned(t.get<std::string>()));
    }

    run("undo");
    REQUIRE(g.find("Planet"));
    CHECK(warned("Planet"));
}
