#include "TestUtil.h"
#include "Generators.h"
#include "Parcels.h"
#include "Streetscape.h"

#include "curve/CurveNodes.h"
#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "gn/NodeLib.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
Layout mexicanCity(double size, uint32_t seed)
{
    Options o;
    REQUIRE(builtinProfile("mexicanCity", &o.profile));
    o.seed = seed;
    Site s;
    s.size = size;
    return build(s, Ground::plane(), o);
}

/** Faces per material slot name. */
std::map<std::string, int> facesByMaterial(const geo::Mesh& m)
{
    std::map<std::string, int> out;
    const auto mat = m.faceAttrs().get<int32_t>("material");
    for (int32_t id : mat)
        ++out[size_t(id) < m.materialNames().size() ? m.materialNames()[size_t(id)] : "?"];
    return out;
}

/** Faces of the mesh whose Newell normal points down (walls such as curbs stand upright and count as neither). */
int facesFacingDown(const geo::Mesh& m)
{
    int down = 0;
    for (geo::Index f = 0; f < geo::Index(m.numFaces()); ++f)
    {
        glm::dvec3 n(0.0);
        std::vector<glm::dvec3> pts;
        for (geo::Index c : m.faceCorners(f))
            pts.push_back(m.position(m.cornerPoint(c)));
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const glm::dvec3 &a = pts[k], &b = pts[(k + 1) % pts.size()];
            n += glm::dvec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
        }
        down += n.y < -0.01 * glm::length(n) ? 1 : 0;
    }
    return down;
}

void loadCityPackages()
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
} // namespace

TEST_CASE("settlement mexicanCity: perimeter blocks built wall to wall, low and flat, narrow streets")
{
    const Layout l = mexicanCity(100000.0, 2);
    CHECK(streetWidth(l.profile, StreetClass::Local) == doctest::Approx(900.0));
    int perimeter = 0, ordinary = 0;
    double blockArea = 0.0;
    for (const Block& b : l.blocks)
        if (b.type != BlockType::Park && b.type != BlockType::Plaza && b.type != BlockType::Market)
        {
            ++ordinary;
            perimeter += b.type == BlockType::Perimeter ? 1 : 0;
            blockArea += b.area;
        }
    REQUIRE(ordinary > 20);
    CHECK(perimeter == ordinary);
    REQUIRE(l.envelopes.size() > 100);
    int party = 0, flat = 0, low = 0;
    double built = 0.0;
    for (const Envelope& env : l.envelopes)
    {
        built += area(env.footprint) / kM2;
        party += std::ranges::count(env.footprint.tag, 1) > 0 ? 1 : 0;
        flat += env.roof == "flat" ? 1 : 0;
        low += env.floors <= 2 ? 1 : 0;
    }
    const double n = double(l.envelopes.size());
    CAPTURE(built / blockArea);
    CHECK(built / blockArea > 0.45);
    CHECK(party > 0.7 * n);
    CHECK(flat > 0.9 * n);
    CHECK(low > 0.4 * n);
}

TEST_CASE("settlement meshes: streets are asphalt between sidewalks, junctions patched, blocks and lots show their ground")
{
    Layout l = mexicanCity(60000.0, 3);
    // A park lot shows grass over the block's yards.
    Lot* park = nullptr;
    for (Lot& lot : l.lots)
        if (lot.use == Use::Residential && (!park || lot.area > park->area))
            park = &lot;
    REQUIRE(park);
    park->use = Use::Park;
    const MeshParts parts = toMeshes(l, Ground::plane());

    const std::map<std::string, int> streets = facesByMaterial(parts.streets);
    CAPTURE(streets.size());
    CHECK(streets.count("Settlement_Asphalt"));
    CHECK(streets.count("Settlement_Sidewalk"));
    CHECK(parts.streets.materialNames().size() == surfaceMaterials().size());
    int patches = 0;
    for (int32_t s : parts.streets.faceAttrs().get<int32_t>("street"))
        patches += s < 0 ? 1 : 0;
    CHECK(patches > 10);
    CHECK(facesFacingDown(parts.streets) == 0);

    const std::map<std::string, int> ground = facesByMaterial(parts.ground);
    CHECK(ground.count("Settlement_Yard"));
    CHECK(ground.count("Settlement_Grass"));
    CHECK(facesFacingDown(parts.ground) == 0);
    // Large blocks are laid in pieces no wider than the grid cell, so they follow uneven ground.
    CHECK(parts.ground.numFaces() > l.blocks.size());
}

TEST_CASE("settlement meshes: sidewalks stand a curb above the road, junctions meet the streets without overlaps")
{
    const Layout l = mexicanCity(60000.0, 5);
    const MeshParts parts = toMeshes(l, Ground::plane());
    const geo::Mesh& m = parts.streets;
    const auto mat = m.faceAttrs().get<int32_t>("material");
    const auto street = m.faceAttrs().get<int32_t>("street");
    const auto& names = m.materialNames();
    auto slot = [&](const char* name) { return int32_t(std::find(names.begin(), names.end(), name) - names.begin()); };
    const int32_t asphalt = slot("Settlement_Asphalt"), sidewalk = slot("Settlement_Sidewalk"), concrete = slot("Settlement_Concrete");
    const int32_t dirt = slot("Settlement_Dirt");

    // Up-facing faces lie flat at one height per surface on flat ground; curbs stand between the two.
    struct Flat
    {
        std::vector<Vec2> ring;
        Vec2 lo, hi;
        bool road;
    };
    std::vector<Flat> flats;
    std::set<double> roadY, walkY;
    int kerbs = 0, junctionKerbs = 0, slanted = 0;
    for (geo::Index f = 0; f < geo::Index(m.numFaces()); ++f)
    {
        std::vector<glm::dvec3> pts;
        for (geo::Index c : m.faceCorners(f))
            pts.push_back(m.position(m.cornerPoint(c)));
        glm::dvec3 n(0.0);
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const glm::dvec3 &a = pts[k], &b = pts[(k + 1) % pts.size()];
            n += glm::dvec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
        }
        // A street running out of the town comes down to the ground as a dirt track.
        if (mat[f] == dirt)
            continue;
        // On flat ground every face is level or upright: anything between is a fold or a sliver standing up.
        const bool level = n.y > (1.0 - 1e-9) * glm::length(n), upright = std::abs(n.y) < 1e-6 * glm::length(n);
        slanted += level || upright ? 0 : 1;
        if (mat[f] == concrete && upright)
        {
            ++kerbs;
            junctionKerbs += street[f] < 0 ? 1 : 0;
            continue;
        }
        if (n.y < 0.5 * glm::length(n) || (mat[f] != asphalt && mat[f] != sidewalk))
            continue;
        Flat fl{{}, Vec2(1e300), Vec2(-1e300), mat[f] == asphalt};
        for (const glm::dvec3& p : pts)
        {
            (fl.road ? roadY : walkY).insert(std::round(p.y * 100.0) / 100.0);
            fl.ring.push_back({p.x, p.z});
            fl.lo = glm::min(fl.lo, fl.ring.back());
            fl.hi = glm::max(fl.hi, fl.ring.back());
        }
        flats.push_back(std::move(fl));
    }
    CHECK(slanted == 0);
    CHECK(kerbs > 100);
    CHECK(junctionKerbs > 20);
    REQUIRE(roadY.size() == 1);
    REQUIRE(walkY.size() == 1);
    CHECK(*walkY.begin() - *roadY.begin() == doctest::Approx(12.0));

    // A grid of samples over the town: no point lies under two street surfaces (a junction patch over a sidewalk, or a
    // sidewalk corner over the road), except on shared edges.
    Vec2 lo(1e300), hi(-1e300);
    for (const Flat& f : flats)
        lo = glm::min(lo, f.lo), hi = glm::max(hi, f.hi);
    const double cell = 1000.0;
    const int nx = int((hi.x - lo.x) / cell) + 1, nz = int((hi.y - lo.y) / cell) + 1;
    std::vector<std::vector<size_t>> bins(size_t(nx * nz));
    for (size_t i = 0; i < flats.size(); ++i)
        for (int x = int((flats[i].lo.x - lo.x) / cell); x <= int((flats[i].hi.x - lo.x) / cell); ++x)
            for (int z = int((flats[i].lo.y - lo.y) / cell); z <= int((flats[i].hi.y - lo.y) / cell); ++z)
                bins[size_t(z * nx + x)].push_back(i);
    auto inside = [](const std::vector<Vec2>& r, Vec2 p) {
        bool in = false;
        for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
            if ((r[i].y > p.y) != (r[j].y > p.y) && p.x < (r[j].x - r[i].x) * (p.y - r[i].y) / (r[j].y - r[i].y) + r[i].x)
                in = !in;
        return in;
    };
    int covered = 0, twice = 0;
    for (double x = lo.x + 37.1; x < hi.x; x += 113.0)
        for (double z = lo.y + 41.3; z < hi.y; z += 107.0)
        {
            const Vec2 p(x, z);
            int over = 0;
            for (size_t i : bins[size_t(int((z - lo.y) / cell) * nx + int((x - lo.x) / cell))])
                if (p.x >= flats[i].lo.x && p.x <= flats[i].hi.x && p.y >= flats[i].lo.y && p.y <= flats[i].hi.y && inside(flats[i].ring, p))
                    ++over;
            covered += over > 0 ? 1 : 0;
            twice += over > 1 ? 1 : 0;
        }
    CAPTURE(covered);
    CAPTURE(twice);
    CHECK(covered > 1000);
    CHECK(twice < covered / 200);
}

namespace
{
/** Up-facing sidewalk faces with a point inside an up-facing carriageway face: the centre and halfway to each corner. */
int sidewalkOnRoad(const geo::Mesh& m, int* sidewalks)
{
    const auto mat = m.faceAttrs().get<int32_t>("material");
    const auto& names = m.materialNames();
    auto slot = [&](const char* name) { return int32_t(std::find(names.begin(), names.end(), name) - names.begin()); };
    const int32_t walk = slot("Settlement_Sidewalk");
    std::vector<std::vector<Vec2>> roads, walks;
    for (geo::Index f = 0; f < geo::Index(m.numFaces()); ++f)
    {
        std::vector<Vec2> ring;
        glm::dvec3 n(0.0);
        const std::vector<geo::Index> pts = m.facePoints(f);
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const glm::dvec3 a = m.position(pts[k]), b = m.position(pts[(k + 1) % pts.size()]);
            n += glm::dvec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
            ring.push_back({a.x, a.z});
        }
        if (n.y < 0.5 * glm::length(n))
            continue;
        (mat[f] == walk ? walks : roads).push_back(std::move(ring));
    }
    auto inside = [](const std::vector<Vec2>& r, Vec2 p) {
        bool in = false;
        for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
            if ((r[i].y > p.y) != (r[j].y > p.y) && p.x < (r[j].x - r[i].x) * (p.y - r[i].y) / (r[j].y - r[i].y) + r[i].x)
                in = !in;
        return in;
    };
    int bad = 0;
    for (const std::vector<Vec2>& w : walks)
    {
        Vec2 c(0.0);
        for (const Vec2& p : w)
            c += p / double(w.size());
        std::vector<Vec2> probes{c};
        for (const Vec2& p : w)
            probes.push_back(glm::mix(c, p, 0.5));
        bool on = false;
        for (const Vec2& p : probes)
            for (const std::vector<Vec2>& r : roads)
                on = on || inside(r, p);
        bad += on ? 1 : 0;
    }
    *sidewalks = int(walks.size());
    return bad;
}

Layout junctionLayout()
{
    Layout l{StreetGraph(150.0, 2000.0)};
    for (StreetClass c : {StreetClass::Arterial, StreetClass::Collector, StreetClass::Local, StreetClass::Alley, StreetClass::Path})
    {
        RoadProfile r;
        builtinRoadProfile(defaultRoadProfile(c), &r);
        l.roads.push_back(r);
    }
    REQUIRE(builtinProfile("mexicanCity", &l.profile));
    l.radius = 100000.0;
    return l;
}
} // namespace

TEST_CASE("settlement meshes: no sidewalk or island lies in the carriageway at skewed junctions of 3 to 6 legs")
{
    // Junctions of 3 to 6 legs at uneven angles, mixed classes.
    const StreetClass classes[] = {StreetClass::Local, StreetClass::Collector, StreetClass::Arterial, StreetClass::Local, StreetClass::Alley, StreetClass::Local};
    for (int legs = 3; legs <= 6; ++legs)
    {
        Layout l = junctionLayout();
        const Vec2 c(0.0, 0.0);
        double a = 0.3;
        for (int k = 0; k < legs; ++k)
        {
            a += 2.0 * kPi / legs * (k % 2 ? 0.6 : 1.4);
            l.graph.addStreet(c, c + Vec2(std::cos(a), std::sin(a)) * 6000.0, classes[k]);
        }
        int walks = 0;
        const int bad = sidewalkOnRoad(toMeshes(l, Ground::plane()).streets, &walks);
        CAPTURE(legs);
        CHECK(walks > 10);
        CHECK(bad == 0);
    }
    // A divided road closer than two sidewalks, joined by a short cross street, and a fork at 20 degrees.
    Layout l = junctionLayout();
    l.graph.addStreet({-8000.0, 0.0}, {8000.0, 0.0}, StreetClass::Collector);
    l.graph.addStreet({-8000.0, 700.0}, {8000.0, 700.0}, StreetClass::Collector);
    l.graph.addStreet({0.0, 0.0}, {0.0, 700.0}, StreetClass::Local);
    l.graph.addStreet({0.0, 700.0}, {0.0, 6000.0}, StreetClass::Local);
    l.graph.addStreet({0.0, 0.0}, {0.0, -6000.0}, StreetClass::Local);
    l.graph.addStreet({0.0, -6000.0}, {-1700.0, -10800.0}, StreetClass::Local);
    l.graph.addStreet({0.0, -6000.0}, {1700.0, -10800.0}, StreetClass::Local);
    int walks = 0;
    const int bad = sidewalkOnRoad(toMeshes(l, Ground::plane()).streets, &walks);
    CHECK(walks > 10);
    CHECK(bad == 0);
}

TEST_CASE("settlement ground: ribbons drape on the quad split the heightfield renderer draws")
{
    // A terrain cell whose shorter diagonal is 1-3: the renderer splits it 0-2, and the street must lie on that surface.
    const std::vector<glm::dvec3> pts = {{0.0, 0.0, 0.0}, {0.0, 50.0, 100.0}, {100.0, 20.0, 100.0}, {100.0, 50.0, 0.0}};
    const std::vector<geo::Index> sizes = {4}, corners = {0, 1, 2, 3};
    const geo::Mesh quad = geo::Mesh::fromPolygons(pts, sizes, corners);
    const Ground g = Ground::fromMesh(quad, 10.0);
    CHECK(g.surfaceHeight({50.0, 50.0}) == doctest::Approx(10.0));
    CHECK(g.surfaceHeight({75.0, 25.0}) == doctest::Approx(0.25 * 0.0 + 0.5 * 50.0 + 0.25 * 20.0));
}

TEST_CASE("settlement furniture: trees in parks and behind houses, none with empty park and yard sets")
{
    Layout l = mexicanCity(60000.0, 4);
    for (Lot& lot : l.lots)
        if (lot.area > 600.0 && lot.use == Use::Residential)
        {
            lot.use = Use::Park;
            break;
        }
    const RoadNetwork net = roadNetwork(l, Ground::plane());
    auto count = [&](const std::vector<FurnitureSet>& sets) {
        std::map<std::string, int> out;
        for (const FurniturePlacement& p : placeFurniture(l, net, Ground::plane(), sets))
            ++out[p.kind];
        return out;
    };
    const std::map<std::string, int> all = count(furnitureSets(false));
    CHECK(all.count("parkTree"));
    CHECK(all.count("yardTree"));
    CHECK(all.at("yardTree") < int(l.envelopes.size()));
    const std::map<std::string, int> none = count(applyFurnitureJson(furnitureSets(false), {{"park", json::array()}, {"yard", json::array()}}));
    CHECK_FALSE(none.count("parkTree"));
    CHECK_FALSE(none.count("yardTree"));
}

TEST_CASE("settlement furniture: street props stand on the street mesh, on the sidewalk or the road")
{
    const Layout l = mexicanCity(60000.0, 4);
    const geo::Mesh streets = toMeshes(l, Ground::plane()).streets;
    std::vector<std::pair<std::vector<Vec2>, double>> tops;
    for (geo::Index f = 0; f < geo::Index(streets.numFaces()); ++f)
    {
        std::vector<Vec2> ring;
        double y = 0.0, lo = 1e300, hi = -1e300;
        for (geo::Index p : streets.facePoints(f))
        {
            const glm::dvec3 q = streets.position(p);
            ring.push_back({q.x, q.z});
            y = q.y, lo = std::min(lo, q.y), hi = std::max(hi, q.y);
        }
        if (hi - lo < 1e-6)
            tops.push_back({std::move(ring), y});
    }
    auto inside = [](const std::vector<Vec2>& r, Vec2 p) {
        bool in = false;
        for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
            if ((r[i].y > p.y) != (r[j].y > p.y) && p.x < (r[j].x - r[i].x) * (p.y - r[i].y) / (r[j].y - r[i].y) + r[i].x)
                in = !in;
        return in;
    };
    int on = 0, off = 0;
    for (const FurniturePlacement& p : placeFurniture(l, roadNetwork(l, Ground::plane()), Ground::plane(), furnitureSets(false)))
    {
        if (p.street == 0)
            continue;
        double top = -1.0;
        for (const auto& [ring, y] : tops)
            if (inside(ring, {p.p.x, p.p.z}))
                top = std::max(top, y);
        if (top < 0.0)
            continue;
        ++on;
        off += std::abs(p.p.y - top) > 1.0 ? 1 : 0;
    }
    CAPTURE(on);
    CHECK(on > 50);
    CHECK(off < on / 20);
}

TEST_CASE("settlement styles: a wealthy house is a mansion only on a large lot")
{
    Layout l;
    l.center = {0.0, 0.0};
    l.radius = 100000.0;
    Envelope env;
    env.footprint = makePolygon({{50000, 50000}, {51000, 50000}, {51000, 51000}, {50000, 51000}}, 0);
    env.use = Use::Residential;
    env.wealth = 2;
    env.floors = 2;
    env.eraName = "modern";
    Lot lot;
    lot.use = Use::Residential;
    lot.area = 150.0;
    finishProgram(env, lot, l);
    CHECK(env.style != "mansion");
    lot.area = 900.0;
    finishProgram(env, lot, l);
    CHECK(env.style == "mansion");
}

TEST_CASE("settlement styles: a Mexican city's low shops are houses with a shop below, a US town's are commercial blocks")
{
    Layout l;
    l.center = {0.0, 0.0};
    l.radius = 100000.0;
    REQUIRE(builtinProfile("mexicanCity", &l.profile));
    Envelope env;
    env.footprint = makePolygon({{60000, 60000}, {61500, 60000}, {61500, 61400}, {60000, 61400}}, 0);
    env.use = Use::Commercial;
    env.groundUse = Use::Commercial;
    env.floors = 2;
    env.eraName = "modern";
    Lot lot;
    lot.use = Use::Commercial;
    lot.area = 210.0;
    finishProgram(env, lot, l);
    CHECK(env.style == "residential");
    CHECK(env.styleFrom.find("shop") != std::string::npos);
    env.floors = 5;
    finishProgram(env, lot, l);
    CHECK(env.style == "modernCommercial");
    REQUIRE(builtinProfile("usTown", &l.profile));
    env.floors = 2;
    finishProgram(env, lot, l);
    CHECK(env.style == "modernCommercial");
}

TEST_CASE("settlement profile: a piece of a town ranks heights and styles from the whole town's centre and radius")
{
    Options o;
    REQUIRE(builtinProfile("mexicanCity", &o.profile));
    Site s;
    s.outline = {{135000, -15000}, {165000, -15000}, {165000, 15000}, {135000, 15000}};
    const Layout own = build(s, Ground::plane(), o);
    applyJson(o.profile, json{{"townRadius", 600000.0}, {"townCentreX", 0.0}, {"townCentreZ", 0.0}});
    const Layout piece = build(s, Ground::plane(), o);
    CHECK(own.radius < 30000.0);
    CHECK(piece.center.x == doctest::Approx(0.0));
    CHECK(piece.center.y == doctest::Approx(0.0));
    CHECK(piece.radius == doctest::Approx(600000.0));
    CHECK(toJson(piece.profile)["townRadius"] == 600000.0);
    auto plainHouses = [](const Layout& l) {
        int n = 0;
        for (const Envelope& e : l.envelopes)
            n += e.styleFrom == "residential" ? 1 : 0;
        return n;
    };
    auto oldCentre = [](const Layout& l) {
        int n = 0;
        for (const Envelope& e : l.envelopes)
            n += e.styleFrom == "three floors or fewer in the old centre" ? 1 : 0;
        return n;
    };
    REQUIRE(piece.envelopes.size() > 20);
    // On its own the piece's edges are its outskirts; within 30% of the town's radius it is all old centre.
    CHECK(plainHouses(own) > 0);
    CHECK(plainHouses(piece) == 0);
    CHECK(oldCentre(piece) > oldCentre(own));

    // Ranked from the piece's own centre over its own radius, the frame changes only the street access heights take:
    // the piece's busiest streets are not the town's, so no building rises past the profile's three floors times the
    // lot's own spread of 1.2, plus one for a shop.
    o.profile.townRadius = 0.0;
    const Layout plain = build(s, Ground::plane(), o);
    applyJson(o.profile, json{{"townCentreX", plain.center.x}, {"townCentreZ", plain.center.y}, {"townRadius", plain.radius}});
    const Layout framed = build(s, Ground::plane(), o);
    auto tallest = [](const Layout& l, Use use) {
        int n = 0;
        for (const Envelope& e : l.envelopes)
            n = e.groundUse == use ? std::max(n, e.floors) : n;
        return n;
    };
    CHECK(tallest(framed, Use::Residential) <= 4);
    CHECK(tallest(framed, Use::Commercial) <= 5);
    CHECK(tallest(plain, Use::Residential) + tallest(plain, Use::Commercial) > tallest(framed, Use::Residential) + tallest(framed, Use::Commercial));
}

TEST_CASE("settlement osm: the data's town point is the centre, its own squares stand in for a generated one")
{
    loadCityPackages();
    run("newScene");
    const json terrain = run("terrainCreate", {{"size", json::array({80000.0, 80000.0})}, {"resolution", 65}, {"amplitude", 200.0}, {"seed", 5}});
    const std::string file = (std::filesystem::path(LILA_SOURCE_DIR) / "tests" / "data" / "osm" / "w1_town.osm").string();
    const json town = run("settlementFromOsm", {{"args", {file}}, {"ground", terrain["mesh"]}, {"origin", {23.2, -106.42}},
                                                {"profile", "mexicanCity"}, {"name", "Pueblo"}, {"overrides", {{"maxFloors", 3}}}});
    CHECK(town["centreFrom"] == "the data's city or town point");
    const json places = json::parse(run("getAttr", {{"args", {town["import"]["curves"]["place"].get<std::string>() + ".osmPlaces"}}}).get<std::string>());
    json at;
    for (const json& p : places)
        if (p.value("kind", std::string()) == "town")
            at = p["at"];
    REQUIRE(at.is_array());
    const json info = run("settlementInfo", {{"args", {town["mesh"]}}});
    const double x = at[0].get<double>(), z = at[at.size() >= 3 ? 2 : 1].get<double>();
    CHECK(std::hypot(info["center"][0].get<double>() - x, info["center"][1].get<double>() - z) < 2000.0);
    CHECK(info["resolvedProfile"]["maxFloors"] == 3);
    CHECK_FALSE(info["lotUses"].contains("plaza"));
}
