#include <Arrange.h>
#include <RigidScene.h>

#include "TestUtil.h"
#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <algorithm>
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

using namespace lila;
using namespace stagehandtest;
using json = nlohmann::json;
namespace physics = lila::physics;

namespace
{
void loadStagehand()
{
    if (!ops::registry().find("settle"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_STAGEHAND_PACKAGE) == "Stagehand");
}

/** A row of upright books on the floor, 2 cm apart, along X. */
void bookRow(physics::RigidScene& s, int count)
{
    for (int k = 0; k < count; ++k)
        s.add(box({k * 6.0, 12.0, 0.0}, {2.0, 12.0, 8.0}));
}

double worldY(const std::string& node) { return run("xform", {{"args", {node}}, {"query", true}, {"worldSpace", true}, {"translation", true}})[1].get<double>(); }
} // namespace

TEST_CASE("physics: a box dropped on the ground comes to rest on it")
{
    physics::Settings st;
    st.ground = true;
    physics::RigidScene s(st);
    const int b = s.add(box({0, 50, 0}, {10, 10, 10}));
    s.settle(5.0);
    CHECK(s.atRest());
    CHECK(s.pose(b).position.y == doctest::Approx(10.0).epsilon(0.02));
    CHECK(s.penetration(b) < 0.5);
    const auto contacts = s.contacts();
    REQUIRE(contacts.size() == 1);
    CHECK(contacts[0].a == b);
    CHECK(contacts[0].b == -1);
    CHECK(contacts[0].resting);
    CHECK(contacts[0].peakImpulse > 0.0);
}

TEST_CASE("physics: the same calls give the same bytes, threaded or not")
{
    auto runOnce = [] {
        physics::Settings st;
        st.ground = true;
        physics::RigidScene s(st);
        bookRow(s, 12);
        s.impulse(0, glm::dvec3(150.0, 0, 0) * s.mass(0), {-2.0, 20.0, 0.0});
        s.settle(4.0);
        return s.save();
    };
    const std::vector<uint8_t> threaded = runOnce();
    CHECK(runOnce() == threaded);
#ifdef _WIN32
    _putenv_s("LILA_PHYSICS_THREADS", "1");
#else
    setenv("LILA_PHYSICS_THREADS", "1", 1);
#endif
    const std::vector<uint8_t> serial = runOnce();
#ifdef _WIN32
    _putenv_s("LILA_PHYSICS_THREADS", "");
#else
    unsetenv("LILA_PHYSICS_THREADS");
#endif
    CHECK(serial == threaded);
}

TEST_CASE("physics: pushing the first book topples the row like dominoes")
{
    physics::Settings st;
    st.ground = true;
    physics::RigidScene s(st);
    bookRow(s, 8);
    s.impulse(0, glm::dvec3(150.0, 0, 0) * s.mass(0), {-2.0, 20.0, 0.0});
    s.settle(6.0);
    CHECK(s.atRest());
    // The last book went over: its tall axis is no longer up.
    const glm::dvec3 up = s.pose(7).rotation * glm::dvec3(0, 1, 0);
    CHECK(up.y < 0.95);
    for (int k = 0; k < 8; ++k)
        CHECK(s.penetration(k) < 1.0);
}

TEST_CASE("physics: a dynamic mesh body shaken as kinematic goes back to dynamic; a static one can't go dynamic")
{
    physics::Settings st;
    st.ground = true;
    physics::RigidScene s(st);
    auto cube = std::make_shared<std::vector<glm::dvec3>>();
    const glm::dvec3 c[8] = {{-5, 0, -5}, {5, 0, -5}, {5, 10, -5}, {-5, 10, -5}, {-5, 0, 5}, {5, 0, 5}, {5, 10, 5}, {-5, 10, 5}};
    for (const int f : {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2})
        cube->push_back(c[f]);
    physics::BodyDesc m;
    m.shape.kind = physics::Shape::Kind::Mesh;
    m.shape.points = cube;
    m.position = {0, 20, 0};
    const int moving = s.add(m);
    s.setRole(moving, physics::Role::Kinematic);
    CHECK_NOTHROW(s.setRole(moving, physics::Role::Dynamic));
    m.role = physics::Role::Static;
    m.position = {40, 0, 0};
    const int fixed = s.add(m);
    CHECK_THROWS_AS(s.setRole(fixed, physics::Role::Dynamic), std::invalid_argument);
}

TEST_CASE("physics: a held body follows the hand and still collides")
{
    physics::Settings st;
    st.ground = true;
    physics::RigidScene s(st);
    const int b = s.add(box({0, 10, 0}, {10, 10, 10}));
    s.add(box({40, 10, 0}, {10, 10, 10}, physics::Role::Static));
    for (int k = 0; k < 240; ++k)
    {
        s.pull(b, {0, 0, 0}, {0, 40, 0}, 6.0, 1.0 / 120.0);
        s.step(1.0 / 120.0);
    }
    CHECK(s.pose(b).position.y == doctest::Approx(40.0).epsilon(0.1));
    // Dragged into the static box, it stops at its face instead of passing through.
    for (int k = 0; k < 240; ++k)
    {
        s.pull(b, {0, 0, 0}, {40, 10, 0}, 6.0, 1.0 / 120.0);
        s.step(1.0 / 120.0);
    }
    CHECK(s.pose(b).position.x < 21.5);
}

TEST_CASE("stagehand: settle drops a mesh onto the one below and reports what it rests on")
{
    loadStagehand();
    run("newScene");
    const std::string table = cube("Table", {0, 0, 0}, {120, 75, 80});
    const std::string mug = cube("Mug", {10, 120, 5}, {8, 10, 8});
    const json r = run("settle", {{"args", {mug}}});
    CHECK(r["atRest"].get<bool>());
    CHECK(worldY(mug) == doctest::Approx(75.0).epsilon(0.02));
    REQUIRE(r["movers"].size() == 1);
    CHECK(r["movers"][0]["restsOn"][0] == table);
    // One undo step puts it back.
    run("undo");
    CHECK(worldY(mug) == doctest::Approx(120.0));
}

TEST_CASE("stagehand: push topples a row of books and the shelf they stand on stays put")
{
    loadStagehand();
    run("newScene");
    const std::string shelf = cube("Shelf", {0, 100, 0}, {150, 2, 25});
    std::vector<std::string> books;
    for (int k = 0; k < 10; ++k)
        books.push_back(cube(("Book" + std::to_string(k)).c_str(), {-60.0 + k * 6.0, 102, 0}, {3, 22, 16}));
    run("physicsBody", {{"args", books}, {"material", "book"}});
    const json r = run("push", {{"args", {books[0]}}, {"direction", json::array({1, 0, 0})}});
    CHECK(r["atRest"].get<bool>());
    CHECK(r["movers"].size() == books.size());
    CHECK(worldY(shelf) == doctest::Approx(100.0));
    CHECK(r["deepestPenetration"].get<double>() < 1.0);
    // The last book was knocked over or along by the chain.
    const json& last = r["movers"][books.size() - 1];
    CHECK((last["turned"].get<double>() > 10.0 || last["moved"].get<double>() > 2.0));
    const json check = run("dressCheck", {{"args", books}});
    CHECK(check["ok"].get<bool>());
}

TEST_CASE("stagehand: drop onto stacks the drops above the target, and dressCheck finds what floats")
{
    loadStagehand();
    run("newScene");
    const std::string desk = cube("Desk", {0, 0, 0}, {100, 70, 60});
    std::vector<std::string> blocks;
    for (int k = 0; k < 3; ++k)
        blocks.push_back(cube(("Block" + std::to_string(k)).c_str(), {300.0 + k * 20.0, 0, 0}, {10, 10, 10}));
    const json r = run("drop", {{"args", blocks}, {"onto", desk}});
    CHECK(r["atRest"].get<bool>());
    for (const std::string& b : blocks)
        CHECK(worldY(b) >= 69.0);

    const std::string floating = cube("Floating", {0, 150, 0}, {10, 10, 10});
    const json check = run("dressCheck", {{"args", {floating}}});
    CHECK_FALSE(check["ok"].get<bool>());
    REQUIRE(check["issues"].size() >= 1);
    CHECK(check["issues"][0]["kind"] == "floating");
}

TEST_CASE("stagehand: sweep brooms things along the stroke, and gather pulls them together")
{
    loadStagehand();
    run("newScene");
    auto worldXZ = [](const std::string& n) {
        const json t = run("xform", {{"args", {n}}, {"query", true}, {"worldSpace", true}, {"translation", true}});
        return glm::dvec2(t[0].get<double>(), t[2].get<double>());
    };
    std::vector<std::string> litter;
    for (int k = 0; k < 5; ++k)
        litter.push_back(cube(("Can" + std::to_string(k)).c_str(), {k * 10.0, 0, (k % 2) * 12.0 - 6.0}, {8, 8, 8}));
    double before = 0.0;
    for (const std::string& c : litter)
        before += worldXZ(c).x;
    const json r = run("sweep", {{"args", litter}, {"path", json::array({json::array({-40, 0, 0}), json::array({200, 0, 0})})}, {"radius", 40.0}});
    CHECK(r["atRest"].get<bool>());
    double after = 0.0;
    for (const std::string& c : litter)
        after += worldXZ(c).x;
    CHECK((after - before) / double(litter.size()) > 40.0);
    CHECK(run("dressCheck", {{"args", litter}})["ok"].get<bool>());
    run("undo");
    CHECK(worldXZ(litter[0]).x == doctest::Approx(0.0));

    run("newScene");
    std::vector<std::string> books;
    const glm::dvec2 corners[] = {{-100, -100}, {100, -100}, {100, 100}, {-100, 100}};
    for (int k = 0; k < 4; ++k)
        books.push_back(cube(("Book" + std::to_string(k)).c_str(), {corners[k].x, 0, corners[k].y}, {16, 4, 22}));
    run("sweep", {{"args", books}, {"path", json::array({json::array({0, 0, 0})})}, {"gather", true}, {"radius", 200.0}});
    for (const std::string& b : books)
        CHECK(glm::length(worldXZ(b)) < 100.0);
    CHECK_THROWS(run("sweep", {{"args", books}, {"path", json::array({json::array({0, 0, 0})})}}));
}

TEST_CASE("arrange: pathProps spaces items along a curve, to its side, standing on the surface below")
{
    loadStagehand();
    run("newScene");
    const std::string kerb = cube("Kerb", {0, 0, 0}, {1200, 15, 300});
    const std::string cone = cube("Cone", {0, 0, 2000}, {20, 40, 20});
    const std::string curve = run("curve", {{"type", "poly"}, {"points", json::array({json::array({-500, 15, 0}), json::array({500, 15, 0})})}, {"name", "Edge"}})
                                  .get<std::string>();
    const json r = run("pathProps", {{"args", {curve}}, {"items", {cone}}, {"spacing", 100.0}, {"spacingJitter", 0.0}, {"offset", 40.0}, {"turn", 0.0}});
    REQUIRE(r["nodes"].is_array());
    CHECK(r["nodes"].size() == 11);
    for (const json& n : r["nodes"])
    {
        const json info = run("dressInfo", {{"args", {n}}})[0];
        // Bottom on the kerb's top, 40 cm to the right of a curve running along +X.
        CHECK(info["boundsMin"][1].get<double>() == doctest::Approx(15.0).epsilon(0.01));
        CHECK((info["boundsMin"][2].get<double>() + info["boundsMax"][2].get<double>()) * 0.5 == doctest::Approx(40.0).epsilon(0.01));
    }
    run("undo");
    CHECK_FALSE(run("objExists", {{"args", {r["group"]}}}).get<bool>());
    CHECK_THROWS(run("pathProps", {{"args", {kerb}}, {"items", {cone}}}));
}

TEST_CASE("stagehand: physicsBody makes a body child with the material's numbers")
{
    loadStagehand();
    run("newScene");
    const std::string crate = cube("Crate", {0, 0, 0}, {50, 50, 50});
    const json bodies = run("physicsBody", {{"args", {crate}}, {"material", "cardboard"}, {"role", "dynamic"}});
    REQUIRE(bodies.size() == 1);
    const std::string body = bodies[0].get<std::string>();
    CHECK(run("getAttr", {{"args", {body + ".density"}}}).get<double>() == doctest::Approx(150.0));
    const json info = run("dressInfo", {{"args", {crate}}});
    CHECK(info[0]["role"] == "dynamic");
    CHECK(info[0]["shape"] == "box");
    // 0.125 m3 of cardboard.
    CHECK(info[0]["mass"].get<double>() == doctest::Approx(18.75).epsilon(0.05));
    run("physicsBody", {{"args", {crate}}, {"remove", true}});
    CHECK(scene::Graph::current().find(body) == nullptr);
}

TEST_CASE("library: a saved asset is found by words, placed as instances on a surface, and replaces blockouts")
{
    loadStagehand();
    run("newScene");
    const auto lib = std::filesystem::temp_directory_path() / ("lila_stagehand_lib_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(lib);
    std::filesystem::create_directories(lib);
    run("libraryPaths", {{"add", lib.generic_string()}});
    const std::string crate = cube("Crate", {500, 0, 0}, {40, 30, 40});
    run("physicsBody", {{"args", {crate}}, {"material", "wood"}, {"role", "dynamic"}});
    const json added = run("libraryAdd", {{"args", {crate}},
                                          {"tier", lib.generic_string()},
                                          {"collection", "Props/Storage"},
                                          {"tags", json::array({"crate", "wood"})},
                                          {"thumbnail", false}});
    CHECK(added["id"] == "Props/Storage/Crate");
    CHECK(added["size"][1].get<double>() == doctest::Approx(30.0));
    CHECK(added["physics"]["material"] == "wood");

    // Words, prefixes and a typo all find it; a size limit rules it out. The starter props are a tier of their own.
    CHECK(run("librarySearch", {{"args", {"crate"}}, {"tier", "studio"}}).size() == 1);
    CHECK(run("librarySearch", {{"args", {"wood cra"}}, {"tier", "studio"}}).size() == 1);
    CHECK(run("librarySearch", {{"args", {"crste"}}, {"tier", "studio"}}).size() == 1);
    CHECK(run("librarySearch", {{"args", {"crate"}}, {"tier", "studio"}, {"maxSize", 20.0}}).empty());

    const std::string table = cube("Table", {0, 0, 0}, {120, 75, 80});
    const json placed = run("place", {{"args", {"Props/Storage/Crate"}}, {"on", table}, {"count", 2}, {"spacing", json::array({45, 0, 0})}});
    REQUIRE(placed.size() == 2);
    for (const json& p : placed)
        CHECK(p["position"][1].get<double>() == doctest::Approx(75.0).epsilon(0.02));
    // The master is loaded once, hidden.
    CHECK(scene::Graph::current().find("StagehandLibrary") != nullptr);
    CHECK(run("getAttr", {{"args", {"StagehandLibrary.visibility"}}}).get<bool>() == false);

    const std::string box = cube("Blockout", {-300, 0, 0}, {80, 60, 80});
    const json swapped = run("replaceWith", {{"args", {box}}, {"asset", "Props/Storage/Crate"}, {"fit", true}});
    REQUIRE(swapped.size() == 1);
    CHECK(scene::Graph::current().find(box) == nullptr);

    std::error_code ec;
    std::filesystem::remove_all(lib, ec);
}

namespace
{
/** Books 3 x 22 x 16 cm about their centre. */
std::vector<stagehand::arrange::Item> books()
{
    stagehand::arrange::Item b;
    b.lo = {-1.5, -11.0, -8.0};
    b.hi = {1.5, 11.0, 8.0};
    stagehand::arrange::Item thin = b;
    thin.lo.x = -1.0;
    thin.hi.x = 1.0;
    return {b, thin};
}

/** World bounds of an item's box under a transform. */
void boxBounds(const stagehand::arrange::Item& it, const glm::dmat4& m, glm::dvec3& lo, glm::dvec3& hi)
{
    lo = glm::dvec3(1e300);
    hi = glm::dvec3(-1e300);
    for (int k = 0; k < 8; ++k)
    {
        const glm::dvec3 c((k & 1) ? it.hi.x : it.lo.x, (k & 2) ? it.hi.y : it.lo.y, (k & 4) ? it.hi.z : it.lo.z);
        const glm::dvec3 q(m * glm::dvec4(c, 1.0));
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
}
} // namespace

TEST_CASE("arrange: a row of books fills the line, stands on it and never runs past its end")
{
    namespace ar = stagehand::arrange;
    const std::vector<ar::Item> items = books();
    const glm::dvec3 line[] = {{0, 100, 0}, {150, 100, 0}};
    ar::RowOptions o;
    o.leanChance = 0.0;
    o.lyingChance = 0.0;
    const std::vector<ar::Placed> row = ar::row(line, items, o);
    REQUIRE(row.size() > 40);
    double prevHi = -1e9;
    for (const ar::Placed& p : row)
    {
        glm::dvec3 lo, hi;
        boxBounds(items[size_t(p.item)], p.transform, lo, hi);
        CHECK(lo.y == doctest::Approx(100.0).epsilon(1e-6));
        CHECK(lo.x >= -1e-6);
        CHECK(hi.x <= 150.0 + 1e-6);
        CHECK(lo.x >= prevHi - 1e-6);
        prevHi = hi.x;
    }
    // Same seed, same row; another seed, another.
    CHECK(ar::row(line, items, o).size() == row.size());
    o.seed = 7;
    const std::vector<ar::Placed> other = ar::row(line, items, o);
    CHECK((other.size() != row.size() || other[3].transform != row[3].transform));
}

TEST_CASE("arrange: leaning and lying books show up, and settling the row leaves it at rest on the shelf")
{
    namespace ar = stagehand::arrange;
    const std::vector<ar::Item> items = books();
    const glm::dvec3 line[] = {{-60, 2, 0}, {60, 2, 0}};
    ar::RowOptions o;
    o.leanChance = 0.3;
    o.lyingChance = 0.1;
    o.seed = 3;
    const std::vector<ar::Placed> row = ar::row(line, items, o);
    int leaning = 0, lying = 0;
    for (const ar::Placed& p : row)
    {
        leaning += p.pose == ar::Pose::Leaning;
        lying += p.pose == ar::Pose::Lying;
    }
    CHECK(leaning > 0);
    CHECK(lying > 0);

    lila::geo::Mesh book;
    {
        const ar::Item& b = items[0];
        lila::geo::Index v[8];
        for (int k = 0; k < 8; ++k)
            v[k] = book.addPoint({(k & 1) ? b.hi.x : b.lo.x, (k & 2) ? b.hi.y : b.lo.y, (k & 4) ? b.hi.z : b.lo.z});
        const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
        for (const auto& f : faces)
        {
            const lila::geo::Index q[4] = {v[f[0]], v[f[1]], v[f[2]], v[f[3]]};
            book.addFace(q);
        }
    }
    std::vector<glm::dmat4> xf;
    std::vector<int> idx;
    for (const ar::Placed& p : row)
    {
        xf.push_back(p.transform);
        idx.push_back(0);
    }
    const lila::geo::Mesh* meshes[] = {&book};
    ar::SettleOptions so;
    so.ground = true;
    so.groundHeight = 2.0;
    const ar::Settled s = ar::settle(xf, idx, meshes, {}, so);
    int resting = 0;
    for (uint8_t r : s.atRest)
        resting += r;
    CHECK(resting == int(row.size()));
    for (const glm::dmat4& m : s.transforms)
        CHECK(m[3].y > 2.0);
    CHECK(!s.contacts.empty());
}

TEST_CASE("arrange: scatter keeps footprints apart and piles start with nothing overlapping")
{
    namespace ar = stagehand::arrange;
    lila::geo::Mesh floor;
    const lila::geo::Index q[4] = {floor.addPoint({-200, 0, -200}), floor.addPoint({-200, 0, 200}), floor.addPoint({200, 0, 200}), floor.addPoint({200, 0, -200})};
    floor.addFace(q);
    ar::Item rock;
    rock.lo = {-5, 0, -5};
    rock.hi = {5, 8, 5};
    const std::vector<ar::Item> items{rock};
    ar::ScatterOptions o;
    o.density = 40.0;
    o.scaleMin = o.scaleMax = 1.0;
    const std::vector<ar::Placed> s = ar::scatter(floor, items, o);
    REQUIRE(s.size() > 20);
    for (size_t i = 0; i < s.size(); ++i)
        for (size_t j = i + 1; j < s.size(); ++j)
            CHECK(glm::length(glm::dvec3(s[i].transform[3]) - glm::dvec3(s[j].transform[3])) >= 10.0 - 1e-6);

    ar::PileOptions po;
    po.count = 25;
    po.radius = 30.0;
    const std::vector<ar::Placed> pile = ar::pile({0, 0, 0}, items, po);
    REQUIRE(pile.size() == 25);
    const double sphere = glm::length(rock.size()) * 0.5 * po.scaleMax;
    for (size_t i = 0; i < pile.size(); ++i)
        for (size_t j = i + 1; j < pile.size(); ++j)
        {
            // Centres of the rock's bounds, which the pile spaces by bounding spheres.
            const glm::dvec3 ci(pile[i].transform * glm::dvec4(0, 4, 0, 1)), cj(pile[j].transform * glm::dvec4(0, 4, 0, 1));
            CHECK(glm::length(ci - cj) >= 2.0 * sphere - 0.5);
        }
}

TEST_CASE("stagehand: fillShelf realized puts books on every board, and live builds an arrangement group")
{
    loadStagehand();
    run("newScene");
    const std::string b1 = cube("Board1", {0, 100, 0}, {90, 2, 25});
    const std::string b2 = cube("Board2", {0, 140, 0}, {90, 2, 25});
    const std::string book = cube("BookSrc", {300, 0, 0}, {3, 22, 16});
    const json r = run("fillShelf", {{"args", {b1, b2}}, {"items", json::array({book})}, {"realize", true}});
    CHECK(r["count"].get<int64_t>() > 30);
    CHECK(r["nodes"].size() == size_t(r["count"].get<int64_t>()));
    CHECK(r["atRest"].get<int64_t>() == r["count"].get<int64_t>());
    const json check = run("dressCheck", {{"args", r["nodes"]}});
    CAPTURE(check["issues"].dump());
    CHECK(check["ok"].get<bool>());

    const json live = run("fillShelf", {{"args", {b1}}, {"items", json::array({book})}, {"seed", 4}});
    CHECK(live.contains("modifier"));
    CHECK(live.contains("group"));
}

TEST_CASE("kit: two wall pieces join end to end by their snap points, and open points are reported")
{
    loadStagehand();
    run("newScene");
    const std::string a = cube("WallA", {0, 0, 0}, {400, 300, 20});
    const std::string b = cube("WallB", {150, 0, 300}, {400, 300, 20});
    for (const std::string& w : {a, b})
    {
        run("addSnapPoint", {{"args", {w}}, {"type", "wallEnd"}, {"position", json::array({200, 0, 0})}, {"normal", json::array({1, 0, 0})}, {"name", "east"}});
        run("addSnapPoint", {{"args", {w}}, {"type", "wallEnd"}, {"position", json::array({-200, 0, 0})}, {"normal", json::array({-1, 0, 0})}, {"name", "west"}});
    }
    CHECK(run("openSnapPoints").size() == 4);
    // Same-type ends join either way round, so name the pair: B's west onto A's east.
    const json r = run("snapTo", {{"args", {b}}, {"target", a}, {"point", b + "|west"}, {"targetPoint", a + "|east"}});
    CHECK(r.contains("point"));
    // B's west end now sits on A's east end, B running on along +X.
    const json bx = run("xform", {{"args", {b}}, {"query", true}, {"worldSpace", true}, {"translation", true}});
    CHECK(bx[0].get<double>() == doctest::Approx(400.0).epsilon(0.01));
    CHECK(std::abs(bx[2].get<double>()) < 0.01);
    CHECK(run("openSnapPoints").size() == 2);

    run("addSnapPoint", {{"args", {a}}, {"type", "doorFrame"}, {"accepts", json::array({"door", "window"})}, {"name", "door"}});
    const json pts = run("snapPoints", {{"args", {a}}});
    const auto door = std::find_if(pts.begin(), pts.end(), [](const json& p) { return p["type"] == "doorFrame"; });
    REQUIRE(door != pts.end());
    CHECK((*door)["accepts"] == json::array({"door", "window"}));
}

TEST_CASE("kit: dropToSurface lands objects on what is below, alignObjects and distribute line them up")
{
    loadStagehand();
    run("newScene");
    cube("Floor", {0, 0, 0}, {500, 10, 500});
    const std::string c1 = cube("C1", {-100, 80, 0}, {20, 20, 20});
    const std::string c2 = cube("C2", {0, 120, 30}, {20, 20, 20});
    const std::string c3 = cube("C3", {150, 60, -20}, {20, 20, 20});
    run("dropToSurface", {{"args", {c1, c2, c3}}});
    for (const std::string& c : {c1, c2, c3})
        CHECK(worldY(c) == doctest::Approx(10.0).epsilon(0.01));
    run("alignObjects", {{"args", {c1, c2, c3}}, {"axis", "z"}, {"to", "first"}});
    run("distribute", {{"args", {c1, c2, c3}}, {"axis", "x"}});
    const double x1 = run("xform", {{"args", {c1}}, {"query", true}, {"worldSpace", true}, {"translation", true}})[0].get<double>();
    const double x2 = run("xform", {{"args", {c2}}, {"query", true}, {"worldSpace", true}, {"translation", true}})[0].get<double>();
    const double x3 = run("xform", {{"args", {c3}}, {"query", true}, {"worldSpace", true}, {"translation", true}})[0].get<double>();
    CHECK(x2 - x1 == doctest::Approx(x3 - x2).epsilon(0.01));
}
