// Settlement export: World Partition cells, live-sync diffs, GeoJSON, USD, DXF and the engine folders.

#include "TestUtil.h"
#include "Export.h"

#include "curve/Dxf.h"
#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <set>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
Layout exportTown(const char* profile = "usTown", double size = 60000.0)
{
    Site s;
    s.size = size;
    Options o;
    REQUIRE(builtinProfile(profile, &o.profile));
    o.seed = 3;
    return build(s, Ground::plane(), o);
}

/** Six placements of three assets at known spots, at 12800: three in cell (0, 0), one each in (1, 0), (-1, -1) and (2, 3). */
std::vector<Placement> exportPlacements()
{
    const json list = json::array({
        {{"id", "bench_1"}, {"asset", "/Game/Props/SM_Bench"}, {"position", {100, 0, 100}}},
        {{"id", "bench_2"}, {"asset", "/Game/Props/SM_Bench"}, {"position", {12000, 0, 500}}, {"tags", {"furniture"}}},
        {{"id", "lamp_1"}, {"asset", "/Game/Props/SM_Lamp"}, {"position", {200, 0, 200}}, {"scale", 2.0}},
        {{"id", "lamp_2"}, {"asset", "/Game/Props/SM_Lamp"}, {"position", {13000, 0, 100}}, {"rotation", {0, 0.7071068, 0, 0.7071068}}},
        {{"id", "house_1"}, {"asset", "/Game/Buildings/BP_House"}, {"position", {-100, 0, -100}}},
        {{"id", "house_2"}, {"asset", "/Game/Buildings/BP_House"}, {"position", {25700, 0, 38500}}},
    });
    return placementsFrom(list);
}

size_t countOf(const std::string& text, std::string_view what)
{
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size()))
        ++n;
    return n;
}

void loadSettlementForExport()
{
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
}
} // namespace

TEST_CASE("settlement export: roads are cut at cell borders and everything lands in the cell it stands in")
{
    const glm::dvec3 line[] = {{-100, 0, 50}, {25700, 0, 50}};
    const auto pieces = cutByCells(line, 12800.0);
    REQUIRE(pieces.size() == 4);
    CHECK(pieces[0].first == Cell{-1, 0});
    CHECK(pieces[1].first == Cell{0, 0});
    CHECK(pieces[2].first == Cell{1, 0});
    CHECK(pieces[3].first == Cell{2, 0});
    CHECK(pieces[1].second.front().x == doctest::Approx(0.0));
    CHECK(pieces[1].second.back().x == doctest::Approx(12800.0));

    Layout l = exportTown();
    l.placements = exportPlacements();
    REQUIRE(l.placements.size() == 6);
    CHECK(l.placements[2].scale.y == doctest::Approx(2.0));
    CHECK(l.placements[1].tags == std::vector<std::string>{"furniture"});
    const RoadNetwork net = roadNetwork(l, Ground::plane());
    const double size = 12800.0;
    const auto cells = partition(l, net, size);
    size_t lots = 0, envelopes = 0, junctions = 0, placements = 0;
    double pieceLength = 0.0;
    for (const auto& [cell, c] : cells)
    {
        lots += c.lots.size();
        envelopes += c.envelopes.size();
        junctions += c.junctions.size();
        placements += c.placements.size();
        for (int k : c.lots)
            CHECK(cellAt(centroid(l.lots[size_t(k)].outline), size) == cell);
        for (const RoadPiece& p : c.roads)
            for (size_t i = 0; i + 1 < p.pts.size(); ++i)
            {
                pieceLength += glm::length(p.pts[i + 1] - p.pts[i]);
                const glm::dvec3 mid = 0.5 * (p.pts[i] + p.pts[i + 1]);
                CHECK(cellAt({mid.x, mid.z}, size) == cell);
            }
    }
    double lineLength = 0.0;
    for (const StreetLine& s : net.lines)
        for (size_t i = 0; i + 1 < s.pts.size(); ++i)
            lineLength += glm::length(s.pts[i + 1] - s.pts[i]);
    CHECK(lots == l.lots.size());
    CHECK(envelopes == l.envelopes.size());
    CHECK(junctions == net.junctions.size());
    CHECK(placements == 6);
    CHECK(pieceLength == doctest::Approx(lineLength).epsilon(1e-9));
    CHECK(cells.at(Cell{0, 0}).placements.size() == 3);
    CHECK(cells.at(Cell{1, 0}).placements.size() == 1);
    CHECK(cells.at(Cell{-1, -1}).placements.size() == 1);
    CHECK(cells.at(Cell{2, 3}).placements.size() == 1);
    // A 600 m town on 128 m cells spans a 5 x 5 grid at least.
    CHECK(cells.size() >= 25);

    // Placement items group by cell and asset: bench and lamp share (0, 0), so two groups there.
    SyncOptions o;
    o.key = "Town";
    const auto items = syncItems(l, net, cells, o);
    size_t place = 0, pcg = 0, massingItems = 0;
    std::set<std::string> keys;
    for (const SyncItem& i : items)
    {
        CHECK(keys.insert(i.key).second);
        place += i.kind == "place";
        pcg += i.kind == "pcg";
        massingItems += i.kind == "massing";
    }
    CHECK(place == 5);
    CHECK(keys.contains("Town/place/0_0/Game_Props_SM_Bench"));
    CHECK(keys.contains("Town/place/0_0/Game_Props_SM_Lamp"));
    CHECK(keys.contains("Town/roads"));
    CHECK(keys.contains("Town/zoneGraph"));
    size_t withData = 0, withEnvelopes = 0;
    for (const auto& [cell, c] : cells)
    {
        withData += !c.lots.empty() || !c.envelopes.empty() || !c.roads.empty() || !c.junctions.empty() || !c.parking.empty();
        withEnvelopes += !c.envelopes.empty();
    }
    CHECK(pcg == withData);
    CHECK(massingItems == withEnvelopes);

    // Massing per cell holds every envelope's prism: n walls plus top and bottom each.
    size_t faces = 0, expected = 0;
    for (const auto& [cell, m] : massingByCell(l, size))
        faces += size_t(m.numFaces());
    for (const Envelope& e : l.envelopes)
        expected += e.footprint.size() + 2;
    CHECK(faces == expected);
    CHECK(size_t(massing(l).numFaces()) == expected);
}

TEST_CASE("settlement export: live sync resends only the cells whose ids changed and removes what is gone")
{
    Layout l = exportTown();
    l.placements = exportPlacements();
    const Ground ground = Ground::plane();
    RoadNetwork net = roadNetwork(l, ground);
    SyncOptions o;
    o.key = "Town";
    auto cells = partition(l, net, 12800.0);
    const auto first = syncItems(l, net, cells, o);
    const json manifest = syncManifest(first, elementHashes(l, net));

    // The same layout again: nothing to send.
    const SyncDiff same = diffSync(manifest, first, elementHashes(l, net));
    CHECK(same.added.empty());
    CHECK(same.changed.empty());
    CHECK(same.removed.empty());
    CHECK(same.unchanged.size() == first.size());
    CHECK(same.changedIds.empty());

    // Rebuilding the layout reproduces every id and hash.
    {
        Layout again = exportTown();
        again.placements = exportPlacements();
        const RoadNetwork net2 = roadNetwork(again, ground);
        const SyncDiff d = diffSync(manifest, syncItems(again, net2, partition(again, net2, 12800.0), o), elementHashes(again, net2));
        CHECK(d.changed.empty());
        CHECK(d.changedIds.empty());
        CHECK(d.removedIds.empty());
    }

    // Move one lamp within its cell, drop one house, drop one lot and its envelope.
    REQUIRE(!l.envelopes.empty());
    l.placements[3].position.x += 50.0;
    l.placements.erase(l.placements.begin() + 5);
    const int lotId = l.envelopes.front().lot;
    const auto lotIt = std::ranges::find_if(l.lots, [&](const Lot& x) { return x.id == lotId; });
    REQUIRE(lotIt != l.lots.end());
    const std::string goneLot = lotKey(*lotIt);
    const Cell lotCell = cellAt(centroid(lotIt->outline), 12800.0);
    const Cell envelopeCell = cellAt(centroid(l.envelopes.front().footprint), 12800.0);
    l.lots.erase(lotIt);
    l.envelopes.erase(l.envelopes.begin());

    net = roadNetwork(l, ground);
    cells = partition(l, net, 12800.0);
    const auto second = syncItems(l, net, cells, o);
    const SyncDiff d = diffSync(manifest, second, elementHashes(l, net));
    CHECK(d.changedIds == std::vector<std::string>{"place:lamp_2"});
    const std::set<std::string> removedIds(d.removedIds.begin(), d.removedIds.end());
    CHECK(removedIds.size() == 3);
    CHECK(removedIds.contains("place:house_2"));
    CHECK(removedIds.contains(goneLot));
    CHECK(removedIds.contains("envelope:" + goneLot.substr(4)));
    CHECK(d.addedIds.empty());
    CHECK(d.removed == std::vector<std::string>{"Town/place/2_3/Game_Buildings_BP_House"});
    // Only the lamp's group, the PCG data of the lot's and envelope's cells and the envelope cell's massing change.
    const std::set<std::string> changed(d.changed.begin(), d.changed.end());
    const std::set<std::string> expected = {"Town/place/1_0/Game_Props_SM_Lamp", "Town/pcg/" + lotCell.name(), "Town/pcg/" + envelopeCell.name(),
                                            "Town/massing/" + envelopeCell.name()};
    CHECK(changed == expected);
    CHECK(d.changed.size() == changed.size());
    CHECK(d.unchanged.size() + d.changed.size() + d.added.size() == second.size());
    CHECK(d.unchanged.size() > second.size() / 2);

    // Changing a setting every item hashes resends them all.
    SyncOptions moved = o;
    moved.settings = "folder=/Game/Elsewhere";
    const SyncDiff all = diffSync(syncManifest(second, elementHashes(l, net)), syncItems(l, net, cells, moved), elementHashes(l, net));
    CHECK(all.changed.size() == second.size());

    // A road-only setting (a landscape paint layer) resends the ground roads alone.
    SyncOptions repainted = o;
    repainted.roadSettings = "roadLayer=roadAsphalt";
    const SyncDiff roads = diffSync(syncManifest(second, elementHashes(l, net)), syncItems(l, net, cells, repainted), elementHashes(l, net));
    CHECK(roads.changed == std::vector<std::string>{"Town/roads"});
    CHECK(roads.added.empty());
    CHECK(roads.removed.empty());
}

TEST_CASE("settlement export: GeoJSON holds every street, block, lot, envelope, lane and placement")
{
    Layout l = exportTown();
    l.placements = exportPlacements();
    const RoadNetwork net = roadNetwork(l, Ground::plane());
    const json g = toGeoJson(l, net, "Town");
    CHECK(g["type"] == "FeatureCollection");
    std::map<std::string, size_t> layers;
    for (const json& f : g["features"])
    {
        REQUIRE(f["type"] == "Feature");
        const std::string type = f["geometry"]["type"];
        const json& c = f["geometry"]["coordinates"];
        ++layers[f["properties"]["layer"].get<std::string>()];
        if (type == "Point")
            CHECK(c.size() == 2);
        else if (type == "LineString")
            CHECK(c.size() >= 2);
        else
        {
            REQUIRE(type == "Polygon");
            const json& ring = c[0];
            REQUIRE(ring.size() >= 4);
            CHECK(ring.front() == ring.back());
            // RFC 7946: exterior rings counter-clockwise.
            double area = 0.0;
            for (size_t k = 0; k + 1 < ring.size(); ++k)
                area += ring[k][0].get<double>() * ring[k + 1][1].get<double>() - ring[k + 1][0].get<double>() * ring[k][1].get<double>();
            CHECK(area > 0.0);
        }
    }
    size_t streets = 0;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        streets += l.graph.isStreet(int(e));
    CHECK(layers["street"] == streets);
    CHECK(layers["junction"] == net.junctions.size());
    CHECK(layers["block"] == l.blocks.size());
    CHECK(layers["lot"] == l.lots.size());
    CHECK(layers["envelope"] == l.envelopes.size());
    CHECK(layers["lane"] == laneLines(l, net).size());
    CHECK(layers["placement"] == 6);
    CHECK(layers["lane"] > 0);
    // A two-way US street has a lane each way.
    size_t forward = 0, backward = 0;
    for (const LaneLine& ln : laneLines(l, net))
        (ln.forward ? forward : backward) += 1;
    CHECK(forward > 0);
    CHECK(backward > 0);
    CHECK(json::parse(g.dump()) == g);
}

TEST_CASE("settlement export: USD holds a prim per street, lane, junction, block, lot, envelope and placement")
{
    Layout l = exportTown();
    l.placements = exportPlacements();
    const Ground ground = Ground::plane();
    const RoadNetwork net = roadNetwork(l, ground);
    const std::string usd = toUsda(l, ground, net, "Town");
    CHECK(usd.starts_with("#usda 1.0"));
    size_t roads = 0;
    for (const StreetLine& s : net.lines)
        roads += s.pts.size() >= 2;
    CHECK(countOf(usd, "def BasisCurves ") == roads + laneLines(l, net).size());
    CHECK(countOf(usd, "def Mesh ") == l.blocks.size() + l.lots.size() + l.envelopes.size());
    CHECK(countOf(usd, "def Xform ") == 1 + net.junctions.size() + 6);
    CHECK(countOf(usd, "def Scope ") == 7);
    CHECK(countOf(usd, "{") == countOf(usd, "}"));
    CHECK(countOf(usd, "custom string lila:use") == l.lots.size() + l.envelopes.size() + l.blocks.size());
    CHECK(countOf(usd, "custom string lila:asset = \"/Game/Props/SM_Lamp\"") == 2);
}

TEST_CASE("settlement export: the DXF plan reads back with a curve per street, lot and building")
{
    Layout l = exportTown();
    const Ground ground = Ground::plane();
    const RoadNetwork net = roadNetwork(l, ground);
    auto layers = dxfLayers(l, ground, net, 100.0);
    std::map<std::string, size_t> counts;
    std::vector<curve::DxfObject> objects;
    for (auto& [name, curves] : layers)
    {
        counts[name] = curves.size();
        objects.push_back({.layer = name, .curves = std::move(curves)});
    }
    CHECK(counts["Lots"] == l.lots.size());
    CHECK(counts["Buildings"] == l.envelopes.size());
    CHECK(counts["Blocks"] == l.blocks.size());
    CHECK(counts["Lanes"] == laneLines(l, net).size());
    CHECK(counts["Streets"] > 0);
    curve::DxfWriteOptions o;
    o.unit = "m";
    const curve::DxfDocument doc = curve::readDxf(curve::writeDxf(objects, o));
    REQUIRE(doc.ok());
    CHECK(doc.unitScale == doctest::Approx(100.0));
    std::map<std::string, size_t> read;
    for (const curve::DxfLayer& layer : doc.layers)
        read[layer.name] += layer.curves.size();
    CHECK(read == counts);
}

TEST_CASE("settlement export: commands write every format and the engine folders carry their importers")
{
    loadSettlementForExport();
    run("newScene");
    const json made = run("settlementCreate", {{"profile", "usTown"}, {"size", 40000.0}, {"seed", 2}});
    const json info = run("settlementInfo", {{"args", {made["mesh"]}}});
    std::random_device rd;
    const auto dir = std::filesystem::temp_directory_path() / ("lila_settlement_export_" + std::to_string(rd()));
    const json placements = json::array({{{"id", "a"}, {"asset", "/Game/SM_A"}, {"position", {0, 0, 0}}}, {{"id", "b"}, {"asset", "/Game/SM_A"}, {"position", {500, 0, 0}}}});

    const json geo = run("settlementExport", {{"args", {made["mesh"]}}, {"path", (dir / "town.geojson").string()}, {"placements", placements}});
    CHECK(geo["format"] == "geojson");
    CHECK(geo["layers"]["lot"] == info["lots"]);
    CHECK(geo["layers"]["envelope"] == info["buildings"]);
    // The layout carries its own furniture placements; the flag adds two more.
    const json base = run("settlementExport", {{"args", {made["mesh"]}}, {"path", (dir / "base.geojson").string()}});
    CHECK(geo["layers"]["placement"].get<int>() == base["layers"].value("placement", 0) + 2);
    const json usd = run("settlementExport", {{"args", {made["mesh"]}}, {"path", (dir / "town.usda").string()}});
    CHECK(usd["prims"].get<int>() > info["lots"].get<int>());
    const json dxf = run("settlementExport", {{"args", {made["mesh"]}}, {"path", (dir / "town.dxf").string()}});
    CHECK(dxf["layers"]["Lots"] == info["lots"]);
    const json js = run("settlementExport", {{"args", {made["mesh"]}}, {"path", (dir / "town.json").string()}});
    CHECK(js["lots"] == info["lots"]);
    CHECK_THROWS(run("settlementExport", {{"args", {made["mesh"]}}, {"path", (dir / "town.usdc").string()}}));
    CHECK_THROWS(run("settlementExport", {{"args", {made["mesh"]}}, {"path", (dir / "x.json").string()}, {"placements", "[{\"asset\": \"/Game/A\"}]"}}));

    const json unity = run("settlementExport", {{"args", {made["mesh"]}}, {"path", (dir / "unity").string()}, {"format", "unity"}});
    CHECK(unity["warnings"].empty());
    CHECK(std::filesystem::exists(unity["files"]["importer"].get<std::string>()));
    CHECK(std::filesystem::exists(unity["files"]["massing"].get<std::string>()));
    CHECK(std::filesystem::exists(unity["files"]["streets"].get<std::string>()));
    std::ifstream uf(unity["files"]["data"].get<std::string>());
    const json ud = json::parse(uf);
    CHECK(ud["target"] == "unity");
    CHECK(ud["lots"].size() == info["lots"].get<size_t>());
    CHECK(ud["meshes"]["massing"].get<std::string>().ends_with("_Massing.fbx"));

    const json godot = run("settlementExport", {{"args", {made["mesh"]}}, {"path", (dir / "godot").string()}, {"format", "godot"}});
    CHECK(godot["warnings"].empty());
    std::ifstream gs(godot["files"]["importer"].get<std::string>());
    const std::string script((std::istreambuf_iterator<char>(gs)), std::istreambuf_iterator<char>());
    CHECK(script.find("%SETTLEMENT_JSON%") == std::string::npos);
    CHECK(script.find(std::filesystem::path(godot["files"]["data"].get<std::string>()).filename().string()) != std::string::npos);

    // A dry run computes cells and the diff without an editor.
    const json dry = run("settlementSendToUnreal", {{"args", {made["mesh"]}}, {"dryRun", true}, {"roads", "meshes"}, {"placements", placements}});
    CHECK(dry["dryRun"] == true);
    CHECK(dry["cells"].get<int>() >= 9);
    CHECK(dry["diff"]["removed"].empty());
    CHECK(dry["diff"]["added"].size() + dry["diff"]["changed"].size() + dry["diff"]["unchanged"].get<size_t>() == dry["items"].get<size_t>());
    CHECK(dry["kinds"]["place"].get<int>() >= 1);
    CHECK(dry["kinds"]["roads"].get<int>() >= 9);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
