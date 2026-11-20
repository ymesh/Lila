// Masonry bonds, chipped stones, paving, groin vaults, blockouts, plaster and ivy, the ruin brush, the structural pass,
// ruined shells, destruction links, wedges and the commands around them.

#include "TestUtil.h"
#include "Masonry.h"
#include "MasonryBake.h"
#include "MasonryBuilding.h"
#include "MasonryRuin.h"
#include "MasonryTools.h"

#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <set>

using namespace lila;
using namespace masonry;
using namespace masonrytest;
using json = nlohmann::json;

namespace
{
void load(const char* package, const char* node)
{
    if (!gn::findDecl(node))
        REQUIRE(!extensions::ExtensionManager::get().load(package).empty());
}

double volume(const geo::Mesh& m)
{
    double v = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            const std::vector<geo::Index> p = m.facePoints(f);
            for (size_t k = 1; k + 1 < p.size(); ++k)
                v += glm::dot(m.position(p[0]), glm::cross(m.position(p[k]), m.position(p[k + 1]))) / 6.0;
        }
    return v;
}

geo::Mesh box(const glm::dvec3& lo, const glm::dvec3& hi)
{
    std::array<glm::dvec3, 8> c;
    for (int i = 0; i < 8; ++i)
        c[size_t(i)] = {(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
    return hexMesh(c);
}
} // namespace

TEST_CASE("masonry bonds: herringbone, ashlar, rubble and dry stone lay clean walls")
{
    for (Bond bond : {Bond::Herringbone, Bond::Ashlar, Bond::Rubble, Bond::DryStone})
    {
        CAPTURE(bondName(bond));
        WallParams p;
        p.bond = bond;
        p.stone = {40.0, 20.0, 25.0, 1.5};
        p.height = 240.0;
        p.thickness = 60.0;
        const Layout l = layoutWall(line({{0, 0, 0}, {600, 0, 0}, {600, 0, 300}}), p);
        REQUIRE(l.stones.size() > 40);
        CHECK(countOverlaps(l.stones, 0.01) == 0);
        std::set<int64_t> lengths;
        for (const Stone& s : l.stones)
            lengths.insert(int64_t(std::lround(s.size.x)));
        if (bond == Bond::Herringbone)
        {
            int leaning = 0;
            for (const Stone& s : l.stones)
                leaning += std::abs(std::abs(s.frame[0].y) - std::sqrt(0.5)) < 0.05 ? 1 : 0;
            CHECK(leaning > int(l.stones.size()) / 2);
        }
        else
            CHECK(lengths.size() > 10);
        if (bond == Bond::DryStone)
        {
            int topHeaders = 0, top = 0;
            for (const Stone& s : l.stones)
                if (s.course == l.courses - 1)
                {
                    ++top;
                    topHeaders += s.role == Role::Header || s.face == Face::Through ? 1 : 0;
                }
            CHECK(topHeaders > top / 2);
        }
        if (bond == Bond::Rubble)
        {
            bool lower = false;
            for (const Stone& s : l.stones)
                lower = lower || s.size.y < l.pitch - p.stone.joint - 1.0;
            CHECK(lower);
        }
    }
}

TEST_CASE("masonry chips: worn stones lose chips at edges, more on old and damaged ones")
{
    StoneShape s;
    s.size = {60.0, 30.0, 35.0};
    s.bevel = 1.5;
    s.seed = 4;
    const geo::Mesh plain = stoneMesh(s);
    s.chips = 0.7;
    const geo::Mesh chipped = stoneMesh(s);
    CHECK(chipped.validate().empty());
    CHECK(chipped.numFaces() > plain.numFaces());
    CHECK(volume(chipped) < volume(plain) - 1.0);
    CHECK(volume(chipped) > volume(plain) * 0.8);
    CHECK(chipped.bounds().max.x <= 30.0 + 1e-6);

    Stone young, old;
    old.age = 1.0f;
    old.damage = 1.0f;
    CHECK(erosionLevel(young) == 0);
    CHECK(erosionLevel(old) == 2);
}

TEST_CASE("masonry paving: cobbles, flags and roof tiles inside their outline, apart")
{
    PavingSpec p;
    p.outline = {{0, 0, 0}, {300, 0, 0}, {300, 0, 200}, {0, 0, 200}};
    for (const char* kind : {"cobbles", "flags", "tiles"})
    {
        CAPTURE(kind);
        p.kind = kind;
        p.size = std::string(kind) == "flags" ? 50.0 : (std::string(kind) == "tiles" ? 25.0 : 12.0);
        const std::vector<Stone> s = pavingStones(p, 1.0, 3);
        REQUIRE(s.size() > 10);
        CHECK(countOverlaps(s, 0.05) == 0);
        for (const Stone& st : s)
        {
            CHECK(st.center.x > -1.0);
            CHECK(st.center.x < 301.0);
            CHECK(st.role == Role::Paving);
        }
        if (std::string(kind) == "tiles")
        {
            double lowZ = 1e300, highZ = -1e300, lowY = 0.0, highY = 0.0;
            for (const Stone& st : s)
            {
                if (st.center.z < lowZ)
                {
                    lowZ = st.center.z;
                    lowY = st.center.y;
                }
                if (st.center.z > highZ)
                {
                    highZ = st.center.z;
                    highY = st.center.y;
                }
            }
            CHECK(highY > lowY + 50.0);
        }
    }
}

TEST_CASE("masonry groin vault: two barrels crossing, each keeping its own web")
{
    VaultSpec v;
    v.groin = true;
    v.centre = {0, 400, 0};
    v.profile.span = 500.0;
    v.profile.ring = 40.0;
    const std::vector<Stone> s = vaultStones(v, Units{50.0, 25.0, 30.0, 1.0}, 2);
    REQUIRE(s.size() > 100);
    CHECK(countOverlaps(s, 0.01) == 0);
    int alongX = 0, alongZ = 0;
    for (const Stone& st : s)
        (std::abs(st.center.x) > std::abs(st.center.z) ? alongX : alongZ) += 1;
    CHECK(alongX > 20);
    CHECK(alongZ > 20);
}

TEST_CASE("masonry blockouts: hollow, solid and tapering blockouts become wall footprints")
{
    geo::Mesh hollow = box({-200, 0, -200}, {200, 600, 200});
    hollow.append(box({-150, -1, -150}, {150, 601, 150}));
    double t = 0.0;
    std::vector<Footprint> f = footprintsFromBlockout(hollow, 0.0, 0.0, &t);
    REQUIRE(f.size() == 1);
    CHECK(f[0].closed);
    CHECK(f[0].points.size() == 4);
    CHECK(t == doctest::Approx(50.0).epsilon(0.05));
    CHECK(f[0].heights[0] == doctest::Approx(600.0).epsilon(0.01));
    CHECK(std::abs(f[0].points[0].x) == doctest::Approx(175.0).epsilon(0.02));

    f = footprintsFromBlockout(box({0, 0, 0}, {500, 300, 400}), 0.0, 0.0, &t);
    REQUIRE(f.size() == 1);
    CHECK(t == doctest::Approx(40.0));

    // A frustum: each band steps in.
    std::array<glm::dvec3, 8> c;
    for (int i = 0; i < 8; ++i)
    {
        const double r = (i & 2) ? 150.0 : 300.0;
        c[size_t(i)] = {(i & 1) ? r : -r, (i & 2) ? 800.0 : 0.0, (i & 4) ? r : -r};
    }
    f = footprintsFromBlockout(hexMesh(c), 30.0, 200.0, &t);
    CHECK(f.size() == 4);
    if (f.size() == 4)
        CHECK(std::abs(f[3].points[0].x) < std::abs(f[0].points[0].x) - 50.0);
    const Built b = buildWalls(f, [] {
        BuildOptions o;
        o.wall.stone = {40.0, 20.0, 25.0, 1.0};
        o.wall.thickness = 30.0;
        return o;
    }());
    CHECK(b.stones.size() > 200);
}

TEST_CASE("masonry dressing: plaster peels and ivy climbs from the ground")
{
    // The ivy is grown by Weathering, which Masonry loads as a dependency.
    masonrytest::loadMasonry();
    BuildOptions o;
    o.wall.stone = {40.0, 20.0, 25.0, 1.0};
    o.wall.thickness = 60.0;
    o.wall.height = 300.0;
    const std::vector<Footprint> f = {line({{0, 0, 0}, {800, 0, 0}})};
    const Plan plan = planWalls(f, o);
    AgeParams a;
    a.age = 0.8;
    a.plaster = 0.6;
    a.ivy = 1.0;
    const Built b = realizePlan(plan, RuinParams{}, a, false);
    REQUIRE(b.dressing.numFaces() > 50);
    std::set<std::string> slots;
    for (const auto& [slot, count] : lila::scene::materials::slotUsage(b.dressing))
        slots.insert(slot);
    CHECK(slots.count(kPlasterSlot) == 1);
    CHECK(slots.count(kIvySlot) == 1);
    // Less plaster, fewer tiles.
    a.ivy = 0.0;
    a.plaster = 0.2;
    const Built thin = realizePlan(plan, RuinParams{}, a, false);
    a.plaster = 1.0;
    const Built full = realizePlan(plan, RuinParams{}, a, false);
    CHECK(thin.dressing.numFaces() < full.dressing.numFaces());
}

TEST_CASE("masonry ruin brush: strokes knock stones out or hold them up, deterministically")
{
    BuildOptions o;
    o.wall.stone = {40.0, 20.0, 25.0, 1.0};
    o.wall.thickness = 60.0;
    o.wall.height = 300.0;
    const Plan plan = planWalls(std::vector<Footprint>{line({{0, 0, 0}, {800, 0, 0}})}, o);
    RuinParams r;
    r.strokes = strokesFromJson(json::array({{{"points", pts({{300, 250, 0}, {500, 250, 0}})}, {"radius", 80.0}, {"strength", 1.0}}}));
    RuinStats st;
    const Built b = realizePlan(plan, r, AgeParams{}, false, &st);
    CHECK(st.painted > 10);
    CHECK(countFloating(b.stones, b.pitch * 0.15) == 0);
    // A holding stroke where a break would take stones keeps them.
    RuinParams broken;
    broken.left = 0.4;
    RuinStats plain;
    realizePlan(plan, broken, AgeParams{}, false, &plain);
    broken.strokes = strokesFromJson(json::array({{{"points", pts({{0, 200, 0}, {800, 200, 0}})}, {"radius", 150.0}, {"strength", -1.0}}}));
    RuinStats held;
    realizePlan(plan, broken, AgeParams{}, false, &held);
    CHECK(held.left > plain.left);
    CHECK_THROWS_AS(strokesFromJson(json("not json")), std::runtime_error);
}

TEST_CASE("masonry structural pass: an arch stands with its keystone and falls without it")
{
    ArchSpec a;
    a.span = 200.0;
    a.ring = 40.0;
    a.unit = 25.0;
    a.joint = 0.5;
    auto arch = [&](bool keystone) {
        std::vector<Stone> s;
        for (double side : {-1.0, 1.0})
        {
            Stone pier;
            pier.size = {60.0, 200.0, 60.0};
            pier.center = {side * 130.0, 100.0, 0.0};
            pier.grounded = true;
            pier.t1 = 1.0;
            s.push_back(pier);
        }
        for (Stone v : archRing(a, glm::dvec3(0, 200.5, 0), glm::dvec3(1, 0, 0), 60.0))
            if (keystone || v.role != Role::Keystone)
                s.push_back(v);
        return s;
    };
    RuinParams r;
    r.structural = true;
    r.rubble = 0.0;
    std::vector<Stone> whole = arch(true);
    const RuinStats standing = ruinStones(whole, r, 20.0);
    CHECK(standing.unstable <= 2);
    std::vector<Stone> broken = arch(false);
    const RuinStats fell = ruinStones(broken, r, 20.0);
    CHECK(fell.unstable > 4);
    CHECK(fell.unstable > standing.unstable);
}

TEST_CASE("masonry ruined shells follow the wall as it stands")
{
    BuildOptions o;
    o.wall.stone = {40.0, 20.0, 25.0, 1.0};
    o.wall.thickness = 60.0;
    o.wall.height = 400.0;
    const Plan plan = planWalls(std::vector<Footprint>{line({{0, 0, 0}, {1200, 0, 0}})}, o);
    const Built intact = realizePlan(plan, RuinParams{}, AgeParams{}, false);
    RuinParams r;
    r.left = 0.4;
    const Built ruined = realizePlan(plan, r, AgeParams{}, false);
    auto total = [](const std::vector<Shell>& shells) {
        double v = 0.0;
        for (const Shell& s : shells)
            v += volume(s.mesh);
        return v;
    };
    const std::vector<Shell> a = shellsFromCells(intact.shellCells, 400.0, 512, 4), b = shellsFromCells(ruined.shellCells, 400.0, 512, 4);
    CHECK(a.size() == 3);
    CHECK(total(a) == doctest::Approx(1200.0 * 60.0 * 400.0).epsilon(0.03));
    CHECK(total(b) < total(a) * 0.75);
    for (const Shell& s : b)
        CHECK(!s.hulls.empty());
}

TEST_CASE("masonry links and contact sheets")
{
    WallParams p;
    p.stone = {40.0, 20.0, 25.0, 1.0};
    p.thickness = 60.0;
    p.height = 120.0;
    Layout l = layoutWall(line({{0, 0, 0}, {300, 0, 0}}), p);
    const std::vector<StoneLink> links = stoneLinks(l.stones, 1.0);
    int anchors = 0;
    for (const StoneLink& k : links)
    {
        anchors += k.b < 0 ? 1 : 0;
        CHECK(k.area > 0.0);
    }
    CHECK(anchors > 0);
    CHECK(links.size() > l.stones.size());

    std::vector<std::vector<Stone>> variants = {l.stones, l.stones};
    variants[1].resize(variants[1].size() / 2);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "lila_masonry_test" / "sheet.png";
    std::string err;
    REQUIRE(writeContactSheet(variants, glm::dvec3(0, 0, -1), 128, file, &err));
    CHECK(std::filesystem::file_size(file) > 500);
}

TEST_CASE("masonry commands: from a blockout, painted, wedged and made destructible")
{
    load(LILA_TEST_MASONRY_PACKAGE, "MasonryWall");
    load(LILA_TEST_DESTRUCTION_PACKAGE, "RigidBodySolver");
    run("newScene");
    const json cube = run("polyCube", {{"width", 400}, {"height", 300}, {"depth", 300}});
    const std::string blockout = cube.is_array() ? cube[0].get<std::string>() : cube.get<std::string>();
    run("xform", {{"args", {blockout}}, {"translation", {0.0, 150.0, 0.0}}});
    const json walls = run("masonryFromMesh", {{"args", {blockout}}, {"stoneLength", 40.0}, {"stoneHeight", 20.0}, {"stoneDepth", 25.0}, {"bond", "ashlar"}});
    CHECK(walls["footprints"].get<int64_t>() == 1);
    CHECK(walls["stones"].get<int64_t>() > 300);
    const std::string mesh = walls["mesh"].get<std::string>();
    CHECK(run("masonryCheck", {{"args", {mesh}}})["ok"].get<bool>());

    const json painted = run("ruinPaint", {{"args", {mesh}}, {"points", pts({{0, 250, -150}, {150, 250, -150}})}, {"radius", 80.0}});
    CHECK(painted["painted"].get<int64_t>() > 5);
    CHECK(painted["strokes"].get<int64_t>() == 1);
    run("undo");
    CHECK(run("masonryInfo", {{"args", {mesh}}})["ruin"]["painted"].get<int64_t>() == 0);
    run("redo");

    const json wedge = run("masonryWedge", {{"args", {mesh}}, {"count", 4}, {"vary", {{"left", {0.8, 0.6, 0.4, 0.2}}}}});
    REQUIRE(wedge["variants"].size() == 4);
    CHECK(std::filesystem::exists(wedge["sheet"].get<std::string>()));
    CHECK(wedge["variants"][3]["ruin"]["left"].get<double>() < wedge["variants"][0]["ruin"]["left"].get<double>());
    run("masonryWedge", {{"args", {mesh}}, {"count", 4}, {"vary", {{"left", {0.8, 0.6, 0.4, 0.2}}}}, {"apply", 2}});
    CHECK(run("masonryInfo", {{"args", {mesh}}})["settings"]["left"].get<double>() == doctest::Approx(0.4));

    const json d = run("makeDestructible", {{"args", {mesh}}, {"strength", 25.0}});
    CHECK(d["links"].get<int64_t>() > d["pieces"].get<int64_t>());
    CHECK(d["anchors"].get<int64_t>() > 0);
    CHECK_THROWS(run("makeDestructible", {{"args", {mesh}}}));
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(mesh);
    REQUIRE(n);
    const scene::MeshPtr out = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
    REQUIRE(out);
    CHECK(out->numFaces() > 1000);

    // The strokes and the destruction save with the scene.
    const json before = run("masonryInfo", {{"args", {mesh}}});
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "lila_masonry_test" / "painted.lila";
    std::filesystem::create_directories(file.parent_path());
    run("saveScene", {{"args", {file.string()}}});
    run("newScene");
    run("openScene", {{"args", {file.string()}}});
    const json reopened = run("masonryInfo", {{"args", {mesh}}});
    CHECK(reopened["ruin"] == before["ruin"]);
    CHECK_THROWS(run("makeDestructible", {{"args", {mesh}}}));
}
