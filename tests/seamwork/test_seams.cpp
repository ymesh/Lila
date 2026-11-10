#include <Seams.h>
#include <lila/Bake.h>

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <tbb/global_control.h>

#include <cmath>
#include <memory>

using namespace seamwork;
using lila::geo::Index;

namespace
{
/** An axis-aligned box, faces wound outward. */
std::shared_ptr<const Mesh> box(const glm::dvec3& lo, const glm::dvec3& hi)
{
    std::vector<glm::dvec3> p(8);
    for (int k = 0; k < 8; ++k)
        p[size_t(k)] = {(k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z};
    const std::vector<Index> sizes(6, 4);
    const std::vector<Index> idx = {0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4, 2, 6, 7, 3, 0, 2, 3, 1, 4, 5, 7, 6};
    return std::make_shared<const Mesh>(Mesh::fromPolygons(p, sizes, idx));
}

Object object(std::string name, std::shared_ptr<const Mesh> m, glm::dmat4 world = glm::dmat4(1.0))
{
    Object o;
    o.name = std::move(name);
    o.bvh = std::make_shared<const MeshBvh>(*m);
    o.mesh = std::move(m);
    o.world = world;
    return o;
}

Ground flatGround(double height = 0.0)
{
    Ground g;
    g.field = std::make_shared<const Heightfield>(lila::geo::makeHeightfield(41, 41, 50.0, {-1000.0, -1000.0}, float(height)));
    g.name = "terrain";
    return g;
}

double totalLength(const Result& r, Kind kind, bool nearOnly = false)
{
    double l = 0.0;
    for (const Curve& c : r.curves)
        if (c.kind == kind && (!nearOnly || c.nearContact))
            l += c.length();
    return l;
}

Options quick()
{
    Options o;
    o.exposureRays = 4;
    return o;
}
} // namespace

TEST_CASE("seams: a wall sunk in flat ground gives its footprint exactly")
{
    const std::vector<Object> objs = {object("Wall", box({-200, -20, -10}, {200, 280, 10}))};
    const Ground g = flatGround();
    const Result r = findSeams(objs, &g, quick());
    REQUIRE(r.curves.size() == 1);
    const Curve& c = r.curves[0];
    CHECK(c.kind == Kind::Ground);
    CHECK(c.b == kTerrain);
    CHECK_FALSE(c.nearContact);
    CHECK(c.closed);
    CHECK(c.length() == doctest::Approx(840.0).epsilon(0.001));
    for (size_t k = 0; k < c.points.size(); ++k)
    {
        CHECK(std::abs(c.points[k].y) < 1e-6);
        CHECK(c.angle[k] == doctest::Approx(90.0).epsilon(0.01));
        CHECK(c.depth[k] == doctest::Approx(20.0).epsilon(0.02));
        CHECK(c.gap[k] == 0.0);
        CHECK(c.upFacing[k] == doctest::Approx(std::sqrt(0.5)).epsilon(0.01));
    }
}

TEST_CASE("seams: the same wall in a ground mesh gives the same curve")
{
    Object ground = object("Floor", box({-1000, -50, -1000}, {1000, 0, 1000}));
    ground.ground = true;
    const std::vector<Object> objs = {object("Wall", box({-200, -20, -10}, {200, 280, 10})), ground};
    const Result r = findSeams(objs, nullptr, quick());
    CHECK(totalLength(r, Kind::Ground) == doctest::Approx(840.0).epsilon(0.001));
    for (const Curve& c : r.curves)
    {
        CHECK(c.a == 0);
        CHECK(c.b == 1);
    }
}

TEST_CASE("seams: box in box gives the exact crossing loop")
{
    const std::vector<Object> objs = {object("A", box({0, 0, 0}, {100, 100, 100})), object("B", box({50, 25, 25}, {150, 75, 75}))};
    Options o = quick();
    o.nearContact = false;
    const Result r = findSeams(objs, nullptr, o);
    REQUIRE(r.curves.size() == 1);
    CHECK(r.curves[0].kind == Kind::Mesh);
    CHECK(r.curves[0].closed);
    CHECK(r.curves[0].length() == doctest::Approx(200.0).epsilon(0.001));
    for (const glm::dvec3& p : r.curves[0].points)
        CHECK(p.x == doctest::Approx(100.0));
}

TEST_CASE("seams: a wall hovering 1 cm up is a near contact with its gap")
{
    const std::vector<Object> objs = {object("Wall", box({-200, 1, -10}, {200, 301, 10}))};
    const Ground g = flatGround();
    Options o = quick();
    o.gap = 2.0;
    const Result r = findSeams(objs, &g, o);
    REQUIRE_FALSE(r.curves.empty());
    CHECK(totalLength(r, Kind::Ground, true) == doctest::Approx(840.0).epsilon(0.002));
    for (const Curve& c : r.curves)
    {
        CHECK(c.nearContact);
        for (double gap : c.gap)
            CHECK(gap == doctest::Approx(1.0).epsilon(0.01));
    }
    SUBCASE("outside the tolerance it is not")
    {
        o.gap = 0.5;
        CHECK(findSeams(objs, &g, o).curves.empty());
    }
    SUBCASE("a sunk wall has no near contact besides its crossing")
    {
        const std::vector<Object> sunk = {object("Wall", box({-200, -20, -10}, {200, 280, 10}))};
        const Result rs = findSeams(sunk, &g, o);
        CHECK(totalLength(rs, Kind::Ground, true) == 0.0);
        CHECK(totalLength(rs, Kind::Ground) == doctest::Approx(840.0).epsilon(0.001));
    }
}

TEST_CASE("seams: a box resting on another is a near contact between them")
{
    const std::vector<Object> objs = {object("Base", box({0, 0, 0}, {100, 100, 100})), object("Top", box({25, 100.5, 25}, {75, 150, 75}))};
    const Result r = findSeams(objs, nullptr, quick());
    REQUIRE_FALSE(r.curves.empty());
    CHECK(totalLength(r, Kind::Mesh, true) == doctest::Approx(200.0).epsilon(0.002));
    for (const Curve& c : r.curves)
        for (const glm::dvec3& p : c.points)
            CHECK(p.y == doctest::Approx(100.0).epsilon(1e-6));
}

TEST_CASE("seams: instances of one mesh join each other; a shared group never does")
{
    auto cube = box({0, 0, 0}, {100, 100, 100});
    auto bvh = std::make_shared<const MeshBvh>(*cube);
    std::vector<Object> objs(2);
    for (int i = 0; i < 2; ++i)
    {
        objs[size_t(i)].name = "Rock" + std::to_string(i);
        objs[size_t(i)].mesh = cube;
        objs[size_t(i)].bvh = bvh;
    }
    objs[1].world = glm::translate(glm::dmat4(1.0), glm::dvec3(50, 25, 25));
    Options o = quick();
    o.nearContact = false;
    const Result r = findSeams(objs, nullptr, o);
    CHECK(totalLength(r, Kind::Mesh) == doctest::Approx(400.0).epsilon(0.001));
    objs[0].group = objs[1].group = 3;
    CHECK(findSeams(objs, nullptr, o).curves.empty());
    objs[0].group = objs[1].group = 0;
    o.ignorePairs = {{1, 0}};
    CHECK(findSeams(objs, nullptr, o).curves.empty());
}

TEST_CASE("seams: the same result at any thread count")
{
    const std::vector<Object> objs = {object("Wall", box({-200, -20, -10}, {200, 280, 10})), object("Rock", box({150, -30, -40}, {260, 60, 40})),
                                      object("Crate", box({-100, 280, -5}, {-60, 320, 30}))};
    const Ground g = flatGround();
    Options o;
    o.exposureRays = 8;
    const Result threaded = findSeams(objs, &g, o);
    Result serial;
    {
        tbb::global_control one(tbb::global_control::max_allowed_parallelism, 1);
        serial = findSeams(objs, &g, o);
    }
    REQUIRE(threaded.curves.size() == serial.curves.size());
    for (size_t i = 0; i < serial.curves.size(); ++i)
    {
        const Curve &a = threaded.curves[i], &b = serial.curves[i];
        CHECK(a.a == b.a);
        CHECK(a.b == b.b);
        REQUIRE(a.points.size() == b.points.size());
        for (size_t k = 0; k < a.points.size(); ++k)
        {
            CHECK(a.points[k] == b.points[k]);
            CHECK(a.exposure[k] == b.exposure[k]);
            CHECK(a.depth[k] == b.depth[k]);
        }
    }
}

TEST_CASE("seams: points, distance fields and the terrain mask follow the curves")
{
    const std::vector<Object> objs = {object("Wall", box({-200, -20, -10}, {200, 280, 10}))};
    Ground g = flatGround();
    const Result r = findSeams(objs, &g, quick());
    const auto pts = samplePoints(r, 10.0);
    CHECK(pts.size() == 84);
    for (const SeamPoint& p : pts)
    {
        CHECK(std::abs(p.position.y) < 1e-6);
        CHECK(glm::length(bisector(p, {0, 1, 0}) - glm::normalize(p.normalA + p.normalB)) < 1e-9);
    }
    const auto d = distanceField(objs[0], 0, r, 500.0);
    // Bottom corners sit 20 cm under the footprint, top corners 280 above it.
    for (Index p = 0; p < objs[0].mesh->pointCapacity(); ++p)
        CHECK(d[p] == doctest::Approx(objs[0].mesh->position(p).y < 0 ? 20.0 : 280.0).epsilon(0.001));

    Heightfield h = *g.field;
    Band band;
    band.width = 100.0;
    band.falloff = Falloff::Linear;
    contactMask(h, g.world, r, band);
    // Samples sit on a 50 cm grid; the footprint's long edges are at z = +-10.
    CHECK(h.maskAt("contact", 0.0, 0.0) == doctest::Approx(0.9).epsilon(0.001));
    CHECK(h.maskAt("contact", 0.0, 50.0) == doctest::Approx(0.6).epsilon(0.001));
    CHECK(h.maskAt("contact", 0.0, 200.0) == 0.0);
}

TEST_CASE("seams: bands fade by their falloff, custom curves included")
{
    Band b;
    b.width = 10.0;
    b.falloff = Falloff::Linear;
    CHECK(b.weight(0.0, {}) == 1.0);
    CHECK(b.weight(5.0, {}) == doctest::Approx(0.5));
    CHECK(b.weight(20.0, {}) == 0.0);
    b.falloff = Falloff::Custom;
    b.curve = "0:1 0.5:0.2 1:0";
    CHECK(b.weight(2.5, {}) == doctest::Approx(0.6));
    b.falloff = Falloff::Smooth;
    b.noise = 0.5;
    double lo = 1.0, hi = 0.0;
    for (int k = 0; k < 50; ++k)
    {
        const double w = b.weight(5.0, glm::dvec3(k * 37.0, 0.0, k * 11.0));
        lo = std::min(lo, w);
        hi = std::max(hi, w);
    }
    CHECK(hi - lo > 0.05);
}

TEST_CASE("seams: creases between stacked boxes come from occlusion")
{
    // An L of two boxes touching along a face but not crossing: the inner corner is a crease.
    const std::vector<Object> objs = {object("Floor", box({0, 0, 0}, {200, 20, 200})), object("Block", box({50, 20.0, 50}, {150, 120, 150}))};
    Options o = quick();
    o.nearContact = false;
    o.crease = true;
    o.creaseRange = 30.0;
    const Result r = findSeams(objs, nullptr, o);
    REQUIRE(r.crease.size() == 2);
    CHECK(r.creaseBackend == "cpu");
    float most = 0.0f;
    for (const auto& values : r.crease)
        for (float v : values)
            most = std::max(most, v);
    CHECK(most > 0.0f);
}

TEST_CASE("seams: crease occlusion through the bake's AO kernel matches the CPU trace")
{
    const std::vector<Object> objs = {object("Floor", box({0, 0, 0}, {200, 20, 200})), object("Block", box({50, 20.0, 50}, {150, 120, 150})),
                                      object("Post", box({160, 20, 160}, {180, 80, 180}))};
    // Below every vertex: a buried point is solid ground to the heightfield trace but behind a one-sided surface to the bake.
    const Ground g = flatGround(-5.0);
    Options o = quick();
    o.nearContact = false;
    o.crease = true;
    o.creaseRange = 30.0;
    o.creaseRays = 256;
    o.creaseBackend = "cpu";
    const Result cpu = findSeams(objs, &g, o);
    o.creaseBackend = "bakeCpu";
    lila::bake::registerBuiltinMaps();
    const Result baked = findSeams(objs, &g, o);
    CHECK(baked.creaseBackend == "bakeCpu");
    REQUIRE(baked.crease.size() == cpu.crease.size());
    size_t compared = 0, enclosed = 0;
    for (size_t i = 0; i < cpu.crease.size(); ++i)
    {
        REQUIRE(baked.crease[i].size() == cpu.crease[i].size());
        for (size_t p = 0; p < cpu.crease[i].size(); ++p)
        {
            INFO("object " << i << " point " << p);
            CHECK(std::abs(baked.crease[i][p] - cpu.crease[i][p]) < 0.15f);
            enclosed += baked.crease[i][p] > 0.2f;
            ++compared;
        }
    }
    CHECK(compared > 16);
    CHECK(enclosed > 0);

    // With no GPU bake backend, auto traces on the CPU and says so.
    o.creaseBackend = "auto";
    const Result fallback = findSeams(objs, &g, o);
    CHECK(fallback.creaseBackend == "cpu");
    bool warned = false;
    for (const std::string& w : fallback.warnings)
        warned = warned || w.find("creases traced on the CPU") != std::string::npos;
    CHECK(warned);
}

#include <Scatter.h>

TEST_CASE("scatter: placements sit on the join, wedge along the bisector and repeat exactly")
{
    const std::vector<Object> objs = {object("Wall", box({-200, -20, -10}, {200, 280, 10}))};
    const Ground g = flatGround();
    const Result r = findSeams(objs, &g, quick());
    ScatterOptions o;
    o.density = 5.0;
    o.clump = 1;
    o.clumpRadius = 0.0;
    o.spread = 0.0;
    o.sink = 0.0;
    o.tilt = 0.0;
    const std::vector<AssetShape> shapes = {{10.0, 5.0}};
    const auto placed = distribute(r, shapes, o);
    CHECK(placed.size() == 42);
    const SegmentIndex idx(r);
    for (const Placement& p : placed)
    {
        CHECK(idx.distance(glm::dvec3(p.world[3]), 1e9) < 1e-6);
        // Up (the asset's Y) leans out of the 90 degree wedge at 45 degrees.
        const glm::dvec3 y = glm::normalize(glm::dvec3(p.world[1]));
        CHECK(y.y == doctest::Approx(std::sqrt(0.5)).epsilon(0.01));
    }
    const auto again = distribute(r, shapes, o);
    REQUIRE(again.size() == placed.size());
    for (size_t i = 0; i < placed.size(); ++i)
        CHECK(again[i].world == placed[i].world);

    o.spread = 50.0;
    o.clumpRadius = 10.0;
    o.clump = 3;
    for (const Placement& p : distribute(r, shapes, o))
        CHECK(idx.distance(p.base, 1e9) <= 60.0 + 1e-6);
    o.angleMax = 45.0;
    CHECK(distribute(r, shapes, o).empty());
}

TEST_CASE("scatter: proximity rules repel, shrink, tilt and cull")
{
    Object door = object("Door", box({-50, 0, -50}, {50, 200, 50}));
    ProximityRule repel{RuleKind::Repel, door, 100.0, 1.0};
    const glm::dmat4 at = glm::translate(glm::dmat4(1.0), glm::dvec3(80, 0, 0));
    RuleOutcome r = applyRules(at, {repel}, {0, 1, 0});
    CHECK(r.world[3].x == doctest::Approx(150.0));
    ProximityRule shrink{RuleKind::Shrink, door, 100.0, 0.5};
    r = applyRules(at, {shrink}, {0, 1, 0});
    CHECK(glm::length(glm::dvec3(r.world[0])) == doctest::Approx(0.5 + 0.5 * 0.3));
    ProximityRule cull{RuleKind::CullInside, door, 0.0, 0.0};
    CHECK(applyRules(glm::translate(glm::dmat4(1.0), glm::dvec3(0, 100, 0)), {cull}, {0, 1, 0}).culled);
    CHECK_FALSE(applyRules(at, {cull}, {0, 1, 0}).culled);
    ProximityRule tilt{RuleKind::Tilt, door, 100.0, 30.0};
    r = applyRules(at, {tilt}, {0, 1, 0});
    CHECK(glm::degrees(std::acos(glm::normalize(glm::dvec3(r.world[1])).y)) == doctest::Approx(30.0 * 0.7).epsilon(0.01));
    CHECK(r.world[1].x > 0.0);
}

TEST_CASE("scatter: every preset builds its assets")
{
    for (const Preset& p : presets())
        for (int k = 0; k < p.variants; ++k)
        {
            const Mesh m = p.asset(k, 1);
            CHECK(m.numFaces() > 0);
            CHECK(m.bounds().min.y >= -1e-6);
        }
}

#include <Strip.h>

TEST_CASE("strips: ribbons lie on the ground beside the wall, faded and facing up")
{
    const std::vector<Object> objs = {object("Wall", box({-200, -20, -10}, {200, 280, 10}))};
    const Ground g = flatGround();
    const Result r = findSeams(objs, &g, quick());
    StripOptions o;
    o.width = 40.0;
    o.divisions = 4;
    o.offset = 0.5;
    o.band.falloff = Falloff::Linear;
    size_t strips = 0;
    const Mesh m = buildStrips(r, o, {}, &strips);
    CHECK(strips == 1);
    REQUIRE(m.numFaces() > 0);
    const auto col = lila::geo::colors::values(m, "Col");
    for (Index p = 0; p < m.pointCapacity(); ++p)
        CHECK(m.position(p).y == doctest::Approx(0.5));
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            CHECK(m.faceNormal(f).y > 0.99);
    float lo = 1.0f, hi = 0.0f;
    for (Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c))
        {
            lo = std::min(lo, col[c].a);
            hi = std::max(hi, col[c].a);
        }
    CHECK(lo == doctest::Approx(0.0));
    CHECK(hi == doctest::Approx(1.0));

    o.side = Side::Both;
    const Mesh v = buildStrips(r, o, {});
    bool wallUp = false;
    for (Index p = 0; p < v.pointCapacity(); ++p)
        wallUp |= v.position(p).y > 39.0;
    CHECK(wallUp);
    CHECK(decalBoxes(r, o, 100.0).size() > 16);
}

#include <Skirt.h>

namespace
{
/** A closed n-sided cylinder along Y. */
std::shared_ptr<const Mesh> cylinder(double radius, double y0, double y1, int n)
{
    std::vector<glm::dvec3> p;
    for (int k = 0; k < n; ++k)
    {
        const double a = 2.0 * kPi * k / n;
        p.push_back({radius * std::cos(a), y0, -radius * std::sin(a)});
        p.push_back({radius * std::cos(a), y1, -radius * std::sin(a)});
    }
    std::vector<Index> sizes, idx;
    for (int k = 0; k < n; ++k)
    {
        const Index a = Index(2 * k), b = Index(2 * ((k + 1) % n));
        sizes.push_back(4);
        idx.insert(idx.end(), {a, b, b + 1, a + 1});
    }
    sizes.push_back(Index(n));
    for (int k = n - 1; k >= 0; --k)
        idx.push_back(Index(2 * k));
    sizes.push_back(Index(n));
    for (int k = 0; k < n; ++k)
        idx.push_back(Index(2 * k + 1));
    return std::make_shared<const Mesh>(Mesh::fromPolygons(p, sizes, idx));
}

/** Border checks: every point with side 0 or 1 lies on its surface with its normal; the only open edges are the two borders. */
void checkSkirt(const Mesh& m, const Object& wall, const Ground* ground, const Object* floor)
{
    REQUIRE(m.numFaces() > 0);
    const auto side = m.pointAttrs().get<float>("side");
    const auto normal = m.cornerAttrs().get<glm::vec3>("normal");
    REQUIRE(!side.empty());
    REQUIRE(!normal.empty());
    double worstDistance = 0.0, worstAngle = 0.0;
    const glm::dmat4 inv = glm::inverse(wall.world);
    auto wallNormalAt = [&](const glm::dvec3& q) {
        lila::geo::BvhClosest cl;
        return wall.bvh->closestPoint(glm::dvec3(inv * glm::dvec4(q, 1.0)), cl) ? glm::normalize(wall.mesh->faceNormal(cl.face)) : glm::dvec3(0.0);
    };
    auto angle = [](const glm::dvec3& a, const glm::dvec3& b) { return glm::degrees(std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0))); };
    for (Index c = 0; c < m.cornerCapacity(); ++c)
    {
        if (!m.cornerValid(c))
            continue;
        const Index p = m.cornerPoint(c);
        const glm::dvec3 pos = m.position(p);
        glm::dvec3 surfaceNormal;
        double distance;
        if (side[p] < 1e-6)
        {
            lila::geo::BvhClosest cl;
            REQUIRE(wall.bvh->closestPoint(glm::dvec3(inv * glm::dvec4(pos, 1.0)), cl));
            distance = cl.distance;
            surfaceNormal = glm::normalize(wall.mesh->faceNormal(cl.face));
        }
        else if (side[p] > 1.0 - 1e-6)
        {
            if (ground)
            {
                distance = std::abs(pos.y - ground->field->heightAt(pos.x, pos.z));
                surfaceNormal = ground->field->normalAt(pos.x, pos.z);
            }
            else
            {
                lila::geo::BvhClosest cl;
                REQUIRE(floor->bvh->closestPoint(pos, cl));
                distance = cl.distance;
                surfaceNormal = glm::normalize(floor->mesh->faceNormal(cl.face));
            }
        }
        else
            continue;
        worstDistance = std::max(worstDistance, distance);
        const glm::dvec3 cn(normal[c]);
        double off = angle(cn, surfaceNormal);
        if (side[p] < 1e-6)
        {
            // On a wall's edge the normal may turn between the faces either side of it.
            const glm::dvec3 t = glm::normalize(glm::cross(glm::dvec3(0.0, 1.0, 0.0), cn) + glm::dvec3(1e-12, 0.0, 0.0));
            const glm::dvec3 na = wallNormalAt(pos - t), nb = wallNormalAt(pos + t);
            off = std::min(off, angle(cn, na) + angle(cn, nb) - angle(na, nb));
        }
        worstAngle = std::max(worstAngle, off);
    }
    CHECK(worstDistance < 0.01);
    CHECK(worstAngle < 1.0);
    // No crack: every open edge lies on a source surface (the borders, and fillet ends that taper onto the ground).
    auto onSurface = [&](const glm::dvec3& q) {
        lila::geo::BvhClosest cl;
        double d = wall.bvh->closestPoint(glm::dvec3(inv * glm::dvec4(q, 1.0)), cl) ? cl.distance : 1e300;
        if (ground)
            d = std::min(d, std::abs(q.y - ground->field->heightAt(q.x, q.z)));
        if (floor && floor->bvh->closestPoint(q, cl))
            d = std::min(d, cl.distance);
        return d < 0.01;
    };
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
        {
            const glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
            INFO("open edge " << a.x << "," << a.y << "," << a.z << " to " << b.x << "," << b.y << "," << b.z);
            CHECK(onSurface((a + b) * 0.5));
        }
}
} // namespace

TEST_CASE("skirt: a sunk round column is swept, its borders on both surfaces with their normals")
{
    const std::vector<Object> objs = {object("Column", cylinder(60.0, -30.0, 300.0, 24))};
    const Ground g = flatGround();
    const Result r = findSeams(objs, &g, quick());
    REQUIRE(r.curves.size() == 1);
    SkirtOptions o;
    o.width = 20.0;
    const SkirtResult s = buildSkirt(r, objs, &g, o);
    REQUIRE(s.curves.size() == 1);
    CHECK(s.curves[0].method == "sweep");
    checkSkirt(s.mesh, objs[0], &g, nullptr);
}

TEST_CASE("skirt: a box wall on a floor mesh gets a skirt and says which method made it")
{
    Object floor = object("Floor", box({-1000, -50, -1000}, {1000, 0, 1000}));
    floor.ground = true;
    const std::vector<Object> objs = {object("Wall", box({-200, -20, -10}, {200, 280, 10})), floor};
    const Result r = findSeams(objs, nullptr, quick());
    SkirtOptions o;
    o.width = 15.0;
    const SkirtResult s = buildSkirt(r, objs, nullptr, o);
    REQUIRE_FALSE(s.curves.empty());
    // Whatever ran is named with its reason (a fallback says what it fell back from).
    CHECK_FALSE(s.curves[0].reason.empty());
    CHECK(s.curves[0].method != "none");
    checkSkirt(s.mesh, objs[0], nullptr, &objs[1]);
}

TEST_CASE("skirt: on terrain the box wall is unioned with a fillet, borders on both surfaces")
{
    const std::vector<Object> objs = {object("Wall", box({-200, -20, -10}, {200, 280, 10}))};
    const Ground g = flatGround();
    const Result r = findSeams(objs, &g, quick());
    SkirtOptions o;
    o.width = 15.0;
    const SkirtResult s = buildSkirt(r, objs, &g, o);
    REQUIRE(s.curves.size() == 1);
    CHECK(s.curves[0].method == "union");
    CHECK(s.curves[0].reason.find("past") != std::string::npos);
    checkSkirt(s.mesh, objs[0], &g, nullptr);
}

TEST_CASE("skirt: a unioned box wall's outer corners are rounded, not tapered")
{
    const std::vector<Object> objs = {object("Wall", box({-200, -20, -10}, {200, 280, 10}))};
    const Ground g = flatGround();
    const Result r = findSeams(objs, &g, quick());
    SkirtOptions o;
    o.width = 15.0;
    o.method = SkirtMethod::Union;
    const SkirtResult s = buildSkirt(r, objs, &g, o);
    REQUIRE(s.curves.size() == 1);
    REQUIRE(s.curves[0].method == "union");
    checkSkirt(s.mesh, objs[0], &g, nullptr);
    // At each corner the ground border runs round it at the fillet width, like the ball the fillet rolls.
    const auto side = s.mesh.pointAttrs().get<float>("side");
    for (const glm::dvec2 corner : {glm::dvec2(200, 10), glm::dvec2(200, -10), glm::dvec2(-200, 10), glm::dvec2(-200, -10)})
    {
        double nearest = 1e300, farthest = 0.0;
        for (Index p = 0; p < s.mesh.pointCapacity(); ++p)
        {
            if (!s.mesh.pointValid(p) || side[p] < 1.0 - 1e-6)
                continue;
            const glm::dvec3 q = s.mesh.position(p);
            const glm::dvec2 d = glm::dvec2(q.x, q.z) - corner;
            // The outer quadrant only: past both faces that meet at the corner.
            if (d.x * corner.x <= 0.0 || d.y * corner.y <= 0.0)
                continue;
            nearest = std::min(nearest, glm::length(d));
            farthest = std::max(farthest, glm::length(d));
        }
        INFO("corner " << corner.x << "," << corner.y);
        CHECK(nearest > o.width * 0.85);
        CHECK(farthest < o.width * 1.15);
    }
    // Shading follows the surface round the corners: no corner normal turned far from its face.
    const auto normal = s.mesh.cornerAttrs().get<glm::vec3>("normal");
    double worst = 0.0;
    for (Index f = 0; f < s.mesh.faceCapacity(); ++f)
    {
        if (!s.mesh.faceValid(f) || glm::length(s.mesh.faceAreaVector(f)) < 1e-3)
            continue;
        const glm::dvec3 fn = glm::normalize(s.mesh.faceAreaVector(f));
        for (Index c : s.mesh.faceCorners(f))
            worst = std::max(worst, glm::degrees(std::acos(std::clamp(glm::dot(fn, glm::dvec3(normal[c])), -1.0, 1.0))));
    }
    CHECK(worst < 60.0);
}
