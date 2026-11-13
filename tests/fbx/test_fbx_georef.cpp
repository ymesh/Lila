#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Georef.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <filesystem>
#include <fstream>

using namespace lila;
namespace fs = std::filesystem;

TEST_CASE("a photogrammetry FBX with an offset and .prj lands on its grid")
{
    scene::georef::setCurrent(std::nullopt);
    const fs::path dir = fs::temp_directory_path() / "lila_fbx_georef";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // A cube 1 m east, 2 m north and 3 m up of the export's origin, written as the scan tool would (east, north, up).
    const bool zUp = scene::georef::sceneZUp();
    const json cube = ops::execute("polyCube", {{"width", 20.0}});
    const std::string name = cube.is_array() ? cube[0].get<std::string>() : cube.get<std::string>();
    ops::execute("setAttr", {{"args", {name + ".translate", zUp ? json{100.0, 200.0, 300.0} : json{100.0, 300.0, -200.0}}}});
    ops::execute("exportFbx", {{"args", {(dir / "scan.fbx").string(), name}}});
    ops::execute("delete", {{"args", {name}}});
    std::ofstream(dir / "scan_offset.xyz") << "500000 4649776 100";
    std::ofstream(dir / "scan.prj") << scene::georef::wkt("EPSG:32633");

    const json r = ops::execute("importFbx", {{"args", {(dir / "scan.fbx").string()}}, {"report", true}});
    const auto geo = scene::georef::current();
    REQUIRE(geo);
    CHECK(geo->origin == glm::dvec3(500000.0, 4649776.0, 100.0));
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const scene::Node* n = g.find(r["roots"][0].get<std::string>());
    REQUIRE(n);
    const glm::dvec3 at(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
    const glm::dvec3 p = scene::georef::toProjected(*geo, at, zUp);
    CHECK(p.x == doctest::Approx(500001.0).epsilon(1e-9));
    CHECK(p.y == doctest::Approx(4649778.0).epsilon(1e-9));
    CHECK(p.z == doctest::Approx(103.0).epsilon(1e-9));
    scene::georef::setCurrent(std::nullopt);
}

TEST_CASE("a georeferenced Lila FBX lands at the same place on the Earth in another scene")
{
    scene::georef::setCurrent(std::nullopt);
    const fs::path dir = fs::temp_directory_path() / "lila_fbx_georef_roundtrip";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const bool zUp = scene::georef::sceneZUp();
    ops::execute("setGeoreference", {{"crs", "EPSG:32633"}, {"origin", {500000.0, 4649776.0, 100.0}}});
    const json cube = ops::execute("polyCube", {{"width", 20.0}});
    const std::string name = cube.is_array() ? cube[0].get<std::string>() : cube.get<std::string>();
    ops::execute("setAttr", {{"args", {name + ".translate", json{1000.0, 500.0, -2000.0}}}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const auto written = scene::georef::current();
    REQUIRE(written);
    const glm::dvec3 where = scene::georef::toProjected(*written, glm::dvec3(g.find(name)->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]), zUp);
    ops::execute("exportFbx", {{"args", {(dir / "site.fbx").string(), name}}});
    ops::execute("delete", {{"args", {name}}});

    // The receiving scene sits 2 km further east: the import is moved into it, not adopted.
    ops::execute("setGeoreference", {{"crs", "EPSG:32633"}, {"origin", {502000.0, 4649776.0, 100.0}}});
    const json r = ops::execute("importFbx", {{"args", {(dir / "site.fbx").string()}}, {"report", true}});
    g.evaluate();
    const auto geo = scene::georef::current();
    REQUIRE(geo);
    CHECK(geo->origin.x == 502000.0);
    const scene::Node* n = g.find(r["roots"][0].get<std::string>());
    REQUIRE(n);
    const glm::dvec3 p = scene::georef::toProjected(*geo, glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]), zUp);
    CHECK(p.x == doctest::Approx(where.x).epsilon(1e-9));
    CHECK(p.y == doctest::Approx(where.y).epsilon(1e-9));
    CHECK(p.z == doctest::Approx(where.z).epsilon(1e-9));

    // Without the flag the file's coordinates are kept as they are.
    const json raw = ops::execute("importFbx", {{"args", {(dir / "site.fbx").string()}}, {"report", true}, {"georeference", false}});
    g.evaluate();
    const glm::dvec3 local(g.find(raw["roots"][0].get<std::string>())->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
    CHECK(glm::length(local - glm::dvec3(1000.0, 500.0, -2000.0)) < 1e-6);
    scene::georef::setCurrent(std::nullopt);
}
