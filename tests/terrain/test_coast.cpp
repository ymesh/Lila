#include "TestUtil.h"
#include "Coast.h"
#include "Fetch.h"
#include "Water.h"

#include "geo/Heightfield.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

using namespace lila;
using namespace terraintest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
constexpr double kLat = 23.2, kLon = -106.4;

/** A DEM-like coast: the sea flat at 0 west of x = 0, land rising east with low bumps. 2.56 km, 10 m cells. */
geo::Heightfield demCoast()
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {256000.0, 256000.0}, 257);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            const double shore = 6000.0 * std::sin(p.y * 0.00003);
            const double land = (p.x - shore) * 0.03 + 80.0 * std::sin(p.x * 0.0011) * std::cos(p.y * 0.0009);
            h.heights[h.index(x, z)] = float(p.x > shore ? std::max(60.0, land) : 0.0);
        }
    return h;
}

/** Georeference putting the heightfield's origin at (kLat, kLon), Local projection. */
terrain::Georef georefFor(const geo::Heightfield& h)
{
    terrain::Georef g;
    g.kind = terrain::ProjectionKind::Local;
    g.latMid = kLat;
    g.mx = 111320.0 * std::cos(kLat / 57.29577951308232);
    g.my = 110570.0;
    g.x0 = kLon;
    g.y0 = kLat;
    g.origin = h.origin;
    return g;
}

/** A coarse GEBCO-like grid over the area: depths growing to 400 m westward, positive (land) east. */
terrain::Dem gebcoFor(const terrain::Georef& g, const geo::Heightfield& h)
{
    terrain::Dem d;
    d.geographic = true;
    d.dx = d.dy = 1.0 / terrain::kGebcoPerDegree;
    const terrain::LatLon nw = g.toGeographic(h.origin - glm::dvec2(5000.0)), se = g.toGeographic(h.origin + h.extent() + glm::dvec2(5000.0));
    d.x0 = nw.lon;
    d.y0 = nw.lat;
    d.width = int((se.lon - nw.lon) / d.dx) + 2;
    d.height = int((nw.lat - se.lat) / d.dy) + 2;
    d.metres.resize(size_t(d.width) * size_t(d.height));
    for (int r = 0; r < d.height; ++r)
        for (int c = 0; c < d.width; ++c)
        {
            const glm::dvec2 p = g.toLocal({d.y0 - r * d.dy, d.x0 + c * d.dx});
            d.metres[size_t(r) * size_t(d.width) + size_t(c)] = float(p.x < 0.0 ? -5.0 + p.x * 0.004 : 10.0 + p.x * 0.0003);
        }
    d.epsg = 4326;
    return d;
}

/** A tidal lagoon: sea west of x = -60000, a flat at +60 cm, higher land east of x = 60000. 1.28 km, 5 m cells. */
geo::Heightfield lagoon()
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {128000.0, 128000.0}, 257);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            double v = p.x < -30000.0 ? -800.0 * std::min(1.0, (-30000.0 - p.x) / 10000.0) : 60.0;
            if (p.x > 40000.0)
                v = 60.0 + (p.x - 40000.0) * 0.08;
            h.heights[h.index(x, z)] = float(v);
        }
    return h;
}

bool hasIssue(const json& issues, const std::string& type, const std::string& about = {})
{
    for (const json& i : issues)
        if (i["type"] == type && (about.empty() || i["about"] == about))
            return true;
    return false;
}
} // namespace

TEST_CASE("coast: bathymetry blends GEBCO under the DEM's flat sea and is continuous at the shoreline")
{
    const geo::Heightfield land = demCoast();
    const terrain::Georef g = georefFor(land);
    const terrain::Dem grid = gebcoFor(g, land);

    // Through a GeoTIFF on disk, as a downloaded tile is read.
    const fs::path dir = fs::temp_directory_path() / "lila_coast_bathy";
    fs::create_directories(dir);
    terrain::writeGeoTiff(dir / "gebco.tif", grid);
    const terrain::Dem back = terrain::readDem(dir / "gebco.tif");
    REQUIRE(back.width == grid.width);

    geo::Heightfield h = land;
    terrain::BathymetrySpec s;
    s.blendWidth = 40000.0;
    const terrain::BathymetryResult r = terrain::applyBathymetry(h, s, &back, &g);
    CHECK(r.source == "grid");
    CHECK(r.seaSamples > size_t(h.size() / 4));
    CHECK(r.gridSamples == r.seaSamples);
    CHECK(r.maxDepth > 20000.0);

    const std::vector<float>& seabed = *h.findMask("seabed");
    double worstStep = 0.0;
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x + 1 < h.nx; ++x)
        {
            const size_t a = h.index(x, z), b = h.index(x + 1, z);
            if ((seabed[a] > 0.5f) != (seabed[b] > 0.5f))
                worstStep = std::max(worstStep, double(std::abs(h.heights[a] - h.heights[b])));
            if (seabed[a] > 0.5f)
                CHECK(h.heights[a] < 0.0f);
        }
    // The DEM's land at the shore is at least 60 cm; the first seabed sample sits a few cm under the water.
    CHECK(worstStep < 150.0);

    // Deeper going out, and near the grid's depth far out.
    const double near = h.heightAt(-3000.0 + 6000.0 * std::sin(0.0), 0.0);
    const double far = h.heightAt(-120000.0, 0.0);
    CHECK(far < near);
    CHECK(far == doctest::Approx(-5.0 * 100.0 - 120000.0 * 0.4).epsilon(0.15));

    // Without a grid: the profile alone, said so.
    geo::Heightfield p = land;
    const terrain::BathymetryResult pr = terrain::applyBathymetry(p, s, nullptr, nullptr);
    CHECK(pr.source == "profile");
    CHECK(pr.gridSamples == 0);
    CHECK(p.heightAt(-120000.0, 0.0) < -1000.0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("coast: channels meet every boat class's minimum width and depth, braided too")
{
    for (const terrain::CraftClass& c : terrain::craftClasses())
    {
        if (c.kind != "boat")
            continue;
        geo::Heightfield h = lagoon();
        terrain::ChannelSpec s;
        s.id = c.name;
        s.craft = c.name;
        s.path = {{-45000.0, 0.0}, {-10000.0, 8000.0}, {20000.0, -6000.0}, {38000.0, 2000.0}};
        s.braids = c.name == "skiff" || c.name == "airboat" ? 3 : 1;
        s.seed = 7;
        std::vector<std::string> warnings;
        const terrain::ChannelResult r = terrain::carveChannel(h, s, 0.0, warnings);
        CAPTURE(c.name);
        for (const std::string& w : warnings)
            CHECK(w.find("coarse") != std::string::npos);
        CHECK(r.strands.size() == size_t(s.braids));
        for (const terrain::ChannelStrand& st : r.strands)
        {
            CHECK(st.minWidth >= c.minWidth);
            CHECK(st.minDepth >= c.minDepth);
        }
        CHECK(terrain::routeIssues(h, [&] {
                  terrain::CoastResult cr;
                  cr.channels = {r};
                  return cr;
              }()).empty());
    }
}

TEST_CASE("coast: lanes stay on water, masks and mangroves are written, stash spots found")
{
    geo::Heightfield h = lagoon();
    h.mask("landcover");
    terrain::CoastSpec spec;
    spec.mangroves.enabled = true;
    spec.channels.push_back(terrain::ChannelSpec::fromJson(
        {{"id", "estuary"}, {"path", {{-45000.0, 0.0}, {0.0, 6000.0}, {35000.0, 0.0}}}, {"craft", "goFast"}, {"braids", 2}, {"seed", 3}}));
    terrain::LaneSpec harbour;
    harbour.id = "marina";
    harbour.craft = "skiff";
    harbour.at = glm::dvec2(-50000.0, -30000.0);
    spec.lanes.push_back(harbour);
    spec.routes.coastDepth = 500.0;
    spec.routes.minLaneLength = 10000.0;
    const terrain::CoastResult r = terrain::applyCoast(h, spec, nullptr);
    CHECK(r.mangroveSource == "band");
    CHECK(r.mangroveArea > 0.0);
    CHECK(r.beachArea >= 0.0);
    CHECK(r.shallowArea > 0.0);
    for (const char* m : {"beach", "surf", "shallows", "reef", "tidal", "mangrove", "channel"})
        CHECK(h.findMask(m));

    std::set<std::string> kinds;
    for (const terrain::Lane& l : r.lanes)
    {
        kinds.insert(l.kind);
        REQUIRE(l.points.size() >= 2);
        for (const glm::dvec3& p : l.points)
            if (h.contains(p.x, p.z))
                CHECK(h.heightAt(p.x, p.z) < p.y);
    }
    CHECK(kinds.count("coast"));
    CHECK(kinds.count("channel"));
    CHECK(kinds.count("harbour"));
    const json issues = terrain::routeIssues(h, r);
    CAPTURE(issues.dump());
    CHECK_FALSE(hasIssue(issues, "laneOverLand"));
    CHECK_FALSE(hasIssue(issues, "channelShallow"));
    CHECK_FALSE(hasIssue(issues, "laneDisconnected"));
    CHECK_FALSE(hasIssue(issues, "harbourUnreachable"));
    CHECK_FALSE(r.stash.empty());
    bool deadEnd = false;
    for (const terrain::StashSpot& s : r.stash)
        deadEnd = deadEnd || s.kind == "deadEnd";
    CHECK(deadEnd);

    // Same spec, same result.
    geo::Heightfield h2 = lagoon();
    h2.mask("landcover");
    const terrain::CoastResult r2 = terrain::applyCoast(h2, spec, nullptr);
    CHECK(h2.heights == h.heights);
    CHECK(r2.lanes.size() == r.lanes.size());

    const json routes = terrain::routesJson(r, {});
    CHECK(routes["format"] == "lila.waterRoutes");
    CHECK(routes["lanes"].size() == r.lanes.size());
    CHECK_FALSE(routes["zoneGraph"]["laneProfiles"].empty());
}

TEST_CASE("coast: mangroves come from the land cover's mangrove class first")
{
    geo::Heightfield h = lagoon();
    std::vector<float>& lc = h.mask("landcover");
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
            if (h.samplePos(x, z).x > -20000.0 && h.samplePos(x, z).x < 0.0)
                lc[h.index(x, z)] = 95.0f / 255.0f;
    terrain::CoastResult r;
    terrain::MangroveSettings s;
    s.enabled = true;
    terrain::applyMangroves(h, 0.0, s, r);
    CHECK(r.mangroveSource == "worldcover");
    CHECK(h.maskAt("mangrove", -10000.0, 0.0) > 0.9);
    CHECK(h.maskAt("mangrove", 20000.0, 0.0) == doctest::Approx(0.0));
}

TEST_CASE("coast: waterCheck catches each failure")
{
    geo::Heightfield h = lagoon();
    terrain::CoastSpec spec;
    spec.routes.coastDepth = 500.0;
    spec.routes.minLaneLength = 10000.0;
    spec.routes.linkReach = 0.0;
    // Too shallow for a go-fast, too narrow for a cutter.
    spec.channels.push_back(terrain::ChannelSpec::fromJson({{"id", "shallow"}, {"path", {{-20000.0, -40000.0}, {30000.0, -40000.0}}}, {"craft", "goFast"}, {"depth", 60.0}}));
    spec.channels.push_back(terrain::ChannelSpec::fromJson({{"id", "narrow"}, {"path", {{-20000.0, 40000.0}, {30000.0, 40000.0}}}, {"craft", "cutter"}, {"width", 600.0}, {"bankAngle", 60.0}}));
    // Over land, over shallow water, far off in open sea, and a berth inland.
    spec.lanes.push_back(terrain::LaneSpec::fromJson({{"id", "overland"}, {"kind", "coast"}, {"craft", "skiff"}, {"path", {{-50000.0, 20000.0}, {55000.0, 20000.0}}}}));
    spec.lanes.push_back(terrain::LaneSpec::fromJson({{"id", "flats"}, {"kind", "coast"}, {"craft", "cutter"}, {"path", {{-36000.0, -60000.0}, {-36000.0, 60000.0}}}}));
    spec.lanes.push_back(terrain::LaneSpec::fromJson({{"id", "lost"}, {"kind", "harbour"}, {"craft", "skiff"}, {"path", {{-63000.0, -63000.0}, {-61000.0, -61000.0}}}}));
    spec.lanes.push_back(terrain::LaneSpec::fromJson({{"id", "inland"}, {"craft", "skiff"}, {"at", {60000.0, 0.0}}}));
    // A strip facing the hills, too short for a twin.
    spec.airstrips.push_back(terrain::AirstripSpec::fromJson({{"id", "strip"}, {"a", {10000.0, -2000.0}}, {"b", {30000.0, -2000.0}}, {"aircraft", "twin"}}));
    const terrain::CoastResult r = terrain::applyCoast(h, spec, nullptr);
    const json issues = terrain::routeIssues(h, r);
    CAPTURE(issues.dump(1));
    CHECK(hasIssue(issues, "channelShallow", "shallow"));
    CHECK(hasIssue(issues, "channelNarrow", "narrow"));
    CHECK(hasIssue(issues, "laneOverLand", "overland"));
    CHECK(hasIssue(issues, "laneShallow", "flats"));
    CHECK(hasIssue(issues, "laneDisconnected", "lost"));
    CHECK(hasIssue(issues, "harbourUnreachable", "inland"));
    CHECK(hasIssue(issues, "airClearance"));
    CHECK(hasIssue(issues, "shortRunway", "strip"));
}

TEST_CASE("coast: a dam holds a reservoir behind its spillway sill")
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {64000.0, 64000.0}, 129);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            h.heights[h.index(x, z)] = float(5000.0 + p.x * 0.15 + std::abs(p.y) * 0.25);
        }
    std::vector<terrain::DamSpec> dams(1);
    dams[0].id = "presa";
    dams[0].a = {0.0, -12000.0};
    dams[0].b = {0.0, 12000.0};
    const std::vector<terrain::DamResult> r = terrain::applyDams(h, dams);
    REQUIRE(r.size() == 1);
    CHECK(r[0].height > 2000.0);
    CHECK(r[0].sill == doctest::Approx(r[0].crest - 300.0));
    CHECK(r[0].downstream.x < -0.9);
    CHECK(h.heightAt(0.0, 0.0) == doctest::Approx(r[0].crest).epsilon(0.001));
    CHECK(h.heightAt(r[0].spillway.x, r[0].spillway.y) == doctest::Approx(r[0].sill).epsilon(0.001));

    terrain::WaterSpec ws;
    terrain::LakeSpec lake;
    lake.id = "presa_reservoir";
    lake.source = "point";
    lake.at = {6000.0, 0.0};
    lake.level = r[0].sill;
    ws.lakes.push_back(lake);
    ws.settings.drainPits = false;
    const terrain::WaterResult w = terrain::applyWater(h, ws);
    REQUIRE_FALSE(w.lakes.empty());
    CHECK(w.lakes[0].level == doctest::Approx(r[0].sill).epsilon(0.001));
    CHECK(w.lakes[0].area > 0.0);
    const std::vector<glm::dvec2> below = terrain::traceDownhill(h, r[0].spillway + r[0].downstream * 4000.0, 30000.0);
    REQUIRE(below.size() > 2);
    CHECK(below.back().x < below.front().x);
}

TEST_CASE("coast: the commands build a coast on an imported DEM, check it, export it and undo")
{
    loadTerrain();
    run("newScene");
    const fs::path dir = fs::temp_directory_path() / "lila_coast_commands";
    fs::create_directories(dir);

    // The land as a GeoTIFF DEM in degrees, and a GEBCO tile around it.
    terrain::Dem land;
    land.geographic = true;
    land.dx = land.dy = 0.0002;
    land.width = land.height = 121;
    land.x0 = kLon;
    land.y0 = kLat;
    land.epsg = 4326;
    land.metres.resize(size_t(land.width) * size_t(land.height));
    for (int r = 0; r < land.height; ++r)
        for (int c = 0; c < land.width; ++c)
            land.metres[size_t(r) * size_t(land.width) + size_t(c)] = c < 50 ? 0.0f : float(1.0 + (c - 50) * 0.6 + 2.0 * std::sin(r * 0.2));
    terrain::writeGeoTiff(dir / "land.tif", land);
    terrain::Dem gebco;
    gebco.geographic = true;
    gebco.dx = gebco.dy = 1.0 / terrain::kGebcoPerDegree;
    gebco.x0 = kLon - 0.02;
    gebco.y0 = kLat + 0.02;
    gebco.width = gebco.height = 20;
    gebco.epsg = 4326;
    gebco.metres.resize(400);
    for (int r = 0; r < 20; ++r)
        for (int c = 0; c < 20; ++c)
            gebco.metres[size_t(r * 20 + c)] = float(gebco.x0 + c * gebco.dx < kLon + 0.01 ? -3.0 - (kLon + 0.01 - (gebco.x0 + c * gebco.dx)) * 4000.0 : 20.0);
    terrain::writeGeoTiff(dir / "gebco.tif", gebco);

    const json t = run("terrainImportDem", {{"args", {(dir / "land.tif").string()}}, {"name", "Coast"}});
    const std::string mesh = t["mesh"];
    REQUIRE(t.contains("georef"));
    run("terrainSeaLevel", {{"args", {mesh}}, {"level", 0.0}});

    const json est = run("terrainBathymetry", {{"args", {mesh}}, {"estimate", true}});
    CHECK(est["estimate"]["bytes"].get<double>() > 0.0);
    const json b = run("terrainBathymetry", {{"args", {mesh}}, {"file", (dir / "gebco.tif").string()}, {"credit", false}});
    CAPTURE(b.dump());
    REQUIRE(b.contains("bathymetry"));
    CHECK(b["bathymetry"]["source"].get<std::string>().find("grid") != std::string::npos);
    CHECK(b["bathymetry"]["maxDepth"].get<double>() > 500.0);

    const json m = run("terrainCoastMasks", {{"args", {mesh}}});
    CHECK(m["masks"]["shallowsM2"].get<double>() > 0.0);
    const json mg = run("terrainMangroves", {{"args", {mesh}}});
    CHECK(mg["mangroveSource"].is_string());
    const json ch = run("terrainAddChannel", {{"args", {mesh}}, {"id", "estuary"}, {"craft", "skiff"},
                                              {"path", {{-40000.0, 0.0}, {-5000.0, 3000.0}, {20000.0, -2000.0}}}});
    REQUIRE(ch["channels"].size() == 1);
    CHECK(ch["channels"][0]["meetsCraft"] == true);
    run("terrainRoutes", {{"args", {mesh}}, {"coastDepth", 300.0}, {"minLaneLength", 5000.0}});
    run("terrainAddLane", {{"args", {mesh}}, {"id", "marina"}, {"craft", "skiff"}, {"at", {-30000.0, -20000.0}}});

    const json routes = run("terrainWaterRoutes", {{"args", {mesh}}});
    CHECK_FALSE(routes["lanes"].empty());
    const json check = run("waterCheck", {{"args", {mesh}}});
    CAPTURE(check.dump(1));
    CHECK_FALSE(hasIssue(check["issues"], "laneOverLand"));
    CHECK_FALSE(hasIssue(check["issues"], "channelShallow"));
    const json tc = run("terrainCheck", {{"args", {mesh}}});
    CHECK(tc["issues"].size() >= check["issues"].size());

    const json stash = run("terrainStashSpots", {{"args", {mesh}}});
    CHECK(stash["scene"]["locators"].size() == stash["spots"].size());
    const json curves = run("terrainRouteCurves", {{"args", {mesh}}});
    CHECK(curves["curves"].size() == routes["lanes"].size());
    const json overlay = run("terrainCoastOverlay", {{"args", {mesh}}, {"show", "all"}});
    CHECK(overlay["lines"].get<int>() > 0);
    run("terrainCoastOverlay", {{"show", "off"}});

    const json files = run("terrainExport", {{"args", {mesh}}, {"target", "files"}, {"folder", (dir / "out").string()}, {"name", "c"}});
    REQUIRE(files.contains("coast"));
    CHECK(fs::exists(dir / "out" / "c_coast.json"));
    CHECK(fs::exists(dir / "out" / "c_routes.json"));
    std::ifstream in(dir / "out" / "c_routes.json");
    const json back = json::parse(in);
    CHECK(back["lanes"].size() == routes["lanes"].size());

    run("terrainCoastRemove", {{"args", {mesh}}, {"ids", {"estuary"}}});
    CHECK(run("terrainCoast", {{"args", {mesh}}})["channels"].empty());
    run("undo");
    CHECK(run("terrainCoast", {{"args", {mesh}}})["channels"].size() == 1);

    // The spec step builds the same pieces.
    const json spec = {{"name", "SpecCoast"},
                       {"source", {{"type", "dem"}, {"file", (dir / "land.tif").string()}}},
                       {"water", {{"sea", {{"level", 0.0}}}}},
                       {"check", false},
                       {"coast",
                        {{"bathymetry", {{"file", (dir / "gebco.tif").string()}, {"credit", false}}},
                         {"masks", true},
                         {"channels", {{{"id", "c1"}, {"craft", "skiff"}, {"path", {{-40000.0, 0.0}, {15000.0, 0.0}}}}}},
                         {"routes", {{"coastDepth", 300.0}, {"minLaneLength", 5000.0}}},
                         {"sceneObjects", false}}}};
    const json built = run("terrainSpec", {{"spec", spec}, {"replace", true}});
    CAPTURE(built.dump());
    bool coastStep = false;
    for (const json& s : built["steps"])
        if (s["step"] == "coast")
            coastStep = s["ok"].get<bool>();
    CHECK(coastStep);
    CHECK(run("terrainCoast", {{"args", {built["terrain"]}}})["channels"].size() == 1);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("coast: terrainAddReservoir dams a valley, fills the reservoir and runs a river below")
{
    loadTerrain();
    run("newScene");
    const fs::path dir = fs::temp_directory_path() / "lila_coast_reservoir";
    fs::create_directories(dir);
    // A valley falling west as a DEM in degrees: 50 m at the middle, rising 0.15 east and 0.25 to either side.
    terrain::Dem dem;
    dem.geographic = true;
    dem.dx = dem.dy = 0.0002;
    dem.width = dem.height = 129;
    dem.x0 = kLon;
    dem.y0 = kLat;
    dem.epsg = 4326;
    dem.metres.resize(size_t(dem.width) * size_t(dem.height));
    const double mx = 111320.0 * std::cos(kLat / 57.29577951308232) * dem.dx, my = 110570.0 * dem.dy;
    for (int r = 0; r < dem.height; ++r)
        for (int c = 0; c < dem.width; ++c)
            dem.metres[size_t(r) * size_t(dem.width) + size_t(c)] = float(50.0 + (c - 64) * mx * 0.15 + std::abs(r - 64) * my * 0.25);
    terrain::writeGeoTiff(dir / "valley.tif", dem);
    const json t = run("terrainImportDem", {{"args", {(dir / "valley.tif").string()}}, {"name", "Valley"}, {"projection", "local"}});
    const std::string mesh = t["mesh"];
    const json r = run("terrainAddReservoir", {{"args", {mesh}}, {"id", "presa"}, {"a", {0.0, -12000.0}}, {"b", {0.0, 12000.0}}});
    CAPTURE(r.dump());
    CHECK(r["dam"]["height"].get<double>() > 2000.0);
    REQUIRE(r.contains("reservoir"));
    CHECK(r["reservoir"]["level"].get<double>() == doctest::Approx(r["dam"]["sill"].get<double>()).epsilon(0.01));
    CHECK(r["reservoir"]["area"].get<double>() > 0.0);
    CHECK(r.contains("river"));
    const json w = run("terrainWater", {{"args", {mesh}}});
    CHECK(w["features"]["lakes"].size() == 1);
    CHECK(w["features"]["rivers"].size() == 1);
    run("terrainCoastRemove", {{"args", {mesh}}, {"ids", {"presa"}}});
    CHECK(run("terrainWater", {{"args", {mesh}}})["features"]["lakes"].empty());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("coast: a grid the terrain can't place leaves the seabed; items off the map are refused or skipped")
{
    auto groundAt = [](const std::string& mesh) {
        const json r = run("terrainSample", {{"args", {mesh}}, {"points", {{-15000.0, -15000.0}}}});
        const json& first = r.is_array() ? r[0] : r["samples"][0];
        return first["height"].get<double>();
    };
    loadTerrain();
    run("newScene");
    const fs::path dir = fs::temp_directory_path() / "lila_coast_nogeo";
    fs::create_directories(dir);
    // A real grid on disk, and a terrain made from nothing: no georeference to place the grid by.
    terrain::Dem gebco;
    gebco.geographic = true;
    gebco.dx = gebco.dy = 1.0 / terrain::kGebcoPerDegree;
    gebco.x0 = kLon - 0.1;
    gebco.y0 = kLat + 0.1;
    gebco.width = gebco.height = 60;
    gebco.epsg = 4326;
    gebco.metres.assign(3600, -400.0f);
    terrain::writeGeoTiff(dir / "gebco.tif", gebco);
    const json t = run("terrainCreate", {{"size", {40000.0, 40000.0}}, {"resolution", 129}, {"flat", true}, {"name", "NoGeo"}});
    const std::string mesh = t["mesh"];
    run("terrainSeaLevel", {{"args", {mesh}}, {"level", 100.0}});
    const double before = groundAt(mesh);
    const json b = run("terrainBathymetry", {{"args", {mesh}}, {"file", (dir / "gebco.tif").string()}, {"credit", false}});
    CAPTURE(b.dump());
    REQUIRE(b.contains("bathymetry"));
    CHECK(b["bathymetry"]["source"] == "none");
    CHECK(b["bathymetry"]["seaSamples"].get<double>() == 0.0);
    bool said = false;
    for (const json& n : b["bathymetry"]["notes"])
        said = said || n.get<std::string>().find("left as it was") != std::string::npos;
    CHECK(said);
    const double after = groundAt(mesh);
    CHECK(after == doctest::Approx(before));

    // Off the map: the commands refuse with where the map is; the spec's coast step skips them with a warning.
    std::string why;
    try
    {
        run("terrainAddReservoir", {{"args", {mesh}}, {"id", "far"}, {"a", {90000.0, 0.0}}, {"b", {90000.0, 10000.0}}});
    }
    catch (const std::exception& e)
    {
        why = e.what();
    }
    CHECK(why.find("reservoir far: dam end a") != std::string::npos);
    CHECK(why.find("outside the map") != std::string::npos);
    why.clear();
    try
    {
        run("terrainAddChannel", {{"args", {mesh}}, {"id", "out"}, {"path", {{0.0, 0.0}, {0.0, 80000.0}}}});
    }
    catch (const std::exception& e)
    {
        why = e.what();
    }
    CHECK(why.find("channel out: point 1") != std::string::npos);

    const json spec = {{"name", "OffMap"},
                       {"source", {{"type", "noise"}, {"amplitude", 3000.0}, {"featureSize", 20000.0}}},
                       {"size", {40000.0, 40000.0}},
                       {"resolution", 129},
                       {"water", {{"sea", {{"level", 0.0}}}}},
                       {"check", false},
                       {"coast",
                        {{"reservoirs", {{{"id", "far"}, {"a", {90000.0, 0.0}}, {"b", {90000.0, 10000.0}}}}},
                         {"channels", {{{"id", "in"}, {"craft", "skiff"}, {"path", {{-15000.0, 0.0}, {15000.0, 0.0}}}}}},
                         {"sceneObjects", false}}}};
    const json built = run("terrainSpec", {{"spec", spec}, {"replace", true}});
    CAPTURE(built.dump());
    bool coastOk = false, warned = false;
    for (const json& s : built["steps"])
        if (s["step"] == "coast")
        {
            coastOk = s["ok"].get<bool>();
            for (const json& w : s.value("warnings", json::array()))
                warned = warned || (w.is_string() && w.get<std::string>().find("reservoir far") != std::string::npos);
        }
    CHECK(coastOk);
    CHECK(warned);
    CHECK(run("terrainCoast", {{"args", {built["terrain"]}}})["channels"].size() == 1);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
