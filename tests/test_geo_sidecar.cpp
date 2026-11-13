#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/GeoSidecar.h"
#include "scene/Georef.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <filesystem>
#include <fstream>

using namespace lila;
using namespace lila::scene;
namespace fs = std::filesystem;

namespace
{
const char* kUtm33Wkt =
    R"(PROJCS["WGS_1984_UTM_Zone_33N",GEOGCS["GCS_WGS_1984",DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137.0,298.257223563]],)"
    R"(PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]],PROJECTION["Transverse_Mercator"],PARAMETER["False_Easting",500000.0],)"
    R"(PARAMETER["False_Northing",0.0],PARAMETER["Central_Meridian",15.0],PARAMETER["Scale_Factor",0.9996],PARAMETER["Latitude_Of_Origin",0.0],UNIT["Meter",1.0]])";

fs::path freshDir(const char* name)
{
    const fs::path d = fs::temp_directory_path() / "lila_geo_sidecar" / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

/** An ASCII PLY triangle with these three points (file coordinates). */
void writePly(const fs::path& p, const glm::dvec3 (&pts)[3])
{
    std::string s = "ply\nformat ascii 1.0\nelement vertex 3\nproperty double x\nproperty double y\nproperty double z\nelement face 1\n"
                    "property list uchar int vertex_indices\nend_header\n";
    for (const glm::dvec3& v : pts)
        s += std::to_string(v.x) + " " + std::to_string(v.y) + " " + std::to_string(v.z) + "\n";
    s += "3 0 1 2\n";
    write(p, s);
}

/** The imported mesh's points in world space. */
std::vector<glm::dvec3> importedPoints(const json& names)
{
    Graph& g = Graph::current();
    g.evaluate();
    const Node* n = g.find(names[0].get<std::string>());
    REQUIRE(n);
    const MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
    std::vector<glm::dvec3> out;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p))
            out.push_back(m->position(p));
    return out;
}

struct SidecarClean
{
    SidecarClean() { georef::setCurrent(std::nullopt); }
    ~SidecarClean() { georef::setCurrent(std::nullopt); }
};
} // namespace

TEST_CASE("a Pix4D mesh with its offset and .prj sets the scene's georeference")
{
    SidecarClean clean;
    const fs::path d = freshDir("pix4d");
    writePly(d / "site.ply", {{1.0, 2.0, 3.0}, {11.0, 2.0, 3.0}, {1.0, 12.0, 3.0}});
    write(d / "site_offset.xyz", "500000.000 4649776.000 100.000\n");
    write(d / "site.prj", kUtm33Wkt);
    const auto found = geoSidecar::find(d / "site.ply");
    REQUIRE(found);
    CHECK(found->hasOffset);
    CHECK(found->source == "Pix4D offset");

    const json r = ops::execute("importPly", {{"args", {(d / "site.ply").string()}}, {"report", true}});
    CHECK(r["georeference"].get<std::string>().find("Set the scene's georeference") != std::string::npos);
    const auto g = georef::current();
    REQUIRE(g);
    CHECK(g->origin == glm::dvec3(500000.0, 4649776.0, 100.0));
    const std::vector<glm::dvec3> pts = importedPoints(r["nodes"]);
    REQUIRE(pts.size() == 3);
    const bool zUp = georef::sceneZUp();
    const glm::dvec3 p = georef::toProjected(*g, pts[0], zUp);
    CHECK(p.x == doctest::Approx(500001.0).epsilon(1e-12));
    CHECK(p.y == doctest::Approx(4649778.0).epsilon(1e-12));
    CHECK(p.z == doctest::Approx(103.0).epsilon(1e-12));
}

TEST_CASE("a second georeferenced file moves into the scene's georeference")
{
    SidecarClean clean;
    georef::setCurrent(georef::Georeference{"EPSG:32633", {499000.0, 4649000.0, 0.0}});
    const fs::path d = freshDir("second");
    writePly(d / "tile.ply", {{0.0, 0.0, 0.0}, {5.0, 0.0, 0.0}, {0.0, 5.0, 0.0}});
    write(d / "tile_offset.xyz", "500000 4649776 100");
    write(d / "tile.prj", kUtm33Wkt);
    const json r = ops::execute("importPly", {{"args", {(d / "tile.ply").string()}}, {"report", true}});
    CHECK(r["georeference"].get<std::string>().find("into the scene's georeference") != std::string::npos);
    const glm::dvec3 p = georef::toProjected(*georef::current(), importedPoints(r["nodes"])[0], georef::sceneZUp());
    CHECK(p.x == doctest::Approx(500000.0).epsilon(1e-9));
    CHECK(p.y == doctest::Approx(4649776.0).epsilon(1e-9));
    CHECK(p.z == doctest::Approx(100.0).epsilon(1e-6));
}

TEST_CASE("a metadata.xml in a local ENU frame lands in the UTM zone around it")
{
    SidecarClean clean;
    const fs::path d = freshDir("terra");
    write(d / "metadata.xml", "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ModelMetadata version=\"1\">\n<SRS>ENU:42,15</SRS>\n<SRSOrigin>0,0,0</SRSOrigin>\n</ModelMetadata>\n");
    writePly(d / "block.ply", {{100.0, 50.0, 7.0}, {101.0, 50.0, 7.0}, {100.0, 51.0, 7.0}});
    const json r = ops::execute("importPly", {{"args", {(d / "block.ply").string()}}, {"report", true}});
    const auto g = georef::current();
    REQUIRE(g);
    CHECK(g->crs == "EPSG:32633");
    // Where PROJ puts the ENU point, within a centimetre.
    const auto want = georef::convert("+proj=ortho +lat_0=42.000000 +lon_0=15.000000 +x_0=0 +y_0=0 +ellps=WGS84 +units=m +no_defs +type=crs",
                                      "EPSG:32633", {{100.0, 50.0, 7.0}});
    REQUIRE(want);
    const glm::dvec3 p = georef::toProjected(*g, importedPoints(r["nodes"])[0], georef::sceneZUp());
    CHECK(glm::length(glm::dvec2(p - (*want)[0])) < 0.01);
}

TEST_CASE("an absolute-coordinate mesh with a .prj is rebased on its centre")
{
    SidecarClean clean;
    const fs::path d = freshDir("absolute");
    writePly(d / "scan.ply", {{500100.25, 4649800.5, 50.0}, {500110.25, 4649800.5, 50.0}, {500100.25, 4649810.5, 52.0}});
    write(d / "scan.prj", kUtm33Wkt);
    const json r = ops::execute("importPly", {{"args", {(d / "scan.ply").string()}}, {"report", true}});
    CHECK(r["georeference"].get<std::string>().find("absolute coordinates, rebased") != std::string::npos);
    const auto g = georef::current();
    REQUIRE(g);
    CHECK(g->origin.x == doctest::Approx(500100.0));
    const std::vector<glm::dvec3> pts = importedPoints(r["nodes"]);
    // Near the origin in the scene (its height is kept), exactly where they were on the grid.
    CHECK(glm::length(pts[0]) < 6000.0);
    const glm::dvec3 p = georef::toProjected(*g, pts[2], georef::sceneZUp());
    CHECK(p.x == doctest::Approx(500100.25).epsilon(1e-12));
    CHECK(p.y == doctest::Approx(4649810.5).epsilon(1e-12));
    CHECK(p.z == doctest::Approx(52.0).epsilon(1e-9));
}

TEST_CASE("a mesh with nothing beside it imports as before")
{
    SidecarClean clean;
    const fs::path d = freshDir("plain");
    writePly(d / "rock.ply", {{1.0, 2.0, 3.0}, {2.0, 2.0, 3.0}, {1.0, 3.0, 3.0}});
    CHECK_FALSE(geoSidecar::find(d / "rock.ply"));
    ops::execute("importPly", {{"args", {(d / "rock.ply").string()}}});
    CHECK_FALSE(georef::current());
}
