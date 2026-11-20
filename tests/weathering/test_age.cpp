#include "TestUtil.h"

#include "Pieces.h"

#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

using namespace weatheringtest;

namespace
{
glm::dvec3 worldPos(const std::string& path)
{
    g().evaluate();
    scene::Node* n = g().find(path);
    REQUIRE(n);
    return glm::dvec3(n->as<glm::dmat4>(scene::Transform::kWorldMatrix)[3]);
}

/** Distance in x, z from a point to an axis-aligned box footprint (0 inside). */
/** The result of a preset run's first step running command. */
json stepOf(const json& r, const char* command)
{
    for (const json& s : r["steps"])
        if (s["command"] == command)
            return s.value("result", json());
    FAIL("no " << command << " step");
    return json();
}

double outside(const glm::dvec3& p, const glm::dvec3& lo, const glm::dvec3& hi)
{
    const double dx = std::max({lo.x - p.x, 0.0, p.x - hi.x});
    const double dz = std::max({lo.z - p.z, 0.0, p.z - hi.z});
    return std::sqrt(dx * dx + dz * dz);
}

/** The house, the ground and three items: two chunks and a tall plank. */
void houseScene()
{
    fresh();
    cube("House", {0, 0, 0}, {400, 300, 400});
    run("polyPlane", {{"name", "Ground"}, {"width", 2000.0}, {"depth", 2000.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}});
    cube("ChunkA", {3000, 0, 0}, {14, 10, 12});
    cube("ChunkB", {3100, 0, 0}, {22, 14, 18});
    cube("Plank", {3200, 0, 0}, {12, 150, 2});
}

json band(int64_t seed, bool realize = true)
{
    return run("baseDressing", {{"args", {"House"}},
                                {"items", {"ChunkA", "ChunkB", "Plank"}},
                                {"width", 90.0},
                                {"density", 12.0},
                                {"realize", realize},
                                {"seed", seed}});
}
} // namespace

TEST_CASE("weathering: base dressing stays in the band round the footprint")
{
    houseScene();
    const json r = band(1);
    REQUIRE(r["count"].get<int>() > 20);
    CHECK(r["ground"].is_array());
    CHECK(r["ground"].size() == 1);
    int leaning = 0;
    for (const json& n : r["nodes"])
    {
        const glm::dvec3 p = worldPos(n.get<std::string>());
        const double d = outside(p, {-200, 0, -200}, {200, 300, 200});
        CHECK(d > 0.0);
        CHECK(d < 90.0 + 30.0);
        CHECK(std::abs(p.y) < 1.0);
    }
    leaning = r["leaning"].get<int>();
    CHECK(leaning > 0);
    CHECK(r["perItem"]["Plank"].get<int>() >= leaning);
}

TEST_CASE("weathering: base dressing is the same for the same seed and differs for another")
{
    houseScene();
    const json a = band(4);
    std::vector<glm::dvec3> first;
    for (const json& n : a["nodes"])
        first.push_back(worldPos(n.get<std::string>()));
    run("delete", {{"args", {a["group"]}}});
    const json b = band(4);
    REQUIRE(b["nodes"].size() == first.size());
    for (size_t k = 0; k < first.size(); ++k)
        CHECK(glm::length(worldPos(b["nodes"][k].get<std::string>()) - first[k]) < 1e-6);
    const json c = band(5);
    bool differs = c["nodes"].size() != first.size();
    for (size_t k = 0; !differs && k < first.size(); ++k)
        differs = glm::length(worldPos(c["nodes"][k].get<std::string>()) - first[k]) > 1e-3;
    CHECK(differs);
}

TEST_CASE("weathering: base dressing follows an L-shaped outline and keeps out of both wings")
{
    fresh();
    cube("WingA", {0, 0, 0}, {600, 300, 200});
    cube("WingB", {-200, 0, 300}, {200, 300, 400});
    cube("Chunk", {3000, 0, 0}, {15, 10, 15});
    const json r = run("baseDressing", {{"args", {"WingA", "WingB"}}, {"items", {"Chunk"}}, {"width", 60.0}, {"density", 20.0}, {"realize", true}});
    REQUIRE(r["count"].get<int>() > 30);
    CHECK(r["ground"] == "plane");
    for (const json& n : r["nodes"])
    {
        const glm::dvec3 p = worldPos(n.get<std::string>());
        CHECK(outside(p, {-300, 0, -100}, {300, 300, 100}) > 0.0);
        CHECK(outside(p, {-300, 0, 100}, {-100, 300, 500}) > 0.0);
    }
}

TEST_CASE("weathering: live base dressing rebuilds when the host moves")
{
    houseScene();
    const json r = band(2, false);
    REQUIRE(r.contains("host"));
    const std::string host = r["host"].get<std::string>();
    auto centre = [&] {
        const geo::Mesh m = meshOf(host);
        glm::dvec3 lo(1e300), hi(-1e300);
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
            {
                lo = glm::min(lo, m.position(p));
                hi = glm::max(hi, m.position(p));
            }
        REQUIRE(lo.x < hi.x);
        return (lo + hi) * 0.5;
    };
    const glm::dvec3 before = centre();
    CHECK(std::abs(before.x) < 60.0);
    run("move", {{"args", {"House"}}, {"by", {500, 0, 0}}});
    const glm::dvec3 after = centre();
    CHECK(after.x - before.x == doctest::Approx(500.0).epsilon(0.1));
}

TEST_CASE("weathering: placeholders by role and style, and the user's own pieces win")
{
    fresh();
    run("weatheringPlaceholders", {{"force", true}});
    const std::vector<std::string> toon = weathering::roleAssets("rubble", weathering::Style::Toon);
    REQUIRE(!toon.empty());
    for (const std::string& id : toon)
        CHECK(id.find("Toon") != std::string::npos);
    const std::vector<std::string> real = weathering::roleAssets("rubble", weathering::Style::Realistic);
    REQUIRE(!real.empty());
    for (const std::string& id : real)
        CHECK(id.find("Toon") == std::string::npos);

    cube("MyRubble", {0, 0, 0}, {20, 15, 20});
    run("weatheringAddPieces", {{"args", {"MyRubble"}}, {"role", "rubble"}, {"thumbnail", false}});
    const std::vector<std::string> mine = weathering::roleAssets("rubble", weathering::Style::Toon);
    REQUIRE(mine.size() == 1);
    CHECK(mine[0] == "Weathering/rubble/MyRubble");
}

TEST_CASE("weathering: age runs a preset in either style, with the user's pieces swapped in")
{
    houseScene();
    const json presets = run("weatherPresets");
    std::set<std::string> names;
    for (const json& p : presets)
        names.insert(p["name"].get<std::string>());
    for (const char* n : {"Concrete Debris", "Heavy Rubble", "Leaning Planks", "Weeds", "Grunge", "Worn Edges", "Chipped Edges", "Ivy", "Roof Sheets", "Windows"})
    {
        CHECK(names.contains(n));
        CHECK(names.contains(std::string(n) + " Toon"));
    }
    const json r = run("weather", {{"args", {"House"}}, {"preset", "Concrete Debris"}, {"style", "toon"}});
    CHECK(r["preset"] == "Concrete Debris Toon");
    CHECK(!r["group"].get<std::string>().empty());
    CHECK(g().find(r["group"].get<std::string>()));

    const json mine = run("weather", {{"args", {"House"}}, {"preset", "Concrete Debris"}, {"slots", {{"pieces", {"ChunkA", "ChunkB"}}}}, {"seed", 3}});
    const json step = stepOf(mine, "baseDressing");
    CHECK(step["perItem"].contains("ChunkA"));
    CHECK(step["perItem"].contains("ChunkB"));
    CHECK(step["perItem"].size() == 2);
}

TEST_CASE("weathering: realized base dressing settles with Stagehand")
{
    houseScene();
    const json r = run("baseDressing", {{"args", {"House"}}, {"items", {"ChunkA", "ChunkB"}}, {"width", 60.0}, {"density", 6.0}, {"realize", true}, {"settle", true}});
    REQUIRE(r["count"].get<int>() > 0);
    CHECK(r.contains("atRest"));
    CHECK(r["atRest"].get<int>() >= r["count"].get<int>() / 2);
}

TEST_CASE("weathering: preset thumbnails leave no trace in the scene")
{
    houseScene();
    const size_t before = g().nodes().size();
    const json r = run("arrangementThumbnails", {{"args", {"Leaning Planks"}}});
    CHECK(r["drawn"].size() + r["skipped"].size() == 1);
    CHECK(g().nodes().size() == before);
}

TEST_CASE("weathering: base dressing keeps off blockers without dressing round them")
{
    fresh();
    cube("A", {0, 0, 0}, {200, 200, 200});
    cube("B", {260, 0, 0}, {200, 200, 200});
    cube("Chunk", {3000, 0, 0}, {12, 10, 12});
    const json r = run("baseDressing", {{"args", {"A"}}, {"blockers", {"B"}}, {"items", {"Chunk"}}, {"width", 120.0}, {"density", 30.0}, {"realize", true}});
    REQUIRE(r["count"].get<int>() > 10);
    for (const json& n : r["nodes"])
    {
        const glm::dvec3 p = worldPos(n.get<std::string>());
        CHECK(outside(p, {160, 0, -100}, {360, 200, 100}) > 0.0);
        CHECK(outside(p, {-100, 0, -100}, {100, 200, 100}) > 0.0);
    }
}

TEST_CASE("weathering: age in tiles rebuilds only the edited block's tile")
{
    fresh();
    std::vector<std::string> blocks;
    for (int x = 0; x < 4; ++x)
        for (int z = 0; z < 4; ++z)
            blocks.push_back(cube(("Block" + std::to_string(x) + std::to_string(z)).c_str(), {x * 1000.0, 0, z * 1000.0}, {300, 250, 300}));
    const json r = run("weather", {{"args", blocks}, {"preset", "Concrete Debris"}, {"tile", 1000.0}});
    REQUIRE(r["tileCount"].get<int>() == 16);
    std::vector<std::string> hosts;
    for (const json& t : r["tiles"])
        hosts.push_back(stepOf(t, "baseDressing")["host"].get<std::string>());
    auto bounds = [&](const std::string& h) {
        const geo::Mesh m = meshOf(h);
        glm::dvec3 lo(1e300);
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                lo = glm::min(lo, m.position(p));
        return lo;
    };
    std::vector<glm::dvec3> before;
    for (const std::string& h : hosts)
        before.push_back(bounds(h));
    run("move", {{"args", {blocks[0]}}, {"by", {0, 0, 120}}});
    int changed = 0;
    for (size_t k = 0; k < hosts.size(); ++k)
        changed += glm::length(bounds(hosts[k]) - before[k]) > 1e-6 ? 1 : 0;
    CHECK(changed == 1);
}

TEST_CASE("weathering: every Age preset runs in both styles")
{
    houseScene();
    int ran = 0;
    for (const json& p : run("weatherPresets"))
    {
        if (!p["slots"].contains("targets"))
            continue;
        const std::string name = p["name"].get<std::string>();
        INFO(name);
        // One effect each: a preset is what one drag onto a mesh applies.
        CHECK(p["steps"].size() == 1);
        // Windows needs a wall with openings; its own tests cover it.
        if (name.rfind("Windows", 0) == 0)
            continue;
        const std::string box = cube(("Box" + std::to_string(ran)).c_str(), {1000.0 * (ran + 1), 0, 0}, {300, 250, 300});
        json r;
        CHECK_NOTHROW(r = run("weather", {{"args", {box}}, {"preset", name}, {"visgroup", ""}}));
        // Trim Sheets isn't loaded here, so edge decals are skipped and the result says so.
        for (const json& s : r["steps"])
            CHECK(s.value("skipped", false) == (s["command"] == "edgeDecals"));
        if (r.contains("notes"))
            CHECK(r["notes"][0].get<std::string>().find("Trim Sheets") != std::string::npos);
        ++ran;
    }
    CHECK(ran == 46);
}

TEST_CASE("weathering: weatheringLive pauses and resumes what weather made")
{
    houseScene();
    const json r = run("weather", {{"args", {"House"}}, {"preset", "Leaning Planks"}, {"visgroup", ""}});
    run("weather", {{"args", {"House"}}, {"preset", "Grunge"}, {"visgroup", ""}});
    run("weather", {{"args", {"House"}}, {"preset", "Chipped Edges"}, {"visgroup", ""}});
    const std::string group = r["group"].get<std::string>();
    // Named by the weathered object: its grunge and chipping go off and the group made for it hides.
    const json off = run("weatheringLive", {{"args", {"House"}}, {"live", false}});
    CHECK(off["modifiers"].get<int>() == 2);
    CHECK(run("getAttr", {{"args", {group + ".visibility"}}}) == false);
    run("weatheringLive", {{"args", {"House"}}});
    CHECK(run("getAttr", {{"args", {group + ".visibility"}}}) == true);
}

TEST_CASE("weathering: OBJ import makes materials from the .mtl")
{
    fresh();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lila_weathering_obj";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "piece.mtl") << "newmtl M_Piece\nKd 0.5 0.25 0.1\n";
    std::ofstream(dir / "piece.obj") << "mtllib piece.mtl\no piece\nusemtl M_Piece\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    run("importObj", {{"args", {(dir / "piece.obj").generic_string()}}});
    const json info = run("materialInfo", {{"args", {"M_Piece"}}});
    const json c = info["baseColor"];
    CHECK(c[0].get<double>() == doctest::Approx(std::pow(0.5, 2.2)).epsilon(0.01));
    CHECK(c[1].get<double>() < c[0].get<double>());
}
