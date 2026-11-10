#include "TestUtil.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Primitives.h"
#include "retopo/Construct.h"
#include "retopo/Handles.h"
#include "retopo/Retopo.h"
#include "retopo/SurfaceStroke.h"
#include "retopo/TopologyCommands.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <doctest/doctest.h>

#include <cmath>
#include <map>

using namespace lila;
using namespace retopotest;
using geo::Index;

namespace
{
constexpr double kPi = 3.14159265358979323846;

geo::Mesh plane(double size, int subdiv)
{
    geo::PlaneOptions o;
    o.width = size;
    o.depth = size;
    o.subdivX = subdiv;
    o.subdivZ = subdiv;
    return geo::makePlane(o);
}

scene::Node* meshNode(const std::string& name, geo::Mesh mesh)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name, scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    return n;
}

json P(double x, double y, double z) { return json::array({x, y, z}); }

/** Each interior edge in two faces, each border edge in one, every face a quad. */
void checkQuadManifold(const geo::Mesh& m, const std::vector<std::vector<Index>>& faces)
{
    std::map<std::pair<Index, Index>, int> edges;
    for (const auto& f : faces)
    {
        CHECK(f.size() == 4);
        for (size_t k = 0; k < f.size(); ++k)
        {
            Index a = f[k], b = f[(k + 1) % f.size()];
            edges[{std::min(a, b), std::max(a, b)}]++;
        }
    }
    for (auto& [e, n] : edges)
        CHECK(n <= 2);
    (void)m;
}

size_t borderEdges(const geo::Mesh& m)
{
    size_t n = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        n += m.edgeValid(e) && m.isBoundaryEdge(e);
    return n;
}
} // namespace

TEST_CASE("surface stroke: resampling, corners and closing")
{
    std::vector<glm::dvec3> line = {{0, 0, 0}, {10, 0, 0}};
    std::vector<glm::dvec3> even = retopo::resamplePolyline(line, 5);
    REQUIRE(even.size() == 6);
    CHECK(glm::length(even[1] - glm::dvec3(2, 0, 0)) < 1e-9);
    CHECK(even.back() == glm::dvec3(10, 0, 0));

    std::vector<glm::dvec3> raw;
    for (int i = 0; i <= 40; ++i)
        raw.push_back({double(i), 0.0, 0.0});
    for (int i = 1; i <= 40; ++i)
        raw.push_back({40.0, 0.0, double(i)});
    retopo::StrokeOptions o;
    o.spacing = 1.0;
    retopo::SurfaceStroke s = retopo::processStroke(raw, nullptr, o);
    CHECK_FALSE(s.closed);
    REQUIRE(s.corners.size() == 1);
    CHECK(glm::length(s.points[s.corners[0]] - glm::dvec3(40, 0, 0)) < 1.5);
    CHECK(s.points.front() == glm::dvec3(0, 0, 0));
    CHECK(s.points.back() == glm::dvec3(40, 0, 40));

    std::vector<glm::dvec3> circle;
    for (int i = 0; i < 64; ++i)
        circle.push_back({20.0 * std::cos(i * 2 * kPi / 63.5), 0.0, 20.0 * std::sin(i * 2 * kPi / 63.5)});
    o.closeDistance = 3.0;
    retopo::SurfaceStroke c = retopo::processStroke(circle, nullptr, o);
    CHECK(c.closed);
    CHECK(c.corners.empty());
}

TEST_CASE("generators: strip keeps square quads and makes a sharp corner")
{
    geo::Mesh ref = plane(200.0, 20);
    geo::MeshBvh bvh(ref);
    retopo::StripSpec s;
    s.knots = {{-60, 0, 0}, {0, 0, 0}, {0, 0, 60}};
    s.corners = {0, 1, 0};
    s.width = 10.0;
    retopo::GenContext ctx{&ref, &bvh, 10.0};
    retopo::GenResult r = retopo::generate(s, ctx);
    REQUIRE(r.ok());
    CHECK(r.faces.size() == 12);
    checkQuadManifold(ref, r.faces);
    REQUIRE(r.sideCounts.size() == 2);
    CHECK(r.sideCounts[0] == 12);
    // Some row sits on the corner, mitred: its two points straddle the corner diagonally.
    bool cornerRow = false;
    for (size_t i = 0; i + 1 < r.positions.size(); i += 2)
    {
        glm::dvec3 mid = (r.positions[i] + r.positions[i + 1]) * 0.5;
        if (glm::length(mid) < 1e-6)
        {
            cornerRow = true;
            CHECK(std::abs(glm::length(r.positions[i] - r.positions[i + 1]) - 10.0 * std::sqrt(2.0)) < 0.5);
        }
    }
    CHECK(cornerRow);
    for (const glm::dvec3& p : r.positions)
        CHECK(std::abs(p.y) < 1e-9);

    s.rows = 2;
    s.divisions = 5;
    r = retopo::generate(s, ctx);
    REQUIRE(r.ok());
    CHECK(r.faces.size() == 10);

    s.rows = 1;
    s.divisions = 0;
    s.startRow = std::array<glm::dvec3, 2>{glm::dvec3(-60, 0, 6), glm::dvec3(-60, 0, -6)};
    r = retopo::generate(s, ctx);
    REQUIRE(r.ok());
    CHECK(r.fixed[0]);
    CHECK(r.fixed[1]);
    CHECK((r.positions[0] == glm::dvec3(-60, 0, -6) || r.positions[0] == glm::dvec3(-60, 0, 6)));
}

TEST_CASE("generators: bridge, sweep and patch")
{
    geo::Mesh ref = plane(200.0, 20);
    geo::MeshBvh bvh(ref);
    retopo::GenContext ctx{&ref, &bvh, 10.0};

    retopo::BridgeSpec b;
    b.a = {{0, 0, 0}, {50, 0, 0}};
    b.b = {{50, 0, 30}, {0, 0, 30}};
    retopo::GenResult r = retopo::generate(b, ctx);
    REQUIRE(r.ok());
    CHECK(r.sideCounts == std::vector<int>{5, 3});
    CHECK(r.faces.size() == 15);
    checkQuadManifold(ref, r.faces);

    b.a = {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}};
    b.aFixed = true;
    r = retopo::generate(b, ctx);
    REQUIRE(r.ok());
    CHECK(r.sideCounts[0] == 3);
    CHECK(r.fixed[0]);

    retopo::SweepSpec sw;
    sw.border = {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {30, 0, 0}};
    sw.anchor = 0;
    sw.span = 3;
    sw.path = {{0, 0, 0}, {0, 0, 40}};
    r = retopo::generate(sw, ctx);
    REQUIRE(r.ok());
    CHECK(r.faces.size() == 12);
    CHECK(glm::length(r.positions.back() - glm::dvec3(30, 0, 40)) < 1e-6);
    sw.span = -1;
    CHECK_FALSE(retopo::generate(sw, ctx).ok());

    retopo::PatchSpec p;
    p.sides = {{{{0, 0, 0}, {40, 0, 0}}, false, 4},
               {{{40, 0, 0}, {40, 0, 20}}, false, 2},
               {{{40, 0, 20}, {0, 0, 20}}, false, 4},
               {{{0, 0, 20}, {0, 0, 10}, {0, 0, 0}}, true, 0}};
    r = retopo::generate(p, ctx);
    REQUIRE(r.ok());
    CHECK(r.faces.size() == 8);
    CHECK(r.sideCounts == std::vector<int>{4, 2, 4, 2});
    REQUIRE(!r.fixed.empty());
    CHECK(r.fixed[0] == 1);

    p.sides[1].count = 3;
    r = retopo::generate(p, ctx);
    REQUIRE(r.ok());
    int total = 0;
    for (int c : r.sideCounts)
        total += c;
    CHECK(total % 2 == 0);
}

TEST_CASE("generators: cylinder rings wrap the reference")
{
    geo::CylinderOptions o;
    o.radius = 20.0;
    o.height = 100.0;
    o.subdivAxis = 48;
    o.subdivHeight = 10;
    o.caps = false;
    geo::Mesh ref = geo::makeCylinder(o);
    geo::MeshBvh bvh(ref);
    retopo::GenContext ctx{&ref, &bvh, 10.0};
    retopo::CylinderSpec c;
    c.points = 8;
    c.rings = {{{20, -30, 0}, {0, 1, 0}, {20, -30, 0}}, {{20, 30, 0}, {0, 1, 0}, {20, 30, 0}}};
    retopo::GenResult r = retopo::generate(c, ctx);
    REQUIRE(r.ok());
    CHECK(r.faces.size() % 8 == 0);
    CHECK(r.faces.size() >= 16);
    for (const glm::dvec3& p : r.positions)
        CHECK(std::abs(std::hypot(p.x, p.z) - 20.0) < 0.6);
    checkQuadManifold(ref, r.faces);

    size_t open = r.faces.size();
    c.capStart = c.capEnd = true;
    r = retopo::generate(c, ctx);
    REQUIRE(r.ok());
    CHECK(r.faces.size() > open);
    c.points = 7;
    CHECK_FALSE(retopo::generate(c, ctx).ok());
}

TEST_CASE("constructs: stitching welds fixed points, hashing notices hand edits")
{
    geo::Mesh m = plane(20.0, 2);
    size_t facesBefore = m.numFaces();
    retopo::BridgeSpec b;
    b.a = {{-10, 0, 10}, {0, 0, 10}, {10, 0, 10}};
    b.aFixed = true;
    b.b = {{-10, 0, 30}, {10, 0, 30}};
    b.rows = 2;
    retopo::GenResult r = retopo::generate(b, retopo::GenContext{});
    REQUIRE(r.ok());
    Index pointsBefore = m.numPoints();
    retopo::StitchResult st = retopo::stitchConstruct(m, r, 7, 1e-6, std::nullopt);
    CHECK(st.faces.size() == 4);
    CHECK(m.numFaces() == facesBefore + 4);
    CHECK(m.numPoints() == pointsBefore + 6);
    CHECK(retopo::constructFaces(m, 7).size() == 4);
    uint64_t h = retopo::constructHash(m, 7);
    CHECK(retopo::constructHash(m, 7) == h);
    Index moved = m.facePoints(retopo::constructFaces(m, 7).back())[0];
    m.setPosition(moved, m.position(moved) + glm::dvec3(0.5, 0, 0));
    CHECK(retopo::constructHash(m, 7) != h);
    retopo::removeConstructFaces(m, 7);
    m.compact();
    CHECK(m.numFaces() == facesBefore);
    CHECK(m.numPoints() == pointsBefore);
}

TEST_CASE("constructs: symmetry stitches the mirror half")
{
    geo::Mesh m;
    retopo::BridgeSpec b;
    b.a = {{0, 0, 0}, {20, 0, 0}};
    b.b = {{0, 0, 10}, {20, 0, 10}};
    b.divisions = 2;
    b.rows = 1;
    retopo::GenResult r = retopo::generate(b, retopo::GenContext{});
    REQUIRE(r.ok());
    retopo::StitchResult st = retopo::stitchConstruct(m, r, 1, 1e-6, geo::axisPlane(0));
    CHECK(st.faces.size() == 4);
    // Points on x = 0 are shared by both halves.
    CHECK(m.numPoints() == 10);
}

TEST_CASE("constructs: symmetry keeps the half a construct mostly lies in, clamped to the plane")
{
    geo::Mesh m;
    retopo::BridgeSpec b;
    b.a = {{-5, 0, 0}, {15, 0, 0}};
    b.b = {{-5, 0, 10}, {15, 0, 10}};
    b.divisions = 4;
    b.rows = 1;
    retopo::GenResult r = retopo::generate(b, retopo::GenContext{});
    REQUIRE(r.ok());
    retopo::StitchResult st = retopo::stitchConstruct(m, r, 1, 1e-6, geo::axisPlane(0));
    // Columns at x = -5, 0, 5, 10, 15: the face beyond x = 0 is dropped, the other three mirror.
    CHECK(st.faces.size() == 6);
    double minX = 1e9;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            minX = std::min(minX, m.position(p).x);
    CHECK(minX == doctest::Approx(-15.0));
    CHECK(m.numPoints() == 14);
}

TEST_CASE("handles: a moved handle carries its region and fades to the border")
{
    geo::Mesh m = plane(100.0, 10);
    std::vector<Index> region = retopo::regionAlong(m, std::vector<glm::dvec3>{{0, 0, 0}}, 30.0);
    REQUIRE(!region.empty());
    retopo::HandleRig rig(m, region, std::vector<glm::dvec3>{{0, 0, 0}}, 0.5);
    retopo::HandleXform x;
    x.pivot = {0, 0, 0};
    x.translate = {0, 10, 0};
    std::vector<glm::dvec3> out = rig.deform(std::span(&x, 1));
    double maxUp = 0.0, borderUp = 0.0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        maxUp = std::max(maxUp, out[i].y);
        bool border = false;
        m.forEachPointEdge(rig.points()[i], [&](Index e) {
            Index q = m.edgeOtherPoint(e, rig.points()[i]);
            border |= std::find(region.begin(), region.end(), q) == region.end();
        });
        if (border)
            borderUp = std::max(borderUp, out[i].y);
    }
    CHECK(maxUp > 9.0);
    CHECK(borderUp < 1e-9);
}

TEST_CASE("commands: constructs regenerate, freeze after hand edits, undo")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    meshNode("ref", plane(200.0, 40));
    json strip = {{"knots", {P(-50, 0, 0), P(0, 0, 0), P(50, 0, 0)}}, {"width", 10.0}};
    json r = ops::execute("retopoConstruct", {{"mesh", "topo"}, {"reference", "ref"}, {"create", true}, {"kind", "strip"}, {"spec", strip}});
    int id = r["id"].get<int>();
    CHECK(id == 1);
    CHECK(r["faces"].get<int>() == 10);
    CHECK(outMesh("topo").numFaces() == 10);

    std::vector<retopo::ConstructRecord> recs = retopo::retopoConstructs(scene::Graph::current(), *scene::Graph::current().find("topo"));
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].live);
    CHECK(recs[0].counts == std::vector<int>{10, 1});

    strip["divisions"] = 4;
    strip["rows"] = 2;
    r = ops::execute("retopoConstruct", {{"mesh", "topo"}, {"kind", "strip"}, {"spec", strip}, {"id", id}});
    CHECK(r["faces"].get<int>() == 8);
    CHECK(outMesh("topo").numFaces() == 8);

    UndoStack::get().undo();
    CHECK(outMesh("topo").numFaces() == 10);
    UndoStack::get().redo();
    CHECK(outMesh("topo").numFaces() == 8);

    ops::execute("retopoDrawMove", {{"mesh", "topo"}, {"points", {0}}, {"positions", {P(-60, 0, 20)}}});
    recs = retopo::retopoConstructs(scene::Graph::current(), *scene::Graph::current().find("topo"));
    REQUIRE(recs.size() == 1);
    CHECK_FALSE(recs[0].live);
    r = ops::execute("retopoConstruct", {{"mesh", "topo"}, {"kind", "strip"}, {"spec", strip}, {"id", id}});
    CHECK(r["frozen"].get<bool>());
    CHECK(outMesh("topo").numFaces() == 8);
    CHECK(retopo::retopoConstructs(scene::Graph::current(), *scene::Graph::current().find("topo")).empty());
    CHECK_THROWS(ops::execute("retopoConstruct", {{"mesh", "topo"}, {"kind", "strip"}, {"spec", strip}, {"id", id}}));

    // A bad spec changes nothing.
    CHECK_THROWS(ops::execute("retopoConstruct", {{"mesh", "topo"}, {"kind", "bridge"}, {"spec", {{"a", {P(0, 0, 0)}}}}}));
    CHECK(outMesh("topo").numFaces() == 8);
}

TEST_CASE("commands: guides, delete and freeze, symmetry")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    meshNode("ref", plane(200.0, 40));
    ops::execute("retopoSetGuides", {{"mesh", "topo"}, {"reference", "ref"}, {"create", true},
                                     {"guides", {{{"points", {P(0, 0, 0), P(10, 0, 0)}}, {"color", {1.0, 0.0, 0.0}}}}}});
    std::vector<retopo::Guide> guides = retopo::retopoGuides(*scene::Graph::current().find("topo"));
    REQUIRE(guides.size() == 1);
    CHECK(guides[0].points.size() == 2);
    CHECK(guides[0].color.r == doctest::Approx(1.0f));

    scene::Graph::current().setValue({scene::Graph::current().find("topo")->id(), scene::MeshNode::kSymmetry, -1}, int64_t(1));
    json bridge = {{"a", {P(0, 0, 0), P(40, 0, 0)}}, {"b", {P(0, 0, 20), P(40, 0, 20)}}, {"divisions", 4}, {"rows", 2}};
    json r = ops::execute("retopoConstruct", {{"mesh", "topo"}, {"kind", "bridge"}, {"spec", bridge}});
    CHECK(r["faces"].get<int>() == 16);
    for (Index e = 0; e < outMesh("topo").edgeCapacity(); ++e)
        if (outMesh("topo").edgeValid(e))
            CHECK(outMesh("topo").edgeFaceCount(e) <= 2);

    json r2 = ops::execute("retopoConstruct", {{"mesh", "topo"}, {"kind", "stroke"}, {"spec", {{"path", {P(0, 0, 50), P(40, 0, 50)}}}}});
    CHECK(r2["faces"].get<int>() == 0);
    CHECK(retopo::retopoConstructs(scene::Graph::current(), *scene::Graph::current().find("topo")).size() == 2);
    ops::execute("retopoConstructDelete", {{"mesh", "topo"}, {"ids", {r2["id"].get<int>()}}});
    ops::execute("retopoConstructFreeze", {{"mesh", "topo"}});
    CHECK(retopo::retopoConstructs(scene::Graph::current(), *scene::Graph::current().find("topo")).empty());
    CHECK(outMesh("topo").numFaces() == 16);
}

TEST_CASE("commands: an auto region fills a hole and joins its border")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    geo::Mesh refMesh = plane(100.0, 50);
    meshNode("ref", refMesh);
    // The retopo mesh: a 10 x 10 grid of 10 cm quads with the middle 4 x 4 missing.
    geo::Mesh topo = plane(100.0, 10);
    std::vector<Index> hole;
    for (Index f = 0; f < topo.faceCapacity(); ++f)
        if (topo.faceValid(f))
        {
            glm::dvec3 c = topo.faceCentroid(f);
            if (std::abs(c.x) < 20.0 && std::abs(c.z) < 20.0)
                hole.push_back(f);
        }
    REQUIRE(hole.size() == 16);
    for (Index f : hole)
        topo.removeFace(f);
    topo.removeAllLoose();
    topo.compact();
    meshNode("topo", topo);
    ops::execute("retopoDrawSetReference", {{"mesh", "topo"}, {"reference", "ref"}});
    size_t borderBefore = borderEdges(outMesh("topo"));
    CHECK(borderBefore == 40 + 16);

    json faces = json::array();
    for (Index f = 0; f < refMesh.faceCapacity(); ++f)
        if (refMesh.faceValid(f))
        {
            glm::dvec3 c = refMesh.faceCentroid(f);
            if (std::abs(c.x) < 20.0 && std::abs(c.z) < 20.0)
                faces.push_back(f);
        }
    json border = json::array();
    for (int k = 0; k < 16; ++k)
    {
        // Round the hole: 4 edges per side, counter-clockwise from (-20, -20).
        int side = k / 4, t = k % 4;
        double u = -20.0 + 10.0 * t;
        glm::dvec3 p = side == 0 ? glm::dvec3(u, 0, -20) : side == 1 ? glm::dvec3(20, 0, u) : side == 2 ? glm::dvec3(-u, 0, 20) : glm::dvec3(-20, 0, -u);
        border.push_back(P(p.x, p.y, p.z));
    }
    json spec = {{"faces", faces}, {"referenceFaces", refMesh.numFaces()}, {"edgeLength", 10.0}, {"border", border}};
    json r = ops::execute("retopoConstruct", {{"mesh", "topo"}, {"kind", "region"}, {"spec", spec}});
    CHECK(r["faces"].get<int>() >= 8);
    CHECK(borderEdges(outMesh("topo")) == 40);
}
