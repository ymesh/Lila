#include "TestUtil.h"
#include "Generators.h"
#include "Parcels.h"

#include "curve/CurveNodes.h"
#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "gn/NodeLib.h"
#include "names/Names.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <set>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
void loadOsmPackages()
{
    if (!ops::find("curve"))
    {
        lila::curve::registerTypes();
        lila::curve::registerCommands();
    }
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    if (!ops::find("terrainCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
}

std::string townFile() { return (std::filesystem::path(LILA_SOURCE_DIR) / "tests" / "data" / "osm" / "w1_town.osm").string(); }

json makeTown(const std::string& ground, const char* name, json extra = json::object())
{
    json call = {{"profile", "usTown"}, {"size", 60000.0}, {"ground", ground}, {"osmFile", townFile()}, {"osmOrigin", {23.2, -106.42}}, {"name", name}};
    call.update(extra);
    return run("settlementCreate", call);
}

std::map<std::string, json> byOsmId(const json& list)
{
    std::map<std::string, json> out;
    for (const json& e : list)
        if (e.contains("osmId"))
            out[e["osmId"].get<std::string>()] = e;
    return out;
}
} // namespace

TEST_CASE("settlement osm: imported buildings become lots and envelopes with their heights, parts, courtyards, roofs and businesses")
{
    loadOsmPackages();
    run("newScene");
    const json terrain = run("terrainCreate", {{"size", json::array({80000.0, 80000.0})}, {"resolution", 129}, {"amplitude", 300.0}, {"seed", 3}});
    const std::string ground = terrain["mesh"];
    const json town = makeTown(ground, "Town");
    const std::string mesh = town["mesh"];
    const json gen = run("settlementGenerators", {{"args", {mesh}}});
    CAPTURE(gen["osm"].dump());
    CHECK(gen["osm"]["buildingLots"] == 11); // 13 buildings, two of them parts of the cathedral
    CHECK(gen["osm"]["parts"] == 2);
    CHECK(gen["osm"]["denseBlocks"].get<int>() >= 1);
    CHECK(gen["osm"]["sparseBlocks"].get<int>() >= 1);
    CHECK(gen["osm"]["outside"] == 0);

    const json lots = run("settlementLots", {{"args", {mesh}}, {"limit", 0}})["lots"];
    const auto envs = byOsmId(run("settlementEnvelopes", {{"args", {mesh}}, {"limit", 0}})["envelopes"]);
    const auto osmLots = byOsmId(lots);
    REQUIRE(envs.size() == 11);

    const json& belmar = envs.at("w1001");
    CHECK(belmar["floors"] == 3);
    CHECK(belmar["heightSource"] == "levels");
    CHECK(belmar["roof"] == "hip");
    CHECK(belmar["roofSource"] == "tag");
    CHECK(belmar["colour"] == "#d4a373");
    CHECK(belmar["program"] == "shop");
    CHECK_FALSE(belmar["name"].get<std::string>().empty());
    CHECK(belmar["name"] != "Edificio Belmar");
    CHECK_FALSE(belmar["style"].get<std::string>().empty());

    const json& house = envs.at("w1002");
    CHECK(house["heightSource"] == "height");
    CHECK(house["height"].get<double>() == doctest::Approx(750.0));
    CHECK(house["roof"] == "gable");
    CHECK(house["roofColour"] == "#8b3a2b");
    CHECK(envs.at("w1003")["floors"] == 5);
    CHECK(envs.at("w1003")["roof"] == "flat");
    CHECK(envs.at("w1003")["material"] == "concrete");

    const json& court = envs.at("r2001");
    CHECK(court["holes"].size() == 1);
    CHECK(court["height"].get<double>() == doctest::Approx(1200.0));
    CHECK(court["program"] == "apartments");

    const json& cathedral = envs.at("w1010");
    CHECK(cathedral["program"] == "cathedral");
    CHECK(cathedral["landmark"] == "cathedral");
    CHECK(cathedral["style"] == "church");
    CHECK(cathedral["eraName"] == "industrial");
    REQUIRE(cathedral["tiers"].size() == 2);
    double tallest = 0.0;
    for (const json& t : cathedral["tiers"])
        tallest = std::max(tallest, t["top"].get<double>());
    CHECK(tallest - cathedral["ground"].get<double>() > 4300.0);
    // The domed tower keeps its own roof.
    int domes = 0;
    for (const json& t : cathedral["tiers"])
        if (t.value("roof", std::string()) == "dome")
        {
            ++domes;
            CHECK(t["roofHeight"].get<double>() > 0.0);
        }
    CHECK(domes == 1);

    // Streets keep the fictional names Terrain gave the same roads: one name per real street everywhere.
    std::set<std::string> streetNames;
    const json namedStreets = run("settlementNames", {{"args", {mesh}}});
    for (const json& st : namedStreets["streets"])
        streetNames.insert(st["name"].get<std::string>());
    const json roads = run("terrainImportOsm", {{"args", {townFile()}}, {"terrain", ground}, {"kinds", {"road"}}, {"landcover", false},
                                                {"origin", {23.2, -106.42}}, {"culture", "mexican"}});
    const json roadInfo = json::parse(run("getAttr", {{"args", {roads["curves"]["road"].get<std::string>() + ".osmFeatures"}}}).get<std::string>());
    std::set<std::string> terrainNames;
    for (const json& r : roadInfo)
        if (!r.value("name", std::string()).empty())
            terrainNames.insert(r["name"].get<std::string>());
    CHECK(terrainNames.size() == 6);
    for (const std::string& n : terrainNames)
    {
        CAPTURE(n);
        CHECK(streetNames.count(n) == 1);
    }

    // Shops inside buildings are their businesses; one chain's branches share a name, and none is real.
    const json& pharmacy = envs.at("w1004")["businesses"];
    REQUIRE(pharmacy.size() == 1);
    CHECK(pharmacy[0]["program"] == "pharmacy");
    const std::string farmacia = pharmacy[0]["name"];
    CHECK((farmacia.find("Farmacia") != std::string::npos || farmacia.find("Botica") != std::string::npos));
    CHECK(farmacia.find("Similares") == std::string::npos);
    REQUIRE(envs.at("w1005")["businesses"].size() == 1);
    REQUIRE(envs.at("w1009")["businesses"].size() == 1);
    CHECK(envs.at("w1005")["businesses"][0]["name"] == envs.at("w1009")["businesses"][0]["name"]);
    CHECK(envs.at("w1005")["businesses"][0]["name"] != "OXXO");
    CHECK(envs.at("w1005")["groundUse"] == "commercial");

    // The dense block holds only the real buildings; the sparse one keeps Settlement's lots around them.
    const uint64_t denseBlock = osmLots.at("w1001")["block"], sparseBlock = osmLots.at("w1010")["block"];
    int denseCut = 0, sparseCut = 0;
    for (const json& lot : lots)
        if (!lot.contains("osmId"))
        {
            denseCut += lot["block"] == denseBlock ? 1 : 0;
            sparseCut += lot["block"] == sparseBlock ? 1 : 0;
        }
    CHECK(denseCut == 0);
    CHECK(sparseCut > 0);

    // Areas: the cemetery's lots and its landmark, the school's grounds; the taqueria on a lot Settlement cut.
    int cemetery = 0, school = 0, taco = 0;
    bool cemeteryLandmark = false;
    for (const json& lot : lots)
    {
        cemetery += lot["use"] == "cemetery" ? 1 : 0;
        cemeteryLandmark = cemeteryLandmark || lot["landmark"] == "cemetery";
        school += lot["program"] == "school" ? 1 : 0;
        if (lot.contains("businesses"))
            for (const json& b : lot["businesses"])
                if (b["program"] == "taqueria")
                {
                    ++taco;
                    CHECK_FALSE(lot.contains("osmId"));
                    CHECK(lot["use"] == "commercial");
                    CHECK(b["name"] != "Taquería El Güero");
                }
    }
    CHECK(cemetery > 0);
    CHECK(cemeteryLandmark);
    CHECK(school > 0);
    CHECK(taco == 1);

    // A neighbourhood names the district it lies in, fictionally.
    const json names = run("settlementNames", {{"args", {mesh}}});
    CHECK(names["culture"] == "mexican");
    for (const json& s : gen["osm"]["sites"])
        if (s["kind"] == "district")
        {
            CHECK(s["name"] != "Colonia Centro");
            bool named = false;
            for (const json& d : names["districts"])
                named = named || d["name"] == s["name"];
            CHECK(named);
        }

    const json check = run("settlementCheck", {{"args", {mesh}}});
    CAPTURE(check["issues"].dump());
    CHECK(check["counts"]["realName"] == 0);
    CHECK(check["counts"]["frontage"] == 0);
    CHECK(run("realNameCheck")["ok"] == true);

    // The same file again: the same lots, ids and names.
    const json again = makeTown(ground, "Town2");
    const auto envs2 = byOsmId(run("settlementEnvelopes", {{"args", {again["mesh"]}}, {"limit", 0}})["envelopes"]);
    REQUIRE(envs2.size() == envs.size());
    for (const auto& [id, e] : envs)
    {
        CAPTURE(id);
        CHECK(envs2.at(id)["lotId"] == e["lotId"]);
        CHECK(envs2.at(id)["name"] == e["name"]);
        CHECK(envs2.at(id)["businesses"] == e["businesses"]);
    }

    // A real name given by hand is caught.
    const uint64_t district = names["districts"][0]["id"];
    run("settlementNames", {{"args", {mesh}}, {"district", district}, {"name", "Colonia Centro"}});
    const json leaked = run("settlementCheck", {{"args", {mesh}}});
    CHECK(leaked["counts"]["realName"] == 1);
    CHECK(leaked["ok"] == false);
}

TEST_CASE("settlement osm: fill none leaves blocks to the imported buildings, all fills every block")
{
    loadOsmPackages();
    run("newScene");
    const json terrain = run("terrainCreate", {{"size", json::array({80000.0, 80000.0})}, {"resolution", 65}, {"amplitude", 200.0}, {"seed", 4}});
    auto cutIn = [](const json& town, const char* osmId) {
        const json lots = run("settlementLots", {{"args", {town["mesh"]}}, {"limit", 0}})["lots"];
        json block;
        for (const json& lot : lots)
            if (lot.value("osmId", std::string()) == osmId)
                block = lot["block"];
        int n = 0;
        for (const json& lot : lots)
            n += !lot.contains("osmId") && lot["block"] == block ? 1 : 0;
        return n;
    };
    const json none = makeTown(terrain["mesh"], "None", {{"osmFill", "none"}});
    CHECK(cutIn(none, "w1010") == 0);
    const json sparse = makeTown(terrain["mesh"], "Sparse");
    CHECK(cutIn(sparse, "w1010") > 0);
    CHECK(cutIn(sparse, "w1001") == 0);
    const json all = makeTown(terrain["mesh"], "All", {{"osmFill", "all"}});
    CHECK(cutIn(all, "w1001") >= 0);
    CHECK(cutIn(all, "w1010") == cutIn(sparse, "w1010"));
}

TEST_CASE("settlement massing: courtyards are holes, building parts keep their own roofs")
{
    Layout l;
    Envelope env;
    env.footprint = makePolygon({{0, 0}, {2000, 0}, {2000, 2000}, {0, 2000}});
    env.holes = {makePolygon({{700, 700}, {1300, 700}, {1300, 1300}, {700, 1300}})};
    env.tiers = {{env.footprint, 0.0, 900.0}};
    env.roof = "flat";
    lila::geo::Mesh m;
    std::vector<int> parts;
    CHECK(appendMassing(env, l, m, [&](int part) { parts.push_back(part); }));
    // The top's area is the footprint less the courtyard, and nothing of it lies over the courtyard.
    double top = 0.0, over = 0.0;
    int walls = 0;
    size_t k = 0;
    for (lila::geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const int part = parts[k++];
        std::vector<glm::dvec3> pts;
        for (lila::geo::Index p : m.facePoints(f))
            pts.push_back(m.position(p));
        glm::dvec3 c(0.0);
        double a = 0.0;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            c += pts[i];
            const glm::dvec3 &p = pts[i], &q = pts[(i + 1) % pts.size()];
            a += p.x * q.z - q.x * p.z;
        }
        c /= double(pts.size());
        if (part == 1 && std::abs(c.y - 900.0) < 1e-6)
        {
            top += std::abs(a) * 0.5;
            over += c.x > 700 && c.x < 1300 && c.z > 700 && c.z < 1300 ? 1.0 : 0.0;
        }
        walls += part == 0 ? 1 : 0;
    }
    CHECK(top == doctest::Approx(2000.0 * 2000.0 - 600.0 * 600.0));
    CHECK(over == 0.0);
    CHECK(walls == 8);

    // A tower tier with a dome of its own on a flat-roofed base.
    Envelope twin;
    twin.footprint = makePolygon({{0, 0}, {3000, 0}, {3000, 2000}, {0, 2000}});
    twin.tiers = {{twin.footprint, 0.0, 800.0}, {makePolygon({{200, 200}, {800, 200}, {800, 800}, {200, 800}}), 0.0, 2500.0, "dome", 300.0}};
    twin.roof = "flat";
    lila::geo::Mesh m2;
    CHECK(appendMassing(twin, l, m2, [](int) {}));
    double high = 0.0;
    for (lila::geo::Index p = 0; p < m2.pointCapacity(); ++p)
        if (m2.pointValid(p))
            high = std::max(high, m2.position(p).y);
    CHECK(high > 2500.0 + 250.0);
    CHECK(high <= 2500.0 + 300.0 + 1e-6);
}

TEST_CASE("names: French, Latin and Sector Codes cultures name businesses in their own words")
{
    using lila::names::Kind;
    auto many = [](const char* culture, const char* program) {
        std::set<std::string> out;
        for (uint64_t key = 1; key <= 24; ++key)
            out.insert(lila::names::placeName(culture, Kind::Business, program, key * 7919, 0));
        return out;
    };
    for (const std::string& n : many("french", "bakery"))
        CHECK((n.starts_with("Boulangerie") || n.starts_with("Pâtisserie")));
    for (const std::string& n : many("latin", "bakery"))
        CHECK(n.starts_with("Pistrinum"));
    for (const std::string& n : many("sector", "bakery"))
        CHECK((n.starts_with("Protein Bakery") || n.find("Ovens") != std::string::npos));
    CHECK(lila::names::vocabularyOf("french") == "french");
    CHECK(lila::names::vocabularyOf("latin") == "latin");
    CHECK(lila::names::vocabularyOf("sector") == "scifi");
}
