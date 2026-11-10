#include <doctest/doctest.h>

#include "TestUtil.h"

#include "geo/ComponentPaths.h"
#include "geo/Primitives.h"
#include "geo/Topology.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"

#include <algorithm>
#include <cmath>

using namespace lila;
using geo::Index;
using namespace testutil;

namespace
{
Index edgeJoining(const geo::Mesh& m, double x0, double z0, double x1, double z1) { return m.findEdge(pointAt(m, x0, z0), pointAt(m, x1, z1)); }

/** 4 x 4 quads, points on the integer grid from -2 to 2. */
geo::Mesh grid() { return geo::makePlane({.width = 4.0, .depth = 4.0, .subdivX = 4, .subdivZ = 4}); }
} // namespace

TEST_CASE("edge path between follows a shared loop, a shared ring, else the shortest path")
{
    geo::Mesh m = grid();
    Index a = edgeJoining(m, -2, 0, -1, 0), b = edgeJoining(m, 1, 0, 2, 0);
    REQUIRE(a != geo::kInvalid);
    REQUIRE(b != geo::kInvalid);
    std::vector<Index> loop = geo::edgePathBetween(m, a, b);
    REQUIRE(loop.size() == 4);
    CHECK(loop.front() == a);
    CHECK(loop.back() == b);
    for (Index e : loop)
        CHECK(std::abs(m.position(m.edgePoint(e, 0)).z) < 1e-9);

    Index r0 = edgeJoining(m, -1, -2, -1, -1), r1 = edgeJoining(m, 2, -2, 2, -1);
    std::vector<Index> ring = geo::edgePathBetween(m, r0, r1);
    CHECK(ring.size() == 4);
    CHECK(has(ring, edgeJoining(m, 0, -2, 0, -1)));
    CHECK(has(ring, edgeJoining(m, 1, -2, 1, -1)));

    // Border edges share the boundary loop, so it is followed the shorter way round.
    std::vector<Index> border = geo::edgePathBetween(m, edgeJoining(m, -2, -2, -1, -2), edgeJoining(m, 1, 2, 2, 2));
    CHECK(border.size() == 9);
    for (Index e : border)
        CHECK(m.isBoundaryEdge(e));

    // Neither loop nor ring: the shortest connected chain.
    Index c0 = edgeJoining(m, -1, -1, 0, -1), c1 = edgeJoining(m, 0, 1, 1, 1);
    std::vector<Index> path = geo::edgePathBetween(m, c0, c1);
    CHECK(has(path, c0));
    CHECK(has(path, c1));
    CHECK(geo::chainEdges(m, path).size() == 1);
    CHECK(path.size() == 4);
}

TEST_CASE("edge path takes the shorter way round a closed loop")
{
    geo::Mesh m = geo::makeCylinder({.radius = 1.0, .height = 2.0, .subdivAxis = 8, .subdivHeight = 2});
    Index mid = geo::kInvalid;
    for (Index e = 0; e < m.edgeCapacity() && mid == geo::kInvalid; ++e)
        if (m.edgeValid(e) && std::abs(m.position(m.edgePoint(e, 0)).y) < 1e-6 && std::abs(m.position(m.edgePoint(e, 1)).y) < 1e-6)
            mid = e;
    REQUIRE(mid != geo::kInvalid);
    std::vector<Index> loop = geo::edgeLoop(m, mid);
    REQUIRE(loop.size() == 8);
    std::vector<Index> path = geo::edgePathBetween(m, loop[0], loop[5]);
    REQUIRE(path.size() == 4);
    for (Index e : {loop[0], loop[5], loop[6], loop[7]})
        CHECK(has(path, e));
}

TEST_CASE("point and face paths between")
{
    geo::Mesh m = grid();
    std::vector<Index> line = geo::pointPathBetween(m, pointAt(m, -2, 0), pointAt(m, 2, 0));
    REQUIRE(line.size() == 5);
    for (Index p : line)
        CHECK(std::abs(m.position(p).z) < 1e-9);
    std::vector<Index> diag = geo::pointPathBetween(m, pointAt(m, -2, -2), pointAt(m, 2, 2));
    CHECK(diag.size() == 9);
    CHECK(diag.front() == pointAt(m, -2, -2));
    CHECK(diag.back() == pointAt(m, 2, 2));

    std::vector<Index> row = geo::facePathBetween(m, faceAt(m, -1.5, 0.5), faceAt(m, 1.5, 0.5));
    REQUIRE(row.size() == 4);
    for (Index f : row)
        CHECK(std::abs(m.faceCentroid(f).z - 0.5) < 1e-9);
    std::vector<Index> across = geo::facePathBetween(m, faceAt(m, -1.5, -1.5), faceAt(m, 1.5, 1.5));
    CHECK(across.size() == 7);
    CHECK(across.front() == faceAt(m, -1.5, -1.5));
    CHECK(across.back() == faceAt(m, 1.5, 1.5));
    CHECK(geo::pointPathBetween(m, pointAt(m, 0, 0), pointAt(m, 0, 0)).size() == 1);
}

TEST_CASE("face loop crosses the edge's ring")
{
    geo::Mesh m = grid();
    std::vector<Index> loop = geo::faceLoop(m, edgeJoining(m, 0, -1, 0, 0));
    REQUIRE(loop.size() == 4);
    for (Index f : loop)
        CHECK(std::abs(m.faceCentroid(f).z + 0.5) < 1e-9);
}

TEST_CASE("contained faces flood fill inside closed borders")
{
    geo::Mesh cyl = geo::makeCylinder({.radius = 1.0, .height = 2.0, .subdivAxis = 8, .subdivHeight = 2, .capFill = geo::CapFill::Ngon});
    std::vector<Index> topRing;
    for (Index e = 0; e < cyl.edgeCapacity(); ++e)
        if (cyl.edgeValid(e) && cyl.position(cyl.edgePoint(e, 0)).y > 0.999 && cyl.position(cyl.edgePoint(e, 1)).y > 0.999)
            topRing.push_back(e);
    REQUIRE(topRing.size() == 8);
    std::vector<Index> cap = geo::containedFaces(cyl, topRing);
    REQUIRE(cap.size() == 1);
    CHECK(cyl.faceSize(cap[0]) == 8);
    CHECK(geo::containedFaces(cyl, topRing, false).size() == cyl.numFaces() - 1);

    // The mesh border is a wall: a square around the centre four quads of a grid.
    geo::Mesh m = grid();
    std::vector<Index> square;
    for (int k = -1; k < 1; ++k)
        for (Index e : {edgeJoining(m, k, -1, k + 1, -1), edgeJoining(m, k, 1, k + 1, 1), edgeJoining(m, -1, k, -1, k + 1), edgeJoining(m, 1, k, 1, k + 1)})
            square.push_back(e);
    std::vector<Index> inside = geo::containedFaces(m, square);
    CHECK(inside.size() == 4);
    for (Index f : inside)
        CHECK(std::abs(m.faceCentroid(f).x) < 1.0);
    Index open[] = {edgeJoining(m, -1, -1, 0, -1)};
    CHECK(geo::containedFaces(m, open).empty());
}

TEST_CASE("loop, path and contained face commands select as one undo step")
{
    using scene::ComponentMode;
    namespace comp = scene::components;
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 4}, {"depth", 4}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    scene::NodeId id = scene::Graph::current().find(plane)->id();
    run("selectMode", {{"args", {"edge"}}});

    json loop = run("selectLoopThrough", {{"args", {plane}}, {"edge", 0}});
    CHECK(loop.size() >= 4);
    CHECK(comp::selected(id, ComponentMode::Edge).size() == loop.size());
    run("undo");
    CHECK(comp::selected(id, ComponentMode::Edge).empty());

    json path = run("selectPathBetween", {{"args", {plane}}, {"kind", "edge"}, {"from", loop[0]}, {"to", loop[2]}});
    CHECK(path.size() == 3);
    CHECK(comp::selected(id, ComponentMode::Edge).size() == 3);
    run("undo");
    CHECK(comp::selected(id, ComponentMode::Edge).empty());

    // Deselecting a run removes only that run, even between neighbours that would otherwise take the whole loop.
    run("selectLoopThrough", {{"args", {plane}}, {"edge", 0}});
    run("selectPathBetween", {{"args", {plane}}, {"kind", "edge"}, {"from", loop[0]}, {"to", loop[2]}, {"deselect", true}});
    CHECK(comp::selected(id, ComponentMode::Edge).size() == loop.size() - 3);
    run("selectLoopThrough", {{"args", {plane}}, {"edge", 0}});
    run("selectPathBetween", {{"args", {plane}}, {"kind", "edge"}, {"from", loop[0]}, {"to", loop[1]}, {"deselect", true}});
    CHECK(comp::selected(id, ComponentMode::Edge).size() == loop.size() - 2);
    run("selectLoopThrough", {{"args", {plane}}, {"edge", 0}, {"deselect", true}});

    json faces = run("selectLoopThrough", {{"args", {plane}}, {"edge", 0}, {"faces", true}});
    CHECK(comp::mode() == ComponentMode::Face);
    CHECK(comp::selected(id, ComponentMode::Face).size() == faces.size());
    CHECK_THROWS_AS(run("selectPathBetween", {{"args", {plane}}, {"kind", "face"}, {"from", 0}}), CommandError);

    run("newScene");
    std::string cyl = run("polyCylinder", {{"radius", 1}, {"height", 2}, {"subdivisionsAxis", 8}, {"subdivisionsHeight", 2}, {"capType", "ngon"}})[0];
    scene::NodeId cid = scene::Graph::current().find(cyl)->id();
    json top = run("findComponents", {{"args", {cyl}}, {"kind", "edge"}, {"extreme", "+y"}, {"count", 8}});
    REQUIRE(top.size() == 8);
    run("selectComponents", {{"args", {cyl}}, {"edges", top}});
    json out = run("selectContainedFaces");
    CHECK(out[cyl].size() == 1);
    CHECK(comp::mode() == ComponentMode::Face);
    run("undo");
    CHECK(comp::selected(cid, ComponentMode::Face).empty());
    CHECK(comp::selected(cid, ComponentMode::Edge).size() == 8);
}
