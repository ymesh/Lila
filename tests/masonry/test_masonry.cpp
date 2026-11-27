#include "TestUtil.h"
#include "../link/FakeServer.h"
#include "MasonryBake.h"
#include "MasonryCore.h"
#include "MasonryShip.h"
#include "core/PrefStore.h"
#include "link/UnrealLink.h"

#include "extensions/ExtensionManager.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

using namespace lila;
using namespace masonry;
using namespace masonrytest;
using json = nlohmann::json;

namespace
{
Footprint circle(double radius, int segments)
{
    Footprint f;
    for (int k = 0; k < segments; ++k)
    {
        const double a = 2.0 * 3.14159265358979323846 * k / segments;
        f.points.push_back({radius * std::cos(a), 0.0, radius * std::sin(a)});
    }
    f.closed = true;
    return f;
}

double signedVolume(const geo::Mesh& m)
{
    double v = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> p = m.facePoints(f);
        for (size_t k = 1; k + 1 < p.size(); ++k)
            v += glm::dot(m.position(p[0]), glm::cross(m.position(p[k]), m.position(p[k + 1]))) / 6.0;
    }
    return v;
}

bool closedManifold(const geo::Mesh& m)
{
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) != 2)
            return false;
    return m.validate().empty();
}
} // namespace

TEST_CASE("masonry: a straight running bond wall fits its height and never overlaps")
{
    WallParams p;
    const Layout l = layoutWall(line({{0, 0, 0}, {500, 0, 0}}), p);
    CHECK(l.courses == int(std::lround(p.height / (p.stone.height + p.stone.joint))));
    CHECK(l.pitch * l.courses == doctest::Approx(p.height));
    CHECK(countOverlaps(l.stones) == 0);
    // Two faces, roughly a stone and a joint each along 5 m.
    const double perCourse = double(l.stones.size()) / l.courses;
    CHECK(perCourse > 2.0 * 500.0 / (p.stone.length + p.stone.joint) - 3.0);
    CHECK(perCourse < 2.0 * 500.0 / (p.stone.length + p.stone.joint) + 6.0);
    for (const Stone& s : l.stones)
    {
        CHECK(s.center.x > -1.0);
        CHECK(s.center.x < 501.0);
        CHECK(std::abs(s.center.z) < p.thickness * 0.5);
        CHECK(s.size.x >= std::max(2.0, p.stone.length * 0.2) - 1e-6);
        CHECK(std::abs(glm::determinant(s.frame) - 1.0) < 1e-9);
    }
}

TEST_CASE("masonry: running bond courses are offset by half a stone")
{
    WallParams p;
    p.jitter = 0.0;
    p.protrude = 0.0;
    const Layout l = layoutWall(line({{0, 0, 0}, {300, 0, 0}}), p);
    // The first outer-face stone at the start of each course: full on even courses, a bat on odd.
    for (int course = 0; course < 4; ++course)
    {
        const Stone* first = nullptr;
        for (const Stone& s : l.stones)
            if (s.course == course && s.face == Face::Outer && (!first || s.center.x < first->center.x))
                first = &s;
        REQUIRE(first);
        CHECK(first->role == (course % 2 ? Role::Bat : Role::Stretcher));
    }
}

TEST_CASE("masonry: every bond lays a clean wall")
{
    for (const std::string& name : bondNames())
    {
        CAPTURE(name);
        WallParams p;
        REQUIRE(parseBond(name, &p.bond));
        const Layout l = layoutWall(line({{0, 0, 0}, {320, 0, 0}, {320, 0, 260}, {-40, 0, 260}}), p);
        CHECK(!l.stones.empty());
        CHECK(countOverlaps(l.stones) == 0);
        std::set<Role> roles;
        for (const Stone& s : l.stones)
            roles.insert(s.role);
        if (p.bond == Bond::English || p.bond == Bond::Flemish || p.bond == Bond::Header || p.bond == Bond::GardenWall)
            CHECK(roles.count(Role::Header) == 1);
        if (p.bond == Bond::Running || p.bond == Bond::Stack)
            CHECK(roles.count(Role::Header) == 0);
    }
}

TEST_CASE("masonry: English bond alternates stretcher and header courses, Flemish mixes them in one course")
{
    WallParams p;
    p.bond = Bond::English;
    const Layout english = layoutWall(line({{0, 0, 0}, {400, 0, 0}}), p);
    for (const Stone& s : english.stones)
    {
        if (s.course % 2 == 0)
            CHECK(s.role != Role::Header);
        else
            CHECK(s.role != Role::Stretcher);
    }
    p.bond = Bond::Flemish;
    const Layout flemish = layoutWall(line({{0, 0, 0}, {400, 0, 0}}), p);
    int headers = 0, stretchers = 0;
    for (const Stone& s : flemish.stones)
        if (s.course == 0)
        {
            headers += s.role == Role::Header;
            stretchers += s.role == Role::Stretcher;
        }
    CHECK(headers > 5);
    CHECK(stretchers > 5);
}

TEST_CASE("masonry: corners are filled on every course")
{
    WallParams p;
    const Layout l = layoutWall(line({{0, 0, 0}, {400, 0, 0}, {400, 0, 300}}), p);
    CHECK(countOverlaps(l.stones) == 0);
    // The path turns left, so the outside of the corner is on the right.
    const glm::dvec2 outside(400.0 + p.thickness * 0.5, -p.thickness * 0.5);
    for (int course = 0; course < l.courses; ++course)
    {
        CAPTURE(course);
        double nearest = 1e300;
        for (const Stone& s : l.stones)
            if (s.course == course)
                nearest = std::min(nearest, glm::length(glm::dvec2(s.center.x, s.center.z) - outside));
        CHECK(nearest < p.stone.length);
    }
}

TEST_CASE("masonry: a closed square and a round tower close without overlaps")
{
    WallParams p;
    p.thickness = 40.0;
    const Layout square = layoutWall(line({{0, 0, 0}, {300, 0, 0}, {300, 0, 300}, {0, 0, 300}}, true), p);
    CHECK(!square.stones.empty());
    CHECK(countOverlaps(square.stones) == 0);

    const Layout tower = layoutWall(circle(150.0, 48), p);
    CHECK(!tower.stones.empty());
    CHECK(countOverlaps(tower.stones) == 0);
    for (const Stone& s : tower.stones)
    {
        const double r = glm::length(glm::dvec2(s.center.x, s.center.z));
        CHECK(r > 150.0 - p.thickness * 0.5 - 1.0);
        CHECK(r < 150.0 + p.thickness * 0.5 + 1.0);
    }
}

TEST_CASE("masonry: a wall thinner than a stone and a half is one stone thick")
{
    WallParams p;
    p.thickness = 12.0;
    const Layout l = layoutWall(line({{0, 0, 0}, {200, 0, 0}, {200, 0, 200}}), p);
    REQUIRE(!l.stones.empty());
    CHECK(countOverlaps(l.stones) == 0);
    for (const Stone& s : l.stones)
    {
        CHECK(s.face == Face::Through);
        CHECK(s.size.z == doctest::Approx(12.0));
    }
}

TEST_CASE("masonry: layout is the same every time")
{
    WallParams p;
    p.bond = Bond::Flemish;
    const Footprint f = line({{0, 0, 0}, {350, 0, 0}, {350, 0, 220}, {600, 0, 500}});
    const Layout a = layoutWall(f, p), b = layoutWall(f, p);
    REQUIRE(a.stones.size() == b.stones.size());
    for (size_t k = 0; k < a.stones.size(); ++k)
    {
        CHECK(a.stones[k].center == b.stones[k].center);
        CHECK(a.stones[k].size == b.stones[k].size);
        CHECK(a.stones[k].frame == b.stones[k].frame);
    }
    p.seed = 2;
    const Layout c = layoutWall(f, p);
    bool differs = c.stones.size() != a.stones.size();
    for (size_t k = 0; !differs && k < a.stones.size(); ++k)
        differs = a.stones[k].center != c.stones[k].center;
    CHECK(differs);
}

TEST_CASE("masonry: chipped stones stay closed, outward and free of slivers")
{
    int failed = 0;
    for (uint32_t seed = 0; seed < 64; ++seed)
        for (const glm::dvec3 size : {glm::dvec3(21.5, 6.5, 10.25), glm::dvec3(10.25, 6.5, 21.5), glm::dvec3(40.0, 25.0, 30.0)})
        {
            StoneShape s;
            s.size = size;
            s.seed = seed;
            s.wear = 0.9;
            s.chips = 1.0;
            const geo::Mesh m = stoneMesh(s);
            bool ok = closedManifold(m) && signedVolume(m) > 0.5 * size.x * size.y * size.z;
            for (geo::Index f = 0; ok && f < m.faceCapacity(); ++f)
            {
                if (!m.faceValid(f))
                    continue;
                // Every face has real area and its normal agrees with its own winding: no folded or zero-area pieces.
                const std::vector<geo::Index> p = m.facePoints(f);
                glm::dvec3 n(0.0);
                for (size_t k = 1; k + 1 < p.size(); ++k)
                    n += glm::cross(m.position(p[k]) - m.position(p[0]), m.position(p[k + 1]) - m.position(p[0]));
                ok = glm::length(n) * 0.5 > 1e-4;
            }
            failed += ok ? 0 : 1;
            CAPTURE(seed);
            CHECK(ok);
        }
    CHECK(failed == 0);
}

TEST_CASE("masonry: stone meshes are closed, outward and fill their box")
{
    for (Detail d : {Detail::Worn, Detail::Clean, Detail::Box})
    {
        StoneShape s;
        s.detail = d;
        s.seed = 3;
        const geo::Mesh m = stoneMesh(s);
        CAPTURE(int(d));
        CHECK(closedManifold(m));
        const double box = s.size.x * s.size.y * s.size.z;
        const double v = signedVolume(m);
        CHECK(v > 0.0);
        CHECK(v <= box + 1e-6);
        CHECK(v > box * (d == Detail::Worn ? 0.75 : 0.85));
        CHECK(m.cornerAttrs().has(geo::attr::uvName(0)));
        const geo::Bounds b = m.bounds();
        CHECK(b.max.x <= s.size.x * 0.5 + 1e-9);
        CHECK(b.max.y <= s.size.y * 0.5 + 1e-9);
        CHECK(b.max.z <= s.size.z * 0.5 + 1e-9);
        if (d == Detail::Box)
            CHECK(m.numFaces() == 6);
        else
            CHECK(m.numFaces() == 26);
    }
}

TEST_CASE("masonry: the core is closed and recessed behind the faces")
{
    WallParams p;
    for (const Footprint& f : {line({{0, 0, 0}, {400, 0, 0}, {400, 0, 300}}), line({{0, 0, 0}, {300, 0, 0}, {300, 0, 300}, {0, 0, 300}}, true)})
    {
        const geo::Mesh core = coreMesh(f, p);
        CHECK(closedManifold(core));
        CHECK(signedVolume(core) > 0.0);
        CHECK(core.bounds().max.y == doctest::Approx(p.height - p.stone.joint - p.recess));
    }
    const geo::Mesh straight = coreMesh(line({{0, 0, 0}, {400, 0, 0}}), p);
    // Free ends stop behind the end stones like the faces do.
    CHECK(signedVolume(straight) == doctest::Approx((400.0 - 2.0 * p.recess) * (p.thickness - 2.0 * p.recess) * (p.height - p.stone.joint - p.recess)));
    CHECK(straight.bounds().min.x == doctest::Approx(p.recess));
    CHECK(straight.bounds().max.x == doctest::Approx(400.0 - p.recess));
}

TEST_CASE("masonry: footprints round trip through loose edges")
{
    const std::vector<Footprint> in = {line({{0, 0, 0}, {100, 0, 0}, {100, 0, 50}}), circle(80.0, 10)};
    const std::vector<Footprint> out = footprintsFromEdges(edgesFromFootprints(in));
    REQUIRE(out.size() == 2);
    CHECK(!out[0].closed);
    CHECK(out[0].points.size() == 3);
    CHECK(out[1].closed);
    CHECK(out[1].points.size() == 10);
}

TEST_CASE("masonry: built walls share a few stone meshes")
{
    BuildOptions o;
    const Footprint f = line({{0, 0, 0}, {1000, 0, 0}, {1000, 0, 600}});
    const auto t0 = std::chrono::steady_clock::now();
    const Built b = buildWalls(std::span<const Footprint>(&f, 1), o);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("16 m wall: " << b.stones.size() << " stones, " << b.meshes.size() << " meshes, " << ms << " ms");
    CHECK(b.stones.size() > 2000);
    CHECK(b.meshes.size() < b.stones.size() / 10);
    REQUIRE(b.reference.size() == b.stones.size());
    for (int r : b.reference)
        CHECK(size_t(r) < b.meshes.size());
}

TEST_CASE("masonry: masonryWall builds an editable wall and masonryInfo checks it")
{
    loadMasonry();
    run("newScene");
    const json w = run("masonryWall", {{"points", json::array({json::array({0, 0, 0}), json::array({300, 0, 0}), json::array({300, 0, 200})})},
                                       {"height", 150.0},
                                       {"bond", "flemish"}});
    CHECK(w["stones"].get<int64_t>() > 100);
    CHECK(w["courses"].get<int64_t>() == 20);
    const std::string mesh = w["mesh"].get<std::string>(), mod = w["modifier"].get<std::string>();

    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(mesh);
    REQUIRE(n);
    const scene::MeshPtr out = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
    REQUIRE(out);
    CHECK(out->numFaces() > 1000);

    json info = run("masonryInfo", {{"args", {mesh}}});
    CHECK(info["checks"]["ok"].get<bool>());
    CHECK(info["stones"] == w["stones"]);
    CHECK(info["settings"]["bond"] == "flemish");

    run("setAttr", {{"args", {mod + ".bond", "english"}}});
    info = run("masonryInfo", {{"args", {mesh}}});
    CHECK(info["settings"]["bond"] == "english");
    CHECK(info["checks"]["overlaps"].get<int64_t>() == 0);

    CHECK_THROWS(run("masonryWall", {{"points", json::array({json::array({0, 0, 0}), json::array({100, 0, 0})})}, {"bond", "nonsense"}}));
}

TEST_CASE("masonry: every stone has a three level LOD chain that fills the same box, cheaper each level")
{
    StoneShape s;
    s.seed = 5;
    const std::array<geo::Mesh, 3> lods = stoneLods(s);
    size_t previous = SIZE_MAX;
    for (const geo::Mesh& m : lods)
    {
        CHECK(closedManifold(m));
        // Worn corners sit inside the box by at most the bevel, so levels swap without a visible jump.
        CHECK(m.bounds().max.x <= s.size.x * 0.5 + 1e-9);
        CHECK(m.bounds().max.x >= s.size.x * 0.5 - s.bevel);
        CHECK(m.bounds().max.y >= s.size.y * 0.5 - s.bevel);
        CHECK(m.numFaces() <= previous);
        previous = m.numFaces();
    }
    CHECK(lods[2].numFaces() == 6);

    BuildOptions o;
    o.lods = true;
    const Footprint f = line({{0, 0, 0}, {300, 0, 0}});
    const Built b = buildWalls(std::span<const Footprint>(&f, 1), o);
    REQUIRE(b.lods.size() == b.meshes.size());
    for (size_t r = 0; r < b.meshes.size(); ++r)
        CHECK(b.lods[r][0].numFaces() == b.meshes[r].numFaces());
}

TEST_CASE("masonry: stones chunk by ground cell")
{
    BuildOptions o;
    const Footprint f = line({{0, 0, 0}, {1000, 0, 0}, {1000, 0, 600}});
    const Built b = buildWalls(std::span<const Footprint>(&f, 1), o);
    const Chunks c = chunkStones(b.stones, 400.0);
    REQUIRE(c.of.size() == b.stones.size());
    CHECK(c.cells.size() >= 5);
    for (size_t k = 1; k < c.cells.size(); ++k)
        CHECK((c.cells[k - 1].x < c.cells[k].x || (c.cells[k - 1].x == c.cells[k].x && c.cells[k - 1].y < c.cells[k].y)));
    for (size_t k = 0; k < b.stones.size(); ++k)
    {
        const glm::ivec2 cell = c.cells[size_t(c.of[k])];
        CHECK(int(std::floor(b.stones[k].center.x / 400.0)) == cell.x);
        CHECK(int(std::floor(b.stones[k].center.z / 400.0)) == cell.y);
    }
}

TEST_CASE("masonry: the ship plan places every stone once, chunk by chunk, in Unreal space")
{
    BuildOptions o;
    o.lods = true;
    const Footprint f = line({{0, 0, 0}, {900, 0, 0}, {900, 0, 500}});
    const Built b = buildWalls(std::span<const Footprint>(&f, 1), o);
    const glm::dmat4 world = glm::translate(glm::dmat4(1.0), glm::dvec3(100.0, 0.0, 0.0));
    ShipOptions so;
    const json plan = shipPlan(b, world, "abc", "Wall", so);
    CHECK(plan["assets"].size() == b.meshes.size() + 1);
    CHECK(plan["assets"].back()["kind"] == "core");
    CHECK(plan["folder"] == "/Game/Lila/Masonry/Wall");

    std::set<std::string> keys, stones;
    size_t placed = 0;
    for (const json& g : plan["groups"])
    {
        CHECK(keys.insert(g["key"].get<std::string>()).second);
        if (g["key"] == "abc:core")
        {
            CHECK(g["mode"] == "actors");
            continue;
        }
        CHECK(g["mode"] == "hism");
        CHECK(g["key"].get<std::string>().rfind("abc:", 0) == 0);
        const int asset = g["asset"].get<int>();
        for (const json& p : g["placements"])
        {
            const size_t k = std::stoul(p["id"].get<std::string>());
            CHECK(stones.insert(p["id"].get<std::string>()).second);
            CHECK(b.reference[k] == asset);
            for (const json& v : p["customData"])
            {
                CHECK(v.get<double>() >= 0.0);
                CHECK(v.get<double>() <= 1.0);
            }
            // Unreal is Z up: Lila's height goes to location z.
            CHECK(p["location"][2].get<double>() == doctest::Approx(b.stones[k].center.y));
            ++placed;
        }
    }
    CHECK(placed == b.stones.size());
    CHECK(keys.count("abc:core") == 1);
    CHECK(plan == shipPlan(b, world, "abc", "Wall", so));
}

TEST_CASE("masonry: a stone's LOD chain writes as an FBX LOD group")
{
    StoneShape s;
    const std::array<geo::Mesh, 3> lods = stoneLods(s);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "lila_masonry_test" / "SM_Stone.fbx";
    std::string err;
    REQUIRE(writeLodFbx(lods, "SM_Stone", file, &err));
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(bytes.find("LodGroup") != std::string::npos);
    CHECK(bytes.find("SM_Stone_LOD2") != std::string::npos);
}

TEST_CASE("masonry: masonryShip plans a wall without Unreal on dryRun")
{
    loadMasonry();
    run("newScene");
    const json w = run("masonryWall", {{"points", json::array({json::array({0, 0, 100}), json::array({600, 0, 100})})}, {"height", 100.0}});
    const json r = run("masonryShip", {{"args", {w["mesh"]}}, {"dryRun", true}, {"chunkSize", 200.0}});
    CHECK(r["stones"] == w["stones"]);
    CHECK(r["chunks"].get<int64_t>() == 3);
    CHECK(r["lodTriangles"].size() == 3);
    CHECK(r["lodTriangles"][0].get<double>() > r["lodTriangles"][2].get<double>());
    CHECK(r["lodTriangles"][2].get<double>() == doctest::Approx(12.0));
    CHECK(r["collisionHulls"].get<int64_t>() == 1);
    const json m = run("masonryShip", {{"args", {w["mesh"]}}, {"dryRun", true}, {"chunkSize", 200.0}, {"mode", "merged"}});
    CHECK(m["groups"].get<int64_t>() == 4);
    CHECK(r["plan"]["groups"].size() == size_t(r["groups"].get<int64_t>()));
    const json lp = run("masonryShip", {{"args", {w["mesh"]}}, {"dryRun", true}, {"chunkSize", 200.0}, {"mode", "lowPoly"}});
    CHECK(lp["groups"].get<int64_t>() == 3);
    CHECK(lp["shellTriangles"].get<int64_t>() > 0);
    CHECK(lp["collisionHulls"].get<int64_t>() == 3);
    CHECK_THROWS(run("masonryShip", {{"args", {w["mesh"]}}, {"mode", "nanite"}, {"dryRun", true}}));
}

TEST_CASE("masonry: a wall's sent record belongs to one level; a level made again under the same path removes nothing")
{
    loadMasonry();
    run("newScene");
    const json w = run("masonryWall", {{"points", json::array({json::array({0, 0, 100}), json::array({600, 0, 100})})}, {"height", 100.0}});
    const std::filesystem::path cache = std::filesystem::temp_directory_path() / "lila_masonry_level_record";
    std::filesystem::remove_all(cache);
    lila::PrefStore::get().set("unreal", "/cacheDir", cache.generic_string());
    std::string level = "/Game/Test/Ruins", guid = "AAAA";
    std::mutex m;
    std::vector<json> removed;
    FakeServer server([&](const json& req) -> std::optional<json> {
        std::lock_guard lock(m);
        const std::string method = req["method"];
        if (method == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"}, {"features", {"progress", "placements", "levelInfo"}}});
        if (method == "levelInfo")
            return FakeServer::reply(req, {{"level", level}, {"guid", guid}, {"partitioned", false}});
        if (method == "importAsset")
            return FakeServer::reply(req, {{"assets", json::array()}, {"warnings", json::array()}, {"errors", json::array()}});
        if (method == "placeInstances")
            return FakeServer::reply(req, {{"spawned", req["params"]["placements"].size()}});
        if (method == "removePlacements")
        {
            removed.push_back(req["params"]["groups"]);
            return FakeServer::reply(req, {{"removed", req["params"]["groups"].size()}});
        }
        return std::nullopt;
    });
    lila::link::Link::ConnectOptions o;
    o.port = server.port();
    lila::link::Link::get().connect(o);
    auto ship = [&](double chunk) {
        return run("masonryShip", {{"args", {w["mesh"]}}, {"mode", "merged"}, {"chunkSize", chunk}, {"textures", false}, {"material", "none"}, {"wait", true}});
    };
    auto setLevel = [&](const std::string& l, const std::string& g) {
        std::lock_guard lock(m);
        level = l;
        guid = g;
    };

    const json first = ship(200.0);
    CHECK(first["removedGroups"] == 0);
    CHECK_FALSE(first.contains("recordIgnored"));
    // Bigger chunks: the same level drops the groups the wall no longer has.
    const json second = ship(600.0);
    CHECK(second["removedGroups"].get<int64_t>() > 0);
    CHECK_FALSE(second.contains("recordIgnored"));
    CHECK(removed.size() == 1);

    // The level deleted and made again under the same path: the record is set aside and nothing is removed.
    setLevel("/Game/Test/Ruins", "BBBB");
    const json remade = ship(200.0);
    CHECK(remade["removedGroups"] == 0);
    REQUIRE(remade.contains("recordIgnored"));
    CHECK(remade["recordIgnored"].get<std::string>().find("deleted and made again") != std::string::npos);
    CHECK(removed.size() == 1);
    // The record now names the new level and is trusted again.
    const json again = ship(600.0);
    CHECK(again["removedGroups"].get<int64_t>() > 0);
    CHECK_FALSE(again.contains("recordIgnored"));
    CHECK(removed.size() == 2);

    // Another level keeps its own record: the wall's first send there removes nothing, and the first level's record stays.
    ship(200.0);
    setLevel("/Game/Other/Ruins", "CCCC");
    const json other = ship(600.0);
    CHECK(other["removedGroups"] == 0);
    CHECK_FALSE(other.contains("recordIgnored"));
    setLevel("/Game/Test/Ruins", "BBBB");
    const json back = ship(600.0);
    CHECK(back["removedGroups"].get<int64_t>() > 0);
    CHECK(removed.size() == 3);

    // A plugin without GUIDs: the record is trusted, with a note.
    setLevel("/Game/Test/Ruins", "");
    CHECK(ship(200.0).contains("levelUnchecked"));
    lila::link::Link::get().disconnect();
    std::filesystem::remove_all(cache);
}

TEST_CASE("masonry: collision hulls cover the wall, one convex piece per segment and band")
{
    WallParams p;
    const std::vector<geo::Mesh> straight = collisionHulls(line({{0, 0, 0}, {400, 0, 0}}), p);
    REQUIRE(straight.size() == 1);
    CHECK(closedManifold(straight[0]));
    CHECK(signedVolume(straight[0]) == doctest::Approx(400.0 * p.thickness * p.height));
    CHECK(collisionHulls(line({{0, 0, 0}, {400, 0, 0}, {400, 0, 300}}), p, 3).size() == 6);
    const std::vector<geo::Mesh> square = collisionHulls(line({{0, 0, 0}, {300, 0, 0}, {300, 0, 300}, {0, 0, 300}}, true), p);
    CHECK(square.size() == 4);
    for (const geo::Mesh& h : square)
        CHECK(signedVolume(h) > 0.0);
}

TEST_CASE("masonry: stones and core carry their material slots, and merge per chunk")
{
    StoneShape s;
    const auto slots = lila::scene::materials::slotUsage(stoneMesh(s));
    REQUIRE(slots.size() == 1);
    CHECK(slots[0].first == kStoneSlot);
    CHECK(lila::scene::materials::slotUsage(coreMesh(line({{0, 0, 0}, {100, 0, 0}}), WallParams{}))[0].first == kMortarSlot);

    BuildOptions o;
    o.lods = true;
    const Footprint f = line({{0, 0, 100}, {900, 0, 100}});
    const Built b = buildWalls(std::span<const Footprint>(&f, 1), o);
    ShipOptions so;
    so.mode = "merged";
    const json plan = shipPlan(b, glm::dmat4(1.0), "k", "Wall", so);
    CHECK(plan["groups"].size() == plan["chunks"].size() + 1);
    size_t covered = 0;
    for (const json& m : plan["members"])
        covered += m.size();
    CHECK(covered == b.stones.size());
    for (const json& as : plan["assets"])
        CHECK((as["kind"] == "chunk" || as["kind"] == "core"));

    const std::vector<int> first = plan["members"][0].get<std::vector<int>>();
    const json& at = plan["origins"][0];
    const glm::dvec3 origin(at[0].get<double>(), at[1].get<double>(), at[2].get<double>());
    const geo::Mesh boxes = mergeStones(b, first, 2, origin);
    CHECK(boxes.numFaces() == 6 * first.size());
    CHECK(boxes.bounds().min.y > -1.0);
    CHECK(mergeStones(b, first, 0, origin).numFaces() > boxes.numFaces());
}

TEST_CASE("masonry: the core writes with UCX collision")
{
    WallParams p;
    const Footprint f = line({{0, 0, 0}, {300, 0, 0}, {300, 0, 200}});
    const geo::Mesh core = coreMesh(f, p);
    const std::vector<geo::Mesh> hulls = collisionHulls(f, p);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "lila_masonry_test" / "SM_Core.fbx";
    std::string err;
    REQUIRE(writeLodFbx(std::span<const geo::Mesh>(&core, 1), "SM_Core", file, &err, hulls));
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(bytes.find("UCX_SM_Core_00") != std::string::npos);
    CHECK(bytes.find("UCX_SM_Core_01") != std::string::npos);
    CHECK(bytes.find("LodGroup") == std::string::npos);
}

namespace
{
/** uv0 bounds of a mesh's corners. */
std::pair<glm::dvec2, glm::dvec2> uvBounds(const geo::Mesh& m)
{
    glm::dvec2 lo(1e300), hi(-1e300);
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            for (geo::Index c : m.faceCorners(f))
            {
                lo = glm::min(lo, glm::dvec2(uv[c]));
                hi = glm::max(hi, glm::dvec2(uv[c]));
            }
    return {lo, hi};
}

std::filesystem::path scratch(const char* name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lila_masonry_test" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}
} // namespace

TEST_CASE("masonry: shelf packing keeps rectangles apart and inside the square")
{
    std::vector<glm::dvec2> sizes;
    for (int k = 0; k < 60; ++k)
        sizes.push_back({5.0 + (k * 7) % 23, 2.0 + (k * 3) % 11});
    const double pad = 4.0 / 1024.0;
    const Packed p = shelfPack(sizes, pad);
    REQUIRE(p.scale > 0.0);
    for (size_t a = 0; a < sizes.size(); ++a)
    {
        const glm::dvec2 lo = p.origin[a], hi = lo + sizes[a] * p.scale;
        CHECK(lo.x >= pad - 1e-12);
        CHECK(lo.y >= pad - 1e-12);
        CHECK(hi.x <= 1.0 - pad + 1e-12);
        CHECK(hi.y <= 1.0 - pad + 1e-12);
        for (size_t b = a + 1; b < sizes.size(); ++b)
        {
            const glm::dvec2 lo2 = p.origin[b], hi2 = lo2 + sizes[b] * p.scale;
            const bool apart = hi.x + pad <= lo2.x + 1e-12 || hi2.x + pad <= lo.x + 1e-12 || hi.y + pad <= lo2.y + 1e-12 || hi2.y + pad <= lo.y + 1e-12;
            CHECK(apart);
        }
    }
    // Most of the square is used.
    double area = 0.0;
    for (const glm::dvec2& s : sizes)
        area += s.x * s.y * p.scale * p.scale;
    CHECK(area > 0.4);
}

TEST_CASE("masonry: the stone atlas gives every LOD of a mesh the same island, apart from other meshes")
{
    BuildOptions o;
    o.lods = true;
    const Footprint f = line({{0, 0, 0}, {400, 0, 0}, {400, 0, 200}});
    Built b = buildWalls(std::span<const Footprint>(&f, 1), o);
    const double density = atlasStones(b, 2048, 8);
    CHECK(density > 0.5);
    std::vector<std::pair<glm::dvec2, glm::dvec2>> boxes;
    for (const auto& chain : b.lods)
    {
        const auto box = uvBounds(chain[2]);
        CHECK(box.first.x >= 0.0);
        CHECK(box.second.y <= 1.0);
        for (size_t l = 0; l < 3; ++l)
        {
            const auto lb = uvBounds(chain[l]);
            CHECK(lb.first.x >= box.first.x - 1e-4);
            CHECK(lb.second.x <= box.second.x + 1e-4);
            CHECK(lb.first.y >= box.first.y - 1e-4);
            CHECK(lb.second.y <= box.second.y + 1e-4);
            CHECK(chain[l].cornerAttrs().has(geo::attr::uvName(1)));
            CHECK(lila::scene::materials::slotUsage(chain[l])[0].first == (l == 0 ? kStoneSlot : kStoneFarSlot));
        }
        boxes.push_back(box);
    }
    CHECK(boxes.size() == b.meshes.size());
}

TEST_CASE("masonry: low poly shells cover the wall chunk by chunk")
{
    WallParams p;
    const Footprint f = line({{0, 0, 0}, {1000, 0, 0}, {1000, 0, 600}});
    const std::vector<Shell> shells = lowPolyShells(std::span<const Footprint>(&f, 1), p, 400.0, 1024, 8);
    CHECK(shells.size() == 4);
    double volume = 0.0;
    for (const Shell& sh : shells)
    {
        CHECK(closedManifold(sh.mesh));
        CHECK(!sh.hulls.empty());
        volume += signedVolume(sh.mesh);
        const auto uv = uvBounds(sh.mesh);
        CHECK(uv.first.x >= 0.0);
        CHECK(uv.second.x <= 1.0);
        CHECK(uv.second.y <= 1.0);
        CHECK(lila::scene::materials::slotUsage(sh.mesh)[0].first == kShellSlot);
    }
    // 16 m of wall at full thickness and height; the corner's mitre adds and takes back the same.
    CHECK(volume == doctest::Approx(1600.0 * p.thickness * p.height).epsilon(0.01));
}

TEST_CASE("masonry: the atlas and the shells bake on the cpu")
{
    BuildOptions o;
    o.lods = true;
    o.wall.height = 40.0;
    const Footprint f = line({{0, 0, 0}, {120, 0, 0}});
    Built b = buildWalls(std::span<const Footprint>(&f, 1), o);
    atlasStones(b, 256, 4);
    BakeSettings s;
    s.folder = scratch("atlas");
    s.size = 256;
    s.padding = 4;
    s.backend = "cpu";
    const json atlas = bakeAtlas(b, o.bevel, "Test_Atlas", s);
    REQUIRE(atlas["outputs"].size() == 3);
    for (const json& out : atlas["outputs"])
        CHECK(std::filesystem::exists(out["file"].get<std::string>()));
    CHECK(atlas["stats"]["hits"].get<int64_t>() > 0);

    const std::vector<Shell> shells = lowPolyShells(std::span<const Footprint>(&f, 1), o.wall, 100.0, 256, 4);
    s.folder = scratch("shells");
    const json baked = bakeShells(b, shells, o.wall, "Test", s);
    CHECK(baked["outputs"].size() == 3 * shells.size());
    CHECK(baked["stats"]["hits"].get<int64_t>() > baked["stats"]["misses"].get<int64_t>());
}

TEST_CASE("masonry: merged stones carry their random and height as vertex colour")
{
    BuildOptions o;
    o.lods = true;
    const Footprint f = line({{0, 0, 0}, {200, 0, 0}});
    const Built b = buildWalls(std::span<const Footprint>(&f, 1), o);
    std::vector<int> all(b.stones.size());
    for (size_t k = 0; k < all.size(); ++k)
        all[k] = int(k);
    const geo::Mesh m = mergeStones(b, all, 1, glm::dvec3(0.0));
    REQUIRE(geo::colors::has(m, kStoneColorSet));
    float lowest = 1.0f, highest = 0.0f;
    for (const glm::vec4& c : geo::colors::values(m, kStoneColorSet))
    {
        CHECK(c.r >= 0.0f);
        CHECK(c.r <= 1.0f);
        lowest = std::min(lowest, c.g);
        highest = std::max(highest, c.g);
    }
    CHECK(lowest == doctest::Approx(0.0f));
    CHECK(highest == doctest::Approx(1.0f));
}
