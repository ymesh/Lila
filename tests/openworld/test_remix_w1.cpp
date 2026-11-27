// World Remix with OSM buildings and places (W1 kinds) and the sea floor (W6 bathymetry): buildings and the shops in
// them move rigidly with their region, large areas are clipped at region edges, every kind survives, .pbf keeps tagged
// nodes, and the sea floor comes from bathymetry grids and the shelf profile.

#include "Fetch.h"
#include "Inputs.h"
#include "Remix.h"
#include "OpenWorldTest.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <random>

using namespace openworld;
using json = nlohmann::json;

namespace
{
using owtest::data;
constexpr double kX0 = -107.0, kY0 = 24.0, kSpan = 0.0005 * 120;

/** Latitude and longitude of the coast fixture's (u east, v north) in 0..1. */
LatLon uv(double u, double v) { return {kY0 + v * kSpan, kX0 + u * kSpan}; }

json harbourSpec(json regionPolygon)
{
    json j = {{"scale", {{"horizontal", 1.0 / 3.0}, {"vertical", 1.0 / 1.5}}},
              {"sources", json::array({{{"id", "harbour"}, {"dem", data("coast.asc")}, {"osm", data("harbour.json")}}})},
              {"seams", {{"erosion", 0}, {"detail", 0.0}, {"roads", false}, {"rivers", false}, {"coast", false}}}};
    j["regions"] = json::array({{{"id", "centro"}, {"op", "cut"}, {"polygon", regionPolygon}, {"translate", {-30000, 25000}}, {"angle", 25}}});
    return j;
}

double ringArea(const std::vector<Vec2>& r)
{
    double a = 0.0;
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
        a += (r[j].x - r[i].x) * (r[j].y + r[i].y);
    return std::abs(a) * 0.5;
}

std::filesystem::path tempFile(const char* name)
{
    const auto dir = std::filesystem::temp_directory_path() / ("lila_remix_w1_" + std::to_string(std::random_device()()));
    std::filesystem::create_directories(dir);
    return dir / name;
}

/** A geographic grid over [west, east] x [south, north] with height f(lon, lat) metres, written as a GeoTIFF. */
std::string writeGrid(const char* name, double west, double south, double east, double north, double step, const std::function<double(double, double)>& f)
{
    terrain::Dem d;
    d.geographic = true;
    d.epsg = 4326;
    d.width = int(std::round((east - west) / step)) + 1;
    d.height = int(std::round((north - south) / step)) + 1;
    d.x0 = west, d.y0 = north, d.dx = step, d.dy = step;
    d.metres.resize(size_t(d.width) * d.height);
    for (int y = 0; y < d.height; ++y)
        for (int x = 0; x < d.width; ++x)
            d.metres[size_t(y) * d.width + x] = float(f(west + x * step, north - y * step));
    const auto p = tempFile(name);
    terrain::writeGeoTiff(p, d);
    return p.string();
}
} // namespace

TEST_CASE("remix: a building and the restaurant in it move rigidly with their region; every W1 kind comes through")
{
    const Source probe = specFromJson(harbourSpec(json::array({{0, 0}, {1, 0}, {1, 1}}))).sources[0];
    const std::shared_ptr<const SourceData> src = loadSource(probe);
    auto baseOf = [&](LatLon ll) { return placeOnBase(probe, 1.0 / 3.0, src->geo.toLocal(ll)); };
    // The region holds the buildings and park and crosses the farmland's north edge.
    json poly = json::array();
    for (auto [u, v] : {std::pair{0.53, 0.28}, {0.70, 0.28}, {0.70, 0.50}, {0.53, 0.50}})
    {
        const Vec2 b = baseOf(uv(u, v));
        poly.push_back({b.x, b.y});
    }
    const RemixSpec s = specFromJson(harbourSpec(poly));
    const std::shared_ptr<RemixResult> r = runRemix(s);
    REQUIRE(r->errors.empty());
    const Rigid motion = s.regions[0].rigid();
    auto mapped = [&](LatLon ll) { return r->geo.toLocal(ll); };

    // The restaurant node and the courtyard building's rings: each output position is the rigid motion of its base.
    const OsmDoc& in = src->osm;
    size_t rigid = 0;
    for (const OsmDoc::Node& n : in.nodes)
    {
        if (tagOf(n.tags, "amenity") != "restaurant")
            continue;
        bool found = false;
        for (const OsmDoc::Node& o : r->osm.nodes)
            if (tagOf(o.tags, "amenity") == "restaurant")
            {
                CHECK(glm::length(mapped(o.at) - motion.apply(baseOf(n.at))) < 1.0);
                found = true;
                ++rigid;
            }
        CHECK(found);
    }
    const terrain::OsmExtract out = terrain::extractOsm(toOsmData(r->osm));
    const terrain::OsmBuilding* civic = nullptr;
    const terrain::OsmBuilding* dome = nullptr;
    for (const terrain::OsmBuilding& b : out.buildings)
    {
        if (b.building == "civic")
            civic = &b;
        if (b.part && b.roofShape == "dome")
            dome = &b;
    }
    REQUIRE(civic);
    CHECK(civic->holes.size() == 1);
    REQUIRE(dome);
    const terrain::OsmExtract src0 = terrain::extractOsm(toOsmData(in));
    for (const terrain::OsmBuilding& b : src0.buildings)
        if (b.building == "civic")
        {
            REQUIRE(b.outer.size() == civic->outer.size());
            for (size_t i = 0; i < b.outer.size(); ++i)
                CHECK(glm::length(mapped(civic->outer[i]) - motion.apply(baseOf(b.outer[i]))) < 1.0);
            REQUIRE(b.holes.size() == 1);
            for (size_t i = 0; i < b.holes[0].size(); ++i)
                CHECK(glm::length(mapped(civic->holes[0][i]) - motion.apply(baseOf(b.holes[0][i]))) < 1.0);
            ++rigid;
        }
    CHECK(rigid == 2);

    // Places, piers, quays and dams survive.
    bool restaurant = false, park = false;
    for (const terrain::OsmPlace& p : out.places)
    {
        restaurant = restaurant || p.value == "restaurant";
        park = park || p.value == "park";
    }
    CHECK(restaurant);
    CHECK(park);
    std::map<terrain::OsmFeatureKind, int> kinds;
    for (const terrain::OsmFeature& f : out.features)
        ++kinds[f.kind];
    CHECK(kinds[terrain::OsmFeatureKind::Pier] == 1);
    CHECK(kinds[terrain::OsmFeatureKind::Quay] == 1);
    CHECK(kinds[terrain::OsmFeatureKind::Dam] == 1);

    // remixExtract (the same map, through forEachPoint) puts the building and the restaurant where the remixed OSM has them.
    terrain::OsmExtract x = src0;
    CHECK(remixExtract(s, *r, "harbour", x) == 0);
    bool sameBuilding = false, samePlace = false;
    for (const terrain::OsmBuilding& b : x.buildings)
        if (b.building == "civic")
        {
            sameBuilding = glm::length(mapped(b.outer[0]) - mapped(civic->outer[0])) < 1.0 && b.holes.size() == 1 &&
                           glm::length(mapped(b.holes[0][0]) - mapped(civic->holes[0][0])) < 1.0;
        }
    for (const terrain::OsmPlace& p : x.places)
        for (const terrain::OsmPlace& q : out.places)
            if (p.value == "restaurant" && q.value == "restaurant")
                samePlace = glm::length(mapped(p.at) - mapped(q.at)) < 1.0;
    CHECK(sameBuilding);
    CHECK(samePlace);
    CHECK_THROWS(remixExtract(s, *r, "nowhere", x));
}

TEST_CASE("remix: a large area crossing a region's edge is clipped there, not sent whole to one side")
{
    const Source probe = specFromJson(harbourSpec(json::array({{0, 0}, {1, 0}, {1, 1}}))).sources[0];
    const std::shared_ptr<const SourceData> src = loadSource(probe);
    auto baseOf = [&](LatLon ll) { return placeOnBase(probe, 1.0 / 3.0, src->geo.toLocal(ll)); };
    Polygon region;
    json poly = json::array();
    for (auto [u, v] : {std::pair{0.53, 0.28}, {0.70, 0.28}, {0.70, 0.50}, {0.53, 0.50}})
    {
        region.push_back(baseOf(uv(u, v)));
        poly.push_back({region.back().x, region.back().y});
    }
    const RemixSpec s = specFromJson(harbourSpec(poly));
    const std::shared_ptr<RemixResult> r = runRemix(s);
    std::vector<std::vector<Vec2>> pieces;
    for (const OsmDoc::Way& w : r->osm.ways)
        if (tagOf(w.tags, "landuse") == "farmland")
        {
            std::vector<Vec2> ring;
            for (size_t i = 0; i + 1 < w.nodes.size(); ++i)
                ring.push_back(r->geo.toLocal(r->osm.node(w.nodes[i])->at));
            pieces.push_back(ring);
        }
    REQUIRE(pieces.size() == 2);
    // One piece stays (no point inside the cut-out region), the other moved with it (every point inside the region's target).
    const Polygon target = s.regions[0].target();
    int stayed = 0, moved = 0;
    for (const auto& ring : pieces)
    {
        bool allInTarget = true, noneInRegion = true;
        for (Vec2 p : ring)
        {
            allInTarget = allInTarget && (inside(target, p) || distanceTo(target, p) < 5.0);
            noneInRegion = noneInRegion && (!inside(region, p) || distanceTo(region, p) < 5.0);
        }
        stayed += noneInRegion && !allInTarget;
        moved += allInTarget;
    }
    CHECK(stayed == 1);
    CHECK(moved == 1);
    // Area is kept: the two pieces add up to the original (cut is rigid and the background unwarped).
    std::vector<Vec2> original;
    for (auto [u, v] : {std::pair{0.40, 0.05}, {0.90, 0.05}, {0.90, 0.45}, {0.40, 0.45}})
        original.push_back(baseOf(uv(u, v)));
    CHECK(ringArea(pieces[0]) + ringArea(pieces[1]) == doctest::Approx(ringArea(original)).epsilon(0.01));

    // Buildings are never clipped: the church stays one closed ring.
    int churches = 0;
    for (const OsmDoc::Way& w : r->osm.ways)
        churches += tagOf(w.tags, "building") == "church";
    CHECK(churches == 1);
}

TEST_CASE("OSM doc: .pbf keeps tagged nodes (points of interest) like .json")
{
    const std::string dir = std::string(LILA_TEST_OPENWORLD_DATA) + "/../../data/osm/";
    std::vector<std::string> notes;
    const OsmDoc pbf = readOsmDoc(dir + "w1_town.osm.pbf", &notes);
    const OsmDoc js = readOsmDoc(dir + "w1_town.json");
    size_t a = 0, b = 0;
    for (const OsmDoc::Node& n : pbf.nodes)
        a += !n.tags.empty();
    for (const OsmDoc::Node& n : js.nodes)
        b += !n.tags.empty();
    CHECK(a > 0);
    CHECK(a == b);
    CHECK(notes.empty());
    // toOsmData hands them on, so extractOsm sees the places.
    CHECK(terrain::extractOsm(toOsmData(pbf)).places.size() == terrain::extractOsm(toOsmData(js)).places.size());
}

TEST_CASE("remix: the sea floor comes from bathymetry grids and the shelf profile where no source covers")
{
    // A map four times the source's footprint: most of it is uncovered sea.
    json j = {{"scale", {{"horizontal", 1.0 / 3.0}, {"vertical", 1.0 / 1.5}}},
              {"sources", json::array({{{"id", "coast"}, {"dem", data("coast.asc")}}})},
              {"map", {{"size", {500000, 500000}}, {"cell", 2000}}},
              {"seams", {{"erosion", 0}, {"detail", 0.0}}}};
    const std::shared_ptr<RemixResult> prof = runRemix(specFromJson(j));
    CHECK(prof->stats["bathymetry"]["source"] == "profile");
    CHECK(prof->stats["bathymetry"]["seaSamples"].get<size_t>() > 1000);
    const Heightfield& h = prof->after;
    const std::vector<float>* seabed = h.findMask("seabed");
    REQUIRE(seabed);
    // Far out (west of the source) is deeper than near its shore; nowhere at the old flat floor everywhere.
    const double near = h.heightAt(-120000, 0), far = h.heightAt(-240000, 0);
    CHECK(near < 0.0);
    CHECK(far < near);
    CHECK((*seabed)[h.index(2, h.nz / 2)] > 0.5f);
    bool notes = false;
    for (const std::string& n : prof->notes)
        notes = notes || n.find("shelf profile") != std::string::npos;
    CHECK(notes);

    // A bathymetry grid 400 m deep around the place: far from the shore the floor takes it (times the vertical scale).
    const std::string grid = writeGrid("gebco.tif", kX0 - 0.15, kY0 - 0.15, kX0 + kSpan + 0.15, kY0 + kSpan + 0.15, 1.0 / 240.0,
                                       [](double, double) { return -400.0; });
    j["sources"][0]["bathymetry"] = grid;
    j["bathymetry"] = {{"blendWidth", 20000}};
    const std::shared_ptr<RemixResult> g = runRemix(specFromJson(j));
    CHECK(g->stats["bathymetry"]["grids"] == true);
    CHECK(g->stats["bathymetry"]["gridSamples"].get<size_t>() > 0);
    CHECK(g->after.heightAt(-200000, 0) == doctest::Approx(-400.0 * 100.0 / 1.5).epsilon(0.05));

    // Off: the flat floor at map.seaDepth, said so.
    j["bathymetry"] = {{"enabled", false}};
    j["map"]["seaDepth"] = -1234;
    const std::shared_ptr<RemixResult> off = runRemix(specFromJson(j));
    CHECK(off->stats["bathymetry"]["source"] == "flat");
    CHECK(off->after.heightAt(-240000, 0) == doctest::Approx(-1234.0));
    CHECK(off->after.findMask("seabed") == nullptr);
    CHECK_THROWS(specFromJson(json{{"bathymetry", {{"shelfDepth", "deep"}}}}));
}

TEST_CASE("remix: flat sea a land DEM leaves gets the sea floor; land and real bathymetry are kept")
{
    // Land to the east at 50 m, sea flat at 0 to the west, as SRTM-style DEMs have it.
    const std::string dem = writeGrid("flatsea.tif", kX0, kY0, kX0 + kSpan, kY0 + kSpan, 0.0005,
                                      [](double lon, double) { return lon > kX0 + kSpan * 0.5 ? 50.0 : 0.0; });
    json j = {{"scale", {{"horizontal", 1.0}, {"vertical", 1.0}}},
              {"sources", json::array({{{"id", "flat"}, {"dem", dem}}})},
              {"seams", {{"erosion", 0}, {"detail", 0.0}}}};
    const std::shared_ptr<RemixResult> r = runRemix(specFromJson(j));
    const std::shared_ptr<const SourceData> src = loadSource(specFromJson(j).sources[0]);
    CHECK_FALSE(src->flatSea.empty());
    const Vec2 e = src->hf.extent();
    // A quarter in from the west edge, well inside the source: below sea level now. Land three quarters in: unchanged.
    const Vec2 sea = src->hf.origin + Vec2(e.x * 0.2, e.y * 0.5), land = src->hf.origin + Vec2(e.x * 0.8, e.y * 0.5);
    CHECK(r->after.heightAt(sea.x, sea.y) < -100.0);
    CHECK(r->after.heightAt(land.x, land.y) == doctest::Approx(5000.0).epsilon(1e-3));

    // The coast fixture's sea already has depth (-20 m and down): kept as it is.
    const json c = {{"scale", {{"horizontal", 1.0}, {"vertical", 1.0}}},
                    {"sources", json::array({{{"id", "coast"}, {"dem", data("coast.asc")}}})},
                    {"seams", {{"erosion", 0}, {"detail", 0.0}}}};
    const std::shared_ptr<const SourceData> coast = loadSource(specFromJson(c).sources[0]);
    CHECK(coast->flatSea.empty());
}
