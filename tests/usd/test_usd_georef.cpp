// A georeferenced scene exported with exportUsd lands at the same place on the Earth when read back.

#include "UsdTestUtil.h"

#include "scene/Georef.h"

using namespace lila;
using namespace lila::usdtest;

namespace
{
std::string made(const json& r) { return r.is_array() ? r[0].get<std::string>() : r.get<std::string>(); }

glm::dvec3 projectedOf(const std::string& name)
{
    graph().evaluate();
    return scene::georef::toProjected(*scene::georef::current(), glm::dvec3(world(name)[3]), scene::georef::sceneZUp());
}
} // namespace

TEST_CASE("usd georeference: a Lila export lands on its place in a scene with another origin")
{
    fresh();
    scene::georef::setCurrent(std::nullopt);
    run("setGeoreference", {{"crs", "EPSG:32633"}, {"origin", {500000.0, 4649776.0, 100.0}}});
    const std::string cube = made(run("polyCube", {{"width", 20.0}}));
    run("setAttr", {{"args", {cube + ".translate", json{1000.0, 500.0, -2000.0}}}});
    const glm::dvec3 where = projectedOf(cube);
    const fs::path file = tempDir() / "georef.usda";
    run("exportUsd", {{"args", {file.generic_string(), cube}}});
    run("delete", {{"args", {cube}}});

    run("setGeoreference", {{"crs", "EPSG:32633"}, {"origin", {502000.0, 4649776.0, 100.0}}});
    const json r = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
    REQUIRE(r["roots"].size() == 1);
    const glm::dvec3 p = projectedOf(r["roots"][0].get<std::string>());
    CHECK(scene::georef::current()->origin.x == 502000.0);
    CHECK(p.x == doctest::Approx(where.x).epsilon(1e-9));
    CHECK(p.y == doctest::Approx(where.y).epsilon(1e-9));
    CHECK(p.z == doctest::Approx(where.z).epsilon(1e-9));

    // Into a scene with no georeference, the file's is adopted and the coordinates are kept.
    fresh();
    scene::georef::setCurrent(std::nullopt);
    const json adopted = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
    REQUIRE(scene::georef::current());
    CHECK(scene::georef::current()->origin.x == 500000.0);
    CHECK(glm::length(glm::dvec3(world(adopted["roots"][0].get<std::string>())[3]) - glm::dvec3(1000.0, 500.0, -2000.0)) < 1e-6);
    scene::georef::setCurrent(std::nullopt);
}

TEST_CASE("usd georeference: a Cesium georeference prim alone sets the scene on the UTM zone around it")
{
    fresh();
    scene::georef::setCurrent(std::nullopt);
    const fs::path file = tempDir() / "cesium.usda";
    std::ofstream(file) << "#usda 1.0\n(\n    defaultPrim = \"Box\"\n    metersPerUnit = 0.01\n    upAxis = \"Y\"\n)\n\n"
                           "def CesiumGeoreferencePrim \"CesiumGeoreference\"\n{\n"
                           "    double cesium:georeferenceOrigin:latitude = 45.8255\n"
                           "    double cesium:georeferenceOrigin:longitude = 11.5327\n"
                           "    double cesium:georeferenceOrigin:height = 1300\n}\n\n"
                           "def Xform \"Box\"\n{\n    double3 xformOp:translate = (0, 0, 0)\n    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n}\n";
    run("importUsd", {{"args", {file.generic_string()}}});
    const auto geo = scene::georef::current();
    REQUIRE(geo);
    CHECK(geo->crs.find("32632") != std::string::npos);
    const json llh = run("georeferenceConvert", {{"points", {{0.0, 0.0, 0.0}}}, {"to", "geographic"}});
    CHECK(llh[0][0].get<double>() == doctest::Approx(45.8255).epsilon(1e-7));
    CHECK(llh[0][1].get<double>() == doctest::Approx(11.5327).epsilon(1e-7));
    scene::georef::setCurrent(std::nullopt);
}
