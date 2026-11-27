// World Remix sources meeting the map: their own vertical scale, edges the map ends inside of, seams that wander over the
// land under them, builtUp extents, the filter for sources finer than the map, and the erosion pass. Synthetic rasters.

#include "Remix.h"

#include "Fetch.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>

using namespace openworld;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
/** A geographic raster n x n over [lon0, lon0 + span] x [lat0 - span, lat0], value(u, v) with u east and v south, 0 to 1. */
template <class F> terrain::Dem raster(int n, double lon0, double lat0, double span, F value)
{
    terrain::Dem d;
    d.width = d.height = n;
    d.geographic = true;
    d.epsg = 4326;
    d.dx = d.dy = span / (n - 1);
    d.x0 = lon0;
    d.y0 = lat0;
    d.metres.resize(size_t(n) * size_t(n));
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            d.metres[size_t(y) * n + x] = float(value(double(x) / (n - 1), double(y) / (n - 1)));
    return d;
}

struct Scratch
{
    fs::path dir = fs::temp_directory_path() / ("lila_remix_look_" + std::to_string(std::rand()));
    Scratch() { fs::create_directories(dir); }
    ~Scratch() { fs::remove_all(dir); }
    std::string file(const char* name) const { return (dir / name).generic_string(); }
};

/** A broad source flat at 10 m and a small one at 50 m over its middle, both 1:1. */
json pair(const Scratch& s)
{
    terrain::writeGeoTiff(s.file("under.tif"), raster(121, -106.5, 23.06, 0.06, [](double, double) { return 10.0; }));
    terrain::writeGeoTiff(s.file("over.tif"), raster(61, -106.5, 23.02, 0.02, [](double, double) { return 50.0; }));
    return {{"scale", {{"horizontal", 1.0}, {"vertical", 0.5}}},
            {"seams", {{"erosion", 0}, {"detail", 0.0}}},
            {"bathymetry", {{"enabled", false}}},
            {"sources", json::array({{{"id", "under"}, {"dem", s.file("under.tif")}}, {{"id", "over"}, {"dem", s.file("over.tif")}, {"at", {0, 0}}}})}};
}

/** Along rows across the over source's west edge, where the height first passes halfway between the two sources. */
std::set<int> crossings(const RemixResult& r, double lo, double hi)
{
    const double mid = 0.5 * (lo + hi);
    std::set<int> at;
    const Heightfield& h = r.after;
    for (double z = -60000.0; z <= 60000.0; z += 2000.0)
        for (double x = -110000.0; x < 0.0; x += h.cell)
            if (h.heightAt(x, z) >= mid)
            {
                at.insert(int(std::lround(x / h.cell)));
                break;
            }
    return at;
}
} // namespace

TEST_CASE("remix sources: their own vertical scale, full land to a map border inside them, and seams that wander")
{
    Scratch s;
    json j = pair(s);
    j["sources"][1]["vertical"] = "1:1";
    // The map ends inside the broad source on every side.
    j["map"] = {{"center", {0, 0}}, {"size", {400000, 400000}}, {"cell", 1000}};
    const RemixSpec spec = specFromJson(j);
    CHECK(toJson(spec)["sources"][1]["vertical"] == "1:1");
    const std::shared_ptr<RemixResult> straight = runRemix(spec);
    REQUIRE(straight->errors.empty());
    const Heightfield& h = straight->after;
    CHECK(h.heightAt(0, 0) == doctest::Approx(5000.0).epsilon(1e-3));
    // The broad source at half height, right to the map's border (its own edges are outside the map).
    CHECK(h.at(0, 0) == doctest::Approx(500.0).epsilon(1e-3));
    CHECK(h.at(h.nx - 1, h.nz / 2) == doctest::Approx(500.0).epsilon(1e-3));
    const std::set<int> line = crossings(*straight, 500.0, 5000.0);
    CHECK(line.size() <= 2);

    j["sources"][1]["seam"] = 30000;
    j["sources"][1]["seamNoise"] = 40000;
    const std::shared_ptr<RemixResult> wander = runRemix(specFromJson(j));
    const std::set<int> wavy = crossings(*wander, 500.0, 5000.0);
    MESSAGE("seam crossing columns: straight " << line.size() << ", wandering " << wavy.size());
    CHECK(wavy.size() >= 8);
    // It wanders inward only: never out past the data's edge.
    for (int c : wavy)
        CHECK(c * wander->after.cell > -102500.0 - wander->after.cell);

    // A map reaching past a source's edge fades it there as before.
    j["map"]["size"] = {1200000, 1200000};
    const std::shared_ptr<RemixResult> wide = runRemix(specFromJson(j));
    CHECK(wide->after.heightAt(-300000.0, 0.0) < 400.0);
}

TEST_CASE("remix sources: builtUp keeps a city source to its built-up land and water, with a mask settlements keep to")
{
    Scratch s;
    json j = pair(s);
    // Built-up in a disc in the over source's north-west quarter, a harbour basin to its east, fields round them.
    auto cover = [](double u, double v) {
        const double d = std::hypot(u - 0.3, v - 0.3);
        if (d < 0.15)
            return 50.0;
        if (std::abs(u - 0.5) < 0.04 && std::abs(v - 0.3) < 0.04)
            return 80.0;
        return 40.0;
    };
    terrain::writeGeoTiff(s.file("cover.tif"), raster(241, -106.5, 23.02, 0.02, cover), true);
    j["sources"][1]["landcover"] = s.file("cover.tif");
    j["sources"][1]["extent"] = "builtUp";
    j["sources"][1]["seam"] = 5000;
    j["sources"][1]["seamNoise"] = 0;
    const RemixSpec spec = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(spec);
    REQUIRE(r->errors.empty());
    const Heightfield& h = r->after;
    const std::vector<float>* present = h.findMask("remixPresent_over");
    REQUIRE(present);
    // The over source spans about 2.05 km at 1:1; its centre at (0, 0), so u and v map to x and z.
    auto at = [&](double u, double v) { return Vec2((u - 0.5) * 205000.0, (v - 0.5) * 222000.0); };
    const Vec2 town = at(0.3, 0.3), basin = at(0.5, 0.3), fields = at(0.8, 0.8);
    CHECK(h.maskAt("remixPresent_over", town.x, town.y) > 0.99);
    CHECK(h.maskAt("remixPresent_over", basin.x, basin.y) > 0.99);
    CHECK(h.maskAt("remixPresent_over", fields.x, fields.y) < 0.01);
    CHECK(h.heightAt(town.x, town.y) == doctest::Approx(2500.0).epsilon(1e-3));
    CHECK(h.heightAt(fields.x, fields.y) == doctest::Approx(500.0).epsilon(1e-3));
    CHECK(toJson(spec)["sources"][1]["extent"] == "builtUp");
    j["sources"][1]["extent"] = "everywhere";
    CHECK_THROWS(specFromJson(j));
}

TEST_CASE("remix sources: a source finer than the map is filtered to the map's cell, and says so")
{
    Scratch s;
    // A 1 m checkerboard of 0 and 20 m: point samples every 3 m would keep its spikes.
    terrain::writeGeoTiff(s.file("fine.tif"), raster(301, -106.5, 23.02, 0.003, [](double u, double v) {
                              return ((int(std::lround(u * 300.0)) + int(std::lround(v * 300.0))) % 2) ? 20.0 : 0.0;
                          }));
    json j = {{"scale", {{"horizontal", 1.0}, {"vertical", 1.0}}},
              {"seams", {{"erosion", 0}, {"detail", 0.0}}},
              {"bathymetry", {{"enabled", false}}},
              {"map", {{"cell", 300.0}}},
              {"sources", json::array({{{"id", "fine"}, {"dem", s.file("fine.tif")}}})}};
    const std::shared_ptr<RemixResult> r = runRemix(specFromJson(j));
    bool noted = false;
    for (const std::string& n : r->notes)
        noted = noted || n.find("filtered") != std::string::npos;
    CHECK(noted);
    const Heightfield& h = r->after;
    double spread = 0.0;
    int n = 0;
    for (int z = h.nz / 4; z < 3 * h.nz / 4; ++z)
        for (int x = h.nx / 4; x < 3 * h.nx / 4; ++x)
        {
            spread += std::abs(h.at(x, z) - 1000.0f);
            ++n;
        }
    MESSAGE("mean distance from the checkerboard's mean: " << spread / n << " cm");
    CHECK(spread / n < 400.0);
}

TEST_CASE("remix sources: the erosion pass carves the eroding sources and holds the rest and the sea")
{
    Scratch s;
    // Ribbed hills sloping to a sea along the west, under a held source in the middle.
    auto hills = [](double u, double v) {
        if (u < 0.1)
            return -20.0;
        return 5.0 + 120.0 * (u - 0.1) + 25.0 * std::sin(u * 60.0) * std::sin(v * 47.0) + 8.0 * std::sin(u * 211.0 + v * 173.0);
    };
    terrain::writeGeoTiff(s.file("hills.tif"), raster(201, -106.5, 23.06, 0.06, hills));
    terrain::writeGeoTiff(s.file("held.tif"), raster(61, -106.47, 23.02, 0.02, [](double u, double v) { return 70.0 + 10.0 * std::sin(u * 40.0 + v * 30.0); }));
    json j = {{"scale", {{"horizontal", 1.0}, {"vertical", 1.0}}},
              {"seams", {{"erosion", 0}, {"detail", 0.0}}},
              {"bathymetry", {{"enabled", false}}},
              {"sources", json::array({{{"id", "hills"}, {"dem", s.file("hills.tif")}}, {{"id", "held"}, {"dem", s.file("held.tif")}, {"at", {150000, 0}}, {"erosion", 0}}})}};
    const std::shared_ptr<RemixResult> plain = runRemix(specFromJson(j));
    j["erosion"] = {{"age", 0.5}, {"detail", 0.1}, {"detailScale", 6000.0}};
    const RemixSpec spec = specFromJson(j);
    CHECK(toJson(spec)["erosion"]["age"] == 0.5);
    const std::shared_ptr<RemixResult> eroded = runRemix(spec);
    REQUIRE(eroded->errors.empty());
    REQUIRE(eroded->stats.contains("erosion"));
    const Heightfield& a = plain->after;
    const Heightfield& b = eroded->after;
    REQUIRE(a.size() == b.size());
    CHECK(b.findMask("wear"));
    CHECK(b.findMask("deposit"));
    double hillsMoved = 0.0, heldMoved = 0.0, seaMoved = 0.0;
    int hillsN = 0, heldN = 0, seaN = 0;
    for (int z = 0; z < a.nz; ++z)
        for (int x = 0; x < a.nx; ++x)
        {
            const size_t i = a.index(x, z);
            const double share = (*b.findMask("remixErosion"))[i];
            const double d = std::abs(double(b.heights[i]) - double(a.heights[i]));
            if (a.heights[i] < -100.0f)
                seaMoved += d, ++seaN;
            else if (share < 1e-4)
                heldMoved = std::max(heldMoved, d), ++heldN;
            else if (share > 0.999 && a.heights[i] > 1000.0f)
                hillsMoved += d, ++hillsN;
        }
    REQUIRE(hillsN > 100);
    REQUIRE(heldN > 100);
    REQUIRE(seaN > 100);
    MESSAGE("mean change: hills " << hillsMoved / hillsN << " cm, held most " << heldMoved << " cm, sea " << seaMoved / seaN << " cm");
    CHECK(hillsMoved / hillsN > 10.0);
    CHECK(heldMoved < 1.0);
    CHECK(seaMoved / seaN < 1.0);
    j["erosion"]["preset"] = "volcanic";
    CHECK_THROWS(specFromJson(j));
}
