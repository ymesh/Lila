#include "TestUtil.h"

#include "Ivy.h"
#include "IvyGrow.h"
#include "Pieces.h"

#include "geo/Bvh.h"
#include "gn/NodeLib.h"
#include "scene/Materials.h"
#include "scene/Transform.h"

using namespace weatheringtest;
namespace ivy = weathering::ivy;

namespace
{
/** A closed box, faces outward. */
geo::Mesh box(const glm::dvec3& lo, const glm::dvec3& hi)
{
    geo::Mesh m;
    geo::Index v[8];
    for (int i = 0; i < 8; ++i)
        v[i] = m.addPoint({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
    const int faces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    for (const auto& f : faces)
        m.addFace({v[f[0]], v[f[1]], v[f[2]], v[f[3]]});
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (glm::dot(m.faceNormal(f), m.faceCentroid(f) - (lo + hi) * 0.5) < 0.0)
            m.flipFace(f);
    return m;
}

/** A wall 400 wide, 300 tall and 30 deep standing on a ground slab. */
geo::Mesh wallScene()
{
    geo::Mesh m = box({-200, 0, -15}, {200, 300, 15});
    m.append(box({-600, -10, -600}, {600, 0, 600}));
    return m;
}

ivy::Pieces leafPieces() { return {.leaves = {{ivy::leafCard(false, 0), 1.0}}, .flowers = {}, .stems = {}}; }

std::vector<glm::dvec3> positionsOf(const geo::Mesh& m)
{
    std::vector<glm::dvec3> out;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            out.push_back(m.position(p));
    return out;
}
} // namespace

TEST_CASE("ivy grower: clinging strands stay within the adhesion distance and growth within the length")
{
    const geo::Mesh surface = wallScene();
    const std::vector<glm::dvec3> seeds = {{-120, 0, 15.5}, {0, 0, 15.5}, {120, 0, 15.5}};
    ivy::Params p;
    p.length = 260.0;
    p.adhesion = 2.0;
    p.gravity = 0.5;
    p.seed = 3;
    const ivy::Result r = ivy::grow(surface, seeds, {}, p, leafPieces());
    REQUIRE(r.strands.size() >= 3);
    CHECK(r.leaves > 20);
    const geo::MeshBvh bvh(surface);
    size_t clinging = 0;
    double highest = 0.0;
    for (const ivy::Strand& s : r.strands)
        for (size_t i = 0; i < s.points.size(); ++i)
        {
            CHECK(s.growth[i] <= p.length + 1e-6);
            double nearSeed = 1e300;
            for (const glm::dvec3& q : seeds)
                nearSeed = std::min(nearSeed, glm::distance(q, s.points[i]));
            CHECK(nearSeed <= p.length + 1.0);
            highest = std::max(highest, s.points[i].y);
            if (s.hang[i])
                continue;
            geo::BvhClosest c;
            REQUIRE(bvh.closestPoint(s.points[i], c));
            CHECK(c.distance <= p.adhesion + 1e-6);
            ++clinging;
        }
    CHECK(clinging > 50);
    // It climbs.
    CHECK(highest > 120.0);
}

TEST_CASE("ivy grower: hangs over a ledge with gravity, never without")
{
    // A low block the ivy climbs and spills over.
    geo::Mesh surface = box({-60, 0, -60}, {60, 120, 60});
    surface.append(box({-800, -10, -800}, {800, 0, 800}));
    const std::vector<glm::dvec3> seeds = {{0, 0, 60.5}, {-30, 0, 60.5}};
    ivy::Params p;
    p.length = 500.0;
    p.climb = 0.9;
    p.gravity = 1.0;
    auto hangs = [&](double gravity) {
        p.gravity = gravity;
        size_t n = 0;
        for (const ivy::Strand& s : ivy::grow(surface, seeds, {}, p, leafPieces()).strands)
            for (uint8_t h : s.hang)
                n += h;
        return n;
    };
    CHECK(hangs(1.0) > 0);
    CHECK(hangs(0.0) == 0);
}

TEST_CASE("ivy grower: the same seed grows the same ivy")
{
    const geo::Mesh surface = wallScene();
    const std::vector<glm::dvec3> seeds = {{-50, 0, 15.5}, {80, 0, 15.5}};
    ivy::Params p;
    p.length = 200.0;
    p.seed = 11;
    const ivy::Result a = ivy::grow(surface, seeds, {}, p, leafPieces());
    const ivy::Result b = ivy::grow(surface, seeds, {}, p, leafPieces());
    REQUIRE(a.mesh.numPoints() > 0);
    CHECK(positionsOf(a.mesh) == positionsOf(b.mesh));
    p.seed = 12;
    const ivy::Result c = ivy::grow(surface, seeds, {}, p, leafPieces());
    CHECK(positionsOf(a.mesh) != positionsOf(c.mesh));
}

TEST_CASE("ivy grower: toon smoothness gives smoother curves")
{
    const geo::Mesh surface = wallScene();
    const std::vector<glm::dvec3> seeds = {{-100, 0, 15.5}, {0, 0, 15.5}, {100, 0, 15.5}};
    ivy::Params p;
    p.length = 250.0;
    p.gravity = 0.0;
    p.smoothness = 0.0;
    const double natural = ivy::meanCurvature(ivy::grow(surface, seeds, {}, p, leafPieces()));
    p.smoothness = 1.0;
    const double toon = ivy::meanCurvature(ivy::grow(surface, seeds, {}, p, leafPieces()));
    MESSAGE("curvature natural " << natural << " toon " << toon);
    CHECK(natural > 0.0);
    CHECK(toon < natural * 0.7);
}

TEST_CASE("ivy grower: user leaves keep their own slots; strokes are followed")
{
    const geo::Mesh surface = wallScene();
    geo::Mesh leaf = box({-1, 0, 0}, {1, 0.2, 3});
    scene::materials::assign(leaf, "MyLeaf", {});
    ivy::Pieces pc;
    pc.leaves = {{leaf, 1.0}};
    ivy::Params p;
    p.length = 150.0;
    const std::vector<glm::dvec3> stroke = {{-150, 20, 15.5}, {-60, 120, 15.5}, {40, 200, 15.5}, {150, 260, 15.5}};
    const ivy::Result r = ivy::grow(surface, {}, std::vector<std::vector<glm::dvec3>>{stroke}, p, pc);
    REQUIRE(r.leaves > 0);
    std::set<std::string> slots;
    for (const auto& [slot, count] : scene::materials::slotUsage(r.mesh))
        slots.insert(slot);
    CHECK(slots.count("MyLeaf") == 1);
    CHECK(slots.count(ivy::kLeafSlot) == 0);
    CHECK(slots.count(ivy::kStemSlot) == 1);
    // Grows past the stroke's end region even though the stroke is longer than the length.
    double toEnd = 1e300;
    for (const ivy::Strand& s : r.strands)
        for (const glm::dvec3& q : s.points)
            toEnd = std::min(toEnd, glm::distance(q, stroke.back()));
    CHECK(toEnd < 40.0);
}

TEST_CASE("ivy grower: ground seeds sit where steep faces meet the ground")
{
    const std::vector<glm::dvec3> seeds = ivy::groundSeeds(wallScene(), 5, 1);
    REQUIRE(seeds.size() == 5);
    for (const glm::dvec3& q : seeds)
    {
        CHECK(q.y < 20.0);
        CHECK(std::abs(q.x) <= 200.5);
    }
}

TEST_CASE("ivy: live node regrows when its host moves, same seed same result, user leaf mesh used")
{
    fresh();
    const std::string wall = cube("Wall", {0, 0, 0}, {400, 300, 30});
    json r = run("ivy", {{"args", {wall}}, {"seeds", {{-100, 0, 15}, {100, 0, 15}}}, {"length", 220.0}, {"seed", 5}});
    REQUIRE(r.contains("group"));
    REQUIRE(r["ivy"].size() == 1);
    const std::string mesh = r["ivy"][0].get<std::string>();
    const std::string node = r["nodes"][0].get<std::string>();
    CHECK(r["warnings"].empty());
    json info = run("ivyInfo", {{"args", {node}}});
    CHECK(info["strands"].get<int64_t>() > 0);
    CHECK(info["leaves"].get<int64_t>() > 0);
    CHECK(info["longest"].get<double>() <= 220.0 + 1e-6);
    const geo::Mesh before = meshOf(mesh);
    REQUIRE(before.numFaces() > 0);

    // Moving the host moves the ivy with it.
    run("setAttr", {{"args", {wall + ".translateX", 500.0}}});
    const geo::Mesh moved = meshOf(mesh);
    CHECK(moved.bounds().center().x == doctest::Approx(before.bounds().center().x + 500.0).epsilon(0.01));

    // Scaling it up regrows over the bigger surface.
    run("setAttr", {{"args", {wall + ".scaleY", 2.0}}});
    const geo::Mesh scaled = meshOf(mesh);
    CHECK(positionsOf(scaled) != positionsOf(moved));
    run("setAttr", {{"args", {wall + ".scaleY", 1.0}}});
    run("setAttr", {{"args", {wall + ".translateX", 0.0}}});
    CHECK(positionsOf(meshOf(mesh)) == positionsOf(before));

    // A second ivy with the same flags is the same ivy.
    json again = run("ivy", {{"args", {wall}}, {"seeds", {{-100, 0, 15}, {100, 0, 15}}}, {"length", 220.0}, {"seed", 5}});
    CHECK(positionsOf(meshOf(again["ivy"][0].get<std::string>())) == positionsOf(before));

    // The user's own leaf mesh, with its own material slot.
    geo::Mesh leaf = box({-1, 0, 0}, {1, 0.3, 4});
    scene::materials::assign(leaf, "MyLeafMat", {});
    scene::Node* ln = g().createNode(*scene::types::find("mesh"), "MyLeaf");
    g().setValue({ln->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(leaf)));
    json own = run("ivy", {{"args", {wall}}, {"seeds", {{0, 0, 15}}}, {"leaves", {"MyLeaf"}}, {"realize", true}});
    REQUIRE(own["nodes"].empty());
    std::set<std::string> slots;
    for (const auto& [slot, count] : scene::materials::slotUsage(meshOf(own["ivy"][0].get<std::string>())))
        slots.insert(slot);
    CHECK(slots.count("MyLeafMat") == 1);
    CHECK(slots.count(ivy::kLeafSlot) == 0);
}

TEST_CASE("ivy: ground seeding, plants, brush strokes and erase")
{
    fresh();
    const std::string wall = cube("Wall", {0, 0, 0}, {300, 250, 30});
    json r = run("ivy", {{"args", {wall}}, {"plant", "vine"}, {"style", "toon"}, {"count", 4}});
    json info = run("ivyInfo", {{"args", {r["nodes"][0]}}});
    CHECK(info["strands"].get<int64_t>() > 0);
    CHECK(info["plant"] == "vine");
    // Toon placeholders are the chunky set.
    CHECK(!weathering::roleAssets("vine-leaf", weathering::Style::Toon).empty());

    json roots = run("ivy", {{"args", {wall}}, {"plant", "roots"}, {"count", 3}});
    CHECK(run("ivyInfo", {{"args", {roots["nodes"][0]}}})["leaves"].get<int64_t>() == 0);

    json painted = run("ivyPaint", {{"args", {wall}}, {"points", {{-100, 20, 15}, {-40, 100, 15}, {30, 180, 15}}}});
    CHECK(painted["strokes"].get<int64_t>() == 1);
    json second = run("ivyPaint", {{"args", {wall}}, {"points", {{80, 20, 15}, {100, 120, 15}}}});
    CHECK(second["strokes"].get<int64_t>() == 2);
    CHECK(second["nodes"][0] == painted["nodes"][0]);
    json erased = run("ivyPaint", {{"args", {wall}}, {"points", {{90, 70, 15}}}, {"erase", true}, {"radius", 200.0}});
    CHECK(erased["erased"].get<int>() > 0);
}

TEST_CASE("ivy: the Ivy geometry node is registered for Masonry and node graphs")
{
    fresh();
    const gn::NodeDecl* d = gn::findDecl(weathering::kIvyGeometryNode);
    REQUIRE(d);
    CHECK(d->sockets.inputIndex("surface") >= 0);
    CHECK(d->sockets.outputIndex("stems") >= 0);
}
