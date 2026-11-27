// Zones: point queries and priority, painted masks, the check's gaps and overlaps, outlines and convex pieces, the engine
// JSON both ways, and the commands end to end (districts from a Settlement, masks from a Terrain, undo, export and import).

#include "Zones.h"

#include "extensions/ExtensionManager.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/ViewOverlays.h"
#include "ui/ViewportHooks.h"
#include "OpenWorldTest.h"

#include <QGuiApplication>

#include <chrono>
#include <glm/gtc/matrix_transform.hpp>
#include <doctest/doctest.h>

#include <filesystem>
#include <random>

using namespace openworld;
using namespace openworld::zones;
using json = nlohmann::json;

namespace
{
using owtest::run;
Polygon square(double x0, double z0, double x1, double z1) { return {{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}}; }

Zone zoneOf(std::string id, Polygon p, int priority = 0, json attrs = json::object())
{
    Zone z;
    z.id = std::move(id);
    z.name = z.id;
    z.priority = priority;
    z.attrs = std::move(attrs);
    z.polygons = {std::move(p)};
    return z;
}

const LayerType& builtIn(const std::string& id)
{
    for (const LayerType& t : builtInTypes())
        if (t.id == id)
            return t;
    FAIL("no type " << id);
    return builtInTypes()[0];
}

} // namespace

TEST_CASE("zones: point query takes the highest priority, then the later zone, with defaults filled in")
{
    ZoneSpec s;
    Layer police = makeLayer(builtIn("police"), "police");
    police.zones.push_back(zoneOf("city", square(0, 0, 10000, 10000), 0, {{"agency", "municipal"}}));
    police.zones.push_back(zoneOf("state", square(5000, 0, 20000, 10000), 0, {{"agency", "state"}}));
    police.zones.push_back(zoneOf("hq", square(6000, 1000, 7000, 2000), 5, {{"agency", "federal"}, {"responseTime", 10}}));
    s.layers.push_back(police);

    // Overlap at equal priority: the later zone wins.
    json q = queryLayer(s.layers[0], {6000, 5000});
    CHECK(q["zone"] == "state");
    CHECK(q["all"] == json({"state", "city"}));
    // Higher priority wins wherever it is.
    q = queryLayer(s.layers[0], {6500, 1500});
    CHECK(q["zone"] == "hq");
    CHECK(q["attrs"]["responseTime"] == 10);
    // Defaults fill what the zone leaves out.
    q = queryLayer(s.layers[0], {1000, 1000});
    CHECK(q["zone"] == "city");
    CHECK(q["attrs"]["responseTime"] == 60);
    CHECK(q["attrs"]["station"] == "");
    // Outside every zone.
    q = queryLayer(s.layers[0], {-100, -100});
    CHECK(q["zone"].is_null());
    CHECK(q["all"].empty());

    // Holes cut a zone out.
    Zone ring = zoneOf("ring", square(0, 0, 1000, 1000));
    ring.holes = {square(400, 400, 600, 600)};
    CHECK(weight(ring, {200, 200}) == 1.0);
    CHECK(weight(ring, {500, 500}) == 0.0);

    // Layers answer independently.
    Layer wanted = makeLayer(builtIn("wanted"), "wanted");
    wanted.zones.push_back(zoneOf("base", square(-50000, -50000, 50000, 50000), 0, {{"level", 2}}));
    s.layers.push_back(wanted);
    const json all = query(s, {6500, 1500});
    CHECK(all["police"]["zone"] == "hq");
    CHECK(all["wanted"]["attrs"]["level"] == 2);
}

TEST_CASE("zones: painted masks, density weights and the spec's JSON round trip")
{
    ZoneSpec s;
    s.maskCell = 200.0;
    Layer traffic = makeLayer(builtIn("traffic"), "traffic");
    Zone z;
    z.id = "downtown";
    z.name = "Downtown";
    z.mask.cell = s.maskCell;
    z.attrs = {{"density", 0.8}};
    paint(z.mask, {.center = {0, 0}, .radius = 3000, .value = 0.5, .strength = 1.0, .softness = 0.0});
    paint(z.mask, {.center = {-20000, 0}, .radius = 1000, .value = 1.0, .strength = 1.0, .softness = 0.0});
    traffic.zones.push_back(z);
    s.layers.push_back(traffic);

    // Tiles on both sides of the origin.
    CHECK(z.mask.tiles.size() >= 4);
    json q = queryLayer(s.layers[0], {100, 100});
    CHECK(q["zone"] == "downtown");
    CHECK(q["weight"].get<double>() == doctest::Approx(0.5).epsilon(0.01));
    CHECK(q["density"].get<double>() == doctest::Approx(0.4).epsilon(0.01));
    CHECK(queryLayer(s.layers[0], {-20000, 0})["density"].get<double>() == doctest::Approx(0.8).epsilon(0.01));
    CHECK(queryLayer(s.layers[0], {10000, 0})["density"].get<double>() == 0.0);

    // Erasing takes it back to nothing, and empty tiles go.
    Mask m = z.mask;
    paint(m, {.center = {-20000, 0}, .radius = 2000, .strength = 1.0, .softness = 0.0, .erase = true});
    CHECK(m.at({-20000, 0}) == 0.0);
    CHECK(m.tiles.size() < z.mask.tiles.size());

    // A region layer counts from half up.
    Layer territory = makeLayer(builtIn("territory"), "territory");
    Zone t = z;
    t.attrs = {{"faction", "Los Tigres"}};
    territory.zones.push_back(t);
    s.layers.push_back(territory);
    CHECK(queryLayer(s.layers[1], {100, 100})["zone"] == "downtown");
    paint(s.layers[1].zones[0].mask, {.center = {0, 0}, .radius = 3000, .value = 0.3, .strength = 1.0, .softness = 0.0});
    CHECK(queryLayer(s.layers[1], {100, 100})["zone"].is_null());

    // Round trip: same text back, same answers.
    s.playArea = {square(-30000, -30000, 30000, 30000)};
    s.types.push_back(typeFromJson({{"id", "gangTurf"}, {"label", "Gang Turf"}, {"defaults", {{"gang", ""}}}}));
    Layer turf = makeLayer(*s.type("gangTurf"), "turf");
    turf.zones.push_back(zoneOf("a", square(0, 0, 100, 100)));
    s.layers.push_back(turf);
    const json j = toJson(s);
    const ZoneSpec back = zonesFromJson(json::parse(j.dump()));
    CHECK(toJson(back) == j);
    CHECK(queryLayer(back.layers[0], {100, 100})["density"].get<double>() == doctest::Approx(0.4).epsilon(0.01));
    CHECK(back.layers[2].type == "gangTurf");

    // Bad specs name the field.
    CHECK_THROWS_WITH_AS(zonesFromJson({{"layers", {{{"type", "nope"}}}}}), doctest::Contains("layers[0].type"), std::runtime_error);
    CHECK_THROWS_WITH_AS(zonesFromJson({{"layers", {{{"type", "police"}, {"zones", {{{"id", "a"}}, {{"id", "a"}}}}}}}}), doctest::Contains("used twice"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(zonesFromJson({{"bogus", 1}}), doctest::Contains("unknown key"), std::runtime_error);
}

TEST_CASE("zones: the check catches gaps in must-cover layers and overlaps in exclusive ones")
{
    ZoneSpec s;
    Layer d = makeLayer(builtIn("district"), "districts");
    REQUIRE(d.mustCover);
    REQUIRE(d.exclusive);
    // Two districts with a 2 km gap between them and a 1 km overlap with a third.
    d.zones.push_back(zoneOf("west", square(0, 0, 40000, 100000)));
    d.zones.push_back(zoneOf("east", square(60000, 0, 100000, 100000)));
    d.zones.push_back(zoneOf("north", square(38000, 0, 62000, 20000)));
    s.layers.push_back(d);
    Layer terr = makeLayer(builtIn("territory"), "territory");
    terr.zones.push_back(zoneOf("a", square(0, 0, 50000, 50000)));
    terr.zones.push_back(zoneOf("b", square(25000, 25000, 75000, 75000)));
    s.layers.push_back(terr);

    const std::vector<Polygon> play = {square(0, 0, 100000, 100000)};
    const std::vector<Issue> issues = check(s, play, 500.0);
    int gaps = 0, overlaps = 0;
    for (const Issue& i : issues)
    {
        CHECK(i.layer == "districts");
        if (i.kind == "gap")
        {
            ++gaps;
            CHECK(i.at.x > 40000);
            CHECK(i.at.x < 60000);
            CHECK(i.at.y > 20000);
            CHECK(i.areaM2 == doctest::Approx(20000.0 * 80000.0 / 10000.0).epsilon(0.03));
        }
        if (i.kind == "overlap")
        {
            ++overlaps;
            CHECK(i.areaM2 == doctest::Approx(2000.0 * 20000.0 / 10000.0).epsilon(0.1));
            CHECK(i.zones.size() == 2);
            CHECK(std::ranges::find(i.zones, "north") != i.zones.end());
        }
    }
    CHECK(gaps == 1);
    CHECK(overlaps == 2);

    // Filling the gap and pulling the north district back clears it; territory overlaps are allowed (contested).
    s.layers[0].zones[2].polygons = {square(40000, 0, 60000, 100000)};
    CHECK(check(s, play, 500.0).empty());

    // A play area larger than the districts shows the margin as a gap; a zone with no region is reported.
    Zone empty;
    empty.id = "nothing";
    empty.name = "Nothing";
    s.layers[1].zones.push_back(empty);
    const std::vector<Issue> more = check(s, {square(-10000, 0, 100000, 100000)}, 500.0);
    CHECK(std::ranges::count_if(more, [](const Issue& i) { return i.kind == "gap"; }) == 1);
    CHECK(std::ranges::count_if(more, [](const Issue& i) { return i.kind == "empty"; }) == 1);
}

TEST_CASE("zones: mask outlines and convex pieces")
{
    Mask m;
    m.cell = 100.0;
    // An L of cells with a hole-free corner, and a separate block.
    for (int z = 0; z < 10; ++z)
        for (int x = 0; x < 10; ++x)
            if (x < 3 || z < 3)
                m.setCell(x, z, 255);
    for (int z = 20; z < 24; ++z)
        for (int x = 20; x < 24; ++x)
            m.setCell(x, z, 255);
    // A ring with a hole.
    for (int z = 40; z < 50; ++z)
        for (int x = 40; x < 50; ++x)
            if (!(x >= 43 && x < 47 && z >= 43 && z < 47))
                m.setCell(x, z, 255);
    std::vector<Polygon> rings, holes;
    maskOutline(m, 128, 1.0, rings, holes);
    REQUIRE(rings.size() == 3);
    REQUIRE(holes.size() == 1);
    double total = 0.0;
    for (const Polygon& r : rings)
        total += area(r);
    CHECK(total == doctest::Approx((51.0 + 16.0 + 100.0) * 100.0 * 100.0));
    CHECK(std::abs(area(holes[0])) == doctest::Approx(16.0 * 100.0 * 100.0));

    // The L as convex pieces: every piece convex, areas summing to the L's.
    const Polygon L = {{0, 0}, {1000, 0}, {1000, 300}, {300, 300}, {300, 1000}, {0, 1000}};
    const std::vector<Polygon> pieces = convexPieces(L);
    CHECK(pieces.size() >= 2);
    CHECK(pieces.size() <= 4);
    double sum = 0.0;
    for (const Polygon& p : pieces)
    {
        sum += area(p);
        for (size_t i = 0; i < p.size(); ++i)
        {
            const Vec2 a = p[i], b = p[(i + 1) % p.size()], c = p[(i + 2) % p.size()];
            CHECK((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) >= -1e-6);
        }
    }
    CHECK(sum == doctest::Approx(area(L)));
    // Clockwise input comes back counter-clockwise.
    Polygon cw = L;
    std::reverse(cw.begin(), cw.end());
    double sumCw = 0.0;
    for (const Polygon& p : convexPieces(cw))
        sumCw += area(p);
    CHECK(sumCw == doctest::Approx(area(L)));

    // A square with two holes, one of them an L: the pieces cover the square less the holes, and none lies in a hole.
    const Polygon square = {{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}};
    const std::vector<Polygon> squareHoles = {{{200, 200}, {400, 200}, {400, 400}, {200, 400}},
                                              {{550, 550}, {900, 550}, {900, 650}, {650, 650}, {650, 900}, {550, 900}}};
    double holed = 0.0;
    for (const Polygon& p : convexPieces(square, squareHoles))
    {
        holed += area(p);
        for (size_t i = 0; i < p.size(); ++i)
        {
            const Vec2 a = p[i], b = p[(i + 1) % p.size()], c = p[(i + 2) % p.size()];
            CHECK((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) >= -1e-6);
        }
        Vec2 mid(0.0);
        for (Vec2 q : p)
            mid += q / double(p.size());
        CHECK(!(mid.x > 200 && mid.x < 400 && mid.y > 200 && mid.y < 400));
    }
    CHECK(holed == doctest::Approx(1000.0 * 1000.0 - 200.0 * 200.0 - std::abs(area(squareHoles[1]))));
    CHECK(convexPieces(square, {}).size() == 1);
}

TEST_CASE("zones: convex pieces of a long traced ring stay quick and exact")
{
    // A comb: 600 teeth, 2,400 corners, the shape a traced boat lane's mask gives.
    Polygon comb;
    const int teeth = 600;
    comb.push_back({0, 0});
    comb.push_back({teeth * 200.0, 0});
    for (int t = teeth - 1; t >= 0; --t)
    {
        const double x = t * 200.0;
        comb.push_back({x + 150.0, 100.0});
        comb.push_back({x + 150.0, 400.0 + (t % 7) * 30.0});
        comb.push_back({x + 50.0, 400.0 + (t % 5) * 40.0});
        comb.push_back({x + 50.0, 100.0});
    }
    comb.back() = {0.0, 100.0};
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<Polygon> pieces = convexPieces(comb);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(seconds < 2.0);
    CHECK(pieces.size() >= size_t(teeth));
    double sum = 0.0;
    size_t notConvex = 0;
    for (const Polygon& p : pieces)
    {
        sum += area(p);
        for (size_t i = 0; i < p.size(); ++i)
        {
            const Vec2 a = p[i], b = p[(i + 1) % p.size()], c = p[(i + 2) % p.size()];
            if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) < -1e-6)
                ++notConvex;
        }
    }
    CHECK(notConvex == 0);
    CHECK(sum == doctest::Approx(area(comb)).epsilon(1e-9));
}

TEST_CASE("zones: engine JSON round trip keeps every answer")
{
    ZoneSpec s;
    s.maskCell = 250.0;
    s.culture = "spanish";
    Layer d = makeLayer(builtIn("district"), "districts");
    Zone a = zoneOf("centro", square(0, 0, 10000, 10000), 2, {{"era", "colonial"}});
    a.holes = {square(4000, 4000, 6000, 6000)};
    d.zones.push_back(a);
    s.layers.push_back(d);
    Layer p = makeLayer(builtIn("pedestrians"), "peds");
    Zone z;
    z.id = "malecon";
    z.name = "Malecon";
    z.mask.cell = s.maskCell;
    paint(z.mask, {.center = {-3000, 2000}, .radius = 2500, .value = 0.7, .strength = 1.0, .softness = 0.5});
    p.zones.push_back(z);
    s.layers.push_back(p);

    std::vector<ExportedMask> masks;
    const json doc = exportJson(s, "World_", &masks);
    REQUIRE(masks.size() == 1);
    CHECK(doc["format"] == "lila.zones");
    CHECK(doc["layers"][1]["zones"][0]["mask"]["image"] == "World_peds_malecon.png");
    CHECK(doc["layers"][0]["zones"][0]["attrs"]["era"] == "colonial");
    CHECK(!doc["layers"][1]["zones"][0]["mask"]["outline"].empty());

    const ZoneSpec back = importJson(json::parse(doc.dump()), [&](const std::string& file, glm::ivec2 size) {
        CHECK(file == "World_peds_malecon.png");
        CHECK(size == masks[0].size);
        return masks[0].values;
    });
    CHECK(toJson(back) == toJson(s));
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-8000.0, 12000.0);
    for (int i = 0; i < 200; ++i)
    {
        const Vec2 pt(u(rng), u(rng));
        CHECK(query(back, pt) == query(s, pt));
    }
}

TEST_CASE("zone commands: create, paint, query, list, check, undo, districts, terrain masks, export and import")
{
    owtest::loadPackages(true, false);
    run("newScene");

    // A terrain to drape over, with a painted mask.
    run("terrainCreate", {{"size", {100000.0, 100000.0}}, {"resolution", 129}, {"flat", true}, {"name", "Ground"}});
    run("terrainStroke", {{"args", {"Ground"}}, {"brush", "mask"}, {"mask", "market"}, {"points", {{20000.0, 20000.0}}}, {"radius", 6000.0},
                          {"strength", 1.0}, {"value", 1.0}, {"stage", "final"}});

    const json made = run("zoneCreate", {{"type", "district"}, {"polygon", {{-50000, -50000}, {0, -50000}, {0, 50000}, {-50000, 50000}}}});
    CHECK(made["created"] == true);
    CHECK(made["groundFrom"] == "the only terrain in the scene");
    CHECK(made["layerCreated"] == true);
    CHECK(made["zone"]["id"] == "district_1");
    CHECK(!made["zone"]["name"].get<std::string>().empty());
    const std::string set = made["set"].get<std::string>();

    run("zoneCreate", {{"layer", "district"}, {"id", "east"}, {"name", "Barrio Este"}, {"polygon", {{0, -50000}, {50000, -50000}, {50000, 50000}, {0, 50000}}}});
    CHECK(run("zoneCheck")["ok"] == true);

    // Undo takes the zone back out.
    run("zoneCreate", {{"layer", "district"}, {"id", "extra"}, {"name", "Extra"}, {"polygon", {{-1000, -1000}, {1000, -1000}, {1000, 1000}, {-1000, 1000}}}});
    CHECK(run("zoneList")["layers"][0]["count"] == 3);
    const json overlapped = run("zoneCheck");
    CHECK(overlapped["counts"]["overlap"].get<int>() >= 1);
    run("undo");
    CHECK(run("zoneList")["layers"][0]["count"] == 2);

    // Attributes are typed by the layer's defaults.
    run("zoneCreate", {{"type", "police"}, {"id", "municipal"}, {"name", "Policia Municipal"}, {"circle", {0, 0, 20000}}, {"attrs", {{"responseTime", 30}}}});
    run("zoneLayer", {{"layer", "police"}, {"mustCover", false}});
    CHECK_THROWS_AS(run("zoneEdit", {{"layer", "police"}, {"zone", "municipal"}, {"attrs", {{"responseTime", "soon"}}}}), lila::CommandError);
    run("zoneEdit", {{"layer", "police"}, {"zone", "municipal"}, {"priority", 3}, {"attrs", {{"agency", "state"}}}});

    // A painted density zone and a point query over every layer.
    const json painted = run("zonePaint", {{"layer", "traffic"}, {"type", "traffic"}, {"zone", "core"}, {"points", {{-10000, 0}, {10000, 0}}},
                                           {"radius", 5000.0}, {"value", 0.5}, {"softness", 0.0}});
    CHECK(painted["changed"].get<int>() > 0);
    json q = run("zoneQuery", {{"point", {5000, 0, 1000}}});
    CHECK(q["layers"]["district"]["zone"] == "east");
    CHECK(q["layers"]["district"]["name"] == "Barrio Este");
    CHECK(q["layers"]["police"]["attrs"]["agency"] == "state");
    CHECK(q["layers"]["police"]["attrs"]["responseTime"] == 30);
    CHECK(q["layers"]["traffic"]["density"].get<double>() == doctest::Approx(0.5).epsilon(0.02));
    q = run("zoneQuery", {{"point", {5000, 40000}}});
    CHECK(q["layers"]["traffic"]["density"].get<double>() == 0.0);
    CHECK(q["layers"]["police"]["zone"].is_null());

    // A gap: the check finds it inside the ground's extent.
    run("zoneDelete", {{"layer", "district"}, {"zone", "east"}});
    json c = run("zoneCheck");
    CHECK(c["ok"] == false);
    CHECK(c["counts"]["gap"] == 1);
    CHECK(c["playAreaFrom"].get<std::string>().starts_with("ground"));
    run("undo");
    CHECK(run("zoneCheck")["ok"] == true);

    // From a terrain mask.
    const json fromMask = run("zoneFromTerrain", {{"masks", {"market"}}, {"layer", "peds"}, {"type", "pedSpawn"}});
    REQUIRE(fromMask["zones"].size() == 1);
    CHECK(run("zoneQuery", {{"point", {20000, 20000}}, {"layer", "peds"}})["layers"]["peds"]["zone"] == "market");
    CHECK(run("zoneQuery", {{"point", {-20000, -20000}}, {"layer", "peds"}})["layers"]["peds"]["zone"].is_null());

    // From a Settlement's districts: one zone per district, named as the settlement names it, covering its quarters.
    const json town = run("settlementCreate", {{"profile", "usTown"}, {"size", 40000.0}, {"seed", 2}, {"name", "Town"}});
    const json names = run("settlementNames", {{"args", {town["mesh"]}}});
    REQUIRE(!names["districts"].empty());
    const json fromD = run("zoneFromDistricts", {{"settlement", town["mesh"]}, {"layer", "quarters"}});
    CHECK(fromD["zones"].size() == names["districts"].size());
    for (const json& d : names["districts"])
    {
        const json at = run("zoneQuery", {{"point", d["center"]}, {"layer", "quarters"}});
        CHECK(at["layers"]["quarters"]["name"] == d["name"]);
    }
    // Running it again replaces rather than adds.
    run("zoneFromDistricts", {{"settlement", town["mesh"]}, {"layer", "quarters"}});
    CHECK(run("zoneList", {{"layer", "quarters"}})["layers"][0]["count"] == names["districts"].size());

    // Export and import: the same answers from a new set.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("lila_zones_test_" + std::to_string(std::random_device()()));
    const json ex = run("zoneExport", {{"folder", dir.string()}, {"name", "World"}});
    CHECK(std::filesystem::exists(ex["json"].get<std::string>()));
    CHECK(ex["masks"].size() == 2);
    const json before = run("zoneSpec");
    std::vector<json> answers;
    for (double x = -45000; x <= 45000; x += 7500)
        answers.push_back(run("zoneQuery", {{"point", {x, x * 0.3}}})["layers"]);
    run("delete", {{"args", {set}}});
    CHECK_THROWS_AS(run("zoneList"), lila::CommandError);
    const json imported = run("zoneImport", {{"path", ex["json"]}, {"setName", "Imported"}});
    CHECK(imported["created"] == true);
    CHECK(run("zoneSpec") == before);
    size_t k = 0;
    for (double x = -45000; x <= 45000; x += 7500)
        CHECK(run("zoneQuery", {{"point", {x, x * 0.3}}})["layers"] == answers[k++]);
    std::filesystem::remove_all(dir);
}

TEST_CASE("zone commands: on a Z-up scene zones lie on world XY")
{
    owtest::loadPackages(true, false);
    run("newScene");
    lila::math::setUpAxis(lila::math::UpAxis::Z);
    run("terrainCreate", {{"size", {100000.0, 100000.0}}, {"resolution", 129}, {"flat", true}, {"name", "Ground"}});
    run("terrainStroke", {{"args", {"Ground"}}, {"brush", "mask"}, {"mask", "market"}, {"points", {{20000.0, -30000.0}}}, {"radius", 6000.0},
                          {"strength", 1.0}, {"value", 1.0}, {"stage", "final"}});
    run("zoneFromTerrain", {{"masks", {"market"}}, {"layer", "peds"}, {"type", "pedSpawn"}});
    CHECK(run("zoneQuery", {{"point", {20000, -30000}}, {"layer", "peds"}})["layers"]["peds"]["zone"] == "market");
    CHECK(run("zoneQuery", {{"point", {20000, -30000, 0}}, {"layer", "peds"}})["layers"]["peds"]["zone"] == "market");
    CHECK(run("zoneQuery", {{"point", {-30000, 20000}}, {"layer", "peds"}})["layers"]["peds"]["zone"].is_null());

    // A closed curve drawn on the XY ground outlines the zone it was drawn as.
    const std::string c = run("curve", {{"points", {{0, 10000, 0}, {0, 20000, 0}, {10000, 20000, 0}, {10000, 10000, 0}, {0, 10000, 0}}}, {"type", "poly"}})
                              .get<std::string>();
    run("zoneCreate", {{"layer", "district"}, {"type", "district"}, {"id", "block"}, {"curve", c}});
    CHECK(run("zoneQuery", {{"point", {5000, 15000, 0}}, {"layer", "district"}})["layers"]["district"]["zone"] == "block");
    CHECK(run("zoneQuery", {{"point", {5000, 0, 15000}}, {"layer", "district"}})["layers"]["district"]["zone"].is_null());
    lila::math::setUpAxis(lila::math::UpAxis::Y);
}

namespace
{
/** A top view of the 100 m square round the origin, 800 px across. */
lila::ui::ViewInfo topView(const std::string& name)
{
    lila::ui::ViewInfo v;
    v.camera = name;
    v.eye = {0.0, 100000.0, 0.0};
    v.view = glm::lookAt(v.eye, glm::dvec3(0.0), glm::dvec3(0.0, 0.0, -1.0));
    v.proj = glm::ortho(-60000.0, 60000.0, -60000.0, 60000.0, 1.0, 400000.0);
    v.width = v.height = 800.0;
    v.ortho = true;
    return v;
}

/** The zones drawer's output in a view: summed fill alpha over a pixel, and how many text glyph triangles. */
struct ZoneDraw
{
    float fillAlphaAt = 0.0f;
    size_t textTris = 0;
    size_t tris = 0;
};

ZoneDraw drawZonesIn(const lila::ui::ViewInfo& view, glm::vec2 px)
{
    using namespace lila::ui;
    // Names are drawn from the font atlas, which needs a GUI application.
    if (!QGuiApplication::instance())
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        static int argc = 1;
        static char name[] = "lila_openworld_tests";
        static char* argv[] = {name, nullptr};
        static QGuiApplication app(argc, argv);
    }
    lila::render::FrameState s;
    OverlayPainter painter(s.overlayTris, 1.0);
    const auto drawers = viewportHooks::drawers();
    const auto ids = viewportHooks::drawerIds();
    for (size_t i = 0; i < drawers.size(); ++i)
        if (ids[i] == "openworld.zones" && viewOverlays::drawerShown(ids[i], view.camera))
            drawers[i](s, painter, view);
    ZoneDraw d;
    const auto& t = s.overlayTris;
    d.tris = t.size() / 3;
    for (size_t i = 0; i + 2 < t.size(); i += 3)
    {
        if (t[i].uv.x >= 0.0f)
        {
            ++d.textTris;
            continue;
        }
        const glm::vec2 a = t[i].pos, b = t[i + 1].pos, c = t[i + 2].pos;
        auto side = [](glm::vec2 p, glm::vec2 q, glm::vec2 r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
        const float s0 = side(a, b, px), s1 = side(b, c, px), s2 = side(c, a, px);
        if ((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0))
            d.fillAlphaAt += t[i].color.a;
    }
    return d;
}
} // namespace

TEST_CASE("zones overlay: one layer drawn with a faint fill, outlines and names; a per-view Show toggle hides it")
{
    using namespace lila::ui;
    owtest::loadPackages(true, false);
    run("newScene");
    run("terrainCreate", {{"size", {100000.0, 100000.0}}, {"resolution", 65}, {"flat", true}, {"name", "Ground"}});
    // Three layers stacked over the same ground, as a world build makes them.
    run("zoneCreate", {{"type", "district"}, {"id", "west"}, {"name", "Barrio Oeste"}, {"polygon", {{-50000, -50000}, {0, -50000}, {0, 50000}, {-50000, 50000}}}});
    run("zoneCreate", {{"layer", "district"}, {"id", "east"}, {"name", "Barrio Este"}, {"polygon", {{0, -50000}, {50000, -50000}, {50000, 50000}, {0, 50000}}}});
    run("zoneCreate", {{"type", "police"}, {"id", "all"}, {"name", "Policia del Valle"}, {"polygon", {{-50000, -50000}, {50000, -50000}, {50000, 50000}, {-50000, 50000}}}});
    run("zoneCreate", {{"type", "wanted"}, {"id", "hot"}, {"name", "Zona Caliente"}, {"polygon", {{-50000, -50000}, {50000, -50000}, {50000, 50000}, {-50000, 50000}}}});

    // A pixel inside a zone, clear of every outline and name.
    const ViewInfo top = topView("top");
    const glm::vec2 px(560.0f, 230.0f);
    lila::scene::Graph& g = lila::scene::Graph::current();
    g.select({});
    // Nobody working on the zones: outlines and names over the ground, no fill.
    const ZoneDraw d = drawZonesIn(top, px);
    CHECK(d.tris > 0);
    CHECK(d.textTris > 0);
    CHECK(d.fillAlphaAt == 0.0f);
    // The set selected: one layer's fill, faint, never the stacked fills of every layer.
    run("select", {{"args", {run("zoneList")["set"]}}});
    const ZoneDraw editing = drawZonesIn(top, px);
    CHECK(editing.fillAlphaAt > 0.05f);
    CHECK(editing.fillAlphaAt <= 0.16f);
    g.select({});

    // The Show toggle: an action and a row per view, off in one view only.
    bool listed = false;
    for (const viewOverlays::Overlay& o : viewOverlays::all())
        listed |= o.id == "openworld.zones" && o.label == "Zones";
    CHECK(listed);
    const Action* show = actions::find(viewOverlays::actionId("openworld.zones"));
    REQUIRE(show);
    CHECK(show->label == "Show Zones");
    viewOverlays::setShown("openworld.zones", "top", false);
    CHECK_FALSE(viewOverlays::shown("openworld.zones", "top"));
    CHECK(viewOverlays::shown("openworld.zones", "persp"));
    CHECK(drawZonesIn(top, px).tris == 0);
    CHECK(drawZonesIn(topView("persp"), px).tris == d.tris);
    viewOverlays::setShown("openworld.zones", "top", true);
    CHECK(drawZonesIn(top, px).tris == d.tris);
}
