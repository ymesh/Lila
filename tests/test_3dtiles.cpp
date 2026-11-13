#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Georef.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace lila;
namespace fs = std::filesystem;

namespace
{
const fs::path kData = fs::path(LILA_TEST_3DTILES_DATA);

/** World bounds of every mesh under node. */
std::pair<glm::dvec3, glm::dvec3> meshBounds(scene::Graph& g, scene::NodeId node, size_t* faces = nullptr)
{
    g.evaluate();
    glm::dvec3 lo(1e300), hi(-1e300);
    std::vector<scene::NodeId> all = g.descendants(node);
    for (scene::NodeId id : all)
    {
        const scene::Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || !scene::effectivelyVisible(g, id, true))
            continue;
        const scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        if (!m)
            continue;
        if (faces)
            *faces += size_t(m->numFaces());
        const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
            {
                const glm::dvec3 q(w * glm::dvec4(m->position(p), 1.0));
                lo = glm::min(lo, q);
                hi = glm::max(hi, q);
            }
    }
    return {lo, hi};
}

glm::dvec3 sceneOfEcef(const glm::dvec3& e)
{
    const json r = ops::execute("georeferenceConvert", {{"points", {{e.x, e.y, e.z}}}, {"from", "EPSG:4978"}, {"to", "scene"}});
    return {r[0][0].get<double>(), r[0][1].get<double>(), r[0][2].get<double>()};
}

template <class T> void put(std::string& s, T v) { s.append(reinterpret_cast<const char*>(&v), sizeof(T)); }

/** A pnts tile: n points around rtc (Earth-centred metres), 1 m apart along x, red. */
std::string pnts(const glm::dvec3& rtc, int n)
{
    std::string bin;
    for (int i = 0; i < n; ++i)
    {
        put(bin, float(i));
        put(bin, 0.0f);
        put(bin, 0.0f);
    }
    const size_t rgbAt = bin.size();
    for (int i = 0; i < n; ++i)
        bin.append({static_cast<char>(0xff), 0, 0});
    while (bin.size() % 8)
        bin += '\0';
    std::string ft = json{{"POINTS_LENGTH", n}, {"POSITION", {{"byteOffset", 0}}}, {"RGB", {{"byteOffset", rgbAt}}}, {"RTC_CENTER", {rtc.x, rtc.y, rtc.z}}}.dump();
    while ((28 + ft.size()) % 8)
        ft += ' ';
    std::string out = "pnts";
    put(out, uint32_t(1));
    put(out, uint32_t(28 + ft.size() + bin.size()));
    put(out, uint32_t(ft.size()));
    put(out, uint32_t(bin.size()));
    put(out, uint32_t(0));
    put(out, uint32_t(0));
    return out + ft + bin;
}
} // namespace

TEST_CASE("3D Tiles: a tileset's b3dm lands on its Earth-centred place, picked by geometric error")
{
    ops::execute("newScene", json::object());
    scene::georef::setCurrent(std::nullopt);
    scene::Graph& g = scene::Graph::current();
    const fs::path tileset = kData / "DiscreteLOD" / "tileset.json";
    const json low = ops::execute("import3dTiles", {{"args", {tileset.string()}}, {"maxError", 2.0}, {"name", "Low"}});
    CHECK(low["tiles"].get<int>() == 1);
    CHECK(low["skipped"].empty());
    const auto geo = scene::georef::current();
    REQUIRE(geo);
    CHECK(geo->crs == "EPSG:32618");

    // The root's box is centred on its transform's translation; its transform scales by 100, so the dragon fills
    // 1420 x 628 x 1008 m.
    size_t lowFaces = 0;
    const auto [lo, hi] = meshBounds(g, g.find(low["node"].get<std::string>())->id(), &lowFaces);
    const glm::dvec3 centre = sceneOfEcef({1215107.7612304366, -4736682.902037748, 4081926.095098698});
    const double size = std::sqrt(1420.0 * 1420.0 + 628.0 * 628.0 + 1008.0 * 1008.0) * 100.0;
    CHECK(glm::length((lo + hi) * 0.5 - centre) < size * 0.01);
    CHECK(glm::length(hi - lo) == doctest::Approx(size).epsilon(0.15));

    // A finer error takes the medium child instead of the root (refine REPLACE).
    const json medium = ops::execute("import3dTiles", {{"args", {tileset.string()}}, {"maxError", 0.5}, {"name", "Medium"}});
    CHECK(medium["tiles"].get<int>() == 1);
    size_t mediumFaces = 0;
    const auto [mlo, mhi] = meshBounds(g, g.find(medium["node"].get<std::string>())->id(), &mediumFaces);
    CHECK(mediumFaces > lowFaces * 2);
    CHECK(glm::length((mlo + mhi) * 0.5 - (lo + hi) * 0.5) < size * 0.01);
    scene::georef::setCurrent(std::nullopt);
}

TEST_CASE("3D Tiles: i3dm instances stand inside their tile's region")
{
    ops::execute("newScene", json::object());
    scene::georef::setCurrent(std::nullopt);
    scene::Graph& g = scene::Graph::current();
    const json r = ops::execute("import3dTiles", {{"args", {(kData / "TreeBillboards" / "tileset.json").string()}}});
    CHECK(r["tiles"].get<int>() == 1);
    CHECK(r["skipped"].empty());
    const scene::NodeId root = g.find(r["node"].get<std::string>())->id();
    g.evaluate();
    int inside = 0, total = 0;
    for (scene::NodeId id : g.children(root))
    {
        const scene::Node* n = g.node(id);
        if (!scene::effectivelyVisible(g, id, true))
            continue;
        ++total;
        const glm::dvec3 p(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
        const json llh = ops::execute("georeferenceConvert", {{"points", {{p.x, p.y, p.z}}}, {"to", "geographic"}});
        const double lat = glm::radians(llh[0][0].get<double>()), lon = glm::radians(llh[0][1].get<double>());
        // Turned to east-north-up where it stands: the tree's height runs along the scene's up.
        const auto [lo, hi] = meshBounds(g, id);
        const glm::dvec3 ext = hi - lo;
        const double vertical = scene::georef::sceneZUp() ? ext.z : ext.y;
        CHECK(vertical > 0.8 * std::max({ext.x, ext.y, ext.z}));
        inside += lat > 0.6988582109 - 1e-6 && lat < 0.6988897891 + 1e-6 && lon > -1.3197004795898053 - 1e-6 && lon < -1.3196595204101946 + 1e-6;
    }
    CHECK(total > 10);
    CHECK(inside == total);
    scene::georef::setCurrent(std::nullopt);
}

TEST_CASE("3D Tiles: a composite of point tiles comes in as point clouds at their place")
{
    ops::execute("newScene", json::object());
    scene::georef::setCurrent(std::nullopt);
    const glm::dvec3 a(1215107.0, -4736682.0, 4081926.0), b = a + glm::dvec3(100.0, 0.0, 0.0);
    const std::string t0 = pnts(a, 5), t1 = pnts(b, 4);
    std::string cmpt = "cmpt";
    put(cmpt, uint32_t(1));
    put(cmpt, uint32_t(16 + t0.size() + t1.size()));
    put(cmpt, uint32_t(2));
    cmpt += t0 + t1;
    const fs::path dir = fs::temp_directory_path() / "lila_3dtiles";
    fs::create_directories(dir);
    const fs::path file = dir / "points.cmpt";
    std::ofstream(file, std::ios::binary) << cmpt;
    // A single tile carries no bounding volume, so the scene needs its georeference first.
    CHECK_THROWS(ops::execute("import3dTiles", {{"args", {file.string()}}}));
    ops::execute("setGeoreference", {{"crs", "EPSG:32618"}, {"latLonHeight", {40.0430, -75.6119, 0.0}}});
    const json r = ops::execute("import3dTiles", {{"args", {file.string()}}});
    CHECK(r["points"].get<int>() == 9);
    scene::Graph& g = scene::Graph::current();
    int clouds = 0;
    for (scene::NodeId id : g.children(g.find(r["node"].get<std::string>())->id()))
        if (g.node(id)->type().isA("pointCloud"))
        {
            ++clouds;
            const json info = ops::execute("pointCloudInfo", {{"args", {g.node(id)->path()}}});
            const glm::dvec3 mn(info["boundsMin"][0].get<double>(), info["boundsMin"][1].get<double>(), info["boundsMin"][2].get<double>());
            const glm::dvec3 at(g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix) * glm::dvec4(mn, 1.0));
            // Each tile's first point is its RTC centre.
            const double d = std::min(glm::length(at - sceneOfEcef(a)), glm::length(at - sceneOfEcef(b)));
            CHECK(d < 600.0);
        }
    CHECK(clouds == 2);
    scene::georef::setCurrent(std::nullopt);
}
