#include "TestUtil.h"

#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
/** The face of a cube-like mesh whose normal is closest to dir. */
Index faceFacing(const Mesh& m, const glm::dvec3& dir)
{
    Index best = kInvalid;
    double bestDot = -2.0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            const double d = glm::dot(m.faceNormal(f), dir);
            if (d > bestDot)
            {
                bestDot = d;
                best = f;
            }
        }
    return best;
}

Index opsEdgeBetween(const Mesh& m, const glm::dvec3& a, const glm::dvec3& b)
{
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            const glm::dvec3 p = m.position(m.edgePoint(e, 0)), q = m.position(m.edgePoint(e, 1));
            if ((glm::distance(p, a) < 1e-6 && glm::distance(q, b) < 1e-6)
                || (glm::distance(p, b) < 1e-6 && glm::distance(q, a) < 1e-6))
                return e;
        }
    return kInvalid;
}

void checkClosed(const Mesh& m)
{
    CHECK(isValid(m));
    CHECK(boundaryEdgeCount(m) == 0);
    CHECK(euler(m) == 2);
}

bool isSymmetric(const Mesh& m, const SymmetryPlane& plane = {})
{
    const SymmetryMap map = SymmetryMap::build(m, plane, 1e-4);
    if (map.unmatchedCount())
        MESSAGE(map.unmatchedCount() << " points without a mirror");
    return map.unmatchedCount() == 0;
}

Index sumHard(const Mesh& m)
{
    std::span<const bool> hard = m.edgeAttrs().get<bool>(attr::kHard);
    Index n = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        n += m.edgeValid(e) && !hard.empty() && hard[e];
    return n;
}
} // namespace

TEST_CASE("ops: delete")
{
    Mesh cube = makeCube();
    deleteFaces(cube, std::vector<Index>{faceFacing(cube, {0, 1, 0})});
    CHECK(isValid(cube));
    CHECK(cube.numFaces() == 5);
    CHECK(boundaryEdgeCount(cube) == 4);

    Mesh c2 = makeCube();
    deleteEdges(c2, std::vector<Index>{0});
    CHECK(isValid(c2));
    CHECK(c2.numFaces() == 4);
    CHECK(c2.numEdges() == 11);

    Mesh c3 = makeCube();
    deletePoints(c3, std::vector<Index>{0});
    CHECK(isValid(c3));
    CHECK(c3.numFaces() == 3);
    CHECK(c3.numPoints() == 7);
}

TEST_CASE("ops: extrude")
{
    Mesh cube = makeCube();
    const Index top = faceFacing(cube, {0, 1, 0});
    ExtrudeOptions eo;
    eo.offset = 50.0;
    ExtrudeResult r = extrudeFaces(cube, std::vector<Index>{top}, eo);
    checkClosed(cube);
    CHECK(r.sides.size() == 4);
    CHECK(cube.numFaces() == 10);
    CHECK(cube.bounds().max.y == doctest::Approx(100.0));
    CHECK(signedVolume(cube) == doctest::Approx(1.5e6));
    // Cube edges were hard, so are the new ones around the extrusion.
    CHECK(sumHard(cube) == 20);

    // Region of two neighbouring faces.
    Mesh grid = makeCube({100, 100, 100, 2, 1, 1});
    std::vector<Index> both;
    for (Index f = 0; f < grid.faceCapacity(); ++f)
        if (grid.faceNormal(f).y > 0.9)
            both.push_back(f);
    REQUIRE(both.size() == 2);
    r = extrudeFaces(grid, both, eo);
    checkClosed(grid);
    CHECK(r.sides.size() == 6);
    CHECK(signedVolume(grid) == doctest::Approx(1.5e6));

    Mesh ind = makeCube({100, 100, 100, 2, 1, 1});
    eo.individual = true;
    r = extrudeFaces(ind, both, eo);
    checkClosed(ind);
    CHECK(r.sides.size() == 8);
}

TEST_CASE("ops: extrude distances are world distances under a scaled object")
{
    for (const double mirror : {1.0, -1.0})
    {
        Mesh cube = makeCube();
        const Index top = faceFacing(cube, {0, 1, 0});
        ExtrudeOptions eo;
        eo.offset = 10.0;
        eo.inset = 10.0;
        eo.space = glm::dmat3(glm::scale(glm::dmat4(1.0), glm::dvec3(2.0 * mirror, 4.0, 0.5)));
        ExtrudeResult r = extrudeFaces(cube, std::vector<Index>{top}, eo);
        checkClosed(cube);
        REQUIRE(r.faces.size() == 1);
        Bounds b;
        cube.forEachFaceCorner(r.faces[0], [&](Index c) { b.extend(cube.position(cube.cornerPoint(c))); });
        CHECK(b.max.x == doctest::Approx(45.0));
        CHECK(b.max.z == doctest::Approx(30.0));
        CHECK(b.min.y == doctest::Approx(52.5));
    }
}

TEST_CASE("ops: extrude copy")
{
    const Mesh cube = makeCube();
    const Index top = faceFacing(cube, {0, 1, 0});
    ExtrudeOptions eo;
    eo.offset = 50.0;
    ExtrudeCopy cap = extrudeCopy(cube, std::vector<Index>{top}, eo);
    CHECK(cube.numFaces() == 6);
    CHECK(cap.mesh.numFaces() == 1);
    REQUIRE(cap.cap.size() == 1);
    CHECK(cap.mesh.faceCentroid(cap.cap[0]).y == doctest::Approx(100.0));
    CHECK(cap.mesh.faceNormal(cap.cap[0]).y == doctest::Approx(1.0));

    ExtrudeCopy walls = extrudeCopy(cube, std::vector<Index>{top}, eo, true);
    CHECK(walls.mesh.numFaces() == 5);
    CHECK(walls.cap.size() == 1);
    CHECK(isValid(walls.mesh));
    // Open where the walls met the cube's top.
    CHECK(boundaryEdgeCount(walls.mesh) == 4);
    CHECK(walls.mesh.bounds().min.y == doctest::Approx(50.0));
    CHECK(walls.mesh.bounds().max.y == doctest::Approx(100.0));
}

TEST_CASE("ops: inset")
{
    Mesh cube = makeCube();
    const Index top = faceFacing(cube, {0, 1, 0});
    InsetOptions io;
    io.thickness = 10.0;
    ExtrudeResult r = insetFaces(cube, std::vector<Index>{top}, io);
    checkClosed(cube);
    CHECK(cube.numFaces() == 10);
    CHECK(cube.faceArea(top) == doctest::Approx(80.0 * 80.0));
    CHECK(signedVolume(cube) == doctest::Approx(1e6));

    Mesh c2 = makeCube();
    io.depth = -10.0;
    insetFaces(c2, std::vector<Index>{faceFacing(c2, {0, 1, 0})}, io);
    checkClosed(c2);
    // The rim slopes down to the inner face: a frustum comes out.
    CHECK(signedVolume(c2) == doctest::Approx(1e6 - 10.0 / 3.0 * (10000.0 + 6400.0 + 8000.0)));
}

TEST_CASE("ops: inset clamps on faces narrower than twice the thickness")
{
    // The +x side is 2.5 wide in z; split in two along y for the region case.
    for (bool individual : {true, false})
    {
        CAPTURE(individual);
        Mesh m = makeCube({.width = 100.0, .height = 100.0, .depth = 2.5, .subdivY = 2});
        std::vector<Index> sel;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f) && m.faceNormal(f).x > 0.5)
                sel.push_back(f);
        REQUIRE(sel.size() == 2);
        const double area = m.faceArea(sel[0]) + m.faceArea(sel[1]);
        const ExtrudeResult r = insetFaces(m, sel, {.thickness = 7.0, .individual = individual});
        checkClosed(m);
        // The inner border closes on the medial line, so no inner face is left.
        CHECK(r.faces.empty());
        CHECK(r.sides.size() == (individual ? 8 : 6));
        double worstFold = 1.0, rimArea = 0.0;
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e) && m.isManifoldEdge(e))
            {
                const Index c = m.edgeCorner(e);
                worstFold = std::min(worstFold, glm::dot(m.faceNormal(m.cornerFace(c)), m.faceNormal(m.cornerFace(m.cornerRadial(c)))));
            }
        CHECK(worstFold > -0.1);
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
            {
                double longest = 0.0;
                m.forEachFaceCorner(f, [&](Index c) { longest = std::max(longest, m.edgeLength(m.cornerEdge(c))); });
                CHECK(m.faceArea(f) > 0.01 * longest);
            }
        for (Index f : r.sides)
        {
            CHECK(m.faceNormal(f).x == doctest::Approx(1.0));
            rimArea += m.faceArea(f);
        }
        for (Index f : r.faces)
        {
            CHECK(m.faceNormal(f).x == doctest::Approx(1.0));
            rimArea += m.faceArea(f);
        }
        // Inner faces and rim tile the original side exactly, with no overlap.
        CHECK(rimArea == doctest::Approx(area));
        CHECK(signedVolume(m) == doctest::Approx(100.0 * 100.0 * 2.5));
    }
}

TEST_CASE("ops: inset clamp leaves insets with room unchanged")
{
    for (bool individual : {true, false})
    {
        CAPTURE(individual);
        Mesh a = makeCube({100, 100, 100, 3, 1, 3}), b = a;
        std::vector<Index> sel;
        for (Index f = 0; f < a.faceCapacity(); ++f)
            if (a.faceValid(f) && a.faceNormal(f).y > 0.5)
                sel.push_back(f);
        insetFaces(a, sel, {.thickness = 10.0, .depth = -5.0, .individual = individual});
        insetFaces(b, sel, {.thickness = 10.0, .depth = -5.0, .individual = individual, .clamp = false});
        REQUIRE(a.pointCapacity() == b.pointCapacity());
        for (Index p = 0; p < a.pointCapacity(); ++p)
            if (a.pointValid(p))
                CHECK(glm::distance(a.position(p), b.position(p)) < 1e-12);
    }
}

TEST_CASE("ops: bevel")
{
    Mesh cube = makeCube();
    const Index e = opsEdgeBetween(cube, {50, 50, 50}, {50, 50, -50});
    REQUIRE(e != kInvalid);
    BevelOptions bo;
    bo.offset = 10.0;
    std::vector<Index> made = bevelEdges(cube, std::vector<Index>{e}, bo);
    checkClosed(cube);
    CHECK(made.size() == 1);
    CHECK(cube.numFaces() == 7);
    CHECK(signedVolume(cube) == doctest::Approx(1e6 - 0.5 * 10 * 10 * 100));

    Mesh all = makeCube();
    std::vector<Index> edges;
    for (Index i = 0; i < all.edgeCapacity(); ++i)
        edges.push_back(i);
    made = bevelEdges(all, edges, bo);
    checkClosed(all);
    CHECK(all.numFaces() == 26);
    CHECK(made.size() == 20);
    // At a 30 degree smoothing angle a chamfer is faceted everywhere.
    CHECK(sumHard(all) == all.numEdges());

    Mesh round = makeCube();
    bo.segments = 3;
    made = bevelEdges(round, edges, bo);
    checkClosed(round);
    // Odd segment counts close each corner with three 1 x 2 quad grids around a centre triangle.
    CHECK(round.numFaces() == 6 + 12 * 3 + 8 * 7);
    // Rounded edges stay soft between segments, hard against the flat sides at 30 degrees.
    CHECK(signedVolume(round) < 1e6);
    CHECK(signedVolume(round) > 1e6 - 12 * 0.5 * 10 * 10 * 80 - 8 * 1000);

    for (int seg : {2, 4})
    {
        Mesh even = makeCube();
        bo.segments = seg;
        bevelEdges(even, edges, bo);
        checkClosed(even);
        // Even counts make three quad grids per corner.
        CHECK(even.numFaces() == Index(6 + 12 * seg + 8 * 3 * (seg / 2) * (seg / 2)));
        for (Index f = 0; f < even.faceCapacity(); ++f)
            if (even.faceValid(f))
                CHECK(even.faceSize(f) == 4);
        // Every point of the round corner stays near the sphere of radius 10 around (40, 40, 40).
        for (Index p = 0; p < even.pointCapacity(); ++p)
        {
            const glm::dvec3 q = even.position(p);
            if (even.pointValid(p) && q.x > 40.0 && q.y > 40.0 && q.z > 40.0)
                CHECK(glm::distance(q, glm::dvec3(40.0)) == doctest::Approx(10.0).epsilon(0.1));
        }
    }

    Mesh single = makeCube();
    bo.segments = 4;
    made = bevelEdges(single, std::vector<Index>{opsEdgeBetween(single, {50, 50, 50}, {50, 50, -50})}, bo);
    checkClosed(single);
    CHECK(made.size() == 4);
    CHECK(single.numFaces() == 6 + 4);
    // A round profile: every point of the strip lies on a cylinder of radius offset around x=y=40.
    for (Index f : made)
        single.forEachFaceCorner(f, [&](Index c) {
            const glm::dvec3 p = single.position(single.cornerPoint(c));
            CHECK(std::hypot(p.x - 40.0, p.y - 40.0) == doctest::Approx(10.0));
        });
    // Soft between segments and against the sides (22.5 degrees), hard against the end caps.
    CHECK(sumHard(single) == 11 + 2 * 4);

    // Two edges meeting at a corner.
    Mesh two = makeCube();
    std::vector<Index> pair = {opsEdgeBetween(two, {50, 50, 50}, {50, 50, -50}), opsEdgeBetween(two, {50, 50, 50}, {-50, 50, 50})};
    bo.segments = 2;
    made = bevelEdges(two, pair, bo);
    checkClosed(two);

    // Open mesh, edge reaching the border.
    Mesh plane = makePlane({100, 100, 2, 2});
    const Index mid = opsEdgeBetween(plane, {0, 0, 50}, {0, 0, 0});
    REQUIRE(mid != kInvalid);
    bo.segments = 1;
    made = bevelEdges(plane, std::vector<Index>{mid}, bo);
    CHECK(isValid(plane));
}

TEST_CASE("ops: edge loops")
{
    Mesh cube = makeCube();
    std::vector<Index> loop = insertEdgeLoop(cube, 0);
    checkClosed(cube);
    CHECK(loop.size() == 4);
    CHECK(cube.numFaces() == 10);

    Mesh c3 = makeCube();
    EdgeLoopOptions lo;
    lo.count = 3;
    loop = insertEdgeLoop(c3, 0, lo);
    checkClosed(c3);
    CHECK(loop.size() == 12);
    CHECK(c3.numFaces() == 18);

    Mesh plane = makePlane({100, 100, 3, 1});
    lo.count = 1;
    lo.fraction = 0.25;
    loop = insertEdgeLoop(plane, opsEdgeBetween(plane, {-50, 0, 50}, {-50, 0, -50}), lo);
    CHECK(isValid(plane));
    CHECK(loop.size() == 3);
    CHECK(plane.numFaces() == 6);
}

TEST_CASE("ops: merge, collapse, dissolve")
{
    Mesh a = makePlane({100, 100, 1, 1});
    Mesh b = makePlane({100, 100, 1, 1});
    for (glm::dvec3& p : b.positions())
        p.x += 100.0;
    a.append(b);
    CHECK(a.numPoints() == 8);
    std::vector<Index> survivors = mergeByDistance(a, {}, {0.01});
    CHECK(isValid(a));
    CHECK(a.numPoints() == 6);
    CHECK(a.numFaces() == 2);
    CHECK(boundaryEdgeCount(a) == 6);

    Mesh cube = makeCube();
    std::vector<Index> pts = collapseEdges(cube, std::vector<Index>{0});
    CHECK(pts.size() == 1);
    checkClosed(cube);
    CHECK(cube.numPoints() == 7);

    Mesh m2 = makeCube();
    CHECK(mergeToCenter(m2, std::vector<Index>{0, 1}) != kInvalid);
    CHECK(isValid(m2));
    Mesh m3 = makeCube();
    CHECK(mergeToTarget(m3, std::vector<Index>{1}, 0) == 0);
    CHECK(isValid(m3));

    Mesh grid = makePlane({100, 100, 2, 1});
    std::vector<Index> faces = dissolveEdges(grid, std::vector<Index>{opsEdgeBetween(grid, {0, 0, 50}, {0, 0, -50})});
    CHECK(isValid(grid));
    CHECK(grid.numFaces() == 1);
    CHECK(grid.numPoints() == 4);

    Mesh g2 = makePlane({100, 100, 2, 2});
    Index center = kInvalid;
    for (Index p = 0; p < g2.pointCapacity(); ++p)
        if (glm::length(g2.position(p)) < 1e-9)
            center = p;
    faces = dissolvePoints(g2, std::vector<Index>{center});
    CHECK(isValid(g2));
    REQUIRE(faces.size() == 1);
    CHECK(g2.faceSize(faces[0]) == 8);
    CHECK(g2.faceNormal(faces[0]).y == doctest::Approx(1.0));
}

TEST_CASE("ops: bridge")
{
    Mesh tube = makeCylinder({50.0, 300.0, 12, 3, 0, false});
    std::vector<Index> middle;
    for (Index f = 0; f < tube.faceCapacity(); ++f)
        if (std::abs(tube.faceCentroid(f).y) < 1.0)
            middle.push_back(f);
    REQUIRE(middle.size() == 12);
    deleteFaces(tube, middle);
    CHECK(boundaryEdgeCount(tube) == 48);
    std::vector<Index> inner;
    for (Index e = 0; e < tube.edgeCapacity(); ++e)
        if (tube.edgeValid(e) && tube.isBoundaryEdge(e)
            && std::abs(tube.position(tube.edgePoint(e, 0)).y) < 60.0)
            inner.push_back(e);
    REQUIRE(inner.size() == 24);
    BridgeOptions bo;
    bo.divisions = 1;
    std::vector<Index> made = bridgeEdgeLoops(tube, inner, bo);
    CHECK(made.size() == 24);
    CHECK(isValid(tube));
    CHECK(boundaryEdgeCount(tube) == 24);
}

namespace
{
/** A 6 x 5 sheet of 100 cm quads with a 4 x 1 slot cut at each z in slotRows. */
Mesh slottedSheet(std::initializer_list<double> slotRows)
{
    Mesh sheet = makePlane({600, 500, 6, 5});
    std::vector<Index> cut;
    for (Index f = 0; f < sheet.faceCapacity(); ++f)
        for (double z : slotRows)
            if (sheet.faceValid(f) && std::abs(sheet.faceCentroid(f).x) < 200 && std::abs(sheet.faceCentroid(f).z - z) < 1)
                cut.push_back(f);
    deleteFaces(sheet, cut);
    return sheet;
}

/** Border edges inside the sheet's outer rim, without those passing through skip. */
std::vector<Index> slotEdges(const Mesh& m, std::optional<glm::dvec3> skip = std::nullopt)
{
    std::vector<Index> out;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
        {
            const glm::dvec3 mid = (m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1))) * 0.5;
            if (std::abs(mid.x) < 299 && std::abs(mid.z) < 249 && !(skip && glm::distance(mid, *skip) < 1))
                out.push_back(e);
        }
    return out;
}
} // namespace

TEST_CASE("ops: bridge works out the sides of a slot")
{
    // The whole border round the slot: its long sides bridge, the ends that join them are left.
    Mesh sheet = slottedSheet({-100});
    REQUIRE(slotEdges(sheet).size() == 10);
    CHECK(planBridges(sheet, slotEdges(sheet)).size() == 1);
    CHECK(bridgeEdges(sheet, slotEdges(sheet)).size() == 4);
    CHECK(isValid(sheet));
    CHECK(boundaryEdgeCount(sheet) == 22);

    // A U: one end left unselected.
    Mesh u = slottedSheet({-100});
    const std::vector<Index> three = slotEdges(u, glm::dvec3(200, 0, -100));
    REQUIRE(three.size() == 9);
    CHECK(bridgeEdges(u, three).size() == 4);
    CHECK(isValid(u));
    CHECK(boundaryEdgeCount(u) == 22);
}

TEST_CASE("ops: bridge pairs separate selections")
{
    // Two slots in one sheet each bridge across, not into each other.
    Mesh sheet = slottedSheet({-100, 100});
    REQUIRE(slotEdges(sheet).size() == 20);
    CHECK(bridgeEdges(sheet, slotEdges(sheet)).size() == 8);
    CHECK(isValid(sheet));
    CHECK(boundaryEdgeCount(sheet) == 22);

    // Two gaps in a tube: each pair of facing loops bridges with its nearest partner.
    Mesh tube = makeCylinder({50.0, 500.0, 12, 5, 0, false});
    std::vector<Index> gaps;
    for (Index f = 0; f < tube.faceCapacity(); ++f)
        if (std::abs(std::abs(tube.faceCentroid(f).y) - 100.0) < 1.0)
            gaps.push_back(f);
    REQUIRE(gaps.size() == 24);
    deleteFaces(tube, gaps);
    std::vector<Index> loops;
    for (Index e = 0; e < tube.edgeCapacity(); ++e)
        if (tube.edgeValid(e) && tube.isBoundaryEdge(e) && std::abs(tube.position(tube.edgePoint(e, 0)).y) < 240.0)
            loops.push_back(e);
    REQUIRE(loops.size() == 48);
    CHECK(planBridges(tube, loops).size() == 2);
    CHECK(bridgeEdges(tube, loops).size() == 24);
    CHECK(isValid(tube));
    CHECK(boundaryEdgeCount(tube) == 24);
}

TEST_CASE("ops: poke, spin, fill, flip, conform, triangulate, quadrangulate")
{
    Mesh cube = makeCube();
    std::vector<Index> made = pokeFaces(cube, std::vector<Index>{faceFacing(cube, {0, 1, 0})});
    CHECK(made.size() == 4);
    checkClosed(cube);

    Mesh grid = makePlane({100, 100, 2, 2});
    const Index mid = opsEdgeBetween(grid, {0, 0, 0}, {0, 0, 50});
    std::vector<Index> spun = spinEdges(grid, std::vector<Index>{mid});
    REQUIRE(spun.size() == 1);
    CHECK(isValid(grid));
    CHECK(grid.numFaces() == 4);

    Mesh open = makeCube();
    deleteFaces(open, std::vector<Index>{2});
    made = fillHoles(open, {});
    CHECK(made.size() == 1);
    checkClosed(open);
    CHECK(signedVolume(open) == doctest::Approx(1e6));

    Mesh sphere = makeSphere();
    flipNormals(sphere, std::vector<Index>{3, 50, 51, 52, 200});
    CHECK_FALSE(sphere.validate(kValidateWinding).empty());
    conformNormals(sphere);
    checkClosed(sphere);
    CHECK(signedVolume(sphere) > 0.0);
    std::vector<Index> all;
    for (Index f = 0; f < sphere.faceCapacity(); ++f)
        all.push_back(f);
    flipNormals(sphere, all);
    conformNormals(sphere);
    CHECK(signedVolume(sphere) > 0.0);

    Mesh tri = makeCube();
    all.clear();
    for (Index f = 0; f < tri.faceCapacity(); ++f)
        all.push_back(f);
    made = triangulateFaces(tri, all);
    CHECK(made.size() == 12);
    checkClosed(tri);
    CHECK(sumHard(tri) == 12);
    made = quadrangulateFaces(tri, made);
    CHECK(made.size() == 6);
    checkClosed(tri);
    tri.compact();
    CHECK(uvsDontOverlap(tri));
}

TEST_CASE("ops: grid fill")
{
    auto fanCylinder = [](int sides) {
        CylinderOptions o;
        o.subdivAxis = sides;
        o.capFill = CapFill::Fan;
        return makeCylinder(o);
    };
    auto poles = [](const Mesh& m, size_t valence) {
        std::vector<Index> out;
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && facesOfPoints(m, std::vector<Index>{p}).size() == valence)
                out.push_back(p);
        return out;
    };
    auto countSides = [](const Mesh& m, size_t sides) {
        int n = 0;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            n += m.faceValid(f) && m.faceSize(f) == sides;
        return n;
    };

    SUBCASE("even fans become a ring and a grid of quads")
    {
        Mesh m = fanCylinder(40);
        const double volume = signedVolume(m);
        const std::vector<Index> centres = poles(m, 40);
        REQUIRE(centres.size() == 2);
        const std::vector<Index> made = gridFillFaces(m, facesOfPoints(m, centres));
        CHECK(made.size() == 2 * (40 + 100));
        checkClosed(m);
        CHECK(signedVolume(m) == doctest::Approx(volume));
        CHECK(countSides(m, 4) == m.numFaces());
        m.compact();
        CHECK(uvsDontOverlap(m));
    }

    SUBCASE("odd fans keep one triangle")
    {
        Mesh m = fanCylinder(21);
        const std::vector<Index> made = gridFillFaces(m, facesOfPoints(m, poles(m, 21)));
        CHECK(made.size() == 2 * (22 + 30));
        checkClosed(m);
        CHECK(countSides(m, 3) == 2);
    }

    SUBCASE("holes and a grid right to the border")
    {
        Mesh m = fanCylinder(16);
        const double volume = signedVolume(m);
        deleteFaces(m, facesOfPoints(m, poles(m, 16)));
        GridFillOptions o;
        o.gridSize = 1.0;
        o.rotate = 3;
        const std::vector<Index> made = gridFillHoles(m, {}, o);
        CHECK(made.size() == 2 * 16);
        checkClosed(m);
        CHECK(signedVolume(m) == doctest::Approx(volume));
        CHECK(countSides(m, 4) == m.numFaces());
    }

    SUBCASE("rectangular holes put the grid's corners on theirs, mirrored holes alike")
    {
        PlaneOptions po;
        po.width = po.depth = 120.0;
        po.subdivX = po.subdivZ = 12;
        Mesh m = makePlane(po);
        std::vector<Index> hole;
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            glm::dvec3 c{0.0};
            m.forEachFaceCorner(f, [&](Index fc) { c += m.position(m.cornerPoint(fc)) / 4.0; });
            if (std::abs(c.x) > 10 && std::abs(c.x) < 30 && c.z > -20 && c.z < 10)
                hole.push_back(f);
        }
        REQUIRE(hole.size() == 12);
        GridFillOptions o;
        o.gridSize = 1.0;
        std::vector<Index> made;
        SUBCASE("faces") { made = gridFillFaces(m, hole, o); }
        SUBCASE("holes")
        {
            deleteFaces(m, hole);
            std::vector<Index> inner;
            for (Index e = 0; e < m.edgeCapacity(); ++e)
                if (m.edgeValid(e) && std::abs(m.position(m.edgePoint(e, 0)).x) < 50 && std::abs(m.position(m.edgePoint(e, 0)).z) < 50)
                    inner.push_back(e);
            made = gridFillHoles(m, inner, o);
        }
        CHECK(made.size() == 12);
        for (Index f : made)
        {
            std::vector<glm::dvec3> p;
            m.forEachFaceCorner(f, [&](Index fc) { p.push_back(m.position(m.cornerPoint(fc))); });
            REQUIRE(p.size() == 4);
            CHECK(glm::length(glm::cross(p[2] - p[0], p[3] - p[1])) / 2 == doctest::Approx(100.0));
        }
    }

    SUBCASE("patches that are not discs are left alone")
    {
        Mesh m = fanCylinder(12);
        std::vector<Index> sides;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f) && m.faceSize(f) == 4)
                sides.push_back(f);
        CHECK(gridFillFaces(m, sides).empty());
        checkClosed(m);
    }
}

TEST_CASE("ops: subdivide")
{
    Mesh cube = makeCube();
    Mesh s = subdivide(cube);
    checkClosed(s);
    CHECK(s.numFaces() == 24);
    CHECK(s.numPoints() == 26);
    CHECK(signedVolume(s) < 0.9e6);
    CHECK(uvsDontOverlap(s));
    Mesh s2 = subdivide(cube, {2});
    checkClosed(s2);
    CHECK(s2.numFaces() == 96);

    std::span<float> crease = cube.edgeAttrs().getOrAdd<float>(attr::kCrease);
    std::fill(crease.begin(), crease.end(), 2.0f);
    Mesh sharp = subdivide(cube, {2});
    CHECK(sharp.bounds().size().x == doctest::Approx(100.0));
    CHECK(signedVolume(sharp) == doctest::Approx(1e6));

    Mesh plane = makePlane();
    Mesh sp = subdivide(plane);
    CHECK(isValid(sp));
    CHECK(sp.bounds().size().x == doctest::Approx(100.0));
}

TEST_CASE("ops: subdivide map")
{
    Mesh cube = makeCube();
    SubdivideMap map;
    Mesh s = subdivide(cube, {2}, &map);
    REQUIRE(map.points.size() == cube.pointCapacity());
    REQUIRE(map.edges.size() == s.edgeCapacity());
    REQUIRE(map.faces.size() == s.faceCapacity());
    // Each cage edge becomes a chain of 4 pieces from one end's image to the other's.
    std::vector<std::map<Index, int>> ends(cube.edgeCapacity());
    for (Index e = 0; e < s.edgeCapacity(); ++e)
        if (s.edgeValid(e) && map.edges[e] != kInvalid)
            for (int k = 0; k < 2; ++k)
                ++ends[map.edges[e]][s.edgePoint(e, k)];
    for (Index e = 0; e < cube.edgeCapacity(); ++e)
        if (cube.edgeValid(e))
        {
            // 4 pieces through 5 points; the two used once are the images of the edge's ends.
            REQUIRE(ends[e].size() == 5);
            std::set<Index> tips;
            for (auto [p, uses] : ends[e])
                if (uses == 1)
                    tips.insert(p);
            CHECK(tips == std::set<Index>{map.points[cube.edgePoint(e, 0)], map.points[cube.edgePoint(e, 1)]});
        }
    std::vector<int> parts(cube.faceCapacity(), 0);
    for (Index f = 0; f < s.faceCapacity(); ++f)
        if (s.faceValid(f))
        {
            REQUIRE(map.faces[f] < cube.faceCapacity());
            ++parts[map.faces[f]];
            // Every piece lies on its cage face's side of the cube.
            CHECK(glm::dot(s.faceCentroid(f), cube.faceNormal(map.faces[f])) > 0.0);
        }
    for (Index f = 0; f < cube.faceCapacity(); ++f)
        if (cube.faceValid(f))
        {
            CHECK(parts[f] == 16);
            CHECK(glm::dot(s.position(map.facePoints[f]), cube.faceNormal(f)) > 0.0);
        }
    // Cage points land on the limit-ish corners, pulled in from the cage's.
    for (Index p = 0; p < cube.pointCapacity(); ++p)
        if (cube.pointValid(p))
            CHECK(glm::length(s.position(map.points[p])) < glm::length(cube.position(p)));

    Mesh plain = subdivide(cube, {2});
    CHECK(plain.numFaces() == s.numFaces());
}

TEST_CASE("ops: subdivide hard edges")
{
    Mesh cube = makeCube();
    setHardEdgesByAngle(cube, 30.0);
    updateCornerNormals(cube);
    REQUIRE(cube.edgeAttrs().has(attr::kHard));
    Mesh soft = subdivide(cube);
    CHECK(!soft.edgeAttrs().has(attr::kHard));
    CHECK(!soft.cornerAttrs().has(attr::kNormal));
    Mesh hard = subdivide(cube, {.levels = 1, .keepHardEdges = true});
    CHECK(hard.edgeAttrs().has(attr::kHard));
}

TEST_CASE("ops: mirror, separate, combine")
{
    Mesh sphere = makeSphere({50.0, 16, 8});
    const Index full = sphere.numPoints();
    std::vector<Index> left;
    for (Index f = 0; f < sphere.faceCapacity(); ++f)
        if (sphere.faceCentroid(f).x < 0.0)
            left.push_back(f);
    deleteFaces(sphere, left);
    std::vector<Index> made = mirrorMesh(sphere);
    sphere.compact();
    checkClosed(sphere);
    CHECK(sphere.numPoints() == full);
    CHECK(signedVolume(sphere) > 0.0);
    CHECK(isSymmetric(sphere));

    Mesh two = makeCube();
    Mesh other = makeTorus();
    two.append(other);
    std::vector<Mesh> parts = separate(two);
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].numFaces() == 6);
    CHECK(parts[1].numFaces() == other.numFaces());
    CHECK(isValid(parts[1]));
    const Mesh* list[2] = {&parts[0], &parts[1]};
    Mesh back = combine(list);
    CHECK(back.numFaces() == two.numFaces());
}

TEST_CASE("ops: mirror directions cut at the plane")
{
    auto shiftedCube = [] {
        Mesh m = makeCube();
        for (glm::dvec3& p : m.positions())
            p.x += 20.0;
        return m;
    };
    auto run = [&](MirrorDirection dir) {
        Mesh m = shiftedCube();
        MirrorOptions o;
        o.direction = dir;
        mirrorMesh(m, o);
        m.compact();
        return m;
    };

    Mesh pos = run(MirrorDirection::Positive);
    checkClosed(pos);
    CHECK(pos.numPoints() == 12);
    CHECK(pos.numFaces() == 10);
    CHECK(isSymmetric(pos));
    CHECK(pos.bounds().min.x == doctest::Approx(-70.0));
    CHECK(pos.bounds().max.x == doctest::Approx(70.0));
    CHECK(signedVolume(pos) == doctest::Approx(1.4e6));

    Mesh neg = run(MirrorDirection::Negative);
    checkClosed(neg);
    CHECK(neg.numPoints() == 12);
    CHECK(neg.numFaces() == 10);
    CHECK(isSymmetric(neg));
    CHECK(neg.bounds().max.x == doctest::Approx(30.0));
    CHECK(signedVolume(neg) == doctest::Approx(6e5));

    Mesh both = run(MirrorDirection::Both);
    CHECK(both.numPoints() == 16);
    CHECK(both.numFaces() == 12);
    CHECK(both.bounds().min.x == doctest::Approx(-70.0));

    // Other axes, and a plane given in another space: world x = 0 is object x = -20 here.
    Mesh y = shiftedCube();
    MirrorOptions oy;
    oy.plane = axisPlane(1);
    oy.direction = MirrorDirection::Negative;
    mirrorMesh(y, oy);
    y.compact();
    checkClosed(y);
    CHECK(y.numFaces() == 10);
    CHECK(y.bounds().max.y == doctest::Approx(50.0));
    Mesh w = makeCube();
    MirrorOptions ow;
    ow.space = glm::translate(glm::dmat4(1.0), glm::dvec3(20.0, 0.0, 0.0));
    ow.direction = MirrorDirection::Positive;
    mirrorMesh(w, ow);
    w.compact();
    checkClosed(w);
    CHECK(w.bounds().min.x == doctest::Approx(-90.0));
    CHECK(w.bounds().max.x == doctest::Approx(50.0));
    CHECK(isSymmetric(w, {glm::dvec3(1.0, 0.0, 0.0), -20.0}));
}

TEST_CASE("ops: mirror cut splits crossing faces and interpolates uvs")
{
    Mesh quad = makePlane();
    for (glm::dvec3& p : quad.positions())
        p.x += 10.0;
    MirrorOptions o;
    mirrorMesh(quad, o);
    quad.compact();
    CHECK(isValid(quad));
    CHECK(quad.numFaces() == 2);
    CHECK(quad.numPoints() == 8);
    o.direction = MirrorDirection::Positive;
    quad = makePlane();
    for (glm::dvec3& p : quad.positions())
        p.x += 10.0;
    mirrorMesh(quad, o);
    quad.compact();
    CHECK(isValid(quad));
    CHECK(quad.numFaces() == 2);
    CHECK(quad.numPoints() == 6);
    for (Index f = 0; f < quad.faceCapacity(); ++f)
        CHECK(quad.faceSize(f) == 4);
    std::span<const glm::vec2> uv = quad.cornerAttrs().get<glm::vec2>(attr::uvName(0));
    REQUIRE(!uv.empty());
    // The cut runs 40% of the way across the 100 wide plane.
    for (Index c = 0; c < quad.cornerCapacity(); ++c)
        if (quad.cornerValid(c) && std::abs(quad.position(quad.cornerPoint(c)).x) < 1e-9)
            CHECK(std::min(std::abs(uv[c].x - 0.4f), std::abs(uv[c].x - 0.6f)) < 1e-5f);

    // A C shape whose two prongs reach across the plane: one face crossing it four times.
    auto cShape = [] {
        Mesh m;
        const double xy[8][2] = {{-5, -10}, {10, -10}, {10, -6}, {-2, -6}, {-2, 6}, {10, 6}, {10, 10}, {-5, 10}};
        std::vector<Index> pts;
        for (const auto& p : xy)
            pts.push_back(m.addPoint({p[0], p[1], 0.0}));
        m.addFace(pts);
        return m;
    };
    auto area = [](const Mesh& m) {
        double a = 0.0;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                a += m.faceArea(f);
        return a;
    };
    Mesh prongs = cShape();
    mirrorMesh(prongs, o);
    prongs.compact();
    CHECK(isValid(prongs));
    CHECK(prongs.numFaces() == 4);
    CHECK(prongs.numPoints() == 12);
    CHECK(area(prongs) == doctest::Approx(160.0));
    CHECK(isSymmetric(prongs));

    Mesh back = cShape();
    o.direction = MirrorDirection::Negative;
    mirrorMesh(back, o);
    back.compact();
    CHECK(isValid(back));
    CHECK(back.numFaces() == 2);
    CHECK(area(back) == doctest::Approx(152.0));
    CHECK(isSymmetric(back));
}

TEST_CASE("symmetry: maps")
{
    Mesh sphere = makeSphere({50.0, 16, 8});
    SymmetryMap map = SymmetryMap::build(sphere);
    CHECK(map.unmatchedCount() == 0);
    Index onPlane = 0;
    for (Index p = 0; p < sphere.pointCapacity(); ++p)
        onPlane += map.onPlane(p);
    CHECK(onPlane == 2 * 7 + 2);

    // Topological match from an edge on the plane.
    Index seed = kInvalid;
    for (Index e = 0; e < sphere.edgeCapacity() && seed == kInvalid; ++e)
        if (map.onPlane(sphere.edgePoint(e, 0)) && map.onPlane(sphere.edgePoint(e, 1)))
            seed = e;
    REQUIRE(seed != kInvalid);
    SymmetryMap topo = SymmetryMap::buildTopological(sphere, seed);
    REQUIRE_FALSE(topo.empty());
    for (Index p = 0; p < sphere.pointCapacity(); ++p)
        CHECK(topo.mirrorPoint(p) == map.mirrorPoint(p));

    const Index f = 5;
    const Index mf = map.mirrorFace(sphere, f);
    REQUIRE(mf != kInvalid);
    CHECK(sphere.faceCentroid(mf).x == doctest::Approx(-sphere.faceCentroid(f).x));
}

TEST_CASE("symmetry: ops stay symmetric")
{
    // Even subdivisions put a column of points on the plane.
    const CubeOptions co{100, 100, 100, 4, 4, 4};
    Mesh m = makeCube(co);
    SymmetryMap map = SymmetryMap::build(m);
    REQUIRE(map.unmatchedCount() == 0);
    std::vector<Index> sel;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        const glm::dvec3 c = m.faceCentroid(f);
        if (m.faceNormal(f).y > 0.9 && c.x > 0.0 && c.x < 25.0 && c.z > 0.0 && c.z < 25.0)
            sel.push_back(f);
    }
    REQUIRE(sel.size() == 1);
    ExtrudeOptions eo;
    eo.offset = 20.0;
    eo.symmetry = &map;
    ExtrudeResult r = extrudeFaces(m, sel, eo);
    CHECK(r.faces.size() == 2);
    checkClosed(m);
    CHECK(isSymmetric(m));

    // Bevel an edge touching the plane.
    m.compact();
    map = SymmetryMap::build(m);
    REQUIRE(map.unmatchedCount() == 0);
    Index e = kInvalid;
    for (Index i = 0; i < m.edgeCapacity() && e == kInvalid; ++i)
    {
        const glm::dvec3 a = m.position(m.edgePoint(i, 0)), b = m.position(m.edgePoint(i, 1));
        if (std::abs(a.y + 50.0) < 1e-6 && std::abs(b.y + 50.0) < 1e-6 && std::abs(a.z - b.z) < 1e-6
            && std::min(a.x, b.x) > -1e-6 && std::min(a.x, b.x) < 1e-6 && std::abs(a.z) < 30.0 && a.z > 0)
            e = i;
    }
    REQUIRE(e != kInvalid);
    BevelOptions bo;
    bo.offset = 5.0;
    bo.segments = 2;
    bo.symmetry = &map;
    bevelEdges(m, std::vector<Index>{e}, bo);
    checkClosed(m);
    CHECK(isSymmetric(m));

    // Inset across the plane.
    m.compact();
    map = SymmetryMap::build(m);
    sel.clear();
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        const glm::dvec3 c = m.faceCentroid(f);
        if (m.faceNormal(f).z > 0.9 && c.x > 0.0 && c.x < 25.0 && c.y > 0.0 && c.y < 25.0)
            sel.push_back(f);
    }
    REQUIRE(sel.size() == 1);
    InsetOptions io;
    io.thickness = 3.0;
    io.depth = -5.0;
    io.symmetry = &map;
    insetFaces(m, sel, io);
    checkClosed(m);
    CHECK(isSymmetric(m));
}

TEST_CASE("ops: more coverage")
{
    // Closed loop around a cylinder's side.
    Mesh cyl = makeCylinder({50.0, 100.0, 16, 2});
    Index ring = kInvalid;
    for (Index e = 0; e < cyl.edgeCapacity() && ring == kInvalid; ++e)
        if (std::abs(cyl.position(cyl.edgePoint(e, 0)).y) < 1e-9 && std::abs(cyl.position(cyl.edgePoint(e, 1)).y) < 1e-9)
            ring = e;
    REQUIRE(ring != kInvalid);
    CHECK(edgeLoop(cyl, ring).size() == 16);

    // Extruding a whole open plane walls in its border and keeps the plane facing back at the base.
    Mesh plane = makePlane({100, 100, 2, 2});
    std::vector<Index> all;
    for (Index f = 0; f < plane.faceCapacity(); ++f)
        all.push_back(f);
    Mesh open = plane;
    ExtrudeOptions eo;
    eo.offset = 10.0;
    ExtrudeResult r = extrudeFaces(plane, all, eo);
    CHECK(isValid(plane));
    CHECK(r.sides.size() == 8);
    CHECK(r.back.size() == 4);
    CHECK(boundaryEdgeCount(plane) == 0);
    CHECK(plane.numPoints() == 18);
    CHECK(plane.bounds().max.y == doctest::Approx(10.0));
    CHECK(plane.bounds().min.y == doctest::Approx(0.0));
    for (Index f : r.back)
        CHECK(plane.faceNormal(f).y == doctest::Approx(-1.0));

    eo.keepBack = false;
    r = extrudeFaces(open, all, eo);
    CHECK(isValid(open));
    CHECK(r.back.empty());
    CHECK(boundaryEdgeCount(open) == 8);

    // Part of an open plane meets the faces around it, so it has no back.
    Mesh part = makePlane({100, 100, 2, 2});
    eo.keepBack = true;
    r = extrudeFaces(part, std::vector<Index>{0}, eo);
    CHECK(isValid(part));
    CHECK(r.back.empty());

    // Individual inset keeps faces apart.
    Mesh grid = makePlane({100, 100, 2, 1});
    InsetOptions io;
    io.thickness = 5.0;
    io.individual = true;
    r = insetFaces(grid, std::vector<Index>{0, 1}, io);
    CHECK(isValid(grid));
    CHECK(r.sides.size() == 8);
    CHECK(grid.faceArea(0) == doctest::Approx(40.0 * 90.0));

    // Merge with symmetry welds both sides.
    Mesh a = makePlane({100, 100, 2, 2});
    Mesh b = a;
    // A gap under the merge distance; coincident points would make mirror matches ambiguous.
    for (glm::dvec3& p : b.positions())
        p.z -= 100.005;
    a.append(b);
    SymmetryMap map = SymmetryMap::build(a);
    REQUIRE(map.unmatchedCount() == 0);
    std::vector<Index> seam;
    for (Index p = 0; p < a.pointCapacity(); ++p)
        if (std::abs(a.position(p).z + 50.0) < 0.01 && a.position(p).x > 1.0)
            seam.push_back(p);
    REQUIRE(seam.size() == 2);
    CHECK(map.expandPoints(seam).size() == 4);
    MergeOptions mo;
    mo.distance = 0.01;
    mo.symmetry = &map;
    mergeByDistance(a, seam, mo);
    CHECK(isValid(a));
    CHECK(a.numPoints() == 18 - 2);
}

TEST_CASE("perf: ops on large meshes")
{
    Mesh m = makePlane({1000.0, 1000.0, 1000, 1000});
    std::vector<Index> some;
    for (Index f = 0; f < m.faceCapacity(); f += 100)
        some.push_back(f);
    auto t0 = std::chrono::steady_clock::now();
    ExtrudeOptions eo;
    eo.offset = 1.0;
    eo.individual = true;
    extrudeFaces(m, some, eo);
    const double tExtrude = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(m.numFaces() == 1000000 + 4 * 10000);

    Mesh q = makePlane({1000.0, 1000.0, 500, 500});
    t0 = std::chrono::steady_clock::now();
    Mesh s = subdivide(q);
    const double tSub = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(s.numFaces() == 1000000);

    Mesh b = makePlane({1000.0, 1000.0, 300, 300});
    std::vector<Index> edges;
    for (Index e = 0; e < b.edgeCapacity(); e += 7)
        if (!b.isBoundaryEdge(e))
            edges.push_back(e);
    t0 = std::chrono::steady_clock::now();
    BevelOptions bo;
    bo.offset = 0.5;
    bevelEdges(b, edges, bo);
    const double tBevel = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(isValid(b));
    MESSAGE("10k individual extrudes on 1M quads " << tExtrude << "s, subdivide 250k -> 1M quads " << tSub
                                                   << "s, bevel " << edges.size() << " edges " << tBevel << "s");
}
