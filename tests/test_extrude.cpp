#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Topology.h"
#include "scene/Graph.h"
#include "scene/Components.h"
#include "scene/LastOp.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace lila;
using json = nlohmann::json;
using geo::Index;
using namespace testutil;

namespace
{
/** Faces lying on top of another face: the same corner positions in any order. */
int coincidentFaces(const geo::Mesh& m)
{
    std::set<std::vector<std::array<long long, 3>>> seen;
    int dupes = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<std::array<long long, 3>> key;
        m.forEachFaceCorner(f, [&](Index c) {
            glm::dvec3 p = m.position(m.cornerPoint(c));
            key.push_back({std::llround(p.x * 1e4), std::llround(p.y * 1e4), std::llround(p.z * 1e4)});
        });
        std::sort(key.begin(), key.end());
        if (!seen.insert(key).second)
            ++dupes;
    }
    return dupes;
}

int zeroAreaFaces(const geo::Mesh& m)
{
    int n = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceArea(f) < 1e-9)
            ++n;
    return n;
}

Index faceFacing(const geo::Mesh& m, const glm::dvec3& dir)
{
    Index best = geo::kInvalid;
    double bestDot = -2.0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && glm::dot(m.faceNormal(f), dir) > bestDot)
        {
            bestDot = glm::dot(m.faceNormal(f), dir);
            best = f;
        }
    return best;
}
} // namespace

TEST_CASE("extrude: thickness pushes the cap out, offset insets it")
{
    geo::Mesh cube = geo::makeCube();
    const Index top = faceFacing(cube, {0, 1, 0});
    geo::ExtrudeOptions o;
    o.offset = 20.0;
    o.inset = 10.0;
    geo::ExtrudeResult r = geo::extrudeFaces(cube, std::vector<Index>{top}, o);
    CHECK(r.sides.size() == 4);
    CHECK(cube.numFaces() == 10);
    CHECK(cube.numPoints() == 12);
    CHECK(cube.faceArea(top) == doctest::Approx(80.0 * 80.0));
    CHECK(cube.bounds().max.y == doctest::Approx(70.0));
    CHECK(coincidentFaces(cube) == 0);
    CHECK(zeroAreaFaces(cube) == 0);
}

TEST_CASE("inset leaves no coincident or zero-area faces")
{
    geo::Mesh cube = geo::makeCube();
    geo::insetFaces(cube, std::vector<Index>{faceFacing(cube, {0, 1, 0})}, {.thickness = 10.0});
    CHECK(cube.numFaces() == 10);
    CHECK(coincidentFaces(cube) == 0);
    CHECK(zeroAreaFaces(cube) == 0);

    // A region of four faces: one inner region plus its rim.
    geo::Mesh grid = geo::makeCube({100, 100, 100, 2, 1, 2});
    std::vector<Index> tops;
    for (Index f = 0; f < grid.faceCapacity(); ++f)
        if (grid.faceValid(f) && grid.faceNormal(f).y > 0.9)
            tops.push_back(f);
    REQUIRE(tops.size() == 4);
    const Index before = grid.numFaces();
    geo::insetFaces(grid, tops, {.thickness = 5.0});
    CHECK(grid.numFaces() == before + 8);
    CHECK(coincidentFaces(grid) == 0);
    CHECK(zeroAreaFaces(grid) == 0);
}

TEST_CASE("polyInset and polyExtrude through the command and adjust-last-op path")
{
    scene::lastOp::install();
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    runUi("polyInset", {{"thickness", 5}});
    CHECK(meshOf(cube).numFaces() == 10);
    CHECK(coincidentFaces(meshOf(cube)) == 0);
    REQUIRE(scene::lastOp::live());
    scene::lastOp::rerun({{"thickness", 10}});
    scene::lastOp::rerun({{"thickness", 12}});
    CHECK(meshOf(cube).numFaces() == 10);
    CHECK(coincidentFaces(meshOf(cube)) == 0);

    run("newScene");
    cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    runUi("polyExtrude", {{"thickness", 20}, {"offset", 10}});
    const geo::Mesh& m = meshOf(cube);
    CHECK(m.numFaces() == 10);
    CHECK(coincidentFaces(m) == 0);
    REQUIRE(scene::lastOp::live());
    scene::lastOp::rerun({{"thickness", 30}});
    CHECK(meshOf(cube).numFaces() == 10);
    glm::dvec3 size = meshOf(cube).bounds().size();
    CHECK(size.x + size.y + size.z == doctest::Approx(330.0));
    run("undo");
    size = meshOf(cube).bounds().size();
    CHECK(size.x + size.y + size.z == doctest::Approx(320.0));
    run("undo");
    CHECK(meshOf(cube).numFaces() == 6);
}

TEST_CASE("polyExtrude duplicate puts the extruded faces in a new mesh")
{
    scene::lastOp::install();
    run("newScene");
    const std::string cube = run("polyCube")[0];
    run("move", {{"args", {cube}}, {"to", {0, 0, 300}}});
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    json made = runUi("polyExtrude", {{"thickness", 20}, {"duplicate", true}});
    REQUIRE(made.size() == 1);
    const std::string copy = made[0];
    CHECK(meshOf(cube).numFaces() == 6);
    CHECK(meshOf(copy).numFaces() == 1);
    scene::Graph& g = scene::Graph::current();
    CHECK(g.find(copy)->as<glm::dvec3>(scene::Transform::kTranslate).z == doctest::Approx(300.0));
    // The cap stays selected, so the handles and the last-op panel work on the copy.
    CHECK(scene::components::mode() == scene::ComponentMode::Face);
    REQUIRE(scene::components::selected(scene::ComponentMode::Face).size() == 1);
    CHECK(scene::components::selected(scene::ComponentMode::Face).begin()->first == g.find(copy)->id());

    REQUIRE(scene::lastOp::live());
    scene::lastOp::rerun({{"thickness", 30}, {"keepSides", true}});
    const auto& sel = scene::components::selected(scene::ComponentMode::Face);
    REQUIRE(sel.size() == 1);
    scene::Node* again = g.node(sel.begin()->first);
    REQUIRE(again);
    const std::string againPath = again->path();
    CHECK(meshOf(againPath).numFaces() == 5);
    CHECK(meshOf(cube).numFaces() == 6);
    // Back to the first setting, then past the extrude.
    run("undo");
    run("undo");
    CHECK(g.find(againPath) == nullptr);
    CHECK(meshOf(cube).numFaces() == 6);
}

TEST_CASE("adjust-last-op stays reachable through selection changes")
{
    scene::lastOp::install();
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    runUi("polyInset", {{"thickness", 5}});
    std::vector<scene::lastOp::Change> changes;
    int listener = scene::lastOp::addListener([&](scene::lastOp::Change c) { changes.push_back(c); });

    runUi("selectComponents", {{"args", {cube}}, {"faces", {1}}});
    runUi("selectComponents", {{"args", {cube}}, {"faces", {2}}});
    CHECK_FALSE(scene::lastOp::live());
    CHECK(scene::lastOp::reachable());
    CHECK(changes.empty());

    run("undo");
    CHECK(scene::lastOp::reachable());
    run("undo");
    CHECK(scene::lastOp::live());
    REQUIRE(changes.size() == 1);
    CHECK(changes.back() == scene::lastOp::Change::Restored);
    run("redo");
    run("redo");

    scene::lastOp::rerun({{"thickness", 10}});
    CHECK(scene::lastOp::live());
    CHECK(meshOf(cube).numFaces() == 10);
    CHECK(UndoStack::get().redoCount() == 0);
    run("undo");
    CHECK(scene::lastOp::live());
    CHECK(scene::lastOp::current()->call["thickness"] == 5);
    run("undo");
    CHECK(meshOf(cube).numFaces() == 6);
    CHECK_FALSE(scene::lastOp::reachable());
    CHECK(changes.back() == scene::lastOp::Change::Other);

    run("redo");
    CHECK(scene::lastOp::live());
    CHECK(changes.back() == scene::lastOp::Change::Restored);
    runUi("selectComponents", {{"args", {cube}}, {"faces", {3}}});
    run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}});
    CHECK_FALSE(scene::lastOp::reachable());
    CHECK(changes.back() == scene::lastOp::Change::Other);
    run("undo");
    CHECK(scene::lastOp::reachable());
    CHECK(changes.back() == scene::lastOp::Change::Restored);
    scene::lastOp::removeListener(listener);
}

TEST_CASE("polyInset on every face of a symmetric cube adds rims without duplicates")
{
    for (bool symmetric : {false, true})
        for (bool individual : {false, true})
        {
            run("newScene");
            std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"subdivisionsX", 2}, {"subdivisionsY", 2}, {"subdivisionsZ", 2}})[0];
            if (symmetric)
                run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
            const int count = int(meshOf(cube).numFaces());
            for (int f = 0; f < count; ++f)
            {
                CAPTURE(f);
                CAPTURE(symmetric);
                CAPTURE(individual);
                run("selectComponents", {{"args", {cube}}, {"faces", json::array({f, (f + 1) % count})}});
                run("polyInset", {{"thickness", 5}, {"individual", individual}});
                CHECK(coincidentFaces(meshOf(cube)) == 0);
                CHECK(zeroAreaFaces(meshOf(cube)) == 0);
                run("undo");
            }
        }
}

TEST_CASE("polyBevel on faces bevels the border of the selection, not the edges inside it")
{
    run("newScene");
    std::string cyl = run("polyCylinder", {{"radius", 50.0}, {"height", 100.0}, {"subdivisionsAxis", 16}, {"subdivisionsCaps", 3}})[0];
    const geo::Mesh before = meshOf(cyl);
    std::vector<uint32_t> top;
    for (Index f = 0; f < before.faceCapacity(); ++f)
        if (before.faceValid(f) && before.faceNormal(f).y > 0.99)
            top.push_back(f);
    REQUIRE(top.size() > 16);
    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {cyl}}, {"faces", top}});
    run("polyBevel", {{"offset", 3.0}});
    const geo::Mesh& m = meshOf(cyl);
    CHECK(m.validate(geo::kValidateDefault | geo::kValidateDegenerate).empty());
    // One ring of chamfer faces round the rim; the cap's own faces keep their count.
    CHECK(m.numFaces() == before.numFaces() + 16);
    int capFaces = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceNormal(f).y > 0.99)
            ++capFaces;
    CHECK(capFaces == int(top.size()));
}

TEST_CASE("polyBevel offset adjusts through adjust-last-op and keeps edge mode")
{
    scene::lastOp::install();
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("selectMode", {{"args", {"edge"}}});
    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    runUi("polyBevel", {{"offset", 2}});
    auto spread = [&] {
        double lo = 1e9;
        const geo::Mesh& m = meshOf(cube);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                for (int i = 0; i < 3; ++i)
                    if (std::abs(m.position(p)[i]) > 1e-6 && std::abs(m.position(p)[i]) < 49.9)
                        lo = std::min(lo, std::abs(m.position(p)[i]));
        return lo;
    };
    CHECK(spread() == doctest::Approx(48.0));
    CHECK(scene::components::mode() == scene::ComponentMode::Edge);
    REQUIRE(scene::lastOp::live());
    scene::lastOp::rerun({{"offset", 10}});
    CHECK(spread() == doctest::Approx(40.0));
    // Past what the cube allows, the recorded offset stops at the clamp: the faces beside the edge close entirely.
    scene::lastOp::rerun({{"offset", 500}});
    const double limit = scene::lastOp::current()->call["offset"].get<double>();
    CHECK(limit == doctest::Approx(100.0));
    CHECK(meshOf(cube).validate(geo::kValidateDefault | geo::kValidateDegenerate).empty());
    CHECK(meshOf(cube).numFaces() == 5);
    run("undo");
    CHECK(spread() == doctest::Approx(40.0));
    run("undo");
    run("undo");
    CHECK(scene::components::mode() == scene::ComponentMode::Edge);
    CHECK(scene::components::selected(scene::Graph::current().find(cube)->id(), scene::ComponentMode::Edge) == std::vector<uint32_t>{0});
}

TEST_CASE("edits run in a component mode stay in it")
{
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    run("selectMode", {{"args", {"face"}}});
    run("selectComponentsWhere", {{"args", {cube}}, {"kind", "face"}, {"extreme", "+z"}});
    run("selectComponentsWhere", {{"args", {cube}}, {"kind", "face"}, {"extreme", "-z"}, {"add", true}});
    run("deleteComponents");
    run("selectComponentsWhere", {{"args", {cube}}, {"kind", "edge"}, {"border", true}});
    REQUIRE(scene::components::mode() == scene::ComponentMode::Edge);
    run("polyBridge");
    CHECK(meshOf(cube).numFaces() == 8);
    CHECK(scene::components::mode() == scene::ComponentMode::Edge);
    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    run("polyCollapse");
    CHECK(scene::components::mode() == scene::ComponentMode::Edge);
}

TEST_CASE("polyTargetWeld: vertex into target, halfway, edges, and across symmetry")
{
    auto hasPoint = [](const geo::Mesh& m, const glm::dvec3& p) {
        for (Index i = 0; i < m.pointCapacity(); ++i)
            if (m.pointValid(i) && glm::distance(m.position(i), p) < 1e-6)
                return true;
        return false;
    };
    run("newScene");
    std::string cube = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    const geo::Mesh before = meshOf(cube);
    const Index a = before.edgePoint(0, 0), b = before.edgePoint(0, 1);
    run("polyTargetWeld", {{"mesh", cube}, {"source", a}, {"target", b}});
    CHECK(meshOf(cube).numPoints() == 7);
    CHECK(hasPoint(meshOf(cube), before.position(b)));
    CHECK_FALSE(hasPoint(meshOf(cube), before.position(a)));
    run("undo");
    CHECK(meshOf(cube).numPoints() == 8);

    run("polyTargetWeld", {{"mesh", cube}, {"source", a}, {"target", b}, {"center", true}});
    CHECK(hasPoint(meshOf(cube), (before.position(a) + before.position(b)) * 0.5));
    run("undo");

    Index apart = geo::kInvalid;
    for (Index e = 1; e < before.edgeCapacity() && apart == geo::kInvalid; ++e)
        if (before.edgeValid(e) && before.edgePoint(e, 0) != a && before.edgePoint(e, 0) != b && before.edgePoint(e, 1) != a &&
            before.edgePoint(e, 1) != b)
            apart = e;
    REQUIRE(apart != geo::kInvalid);
    run("polyTargetWeld", {{"mesh", cube}, {"source", 0}, {"target", apart}, {"edge", true}});
    CHECK(meshOf(cube).numPoints() == 6);
    run("undo");

    // With X symmetry, welding a +X vertex to its neighbour welds the -X pair too; welding across the plane meets on it.
    run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
    Index px = geo::kInvalid, nx = geo::kInvalid, py = geo::kInvalid;
    for (Index i = 0; i < before.pointCapacity(); ++i)
    {
        const glm::dvec3 p = before.position(i);
        if (p.x > 0 && p.y > 0 && p.z > 0)
            px = i;
        if (p.x < 0 && p.y > 0 && p.z > 0)
            nx = i;
        if (p.x > 0 && p.y < 0 && p.z > 0)
            py = i;
    }
    run("polyTargetWeld", {{"mesh", cube}, {"source", px}, {"target", py}});
    CHECK(meshOf(cube).numPoints() == 6);
    run("undo");
    run("polyTargetWeld", {{"mesh", cube}, {"source", px}, {"target", nx}});
    CHECK(meshOf(cube).numPoints() == 7);
    CHECK(hasPoint(meshOf(cube), glm::dvec3(0.0, before.position(px).y, before.position(px).z)));
}

/** Points without a partner mirrored across x = 0. */
static int unmirrored(const geo::Mesh& m)
{
    std::set<std::array<long long, 3>> at;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            at.insert({std::llround(m.position(p).x * 1e3), std::llround(m.position(p).y * 1e3), std::llround(m.position(p).z * 1e3)});
    int n = 0;
    for (const auto& k : at)
        n += !at.contains({-k[0], k[1], k[2]});
    return n;
}

TEST_CASE("polyGridFill with symmetry fills both sides alike")
{
    for (double size : {0.4, 1.0})
        for (const char* shape : {"side", "top", "fan"})
        {
            CAPTURE(size);
            CAPTURE(shape);
            run("newScene");
            std::string cube = run("polyCube", {{"width", 120.0}, {"height", 120.0}, {"depth", 120.0}, {"subdivisionsX", 6}, {"subdivisionsY", 6},
                                                {"subdivisionsZ", 6}})[0];
            run("setSymmetry", {{"args", {cube}}, {"axis", "x"}});
            const geo::Mesh& m = meshOf(cube);
            std::vector<Index> faces;
            Index side = geo::kInvalid;
            for (Index f = 0; f < m.faceCapacity(); ++f)
            {
                if (!m.faceValid(f))
                    continue;
                glm::dvec3 c{0.0};
                m.forEachFaceCorner(f, [&](Index fc) { c += m.position(m.cornerPoint(fc)) / 4.0; });
                if (std::string(shape) == "top" && c.y > 59 && c.x > 10 && c.x < 50 && c.z > -30 && c.z < 30)
                    faces.push_back(f);
                if (std::string(shape) == "side" && c.x > 59 && c.y > -20 && c.y < 40 && c.z > -40 && c.z < 20)
                    faces.push_back(f);
                if (c.x > 59 && std::abs(c.y - 10) < 1 && std::abs(c.z - 10) < 1)
                    side = f;
            }
            if (std::string(shape) == "fan")
            {
                REQUIRE(side != geo::kInvalid);
                run("selectMode", {{"args", {"face"}}});
                run("selectComponents", {{"args", {cube}}, {"faces", {side}}});
                run("polyPoke");
                REQUIRE(unmirrored(meshOf(cube)) == 0);
                const geo::Mesh& poked = meshOf(cube);
                Index centre = geo::kInvalid;
                for (Index p = 0; p < poked.pointCapacity(); ++p)
                    if (poked.pointValid(p) && glm::distance(poked.position(p), glm::dvec3(60, 10, 10)) < 1e-6)
                        centre = p;
                REQUIRE(centre != geo::kInvalid);
                run("selectMode", {{"args", {"vertex"}}});
                run("selectComponents", {{"args", {cube}}, {"vertices", {centre}}});
            }
            else
            {
                REQUIRE(!faces.empty());
                run("selectMode", {{"args", {"face"}}});
                run("selectComponents", {{"args", {cube}}, {"faces", faces}});
            }
            run("polyGridFill", {{"gridSize", size}});
            CHECK(unmirrored(meshOf(cube)) == 0);
            CHECK(zeroAreaFaces(meshOf(cube)) == 0);
        }
}

/** Points without a partner mirrored across y = 0. */
static int unmirroredY(const geo::Mesh& m)
{
    std::set<std::array<long long, 3>> at;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            at.insert({std::llround(m.position(p).x * 1e3), std::llround(m.position(p).y * 1e3), std::llround(m.position(p).z * 1e3)});
    int n = 0;
    for (const auto& k : at)
        n += !at.contains({k[0], -k[1], k[2]});
    return n;
}

TEST_CASE("polyGridFill with symmetry fills round mirrored fans alike")
{
    for (int sides : {12, 16, 20, 13})
        for (double size : {0.4, 1.0})
        {
            CAPTURE(sides);
            CAPTURE(size);
            run("newScene");
            std::string cyl = run("polyCylinder", {{"radius", 50.0}, {"height", 100.0}, {"subdivisionsAxis", sides}, {"capType", "fan"}})[0];
            run("setSymmetry", {{"args", {cyl}}, {"axis", "y"}});
            const geo::Mesh& m = meshOf(cyl);
            Index top = geo::kInvalid;
            for (Index p = 0; p < m.pointCapacity(); ++p)
                if (m.pointValid(p) && glm::distance(m.position(p), glm::dvec3(0, 50, 0)) < 1e-6)
                    top = p;
            REQUIRE(top != geo::kInvalid);
            run("selectMode", {{"args", {"vertex"}}});
            run("selectComponents", {{"args", {cyl}}, {"vertices", {top}}});
            run("polyGridFill", {{"gridSize", size}});
            CHECK(unmirroredY(meshOf(cyl)) == 0);
            CHECK(zeroAreaFaces(meshOf(cyl)) == 0);
            CHECK(coincidentFaces(meshOf(cyl)) == 0);
        }
}

TEST_CASE("polyGridFill radial rings a cap in quads, mirrored")
{
    for (int sides : {8, 12, 13, 16, 20, 22, 32, 48, 64})
    {
        CAPTURE(sides);
        run("newScene");
        std::string cyl = run("polyCylinder", {{"radius", 50.0}, {"height", 100.0}, {"subdivisionsAxis", sides}, {"capType", "fan"}})[0];
        run("setSymmetry", {{"args", {cyl}}, {"axis", "y"}});
        const geo::Mesh& m = meshOf(cyl);
        Index top = geo::kInvalid;
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && glm::distance(m.position(p), glm::dvec3(0, 50, 0)) < 1e-6)
                top = p;
        REQUIRE(top != geo::kInvalid);
        run("selectMode", {{"args", {"vertex"}}});
        run("selectComponents", {{"args", {cyl}}, {"vertices", {top}}});
        run("polyGridFill", {{"style", "radial"}});
        const geo::Mesh& filled = meshOf(cyl);
        CHECK(unmirroredY(filled) == 0);
        CHECK(zeroAreaFaces(filled) == 0);
        CHECK(coincidentFaces(filled) == 0);
        CHECK(geo::boundaryLoops(filled).empty());
        int triangles = 0, quads = 0, flipped = 0;
        for (Index f = 0; f < filled.faceCapacity(); ++f)
        {
            if (!filled.faceValid(f))
                continue;
            triangles += filled.faceSize(f) == 3;
            quads += filled.faceSize(f) == 4;
            glm::dvec3 c{0.0}, nrm{0.0};
            std::vector<glm::dvec3> pts;
            filled.forEachFaceCorner(f, [&](Index fc) { pts.push_back(filled.position(filled.cornerPoint(fc))); });
            for (size_t i = 0; i < pts.size(); ++i)
                nrm += glm::cross(pts[i], pts[(i + 1) % pts.size()]);
            for (const glm::dvec3& q : pts)
                c += q / double(pts.size());
            // Cap faces face away from the cylinder's middle.
            if (std::abs(c.y) > 49.0 && nrm.y * c.y <= 0.0)
                ++flipped;
        }
        CHECK(flipped == 0);
        // Symmetry fills both caps: quads but for an odd border's one triangle each.
        CHECK(triangles == (sides % 2 == 1 ? 2 : 0));
        if (sides >= 32)
            CHECK(quads > sides * 5); // rings, not one ring round a grid
    }
}
