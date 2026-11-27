// World Remix core: identity at scale 1, rigid regions, fold-free warp, heights and features through one map, road
// reconnection, river drainage, and remixCheck catching each failure. Synthetic fixtures only.

#include "Remix.h"

#include "Fetch.h"
#include "OpenWorldTest.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

using namespace openworld;
using json = nlohmann::json;

namespace
{
using owtest::data;
json coast() { return {{"id", "coast"}, {"dem", data("coast.asc")}, {"osm", data("coast.json")}}; }
json inland(Vec2 at) { return {{"id", "inland"}, {"dem", data("inland.asc")}, {"osm", data("inland.json")}, {"at", {at.x, at.y}}}; }

json base(double h = 1.0 / 3.0, double v = 1.0 / 1.5)
{
    return {{"scale", {{"horizontal", h}, {"vertical", v}}}, {"sources", json::array({coast()})}, {"seams", {{"erosion", 0}, {"detail", 0.0}}}};
}

json box(double x0, double z0, double x1, double z1) { return json::array({{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}}); }

const OsmDoc::Way* wayNamed(const OsmDoc& d, std::string_view name)
{
    for (const OsmDoc::Way& w : d.ways)
        if (tagOf(w.tags, "name") == name)
            return &w;
    return nullptr;
}

int countKind(const json& check, const char* kind) { return check["counts"][kind].get<int>(); }
} // namespace

TEST_CASE("remix spec: JSON round trip, scales and errors")
{
    json j = base();
    j["regions"] = json::array({{{"id", "a"}, {"op", "cut"}, {"polygon", box(0, 0, 100, 100)}, {"translate", {50, 0}}, {"angle", 30}}});
    j["pins"] = json::array({{{"id", "p"}, {"at", {1, 2}}}});
    const RemixSpec s = specFromJson(j);
    const RemixSpec t = specFromJson(toJson(s));
    CHECK(toJson(s) == toJson(t));
    CHECK(t.pins[0].to == Vec2(1, 2));
    CHECK(parseScale("1:3", "x") == doctest::Approx(1.0 / 3.0));
    CHECK(parseScale(0.5, "x") == 0.5);
    CHECK_THROWS(parseScale("abc", "x"));
    json bad = base();
    bad["nonsense"] = 1;
    CHECK_THROWS(specFromJson(bad));
    CHECK(remixSpecSchema()["$id"] == "lila://schema/remix-spec");
    const RemixSpec d = specFromJson(json::object());
    CHECK(d.horizontal == doctest::Approx(1.0 / 3.0));
    CHECK(d.vertical == doctest::Approx(1.0 / 1.5));
}

TEST_CASE("OSM doc: reads Overpass JSON and XML whole, writes them back")
{
    const OsmDoc d = readOsmDoc(data("coast.json"));
    CHECK(d.ways.size() == 5);
    size_t tagged = 0;
    for (const OsmDoc::Node& n : d.nodes)
        tagged += !n.tags.empty();
    CHECK(tagged == 1);
    const OsmDoc back = parseOsmDocJson(writeOsmDocJson(d));
    CHECK(back.nodes.size() == d.nodes.size());
    CHECK(back.ways.size() == d.ways.size());
    CHECK(wayKind(*wayNamed(back, "Ridge Road")) == WayKind::Road);
    CHECK(wayKind(*wayNamed(back, "Rio Uno")) == WayKind::River);

    const OsmDoc x = parseOsmDocXml(R"(<osm><node id="1" lat="1" lon="2"><tag k="shop" v="bakery"/></node><node id="2" lat="1.1" lon="2"/>
        <way id="5"><nd ref="1"/><nd ref="2"/><tag k="highway" v="residential"/></way>
        <relation id="9"><member type="way" ref="5" role="outer"/><tag k="type" v="route"/></relation></osm>)");
    REQUIRE(x.nodes.size() == 2);
    CHECK(tagOf(x.nodes[0].tags, "shop") == "bakery");
    CHECK(x.ways[0].nodes.size() == 2);
    CHECK(x.relations[0].members[0].ref == 5);
}

TEST_CASE("remix: identity at scale 1 with no regions")
{
    const RemixSpec s = specFromJson(base(1.0, 1.0));
    const std::shared_ptr<RemixResult> r = runRemix(s);
    CHECK(r->warp->identity());
    CHECK(r->errors.empty());
    const std::shared_ptr<const SourceData> src = loadSource(s.sources[0]);
    const Heightfield& h = src->hf;
    // Away from the feathered edge the heights are the source's.
    int checked = 0;
    for (int z = h.nz / 6; z < h.nz - h.nz / 6; z += 5)
        for (int x = h.nx / 6; x < h.nx - h.nx / 6; x += 5)
        {
            const Vec2 p = h.samplePos(x, z);
            CHECK(r->after.heightAt(p.x, p.y) == doctest::Approx(h.at(x, z)).epsilon(1e-4));
            ++checked;
        }
    CHECK(checked > 100);
    // Every original node comes back at its own latitude and longitude.
    const OsmDoc& in = src->osm;
    size_t same = 0;
    for (const OsmDoc::Node& n : in.nodes)
        if (const OsmDoc::Node* o = r->osm.node(n.id))
        {
            CHECK(o->at.lat == doctest::Approx(n.at.lat).epsilon(1e-9));
            CHECK(o->at.lon == doctest::Approx(n.at.lon).epsilon(1e-9));
            ++same;
        }
    CHECK(same == in.nodes.size());
    CHECK(r->roadEnds.empty());
    const json c = checkRemix(s, *r);
    CHECK(c["ok"].get<bool>());
}

TEST_CASE("remix: move and keep regions stay rigid, the warp never folds")
{
    json j = base();
    j["regions"] = json::array({{{"id", "city"}, {"op", "move"}, {"polygon", box(20000, -20000, 50000, 10000)}, {"translate", {-30000, 15000}}, {"angle", 20}},
                                {{"id", "port"}, {"op", "keep"}, {"polygon", box(-30000, -40000, -10000, -20000)}}});
    j["pins"] = json::array({{{"id", "p1"}, {"at", {80000, 40000}}, {"to", {70000, 30000}}}, {{"id", "hold"}, {"at", {-35000, 40000}}}});
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    const Warp& w = *r->warp;
    CHECK_FALSE(w.identity());
    const Rigid city = s.regions[0].rigid();
    std::vector<Vec2> inside;
    for (double x = 21000; x < 50000; x += 3700)
        for (double z = -19000; z < 10000; z += 3300)
            inside.push_back({x, z});
    for (const Vec2& p : inside)
        CHECK(glm::length(w.forward(p) - city.apply(p)) < 0.01);
    for (size_t i = 0; i + 1 < inside.size(); ++i)
        CHECK(glm::length(w.forward(inside[i]) - w.forward(inside[i + 1])) == doctest::Approx(glm::length(inside[i] - inside[i + 1])).epsilon(1e-7));
    for (double x = -29000; x < -10000; x += 4000)
        CHECK(glm::length(w.forward({x, -30000}) - Vec2(x, -30000)) < 0.01);
    CHECK(glm::length(w.forward({80000, 40000}) - Vec2(70000, 30000)) < 1.0);
    // The inverse undoes the forward.
    for (const Vec2& p : {Vec2(0, 0), Vec2(60000, -30000), Vec2(-20000, 30000)})
        CHECK(glm::length(w.inverse(w.forward(p)) - p) < 0.5);
    const Warp::Folds f = w.folds(r->lo, r->hi, 96);
    CHECK(f.folded == 0);
    CHECK(f.minJacobian > 0.0);
    CHECK(w.minStepJacobian() > 0.3);
    CHECK(checkRemix(s, *r)["counts"]["fold"] == 0);
}

TEST_CASE("remix: a squeeze between a moved region and a held one stays fold-free")
{
    json j = base();
    j["regions"] = json::array({{{"id", "a"}, {"op", "move"}, {"polygon", box(30000, -10000, 50000, 10000)}, {"translate", {-55000, 0}}},
                                {{"id", "b"}, {"op", "keep"}, {"polygon", box(-40000, -10000, -30000, 10000)}}});
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    const Warp::Folds f = r->warp->folds(r->lo, r->hi, 128);
    CHECK(f.folded == 0);
    CHECK(r->warp->steps() >= 4);
}

TEST_CASE("remix: heights and features go through the same map (a road on a ridge stays on it)")
{
    json j = base();
    j["regions"] = json::array({{{"id", "east"}, {"op", "move"}, {"polygon", box(45000, -30000, 100000, 30000)}, {"translate", {-20000, 25000}}}});
    j["pins"] = json::array({{{"id", "p"}, {"at", {0, 60000}}, {"to", {5000, 50000}}}});
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    const OsmDoc::Way* road = wayNamed(r->osm, "Ridge Road");
    REQUIRE(road);
    const Heightfield& h = r->after;
    int onRidge = 0, total = 0;
    for (int64_t id : road->nodes)
    {
        const OsmDoc::Node* n = r->osm.node(id);
        const Vec2 p = r->geo.toLocal(n->at);
        // The ridge is strong only inland; near the coast it fades to nothing.
        if (p.x < 0.0)
            continue;
        const double d = 4.0 * h.cell;
        const double here = h.heightAt(p.x, p.y);
        ++total;
        if (here >= h.heightAt(p.x, p.y + d) && here >= h.heightAt(p.x, p.y - d))
            ++onRidge;
    }
    CHECK(total > 15);
    CHECK(onRidge >= total * 9 / 10);
    // Land cover masks went through the same map: the town stays under the cafe.
    for (const OsmDoc::Node& n : r->osm.nodes)
        if (tagOf(n.tags, "amenity") == "cafe")
        {
            const Vec2 p = r->geo.toLocal(n.at);
            CHECK(h.maskAt("landcover_urban", p.x, p.y) > 0.5);
        }
}

TEST_CASE("remix: cut roads are reconnected by the terrain router, keeping their class")
{
    json j = base();
    j["regions"] = json::array({{{"id", "mid"}, {"op", "cut"}, {"polygon", box(25000, -12000, 60000, 12000)}, {"translate", {10000, 40000}}}});
    j["seams"]["width"] = 1200;
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    REQUIRE(r->roadEnds.size() >= 4);
    for (const CutEnd& e : r->roadEnds)
        CHECK(e.joined);
    int reconnects = 0;
    for (const OsmDoc::Way& w : r->osm.ways)
        if (tagOf(w.tags, "remix") == "reconnect")
        {
            ++reconnects;
            const std::string_view cls = tagOf(w.tags, "highway");
            CHECK((cls == "primary" || cls == "secondary"));
            // Joined at the cut ends: the reconnection shares its end nodes with the cut roads.
            int shared = 0;
            for (const OsmDoc::Way& o : r->osm.ways)
                if (&o != &w && tagOf(o.tags, "remix").empty())
                    for (int64_t id : o.nodes)
                        shared += id == w.nodes.front() || id == w.nodes.back();
            CHECK(shared >= 2);
        }
    CHECK(reconnects >= 2);
    CHECK(r->stats["holeSamples"].get<size_t>() > 0);
    const json c = checkRemix(s, *r);
    CHECK(countKind(c, "danglingRoad") == 0);

    // With reconnection off, the check catches every dangling end.
    j["seams"]["roads"] = false;
    const RemixSpec off = specFromJson(j);
    const json c2 = checkRemix(off, *runRemix(off));
    CHECK(countKind(c2, "danglingRoad") >= 4);
    CHECK_FALSE(c2["ok"].get<bool>());
}

TEST_CASE("remix: rivers are re-solved to drain past a pasted dam; the check catches it when off")
{
    json j = base();
    // A copy of the ridge dropped across the river valley.
    j["regions"] = json::array({{{"id", "dam"}, {"op", "duplicate"}, {"polygon", box(28000, -9000, 44000, 9000)}, {"translate", {0, -55400}}}});
    j["seams"]["width"] = 800;
    j["seams"]["roads"] = false;
    j["seams"]["rivers"] = false;
    const RemixSpec off = specFromJson(j);
    const std::shared_ptr<RemixResult> broken = runRemix(off);
    const json c = checkRemix(off, *broken);
    CHECK(countKind(c, "riverNoDrain") >= 1);

    j["seams"]["rivers"] = true;
    const RemixSpec on = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(on);
    REQUIRE_FALSE(r->rivers.empty());
    double extended = 0.0;
    for (const RiverState& s : r->rivers)
    {
        if (s.drainedBefore)
            CHECK(s.drains);
        extended += s.extended;
    }
    CHECK(extended > 0.0);
    CHECK(countKind(checkRemix(on, *r), "riverNoDrain") == 0);
}

TEST_CASE("remix: the check catches overlapping regions, folds and unreadable sources")
{
    json j = base();
    j["regions"] = json::array({{{"id", "a"}, {"op", "cut"}, {"polygon", box(0, 0, 10000, 10000)}, {"translate", {30000, 0}}},
                                {{"id", "b"}, {"op", "cut"}, {"polygon", box(0, 20000, 10000, 30000)}, {"translate", {32000, -18000}}}});
    RemixSpec s = specFromJson(j);
    CHECK(countKind(checkRemix(s, *runRemix(s)), "overlap") == 1);

    json f = base();
    f["pins"] = json::array({{{"id", "a"}, {"at", {0, 0}}, {"to", {30000, 0}}}, {{"id", "b"}, {"at", {30000, 0}}, {"to", {0, 0}}}});
    f["warp"] = {{"falloff", 40000}, {"maxSteps", 2}};
    s = specFromJson(f);
    const std::shared_ptr<RemixResult> folded = runRemix(s);
    CHECK(countKind(checkRemix(s, *folded), "fold") > 0);

    json m = base();
    m["sources"].push_back({{"id", "gone"}, {"dem", data("missing.asc")}, {"at", {500000, 0}}});
    s = specFromJson(m);
    const std::shared_ptr<RemixResult> partial = runRemix(s);
    CHECK(partial->errors.size() == 1);
    CHECK(countKind(checkRemix(s, *partial), "source") == 1);
}

TEST_CASE("remix: two sources composed onto one map with a region moved between them")
{
    json j = base();
    j["sources"].push_back(inland({250000, -60000}));
    // The inland town lifted out and placed beside the coast.
    j["regions"] = json::array({{{"id", "town"}, {"op", "cut"}, {"polygon", box(230000, -80000, 270000, -40000)}, {"translate", {-140000, 60000}}}});
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    CHECK(r->errors.empty());
    const OsmDoc::Way* hw = wayNamed(r->osm, "Highway");
    REQUIRE(hw);
    bool townhall = false;
    for (const OsmDoc::Node& n : r->osm.nodes)
        if (tagOf(n.tags, "amenity") == "townhall")
        {
            townhall = true;
            const Vec2 p = r->geo.toLocal(n.at);
            CHECK(glm::length(p - Vec2(250000 - 140000, -60000 + 60000)) < 5000.0);
        }
    CHECK(townhall);
    for (const CutEnd& e : r->roadEnds)
        CHECK(e.joined);
    CHECK(r->after.nx > 100);
}

TEST_CASE("remix: cut coastline is re-merged along sea level")
{
    json j = base();
    // A stretch of coast lifted and slid north along the shore.
    j["regions"] = json::array({{{"id", "bay"}, {"op", "cut"}, {"polygon", box(-60000, 20000, -25000, 50000)}, {"translate", {0, -60000}}}});
    j["seams"]["width"] = 1000;
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    REQUIRE(r->coastEnds.size() >= 2);
    size_t joined = 0;
    for (const CutEnd& e : r->coastEnds)
        joined += e.joined;
    CHECK(joined >= 2);
    int merged = 0;
    for (const OsmDoc::Way& w : r->osm.ways)
        if (tagOf(w.tags, "remix") == "coast")
        {
            ++merged;
            CHECK(tagOf(w.tags, "natural") == "coastline");
            // The merged stretch runs along sea level.
            for (int64_t id : w.nodes)
            {
                const Vec2 p = r->geo.toLocal(r->osm.node(id)->at);
                CHECK(std::abs(r->after.heightAt(p.x, p.y)) < 800.0);
            }
        }
    CHECK(merged >= 1);
    const json c = checkRemix(s, *r);
    CHECK(countKind(c, "coastGap") == int(r->coastEnds.size() - joined));
}

TEST_CASE("remix: a road crossing a delete strip narrower than the probe step is still cut")
{
    json j = base();
    // 4 m wide, far under half the 30 m seam the probe steps by.
    j["regions"] = json::array({{{"id", "strip"}, {"op", "delete"}, {"polygon", box(39800, -200000, 40200, 200000)}}});
    j["seams"]["width"] = 3000;
    j["seams"]["roads"] = false;
    const RemixSpec s = specFromJson(j);
    const std::shared_ptr<RemixResult> r = runRemix(s);
    CHECK(r->roadEnds.size() >= 2);
    CHECK(countKind(checkRemix(s, *r), "danglingRoad") >= 2);
}
TEST_CASE("remix: a pasted region is levelled to the ground round it, its seam widened to a gentle slope")
{
    auto steepestAcrossEdge = [](const RemixResult& r) {
        // The target's west edge at x = 90000, crossed by rows from 2 km outside to 2 km inside.
        double worst = 0.0;
        for (double z = -15000.0; z <= 15000.0; z += 5000.0)
            for (double x = 88000.0; x < 92000.0; x += r.after.cell)
                worst = std::max(worst, std::abs(r.after.heightAt(x + r.after.cell, z) - r.after.heightAt(x, z)) / r.after.cell);
        return worst;
    };
    json j = base();
    j["sources"].push_back(inland({250000, -60000}));
    j["regions"] = json::array({{{"id", "town"}, {"op", "cut"}, {"polygon", box(230000, -80000, 270000, -40000)}, {"translate", {-140000, 60000}}}});
    const std::shared_ptr<RemixResult> matched = runRemix(specFromJson(j));
    REQUIRE(matched->stats.contains("regionLevels"));
    const json lv = matched->stats["regionLevels"][0];
    CHECK(lv["region"] == "town");
    CHECK(lv["seam"].get<double>() >= specFromJson(j).seams.width);
    // The step left fits the seam at the gentle grade, unless the seam reached its cap of a quarter of the region.
    CHECK((lv["stepLeft"].get<double>() <= lv["seam"].get<double>() * kSeamGrade + 1.0 || lv["seam"].get<double>() >= 0.25 * 40000.0 - 1.0));
    j["regions"][0]["matchHeight"] = false;
    const std::shared_ptr<RemixResult> raw = runRemix(specFromJson(j));
    CHECK_FALSE(raw->stats.contains("regionLevels"));
    MESSAGE("steepest across the edge: matched " << steepestAcrossEdge(*matched) << ", raw " << steepestAcrossEdge(*raw) << ", offset " << lv["offset"]);
    CHECK(steepestAcrossEdge(*matched) < steepestAcrossEdge(*raw));
    CHECK(toJson(specFromJson(j))["regions"][0]["matchHeight"] == false);
}

TEST_CASE("remix: a sea region sinks the land inside it to the floor over its seam band and ends the roads at the new shore")
{
    json j = base();
    j["seams"]["width"] = 3000;
    const std::shared_ptr<RemixResult> fitted = runRemix(specFromJson(j));
    // Both runs on the fitted grid, so samples compare one to one.
    const Vec2 centre = fitted->after.origin + fitted->after.extent() * 0.5;
    j["map"] = {{"center", {centre.x, centre.y}}, {"size", {fitted->after.extent().x, fitted->after.extent().y}}, {"cell", fitted->after.cell}};
    const RemixSpec land = specFromJson(j);
    const std::shared_ptr<RemixResult> before = runRemix(land);
    // A channel across the whole map over the ridge and its roads, open to the sea at the map edge.
    const json poly = box(20000, -400000, 70000, 400000);
    j["regions"] = json::array({{{"id", "bay"}, {"op", "sea"}, {"polygon", poly}}});
    const RemixSpec s = specFromJson(j);
    CHECK(toJson(s)["regions"][0]["op"] == "sea");
    CHECK_FALSE(toJson(s)["regions"][0].contains("translate"));
    const std::shared_ptr<RemixResult> r = runRemix(s);
    CHECK(r->errors.empty());
    const Polygon p = {{20000, -400000}, {70000, -400000}, {70000, 400000}, {20000, 400000}};
    int inner = 0, outer = 0;
    REQUIRE(r->after.nx == before->after.nx);
    REQUIRE(r->after.nz == before->after.nz);
    for (int z = 0; z < r->after.nz; z += 3)
        for (int x = 0; x < r->after.nx; x += 3)
        {
            const Vec2 q = r->after.samplePos(x, z);
            const double sd = signedDistance(p, q);
            if (sd > 3500.0)
            {
                // Past the band: sea floor, under sea level.
                CHECK(r->after.at(x, z) < 0.0f);
                ++inner;
            }
            else if (sd < -6000.0 && before->after.at(x, z) > 100.0f)
            {
                CHECK(r->after.at(x, z) == doctest::Approx(before->after.at(x, z)).epsilon(1e-3));
                ++outer;
            }
        }
    CHECK(inner > 20);
    CHECK(outer > 100);
    // Nothing is left inside, and the roads cut at its edge stop there: no reconnection through the water, nothing dangling.
    for (const OsmDoc::Node& n : r->osm.nodes)
        CHECK(signedDistance(p, r->geo.toLocal(n.at)) < 1.0);
    for (const OsmDoc::Way& w : r->osm.ways)
        CHECK(tagOf(w.tags, "remix") != "reconnect");
    CHECK(countKind(checkRemix(s, *r), "danglingRoad") == 0);
}

TEST_CASE("remix: a lagoon the elevation holds above sea level joins the sea where the land cover says water")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("lila_remix_lagoon_" + std::to_string(std::rand()));
    fs::create_directories(dir);
    auto raster = [](int n, auto value) {
        terrain::Dem d;
        d.width = d.height = n;
        d.geographic = true;
        d.dx = d.dy = 0.02 / (n - 1);
        d.x0 = -106.5;
        d.y0 = 23.02;
        d.epsg = 4326;
        d.metres.resize(size_t(n) * size_t(n));
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                d.metres[size_t(y) * n + x] = value(double(x) / (n - 1), double(y) / (n - 1));
        return d;
    };
    // Open sea along the west edge; a lagoon behind it held at 1.5 m; land at 10 m.
    auto lagoon = [](double u, double v) { return u > 0.12 && u < 0.5 && v > 0.35 && v < 0.65; };
    terrain::writeGeoTiff(dir / "dem.tif", raster(81, [&](double u, double v) { return u <= 0.12 ? 0.0f : lagoon(u, v) ? 1.5f : 10.0f; }));
    terrain::writeGeoTiff(dir / "cover.tif", raster(241, [&](double u, double v) { return u <= 0.12 || lagoon(u, v) ? 80.0f : 40.0f; }), true);

    json j = {{"scale", {{"horizontal", 1.0}, {"vertical", 1.0}}}, {"seams", {{"erosion", 0}, {"detail", 0.0}}},
              {"sources", json::array({{{"id", "lagoon"}, {"dem", (dir / "dem.tif").generic_string()}}})}};
    const std::shared_ptr<RemixResult> bare = runRemix(specFromJson(j));
    j["sources"][0]["landcover"] = (dir / "cover.tif").generic_string();
    const std::shared_ptr<RemixResult> covered = runRemix(specFromJson(j));
    REQUIRE(bare->errors.empty());
    REQUIRE(covered->errors.empty());
    // The middle of the lagoon: an island of 1.5 m ground without land cover, sea floor with it.
    const Vec2 mid = bare->after.origin + Vec2(0.31 * bare->after.extent().x, 0.5 * bare->after.extent().y);
    CHECK(bare->after.heightAt(mid.x, mid.y) > 100.0);
    // At least a metre deep, whatever the floor reads there.
    CHECK(covered->after.heightAt(mid.x, mid.y) < -90.0);
    // Dry land stays where it was.
    const Vec2 land = bare->after.origin + Vec2(0.8 * bare->after.extent().x, 0.2 * bare->after.extent().y);
    CHECK(covered->after.heightAt(land.x, land.y) == doctest::Approx(bare->after.heightAt(land.x, land.y)).epsilon(1e-3));
    fs::remove_all(dir);
}
