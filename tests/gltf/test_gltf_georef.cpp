#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Georef.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <filesystem>

using namespace lila;
using namespace lila::scene;

namespace
{
json call(const char* cmd, json args = json::object()) { return ops::execute(cmd, args); }

struct Clean
{
    Clean() { georef::setCurrent(std::nullopt); }
    ~Clean() { georef::setCurrent(std::nullopt); }
};
} // namespace

TEST_CASE("a glTF from a georeferenced scene lands where it was in another one")
{
    Clean clean;
    georef::setCurrent(georef::Georeference{"EPSG:32633", {500000.0, 4649776.0, 0.0}});
    const json made = call("polyCube", {{"width", 10.0}});
    const std::string cube = made.is_array() ? made[0].get<std::string>() : made.get<std::string>();
    const auto path = (std::filesystem::temp_directory_path() / "lila_georef_roundtrip.gltf").string();
    call("exportGltf", {{"args", {path, cube}}});
    // The second scene's origin is 1 km west and 500 m south: the cube comes in 1 km east and 500 m north of it.
    georef::setCurrent(georef::Georeference{"EPSG:32633", {499000.0, 4649276.0, 0.0}});
    const json imported = call("importGltf", {{"args", {path}}});
    Graph& g = Graph::current();
    g.evaluate();
    const std::string name = imported.is_array() ? imported[0].get<std::string>() : imported.value("nodes", json::array())[0].get<std::string>();
    const Node* n = g.find(name);
    REQUIRE(n);
    const glm::dvec3 at(n->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
    const glm::dvec3 p = georef::toProjected(*georef::current(), at, georef::sceneZUp());
    CHECK(p.x == doctest::Approx(500000.0).epsilon(1e-9));
    CHECK(p.y == doctest::Approx(4649776.0).epsilon(1e-9));
}
