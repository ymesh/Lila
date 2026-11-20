// Masonry architecture, ruin, age and the commands around them, down to whale scale.

#include "TestUtil.h"
#include "Masonry.h"
#include "MasonryArch.h"
#include "MasonryBuilding.h"
#include "MasonryRuin.h"
#include "MasonryShip.h"

#include "extensions/ExtensionManager.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

using namespace lila;
using namespace masonry;
using namespace masonrytest;
using json = nlohmann::json;

namespace
{
double ms(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }

int countRole(std::span<const Stone> stones, Role r)
{
    int n = 0;
    for (const Stone& s : stones)
        n += s.role == r ? 1 : 0;
    return n;
}

/** A hash of every stone's placement, to prove two builds the same. */
uint64_t hashOf(std::span<const Stone> stones)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        h = (h ^ bits) * 1099511628211ull;
    };
    for (const Stone& s : stones)
    {
        for (int k = 0; k < 3; ++k)
        {
            mix(s.center[k]);
            mix(s.size[k]);
            for (int j = 0; j < 3; ++j)
                mix(s.frame[k][j]);
        }
        mix(double(s.role));
    }
    return h;
}
} // namespace

TEST_CASE("masonry arches: every kind lays radial voussoirs and a keystone that never overlap")
{
    for (ArchKind kind : {ArchKind::Round, ArchKind::Pointed, ArchKind::Segmental, ArchKind::Flat})
    {
        CAPTURE(archName(kind));
        ArchSpec a;
        a.kind = kind;
        a.span = 200.0;
        a.ring = 30.0;
        a.unit = 12.0;
        const std::vector<Stone> ring = archRing(a, glm::dvec3(0.0), glm::dvec3(1, 0, 0), 40.0);
        REQUIRE(ring.size() >= 5);
        CHECK(countRole(ring, Role::Keystone) == 1);
        CHECK(countOverlaps(ring, 0.01) == 0);
        // Every stone sits above the springing line and inside the extrados outline.
        for (const Stone& s : ring)
        {
            for (const glm::dvec3& c : stoneCorners(s))
            {
                CHECK(c.y > -0.5);
                const double half = extradosHalfWidth(a, c.y);
                CHECK(std::abs(c.x) <= std::max(half, 0.0) + 0.5);
            }
        }
        if (kind == ArchKind::Round)
            for (const Stone& s : ring)
            {
                // Radial: the stone's up axis points away from the arch's centre, within half a degree.
                const glm::dvec3 radial = glm::normalize(glm::dvec3(s.center.x, s.center.y, 0.0));
                CHECK(glm::degrees(std::acos(std::clamp(glm::dot(radial, s.frame[1]), -1.0, 1.0))) < 0.5);
            }
    }
    ArchSpec round;
    round.span = 300.0;
    CHECK(archRise(round) == doctest::Approx(150.0));
    round.kind = ArchKind::Pointed;
    CHECK(archRise(round) == doctest::Approx(300.0 * std::sqrt(3.0) * 0.5));
}

TEST_CASE("masonry openings: doors, windows and niches cut the coursing with arches, lintels, sills and jambs")
{
    WallParams p;
    p.height = 400.0;
    p.thickness = 40.0;
    Opening door;
    door.name = "door";
    door.at = 150.0;
    door.width = 90.0;
    door.height = 220.0;
    door.arch = ArchKind::Round;
    Opening window;
    window.name = "window";
    window.at = 450.0;
    window.width = 70.0;
    window.height = 140.0;
    window.sill = 120.0;
    window.arch = ArchKind::Pointed;
    Opening square;
    square.at = 700.0;
    square.width = 80.0;
    square.height = 160.0;
    square.sill = 90.0;
    square.arch = ArchKind::Lintel;
    Opening niche;
    niche.at = 900.0;
    niche.width = 60.0;
    niche.height = 120.0;
    niche.sill = 60.0;
    niche.arch = ArchKind::Segmental;
    niche.through = false;
    p.openings = {door, window, square, niche};
    const Layout l = layoutWall(line({{0, 0, 0}, {1050, 0, 0}}), p);
    REQUIRE(!l.stones.empty());
    CHECK(countOverlaps(l.stones, 0.01) == 0);
    CHECK(countRole(l.stones, Role::Keystone) == 3);
    CHECK(countRole(l.stones, Role::Lintel) == 1);
    CHECK(countRole(l.stones, Role::Sill) == 3);
    CHECK(countRole(l.stones, Role::Jamb) > 8);
    CHECK(countRole(l.stones, Role::Voussoir) > 20);
    // Nothing stands in the clear opening of the door.
    for (const Stone& s : l.stones)
    {
        const bool inDoor = std::abs(s.center.x - 150.0) < 45.0 - 1.0 && s.center.y < 150.0;
        CHECK_FALSE(inDoor);
    }
    // The niche leaves the inner face whole: inner stones behind it, none of the outer ones.
    int innerBehind = 0, outerIn = 0;
    for (const Stone& s : l.stones)
        if (std::abs(s.center.x - 900.0) < 25.0 && s.center.y > 70.0 && s.center.y < 120.0)
        {
            innerBehind += s.face == Face::Inner ? 1 : 0;
            outerIn += s.face == Face::Outer && s.role != Role::Voussoir && s.role != Role::Keystone ? 1 : 0;
        }
    CHECK(innerBehind > 0);
    CHECK(outerIn == 0);

    // The core has a hole where the door is: no cell covers the middle of the door at waist height.
    bool covered = false;
    std::vector<geo::Mesh> cells = coreCells(line({{0, 0, 0}, {1050, 0, 0}}), p, l.stones);
    for (const geo::Mesh& c : cells)
    {
        const geo::Bounds b = c.bounds();
        if (b.min.x < 150.0 && b.max.x > 150.0 && b.min.y < 100.0 && b.max.y > 100.0)
            covered = true;
    }
    CHECK_FALSE(covered);
}

TEST_CASE("masonry terrain: courses step over sloping ground, the top follows per-point heights, the batter leans in")
{
    WallParams p;
    p.stone = {40.0, 20.0, 25.0, 1.0};
    p.thickness = 60.0;
    p.height = 300.0;
    Footprint slope = line({{0, 0, 0}, {1000, 250, 0}});
    const Layout l = layoutWall(slope, p);
    REQUIRE(!l.stones.empty());
    CHECK(countOverlaps(l.stones, 0.01) == 0);
    for (const Stone& s : l.stones)
    {
        const double ground = 250.0 * std::clamp(s.center.x / 1000.0, 0.0, 1.0);
        // Never buried by more than a course, never standing on air over the ground (stepped, so up to a course).
        CHECK(s.center.y - s.size.y * 0.5 > ground - l.pitch - 1.0);
        CHECK(s.center.y + s.size.y * 0.5 < ground + p.height + l.pitch);
        CHECK(std::abs(s.frame[1].y) > 0.999);
    }
    // Courses run level: the bottom course is short, at the low end only.
    double lowestEnd = -1.0;
    for (const Stone& s : l.stones)
        if (s.course == 0)
            lowestEnd = std::max(lowestEnd, s.center.x);
    CHECK(lowestEnd < 300.0);

    Footprint gable = line({{0, 0, 0}, {300, 0, 0}, {600, 0, 0}});
    gable.heights = {200.0, 500.0, 200.0};
    const Layout g = layoutWall(gable, p);
    double midTop = 0.0, endTop = 0.0;
    for (const Stone& s : g.stones)
    {
        const double top = s.center.y + s.size.y * 0.5;
        if (std::abs(s.center.x - 300.0) < 40.0)
            midTop = std::max(midTop, top);
        if (s.center.x < 40.0)
            endTop = std::max(endTop, top);
    }
    CHECK(midTop > 450.0);
    CHECK(endTop < 260.0);
    CHECK(countOverlaps(g.stones, 0.01) == 0);

    p.batter = 4.0;
    p.height = 600.0;
    const Layout b = layoutWall(line({{0, 0, 0}, {500, 0, 0}}), p);
    double bottomSpread = 0.0, topSpread = 0.0;
    for (const Stone& s : b.stones)
    {
        if (s.course == 0)
            bottomSpread = std::max(bottomSpread, std::abs(s.center.z));
        if (s.course == b.courses - 1)
            topSpread = std::max(topSpread, std::abs(s.center.z));
    }
    CHECK(topSpread < bottomSpread - 5.0);
    CHECK(countOverlaps(b.stones, 0.01) == 0);
}

TEST_CASE("masonry bands and cornice stand out of the outer face")
{
    WallParams p;
    p.height = 300.0;
    p.thickness = 40.0;
    p.bands = {150.0};
    p.cornice = 2;
    const Layout l = layoutWall(line({{0, 0, 0}, {400, 0, 0}}), p);
    const int bands = countRole(l.stones, Role::Band);
    CHECK(bands > 20);
    double outermost = 0.0, plain = 0.0;
    for (const Stone& s : l.stones)
    {
        const double reach = std::abs(s.center.z) + s.size.z * 0.5;
        (s.role == Role::Band ? outermost : plain) = std::max(s.role == Role::Band ? outermost : plain, reach);
    }
    CHECK(outermost > plain + 2.0);
    CHECK(countOverlaps(l.stones, 0.01) == 0);
}

TEST_CASE("masonry elements: columns, stairs, vaults and domes")
{
    ColumnSpec c;
    c.height = 800.0;
    c.diameter = 80.0;
    const std::vector<Stone> col = columnStones(c, 1.0, 3);
    REQUIRE(col.size() >= 6);
    CHECK(countOverlaps(col, 0.01) == 0);
    double top = 0.0;
    for (const Stone& s : col)
        top = std::max(top, s.center.y + s.size.y * 0.5);
    CHECK(top == doctest::Approx(800.0).epsilon(0.01));
    int drums = 0;
    for (const Stone& s : col)
        drums += s.shape == Shape::Drum ? 1 : 0;
    CHECK(drums >= 5);

    StairSpec st;
    st.steps = 10;
    st.width = 300.0;
    std::vector<geo::Mesh> fill;
    const std::vector<Stone> steps = stairStones(st, 40.0, 1.0, &fill);
    CHECK(countRole(steps, Role::Step) >= 10);
    CHECK(fill.size() == 9);
    CHECK(countOverlaps(steps, 0.01) == 0);

    Units u{60.0, 30.0, 40.0, 1.5};
    VaultSpec barrel;
    barrel.from = {0, 300, 0};
    barrel.to = {600, 300, 0};
    barrel.profile.span = 400.0;
    barrel.profile.ring = 40.0;
    const std::vector<Stone> vault = vaultStones(barrel, u, 1);
    CHECK(vault.size() > 100);
    CHECK(countOverlaps(vault, 0.01) == 0);

    VaultSpec dome;
    dome.dome = true;
    dome.centre = {0, 500, 0};
    dome.radius = 400.0;
    dome.ring = 50.0;
    const std::vector<Stone> d = vaultStones(dome, u, 1);
    CHECK(d.size() > 100);
    CHECK(countOverlaps(d, 0.5) == 0);
}

namespace
{
json chapelSpec(double scale)
{
    return {{"scale", scale},
            {"stone", {{"length", 60}, {"height", 30}, {"depth", 35}, {"joint", 1.5}}},
            {"walls",
             json::array({{{"name", "nave"},
                           {"points", pts({{0, 0, 0}, {1600, 0, 0}, {1600, 0, 800}, {0, 0, 800}})},
                           {"closed", true},
                           {"height", 900},
                           {"thickness", 90},
                           {"cornice", 1},
                           {"bands", json::array({450})},
                           {"openings", json::array({{{"name", "west_door"}, {"at", 2000}, {"width", 200}, {"height", 450}, {"arch", "round"}},
                                                     {{"name", "window_1"}, {"at", 500}, {"width", 120}, {"height", 300}, {"sill", 300}, {"arch", "pointed"}},
                                                     {{"name", "window_2"}, {"at", 1100}, {"width", 120}, {"height", 300}, {"sill", 300}, {"arch", "pointed"}}})},
                           {"buttresses", json::array({{{"at", 800}, {"length", 150}, {"width", 120}, {"height", 700}, {"top", 300}}})}}})},
            {"colonnades", json::array({{{"name", "portico"}, {"from", {-300, 0, 100}}, {"to", {-300, 0, 700}}, {"count", 4}, {"height", 600}, {"diameter", 70}}})},
            {"stairs", json::array({{{"name", "steps"}, {"at", {1250, 0, -400}}, {"direction", {0, 0, 1}}, {"width", 300}, {"steps", 6}, {"rise", 20}, {"going", 40}}})},
            {"arches", json::array({{{"name", "gate"}, {"at", {800, 0, -800}}, {"span", 300}, {"kind", "pointed"}}})},
            {"vaults", json::array({{{"name", "apse"}, {"kind", "dome"}, {"at", {2000, 0, 400}}, {"radius", 300}, {"ring", 50}}})},
            {"floors", json::array({{{"name", "roof"}, {"points", pts({{0, 0, 0}, {1600, 0, 0}, {1600, 0, 800}, {0, 0, 800}})}, {"y", 940}, {"thickness", 40}}})}};
}
} // namespace

TEST_CASE("masonry building spec: every element is a named region")
{
    const Plan plan = planBuilding(chapelSpec(1.0), BuildOptions{});
    const Built b = realizePlan(plan, RuinParams{}, AgeParams{}, false);
    std::set<std::string> regions(b.regions.begin(), b.regions.end());
    for (const char* r : {"nave", "nave/west_door", "nave/window_1", "nave/buttress_1", "portico/column_1", "portico/column_4", "steps", "gate", "apse", "roof"})
    {
        CAPTURE(r);
        CHECK(regions.count(r) == 1);
    }
    std::vector<int> perRegion(b.regions.size(), 0);
    for (const Stone& s : b.stones)
        ++perRegion[size_t(s.region)];
    for (size_t k = 0; k < b.regions.size(); ++k)
    {
        CAPTURE(b.regions[k]);
        CHECK(perRegion[k] > 0);
    }
    CHECK(countRole(b.stones, Role::Slab) == 1);
    CHECK(!b.coreHulls.empty());
    CHECK(b.meshes.size() < b.stones.size() / 4);

    CHECK_THROWS_WITH_AS(planBuilding(json{{"walls", json::array({{{"points", pts({{0, 0, 0}, {100, 0, 0}})}, {"hieght", 3}}})}}, BuildOptions{}),
                         doctest::Contains("hieght"), std::runtime_error);
    CHECK_THROWS_AS(planBuilding(json{{"colour", 1}}, BuildOptions{}), std::runtime_error);
    CHECK(specSchema()["properties"].contains("walls"));
}

TEST_CASE("masonry ruin: how much is left, deterministic, nothing floats, rubble at the foot")
{
    const Plan plan = planBuilding(chapelSpec(1.0), BuildOptions{});
    for (double left : {0.75, 0.5, 0.25})
    {
        CAPTURE(left);
        RuinParams r;
        r.left = left;
        r.gaps = 1;
        RuinStats st;
        const Built b = realizePlan(plan, r, AgeParams{}, false, &st);
        CHECK(st.left == doctest::Approx(left).epsilon(0.25));
        CHECK(st.removed > 0);
        CHECK(st.rubble > 0);
        CHECK(countFloating(b.stones, b.pitch * 0.15) == 0);
        // Rubble lies on the ground or on other stones, never above the standing ruin.
        double groundTop = 0.0;
        for (const Stone& s : b.stones)
            if (s.role == Role::Rubble)
                groundTop = std::max(groundTop, s.center.y);
        CHECK(groundTop < 900.0);
        if (left == 0.5)
        {
            CHECK(st.slabPieces > 4);
            CHECK(st.fallenPieces > 0);
            const Built again = realizePlan(plan, r, AgeParams{}, false);
            CHECK(hashOf(again.stones) == hashOf(b.stones));
            r.seed = 7;
            CHECK(hashOf(realizePlan(plan, r, AgeParams{}, false).stones) != hashOf(b.stones));
        }
    }

    // Tipped stones lean by no more than tip.
    RuinParams r;
    r.left = 0.5;
    r.loose = 1.0;
    r.tip = 30.0;
    RuinStats st;
    const Built b = realizePlan(plan, r, AgeParams{}, false, &st);
    CHECK(st.loose > 0);
    for (const Stone& s : b.stones)
        if (s.loose && s.role != Role::Rubble && s.role != Role::Slab)
            CHECK(glm::degrees(std::acos(std::clamp(s.frame[1].y, -1.0, 1.0))) < 31.0);

    // Only the named region ruins; the portico stands whole.
    RuinParams only;
    only.left = 0.3;
    only.regions = {"nave"};
    const Built part = realizePlan(plan, only, AgeParams{}, false);
    const Built whole = realizePlan(plan, RuinParams{}, AgeParams{}, false);
    auto count = [](const Built& x, const std::string& prefix) {
        int n = 0;
        for (const Stone& s : x.stones)
            n += s.role != Role::Rubble && x.regions[size_t(s.region)].starts_with(prefix) ? 1 : 0;
        return n;
    };
    CHECK(count(part, "portico") == count(whole, "portico"));
    CHECK(count(part, "nave") < count(whole, "nave"));
}

TEST_CASE("masonry ruin: any instance set, settled with the rigid-body solver")
{
    // A dry stone stack placed by hand: 6 x 10 boxes, no grounded flags.
    std::vector<Stone> stones;
    for (int row = 0; row < 10; ++row)
        for (int k = 0; k < 6; ++k)
        {
            Stone s;
            s.size = {50.0, 25.0, 40.0};
            s.center = {k * 52.0 + (row % 2) * 26.0, row * 26.0 + 12.5, 0.0};
            s.face = Face::Through;
            stones.push_back(s);
        }
    RuinParams r;
    r.left = 0.5;
    r.rubble = 1.0;
    r.settle = true;
    std::vector<Stone> ruined = stones;
    std::vector<int> kept;
    const RuinStats st = ruinStones(ruined, r, 0.0, {}, &kept);
    CHECK(st.removed > 0);
    CHECK(st.settled);
    CHECK(kept.size() == ruined.size());
    CHECK(ruined.size() == stones.size()); // all fallen stones kept as rubble
    for (const Stone& s : ruined)
        CHECK(s.center.y > -1.0);
    CHECK(countFloating(ruined, 2.0) <= st.rubble);
}

TEST_CASE("masonry age: wet near the ground, moss only with age")
{
    const Plan plan = planWalls(std::vector<Footprint>{line({{0, 0, 0}, {600, 0, 0}})}, [] {
        BuildOptions o;
        o.wall.height = 400.0;
        o.wall.stone = {40.0, 20.0, 25.0, 1.0};
        o.wall.thickness = 60.0;
        return o;
    }());
    AgeParams young;
    const Built fresh = realizePlan(plan, RuinParams{}, young, false);
    for (const Stone& s : fresh.stones)
        CHECK(s.moss == 0.0f);
    AgeParams old;
    old.age = 1.0;
    old.moss = 1.0;
    const Built aged = realizePlan(plan, RuinParams{}, old, false);
    double lowWet = 0.0, highWet = 0.0, moss = 0.0;
    int low = 0, high = 0;
    for (const Stone& s : aged.stones)
    {
        if (s.course == 0)
        {
            lowWet += s.wet;
            ++low;
        }
        if (s.course == aged.courses / 2)
        {
            highWet += s.wet;
            ++high;
        }
        moss = std::max(moss, double(s.moss));
        CHECK(s.age > 0.5f);
    }
    CHECK(lowWet / low > highWet / high + 0.3);
    CHECK(moss > 0.3);
}

TEST_CASE("masonry tile: the UV scale follows the tile setting")
{
    StoneShape s;
    s.size = {100.0, 50.0, 50.0};
    s.detail = Detail::Box;
    auto span = [](const geo::Mesh& m) {
        double lo = 1e300, hi = -1e300;
        for (const glm::vec2& v : m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)))
        {
            lo = std::min(lo, double(v.x));
            hi = std::max(hi, double(v.x));
        }
        return hi - lo;
    };
    s.tile = 100.0;
    const double a = span(stoneMesh(s));
    s.tile = 50.0;
    CHECK(span(stoneMesh(s)) == doctest::Approx(a * 2.0).epsilon(0.05));
    s.shape = Shape::Drum;
    CHECK(stoneMesh(s).numFaces() == 8 + 2);
    s.shape = Shape::Wedge;
    s.taper = 0.5;
    s.detail = Detail::Worn;
    const geo::Mesh w = stoneMesh(s);
    CHECK(w.validate().empty());
}

TEST_CASE("masonry whale scale: stones metres across, a wall 50 m tall and 500 m long")
{
    BuildOptions o;
    o.wall.stone = {300.0, 150.0, 200.0, 4.0};
    o.wall.height = 5000.0;
    o.wall.thickness = 600.0;
    o.wall.protrude = 3.0;
    o.bevel = 6.0;
    Opening gate;
    gate.at = 25000.0;
    gate.width = 2000.0;
    gate.height = 3500.0;
    gate.arch = ArchKind::Round;
    o.wall.openings = {gate};
    const std::vector<Footprint> fps = {line({{0, 0, 0}, {50000, 400, 0}})};
    auto t0 = std::chrono::steady_clock::now();
    const Plan plan = planWalls(fps, o);
    const Built intact = realizePlan(plan, RuinParams{}, AgeParams{}, false);
    const double buildMs = ms(t0);
    t0 = std::chrono::steady_clock::now();
    RuinParams r;
    r.left = 0.5;
    r.gaps = 2;
    RuinStats st;
    const Built ruined = realizePlan(plan, r, AgeParams{0.8, 0.6, 0.0, 1}, false, &st);
    const double ruinMs = ms(t0);
    t0 = std::chrono::steady_clock::now();
    const Built lods = realizePlan(plan, RuinParams{}, AgeParams{}, true);
    const double lodMs = ms(t0);
    MESSAGE("whale wall 500 m x 50 m, 3 m stones: " << intact.stones.size() << " stones, " << intact.meshes.size() << " meshes, " << intact.courses
                                                    << " courses; build " << buildMs << " ms, ruin+age " << ruinMs << " ms (" << ruined.stones.size()
                                                    << " stones, " << st.rubble << " rubble), with LODs " << lodMs << " ms");
    CHECK(intact.stones.size() > 5000);
    CHECK(intact.courses == doctest::Approx(5000.0 / 154.0).epsilon(0.1));
    CHECK(countOverlaps(intact.stones, 0.01) == 0);
    CHECK(countRole(intact.stones, Role::Keystone) == 1);
    CHECK(st.left == doctest::Approx(0.5).epsilon(0.25));
    CHECK(countFloating(ruined.stones, ruined.pitch * 0.15) == 0);
    CHECK(buildMs < 5000.0);
    CHECK(ruinMs < 10000.0);
    CHECK(lods.lods.size() == lods.meshes.size());
}

TEST_CASE("masonry commands: building, spec round trip, ruin, age, check, undo and save/reopen")
{
    loadMasonry();
    run("newScene");
    const json b = run("masonryBuilding", {{"spec", chapelSpec(1.0)}, {"name", "Chapel"}});
    CHECK(b["stones"].get<int64_t>() > 1000);
    CHECK(b["regions"].contains("nave/west_door"));
    const std::string mesh = b["mesh"].get<std::string>();

    json spec = run("masonrySpec", {{"args", {mesh}}});
    CHECK(spec["spec"] == chapelSpec(1.0));
    CHECK(spec["regions"].size() > 8);
    spec["spec"]["walls"][0]["height"] = 1000;
    spec["spec"]["floors"][0]["y"] = 1040;
    const json rebuilt = run("masonryBuilding", {{"node", mesh}, {"spec", spec["spec"]}});
    CHECK(rebuilt["stones"].get<int64_t>() > b["stones"].get<int64_t>());

    const json checked = run("masonryCheck", {{"args", {mesh}}});
    CAPTURE(checked.dump());
    CHECK(checked["ok"].get<bool>());
    CHECK(checked["floating"].get<int64_t>() == 0);

    const json ruined = run("ruin", {{"args", {mesh}}, {"left", 0.5}, {"collapse", 90.0}, {"gaps", 1}});
    CHECK(ruined["removed"].get<int64_t>() > 0);
    CHECK(ruined["left"].get<double>() == doctest::Approx(0.5).epsilon(0.25));
    const json aged = run("age", {{"args", {mesh}}, {"age", 0.9}});
    CHECK(aged["meanAge"].get<double>() > 0.5);
    const json after = run("masonryCheck", {{"args", {mesh}}});
    CHECK(after["floating"].get<int64_t>() == 0);
    const json info = run("masonryInfo", {{"args", {mesh}}});
    CHECK(info["ruin"]["removed"] == ruined["removed"]);

    // Undo the age, then the ruin: the building stands whole again.
    run("undo");
    run("undo");
    const json whole = run("masonryInfo", {{"args", {mesh}}});
    CHECK(whole["ruin"]["removed"].get<int64_t>() == 0);
    CHECK(whole["stones"] == rebuilt["stones"]);
    run("redo");
    run("redo");

    // Save and reopen: the same building, ruined and aged the same way.
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "lila_masonry_test" / "chapel.lila";
    std::filesystem::create_directories(file.parent_path());
    run("saveScene", {{"args", {file.string()}}});
    run("newScene");
    run("openScene", {{"args", {file.string()}}});
    const json reopened = run("masonryInfo", {{"args", {mesh}}});
    CHECK(reopened["stones"] == info["stones"]);
    CHECK(reopened["ruin"] == info["ruin"]);
    CHECK(reopened["settings"]["age"].get<double>() == doctest::Approx(0.9));

    // The node evaluates to the same stones the command reported.
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(mesh);
    REQUIRE(n);
    const scene::MeshPtr out = n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"));
    REQUIRE(out);
    CHECK(out->numFaces() > 1000);
}

TEST_CASE("masonry commands: walls on terrain with openings, ruined in a region, and hand-placed stones ruined in place")
{
    loadMasonry();
    run("newScene");
    const json w = run("masonryWall", {{"points", pts({{0, 0, 0}, {800, 150, 0}})},
                                       {"heights", json::array({300, 220})},
                                       {"stoneLength", 40.0},
                                       {"stoneHeight", 20.0},
                                       {"stoneDepth", 25.0},
                                       {"thickness", 60.0},
                                       {"batter", 2.0},
                                       {"openings", json::array({{{"at", 400}, {"width", 100}, {"height", 180}, {"arch", "segmental"}, {"name", "gate"}}})}});
    CHECK(w["regions"].contains("wall_0/gate"));
    CHECK(w["roles"]["voussoir"].get<int64_t>() > 5);
    const std::string mesh = w["mesh"].get<std::string>();
    const json wallCheck = run("masonryCheck", {{"args", {mesh}}});
    CAPTURE(wallCheck.dump());
    CHECK(wallCheck["ok"].get<bool>());
    const json r = run("ruin", {{"args", {mesh}}, {"left", 0.6}, {"regions", json::array({"wall_0"})}});
    CHECK(r["removed"].get<int64_t>() > 0);
    CHECK(run("masonryInfo", {{"args", {mesh}}})["settings"]["ruinRegions"] == "wall_0");

    // Ten cubes stacked by hand.
    std::vector<std::string> cubes;
    for (int k = 0; k < 10; ++k)
    {
        const json c = run("polyCube", {{"width", 40}, {"height", 20}, {"depth", 30}});
        const std::string name = c.is_array() ? c[0].get<std::string>() : c.get<std::string>();
        run("xform", {{"args", {name}}, {"translation", {0.0, 10.0 + k * 20.5, 0.0}}});
        cubes.push_back(name);
    }
    const json placed = run("ruin", {{"args", cubes}, {"left", 0.4}, {"rubble", 0.5}});
    CHECK(placed["removed"].get<int64_t>() > 0);
    CHECK(placed["deleted"].size() + placed["moved"].size() > 0);
    run("undo");
    for (const std::string& c : cubes)
        CHECK(scene::Graph::current().find(c) != nullptr);
}

TEST_CASE("masonry ship: assembly and Nanite are options, custom data carries age, damage, wet and moss")
{
    const Plan plan = planBuilding(chapelSpec(1.0), BuildOptions{});
    RuinParams r;
    r.left = 0.6;
    const Built b = realizePlan(plan, r, AgeParams{0.8, 0.5, 0.0, 1}, true);
    ShipOptions so;
    const json plain = shipPlan(b, glm::dmat4(1.0), "k", "Chapel", so);
    bool sawDamage = false;
    for (const json& g : plain["groups"])
        for (const json& p : g["placements"])
            if (p["customData"].size() == 6)
                sawDamage = sawDamage || p["customData"][3].get<double>() > 0.5;
    CHECK(sawDamage);
    so.mode = "assembly";
    const json asm_ = shipPlan(b, glm::dmat4(1.0), "k", "Chapel", so);
    int assemblies = 0, stones = 0;
    for (const json& as : asm_["assets"])
    {
        assemblies += as["kind"] == "assembly" ? 1 : 0;
        stones += as["kind"] == "stone" ? 1 : 0;
    }
    CHECK(assemblies == 1);
    CHECK(stones == int(b.meshes.size()));
    CHECK(asm_["groups"].size() == 2);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "lila_masonry_test" / "SM_Chapel_Assembly.usda";
    std::string err;
    REQUIRE(writeAssemblyUsda(b, asm_, file, &err));
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("NaniteAssemblyRootAPI") != std::string::npos);
    CHECK(text.find("NaniteAssemblyExternalRefAPI") != std::string::npos);
    CHECK(text.find("SM_Chapel_Stone_0.SM_Chapel_Stone_0") != std::string::npos);
    // Unreal's importer reads the part path only as a token or string; an asset-typed value leaves the part empty.
    CHECK(text.find("uniform token unreal:naniteAssembly:meshAssetPath = \"/Game/") != std::string::npos);

    std::vector<int> all(b.stones.size());
    for (size_t k = 0; k < all.size(); ++k)
        all[k] = int(k);
    const geo::Mesh merged = mergeStones(b, all, 2, glm::dvec3(0.0));
    CHECK(merged.cornerAttrs().has(geo::attr::uvName(2)));
}


TEST_CASE("masonry commands: a new building can be aged with a weathering preset")
{
    loadMasonry();
    run("newScene");
    const json b = run("masonryBuilding", {{"spec", chapelSpec(1.0)}, {"name", "AgedChapel"}, {"weather", "Concrete Debris"}});
    REQUIRE(b.contains("weather"));
    CHECK(b["weather"]["preset"] == "Concrete Debris");
    CHECK(!b["weather"]["group"].get<std::string>().empty());
}
