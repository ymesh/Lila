// Settlement: stable street ids and changes to generated streets, road cross-sections as shape grammars, and the
// junction control and parking a traffic system reads.

#include "TestUtil.h"
#include "SettlementCore.h"

#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <random>
#include <set>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
Options grid()
{
    Options o;
    REQUIRE(builtinProfile("usTown", &o.profile));
    return o;
}

std::set<uint64_t> streetIds(const Layout& l)
{
    std::set<uint64_t> ids;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)))
            ids.insert(l.graph.edges[e].street);
    return ids;
}

/** The id of the street through the edge nearest q. */
uint64_t streetNear(const Layout& l, Vec2 q)
{
    const int e = l.graph.nearestEdge(q, 1e6);
    REQUIRE(e >= 0);
    return l.graph.edges[size_t(e)].street;
}
} // namespace

TEST_CASE("settlement: generated streets keep their ids through unrelated edits and re-runs")
{
    Site site{.size = 120000.0};
    const Layout a = build(site, Ground::plane(), grid());
    const std::set<uint64_t> before = streetIds(a);
    REQUIRE(before.size() > 10);
    for (uint64_t id : before)
        CHECK(id >= kStreetIdBit);
    CHECK(streetIds(build(site, Ground::plane(), grid())) == before); // the same every run

    // An exclusion in one corner reshapes the streets there and nowhere else.
    site.exclusions.push_back({1, "", "", {{40000, 40000}, {58000, 40000}, {58000, 58000}, {40000, 58000}}, 0.0});
    const Layout b = build(site, Ground::plane(), grid());
    const std::set<uint64_t> after = streetIds(b);
    size_t kept = 0;
    for (uint64_t id : before)
        kept += after.contains(id);
    CHECK(double(kept) / double(before.size()) > 0.6);
    const uint64_t far = streetNear(a, {-50000, -45000});
    CHECK(after.contains(far));

    // Overrides follow the id: one street removed, one made an avenue, one id that no longer exists reported.
    const uint64_t other = streetNear(a, {-20000, -45000});
    REQUIRE(other != far);
    site.overrides.push_back({far, true});
    StreetOverride wider;
    wider.street = other;
    wider.hasClass = true;
    wider.cls = StreetClass::Arterial;
    wider.profile = "avenue";
    site.overrides.push_back(wider);
    site.overrides.push_back({kStreetIdBit + 12345, true});
    const Layout c = build(site, Ground::plane(), grid());
    CHECK_FALSE(streetIds(c).contains(far));
    bool found = false;
    for (size_t e = 0; e < c.graph.edges.size(); ++e)
        if (c.graph.isStreet(int(e)) && c.graph.edges[e].street == other)
        {
            found = true;
            CHECK(c.graph.edges[e].cls == StreetClass::Arterial);
            CHECK(edgeProfile(c, int(e)).name == "avenue");
        }
    CHECK(found);
    REQUIRE(c.staleOverrides.size() == 1);
    CHECK(c.staleOverrides[0] == kStreetIdBit + 12345);
}

TEST_CASE("settlement: organic streets keep their ids through an edit in one corner")
{
    for (const std::string name : {"medievalTown", "hamlet"})
    {
        CAPTURE(name);
        Options o;
        REQUIRE(builtinProfile(name, &o.profile));
        Site site{.size = 60000.0};
        const Layout a = build(site, Ground::plane(), o);
        const std::set<uint64_t> before = streetIds(a);
        REQUIRE(before.size() > 10);

        auto kept = [&](const Layout& b) {
            const std::set<uint64_t> after = streetIds(b);
            size_t n = 0;
            for (uint64_t id : before)
                n += after.contains(id);
            return double(n) / double(before.size());
        };
        // A street drawn in one corner, then an exclusion in another: most of the town is laid as it was.
        Site drawn = site;
        SitePath path;
        path.id = 1;
        path.pts = {{-20000.0, 0.0, -26000.0}, {-28000.0, 0.0, -18000.0}};
        drawn.paths.push_back(path);
        const double withStreet = kept(build(drawn, Ground::plane(), o));
        Site excluded = site;
        excluded.exclusions.push_back({1, "", "", {{21000, 21000}, {29000, 21000}, {29000, 29000}, {21000, 29000}}, 0.0});
        const double withExclusion = kept(build(excluded, Ground::plane(), o));
        MESSAGE(name << " ids kept: street " << withStreet << ", exclusion " << withExclusion);
        CHECK(withStreet > 0.9);
        CHECK(withExclusion > 0.9);
    }
}

TEST_CASE("settlement: road profiles go to shape grammar slots and come back")
{
    RoadProfile avenue;
    REQUIRE(builtinRoadProfile("avenue", &avenue));
    const std::vector<CrossSlot> slots = crossSection(avenue);
    double total = 0.0;
    for (const CrossSlot& s : slots)
        total += s.size;
    CHECK(total == doctest::Approx(avenue.width()));
    const std::string g = grammarOf(slots);
    CHECK(g == "[Sidewalk,Lane,Lane,Median,Lane,Lane,Sidewalk]");
    CHECK(grammarSymbols(g).size() == slots.size());

    const RoadProfile back = profileFromSlots("avenue2", slots);
    CHECK(back.lanes == avenue.lanes);
    CHECK(back.laneWidth == doctest::Approx(avenue.laneWidth));
    CHECK(back.median == doctest::Approx(avenue.median));
    CHECK(back.sidewalk == doctest::Approx(avenue.sidewalk));
    CHECK(back.width() == doctest::Approx(avenue.width()));

    // Someone else's kit: repeats in the grammar, curbs, a bike lane and a slot name nobody knows.
    const std::vector<std::string> symbols = grammarSymbols("[Grass, Pavement, Curb, [DriveLane]2, Planter, [DriveLane]2, Curb, Pavement, Grass, Bollards]");
    REQUIRE(symbols.size() == 12);
    CHECK(symbols[3] == "DriveLane");
    const std::map<std::string, double> sizes = {{"Grass", 150}, {"Pavement", 250}, {"Curb", 30}, {"DriveLane", 330}, {"Planter", 200}, {"Bollards", 40}};
    std::vector<CrossSlot> kit;
    for (const std::string& s : symbols)
        kit.push_back({s, sizes.at(s)});
    std::vector<std::string> unknown;
    const RoadProfile p = profileFromSlots("kit", kit, &unknown);
    CHECK(p.lanes == 4);
    CHECK(p.laneWidth == doctest::Approx(330.0));
    CHECK(p.sidewalk == doctest::Approx(280.0));
    CHECK(p.verge == doctest::Approx(150.0 + 20.0 + 100.0));
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0] == "Bollards");
}

TEST_CASE("settlement: junctions get lights, stop lines and crosswalks; parked streets get spaces")
{
    Options o = grid();
    o.blocks = false;
    Site site{.size = 160000.0};
    SitePath parked;
    parked.id = 1;
    parked.profile = "parkedStreet";
    parked.pts = {{-70000, std::nan(""), 3000}, {70000, std::nan(""), 3000}};
    site.paths.push_back(parked);
    const Layout l = build(site, Ground::plane(), o);
    const RoadNetwork net = roadNetwork(l, Ground::plane());
    int signals = 0, stops = 0;
    for (const Junction& j : net.junctions)
    {
        if (j.control == Control::Signal)
        {
            ++signals;
            std::set<int> seen;
            for (const std::vector<int>& phase : j.phases)
            {
                CHECK(phase.size() <= 2);
                if (phase.size() == 2) // opposite approaches only
                    CHECK(glm::dot(j.ends[size_t(phase[0])].out, j.ends[size_t(phase[1])].out) < -0.7);
                for (int e : phase)
                    CHECK(seen.insert(e).second);
            }
        }
        stops += j.control == Control::Stop;
        if (j.control == Control::Signal || j.control == Control::Stop)
            CHECK(!j.crosswalks.empty());
    }
    CHECK(signals > 0);
    CHECK(signals + stops > 0);
    REQUIRE(!net.parking.empty());
    RoadProfile ps;
    REQUIRE(builtinRoadProfile("parkedStreet", &ps));
    for (const ParkingSpot& p : net.parking)
    {
        CHECK(std::abs(p.p.z - 3000.0) == doctest::Approx(0.5 * ps.lanes * ps.laneWidth + 0.5 * ps.parking).epsilon(0.01));
        CHECK(net.lines[size_t(p.line)].path == 1);
    }
}

TEST_CASE("settlement: commands list, change and remove generated streets by id, undo, save and reopen")
{
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    run("newScene");
    const json made = run("settlementCreate", {{"profile", "usTown"}, {"size", 80000.0}, {"blocks", false}});
    const std::string mesh = made["mesh"].get<std::string>();
    const json streets = run("settlementStreets", {{"args", {mesh}}, {"near", json::array({-30000, -30000})}, {"radius", 3000.0}});
    REQUIRE(streets.size() >= 1);
    const uint64_t id = streets[0]["id"].get<uint64_t>();
    CHECK(id >= kStreetIdBit);

    const json changed = run("settlementEditStreet", {{"args", {mesh}}, {"id", id}, {"profile", "highway"}});
    REQUIRE(changed["streets"].size() == 1);
    CHECK(changed["streets"][0]["profile"] == "highway");
    CHECK_THROWS(run("settlementEditStreet", {{"args", {mesh}}, {"id", int64_t(kStreetIdBit + 7)}, {"profile", "lane"}}));

    const json removed = run("settlementDelete", {{"args", {mesh}}, {"ids", json::array({id})}});
    CHECK(removed["streets"].size() == 1);
    auto present = [&] {
        for (const json& s : run("settlementStreets", {{"args", {mesh}}}))
            if (s["id"].get<uint64_t>() == id)
                return true;
        return false;
    };
    CHECK_FALSE(present());
    run("undo");
    CHECK(present());
    run("redo");
    CHECK_FALSE(present());

    std::random_device rd;
    const auto path = std::filesystem::temp_directory_path() / ("lila_settlement_ids_" + std::to_string(rd()) + ".lila");
    run("saveScene", {{"args", {path.string()}}});
    run("newScene");
    run("openScene", {{"args", {path.string()}}});
    CHECK_FALSE(present());
    CHECK(run("settlementInfo", {{"args", {mesh}}})["checks"]["staleOverrides"].empty());
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
