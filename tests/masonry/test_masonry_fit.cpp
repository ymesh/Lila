// Masonry fit: intact walls stand on their beds, floors fit inside their walls (or their own outline) and carry
// collision, rubble spreads instead of stacking, loose stones keep clear of their neighbours, and masonryCheck says
// where problems are.

#include "TestUtil.h"
#include "Masonry.h"
#include "MasonryBuilding.h"
#include "MasonryRuin.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <tuple>

using namespace lila;
using namespace masonry;
using namespace masonrytest;
using json = nlohmann::json;

namespace
{
const json kStone = {{"length", 60}, {"height", 30}, {"depth", 35}, {"joint", 1.5}};

json naveWall(bool buttresses)
{
    json w = {{"name", "nave"},
              {"points", pts({{0, 0, 0}, {1600, 0, 0}, {1600, 0, 800}, {0, 0, 800}})},
              {"closed", true},
              {"height", 900},
              {"thickness", 90},
              {"cornice", 1},
              {"openings",
               {{{"name", "east_window"}, {"at", 2000}, {"width", 120}, {"height", 300}, {"sill", 350}, {"arch", "pointed"}},
                {{"name", "south_window"}, {"at", 1000}, {"width", 100}, {"height", 280}, {"sill", 350}, {"arch", "pointed"}}}}};
    if (buttresses)
        w["buttresses"] = {{{"at", 400}}, {{"at", 800}}, {{"at", 1200}}};
    return w;
}

json towerWall()
{
    return {{"name", "tower"},
            {"points", pts({{-640, 0, 150}, {-140, 0, 150}, {-140, 0, 650}, {-640, 0, 650}})},
            {"closed", true},
            {"height", 1800},
            {"thickness", 100},
            {"bands", {900}},
            {"cornice", 2},
            {"openings",
             {{{"name", "west_door"}, {"at", 1750}, {"width", 160}, {"height", 320}, {"arch", "round"}},
              {{"name", "belfry"}, {"at", 250}, {"width", 90}, {"height", 200}, {"sill", 1450}, {"arch", "round"}}}}};
}

Built building(const json& spec, const RuinParams& r = {}, RuinStats* st = nullptr)
{
    json s = spec;
    s["stone"] = kStone;
    s["bond"] = "ashlar";
    return realizePlan(planBuilding(s, BuildOptions{}), r, AgeParams{}, false, st);
}

int floating(const Built& b) { return countFloating(b.stones, std::max(0.1, b.pitch * 0.15)); }

std::vector<int> standingOf(const Built& b)
{
    std::vector<int> out;
    for (size_t k = 0; k < b.stones.size(); ++k)
        if (!isRubble(b.stones[k]))
            out.push_back(int(k));
    return out;
}

size_t standingOverlaps(const Built& b) { return overlappingPairs(b, standingOf(b), 0.1).size(); }
} // namespace

TEST_CASE("masonry fit: intact walls stand on their beds, over arch crowns and short rough stones too")
{
    SUBCASE("dry stone, straight on flat ground")
    {
        BuildOptions o;
        o.wall.bond = Bond::DryStone;
        o.wall.height = 120;
        o.wall.thickness = 60;
        o.wall.stone = {40, 20, 30, 1.0};
        const Footprint f = line({{0, 0, 0}, {800, 0, 0}});
        const Built b = realizePlan(planWalls(std::span<const Footprint>(&f, 1), o), RuinParams{}, AgeParams{}, false);
        CHECK(floating(b) == 0);
        CHECK(countOverlaps(b.stones, 0.1) == 0);
    }
    SUBCASE("a tower with a band, a cornice and a belfry")
    {
        const Built b = building({{"walls", {towerWall()}}});
        CHECK(floating(b) == 0);
        CHECK(standingOverlaps(b) == 0);
    }
    SUBCASE("a nave without buttresses, windows under pointed arches")
    {
        const Built b = building({{"walls", {naveWall(false)}}});
        CHECK(floating(b) == 0);
        CHECK(standingOverlaps(b) == 0);
    }
    SUBCASE("a floor alone on the ground")
    {
        const json floor = {{"name", "paving"}, {"points", pts({{0, 0, 0}, {1600, 0, 0}, {1600, 0, 800}, {0, 0, 800}})}, {"y", 0}, {"thickness", 10}, {"surface", "flags"}};
        const Built b = building({{"floors", {floor}}});
        CHECK(floating(b) == 0);
    }
}

TEST_CASE("masonry fit: floors fit inside their walls, whole and broken")
{
    const json nave = naveWall(true);
    const json follow = pts({{0, 0, 0}, {1600, 0, 0}, {1600, 0, 800}, {0, 0, 800}});
    const json inside = pts({{45, 0, 45}, {1555, 0, 45}, {1555, 0, 755}, {45, 0, 755}});
    for (const auto& [tag, outline, y] : {std::tuple{"outline on the walls", follow, 0.0}, std::tuple{"outline on the inside face", inside, 0.0},
                                          std::tuple{"raised floor", follow, 300.0}})
    {
        CAPTURE(tag);
        const json spec = {{"walls", {nave}}, {"floors", {{{"name", "paving"}, {"points", outline}, {"y", y}, {"thickness", y > 0.0 ? 30 : 10}, {"surface", "flags"}}}}};
        const Built whole = building(spec);
        CHECK(standingOverlaps(whole) == 0);
        CHECK(floating(whole) == 0);
        int slabs = 0, paving = 0;
        for (const Stone& s : whole.stones)
        {
            slabs += s.role == Role::Slab ? 1 : 0;
            paving += s.role == Role::Paving ? 1 : 0;
            // Cut back to the walls' inside faces, 45 cm in from the walls' centre line.
            if (s.role == Role::Slab)
                CHECK(s.size.x < 1600.0 - 89.0);
        }
        CHECK(slabs == 1);
        CHECK(paving > 100);

        RuinParams r;
        r.left = 0.5;
        RuinStats st;
        const Built ruined = building(spec, r, &st);
        CHECK(st.slabPieces > 4);
        CHECK(standingOverlaps(ruined) == 0);
    }
}

TEST_CASE("masonry fit: rubble inside a closed tower spreads over its floor instead of stacking")
{
    BuildOptions o;
    o.wall.bond = Bond::Ashlar;
    o.wall.height = 1200;
    o.wall.thickness = 90;
    o.wall.stone = {50, 28, 35, 1.5};
    o.wall.batter = 1;
    o.wall.cornice = 2;
    Footprint f;
    f.closed = true;
    for (int i = 0; i < 16; ++i)
        f.points.push_back({300 * std::cos(i / 16.0 * 2.0 * kPi), 0, 300 * std::sin(i / 16.0 * 2.0 * kPi)});
    Opening door;
    door.at = 150;
    door.width = 120;
    door.height = 240;
    o.wall.openings = {door};
    RuinParams r;
    r.left = 0.5;
    r.collapse = 90;
    r.gaps = 1;
    RuinStats st;
    const Built b = realizePlan(planWalls(std::span<const Footprint>(&f, 1), o), r, AgeParams{}, false, &st);
    CHECK(st.rubble > 500);
    double highest = 0.0;
    CHECK(countStacked(b.stones, 0.5 * 1200.0, nullptr, &highest) == 0);
    CHECK(highest < 0.4 * 1200.0);
    int inside = 0;
    for (const Stone& s : b.stones)
        inside += s.role == Role::Rubble && std::hypot(s.center.x, s.center.z) < 240.0 ? 1 : 0;
    CHECK(inside > 100);
    std::vector<Stone> rubble;
    for (const Stone& s : b.stones)
        if (isRubble(s))
            rubble.push_back(s);
    CHECK(countOverlaps(rubble, 1.0) < rubble.size() / 50);
}

TEST_CASE("masonry fit: loose stones of a dry stone wall over a hill keep clear of their neighbours")
{
    BuildOptions o;
    o.wall.bond = Bond::DryStone;
    o.wall.height = 120;
    o.wall.thickness = 60;
    o.wall.stone = {40, 20, 30, 1.0};
    Footprint f;
    const glm::dvec3 corners[] = {{-2500, 0, 4000}, {0, 0, 3500}, {2500, 0, 4200}};
    for (int seg = 0; seg < 2; ++seg)
        for (int i = 0; i < 64; ++i)
        {
            glm::dvec3 p = glm::mix(corners[seg], corners[seg + 1], i / 64.0);
            p.y = 200.0 * std::sin(p.x / 900.0) + 120.0 * std::cos(p.z / 400.0);
            f.points.push_back(p);
        }
    const Built whole = realizePlan(planWalls(std::span<const Footprint>(&f, 1), o), RuinParams{}, AgeParams{}, false);
    REQUIRE(standingOverlaps(whole) == 0);
    for (double loose : {0.4, 1.0})
    {
        CAPTURE(loose);
        RuinParams r;
        r.left = 0.75;
        r.loose = loose;
        RuinStats st;
        const Built b = realizePlan(planWalls(std::span<const Footprint>(&f, 1), o), r, AgeParams{}, false, &st);
        CHECK(st.loose > 20);
        CHECK(standingOverlaps(b) == 0);
    }
}

TEST_CASE("masonry fit commands: a wall's region name, bounds, and where masonryCheck finds interpenetration")
{
    loadMasonry();
    run("newScene");
    const json w = run("masonryWall", {{"points", pts({{0, 0, 0}, {400, 0, 0}})}, {"region", "garden_wall"}, {"height", 100}, {"name", "Garden"}});
    CHECK(w["regions"].contains("garden_wall"));
    CHECK(!w["regions"].contains("wall_0"));
    REQUIRE(w["bounds"].is_object());
    CHECK(w["bounds"]["min"][0].get<double>() < 1.0);
    CHECK(w["bounds"]["max"][0].get<double>() > 399.0);
    CHECK(w["bounds"]["max"][1].get<double>() > 90.0);

    // Two walls crossing: their stones interpenetrate where they meet, and the check names both.
    const json spec = {{"walls",
                        {{{"name", "east"}, {"points", pts({{0, 0, 0}, {400, 0, 0}})}, {"height", 100}, {"thickness", 40}},
                         {{"name", "north"}, {"points", pts({{200, 0, -200}, {200, 0, 200}})}, {"height", 100}, {"thickness", 40}}}}};
    const json b = run("masonryBuilding", {{"spec", spec}, {"name", "Crossing"}});
    CHECK(b["bounds"].is_object());
    const json c = run("masonryCheck", {{"args", {b["mesh"]}}});
    CHECK(!c["ok"].get<bool>());
    REQUIRE(c["overlaps"].get<int64_t>() > 0);
    CHECK(c["overlapRegions"].contains("east + north"));
    CHECK(c["overlapPairs"].size() > 0);
    CHECK(c["issues"].dump().find("east + north") != std::string::npos);
    CHECK(c["stacked"].get<int64_t>() == 0);
}

namespace
{
double meshVolume(const geo::Mesh& m)
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

const json kRect = pts({{0, 0, 0}, {1600, 0, 0}, {1600, 0, 800}, {0, 0, 800}});
const json kEll = pts({{0, 0, 0}, {1200, 0, 0}, {1200, 0, 400}, {500, 0, 400}, {500, 0, 900}, {0, 0, 900}});
} // namespace

TEST_CASE("masonry fit: a floor on its own carries collision, an L-shaped one in convex pieces")
{
    for (const auto& [outline, area] : {std::pair{kRect, 1600.0 * 800.0}, std::pair{kEll, 1200.0 * 400.0 + 500.0 * 500.0}})
    {
        CAPTURE(outline.dump());
        const Built b = building({{"floors", {{{"name", "paving"}, {"points", outline}, {"y", 0}, {"thickness", 10}}}}});
        REQUIRE(!b.coreHulls.empty());
        double volume = 0.0;
        for (const geo::Mesh& h : b.coreHulls)
        {
            CHECK(meshVolume(h) > 0.0);
            volume += meshVolume(h);
        }
        CHECK(volume == doctest::Approx(area * 10.0).epsilon(0.001));

        RuinParams r;
        r.left = 0.5;
        const Built ruined = building({{"floors", {{{"name", "paving"}, {"points", outline}, {"y", 300}, {"thickness", 30}}}}}, r);
        int inPlace = 0;
        for (const Stone& s : ruined.stones)
            inPlace += s.role == Role::Slab && !s.loose ? 1 : 0;
        CHECK(int(ruined.coreHulls.size()) >= inPlace);
    }
    loadMasonry();
    run("newScene");
    const json f = run("masonryBuilding", {{"spec", {{"floors", {{{"name", "paving"}, {"points", kRect}, {"y", 0}, {"thickness", 10}, {"surface", "flags"}}}}}}, {"name", "Floor"}});
    const json c = run("masonryCheck", {{"args", {f["mesh"]}}});
    CHECK(c["ok"].get<bool>());
    CHECK(c["collisionHulls"].get<int64_t>() > 0);
    run("ruin", {{"args", {f["mesh"]}}, {"left", 0.5}});
    CHECK(run("masonryCheck", {{"args", {f["mesh"]}}})["ok"].get<bool>());
}

TEST_CASE("masonry fit: paving on a floor without walls stays inside its outline")
{
    for (const char* surface : {"flags", "cobbles"})
        for (const json& outline : {kRect, kEll})
        {
            CAPTURE(surface);
            CAPTURE(outline.dump());
            const Built b = building({{"floors", {{{"name", "paving"}, {"points", outline}, {"y", 0}, {"thickness", 10}, {"surface", surface}}}}});
            std::vector<glm::dvec3> poly;
            for (const json& q : outline)
                poly.push_back({q[0].get<double>(), 0.0, q[2].get<double>()});
            auto inside = [&](double x, double z) {
                bool in = false;
                for (size_t i = 0, k = poly.size() - 1; i < poly.size(); k = i++)
                    if ((poly[i].z > z) != (poly[k].z > z) && x < (poly[k].x - poly[i].x) * (z - poly[i].z) / (poly[k].z - poly[i].z) + poly[i].x)
                        in = !in;
                return in;
            };
            int paving = 0, outside = 0;
            for (const Stone& s : b.stones)
            {
                if (s.role != Role::Paving)
                    continue;
                ++paving;
                for (const glm::dvec3& c : stoneCorners(s))
                {
                    const glm::dvec3 in = c + (s.center - c) * 0.001;
                    outside += inside(in.x, in.z) ? 0 : 1;
                }
            }
            CHECK(paving > 50);
            CHECK(outside == 0);
        }
}

TEST_CASE("masonry fit: a buttress's short top courses stay within its own footprint")
{
    json nave = naveWall(true);
    nave.erase("cornice");
    nave.erase("openings");
    const Built b = building({{"walls", {nave}}});
    CHECK(standingOverlaps(b) == 0);
    for (const Stone& s : b.stones)
        if (s.t1 >= s.t0 && b.regions[size_t(s.region)].find("buttress") != std::string::npos)
            CHECK(s.t0 >= -0.5);
}
