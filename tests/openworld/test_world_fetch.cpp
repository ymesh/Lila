// World fetch: a spec's missing real data planned and sized, consent and key refusals before any download, downloads
// written straight to the spec's own paths through a stand-in network, retries and resume, and the commands
// (worldFetch, worldBuild estimate/fetch, terrainBathymetry from a bbox with no terrain). No network anywhere.

#include "World.h"
#include "WorldFetch.h"

#include "Coast.h"
#include "Fetch.h"
#include "OsmDoc.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "extensions/ExtensionManager.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "OpenWorldTest.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace openworld::world;

namespace
{
using owtest::run;
constexpr const char* kKey = "SECRETKEY12345";

fs::path scratch(const std::string& name)
{
    const fs::path d = fs::temp_directory_path() / ("lila_world_fetch_" + name);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

/** A geographic grid over [west, south] to [east, north], height h everywhere. */
terrain::Dem grid(double west, double south, double east, double north, int n, float h)
{
    terrain::Dem d;
    d.width = d.height = n;
    d.geographic = true;
    d.dx = (east - west) / (n - 1);
    d.dy = (north - south) / (n - 1);
    d.x0 = west;
    d.y0 = north;
    d.epsg = 4326;
    d.metres.assign(size_t(n) * size_t(n), h);
    return d;
}

/** The spec: two sources in one Copernicus tile, the capital's elevation already there, the coast's GEBCO named twice. */
fs::path writeSpec(const fs::path& dir, bool bathymetry = true)
{
    json coast = {{"id", "coast"}, {"bbox", {-106.52, 23.13, -106.50, 23.15}}, {"dem", "real/coast_dem.tif"}, {"osm", "real/coast.osm.pbf"},
                  {"landcover", "real/coast_wc.tif"}, {"at", {0, 0}}};
    if (bathymetry)
        coast["bathymetry"] = "real/gebco.tif";
    json spec = {{"name", "FetchTest"},
                 {"remix", {{"sources", {coast, {{"id", "capital"}, {"bbox", {-106.40, 23.30, -106.38, 23.32}}, {"dem", "real/capital_dem.tif"}, {"at", {100000, 0}}}}}}}};
    if (bathymetry)
        spec["terrain"] = {{"coast", {{"bathymetry", {{"file", "real/gebco.tif"}}}}}};
    fs::create_directories(dir / "real");
    terrain::writeGeoTiff(dir / "real" / "capital_dem.tif", grid(-106.40, 23.30, -106.38, 23.32, 8, 50.0f));
    const fs::path f = dir / "spec.json";
    std::ofstream(f) << spec.dump(1);
    return f;
}

json loadResolved(const fs::path& file)
{
    std::ifstream in(file);
    return resolvePaths(json::parse(in), file.parent_path().generic_string());
}

const FetchItem* item(const FetchPlan& p, const std::string& id)
{
    for (const FetchItem& it : p.items)
        if (it.id == id)
            return &it;
    return nullptr;
}

/** A stand-in network: answers Copernicus, Overpass and OpenTopography with small files and counts every call. */
struct StandIn
{
    int calls = 0;
    int overpassFailures = 0;
    bool landcoverFails = false;
    std::optional<bool> metered = false;
    std::vector<std::string> urls;
    std::vector<int> waits;
    /** Overpass reports its bytes as a real transfer does: half, then all of it. */
    bool reportBytes = false;

    FetchNet net()
    {
        FetchNet n;
        n.get = [this](const std::string& url, const fs::path& dest, std::string* why) {
            ++calls;
            urls.push_back(url);
            if (url.find("copernicus") != std::string::npos)
            {
                terrain::writeGeoTiff(dest, grid(-107.0, 23.0, -106.0, 24.0, 61, 120.0f));
                return 200;
            }
            if (url.find("overpass") != std::string::npos)
            {
                if (overpassFailures-- > 0)
                {
                    *why = "connection refused";
                    return 0;
                }
                const std::string body = R"({"version": 0.6, "elements": [{"type": "node", "id": 1, "lat": 23.14, "lon": -106.51, "tags": {"place": "village", "name": "Aldea"}}]})";
                if (reportBytes)
                {
                    reportFetchBytes(body.size() / 2, body.size());
                    reportFetchBytes(body.size(), body.size());
                }
                std::ofstream(dest) << body;
                return 200;
            }
            if (url.find("opentopography") != std::string::npos)
            {
                if (url.find(kKey) == std::string::npos)
                {
                    *why = "invalid API key";
                    return 401;
                }
                terrain::writeGeoTiff(dest, grid(-106.6, 23.0, -106.3, 23.4, 40, -800.0f));
                return 200;
            }
            return 404;
        };
        n.landcover = [this](const terrain::GeoBox& b) {
            ++calls;
            if (landcoverFails)
                throw std::runtime_error("WorldCover tile read timed out");
            terrain::LandCoverFetch f;
            f.codes = grid(b.west, b.south, b.east, b.north, 10, 30.0f);
            f.tiles = 1;
            f.bytes = 1234;
            return f;
        };
        n.metered = [this] { return metered; };
        n.wait = [this](int ms) { waits.push_back(ms); };
        return n;
    }
};

std::string message(const std::function<void()>& fn)
{
    try
    {
        fn();
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
    return {};
}

} // namespace

TEST_CASE("world fetch: the plan lists each missing file with its size, skips present ones, and paths are the spec's own")
{
    const fs::path dir = scratch("plan");
    const json spec = loadResolved(writeSpec(dir));
    const FetchPlan p = planFetch(spec, dir / "cache");

    // The coast's GEBCO is named by its source and by the terrain's coast step: one item.
    CHECK(p.items.size() == 5);
    REQUIRE(item(p, "capital.dem"));
    CHECK(item(p, "capital.dem")->present);
    for (const char* id : {"coast.dem", "coast.osm", "coast.landcover", "coast.bathymetry"})
    {
        CAPTURE(id);
        REQUIRE(item(p, id));
        CHECK_FALSE(item(p, id)->present);
        CHECK(item(p, id)->bytes > 0);
        CHECK(item(p, id)->file.parent_path() == (dir / "real").lexically_normal());
    }
    CHECK(item(p, "coast.dem")->bytes == kCopernicusTileBytes);
    // GEBCO is fetched two cells wider than the source.
    CHECK(item(p, "coast.bathymetry")->box.west == doctest::Approx(-106.52 - 2.0 / terrain::kGebcoPerDegree));
    CHECK(p.missing == 4);
    CHECK(p.keyNeeded);

    FetchOptions o;
    const json j = planJson(p, o);
    CHECK(j["consentNeeded"] == true);
    CHECK(j["keyMissing"] == true);
    CHECK(j["megabytes"].get<double>() > 25.0);
    CHECK(j["summary"].get<std::string>().find("coast.dem 25.0 MB") != std::string::npos);
    CHECK(fetchSize(12000) == "12 KB");
    CHECK(fetchSize(5u << 20) == "5.0 MB");

    // A cached Copernicus tile costs nothing.
    fs::create_directories(dir / "cache" / "copernicus");
    terrain::writeGeoTiff(dir / "cache" / "copernicus" / (terrain::copernicusTileNames({-106.52, 23.13, -106.50, 23.15})[0] + ".tif"),
                          grid(-107.0, 23.0, -106.0, 24.0, 61, 120.0f));
    const FetchPlan cached = planFetch(spec, dir / "cache");
    CHECK(item(cached, "coast.dem")->cached);
    CHECK(item(cached, "coast.dem")->bytes == 0);
    CHECK(double(cached.bytes) / 1048576.0 < 25.0);

    // A coast grid of its own without a bbox takes the bathymetry source's box, else all sources' boxes together.
    json coastOnly = {{"remix", spec["remix"]}, {"terrain", {{"coast", {{"bathymetry", {{"file", (dir / "real" / "other.tif").generic_string()}}}}}}}};
    FetchPlan u = planFetch(coastOnly, dir / "cache");
    REQUIRE(item(u, "terrain.coast.bathymetry"));
    CHECK(item(u, "terrain.coast.bathymetry")->boxFrom == "bathymetry source");
    CHECK(item(u, "terrain.coast.bathymetry")->box.north < 23.2);
    coastOnly["remix"]["sources"][0].erase("bathymetry");
    u = planFetch(coastOnly, dir / "cache");
    CHECK(item(u, "terrain.coast.bathymetry")->boxFrom == "sources");
    CHECK(item(u, "terrain.coast.bathymetry")->box.north > 23.32);

    // With a map size, a source's GEBCO reaches the whole map through the source's placement and scale.
    json mapped = spec;
    mapped["remix"]["scale"] = {{"horizontal", "1:4"}, {"vertical", 1.0}};
    mapped["remix"]["map"] = {{"center", {0, 0}}, {"size", {4000000, 3000000}}};
    const FetchPlan m = planFetch(mapped, dir / "cache");
    const FetchItem* g = item(m, "coast.bathymetry");
    REQUIRE(g);
    CHECK(g->boxFrom == "the map, through the source");
    // 40 km of map at 1:4 is 160 km of the source: about 0.78 degrees either side of its centre at 23 N.
    const double centreLon = 0.5 * (-106.52 + -106.30);
    CHECK(g->box.west == doctest::Approx(centreLon - 80000.0 / (111412.84 * std::cos(23.225 * 0.017453292519943295) - 93.5 * std::cos(3 * 23.225 * 0.017453292519943295)) - 2.0 / terrain::kGebcoPerDegree).epsilon(1e-3));
    CHECK(g->box.north - g->box.south > 1.0);
    CHECK(item(m, "coast.dem")->box.west == doctest::Approx(-106.52));
    fs::remove_all(dir);
}

TEST_CASE("world fetch: over 25 MB without consent, or metered, is refused before any download")
{
    const fs::path dir = scratch("consent");
    const FetchPlan p = planFetch(loadResolved(writeSpec(dir, false)), dir / "cache");
    StandIn s;
    FetchOptions o;
    o.cache = dir / "cache";
    const std::string big = message([&] { runFetch(p, o, s.net()); });
    CHECK(big.find("MB to download") != std::string::npos);
    CHECK(big.find("consent=True") != std::string::npos);
    CHECK(big.find("coast.dem 25.0 MB") != std::string::npos);
    CHECK(s.calls == 0);

    // Under the limit, a metered connection still needs consent.
    fs::create_directories(dir / "cache" / "copernicus");
    terrain::writeGeoTiff(dir / "cache" / "copernicus" / (terrain::copernicusTileNames({-106.52, 23.13, -106.50, 23.15})[0] + ".tif"),
                          grid(-107.0, 23.0, -106.0, 24.0, 61, 120.0f));
    const FetchPlan small = planFetch(loadResolved(dir / "spec.json"), dir / "cache");
    s.metered = true;
    const std::string metered = message([&] { runFetch(small, o, s.net()); });
    CHECK(metered.find("metered") != std::string::npos);
    CHECK(s.calls == 0);
    fs::remove_all(dir);
}

TEST_CASE("world fetch: a missing OpenTopography key says how to pass and save one, and the key never shows")
{
    const fs::path dir = scratch("key");
    const FetchPlan p = planFetch(loadResolved(writeSpec(dir)), dir / "cache");
    StandIn s;
    FetchOptions o;
    o.cache = dir / "cache";
    o.consent = true;
    const std::string missing = message([&] { runFetch(p, o, s.net()); });
    CHECK(missing.find("coast.bathymetry") != std::string::npos);
    CHECK(missing.find("apiKey=") != std::string::npos);
    CHECK(missing.find("saveKey=True") != std::string::npos);
    CHECK(missing.find("opentopography.org") != std::string::npos);
    CHECK(s.calls == 0);

    // A wrong key fails at the server; the message names the item and hides the key.
    o.apiKey = "WRONGKEY99999";
    const std::string wrong = message([&] { runFetch(p, o, s.net()); });
    CHECK(wrong.find("coast.bathymetry") != std::string::npos);
    CHECK(wrong.find("401") != std::string::npos);
    CHECK(wrong.find("WRONGKEY99999") == std::string::npos);
    CHECK(wrong.find("fetched before it and kept: coast.dem, coast.osm, coast.landcover") != std::string::npos);

    // Commands never echo it either.
    CHECK(lila::ops::toPython("worldFetch", {{"file", "w.json"}, {"apiKey", kKey}}).find(kKey) == std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("world fetch: downloads go straight to the spec's paths, retry, resume after a failure, and carry the credits")
{
    const fs::path dir = scratch("run");
    const fs::path specFile = writeSpec(dir);
    StandIn s;
    FetchOptions o;
    o.cache = dir / "cache";
    o.consent = true;
    o.apiKey = kKey;

    // Land cover fails every attempt: the run stops there, naming it; what came before stays.
    s.landcoverFails = true;
    const std::string failed = message([&] { runFetch(planFetch(loadResolved(specFile), o.cache), o, s.net()); });
    CHECK(failed.find("coast.landcover") != std::string::npos);
    CHECK(failed.find("timed out") != std::string::npos);
    CHECK(failed.find("after 3 attempts") != std::string::npos);
    CHECK(failed.find("run again to resume") != std::string::npos);
    CHECK(fs::exists(dir / "real" / "coast_dem.tif"));
    CHECK(fs::exists(dir / "real" / "coast.osm.pbf"));
    CHECK_FALSE(fs::exists(dir / "real" / "coast_wc.tif"));
    CHECK_FALSE(fs::exists(dir / "real" / "coast_wc.tif.part"));

    // Run again: present files are not fetched again; Overpass is unreachable once and retried.
    s.landcoverFails = false;
    s.calls = 0;
    s.urls.clear();
    s.waits.clear();
    fs::remove(dir / "real" / "coast.osm.pbf");
    fs::remove_all(dir / "cache" / "overpass");
    s.overpassFailures = 1;
    const FetchPlan p = planFetch(loadResolved(specFile), o.cache);
    CHECK(item(p, "coast.dem")->present);
    const json r = runFetch(p, o, s.net());
    CHECK(r["fetched"] == 3);
    CHECK(r["present"] == 2);
    for (const std::string& u : s.urls)
        CHECK(u.find("copernicus") == std::string::npos);
    std::map<std::string, json> byId;
    for (const json& it : r["items"])
        byId[it["id"].get<std::string>()] = it;
    CHECK(byId["coast.osm"]["state"] == "fetched");
    CHECK(byId["coast.osm"]["attempts"] == 2);
    CHECK(s.waits.size() == 1);
    CHECK(byId["coast.dem"]["state"] == "present");
    CHECK(byId["capital.dem"]["state"] == "present");
    CHECK(byId["coast.bathymetry"]["state"] == "fetched");
    CHECK(r.dump().find(kKey) == std::string::npos);

    // Written where the spec says, and readable: Overpass JSON under the .pbf name reads by content.
    const openworld::OsmDoc osm = openworld::readOsmDoc(dir / "real" / "coast.osm.pbf");
    CHECK(osm.nodes.size() == 1);
    const terrain::Dem g = terrain::readDem(dir / "real" / "gebco.tif");
    CHECK(g.geographic);
    CHECK(g.metres[0] == doctest::Approx(-800.0f));
    CHECK(terrain::readDem(dir / "real" / "coast_wc.tif").width == 10);
    std::string text;
    {
        std::ifstream credits(dir / "real" / "ATTRIBUTION.txt");
        text.assign(std::istreambuf_iterator<char>(credits), {});
    }
    for (const char* who : {"OpenStreetMap", "GEBCO", "WorldCover", "Copernicus"})
        CHECK(text.find(who) != std::string::npos);

    // Everything there: nothing more to do and nothing downloaded.
    s.calls = 0;
    const FetchPlan done = planFetch(loadResolved(specFile), o.cache);
    CHECK(done.missing == 0);
    CHECK(runFetch(done, o, s.net())["fetched"] == 0);
    CHECK(s.calls == 0);
    fs::remove_all(dir);
}

TEST_CASE("world fetch: commands estimate and refuse without a terrain or the network; GEBCO from a bbox alone")
{
    owtest::loadPackages(true, true);
    run("newScene");
    const fs::path dir = scratch("commands");
    const fs::path withBathy = writeSpec(dir);
    const fs::path small = writeSpec(dir / "nobathy", false);
    const std::string file = withBathy.generic_string();

    // Estimate only: sizes per item and total, nothing built, no terrain needed.
    const json e = run("worldBuild", {{"file", file}, {"estimate", true}})["estimate"];
    CHECK(e["missing"] == 4);
    CHECK(e["consentNeeded"] == true);
    CHECK(e["keyMissing"] == true);
    CHECK(run("worldFetch", {{"file", file}, {"estimate", true}})["estimate"]["summary"] == e["summary"]);
    for (const json& it : e["items"])
        if (it["state"] == "missing")
            CHECK(fs::path(it["file"].get<std::string>()).parent_path() == (dir / "real").lexically_normal());

    // Missing data without fetch stops before the build, naming the file and the way out.
    const std::string noFetch = message([&] { run("worldBuild", {{"file", file}}); });
    CHECK(noFetch.find("coast.dem") != std::string::npos);
    CHECK(noFetch.find("fetch=True") != std::string::npos);
    // GEBCO without a key, then over 25 MB without consent: refused before any download.
    CHECK(message([&] { run("worldBuild", {{"file", file}, {"fetch", true}, {"consent", true}}); }).find("saveKey=True") != std::string::npos);
    const std::string big = message([&] { run("worldBuild", {{"file", small.generic_string()}, {"fetch", true}}); });
    CHECK(big.find("consent=True") != std::string::npos);
    CHECK(big.find("MB") != std::string::npos);
    // Offline with nothing cached names the item.
    CHECK(message([&] { run("worldFetch", {{"file", small.generic_string()}, {"offline", true}}); }).find("offline") != std::string::npos);

    // terrainBathymetry from a bbox with no terrain in the scene.
    const json box = {-106.53, 23.12, -106.49, 23.16};
    const json est = run("terrainBathymetry", {{"bbox", box}, {"estimate", true}});
    CHECK(est["estimate"]["megabytes"].get<double>() > 0.0);
    CHECK(est["consentNeeded"] == false);
    CHECK(message([&] { run("terrainBathymetry", {{"bbox", box}, {"fetch", true}}); }).find("saveKey=True") != std::string::npos);
    // A grid fetched before (in the download cache) writes to file= offline, without a terrain or a key.
    terrain::FetchRequest r;
    r.source = terrain::DemSource::OpenTopography;
    r.box = {-106.53, 23.12, -106.49, 23.16};
    r.dataset = "GEBCOIceTopo";
    r.cache = fs::temp_directory_path() / "lila_terrain" / "dem_cache";
    const fs::path cacheFile = terrain::openTopographyCacheFile(r);
    fs::create_directories(cacheFile.parent_path());
    terrain::writeGeoTiff(cacheFile, grid(-106.6, 23.0, -106.3, 23.4, 40, -500.0f));
    const fs::path out = dir / "real" / "from_bbox.tif";
    const json f = run("terrainBathymetry", {{"bbox", box}, {"fetch", true}, {"offline", true}, {"file", out.generic_string()}});
    CHECK(f["applied"] == false);
    CHECK(fs::exists(out));
    CHECK(terrain::readDem(out).metres[0] == doctest::Approx(-500.0f));
    std::error_code ec;
    fs::remove(cacheFile, ec);
    fs::remove_all(dir, ec);
}

namespace
{
using FetchKind = FetchEvent::Kind;

std::vector<FetchEvent> eventsFor(const std::vector<FetchEvent>& all, const std::string& id)
{
    std::vector<FetchEvent> out;
    for (const FetchEvent& e : all)
        if (e.id == id)
            out.push_back(e);
    return out;
}

const std::string* lineStarting(const std::vector<std::string>& lines, const std::string& start)
{
    for (const std::string& l : lines)
        if (l.starts_with(start))
            return &l;
    return nullptr;
}

uint64_t logSeq()
{
    uint64_t seq = 0;
    for (const lila::LogEntry& e : lila::log::since(0, 100000))
        seq = std::max(seq, e.seq);
    return seq;
}

/** Script Output lines logged after seq. */
std::vector<std::string> loggedSince(uint64_t seq)
{
    std::vector<std::string> out;
    for (const lila::LogEntry& e : lila::log::since(seq, 5000))
        out.push_back(e.text);
    return out;
}
} // namespace

TEST_CASE("world fetch: progress events per item in order, with byte counts, retries, and a stop that keeps what came before")
{
    const fs::path dir = scratch("events");
    const fs::path specFile = writeSpec(dir);
    StandIn s;
    s.reportBytes = true;
    s.overpassFailures = 1;
    FetchOptions o;
    o.cache = dir / "cache";
    o.consent = true;
    o.apiKey = kKey;
    std::vector<FetchEvent> events;
    o.onEvent = [&](const FetchEvent& e) { events.push_back(e); };
    const FetchPlan p = planFetch(loadResolved(specFile), o.cache);
    const json r = runFetch(p, o, s.net());
    CHECK(r["fetched"] == 4);
    std::map<std::string, uint64_t> written;
    for (const json& it : r["items"])
        written[it["id"].get<std::string>()] = it.value("bytes", uint64_t(0));

    // Items run in plan order, numbered among the four missing; each starts, then ends with Done and the bytes written.
    std::vector<std::string> started;
    for (const FetchEvent& e : events)
        if (e.kind == FetchKind::Start)
            started.push_back(e.id);
    CHECK(started == std::vector<std::string>{"coast.dem", "coast.osm", "coast.landcover", "coast.bathymetry"});
    for (const std::string& id : started)
    {
        CAPTURE(id);
        const std::vector<FetchEvent> ev = eventsFor(events, id);
        REQUIRE(ev.size() >= 2);
        CHECK(ev.front().kind == FetchKind::Start);
        CHECK(ev.back().kind == FetchKind::Done);
        CHECK(ev.front().count == 4);
        CHECK(ev.back().bytes == written[id]);
        CHECK(ev.back().bytes > 0);
        uint64_t last = 0;
        for (const FetchEvent& e : ev)
            if (e.kind == FetchKind::Bytes)
            {
                CHECK(e.bytes >= last);
                last = e.bytes;
            }
    }
    CHECK(eventsFor(events, "coast.osm").front().index == 2);

    // Overpass: unreachable once (a retry naming why and the pause), then its bytes as they arrived, then done.
    const std::vector<FetchEvent> osm = eventsFor(events, "coast.osm");
    std::vector<FetchKind> kinds;
    for (const FetchEvent& e : osm)
        kinds.push_back(e.kind);
    REQUIRE(kinds == std::vector<FetchKind>{FetchKind::Start, FetchKind::Retry, FetchKind::Bytes, FetchKind::Bytes, FetchKind::Bytes, FetchKind::Done});
    CHECK(osm[1].attempt == 1);
    CHECK(osm[1].waitMs == 2000);
    CHECK(osm[1].why == "connection refused");
    const uint64_t body = fs::file_size(dir / "real" / "coast.osm.pbf");
    CHECK(osm[2].bytes == body / 2);
    CHECK(osm[2].expected == body);
    CHECK(osm[2].exact);
    CHECK(osm[3].bytes == body);
    CHECK(osm[4].bytes == body);
    CHECK(osm[5].bytes == body);
    CHECK(osm[5].attempt == 2);
    // Land cover reports the bytes its reads brought.
    bool landcoverBytes = false;
    for (const FetchEvent& e : eventsFor(events, "coast.landcover"))
        landcoverBytes |= e.kind == FetchKind::Bytes && e.bytes == 1234;
    CHECK(landcoverBytes);
    fs::remove_all(dir);

    // Stop asked while Overpass sends: it fails as stopped, nothing of it is written, the elevation before it stays.
    const fs::path dir2 = scratch("events_stop");
    const fs::path spec2 = writeSpec(dir2);
    StandIn s2;
    s2.reportBytes = true;
    events.clear();
    bool stop = false;
    o.cache = dir2 / "cache";
    o.onEvent = [&](const FetchEvent& e) {
        events.push_back(e);
        if (e.kind == FetchKind::Bytes && e.id == "coast.osm")
            stop = true;
    };
    o.cancelled = [&] { return stop; };
    const std::string stopped = message([&] { runFetch(planFetch(loadResolved(spec2), o.cache), o, s2.net()); });
    CHECK(stopped.find("coast.osm") != std::string::npos);
    CHECK(stopped.find("stopped") != std::string::npos);
    CHECK(stopped.find("fetched before it and kept: coast.dem") != std::string::npos);
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().kind == FetchKind::Failed);
    CHECK(events.back().id == "coast.osm");
    CHECK(events.back().why == "stopped");
    CHECK(fs::exists(dir2 / "real" / "coast_dem.tif"));
    CHECK_FALSE(fs::exists(dir2 / "real" / "coast.osm.pbf"));
    fs::remove_all(dir2);
}

TEST_CASE("world fetch: the estimate and the run's outcome print as tables")
{
    const fs::path dir = scratch("table");
    const fs::path specFile = writeSpec(dir);
    FetchOptions o;
    o.cache = dir / "cache";
    const FetchPlan p = planFetch(loadResolved(specFile), o.cache);

    const std::vector<std::string> plan = fetchTable(p, o);
    REQUIRE(plan.size() >= 8);
    CHECK(plan[0].starts_with("item"));
    const std::string* dem = lineStarting(plan, "coast.dem");
    REQUIRE(dem);
    CHECK(dem->find("25.0 MB") != std::string::npos);
    CHECK(dem->ends_with("to download"));
    // Sizes right-aligned under "size", statuses left-aligned under "status".
    CHECK(dem->find("25.0 MB") + 7 == plan[0].find("size") + 4);
    CHECK(dem->find("to download") == plan[0].find("status"));
    const std::string* capital = lineStarting(plan, "capital.dem");
    REQUIRE(capital);
    CHECK(capital->ends_with("present"));
    const std::string* total = lineStarting(plan, "total");
    REQUIRE(total);
    CHECK(total->find("about ") != std::string::npos);
    CHECK(total->ends_with("4 to download, 1 present"));
    CHECK(lineStarting(plan, "over 25 MB: needs consent=True"));
    REQUIRE(lineStarting(plan, "GEBCO needs an OpenTopography key"));
    CHECK(lineStarting(plan, "GEBCO needs an OpenTopography key")->find("saveKey=True") != std::string::npos);

    // After a run: what was written, present ones as they were, and the total of what came down.
    StandIn s;
    o.consent = true;
    o.apiKey = kKey;
    const json r = runFetch(p, o, s.net());
    const std::vector<std::string> done = fetchTable(p, o, &r["items"]);
    REQUIRE(lineStarting(done, "coast.osm"));
    CHECK(lineStarting(done, "coast.osm")->ends_with("fetched"));
    CHECK(lineStarting(done, "capital.dem")->ends_with("present"));
    CHECK(lineStarting(done, "total")->ends_with("4 fetched, 1 present"));
    CHECK(lineStarting(done, "total")->find(fetchSize(r["bytes"].get<uint64_t>())) != std::string::npos);
    CHECK_FALSE(lineStarting(done, "over 25 MB"));

    // A run stopped at the OSM: what came before fetched, the OSM failed, the rest still to download.
    const json partial = json::array({{{"id", "coast.dem"}, {"state", "fetched"}, {"bytes", 2048}}, {{"id", "coast.osm"}, {"state", "failed"}}});
    const std::vector<std::string> failed = fetchTable(p, o, &partial);
    CHECK(lineStarting(failed, "coast.dem")->ends_with("fetched"));
    CHECK(lineStarting(failed, "coast.osm")->ends_with("failed"));
    CHECK(lineStarting(failed, "coast.landcover")->ends_with("to download"));
    CHECK(lineStarting(failed, "total")->ends_with("1 fetched, 1 failed, 2 to download, 1 present"));
    fs::remove_all(dir);

    // The commands log the table, and tell Python to show nothing more than it at the prompt.
    owtest::loadPackages(true, true);
    const fs::path dir2 = scratch("table_cmd");
    const fs::path spec2 = writeSpec(dir2);
    const uint64_t seq = logSeq();
    const json e = run("worldFetch", {{"file", spec2.generic_string()}, {"estimate", true}});
    CHECK(e["_echo"] == "");
    bool header = false, row = false;
    for (const std::string& l : loggedSince(seq))
    {
        header |= l.starts_with("  item") && l.find("status") != std::string::npos;
        row |= l.starts_with("  capital.dem") && l.ends_with("present");
    }
    CHECK(header);
    CHECK(row);
    fs::remove_all(dir2);
}

TEST_CASE("world fetch: a placeholder API key is refused and never saved; a saved placeholder counts as no key")
{
    CHECK_FALSE(terrain::apiKeyProblem("").empty());
    CHECK_FALSE(terrain::apiKeyProblem("<your OpenTopography key>").empty());
    CHECK_FALSE(terrain::apiKeyProblem("YOUR_API_KEY").empty());
    CHECK_FALSE(terrain::apiKeyProblem("...").empty());
    CHECK(terrain::apiKeyProblem("0123456789abcdef0123456789abcdef").empty());

    owtest::loadPackages(true, true);
    run("newScene");
    lila::PrefStore& prefs = lila::PrefStore::get();
    prefs.remove("terrain", "/openTopography/apiKey");
    const fs::path dir = scratch("placeholder");
    const std::string file = writeSpec(dir).generic_string();
    const std::string placeholder = "<your OpenTopography key>";

    // Passed: refused before anything downloads, and not saved.
    const std::string passed = message([&] { run("worldFetch", {{"file", file}, {"consent", true}, {"apiKey", placeholder}, {"saveKey", true}}); });
    CHECK(passed.find("placeholder") != std::string::npos);
    CHECK(passed.find("nothing was downloaded or saved") != std::string::npos);
    CHECK(prefs.value("terrain", "/openTopography/apiKey").is_null());
    CHECK(message([&] { run("worldBuild", {{"file", file}, {"fetch", true}, {"consent", true}, {"apiKey", "..."}}); }).find("placeholder") != std::string::npos);
    const json box = {-106.53, 23.12, -106.49, 23.16};
    CHECK(message([&] { run("terrainBathymetry", {{"bbox", box}, {"fetch", true}, {"apiKey", placeholder}, {"saveKey", true}}); }).find("placeholder") != std::string::npos);
    CHECK(prefs.value("terrain", "/openTopography/apiKey").is_null());

    // Saved earlier: ignored as no key, and the messages say why.
    prefs.set("terrain", "/openTopography/apiKey", placeholder);
    const json e = run("worldFetch", {{"file", file}, {"estimate", true}})["estimate"];
    CHECK(e["keyMissing"] == true);
    CHECK(e.value("keyNote", std::string()).find("saved OpenTopography key is a placeholder") != std::string::npos);
    const std::string saved = message([&] { run("worldFetch", {{"file", file}, {"consent", true}}); });
    CHECK(saved.find("coast.bathymetry") != std::string::npos);
    CHECK(saved.find("saved OpenTopography key is a placeholder") != std::string::npos);
    CHECK(message([&] { run("terrainBathymetry", {{"bbox", box}, {"fetch", true}}); }).find("saved OpenTopography key is a placeholder") != std::string::npos);
    prefs.remove("terrain", "/openTopography/apiKey");
    fs::remove_all(dir);
}
