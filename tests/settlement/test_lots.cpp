// Settlement blocks, lots, land use, landmarks and envelopes: frontage, overlaps, area distribution, setbacks, party
// walls, foundations, overrides by stable id, massing and the commands.

#include "TestUtil.h"
#include "Parcels.h"

#include "curve/CurveBoolean.h"
#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "gn/NodeLib.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
Options lotsProfile(const char* name, uint32_t seed = 1)
{
    Options o;
    REQUIRE(builtinProfile(name, &o.profile));
    o.seed = seed;
    return o;
}

Site lotsSquare(double size)
{
    Site s;
    s.size = size;
    return s;
}

/** A grid ground with heights from f(x, z), facing up. */
template <class F> geo::Mesh lotsGround(double size, int n, F&& f)
{
    geo::Mesh m;
    std::vector<geo::Index> ids;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
        {
            const double px = -size / 2 + size * x / n, pz = -size / 2 + size * z / n;
            ids.push_back(m.addPoint({px, f(px, pz), pz}));
        }
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            const geo::Index a = ids[size_t(z * (n + 1) + x)], b = a + 1, c = ids[size_t((z + 1) * (n + 1) + x)], d = c + 1;
            m.addFace({a, c, d, b});
        }
    return m;
}

curve::Spline lotsSpline(const Polygon& p)
{
    curve::Spline s;
    s.kind = curve::Kind::Poly;
    s.closed = true;
    for (const Vec2& q : p.pts)
        s.points.push_back({q.x, 0.0, q.y});
    return s;
}

/** Area two polygons share, cm2, by the curve boolean. */
double lotsOverlap(const Polygon& a, const Polygon& b)
{
    curve::CurveBooleanOptions o;
    o.op = curve::BooleanOp::Intersect;
    o.normal = {0.0, 1.0, 0.0};
    const curve::CurveBooleanResult r = curve::curveBoolean({{lotsSpline(a)}, {lotsSpline(b)}}, o);
    double total = 0.0;
    for (const curve::Spline& s : r.curves)
    {
        std::vector<Vec2> pts;
        for (const glm::dvec3& q : s.points)
            pts.push_back({q.x, q.z});
        total += signedArea(pts);
    }
    return std::abs(total);
}

/** Counts lot pairs that overlap by more than 0.5 m2. */
int lotsOverlapping(const Layout& l)
{
    std::vector<std::pair<Vec2, Vec2>> box;
    for (const Lot& lot : l.lots)
    {
        Vec2 lo(1e300), hi(-1e300);
        for (const Vec2& q : lot.outline.pts)
            lo = glm::min(lo, q), hi = glm::max(hi, q);
        box.push_back({lo, hi});
    }
    int bad = 0;
    for (size_t i = 0; i < l.lots.size(); ++i)
        for (size_t j = i + 1; j < l.lots.size(); ++j)
        {
            if (box[i].second.x <= box[j].first.x + 1.0 || box[j].second.x <= box[i].first.x + 1.0 || box[i].second.y <= box[j].first.y + 1.0 ||
                box[j].second.y <= box[i].first.y + 1.0)
                continue;
            if (const double ov = lotsOverlap(l.lots[i].outline, l.lots[j].outline); ov > 0.5 * kM2)
            {
                if (++bad <= 6)
                    MESSAGE("overlap " << ov / kM2 << " m2: blocks " << l.lots[i].block << "/" << l.lots[j].block << " methods "
                                       << std::string(lotMethodName(l.blocks[size_t(l.lots[i].block)].method)) << "/" << std::string(lotMethodName(l.blocks[size_t(l.lots[j].block)].method))
                                       << " areas " << l.lots[i].area << "/" << l.lots[j].area << " sizes " << l.lots[i].outline.size() << "/" << l.lots[j].outline.size());
            }
        }
    return bad;
}

double lotsSegmentDistance(Vec2 q, Vec2 a, Vec2 b)
{
    const Vec2 d = b - a;
    const double t = std::clamp(glm::dot(q - a, d) / std::max(glm::dot(d, d), 1e-12), 0.0, 1.0);
    return glm::length(a + d * t - q);
}

bool lotsInside(const Polygon& outer, Vec2 q, double tol)
{
    if (contains(outer.pts, q))
        return true;
    for (size_t k = 0; k < outer.size(); ++k)
        if (lotsSegmentDistance(q, outer.pts[k], outer.pts[(k + 1) % outer.size()]) < tol)
            return true;
    return false;
}

const Lot* lotsLandmark(const Layout& l, const std::string& name)
{
    for (const Lot& lot : l.lots)
        if (lot.landmark == name)
            return &lot;
    return nullptr;
}

void lotsLoadPackage()
{
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
}
} // namespace

TEST_CASE("settlement lots: every lot faces a street and no two lots overlap, in every style")
{
    for (const char* name : {"medievalTown", "europeanCity", "usTown", "modernCity", "hamlet"})
    {
        CAPTURE(std::string(name));
        const Layout l = build(lotsSquare(50000.0), Ground::plane(), lotsProfile(name, 4));
        REQUIRE(l.lots.size() > 10);
        int landlocked = 0;
        for (const Lot& lot : l.lots)
            landlocked += lot.frontages == 0 ? 1 : 0;
        CHECK(landlocked == 0);
        CHECK(lotsOverlapping(l) == 0);
        const json s = parcelStats(l);
        CHECK(s["landlockedLots"].get<int>() == 0);
    }
}

TEST_CASE("settlement lots: each lot method keeps every lot on a street and the lots apart")
{
    for (const char* method : {"frontage", "obb", "skeleton", "burgage", "voronoi", "strips"})
    {
        CAPTURE(std::string(method));
        Options o = lotsProfile("medievalTown", 2);
        o.profile.lotMethod = method;
        const Layout l = build(lotsSquare(40000.0), Ground::plane(), o);
        int ordinary = 0, withMethod = 0;
        for (const Block& b : l.blocks)
            if (b.method != LotMethod::Whole && b.type != BlockType::Field)
            {
                ++ordinary;
                withMethod += std::string(lotMethodName(b.method)) == method ? 1 : 0;
            }
        CHECK(ordinary > 0);
        CHECK(withMethod == ordinary);
        int landlocked = 0;
        for (const Lot& lot : l.lots)
            landlocked += lot.frontages == 0 ? 1 : 0;
        CHECK(landlocked == 0);
        CHECK(lotsOverlapping(l) == 0);
        // Fallbacks are reported, and rare.
        CHECK(int(l.parcelWarnings.size()) <= ordinary / 4);
    }
}

TEST_CASE("settlement lots: lot areas follow the style's lot size")
{
    for (const char* name : {"medievalTown", "usTown", "europeanCity"})
    {
        CAPTURE(std::string(name));
        const Options o = lotsProfile(name, 3);
        const Layout l = build(lotsSquare(60000.0), Ground::plane(), o);
        const json a = parcelStats(l)["lotAreaM2"];
        const double target = o.profile.lotWidth * o.profile.lotDepth / kM2;
        CAPTURE(a.dump());
        CHECK(a["count"].get<int>() > 30);
        CHECK(a["p50"].get<double>() > 0.2 * target);
        CHECK(a["p50"].get<double>() < 3.0 * target);
        CHECK(a["p90"].get<double>() / std::max(a["p10"].get<double>(), 1.0) < 40.0);
    }
}

TEST_CASE("settlement lots: block types by era: market, rows and fields in a medieval town, courtyards in a European city")
{
    const Layout medieval = build(lotsSquare(60000.0), Ground::plane(), lotsProfile("medievalTown", 5));
    std::map<std::string, int> types;
    for (const Block& b : medieval.blocks)
        ++types[blockTypeName(b.type)];
    CHECK(types["market"] == 1);
    CHECK(types["row"] + types["perimeter"] > 5);
    CHECK(types["field"] > 0);

    const Layout capital = build(lotsSquare(80000.0), Ground::plane(), lotsProfile("europeanCity", 5));
    int courtyards = 0, withYard = 0;
    for (const Block& b : capital.blocks)
        if (b.type == BlockType::Courtyard)
        {
            ++courtyards;
            withYard += b.courtyards.empty() ? 0 : 1;
        }
    CHECK(courtyards > 0);
    CHECK(withYard > 0);

    const Layout modern = build(lotsSquare(120000.0), Ground::plane(), lotsProfile("modernCity", 5));
    int open = 0;
    for (const Block& b : modern.blocks)
        open += b.type == BlockType::Open || b.type == BlockType::Superblock ? 1 : 0;
    CHECK(open > 0);
}

TEST_CASE("settlement lots: land use from rules, landmarks with reasons, ports on the river")
{
    Site site = lotsSquare(60000.0);
    site.water.push_back({{{-30000, 12000}, {0, 9000}, {30000, 14000}}, 1500.0});
    const Options o = lotsProfile("medievalTown", 6);
    const Layout l = build(site, Ground::plane(), o);
    const json s = parcelStats(l);
    CAPTURE(s["lotUses"].dump());
    for (const char* use : {"residential", "commercial", "market", "port", "industrial", "religious", "civic", "plaza"})
        CHECK(s["lotUses"].value(use, 0) > 0);
    for (const char* tier : {"low", "middle", "high"})
        CHECK(s["residentialWealth"].value(tier, 0) > 0);
    for (const char* name : {"market_square", "church", "town_hall"})
    {
        CAPTURE(std::string(name));
        const Lot* lot = lotsLandmark(l, name);
        REQUIRE(lot);
        CHECK(!lot->reasons.empty());
    }
    CHECK(lotsLandmark(l, "church")->reasons.front() == "fronts market_square");
    // Industry goes downwind: the prevailing wind blows toward +X.
    double x = 0.0;
    int n = 0;
    for (const Lot& lot : l.lots)
        if (lot.use == Use::Industrial)
        {
            x += centroid(lot.outline).x - l.center.x;
            ++n;
        }
    REQUIRE(n > 0);
    CHECK(x / n > 0.0);
    // Access is highest on the arterials.
    double art = 0.0, loc = 0.0;
    int na = 0, nl = 0;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)))
        {
            if (l.graph.edges[e].cls == StreetClass::Arterial)
                art += l.edgeAccess[e], ++na;
            else if (l.graph.edges[e].cls == StreetClass::Local)
                loc += l.edgeAccess[e], ++nl;
        }
    REQUIRE(na > 0);
    REQUIRE(nl > 0);
    CHECK(art / na > loc / nl);
}

TEST_CASE("settlement lots: a castle on the hill, parks on steep ground, and every foundation below the ground")
{
    const geo::Mesh hill = lotsGround(60000.0, 60, [](double x, double z) {
        return 2500.0 * std::exp(-((x - 14000.0) * (x - 14000.0) + (z - 10000.0) * (z - 10000.0)) / (2.0 * 5000.0 * 5000.0)) + 0.04 * x;
    });
    const Ground ground = Ground::fromMesh(hill, 200.0);
    Site site;
    site.outline = {{-28000, -28000}, {28000, -28000}, {28000, 28000}, {-28000, 28000}};
    const Layout l = build(site, ground, lotsProfile("medievalTown", 3));
    const Lot* castle = lotsLandmark(l, "castle");
    REQUIRE(castle);
    CHECK(glm::length(centroid(castle->outline) - Vec2(14000.0, 10000.0)) < 9000.0);
    CHECK(castle->reasons.front().find("high ground") != std::string::npos);

    REQUIRE(!l.envelopes.empty());
    int above = 0;
    for (const Envelope& env : l.envelopes)
    {
        CHECK(env.base < env.ground);
        CHECK(env.foundationSource == "ground");
        CHECK(env.foundationDepth >= l.profile.foundation - 1e-6);
        Vec2 lo(1e300), hi(-1e300);
        for (const Vec2& q : env.footprint.pts)
            lo = glm::min(lo, q), hi = glm::max(hi, q);
        for (double z = lo.y; z <= hi.y; z += 100.0)
            for (double x = lo.x; x <= hi.x; x += 100.0)
                if (contains(env.footprint.pts, {x, z}) && env.base > ground.height({x, z}))
                    ++above;
    }
    CHECK(above == 0);

    // Measured foundations win where they reach lower.
    Site measured = site;
    const Envelope& first = l.envelopes.front();
    measured.foundations[first.key] = {first.ground - 300.0, first.ground + 50.0};
    const Layout m = build(measured, ground, lotsProfile("medievalTown", 3));
    auto it = std::ranges::find_if(m.envelopes, [&](const Envelope& e) { return e.key == first.key; });
    REQUIRE(it != m.envelopes.end());
    CHECK(it->foundationSource == "terrainRegionStats");
    CHECK(it->base == doctest::Approx(first.ground - 300.0 - l.profile.foundation));
}

TEST_CASE("settlement lots: envelopes stay inside their lot minus setbacks, tiers inside the footprint")
{
    for (const char* name : {"medievalTown", "usTown", "manhattan", "modernCity", "europeanCity"})
    {
        CAPTURE(std::string(name));
        const Layout l = build(lotsSquare(60000.0), Ground::plane(), lotsProfile(name, 2));
        REQUIRE(!l.envelopes.empty());
        int outside = 0, close = 0, tiers = 0;
        for (const Envelope& env : l.envelopes)
        {
            const Lot& lot = l.lots[size_t(env.lot)];
            CHECK(lot.key == env.key);
            for (const Vec2& q : env.footprint.pts)
            {
                if (!lotsInside(lot.outline, q, 1.0))
                    ++outside;
                for (size_t k = 0; k < lot.outline.size(); ++k)
                {
                    const double keep = lot.outline.tag[k] >= 0 ? env.frontSetback : env.sideSetback;
                    if (lotsSegmentDistance(q, lot.outline.pts[k], lot.outline.pts[(k + 1) % lot.outline.size()]) < keep - 1.0)
                        ++close;
                }
            }
            REQUIRE(!env.tiers.empty());
            CHECK(env.tiers.front().bottom == doctest::Approx(env.base));
            CHECK(env.tiers.back().top == doctest::Approx(env.top));
            for (size_t t = 1; t < env.tiers.size(); ++t)
            {
                ++tiers;
                for (const Vec2& q : env.tiers[t].footprint.pts)
                    CHECK(lotsInside(env.tiers[t - 1].footprint, q, 1.0));
            }
            CHECK(!env.entrances.empty());
        }
        CHECK(outside == 0);
        CHECK(close == 0);
        if (std::string(name) == "manhattan")
            CHECK(tiers > 0);
    }
}

TEST_CASE("settlement lots: party walls are marked on both neighbours")
{
    const Layout l = build(lotsSquare(50000.0), Ground::plane(), lotsProfile("medievalTown", 8));
    std::map<int, const Envelope*> byLot;
    for (const Envelope& env : l.envelopes)
        byLot[env.lot] = &env;
    int party = 0, unmatched = 0;
    for (const Envelope& env : l.envelopes)
        for (size_t a = 0; a < env.footprint.size(); ++a)
        {
            if (env.footprint.tag[a] != 1)
                continue;
            ++party;
            const auto it = byLot.find(env.partyWith[a]);
            if (it == byLot.end())
            {
                ++unmatched;
                continue;
            }
            const Envelope& other = *it->second;
            const Vec2 p = env.footprint.pts[a], q = env.footprint.pts[(a + 1) % env.footprint.size()];
            const Vec2 u = glm::normalize(q - p);
            bool found = false;
            for (size_t b = 0; b < other.footprint.size(); ++b)
            {
                const Vec2 r = other.footprint.pts[b], s = other.footprint.pts[(b + 1) % other.footprint.size()];
                if (other.footprint.tag[b] != 1 || std::abs(cross(u, r - p)) > 5.0 || std::abs(cross(u, s - p)) > 5.0 || glm::dot(u, s - r) >= 0.0)
                    continue;
                const double t0 = glm::dot(r - p, u), t1 = glm::dot(s - p, u);
                if (std::min(glm::length(q - p), std::max(t0, t1)) - std::max(0.0, std::min(t0, t1)) >= 100.0)
                    found = true;
            }
            if (!found)
                ++unmatched;
        }
    CHECK(party > 20);
    CHECK(unmatched == 0);
}

TEST_CASE("settlement lots: stable ids hold across rebuilds, and overrides by id change one block and one lot")
{
    const Options o = lotsProfile("medievalTown", 9);
    const Layout a = build(lotsSquare(40000.0), Ground::plane(), o);
    const Layout b = build(lotsSquare(40000.0), Ground::plane(), o);
    REQUIRE(a.blocks.size() == b.blocks.size());
    std::set<uint64_t> keys;
    for (size_t k = 0; k < a.blocks.size(); ++k)
    {
        CHECK(a.blocks[k].key == b.blocks[k].key);
        keys.insert(a.blocks[k].key);
    }
    CHECK(keys.size() == a.blocks.size());

    const Block* target = nullptr;
    for (const Block& blk : a.blocks)
        if (blk.method != LotMethod::Whole && blk.area > 2000.0)
            target = &blk;
    REQUIRE(target);
    Site site = lotsSquare(40000.0);
    applyEdits(site, {{"blockOverrides", json::array({{{"block", target->key}, {"type", "open"}, {"method", "obb"}}})},
                      {"lotOverrides", json::array({{{"lot", a.lots[3].key}, {"landmark", "guild_hall"}, {"use", "civic"}}})}});
    const Layout c = build(site, Ground::plane(), o);
    int changed = 0;
    for (const Block& blk : c.blocks)
        if (blk.key == target->key)
        {
            CHECK(blk.type == BlockType::Open);
            CHECK(blk.method == LotMethod::Obb);
            ++changed;
        }
    CHECK(changed == 1);
    const Lot* hall = lotsLandmark(c, "guild_hall");
    REQUIRE(hall);
    CHECK(hall->use == Use::Civic);
    CHECK(hall->reasons == std::vector<std::string>{"placed by hand"});
    CHECK_THROWS(applyEdits(site, {{"blockOverrides", json::array({{{"block", 1}, {"type", "castle"}}})}}));
}

TEST_CASE("settlement lots: massing gives tiers and roofs, by route and district")
{
    Site site = lotsSquare(40000.0);
    site.buildingRoutes.push_back({BuildingRoute::Massing, "", {}});
    site.buildingRoutes.push_back({BuildingRoute::Library, "east", {{5000, -20000}, {20000, -20000}, {20000, 20000}, {5000, 20000}}});
    const Layout l = build(site, Ground::plane(), lotsProfile("medievalTown", 1));
    int massing = 0, library = 0;
    for (const Envelope& env : l.envelopes)
    {
        massing += env.route == BuildingRoute::Massing ? 1 : 0;
        library += env.route == BuildingRoute::Library ? 1 : 0;
        if (env.route == BuildingRoute::Library)
            CHECK(centroid(l.lots[size_t(env.lot)].outline).x > 5000.0);
    }
    CHECK(massing > 0);
    CHECK(library > 0);
    CHECK(l.placements.empty()); // nothing fitted yet

    // Fitted library assets become engine placements, turned to the street, on every rebuild.
    Site picked = site;
    for (const Envelope& env : l.envelopes)
        if (env.route == BuildingRoute::Library)
            picked.libraryPicks[env.key] = {"Town/House", "/Game/Town/SM_House", 0.0};
    const Layout withPicks = build(picked, Ground::plane(), lotsProfile("medievalTown", 1));
    REQUIRE(int(withPicks.placements.size()) == library);
    for (const Placement& pl : withPicks.placements)
    {
        CHECK(pl.asset == "/Game/Town/SM_House");
        CHECK(pl.id.rfind("building/", 0) == 0);
        CHECK(glm::length(pl.rotation) == doctest::Approx(1.0));
        CHECK(pl.position.x > 5000.0 - 500.0); // a building stands on its lot's frontage, a little off the lot's centre
    }
    CHECK(build(picked, Ground::plane(), lotsProfile("medievalTown", 1)).placements.front().id == withPicks.placements.front().id);
    const MeshParts parts = toMeshes(l, Ground::plane());
    const auto part = parts.envelopes.faceAttrs().get<int32_t>("part");
    int roofs = 0;
    for (int32_t p : part)
        roofs += p == 1 ? 1 : 0;
    CHECK(roofs > massing); // pitched roofs have two or more planes
    std::map<std::string, int> kinds;
    for (const Envelope& env : l.envelopes)
        ++kinds[env.roof];
    CHECK(kinds["gable"] > 0);
}

TEST_CASE("settlement lots: commands list, override by id with undo, choose routes, and name what is missing")
{
    lotsLoadPackage();
    run("newScene");
    const json made = run("settlementCreate", {{"profile", "medievalTown"}, {"size", 40000.0}, {"seed", 3}});
    const std::string mesh = made["mesh"].get<std::string>();
    const json blocks = run("settlementBlocks", {{"args", {mesh}}});
    REQUIRE(blocks["blocks"].size() > 3);
    json target;
    for (const json& b : blocks["blocks"])
        if (b["method"] != "whole" && b["areaM2"].get<double>() > 1500.0)
            target = b;
    REQUIRE(target.is_object());
    const uint64_t id = target["blockId"].get<uint64_t>();
    const json sub = run("settlementSubdivide", {{"args", {mesh}}, {"block", id}, {"type", "open"}, {"method", "voronoi"}});
    REQUIRE(sub["blocks"].size() == 1);
    CHECK(sub["blocks"][0]["type"] == "open");
    CHECK(sub["blocks"][0]["method"] == "voronoi");
    run("undo");
    for (const json& b : run("settlementBlocks", {{"args", {mesh}}})["blocks"])
        if (b["blockId"].get<uint64_t>() == id)
            CHECK(b["method"] == target["method"]);

    const json lots = run("settlementLots", {{"args", {mesh}}, {"landmarks", true}});
    CHECK(lots["count"].get<int>() >= 3);
    const json lot0 = run("settlementLots", {{"args", {mesh}}, {"use", "residential"}, {"limit", 1}})["lots"][0];
    const json placed = run("settlementPlaceLandmark", {{"args", {mesh}}, {"lot", lot0["lotId"]}, {"name", "well"}, {"use", "civic"}});
    CHECK(placed["landmark"] == "well");
    CHECK(placed["use"] == "civic");
    const json used = run("settlementLandUse", {{"args", {mesh}}, {"lot", lot0["lotId"]}, {"use", "industrial"}});
    CHECK(used["lots"][0]["use"] == "industrial");
    CHECK(used["uses"]["industrial"].get<int>() >= 1);

    const json env = run("settlementEnvelopes", {{"args", {mesh}}, {"limit", 5}});
    REQUIRE(env["envelopes"].size() == 5);
    for (const char* key : {"tiers", "sides", "partyWith", "entrances", "roof", "ridge", "foundationDepth", "foundationSource", "groundUse", "corner"})
        CHECK(env["envelopes"][0].contains(key));

    const json massing = run("settlementBuildings", {{"args", {mesh}}, {"route", "massing"}});
    CHECK(massing["envelopes"].get<int>() > 0);
    CHECK_THROWS_WITH_AS(run("settlementBuildings", {{"args", {mesh}}, {"route", "library"}}), doctest::Contains("librarySearch"), std::exception);
    // Another suite in this executable may have loaded Masonry already.
    if (ops::find("masonryBuilding"))
        CHECK(run("settlementBuildings", {{"args", {mesh}}, {"route", "masonry"}}).dump().find("built") != std::string::npos);
    else
        CHECK_THROWS_WITH_AS(run("settlementBuildings", {{"args", {mesh}}, {"route", "masonry"}}), doctest::Contains("masonryBuilding"), std::exception);
    CHECK_THROWS_WITH_AS(run("settlementFoundations", {{"args", {mesh}}}), doctest::Contains("flat ground"), std::exception);
}

TEST_CASE("settlement lots: foundations measured on a Terrain with terrainRegionStats")
{
    lotsLoadPackage();
    if (!ops::find("terrainRegionStats"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
    run("newScene");
    const json terrain = run("terrainCreate", {{"size", json::array({60000.0, 60000.0})}, {"resolution", 129}, {"amplitude", 1500.0}, {"seed", 3}});
    const std::string ground = terrain["mesh"].get<std::string>();
    const json made = run("settlementCreate", {{"profile", "hamlet"}, {"size", 30000.0}, {"seed", 2}, {"ground", ground}});
    const std::string mesh = made["mesh"].get<std::string>();
    const json before = run("settlementEnvelopes", {{"args", {mesh}}, {"limit", 0}});
    CHECK(before.contains("warning"));
    CHECK(before["foundationSources"].value("ground", 0) == before["count"].get<int>());

    const json measured = run("settlementFoundations", {{"args", {mesh}}});
    CAPTURE(measured.dump());
    CHECK(measured["measured"].get<int>() > 0);
    CHECK(measured["terrain"] == ground);
    const json after = run("settlementEnvelopes", {{"args", {mesh}}, {"limit", 0}});
    CHECK(after["foundationSources"].value("terrainRegionStats", 0) == measured["measured"].get<int>());
    CHECK_FALSE(after.contains("warning"));
    for (const json& e : after["envelopes"])
        CHECK(e["base"].get<double>() < e["ground"].get<double>());
    run("undo");
    CHECK(run("settlementEnvelopes", {{"args", {mesh}}, {"limit", 1}})["foundationSources"].value("terrainRegionStats", 0) == 0);
}
