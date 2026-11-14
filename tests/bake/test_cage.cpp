#include <doctest/doctest.h>

#include "TestUtil.h"
#include "bake/BakeNodes.h"
#include "bake/Cage.h"
#include "bake/CageEstimate.h"
#include "bake/CagePreview.h"
#include "core/MainQueue.h"
#include "core/Progress.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Triangulate.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <set>
#include <thread>

using namespace lila;
using namespace baketest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
/** A project with one group: low and high meshes, output into a temp folder. */
std::string pairGroup(const std::string& low, const std::string& high, const fs::path& folder)
{
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p}, {"folder", folder.generic_string()}, {"resolution", 64}, {"supersampling", 1}});
    ops::execute("bakeGroup", {{"args", {"pair"}}, {"project", p}, {"lows", {low}}, {"highs", {high}}});
    return "pair";
}
} // namespace

TEST_CASE("UV-space dabs reach the points whose corners lie under the brush")
{
    geo::PlaneOptions o;
    o.subdivX = o.subdivZ = 4;
    auto plane = std::make_shared<const geo::Mesh>(geo::makePlane(o));
    bake::PointPaint paint(plane, *bake::paintAttr("offset"));
    std::span<const glm::vec2> uv = plane->cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!uv.empty());

    const glm::dvec2 center(0.5, 0.5);
    const double radius = 0.3;
    std::set<geo::Index> expected;
    for (geo::Index c = 0; c < plane->cornerCapacity(); ++c)
        if (plane->cornerValid(c) && glm::length(glm::dvec2(uv[c]) - center) <= radius)
            expected.insert(plane->cornerPoint(c));
    const auto pts = paint.uvPoints(center, radius);
    std::set<geo::Index> got;
    for (auto [p, w] : pts)
        if (w > 0.0f)
            got.insert(p);
    CHECK(got == expected);
    // Full weight at the centre point, none at the rim.
    for (auto [p, w] : pts)
        CHECK(w <= 1.0f);

    // A dab inside one face larger than the brush still reaches that face's corners (weight by distance, may be 0).
    geo::PlaneOptions big;
    auto quad = std::make_shared<const geo::Mesh>(geo::makePlane(big));
    bake::PointPaint one(quad, *bake::paintAttr("offset"));
    one.setFalloff(bake::PaintFalloff::Constant);
    CHECK(one.uvPoints({0.5, 0.5}, 0.05).size() == 4);
    CHECK(one.uvPoints({5.0, 5.0}, 0.05).empty());
}

TEST_CASE("UV dabs keep points split across islands consistent: one application per point")
{
    auto cube = std::make_shared<const geo::Mesh>(geo::makeCube());
    bake::PointPaint paint(cube, *bake::paintAttr("offset"));
    paint.setFalloff(bake::PaintFalloff::Constant);
    // Every cube point has corners in several islands; a dab over the whole layout reaches each point once.
    const auto pts = paint.uvPoints({0.5, 0.5}, 2.0);
    CHECK(pts.size() == cube->numPoints());
    std::set<geo::Index> unique;
    for (auto [p, w] : pts)
        unique.insert(p);
    CHECK(unique.size() == pts.size());
    paint.apply(pts, bake::PaintOp::Add, 1.0f, 1.0f);
    for (geo::Index p = 0; p < cube->pointCapacity(); ++p)
        if (cube->pointValid(p))
            CHECK(paint.values()[p] == doctest::Approx(2.0f));

    // A small dab on one island's corner still moves the point every island shows it at.
    std::span<const glm::vec2> uv = cube->cornerAttrs().get<glm::vec2>("uv0");
    const geo::Index c0 = cube->faceCorner(0);
    const geo::Index p0 = cube->cornerPoint(c0);
    bake::PointPaint small(cube, *bake::paintAttr("offset"));
    small.apply(small.uvPoints(glm::dvec2(uv[c0]), 0.01), bake::PaintOp::Set, 3.0f, 1.0f);
    CHECK(small.values()[p0] == doctest::Approx(3.0f));
    size_t corners = 0;
    std::set<std::pair<float, float>> places;
    cube->forEachPointCorner(p0, [&](geo::Index c) {
        ++corners;
        places.insert({uv[c].x, uv[c].y});
    });
    CHECK(places.size() > 1); // the point really is split across islands
}

TEST_CASE("bakeAutoPaintCage: one click gives a near-zero-miss cage on known pairs")
{
    struct Pair
    {
        const char* name;
        geo::Mesh high;
    };
    geo::SphereOptions so;
    so.radius = 75.0;
    so.subdivAxis = 64;
    so.subdivHeight = 32;
    geo::CubeOptions crate;
    crate.width = crate.height = crate.depth = 104.0;
    crate.subdivX = crate.subdivY = crate.subdivZ = 6;
    geo::Mesh lumpy = geo::makeCube(crate);
    // Panels pushed in and out across the faces, like a crate's planks.
    for (geo::Index p = 0; p < lumpy.pointCapacity(); ++p)
        if (lumpy.pointValid(p))
        {
            glm::dvec3 q = lumpy.position(p);
            q *= 1.0 + 0.04 * std::sin(q.x * 0.11) * std::cos(q.z * 0.13) - 0.03 * std::sin(q.y * 0.17);
            lumpy.setPosition(p, q);
        }
    std::vector<Pair> pairs;
    pairs.push_back({"sphere over cube", geo::makeSphere(so)});
    pairs.push_back({"lumpy crate", std::move(lumpy)});

    for (Pair& pr : pairs)
    {
        const std::string name = pr.name;
        CAPTURE(name);
        newScene();
        geo::CubeOptions lo;
        lo.subdivX = lo.subdivY = lo.subdivZ = 3;
        const std::string low = addMesh("box_low", geo::makeCube(lo));
        const std::string high = addMesh("box_high", std::move(pr.high));
        const std::string grp = pairGroup(low, high, tempDir("lila_cage_estimate"));
        // A hand-set cage of a few cm, as a group starts out before anyone tunes it.
        ops::execute("bakeGroup", {{"args", {grp}}, {"frontal", 3.0}, {"rear", 3.0}});

        const json before = ops::execute("bakeCageCoverage", {{"args", {grp}}, {"resolution", 256}});
        const json r = ops::execute("bakeAutoPaintCage", {{"args", {grp}}, {"resolution", 256}});
        MESSAGE(std::string(pr.name) << ": miss rate before " << r["before"]["missRate"].get<double>() * 100.0 << "%, after "
                        << r["after"]["missRate"].get<double>() * 100.0 << "% (" << r["after"]["misses"] << " of " << r["after"]["samples"]
                        << "), frontal " << r["frontal"] << " rear " << r["rear"] << ", passes " << r["iterations"]);
        CHECK(before["misses"].get<uint64_t>() == r["before"]["misses"].get<uint64_t>());
        CHECK(r["before"]["misses"].get<uint64_t>() > 0);
        CHECK(r["after"]["missRate"].get<double>() < 0.0005);

        // Applied: the scene now bakes with it, and it is one undo step.
        const json after = ops::execute("bakeCageCoverage", {{"args", {grp}}, {"resolution", 256}});
        CHECK(after["misses"] == r["after"]["misses"]);
        const bake::GroupSettings s = bake::groupSettings(*graph().find(grp));
        CHECK(s.frontal == doctest::Approx(r["frontal"].get<double>()));
        CHECK(baseMesh(low)->pointAttrs().has(bake::kOffsetAttr));
        UndoStack::get().undo();
        CHECK(!baseMesh(low)->pointAttrs().has(bake::kOffsetAttr));
        CHECK(bake::groupSettings(*graph().find(grp)).frontal == doctest::Approx(3.0));
    }
}

TEST_CASE("bakeAutoPaintCage: a low whose modifiers change its points gets the cage on its base points")
{
    newScene();
    geo::CubeOptions lo;
    lo.subdivX = lo.subdivY = lo.subdivZ = 2;
    const std::string low = addMesh("box_low", geo::makeCube(lo));
    ops::execute("addModifier", {{"args", {low}}, {"kind", "bevel"}, {"params", {{"segments", 2}}}});
    ops::execute("addModifier", {{"args", {low}}, {"kind", "subdivision"}});
    geo::SphereOptions so;
    so.radius = 75.0;
    so.subdivAxis = 64;
    so.subdivHeight = 32;
    const std::string high = addMesh("box_high", geo::makeSphere(so));
    const std::string grp = pairGroup(low, high, tempDir("lila_cage_estimate_mods"));
    ops::execute("bakeGroup", {{"args", {grp}}, {"frontal", 3.0}, {"rear", 3.0}});
    const geo::Index basePoints = baseMesh(low)->pointCapacity();
    REQUIRE(lila::scene::modifiers::exportMesh(graph(), graph().find(low)->id())->pointCapacity() != basePoints);

    const json r = ops::execute("bakeAutoPaintCage", {{"args", {grp}}, {"resolution", 256}});
    MESSAGE("miss rate before " << r["before"]["missRate"].get<double>() * 100.0 << "%, after " << r["after"]["missRate"].get<double>() * 100.0 << "%");
    CHECK(r["mapped"] == json::array({"box_low"}));
    CHECK(r["after"]["missRate"].get<double>() < r["before"]["missRate"].get<double>());
    CHECK(r["after"]["missRate"].get<double>() < 0.005);
    const json after = ops::execute("bakeCageCoverage", {{"args", {grp}}, {"resolution", 256}});
    CHECK(after["misses"] == r["after"]["misses"]);
    const geo::Mesh& base = *baseMesh(low);
    CHECK(base.pointCapacity() == basePoints);
    CHECK(base.pointAttrs().has(bake::kOffsetAttr));
    CHECK(!base.pointAttrs().has("bakeCageProbeX"));
    UndoStack::get().undo();
    CHECK(!baseMesh(low)->pointAttrs().has(bake::kOffsetAttr));
}

TEST_CASE("bakeAutoPaintCage: a low's cage stops short of another low lying on it")
{
    newScene();
    // A skirt and a ribbon 5 cm above its middle, both inside a high box whose top is 20 cm up: both push out past 15 cm,
    // but the skirt under the ribbon has to stop short of it.
    geo::PlaneOptions skirtO;
    skirtO.subdivX = skirtO.subdivZ = 10;
    const std::string skirt = addMesh("skirt_low", geo::makePlane(skirtO));
    geo::PlaneOptions ribbonO;
    ribbonO.width = ribbonO.depth = 30.0;
    ribbonO.subdivX = ribbonO.subdivZ = 3;
    geo::Mesh ribbonMesh = geo::makePlane(ribbonO);
    for (geo::Index q = 0; q < ribbonMesh.pointCapacity(); ++q)
        if (ribbonMesh.pointValid(q))
            ribbonMesh.setPosition(q, ribbonMesh.position(q) + glm::dvec3(0.0, 5.0, 0.0));
    const std::string ribbon = addMesh("ribbon_low", std::move(ribbonMesh));
    geo::CubeOptions boxO;
    boxO.width = boxO.depth = 140.0;
    boxO.height = 40.0;
    const std::string box = addMesh("box_high", geo::makeCube(boxO));
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p}, {"folder", tempDir("lila_cage_clearance").generic_string()}, {"resolution", 64}, {"supersampling", 1}});
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {skirt, ribbon}}, {"highs", {box}}});

    const json r = ops::execute("bakeAutoPaintCage", {{"args", {"g"}}, {"resolution", 128}});
    const double frontal = r["frontal"].get<double>();
    const geo::Mesh& m = *baseMesh(skirt);
    const auto offsets = m.pointAttrs().get<float>(bake::kOffsetAttr);
    double under = 0.0, open = std::numeric_limits<double>::max();
    for (geo::Index q = 0; q < m.pointCapacity(); ++q)
    {
        if (!m.pointValid(q))
            continue;
        const glm::dvec3 at = m.position(q);
        const double push = double(offsets[q]) * frontal;
        if (std::abs(at.x) < 14.0 && std::abs(at.z) < 14.0)
            under = std::max(under, push);
        else if (std::abs(at.x) > 25.0 || std::abs(at.z) > 25.0)
            open = std::min(open, push);
    }
    MESSAGE("skirt push under the ribbon " << under << " cm, away from it " << open << " cm");
    CHECK(under < 5.0);
    CHECK(open > 15.0);
}

TEST_CASE("bakeAutoPaintCage: a low's cage stops short of its own surface across a fold")
{
    newScene();
    // The skirt and a closed slab 5 cm above its middle as one low (an arm over the torso): the skirt under the slab and
    // the slab's underside push into the same gap, so each keeps under half of it.
    geo::PlaneOptions skirtO;
    skirtO.subdivX = skirtO.subdivZ = 10;
    geo::Mesh low = geo::makePlane(skirtO);
    geo::CubeOptions slabO;
    slabO.width = slabO.depth = 30.0;
    slabO.height = 2.0;
    slabO.subdivX = slabO.subdivZ = 3;
    geo::Mesh slab = geo::makeCube(slabO);
    for (geo::Index q = 0; q < slab.pointCapacity(); ++q)
        if (slab.pointValid(q))
            slab.setPosition(q, slab.position(q) + glm::dvec3(0.0, 6.0, 0.0));
    low.append(slab);
    const std::string folded = addMesh("folded_low", std::move(low));
    geo::CubeOptions boxO;
    boxO.width = boxO.depth = 140.0;
    boxO.height = 40.0;
    const std::string box = addMesh("box_high", geo::makeCube(boxO));
    pairGroup(folded, box, tempDir("lila_cage_fold"));

    const json r = ops::execute("bakeAutoPaintCage", {{"args", {"pair"}}, {"resolution", 128}});
    const double frontal = r["frontal"].get<double>();
    const geo::Mesh& m = *baseMesh(folded);
    const auto offsets = m.pointAttrs().get<float>(bake::kOffsetAttr);
    double under = 0.0, open = std::numeric_limits<double>::max();
    for (geo::Index q = 0; q < m.pointCapacity(); ++q)
    {
        if (!m.pointValid(q))
            continue;
        const glm::dvec3 at = m.position(q);
        if (std::abs(at.y) > 0.01)
            continue;
        const double push = double(offsets[q]) * frontal;
        if (std::abs(at.x) < 14.0 && std::abs(at.z) < 14.0)
            under = std::max(under, push);
        else if (std::abs(at.x) > 25.0 || std::abs(at.z) > 25.0)
            open = std::min(open, push);
    }
    MESSAGE("skirt push under its own slab " << under << " cm, away from it " << open << " cm");
    CHECK(under < 2.5);
    CHECK(open > 15.0);
}

namespace
{
/** The most push (offset times frontal) on a low's base points after an auto paint. */
double mostPush(const std::string& low, const json& r)
{
    const auto offsets = baseMesh(low)->pointAttrs().get<float>(bake::kOffsetAttr);
    double most = 0.0;
    for (float o : offsets)
        most = std::max(most, double(o) * r["frontal"].get<double>());
    return most;
}
} // namespace

TEST_CASE("bakeAutoPaintCage: a low baking onto itself ignores its other shells where they cross")
{
    newScene();
    // A block with a bar run through it, one mesh baking onto its own smoothed self: where each shell sits inside the
    // other, walking out would cross the other's whole length.
    geo::CubeOptions blockO;
    blockO.width = blockO.height = blockO.depth = 20.0;
    blockO.subdivX = blockO.subdivY = blockO.subdivZ = 4;
    geo::CubeOptions barO;
    barO.width = 80.0;
    barO.height = barO.depth = 6.0;
    barO.subdivX = 16;
    barO.subdivY = barO.subdivZ = 2;
    const geo::Mesh block = geo::makeCube(blockO), bar = geo::makeCube(barO);
    const geo::Mesh* parts[] = {&block, &bar};
    const std::string low = addMesh("crossed_low", geo::combine(parts));
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p}, {"folder", tempDir("lila_cage_shells").generic_string()}, {"resolution", 64}, {"supersampling", 1}});
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {low}}, {"self", true}, {"preset", "Smooth x2"}});

    const json r = ops::execute("bakeAutoPaintCage", {{"args", {"g"}}, {"resolution", 128}});
    const double most = mostPush(low, r);
    MESSAGE("most push " << most << ", miss rate " << r["after"]["missRate"].get<double>() * 100.0 << "%");
    // The bar runs 30 past each block face; a cage that tracks the smoothed surface stays within a few units of it.
    CHECK(most < 3.0);
}

TEST_CASE("bakeAutoPaintCage: a low baking onto itself doesn't push out through its own fold")
{
    newScene();
    // A bar folded back over itself, 1 unit up, as a pipe's too-tight bend folds: each half's walk out crosses the
    // other half, though its smoothed self lies within a triangle of it.
    geo::CubeOptions barO;
    barO.width = 80.0;
    barO.height = barO.depth = 6.0;
    barO.subdivX = 32;
    barO.subdivY = barO.subdivZ = 2;
    geo::Mesh bar = geo::makeCube(barO);
    for (geo::Index q = 0; q < bar.pointCapacity(); ++q)
        if (bar.pointValid(q))
            if (const glm::dvec3 at = bar.position(q); at.x > 0.0)
                bar.setPosition(q, {-at.x, at.y + 1.0 * at.x / 40.0, at.z});
    const std::string low = addMesh("folded_low", std::move(bar));
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p}, {"folder", tempDir("lila_cage_fold").generic_string()}, {"resolution", 64}, {"supersampling", 1}});
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {low}}, {"self", true}, {"preset", "Smooth x2"}});

    const json r = ops::execute("bakeAutoPaintCage", {{"args", {"g"}}, {"resolution", 128}});
    const double most = mostPush(low, r);
    MESSAGE("most push " << most);
    CHECK(most < 2.5);
}

TEST_CASE("bakeAutoPaintCage: an open sheet seen from behind doesn't read as material the low starts in")
{
    newScene();
    geo::PlaneOptions lowO;
    lowO.subdivX = lowO.subdivZ = 8;
    const std::string low = addMesh("floor_low", geo::makePlane(lowO));
    // The high under the low, and an open sheet facing away 10 above it: walking up meets the sheet's back.
    geo::PlaneOptions sheetO;
    sheetO.subdivX = sheetO.subdivZ = 8;
    const std::string under = addMesh("floor_high", geo::makePlane(sheetO), glm::dvec3(0.0, -0.5, 0.0));
    const std::string sheet = addMesh("sheet_high", geo::makePlane(sheetO), glm::dvec3(0.0, 10.0, 0.0));
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p}, {"folder", tempDir("lila_cage_sheet").generic_string()}, {"resolution", 64}, {"supersampling", 1}});
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {low}}, {"highs", {under, sheet}}});

    const json r = ops::execute("bakeAutoPaintCage", {{"args", {"g"}}, {"resolution", 128}, {"iterations", 0}});
    const double most = mostPush(low, r);
    MESSAGE("most push " << most);
    CHECK(most < 2.0);
}

namespace
{
/** A lumpy high sphere of about 2 x axis x height triangles, crossing in and out of a radius-75 low sphere, as a sculpt over its blockout. */
geo::Mesh lumpySphere(int axis, int height)
{
    geo::SphereOptions so;
    so.radius = 75.0;
    so.subdivAxis = axis;
    so.subdivHeight = height;
    geo::Mesh m = geo::makeSphere(so);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            const glm::dvec3 q = m.position(p);
            const double bumps = 0.08 * std::sin(q.x * 0.2) * std::cos(q.z * 0.17) + 0.03 * std::sin(q.y * 0.9) * std::sin(q.x * 0.7);
            m.setPosition(p, q * (1.0 + bumps));
        }
    return m;
}

/** A sphere low crossing in and out of a lumpy high, with a lid low lying over it: refining passes and clearance both run. */
std::string lumpyGroup(int lowAxis, int highAxis, const std::string& folder)
{
    geo::SphereOptions lo;
    lo.radius = 75.0;
    lo.subdivAxis = lowAxis;
    lo.subdivHeight = lowAxis / 2;
    const std::string low = addMesh("ball_low", geo::makeSphere(lo));
    geo::PlaneOptions lidO;
    lidO.width = lidO.depth = 60.0;
    lidO.subdivX = lidO.subdivZ = 20;
    geo::Mesh lidMesh = geo::makePlane(lidO);
    for (geo::Index q = 0; q < lidMesh.pointCapacity(); ++q)
        if (lidMesh.pointValid(q))
            lidMesh.setPosition(q, lidMesh.position(q) + glm::dvec3(0.0, 88.0, 0.0));
    const std::string lid = addMesh("lid_low", std::move(lidMesh));
    const std::string high = addMesh("ball_high", lumpySphere(highAxis, highAxis / 2));
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p}, {"folder", tempDir(folder).generic_string()}, {"resolution", 64}, {"supersampling", 1}});
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {low, lid}}, {"highs", {high}}, {"frontal", 3.0}, {"rear", 3.0}});
    return "g";
}
} // namespace

TEST_CASE("A soup BVH traces like the mesh BVH it replaces")
{
    const geo::Mesh m = lumpySphere(48, 24);
    const geo::Triangulation tris = geo::triangulate(m);
    std::vector<glm::vec3> soup;
    for (geo::Index c : tris.corners)
        soup.emplace_back(m.position(m.cornerPoint(c)));
    const geo::MeshBvh ref(m);
    geo::MeshBvh fast;
    fast.buildSoup(soup);
    REQUIRE(fast.triangleCount() == ref.triangleCount());
    int hits = 0;
    for (int i = 0; i < 2000; ++i)
    {
        const double a = i * 2.399963, z = 1.0 - 2.0 * (i + 0.5) / 2000.0;
        const glm::dvec3 dir(std::sqrt(1.0 - z * z) * std::cos(a), z, std::sqrt(1.0 - z * z) * std::sin(a));
        const glm::dvec3 origin = glm::dvec3(3.0, -2.0, 1.0) + dir * 200.0;
        geo::BvhHit h0, h1;
        const bool r0 = ref.raycast(origin, -dir, h0), r1 = fast.raycast(origin, -dir, h1);
        REQUIRE(r0 == r1);
        if (!r0)
            continue;
        ++hits;
        // The soup holds floats where the mesh holds doubles.
        CHECK(h1.t == doctest::Approx(h0.t).epsilon(1e-6));
        CHECK(h1.triangle == h0.triangle);
        CHECK(h1.frontFacing == h0.frontFacing);
    }
    CHECK(hits == 2000);
}

TEST_CASE("bakeAutoPaintCage: the parallel estimate paints the reference cage")
{
    newScene();
    const std::string grp = lumpyGroup(24, 96, "lila_cage_reference");
    const json r = ops::execute("bakeAutoPaintCage", {{"args", {grp}}, {"resolution", 128}});
    auto summary = [](const std::string& low) {
        const auto v = baseMesh(low)->pointAttrs().get<float>(bake::kOffsetAttr);
        double sum = 0.0, top = 0.0;
        for (float x : v)
        {
            sum += x;
            top = std::max(top, double(x));
        }
        return std::pair{sum, top};
    };
    const auto [ballSum, ballMax] = summary("ball_low");
    const auto [lidSum, lidMax] = summary("lid_low");
    MESSAGE(std::setprecision(9) << "frontal " << r["frontal"].get<double>() << " rear " << r["rear"].get<double>() << " passes " << r["iterations"]
                                 << " misses " << r["before"]["misses"] << " -> " << r["after"]["misses"] << " ball " << ballSum << " / " << ballMax
                                 << " lid " << lidSum << " / " << lidMax);
    // The serial estimate's numbers on this pair.
    CHECK(r["frontal"].get<double>() == doctest::Approx(7.98385859).epsilon(1e-5));
    CHECK(r["rear"].get<double>() == doctest::Approx(24.350925).epsilon(1e-5));
    CHECK(r["iterations"] == 1);
    CHECK(r["before"]["misses"] == 21739);
    CHECK(r["after"]["misses"] == 824);
    CHECK(ballSum == doctest::Approx(235.683852).epsilon(1e-5));
    CHECK(ballMax == doctest::Approx(1.43853807).epsilon(1e-5));
    CHECK(lidSum == doctest::Approx(18.7478723).epsilon(1e-5));
    CHECK(lidMax == doctest::Approx(0.0425121821).epsilon(1e-5));
    // The whole run reports under one root, closed once it returns.
    CHECK(progress::snapshot().empty());
    UndoStack::get().undo();
    CHECK(!baseMesh("ball_low")->pointAttrs().has(bake::kOffsetAttr));
}

// Timing run on a production-sized pair: lila_bake_tests -tc="*benchmark*" --no-skip
TEST_CASE("bakeAutoPaintCage benchmark" * doctest::skip())
{
    for (const bool modifiers : {false, true})
    {
        newScene();
        const std::string grp = lumpyGroup(modifiers ? 48 : 96, 1024, "lila_cage_bench");
        if (modifiers)
            ops::execute("addModifier", {{"args", {"ball_low"}}, {"kind", "subdivision"}});
        const auto t0 = std::chrono::steady_clock::now();
        const json r = ops::execute("bakeAutoPaintCage", {{"args", {grp}}});
        const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE(std::string(modifiers ? "subdivided low" : "plain low") << ": " << total << " s, miss rate " << r["before"]["missRate"].get<double>() * 100.0
                                                                       << "% -> " << r["after"]["missRate"].get<double>() * 100.0 << "%, passes "
                                                                       << r["iterations"] << ", " << r["timings"].dump());
    }
}

TEST_CASE("cage previews bake in memory off the main thread and write nothing")
{
    newScene();
    geo::SphereOptions so;
    so.radius = 70.0;
    const std::string low = addMesh("ball_low", geo::makeCube());
    const std::string high = addMesh("ball_high", geo::makeSphere(so));
    const fs::path folder = tempDir("lila_cage_preview");
    const std::string grp = pairGroup(low, high, folder);
    const scene::NodeId gid = graph().find(grp)->id();

    int heard = 0;
    const int id = bake::addCagePreviewListener([&](const std::shared_ptr<const bake::CagePreview>&) { ++heard; });
    bake::CagePreviewOptions o;
    o.resolution = 64;
    o.backend = "cpu";
    bake::requestCagePreview(graph(), gid, o);
    // A second request supersedes the first.
    std::vector<float> big(baseMesh(low)->pointCapacity(), 5.0f);
    o.offsets[low] = big;
    bake::requestCagePreview(graph(), gid, o);
    for (int i = 0; i < 600 && bake::cagePreviewBusy(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    mainq::drain();
    bake::removeCagePreviewListener(id);
    CHECK(heard >= 1);
    const auto pv = bake::latestCagePreview(gid);
    REQUIRE(pv);
    CHECK(pv->generation >= 2);
    const bake::CagePreviewTexture* n = pv->find("ball_low", "normal");
    const bake::CagePreviewTexture* e = pv->find("ball_low", "error");
    REQUIRE(n);
    REQUIRE(e);
    CHECK(n->width == 64);
    CHECK(n->value.size() == 64 * 64);
    CHECK(pv->stats.samples > 0);
    CHECK(fs::is_empty(folder));
}
