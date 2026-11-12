// Settlement streetscape: road and sidewalk meshes, furniture per road profile, street and district names, the lane
// graph's checks and transit routes.

#include "TestUtil.h"
#include "SettlementCore.h"
#include "Streetscape.h"

#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
Options styled(const char* name, bool blocks = true)
{
    Options o;
    REQUIRE(builtinProfile(name, &o.profile));
    o.blocks = blocks;
    return o;
}

double lengthXZ(const std::vector<glm::dvec3>& pts)
{
    double len = 0.0;
    for (size_t k = 0; k + 1 < pts.size(); ++k)
        len += glm::length(glm::dvec2(pts[k + 1].x - pts[k].x, pts[k + 1].z - pts[k].z));
    return len;
}

std::vector<int32_t> faceInts(const geo::Mesh& m, const char* name)
{
    std::span<const int32_t> v = m.faceAttrs().get<int32_t>(name);
    return {v.begin(), v.end()};
}

std::map<uint64_t, std::string> namesById(const Layout& l)
{
    std::map<uint64_t, std::string> out;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)))
            out[l.graph.edges[e].street] = streetName(l, l.graph.edges[e].street);
    return out;
}
} // namespace

TEST_CASE("settlement streetscape: road meshes cover each road's carriageway, patch junctions, round kerbs and mark crosswalks")
{
    Site site{.size = 120000.0};
    const Layout l = build(site, Ground::plane(), styled("usTown", false));
    const RoadNetwork net = roadNetwork(l, Ground::plane());
    REQUIRE(net.junctions.size() > 10);
    const RoadMeshes m = roadMeshes(l, net, Ground::plane());

    // Carriageway area is twice the half width times the length of every road with lanes.
    double expected = 0.0, walk = 0.0;
    for (const StreetLine& line : net.lines)
    {
        const RoadSection s = roadSection(l, line);
        expected += 2.0 * s.carriage * lengthXZ(line.pts) / kM2;
        walk += 2.0 * (s.sidewalk + s.verge) * lengthXZ(line.pts) / kM2;
    }
    CHECK(m.roadArea == doctest::Approx(expected).epsilon(0.03));
    CHECK(m.sidewalkArea == doctest::Approx(walk).epsilon(0.08)); // mitred bends add a little
    CHECK(m.junctionArea > 0.0);
    CHECK(m.roads.numFaces() > net.lines.size());

    int ends = 0, crosswalks = 0;
    for (const Junction& j : net.junctions)
    {
        ends += int(j.ends.size());
        crosswalks += int(j.crosswalks.size());
    }
    CHECK(m.curbReturns > 0);
    CHECK(m.curbReturns <= ends);
    CHECK(m.curbReturnArea > 0.0);
    CHECK(int(m.crosswalks.size()) == crosswalks);
    CHECK(m.markingArea > 0.0);
    for (const DecalPlacement& d : m.crosswalks)
    {
        CHECK(d.depth == 300.0);
        CHECK(d.width > 0.0);
    }
    // Every face tagged; sidewalks hold sidewalks, kerbs and returns only.
    const std::vector<int32_t> parts = faceInts(m.sidewalks, "part");
    REQUIRE(parts.size() == m.sidewalks.numFaces());
    std::set<int32_t> kinds(parts.begin(), parts.end());
    CHECK(kinds.count(int32_t(RoadPart::Sidewalk)));
    CHECK(kinds.count(int32_t(RoadPart::Curb)));
    CHECK(kinds.count(int32_t(RoadPart::CurbReturn)));
    CHECK_FALSE(kinds.count(int32_t(RoadPart::Carriageway)));
    // Road faces face up.
    for (geo::Index f = 0; f < std::min<geo::Index>(m.roads.numFaces(), 200); ++f)
        CHECK(m.roads.faceNormal(f).y > 0.9);

    // Medieval: cobble and earth edge to edge, a gutter, no kerbs or markings.
    Options mo = styled("medievalTown", false);
    const Layout ml = build(Site{.size = 60000.0}, Ground::plane(), mo);
    const RoadNetwork mnet = roadNetwork(ml, Ground::plane());
    const RoadMeshes mm = roadMeshes(ml, mnet, Ground::plane());
    CHECK(medievalStyle(ml));
    CHECK(mm.sidewalks.numFaces() == 0);
    CHECK(mm.markings.numFaces() == 0);
    CHECK(mm.curbReturns == 0);
    double medievalExpected = 0.0;
    for (const StreetLine& line : mnet.lines)
        medievalExpected += line.width * lengthXZ(line.pts) / kM2;
    CHECK(mm.roadArea == doctest::Approx(medievalExpected).epsilon(0.03));
    std::set<int32_t> surfaces;
    for (int32_t s : faceInts(mm.roads, "surface"))
        surfaces.insert(s);
    CHECK(surfaces.count(int32_t(Surface::Cobble)));
    CHECK_FALSE(surfaces.count(int32_t(Surface::Asphalt)));
    double lowest = 1e9;
    for (geo::Index p = 0; p < mm.roads.numPoints(); ++p)
        lowest = std::min(lowest, mm.roads.position(p).y);
    CHECK(lowest == doctest::Approx(2.0 - 8.0)); // lift less the gutter
}

TEST_CASE("settlement streetscape: furniture follows each road profile's set, junction control and squares, with stable ids")
{
    Site site{.size = 120000.0};
    SitePath avenue;
    avenue.id = 1;
    avenue.profile = "avenue";
    avenue.cls = StreetClass::Arterial;
    avenue.pts = {{-55000, std::nan(""), 1500}, {55000, std::nan(""), 1500}};
    site.paths.push_back(avenue);
    const Layout l = build(site, Ground::plane(), styled("usTown"));
    const RoadNetwork net = roadNetwork(l, Ground::plane());
    const std::vector<FurnitureSet> sets = furnitureSets(false);
    const std::vector<FurniturePlacement> placed = placeFurniture(l, net, Ground::plane(), sets);

    // Lamps on avenues: alternate sides, one per 30 m slot of each avenue line.
    int lampSlots = 0, hydrants = 0, treeSlots = 0;
    for (const StreetLine& line : net.lines)
    {
        const std::string& prof = l.roads[size_t(line.profile)].name;
        const double len = lengthXZ(line.pts);
        if (prof == "avenue" && len >= 1500.0)
            lampSlots += std::max(1, int(len / 3000.0));
        if (prof == "avenue" && roadSection(l, line).sidewalk >= 250.0 && len >= 500.0)
            treeSlots += 2 * std::max(1, int(len / 1000.0));
        if ((prof == "avenue" || prof == "street" || prof == "parkedStreet") && len >= 4500.0)
            hydrants += std::max(1, int(len / 9000.0));
    }
    std::map<std::string, std::map<std::string, int>> byProfile;
    std::set<uint64_t> ids;
    for (const FurniturePlacement& p : placed)
    {
        ++byProfile[p.profile][p.kind];
        CHECK(ids.insert(p.id).second);
        CHECK(p.id <= kStreetscapeIdMask);
    }
    REQUIRE(lampSlots > 0);
    CHECK(byProfile["avenue"]["lamp"] == lampSlots);
    CHECK(byProfile["avenue"]["tree"] == treeSlots);
    int hydrantCount = 0;
    for (const char* p : {"avenue", "street", "parkedStreet"})
        hydrantCount += byProfile[p]["hydrant"];
    CHECK(hydrantCount == hydrants);

    int signalEnds = 0, stopEnds = 0;
    for (const Junction& j : net.junctions)
        for (const RoadEnd& e : j.ends)
            if (e.vehicle)
            {
                signalEnds += j.control == Control::Signal;
                stopEnds += j.control == Control::Stop;
            }
    REQUIRE(signalEnds > 0);
    CHECK(byProfile["junction"]["signal"] == signalEnds);
    CHECK(byProfile["junction"]["stopSign"] == stopEnds);
    CHECK(byProfile["junction"]["streetSign"] > 0);
    // A modern square: benches round it and trees on it.
    const Layout city = build(Site{.size = 120000.0}, Ground::plane(), styled("europeanCity"));
    std::map<std::string, int> square;
    int plazas = 0;
    for (const Block& b : city.blocks)
        plazas += b.use == Use::Plaza;
    REQUIRE(plazas == 1);
    for (const FurniturePlacement& p : placeFurniture(city, roadNetwork(city, Ground::plane()), Ground::plane(), sets))
        if (p.profile == "plaza")
            ++square[p.kind];
    CHECK(square["bench"] > 0);

    // The same layout gives the same ids; a set given as data changes only its profile.
    const std::vector<FurniturePlacement> again = placeFurniture(l, net, Ground::plane(), sets);
    REQUIRE(again.size() == placed.size());
    for (size_t k = 0; k < placed.size(); ++k)
        CHECK(again[k].id == placed[k].id);
    const std::vector<FurnitureSet> custom = applyFurnitureJson(sets, json::parse(R"({"avenue": [{"kind": "lamp", "spacing": 1500, "sides": "both"}]})"));
    std::map<std::string, int> avenueKinds;
    for (const FurniturePlacement& p : placeFurniture(l, net, Ground::plane(), custom))
        if (p.profile == "avenue")
            ++avenueKinds[p.kind];
    CHECK(avenueKinds.size() == 1);
    CHECK(avenueKinds["lamp"] > 2 * lampSlots);
    CHECK_THROWS(applyFurnitureJson(sets, json::parse(R"({"avenue": [{"kind": "lamp", "where": "roof"}]})")));

    // The layout's placements: one per piece, same ids as text, asset by kind, turned about +Y only.
    Layout withPlacements = l;
    const json edits = json::parse(R"({"furniture": {"assets": {"lamp": "/Game/City/SM_Lamp"}}})");
    addFurniturePlacements(withPlacements, Ground::plane(), edits);
    REQUIRE(withPlacements.placements.size() == placed.size());
    for (size_t k = 0; k < placed.size(); ++k)
    {
        const Placement& p = withPlacements.placements[k];
        CHECK(p.id == std::to_string(placed[k].id));
        CHECK(p.asset == (placed[k].kind == "lamp" ? std::string("/Game/City/SM_Lamp") : furnitureAsset(json(), placed[k].kind)));
        CHECK(glm::length(p.rotation) == doctest::Approx(1.0));
        CHECK(p.rotation.x == 0.0);
        CHECK(p.rotation.z == 0.0);
        CHECK(p.position == placed[k].p);
        CHECK(p.tags.size() == 4);
    }
    CHECK(furnitureAsset(json(), "bench") == "/Game/Settlement/Furniture/SM_Bench");

    // Medieval: lanterns, a well and market stalls on the square, no signals, hydrants or signs.
    const Layout ml = build(Site{.size = 60000.0}, Ground::plane(), styled("medievalTown"));
    const RoadNetwork mnet = roadNetwork(ml, Ground::plane());
    std::map<std::string, int> medieval;
    for (const FurniturePlacement& p : placeFurniture(ml, mnet, Ground::plane(), furnitureSets(true)))
        ++medieval[p.kind];
    CHECK(medieval["lantern"] > 0);
    CHECK(medieval["well"] == 1);
    CHECK(medieval["marketStall"] > 4);
    CHECK(medieval["cart"] > 0);
    CHECK(medieval["signal"] == 0);
    CHECK(medieval["hydrant"] == 0);
    CHECK(medieval["stopSign"] == 0);
}

TEST_CASE("settlement streetscape: street and district names are stable by id, overridable, and follow the culture")
{
    // A grid, whose streets keep their ids through unrelated edits, named in Old English.
    Site site{.size = 120000.0};
    site.culture = "oldEnglish";
    const Options o = styled("usTown", false);
    const Layout a = build(site, Ground::plane(), o);
    CHECK(a.culture == "oldEnglish");
    const std::map<uint64_t, std::string> before = namesById(a);
    REQUIRE(before.size() > 10);
    std::set<std::string> distinct;
    for (const auto& [id, name] : before)
    {
        CHECK_FALSE(name.empty());
        distinct.insert(name);
    }
    CHECK(distinct.size() > 5);
    REQUIRE(!a.districts.empty());
    for (const District& d : a.districts)
    {
        CHECK_FALSE(d.name.empty());
        CHECK(d.area > 0.0);
    }
    CHECK(namesById(build(site, Ground::plane(), o)) == before); // every run

    // Another street's profile changed and one street renamed: every other street keeps its name.
    auto it = before.begin();
    const uint64_t changed = it->first, renamed = std::next(it, 3)->first;
    Site edited = site;
    edited.overrides.push_back({.street = changed, .profile = "street"});
    edited.overrides.push_back({.street = renamed, .name = "Pilgrims Way"});
    const Layout b = build(edited, Ground::plane(), o);
    const std::map<uint64_t, std::string> after = namesById(b);
    CHECK(streetName(b, renamed) == "Pilgrims Way");
    int kept = 0, compared = 0;
    for (const auto& [id, name] : before)
    {
        if (!after.contains(id) || name == before.at(renamed))
            continue;
        ++compared;
        kept += after.at(id) == name;
    }
    CHECK(compared > 10);
    CHECK(kept == compared);
    for (size_t k = 0; k < a.districts.size() && k < b.districts.size(); ++k)
    {
        CHECK(b.districts[k].id == a.districts[k].id);
        CHECK(b.districts[k].name == a.districts[k].name);
    }

    // An exclusion in one corner leaves almost every name where it was.
    Site added = site;
    added.exclusions.push_back({1, "", "", {{40000, 40000}, {58000, 40000}, {58000, 58000}, {40000, 58000}}, 0.0});
    const std::map<uint64_t, std::string> withFar = namesById(build(added, Ground::plane(), o));
    int same = 0, shared = 0;
    for (const auto& [id, name] : before)
        if (withFar.contains(id))
        {
            ++shared;
            same += withFar.at(id) == name;
        }
    REQUIRE(shared > 10);
    CHECK(double(same) / shared > 0.9);

    // Cultures: French words first, numbered US grid streets, sector codes; an unknown culture is an error.
    Site french = site;
    french.culture = "french";
    bool frenchWord = true;
    for (const auto& [id, name] : namesById(build(french, Ground::plane(), o)))
        frenchWord &= name.starts_with("Rue") || name.starts_with("Avenue") || name.starts_with("Boulevard") || name.starts_with("Ruelle") ||
                      name.starts_with("Passage") || name.starts_with("Impasse") || name.starts_with("All") || name.starts_with("Sentier") ||
                      name.starts_with("Escalier");
    CHECK(frenchWord);
    const Layout grid = build(Site{.size = 120000.0}, Ground::plane(), o);
    CHECK(grid.culture == "usGrid");
    int numbered = 0;
    for (const auto& [id, name] : namesById(grid))
        numbered += name.find("1st") != std::string::npos || name.find("2nd") != std::string::npos || name.find("3rd") != std::string::npos ||
                    name.find("th Street") != std::string::npos || name.find("th Avenue") != std::string::npos;
    CHECK(numbered > 5);
    Site sector = site;
    sector.culture = "sector";
    for (const auto& [id, name] : namesById(build(sector, Ground::plane(), o)))
        CHECK((name.starts_with("Trunk") || name.starts_with("Corridor") || name.starts_with("Spur") || name.starts_with("Duct") ||
               name.starts_with("Walkway")));
    Site bad = site;
    bad.culture = "klingon";
    CHECK_THROWS(build(bad, Ground::plane(), o));
    CHECK(toJson(a)["streets"][0].contains("name"));
    CHECK(toJson(a)["districts"].size() == a.districts.size());
}

TEST_CASE("settlement streetscape: lane graph is strongly connected per mode, greens never conflict, transit routes are reachable")
{
    const Layout l = build(Site{.size = 160000.0}, Ground::plane(), styled("europeanCity", false));
    const RoadNetwork net = roadNetwork(l, Ground::plane());
    LaneGraph g = laneGraph(l, net);
    int vehicle = 0, pedestrian = 0;
    for (const Lane& x : g.lanes)
        (x.mode == LaneMode::Vehicle ? vehicle : pedestrian)++;
    CHECK(vehicle > 50);
    CHECK(pedestrian > 50);
    std::map<Movement, int> moves;
    for (const LaneLink& k : g.links)
        ++moves[k.move];
    CHECK(moves[Movement::Through] > 0);
    CHECK(moves[Movement::Left] > 0);
    CHECK(moves[Movement::Right] > 0);
    CHECK(moves[Movement::Corner] > 0);

    addTransit(g, l, net);
    int bus = 0, tram = 0;
    for (const TransitRoute& r : g.routes)
    {
        INFO(r.name, " ", r.note);
        CHECK(r.reachable);
        CHECK(r.stops.size() >= 2);
        CHECK(r.length > 0.0);
        bus += r.mode == "bus";
        tram += r.mode == "tram";
        for (const TransitStop& s : r.stops)
            CHECK(std::ranges::find(r.lanes, s.lane) != r.lanes.end());
    }
    CHECK(bus >= 2);
    CHECK(tram == 2); // one route, out and back

    const json c = checkLaneGraph(g, net);
    INFO(c.dump());
    CHECK(c["modes"]["vehicle"]["components"] == 1);
    CHECK(c["modes"]["pedestrian"]["components"] == 1);
    CHECK(c["modes"]["vehicle"]["cannotReachExit"].empty());
    CHECK(c["conflictingGreens"].empty());
    CHECK(c["unphasedApproaches"].empty());
    CHECK(c["ok"] == true);
    CHECK(checkLanes(l, Ground::plane())["ok"] == true);

    // Junction phases are the ones the lanes move in, extra skewed-junction phases included, and every crosswalk walks
    // in a phase that holds the traffic over it.
    int walksChecked = 0;
    for (const LaneLink& k : g.links)
    {
        if (k.junction < 0 || k.phase < 0)
            continue;
        const Junction& j = net.junctions[size_t(k.junction)];
        REQUIRE(size_t(k.phase) < j.phases.size());
        const Lane& from = g.lanes[size_t(k.from)];
        if (from.mode == LaneMode::Vehicle)
        {
            bool listed = false;
            for (int e : j.phases[size_t(k.phase)])
                listed |= j.ends[size_t(e)].line == from.line && j.ends[size_t(e)].start == (from.dir < 0);
            CHECK(listed);
        }
        else if (k.move == Movement::Crosswalk)
            ++walksChecked;
    }
    CHECK(walksChecked > 0);
    for (const Junction& j : net.junctions)
    {
        if (j.control != Control::Signal)
            continue;
        REQUIRE(j.walks.size() == j.phases.size());
        for (size_t w = 0; w < j.crosswalks.size(); ++w)
        {
            bool walks = false;
            for (size_t p = 0; p < j.phases.size(); ++p)
                if (std::ranges::find(j.walks[p], int(w)) != j.walks[p].end())
                {
                    walks = true;
                    CHECK(std::ranges::find(j.phases[p], j.crosswalks[w].end) == j.phases[p].end());
                }
            CHECK(walks);
        }
    }

    // Every movement of a signalised junction given green together, none yielding, is caught.
    LaneGraph bad = laneGraph(l, net);
    int junction = -1;
    for (size_t j = 0; j < net.junctions.size() && junction < 0; ++j)
        if (net.junctions[j].control == Control::Signal && net.junctions[j].phases.size() >= 2)
            junction = int(j);
    REQUIRE(junction >= 0);
    for (LaneLink& k : bad.links)
        if (k.junction == junction && k.phase >= 0)
        {
            k.phase = 0;
            k.yields = false;
        }
    const json cb = checkLaneGraph(bad, net);
    CHECK_FALSE(cb["conflictingGreens"].empty());
    CHECK(cb["ok"] == false);

    // A one-way street off a two-way one, cut off at a dead end: its lanes cannot get out.
    Options o = styled("europeanCity", false);
    o.profile.pattern = Pattern::None;
    Site oneWay{.size = 160000.0};
    SitePath main;
    main.id = 2;
    main.cls = StreetClass::Collector;
    main.pts = {{-30000, std::nan(""), 3000}, {30000, std::nan(""), 3000}};
    oneWay.paths.push_back(main);
    SitePath spur;
    spur.id = 1;
    spur.cls = StreetClass::Local;
    spur.profile = "spur";
    spur.pts = {{3000, std::nan(""), 3000}, {3000, std::nan(""), -9000}};
    oneWay.paths.push_back(spur);
    RoadProfile p;
    REQUIRE(builtinRoadProfile("street", &p));
    p.name = "spur";
    p.oneWay = true;
    oneWay.roadProfiles.push_back(p);
    const Layout lw = build(oneWay, Ground::plane(), o);
    const json cw = checkLanes(lw, Ground::plane(), {.busRoutes = 0, .tramRoutes = 0});
    CHECK(cw["ok"] == false);
    CHECK(cw["modes"]["vehicle"]["components"].get<int>() > 1);

    // Medieval styles run no buses or trams by default.
    const Layout ml = build(Site{.size = 60000.0}, Ground::plane(), styled("medievalTown", false));
    const RoadNetwork mnet = roadNetwork(ml, Ground::plane());
    LaneGraph mg = laneGraph(ml, mnet);
    addTransit(mg, ml, mnet);
    CHECK(mg.routes.empty());
    CHECK(checkLaneGraph(mg, mnet)["modes"]["vehicle"]["components"] == 1);
}

TEST_CASE("settlement streetscape: commands mesh roads, place furniture, name streets and districts, undo, and report lanes")
{
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    run("newScene");
    const json made = run("settlementCreate", {{"profile", "usTown"}, {"size", 80000.0}, {"blocks", false}});
    const std::string mesh = made["mesh"].get<std::string>();

    const json roads = run("settlementRoadMesh", {{"args", {mesh}}});
    CHECK(roads["builder"] == "own");
    CHECK(roads["areaM2"]["roads"].get<double>() > 1000.0);
    CHECK(roads["curbReturns"].get<int>() > 0);
    CHECK(roads["meshes"].size() == 3);
    CHECK(run("objExists", {{"args", {roads["meshes"][0]}}}) == true);
    const json again = run("settlementRoadMesh", {{"args", {mesh}}, {"markings", "none"}});
    CHECK(again["meshes"].size() == 2);
    CHECK(again["crosswalks"].empty());

    const json f = run("settlementFurniture", {{"args", {mesh}}});
    CHECK(f["count"].get<int>() > 20);
    CHECK(f["byKind"]["lamp"].get<int>() > 0);
    CHECK(f["byKind"]["signal"].get<int>() > 0);
    CHECK(run("settlementFurniture", {{"args", {mesh}}})["placements"] == f["placements"]);
    CHECK(run("settlementFurniture", {{"args", {mesh}}, {"kinds", {"lamp"}}})["byKind"].size() == 1);
    CHECK(f["layoutPlacements"] == f["count"]);
    const json stored = run("settlementFurniture", {{"args", {mesh}}, {"store", true}, {"sets", {{"lane", json::array()}, {"street", json::array()}}},
                                                    {"engineAssets", {{"lamp", "/Game/City/SM_Lamp"}}}});
    CHECK(stored["count"].get<int>() < f["count"].get<int>());
    CHECK(stored["layoutPlacements"] == stored["count"]);
    run("undo");
    CHECK(run("settlementFurniture", {{"args", {mesh}}})["count"] == f["count"]);

    const json names = run("settlementNames", {{"args", {mesh}}});
    CHECK(names["culture"] == "usGrid");
    REQUIRE(names["streets"].size() > 4);
    const json french = run("settlementNames", {{"args", {mesh}}, {"culture", "french"}});
    CHECK(french["culture"] == "french");
    const uint64_t id = french["streets"][0]["id"].get<uint64_t>();
    run("settlementEditStreet", {{"args", {mesh}}, {"id", id}, {"name", "Rue Neuve"}});
    auto nameOf = [&](uint64_t street) {
        for (const json& s : run("settlementStreets", {{"args", {mesh}}}))
            if (s["id"].get<uint64_t>() == street)
                return s["name"].get<std::string>();
        return std::string();
    };
    CHECK(nameOf(id) == "Rue Neuve");
    run("undo");
    CHECK(nameOf(id) == french["streets"][0]["name"].get<std::string>());
    run("undo");
    CHECK(run("settlementNames", {{"args", {mesh}}})["culture"] == "usGrid");
    const uint64_t district = names["districts"][0]["id"].get<uint64_t>();
    const json renamed = run("settlementNames", {{"args", {mesh}}, {"district", district}, {"name", "Old Town"}});
    bool found = false;
    for (const json& d : renamed["districts"])
        found |= d["id"].get<uint64_t>() == district && d["name"] == "Old Town";
    CHECK(found);
    CHECK_THROWS(run("settlementNames", {{"args", {mesh}}, {"culture", "klingon"}}));

    std::random_device rd;
    const auto path = std::filesystem::temp_directory_path() / ("lila_settlement_names_" + std::to_string(rd()) + ".json");
    run("settlementExport", {{"args", {mesh}}, {"path", path.string()}});
    std::ifstream in(path);
    const json exported = json::parse(in);
    in.close();
    CHECK(exported["streets"][0].contains("name"));
    CHECK(exported["districts"].size() == renamed["districts"].size());
    std::error_code ec;
    std::filesystem::remove(path, ec);

    const json lanes = run("settlementLanes", {{"args", {mesh}}});
    CHECK(lanes["checks"]["ok"] == true);
    CHECK(lanes["lanes"]["vehicle"].get<int>() > 0);
    const json transit = run("settlementTransit", {{"args", {mesh}}, {"busRoutes", 1}});
    CHECK(transit["routes"].size() >= 2);
    CHECK(transit["unreachable"].empty());
}

