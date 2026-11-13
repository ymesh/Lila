#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Georef.h"
#include "scene/Graph.h"
#include "scene/SceneData.h"
#include "scene/Transform.h"

#include <filesystem>

#include <cmath>

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

TEST_CASE("PROJ knows UTM and finds latitude and longitude")
{
    const georef::CrsInfo utm = georef::crsInfo("EPSG:32633");
    REQUIRE(utm.valid);
    CHECK(utm.projected);
    CHECK(georef::usable(utm));
    CHECK(utm.authority == "EPSG:32633");
    CHECK(utm.name.find("UTM zone 33N") != std::string::npos);
    CHECK(utm.unitToMetre == doctest::Approx(1.0));
    CHECK_FALSE(georef::usable(georef::crsInfo("EPSG:4326")));
    CHECK_FALSE(georef::crsInfo("EPSG:not-a-code").valid);

    // Zone 33's central meridian, on the equator.
    const auto ll = georef::toGeographic("EPSG:32633", {500000.0, 0.0, 12.0});
    REQUIRE(ll);
    CHECK(ll->x == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(ll->y == doctest::Approx(15.0).epsilon(1e-9));
    CHECK(ll->z == doctest::Approx(12.0));
    const auto back = georef::fromGeographic("EPSG:32633", {42.0, 15.0, 0.0});
    REQUIRE(back);
    CHECK(back->x == doctest::Approx(500000.0).epsilon(1e-6));
    CHECK(back->y == doctest::Approx(4649776.22).epsilon(1e-6));

    // British National Grid uses its own datum: PROJ goes through it, and back.
    const auto bng = georef::fromGeographic("EPSG:27700", {51.5007, -0.1246, 0.0});
    REQUIRE(bng);
    CHECK(std::abs(bng->x - 530200.0) < 300.0);
    CHECK(std::abs(bng->y - 179700.0) < 300.0);
    const auto bngBack = georef::toGeographic("EPSG:27700", *bng);
    REQUIRE(bngBack);
    CHECK(bngBack->x == doctest::Approx(51.5007).epsilon(1e-9));
    CHECK(bngBack->y == doctest::Approx(-0.1246).epsilon(1e-7));
}

TEST_CASE("scene points map onto the projected grid")
{
    Clean clean;
    georef::Georeference g{"EPSG:32633", {500000.0, 4649776.0, 120.0}};
    // Y-up: +X north, +Z east, +Y up; cm to metres.
    const glm::dvec3 p = georef::toProjected(g, {300.0, 200.0, 100.0}, false);
    CHECK(p.x == doctest::Approx(500001.0));
    CHECK(p.y == doctest::Approx(4649779.0));
    CHECK(p.z == doctest::Approx(122.0));
    const glm::dvec3 q = georef::fromProjected(g, p, false);
    CHECK(glm::length(q - glm::dvec3(300.0, 200.0, 100.0)) < 1e-6);
    // Z-up: +X east, +Y north.
    const glm::dvec3 z = georef::toProjected(g, {100.0, 300.0, 200.0}, true);
    CHECK(glm::length(z - p) < 1e-9);
}

TEST_CASE("setGeoreference saves with the scene, undoes and refuses unprojected CRSs")
{
    Clean clean;
    CHECK(call("georeference").is_null());
    const json r = call("setGeoreference", {{"crs", "EPSG:32633"}, {"llh", {42.0, 15.0, 30.0}}});
    CHECK(r["authority"] == "EPSG:32633");
    REQUIRE(georef::current());
    CHECK(georef::current()->origin.x == doctest::Approx(500000.0).epsilon(1e-6));
    CHECK(r["originLatLonHeight"][0].get<double>() == doctest::Approx(42.0).epsilon(1e-9));
    CHECK(data::toJson().contains("georeference"));
    CHECK_THROWS_AS(call("setGeoreference", {{"crs", "EPSG:4326"}}), CommandError);

    const json moved = call("georeferenceConvert", {{"points", {{0.0, 0.0, 100.0}}}, {"from", "scene"}, {"to", "projected"}});
    CHECK(moved[0][0].get<double>() == doctest::Approx(500001.0).epsilon(1e-9));
    const json home = call("georeferenceConvert", {{"points", {{42.0, 15.0, 30.0}}}, {"from", "geographic"}, {"to", "scene"}});
    CHECK(std::abs(home[0][0].get<double>()) < 1e-3);
    CHECK(std::abs(home[0][2].get<double>()) < 1e-3);
    const json out = georef::exportJson();
    CHECK(out["wgs84"]["latitude"].get<double>() == doctest::Approx(42.0).epsilon(1e-9));
    CHECK(out["wgs84"]["longitude"].get<double>() == doctest::Approx(15.0).epsilon(1e-9));
    CHECK(call("setGeoreference", {{"clear", true}}).is_null());
    CHECK_FALSE(georef::current());
}

TEST_CASE("a georeferenced file sets the scene's georeference or moves into it")
{
    Clean clean;
    const georef::Georeference first{"EPSG:32633", {500000.0, 4649776.0, 0.0}};
    const georef::Placement a = georef::place(first);
    CHECK(a.adopted);
    CHECK(a.message.find("Set the scene's georeference") != std::string::npos);
    CHECK(a.matrix == glm::dmat4(1.0));
    REQUIRE(georef::current());

    // The same CRS 1 km east, 2 km north and 10 m up: a plain move, in scene axes and cm.
    const georef::Placement b = georef::place({"EPSG:32633", {501000.0, 4651776.0, 10.0}});
    CHECK_FALSE(b.adopted);
    CHECK(b.message.find("Moved into") != std::string::npos);
    CHECK(b.matrix[3].z == doctest::Approx(100000.0).epsilon(1e-9));
    CHECK(b.matrix[3].y == doctest::Approx(1000.0).epsilon(1e-9));
    CHECK(b.matrix[3].x == doctest::Approx(200000.0).epsilon(1e-9));
    CHECK(b.matrix[0].x == doctest::Approx(1.0).epsilon(1e-9));

    // The next zone over: transformed, turned by the grids' convergence, and landing where PROJ says.
    const auto inZone34 = georef::convert("EPSG:32633", "EPSG:32634", {{740000.0, 4649776.0, 0.0}});
    REQUIRE(inZone34);
    const georef::Placement c = georef::place({"EPSG:32634", (*inZone34)[0]});
    CHECK(c.message.find("Transformed from EPSG:32634") != std::string::npos);
    CHECK(c.matrix[3].z == doctest::Approx(240000.0 * 100.0).epsilon(1e-6));
    CHECK(std::abs(c.matrix[3].x) < 1.0);
    // Grid convergence between neighbouring zones at 42 N is a few degrees: the file's north (scene +X) turns toward east.
    const double turn = std::atan2(c.matrix[0].z, c.matrix[0].x);
    CHECK(std::abs(turn) > 0.01);
    CHECK(std::abs(turn) < 0.2);
}

TEST_CASE("georeferenceSearch finds CRSs by name and code")
{
    const json r = call("georeferenceSearch", {{"limit", 2000}}) ;
    CHECK(r.size() > 100);
    bool found = false;
    for (const auto& [code, name] : georef::search("utm zone 33n"))
        found = found || code == "EPSG:32633";
    CHECK(found);
}

TEST_CASE("georef: the globe frame matches where Cesium for Unreal puts a point")
{
    // The Asiago survey's origin; Cesium 2.31 in UE 5.8 put this grid position at the globe position below.
    georef::Georeference g;
    g.crs = "EPSG:32632";
    g.origin = {696700.0, 5077800.0, 1300.0};
    const auto globe = georef::globeFrame(g);
    REQUIRE(globe);
    CHECK(glm::length(globe->toGlobe(glm::dvec3(0.0))) < 1e-6);
    const glm::dvec3 p = globe->toGlobe({2868.0, 1570.0, -3718.0});
    CHECK(std::abs(p.x - 2817.12) < 1.0);
    CHECK(std::abs(p.y - 1660.34) < 1.0);
    CHECK(std::abs(p.z - (-3718.01)) < 1.0);
    // Grid north turns by the meridian convergence there, about (lon - 9) sin(lat) = 1.82 degrees.
    const glm::dmat3 r = globe->rotationAt(glm::dvec3(0.0));
    CHECK(std::abs(glm::degrees(std::atan2(r[0].y, r[0].x))) == doctest::Approx(1.82).epsilon(0.02));
    CHECK(glm::determinant(r) == doctest::Approx(1.0));
}
