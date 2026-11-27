#include "TestUtil.h"
#include "Osm.h"

#include "names/Names.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

using namespace lila;
using namespace terraintest;
using namespace terrain;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
constexpr double kTownLat = 23.2, kTownLon = -106.42;

fs::path fixture(const char* name) { return fs::path(LILA_SOURCE_DIR) / "tests" / "data" / "osm" / name; }

const OsmBuilding* building(const OsmExtract& x, int64_t id)
{
    for (const OsmBuilding& b : x.buildings)
        if (b.id == id)
            return &b;
    return nullptr;
}

const OsmPlace* place(const OsmExtract& x, const std::string& program)
{
    for (const OsmPlace& p : x.places)
        if (p.program == program)
            return &p;
    return nullptr;
}

int kindCount(const OsmExtract& x, OsmFeatureKind k)
{
    return int(std::count_if(x.features.begin(), x.features.end(), [&](const OsmFeature& f) { return f.kind == k; }));
}

/** Every name a curve's osmFeatures and osmPlaces hold. */
std::vector<std::string> namesOn(const std::string& curve)
{
    std::vector<std::string> out;
    for (const char* attr : {"osmFeatures", "osmPlaces"})
    {
        json v;
        try
        {
            v = run("getAttr", {{"args", {curve + "." + attr}}});
        }
        catch (const std::exception&)
        {
            continue;
        }
        for (const json& i : json::parse(v.get<std::string>()))
            if (i.contains("name") && i["name"].is_string() && !i["name"].get<std::string>().empty())
                out.push_back(i["name"]);
    }
    return out;
}
} // namespace

TEST_CASE("terrain osm places: buildings, parts, courtyards, places and harbour works read alike from xml, json and pbf")
{
    const OsmExtract xml = extractOsm(readOsm(fixture("w1_town.osm")));
    const OsmExtract js = extractOsm(readOsm(fixture("w1_town.json")));
    const OsmExtract pbf = extractOsm(readOsm(fixture("w1_town.osm.pbf")));
    for (const OsmExtract* x : {&xml, &js, &pbf})
    {
        CAPTURE(x == &xml ? "xml" : x == &js ? "json" : "pbf");
        // Nine houses, the cathedral and its two parts, the courtyard block.
        CHECK(x->buildings.size() == 13);
        const OsmBuilding* belmar = building(*x, 1001);
        REQUIRE(belmar);
        CHECK(belmar->levels == 3);
        CHECK(belmar->roofShape == "hipped");
        CHECK(belmar->colour == "#d4a373");
        CHECK(belmar->program == "shop");
        CHECK(std::ranges::find(belmar->realNames, "Edificio Belmar") != belmar->realNames.end());
        const OsmBuilding* house = building(*x, 1002);
        REQUIRE(house);
        CHECK(house->height == doctest::Approx(7.5));
        CHECK(house->roofColour == "#8b3a2b");
        CHECK(building(*x, 1003)->material == "concrete");
        const OsmBuilding* cathedral = building(*x, 1010);
        REQUIRE(cathedral);
        CHECK(cathedral->program == "cathedral");
        CHECK(cathedral->kind == "religious");
        CHECK(cathedral->startDate == "1856");
        const OsmBuilding* tower = building(*x, 1011);
        REQUIRE(tower);
        CHECK(tower->part);
        CHECK(tower->height == doctest::Approx(45.0));
        CHECK(building(*x, 1012)->minHeight == doctest::Approx(0.0));
        const OsmBuilding* court = building(*x, 2001);
        REQUIRE(court);
        CHECK(court->type == 'r');
        CHECK(court->holes.size() == 1);
        CHECK(court->outer.size() == 4);
        CHECK(court->height == doctest::Approx(12.0));
        // The relation's outer way is the building, not a second one.
        CHECK_FALSE(building(*x, 1013));

        const OsmPlace* taco = place(*x, "taqueria");
        REQUIRE(taco);
        CHECK(taco->type == 'n');
        CHECK(taco->rings.empty());
        CHECK(place(*x, "pharmacy"));
        CHECK(place(*x, "pharmacy")->chain == "farmacias similares");
        const OsmPlace* cemetery = place(*x, "cemetery");
        REQUIRE(cemetery);
        CHECK(cemetery->kind == "cemetery");
        CHECK(cemetery->rings.size() == 1);
        CHECK(place(*x, "school")->kind == "school");
        CHECK(place(*x, "town")->kind == "town");
        CHECK(place(*x, "district")->kind == "district");
        CHECK(place(*x, "beach")->kind == "beach");
        CHECK(std::count_if(x->places.begin(), x->places.end(), [](const OsmPlace& p) { return p.program == "grocery"; }) == 2);

        CHECK(kindCount(*x, OsmFeatureKind::Pier) == 1);
        CHECK(kindCount(*x, OsmFeatureKind::Breakwater) == 1);
        CHECK(kindCount(*x, OsmFeatureKind::Quay) == 1);
        CHECK(kindCount(*x, OsmFeatureKind::Dam) == 1);
        CHECK(kindCount(*x, OsmFeatureKind::Road) == 6);
    }

    // A warp before import reaches every position: features, areas, buildings with their holes, and places.
    OsmExtract moved = xml;
    size_t visited = 0;
    forEachPoint(moved, [&](LatLon& p) {
        p.lat += 1.0;
        ++visited;
    });
    CHECK(visited > 100);
    CHECK(building(moved, 2001)->holes[0][0].lat == doctest::Approx(building(xml, 2001)->holes[0][0].lat + 1.0));
    CHECK(place(moved, "taqueria")->at.lat == doctest::Approx(place(xml, "taqueria")->at.lat + 1.0));
}

TEST_CASE("terrain osm places: programs from tags")
{
    CHECK(osmProgram({{"amenity", "restaurant"}, {"cuisine", "tacos"}}).first == "taqueria");
    CHECK(osmProgram({{"amenity", "restaurant"}, {"cuisine", "seafood"}}).first == "seafood");
    CHECK(osmProgram({{"shop", "car_parts"}}).first == "carParts");
    CHECK(osmProgram({{"shop", "tyres"}}).first == "tyres");
    CHECK(osmProgram({{"shop", "convenience"}}).first == "grocery");
    CHECK(osmProgram({{"landuse", "greenhouse_horticulture"}}).first == "greenhouse");
    CHECK(osmProgram({{"landuse", "farmland"}}).first == "farm");
    CHECK(osmProgram({{"landuse", "industrial"}}).first == "factory");
    CHECK(osmProgram({{"aeroway", "aerodrome"}}).second == "airport");
    CHECK(osmProgram({{"harbour", "yes"}}).first == "port");
    CHECK(osmProgram({{"man_made", "lighthouse"}}).first == "lighthouse");
    CHECK(osmProgram({{"office", "government"}}).second == "government");
    CHECK(osmProgram({{"building", "house"}}).second == "building");
    CHECK(osmProgram({{"highway", "residential"}}).second.empty());
}

TEST_CASE("terrain osm places: every imported name is fictional, stable across imports, and the real ones only in the hidden source")
{
    loadTerrain();
    run("newScene");
    const json t = run("terrainCreate", {{"size", {80000.0, 80000.0}}, {"resolution", 129}, {"amplitude", 500.0}, {"name", "Town"}});
    const std::string mesh = t["mesh"];
    const json o = run("terrainImportOsm", {{"args", {fixture("w1_town.osm").string()}}, {"terrain", mesh}, {"origin", {kTownLat, kTownLon}}});
    CAPTURE(o.dump());
    CHECK(o["culture"] == "mexican");
    CHECK(o["cultureFrom"].get<std::string>().find("Mexico") != std::string::npos);
    CHECK(o["features"]["building"] == 13);
    CHECK(o["features"]["pier"] == 1);
    CHECK(o["named"].get<int>() > 10);
    CHECK(fs::exists(o["attributionFile"].get<std::string>()));

    const std::string buildings = o["curves"]["building"], places = o["curves"]["place"], roads = o["curves"]["road"];
    const json binfo = json::parse(run("getAttr", {{"args", {buildings + ".osmFeatures"}}}).get<std::string>());
    // Outer rings and the courtyard's hole: one spline each, the hole pointing at its outer.
    CHECK(binfo.size() == 14);
    const auto inner = std::find_if(binfo.begin(), binfo.end(), [](const json& i) { return i.value("ring", "") == "inner"; });
    REQUIRE(inner != binfo.end());
    CHECK(binfo[(*inner)["outer"].get<size_t>()]["id"] == (*inner)["id"]);
    const auto tower = std::find_if(binfo.begin(), binfo.end(), [](const json& i) { return i.value("id", "") == "w1011"; });
    REQUIRE(tower != binfo.end());
    CHECK((*tower)["height"].get<double>() == doctest::Approx(4500.0));
    CHECK((*tower)["part"] == true);

    // No real name anywhere in the output: not a business, a chain, a street, a river, a town.
    std::vector<std::string> output;
    for (const std::string& c : {buildings, places, roads})
        for (const std::string& n : namesOn(c))
            output.push_back(n);
    for (const char* k : {"river", "dam", "pier", "lake"})
        if (o["curves"].contains(k))
            for (const std::string& n : namesOn(o["curves"][k]))
                output.push_back(n);
    CHECK(output.size() > 12);
    const std::set<std::string> real = {"Edificio Belmar", "Farmacias Similares", "OXXO", "Catedral Basílica de la Inmaculada Concepción",
                                        "Casa del Marino", "Taquería El Güero", "Mazatlán", "Colonia Centro", "Panteón Municipal Número Uno",
                                        "Escuela Primaria Benito Juárez", "Avenida Olas Altas", "Calle Benito Juárez", "Río Presidio",
                                        "Muelle Fiscal", "Presa Picachos", "Playa Olas Altas", "Presa Sanalona"};
    for (const std::string& n : output)
    {
        CAPTURE(n);
        CHECK_FALSE(real.count(n));
        CHECK(n.find("Similares") == std::string::npos);
        CHECK(n.find("OXXO") == std::string::npos);
    }
    // The real names stay only in the hidden attribute, which the editors don't show.
    const json source = json::parse(run("getAttr", {{"args", {places + ".sourceNames"}}}).get<std::string>());
    bool keptReal = false;
    for (const auto& [k, v] : source.items())
        for (const json& n : v)
            keptReal = keptReal || n == "Taquería El Güero";
    CHECK(keptReal);
    scene::Node* placeNode = scene::Graph::current().find(places);
    REQUIRE(placeNode);
    const int src = placeNode->type().attrIndex("sourceNames");
    REQUIRE(src >= 0);
    CHECK((placeNode->type().attrs[size_t(src)].flags & scene::AttrHidden) != 0);

    // A taqueria is named as one; the two branches of one chain share a fictional chain name.
    const json points = json::parse(run("getAttr", {{"args", {places + ".osmPlaces"}}}).get<std::string>());
    std::string taco, oxxo1, oxxo2;
    for (const json& p : points)
    {
        if (p["program"] == "taqueria")
            taco = p["name"].get<std::string>();
        if (p["program"] == "grocery")
            (oxxo1.empty() ? oxxo1 : oxxo2) = p["name"].get<std::string>();
    }
    CAPTURE(taco);
    CHECK((taco.find("Taquer") != std::string::npos || taco.find("Tacos") != std::string::npos));
    CHECK_FALSE(oxxo1.empty());
    CHECK(oxxo1 == oxxo2);

    // The same import again names everything the same (read before the second import reuses the curve names).
    const std::vector<std::string> firstBuildings = namesOn(buildings), firstPlaces = namesOn(places);
    const json again = run("terrainImportOsm", {{"args", {fixture("w1_town.json").string()}}, {"terrain", mesh}, {"origin", {kTownLat, kTownLon}},
                                                {"kinds", {"building", "place"}}, {"name", "Again"}});
    CHECK(namesOn(again["curves"]["building"]) == firstBuildings);
    std::vector<std::string> a = namesOn(again["curves"]["place"]), b = firstPlaces;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    CHECK(a == b);
    // Another culture names them otherwise, in its own words.
    const json english = run("terrainImportOsm", {{"args", {fixture("w1_town.osm").string()}}, {"terrain", mesh}, {"origin", {kTownLat, kTownLon}},
                                                  {"kinds", {"place"}}, {"culture", "usGrid"}, {"name", "English"}});
    CHECK(english["culture"] == "usGrid");
    CHECK(english["cultureFrom"] == "given");
    CHECK(namesOn(english["curves"]["place"]) != firstPlaces);
    CHECK_THROWS(run("terrainImportOsm", {{"args", {fixture("w1_town.osm").string()}}, {"terrain", mesh}, {"origin", {kTownLat, kTownLon}}, {"culture", "klingon"}}));
}

TEST_CASE("terrain osm places: realNameCheck catches a real name that leaks into the output")
{
    loadTerrain();
    run("newScene");
    const json t = run("terrainCreate", {{"size", {80000.0, 80000.0}}, {"resolution", 65}, {"amplitude", 500.0}, {"name", "Town"}});
    const json o = run("terrainImportOsm", {{"args", {fixture("w1_town.osm.pbf").string()}}, {"terrain", t["mesh"]}, {"origin", {kTownLat, kTownLon}}});
    const json clean = run("realNameCheck");
    CAPTURE(clean.dump());
    CHECK(clean["ok"] == true);
    CHECK(clean["realNames"].get<int>() >= 17);
    CHECK(clean["blocklist"].get<int>() > 1000);

    // A real business name written back into the output.
    const std::string places = o["curves"]["place"];
    json points = json::parse(run("getAttr", {{"args", {places + ".osmPlaces"}}}).get<std::string>());
    for (json& p : points)
        if (p["program"] == "taqueria")
            p["name"] = "Taquería El Güero";
    run("setAttr", {{"args", {places + ".osmPlaces", points.dump()}}});
    json leak = run("realNameCheck");
    CHECK(leak["ok"] == false);
    REQUIRE(leak["leaks"].size() == 1);
    CHECK(leak["leaks"][0]["real"] == "Taquería El Güero");
    CHECK(leak["leaks"][0]["where"].get<std::string>().find("osmPlaces") != std::string::npos);

    // A node named after a real street; a sign text naming a real brand from the blocklist, accents and case aside.
    run("group", {{"empty", true}, {"name", "Avenida_Olas_Altas"}});
    leak = run("realNameCheck", {{"texts", {{{"where", "sign:12"}, {"text", "FARMACIAS similares 24h"}}, "Abarrotes Doña Lupe"}}});
    std::set<std::string> found;
    for (const json& l : leak["leaks"])
        found.insert(l["real"].get<std::string>());
    CHECK(found.count("Avenida Olas Altas"));
    CHECK(found.count("Farmacias Similares"));
    CHECK(leak["leaks"].size() == 3);
    // Generic words alone are no real name.
    CHECK(run("realNameCheck", {{"texts", {"Farmacia", "Calle", "Escuela Primaria"}}, {"scene", false}})["ok"] == true);
}

TEST_CASE("terrain osm places: names that are not distinctive only match whole")
{
    names::RealNames r;
    r.add("Escuela Primaria", "osm");
    r.add("Taquería El Güero", "osm");
    r.add("Sol", "osm");
    CHECK(r.find("Escuela Primaria"));
    CHECK_FALSE(r.find("Escuela Primaria Las Palmas"));
    CHECK(r.find("taqueria el guero"));
    CHECK(r.find("La nueva Taquería El Güero"));
    CHECK(r.find("SOL"));
    CHECK_FALSE(r.find("Playa del Sol"));
    const names::NameHashes h = r.hashes();
    CHECK(h.matches("La nueva Taquería El Güero"));
    CHECK_FALSE(h.matches("Playa del Sol"));
    CHECK(names::NameHashes::fromJson(h.toJson()).matches("TAQUERIA EL GUERO"));
}

TEST_CASE("terrain osm places: fictional names are stable, varied, never real, and use the culture's words")
{
    auto real = std::make_shared<names::RealNames>();
    real->add("Farmacia San Rafael", "test");
    names::NameBook book("mexican", real);
    names::NameBook again("mexican", real);
    std::set<std::string> pharmacies;
    for (uint64_t k = 0; k < 40; ++k)
    {
        const std::string n = book.place(names::Kind::Business, "pharmacy", k);
        CHECK(n == again.place(names::Kind::Business, "pharmacy", k));
        CHECK(n != "Farmacia San Rafael");
        CHECK((n.find("Farmacia") != std::string::npos || n.find("Botica") != std::string::npos));
        CHECK_FALSE(names::brandBlocklist().find(n));
        pharmacies.insert(n);
    }
    CHECK(pharmacies.size() == 40);
    const std::string parts = book.place(names::Kind::Business, "carParts", 7);
    CHECK((parts.find("Refacc") != std::string::npos || parts.find("Autopartes") != std::string::npos));
    CHECK(book.place(names::Kind::River, "", 3).starts_with("Río"));
    const std::string cemetery = book.place(names::Kind::Cemetery, "", 3);
    CHECK((cemetery.find("Pante") != std::string::npos || cemetery.find("Cementerio") != std::string::npos));
    CHECK(book.district(5).starts_with("Colonia "));
    CHECK(names::cultureForLocation(24.8, -107.4) == "mexican");
    CHECK(names::cultureForLocation(51.5, -0.1) == "oldEnglish");
    for (const std::string& c : names::cultures())
        for (const std::string& p : names::programs())
            CHECK_FALSE(names::placeName(c, names::Kind::Business, p, 11, 0).empty());
}
