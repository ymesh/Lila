#include "TestUtil.h"

#include "geo/LoopTools.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
Index pointAt(const Mesh& m, const glm::dvec3& p)
{
    for (Index i = 0; i < m.pointCapacity(); ++i)
        if (m.pointValid(i) && glm::distance(m.position(i), p) < 1e-6)
            return i;
    return kInvalid;
}

/** The plane's edges along z at x, between zFrom and zTo (grid lines every 25 cm on a 100 cm, 4 x 4 plane). */
std::vector<Index> columnEdges(const Mesh& m, double x, double zFrom, double zTo)
{
    std::vector<Index> out;
    for (double z = zFrom; z < zTo - 1e-9; z += 25.0)
        out.push_back(m.findEdge(pointAt(m, {x, 0.0, z}), pointAt(m, {x, 0.0, z + 25.0})));
    return out;
}

/** Points whose x is within 1e-6 of x. */
int pointsAtX(const Mesh& m, double x)
{
    int n = 0;
    for (Index i = 0; i < m.pointCapacity(); ++i)
        n += m.pointValid(i) && std::abs(m.position(i).x - x) < 1e-6;
    return n;
}

bool mirrored(const Mesh& m)
{
    return SymmetryMap::build(m, {}, 1e-4).unmatchedCount() == 0;
}
} // namespace

TEST_CASE("edge loops: several loops follow fraction and spacing")
{
    std::vector<double> even = edgeLoopFractions({0.5, 3});
    REQUIRE(even.size() == 3);
    CHECK(even[0] == doctest::Approx(0.25));
    CHECK(even[2] == doctest::Approx(0.75));

    std::vector<double> slid = edgeLoopFractions({0.3, 2});
    REQUIRE(slid.size() == 2);
    CHECK(slid[0] == doctest::Approx(0.3 - 1.0 / 6.0));
    CHECK(slid[1] == doctest::Approx(0.3 + 1.0 / 6.0));

    std::vector<double> packed = edgeLoopFractions({0.5, 2, 0.3});
    CHECK(packed[1] - packed[0] == doctest::Approx(0.1));

    // Pushed against the end, the group slides back onto the edge instead of leaving it.
    std::vector<double> end = edgeLoopFractions({0.0, 2});
    CHECK(end[0] > 0.0);
    CHECK(end[1] - end[0] == doctest::Approx(1.0 / 3.0));

    Mesh plane = makePlane({100, 100, 1, 1});
    const Index ring = plane.findEdge(pointAt(plane, {-50, 0, 50}), pointAt(plane, {-50, 0, -50}));
    REQUIRE(ring != kInvalid);
    const glm::dvec3 from = plane.position(plane.edgePoint(ring, 0)), to = plane.position(plane.edgePoint(ring, 1));
    std::vector<Index> loops = insertEdgeLoop(plane, ring, {0.25, 2, 0.5});
    CHECK(isValid(plane));
    CHECK(loops.size() == 2);
    CHECK(plane.numFaces() == 3);
    // 0.25 either side by half of a 1/6 gap, from the ring edge's first point.
    CHECK(pointAt(plane, glm::mix(from, to, 0.25 - 1.0 / 12.0)) != kInvalid);
    CHECK(pointAt(plane, glm::mix(from, to, 0.25 + 1.0 / 12.0)) != kInvalid);
}

TEST_CASE("offset edge loop: a full loop gets a loop either side")
{
    Mesh m = makePlane({100, 100, 4, 4});
    std::vector<Index> made = offsetEdgeLoops(m, columnEdges(m, 0.0, -50.0, 50.0));
    CHECK(isValid(m));
    CHECK(made.size() == 8);
    CHECK(m.numFaces() == 24);
    CHECK(pointsAtX(m, 6.25) == 5);
    CHECK(pointsAtX(m, -6.25) == 5);

    Mesh one = makePlane({100, 100, 4, 4});
    offsetEdgeLoops(one, columnEdges(one, 0.0, -50.0, 50.0), {.distance = 10.0, .absolute = true, .side = OffsetSide::Left});
    CHECK(isValid(one));
    CHECK(one.numFaces() == 20);
    CHECK(pointsAtX(one, 10.0) + pointsAtX(one, -10.0) == 5);

    Mesh removed = makePlane({100, 100, 4, 4});
    offsetEdgeLoops(removed, columnEdges(removed, 0.0, -50.0, 50.0), {}, true);
    CHECK(isValid(removed));
    CHECK(removed.numFaces() == 20);
    CHECK(pointsAtX(removed, 0.0) == 0);
}

TEST_CASE("offset edge loop: open ends converge or wrap")
{
    Mesh m = makePlane({100, 100, 4, 4});
    offsetEdgeLoops(m, columnEdges(m, 0.0, -25.0, 25.0));
    CHECK(isValid(m));
    CHECK(m.numFaces() == 20);
    CHECK(pointsAtX(m, 6.25) == 1);

    Mesh w = makePlane({100, 100, 4, 4});
    std::vector<Index> made = offsetEdgeLoops(w, columnEdges(w, 0.0, -25.0, 25.0), {.ends = OffsetEnds::Wrap});
    CHECK(isValid(w));
    CHECK(made.size() == 8);
    CHECK(w.numFaces() == 24);
    // The two loops meet on the edges carrying the chain on past its ends.
    CHECK(pointAt(w, {0, 0, 31.25}) != kInvalid);
    CHECK(pointAt(w, {0, 0, -31.25}) != kInvalid);
}

TEST_CASE("offset edge loop follows symmetry, one side included")
{
    for (OffsetSide side : {OffsetSide::Both, OffsetSide::Left})
    {
        Mesh m = makePlane({100, 100, 4, 4});
        const SymmetryMap sym = SymmetryMap::build(m);
        OffsetLoopOptions o;
        o.side = side;
        o.symmetry = &sym;
        offsetEdgeLoops(m, columnEdges(m, -25.0, -50.0, 50.0), o);
        CHECK(isValid(m));
        CHECK(m.numFaces() == (side == OffsetSide::Both ? 32 : 24));
        CHECK(mirrored(m));
    }
}

TEST_CASE("edit edge flow bends a flat loop to its neighbours' curve")
{
    // A parabola across x, with the x = 0 column pressed flat between its neighbours.
    auto bent = [] {
        Mesh m = makePlane({100, 100, 4, 4});
        for (Index p = 0; p < m.pointCapacity(); ++p)
        {
            glm::dvec3 q = m.position(p);
            q.y = std::abs(q.x) < 1e-9 ? 0.0 : 10.0 - q.x * q.x / 62.5;
            m.setPosition(p, q);
        }
        return m;
    };
    auto heightAt = [](const PointMoves& moves, const Mesh& m, double z) {
        for (const PointMove& mv : moves)
            if (std::abs(m.position(mv.point).x) < 1e-9 && std::abs(m.position(mv.point).z - z) < 1e-9)
                return mv.position.y;
        return -1e9;
    };

    Mesh m = bent();
    const std::vector<Index> loop = columnEdges(m, 0.0, -50.0, 50.0);
    PointMoves up = editEdgeFlow(m, loop, {}, {});
    CHECK(up.size() == 5);
    CHECK(heightAt(up, m, 0.0) > 3.0);
    for (const PointMove& mv : up)
    {
        CHECK(mv.position.x == doctest::Approx(m.position(mv.point).x));
        CHECK(mv.position.z == doctest::Approx(m.position(mv.point).z));
    }

    EdgeFlowOptions flat;
    flat.amount = 0.0;
    CHECK(heightAt(editEdgeFlow(m, loop, {}, flat), m, 0.0) == doctest::Approx(0.0));
    EdgeFlowOptions inverted;
    inverted.amount = -1.0;
    CHECK(heightAt(editEdgeFlow(m, loop, {}, inverted), m, 0.0) < -3.0);

    EdgeFlowOptions fromNormals;
    fromNormals.edgeFlow = false;
    CHECK(heightAt(editEdgeFlow(m, loop, {}, fromNormals), m, 0.0) > 1.0);

    // A loose vertex fits both directions: across x it rises, along z it is already flat.
    const Index centre = pointAt(m, {0, 0, 0});
    PointMoves loose = editEdgeFlow(m, {}, std::vector<Index>{centre}, {});
    REQUIRE(loose.size() == 1);
    CHECK(loose[0].position.y > 1.0);
    CHECK(loose[0].position.y < heightAt(up, m, 0.0));
}

TEST_CASE("edit edge flow ignores a far point folded back between the anchors")
{
    Mesh m = makePlane({100, 100, 4, 4});
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (std::abs(m.position(p).x + 50.0) < 1e-9)
            m.setPosition(p, m.position(p) + glm::dvec3(55.0, 0.5, 0.0));
    for (const PointMove& mv : editEdgeFlow(m, columnEdges(m, 0.0, -50.0, 50.0), {}, {}))
        CHECK(glm::distance(mv.position, m.position(mv.point)) < 25.0);
}

TEST_CASE("edit edge flow selections of points")
{
    Mesh m = makePlane({100, 100, 4, 4});
    std::vector<Index> column;
    for (double z = -50.0; z <= 50.0; z += 25.0)
        column.push_back(pointAt(m, {0, 0, z}));
    std::vector<Index> edges, loose;
    flowSelectionOfPoints(m, column, edges, loose);
    CHECK(edges.size() == 4);
    CHECK(loose.empty());

    std::vector<Index> patch;
    for (double x = -25.0; x <= 25.0; x += 25.0)
        for (double z = -25.0; z <= 25.0; z += 25.0)
            patch.push_back(pointAt(m, {x, 0, z}));
    flowSelectionOfPoints(m, patch, edges, loose);
    CHECK(loose.size() == 9);
}

TEST_CASE("edit edge flow moves neighbouring loops together")
{
    // Three flat columns side by side in a parabola across x: each curves from the unselected columns past them.
    Mesh m = makePlane({200, 200, 8, 8});
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        glm::dvec3 q = m.position(p);
        q.y = std::abs(q.x) < 30.0 ? 0.0 : 10.0 - q.x * q.x / 250.0;
        m.setPosition(p, q);
    }
    std::vector<Index> loops;
    for (double x : {-25.0, 0.0, 25.0})
        std::ranges::copy(columnEdges(m, x, -100.0, 100.0), std::back_inserter(loops));
    PointMoves moves = editEdgeFlow(m, loops, {}, {});
    auto heightAt = [&](double x) {
        for (const PointMove& mv : moves)
            if (std::abs(m.position(mv.point).x - x) < 1e-9 && std::abs(m.position(mv.point).z) < 1e-9)
                return mv.position.y;
        return -1e9;
    };
    CHECK(heightAt(0.0) > 8.0);
    CHECK(heightAt(25.0) > 5.0);
    CHECK(heightAt(25.0) < heightAt(0.0));
    CHECK(heightAt(-25.0) == doctest::Approx(heightAt(25.0)));
}

TEST_CASE("split rings subdivides one way")
{
    Mesh m = makePlane({100, 100, 4, 4});
    const std::vector<Index> column = columnEdges(m, 0.0, -50.0, 50.0);
    std::vector<Index> made = splitRings(m, column);
    CHECK(isValid(m));
    CHECK(m.numFaces() == 32);
    CHECK(made.size() == 16);
    CHECK(pointAt(m, {0, 0, -37.5}) != kInvalid);
    CHECK(pointAt(m, {-37.5, 0, 0}) == kInvalid);

    // Two edges of one ring cut it once.
    Mesh once = makePlane({100, 100, 4, 4});
    splitRings(once, std::vector<Index>{columnEdges(once, 0.0, -50.0, -25.0)[0], columnEdges(once, 25.0, -50.0, -25.0)[0]}, 2);
    CHECK(isValid(once));
    CHECK(once.numFaces() == 24);

    // Faces: a strip along x gets lines across it, through whole rings.
    Mesh strip = makePlane({100, 100, 4, 4});
    std::vector<Index> row;
    for (Index f = 0; f < strip.faceCapacity(); ++f)
        if (strip.faceValid(f) && std::abs(strip.faceCentroid(f).z - 12.5) < 1e-6)
            row.push_back(f);
    REQUIRE(row.size() == 4);
    splitRings(strip, regionRingEdges(strip, row, true));
    CHECK(isValid(strip));
    CHECK(strip.numFaces() == 32);
    CHECK(pointsAtX(strip, 12.5) == 5);
    Mesh along = makePlane({100, 100, 4, 4});
    splitRings(along, regionRingEdges(along, row, false));
    CHECK(along.numFaces() == 20);
    CHECK(pointAt(along, {0, 0, 12.5}) != kInvalid);
}

TEST_CASE("split rings follows symmetry")
{
    Mesh m = makePlane({100, 100, 4, 4});
    const SymmetryMap sym = SymmetryMap::build(m);
    const Index left = m.findEdge(pointAt(m, {-50, 0, 0}), pointAt(m, {-25, 0, 0}));
    const Index right = m.findEdge(pointAt(m, {25, 0, 0}), pointAt(m, {50, 0, 0}));
    splitRings(m, std::vector<Index>{left, right}, 1, &sym);
    CHECK(isValid(m));
    CHECK(m.numFaces() == 24);
    CHECK(pointsAtX(m, -37.5) == 5);
    CHECK(pointsAtX(m, 37.5) == 5);
    CHECK(mirrored(m));
}

TEST_CASE("edit edge flow centres a loop on a band curving in its own plane")
{
    // A flat 4 x 4 plane bent into an annulus sector: x is the radius from 200, z the angle at 15 degrees per 25 cm.
    auto annulus = [](double x, double z) {
        const double r = 200.0 + x, a = glm::radians(z / 25.0 * 15.0);
        return glm::dvec3(r * std::cos(a), 0.0, r * std::sin(a));
    };
    Mesh m = makePlane({100, 100, 4, 4});
    const std::vector<Index> loop = columnEdges(m, 0.0, -50.0, 50.0);
    std::vector<glm::dvec3> uv(m.pointCapacity());
    for (Index p = 0; p < m.pointCapacity(); ++p)
        uv[p] = m.position(p);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        m.setPosition(p, annulus(uv[p].x, uv[p].z));
    // The z = 0 row pressed onto the chords toward one neighbour: off the arc and off centre.
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (std::abs(uv[p].z) < 1e-9)
            m.setPosition(p, glm::mix(annulus(uv[p].x, -25.0), annulus(uv[p].x, 25.0), 0.3));
    std::vector<Index> row;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && std::abs(uv[m.edgePoint(e, 0)].z) < 1e-9 && std::abs(uv[m.edgePoint(e, 1)].z) < 1e-9)
            row.push_back(e);
    REQUIRE(row.size() == 4);
    for (const PointMove& mv : editEdgeFlow(m, row, {}, {}))
    {
        CHECK(std::abs(std::atan2(mv.position.z, mv.position.x)) < 1e-9);
        CHECK(glm::length(mv.position) == doctest::Approx(200.0 + uv[mv.point].x).epsilon(1e-9));
    }
}

TEST_CASE("edit edge flow spreads split rings as subdivision would")
{
    // An open 12-sided cylinder, every column split twice: the new points land evenly on the circle.
    Mesh m = makeCylinder({.radius = 100.0, .height = 100.0, .subdivAxis = 12, .caps = false});
    std::vector<Index> bottom;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.position(m.edgePoint(e, 0)).y < -49.0 && m.position(m.edgePoint(e, 1)).y < -49.0)
            bottom.push_back(e);
    REQUIRE(bottom.size() == 12);
    const std::vector<Index> made = splitRings(m, bottom, 2);
    CHECK(isValid(m));
    const PointMoves moves = editEdgeFlow(m, made, {}, {});
    CHECK(moves.size() == 2 * 2 * 12);
    for (const PointMove& mv : moves)
    {
        CHECK(std::hypot(mv.position.x, mv.position.z) == doctest::Approx(100.0).epsilon(1e-9));
        m.setPosition(mv.point, mv.position);
    }
    // Every edge round the circle is the same length.
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && std::abs(m.position(m.edgePoint(e, 0)).y - m.position(m.edgePoint(e, 1)).y) < 1e-9)
            CHECK(m.edgeLength(e) == doctest::Approx(2.0 * 100.0 * std::sin(glm::radians(5.0))).epsilon(1e-9));
}
