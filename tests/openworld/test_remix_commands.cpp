// World Remix through the OpenWorld package's commands: create from two sources, add a region and a pin, apply (files
// and features on the terrain), check, read the spec back and undo an edit.

#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "OpenWorldTest.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <random>

using namespace lila;
using json = nlohmann::json;

namespace
{
using owtest::data;
} // namespace

TEST_CASE("remix commands: create, region, pin, apply, check, undo")
{
    owtest::loadPackages(false, false);
    ops::execute("newScene", json::object());
    const json made = ops::execute("remixCreate", {{"name", "World"},
                                                   {"sources", json::array({{{"id", "coast"}, {"dem", data("coast.asc")}, {"osm", data("coast.json")}}})},
                                                   {"spec", {{"seams", {{"erosion", 4}}}}}});
    const std::string mesh = made["mesh"].get<std::string>();
    CHECK(made["stats"]["grid"]["resolution"][0].get<int>() > 50);
    const json src = ops::execute("remixSource", {{"args", {mesh}}, {"id", "inland"}, {"dem", data("inland.asc")}, {"osm", data("inland.json")}, {"at", {250000, -60000}}});
    CHECK(src["stats"]["sources"] == 2);

    const json reg = ops::execute("remixAddRegion", {{"args", {mesh}},
                                                     {"id", "town"},
                                                     {"op", "cut"},
                                                     {"polygon", {{230000, -80000}, {270000, -80000}, {270000, -40000}, {230000, -40000}}},
                                                     {"translate", {-140000, 60000}}});
    CHECK(reg["id"] == "town");
    CHECK(reg["target"][0][0].get<double>() == doctest::Approx(90000.0));
    const json pin = ops::execute("remixPin", {{"args", {mesh}}, {"at", {0, 50000}}, {"to", {0, 40000}}});
    CHECK(pin["id"] == "pin1");

    // The terrain is the remix: Terrain commands see its heightfield.
    const json info = ops::execute("terrainInfo", {{"args", {mesh}}});
    CHECK(info.contains("resolution"));

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("lila_remix_test_" + std::to_string(std::random_device()()));
    const json applied = ops::execute("remixApply", {{"args", {mesh}}, {"folder", dir.string()}});
    CHECK(std::filesystem::exists(applied["files"]["osm"].get<std::string>()));
    CHECK(std::filesystem::exists(applied["files"]["heightfield"].get<std::string>()));
    CHECK(std::filesystem::exists(applied["files"]["dem"].get<std::string>()));
    REQUIRE(applied.contains("features"));
    CHECK(applied["features"]["counts"]["road"].get<int>() > 0);
    CHECK(applied["features"]["counts"]["river"].get<int>() > 0);
    CHECK(applied["check"].contains("ok"));

    // The written DEM reads back where the remixed OSM lies: a terrain from it and the same OSM line up.
    const json check = ops::execute("remixCheck", {{"args", {mesh}}});
    CHECK(check["counts"]["overlap"] == 0);
    CHECK(check["counts"]["fold"] == 0);

    const json spec = ops::execute("remixSpec", {{"args", {mesh}}});
    CHECK(spec["regions"].size() == 1);
    CHECK(spec["pins"].size() == 1);

    ops::execute("remixAddRegion", {{"args", {mesh}}, {"id", "town"}, {"translate", {-100000, 60000}}});
    CHECK(ops::execute("remixSpec", {{"args", {mesh}}})["regions"][0]["translate"][0].get<double>() == doctest::Approx(-100000.0));
    ops::execute("undo", json::object());
    CHECK(ops::execute("remixSpec", {{"args", {mesh}}})["regions"][0]["translate"][0].get<double>() == doctest::Approx(-140000.0));

    const json view = ops::execute("remixView", {{"args", {mesh}}, {"before", true}});
    CHECK(view["before"].get<bool>());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("remix commands: on a Z-up scene the remix stands on the ground and curves map onto it")
{
    owtest::loadPackages(false, false);
    ops::execute("newScene", json::object());
    math::setUpAxis(math::UpAxis::Z);
    const json made = ops::execute("remixCreate", {{"name", "WorldZ"}, {"sources", json::array({{{"id", "coast"}, {"dem", data("coast.asc")}}})}});
    const std::string mesh = made["mesh"].get<std::string>();
    const json rot = ops::execute("getAttr", {{"args", {mesh + ".rotate"}}});
    CHECK(rot[0].get<double>() == doctest::Approx(90.0));
    CHECK(rot[2].get<double>() == doctest::Approx(90.0));
    // World (X, Y) on the ground is the map's (Y, X): a curve drawn on the Z-up ground lands where it was drawn.
    const std::string c = ops::execute("curve", {{"points", {{0, 10000, 0}, {0, 20000, 0}, {10000, 20000, 0}, {10000, 10000, 0}, {0, 10000, 0}}}, {"type", "poly"}})
                              .get<std::string>();
    ops::execute("remixAddRegion", {{"args", {mesh}}, {"id", "r"}, {"op", "keep"}, {"curve", c}});
    const json poly = ops::execute("remixSpec", {{"args", {mesh}}})["regions"][0]["polygon"];
    REQUIRE(poly.size() == 4);
    CHECK(poly[0][0].get<double>() == doctest::Approx(10000.0).epsilon(1e-6));
    CHECK(std::abs(poly[0][1].get<double>()) < 1e-3);
    CHECK(poly[2][0].get<double>() == doctest::Approx(20000.0).epsilon(1e-6));
    CHECK(poly[2][1].get<double>() == doctest::Approx(10000.0).epsilon(1e-6));
    math::setUpAxis(math::UpAxis::Y);
}
