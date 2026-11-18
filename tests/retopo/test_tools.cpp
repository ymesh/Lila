#include "TestUtil.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Bvh.h"
#include "geo/ObjIO.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "geo/TestUtil.h"
#include "io/SceneFile.h"
#include "retopo/Retopo.h"
#include "retopo/RetopoCommands.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace lila;
using namespace retopotest;
using geo::Index;

namespace
{
const geo::Mesh& toolOutMesh(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    return *g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}
} // namespace

TEST_CASE("quad remesh: symmetry is exact")
{
    geo::Mesh src = sphere(96);
    retopo::QuadRemeshOptions o;
    o.targetFaces = 1500;
    o.symmetryAxis = 0;
    retopo::QuadRemeshResult r = retopo::quadRemesh(src, o);
    REQUIRE(r.ok());
    report("symmetric sphere", r);
    if (std::getenv("LILA_RETOPO_DUMP"))
        geo::writeObjFile((std::filesystem::temp_directory_path() / "retopo_symmetric.obj").string(), r.mesh);
    CHECK(r.metrics.quadPercent > 95.0);
    CHECK(geo::test::boundaryEdgeCount(r.mesh) == 0);
    CHECK(r.mesh.validate().empty());

    geo::SymmetryPlane plane;
    geo::SymmetryMap map = geo::SymmetryMap::build(r.mesh, plane, 1e-12);
    CHECK(map.unmatchedCount() == 0);
    Index exact = 0;
    for (Index p = 0; p < r.mesh.pointCapacity(); ++p)
    {
        Index q = map.mirrorPoint(p);
        if (q != geo::kInvalid && r.mesh.position(q) == plane.reflect(r.mesh.position(p)))
            ++exact;
    }
    CHECK(exact == r.mesh.numPoints());
}

TEST_CASE("relax keeps points on the surface and pins the boundary")
{
    geo::Mesh target = sphere(96);
    geo::MeshBvh bvh(target);
    geo::QuadSphereOptions qs;
    qs.subdiv = 8;
    geo::Mesh m = geo::makeQuadSphere(qs);
    // Open it up so it has a border, then push the inner points off the surface.
    std::vector<Index> top;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceCentroid(f).y > 40.0)
            top.push_back(f);
    geo::deleteFaces(m, top);
    m.compact();
    std::vector<glm::dvec3> border;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        glm::dvec3 at = m.position(p);
        if (m.isBoundaryPoint(p))
            border.push_back(at);
        else
            m.setPosition(p, at + glm::dvec3(std::sin(p * 1.7), std::cos(p * 2.3), std::sin(p * 0.7)) * 2.0);
    }
    retopo::RelaxOptions ro;
    ro.iterations = 20;
    retopo::relaxMesh(m, &bvh, ro);
    double worst = 0.0;
    size_t b = 0;
    bool pinned = true;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        geo::BvhClosest c;
        REQUIRE(bvh.closestPoint(m.position(p), c));
        if (m.isBoundaryPoint(p))
            pinned &= m.position(p) == border[b++];
        else
            worst = std::max(worst, c.distance);
    }
    CHECK(worst < 1e-6);
    CHECK(pinned);

    geo::QuadSphereOptions big;
    big.radius = 60.0;
    big.subdiv = 6;
    geo::Mesh wrap = geo::makeQuadSphere(big);
    retopo::shrinkwrap(wrap, bvh, 3);
    worst = 0.0;
    for (Index p = 0; p < wrap.pointCapacity(); ++p)
    {
        geo::BvhClosest c;
        bvh.closestPoint(wrap.position(p), c);
        worst = std::max(worst, c.distance);
    }
    CHECK(worst < 1e-6);
}

TEST_CASE("repair fixes a broken mesh")
{
    // Unwelded neighbours, a duplicate face, a fin making an edge non-manifold, a flipped face and a sliver.
    std::vector<glm::dvec3> pts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                   {1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0},
                                   {1, 0, 0}, {1, 1, 0}, {1, 0.5, 1},
                                   {2, 0, 0}, {3, 0, 0}, {3, 1, 0}, {2, 1, 0},
                                   {0, 0, 0}, {0.5, 0, 0}, {1, 0, 0}};
    std::vector<Index> sizes = {4, 4, 4, 3, 4, 3};
    std::vector<Index> idx = {0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 8, 9, 10, 11, 14, 13, 12, 15, 16, 17};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    retopo::RepairReport r = retopo::repairMesh(m);
    MESSAGE(r.summary());
    using retopo::MeshIssue;
    CHECK(r.fixed[size_t(MeshIssue::Coincident)] > 0);
    CHECK(r.fixed[size_t(MeshIssue::Lamina)] == 1);
    CHECK(r.fixed[size_t(MeshIssue::ZeroAreaFace)] >= 1);
    CHECK(r.fixed[size_t(MeshIssue::NonManifoldEdge)] == 1);
    CHECK(geo::test::isValid(m, geo::kValidateDefault | geo::kValidateDegenerate));
    CHECK(m.numFaces() == 4);
}

TEST_CASE("voxel remesh closes holes")
{
    geo::CubeOptions co;
    co.subdivX = co.subdivY = co.subdivZ = 10;
    geo::Mesh m = geo::makeCube(co);
    // A 10 cm hole, under the two voxels the remesher closes over at this resolution.
    std::vector<Index> one = {0};
    geo::deleteFaces(m, one);
    m.compact();
    REQUIRE(geo::test::boundaryEdgeCount(m) > 0);
    retopo::VoxelOptions vo;
    vo.resolution = 16;
    geo::Mesh v = retopo::voxelRemesh(m, vo);
    CHECK(v.numFaces() > 300);
    CHECK(geo::test::boundaryEdgeCount(v) == 0);
    CHECK(geo::test::isValid(v));
    CHECK(geo::test::signedVolume(v) > 0.8e6);
}

TEST_CASE("commands: quadRemesh, relaxMesh, shrinkwrap, meshRepair undo cleanly")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    makeMeshNode("src", sphere(64));
    json r = ops::execute("quadRemesh", {{"args", {"src"}}, {"targetFaces", 600}, {"symmetry", "x"}, {"transferUvs", true}});
    std::string name = r["name"].get<std::string>();
    CHECK(r["metrics"]["quadPercent"].get<double>() > 95.0);
    REQUIRE(scene::Graph::current().find(name));
    CHECK(toolOutMesh(name).cornerAttrs().has("uv0"));
    ops::execute("relaxMesh", {{"args", {name}}, {"target", "src"}, {"iterations", 3}});
    ops::execute("shrinkwrap", {{"args", {name}}, {"target", "src"}});
    ops::execute("meshRepair", {{"args", {name}}});
    for (int i = 0; i < 4; ++i)
        UndoStack::get().undo();
    CHECK(scene::Graph::current().find(name) == nullptr);
}

TEST_CASE("commands: quadRemesh takes density handles, feature guides, a method, and reports poles")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    makeMeshNode("src", sphere(64));
    json found = ops::execute("retopoPoles", {{"args", {"src"}}, {"targetFaces", 600}});
    json r = ops::execute("quadRemesh", {{"args", {"src"}},
                                         {"targetFaces", 600},
                                         {"densityHandles", {{{"position", {50.0, 0.0, 0.0}}, {"radius", 30.0}, {"scale", 0.6}}}},
                                         {"featureGuidePoints", {{{-50.0, 0.0, 0.0}, {0.0, 0.0, 50.0}}}},
                                         {"poles", found["poles"]}});
    CHECK(r["layout"].is_string());
    REQUIRE(r["poles"].is_array());
    CHECK(r["poles"].size() == found["poles"].size());
    CHECK(std::abs(r["metrics"]["faces"].get<double>() / 600.0 - 1.0) < 0.25);

    json grid = ops::execute("quadRemesh", {{"args", {"src"}}, {"targetFaces", 300}, {"method", "grid"}});
    CHECK(grid["layout"] != "grid");
    CHECK_THROWS(ops::execute("quadRemesh", {{"args", {"src"}}, {"method", "lattice"}}));
}

TEST_CASE("commands: retopo draw edits")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    auto P = [](double x, double y) { return json::array({x, y, 0.0}); };
    ops::execute("retopoDrawAddFace", {{"mesh", "retopo"}, {"create", true}, {"points", {P(0, 0), P(10, 0), P(10, 10), P(0, 10)}}});
    // Shares the first quad's right edge in the same winding, so it gets flipped to match.
    ops::execute("retopoDrawAddFace", {{"mesh", "retopo"}, {"points", {P(10, 10), P(20, 10), P(20, 0), P(10, 0)}}, {"weld", 0.5}});
    {
        const geo::Mesh& m = toolOutMesh("retopo");
        CHECK(m.numPoints() == 6);
        CHECK(m.numFaces() == 2);
        CHECK(geo::test::isValid(m));
    }
    ops::execute("retopoDrawInsertLoop", {{"mesh", "retopo"}, {"edge", int(toolOutMesh("retopo").findEdge(0, 3))}});
    CHECK(toolOutMesh("retopo").numFaces() == 4);
    ops::execute("retopoDrawMove", {{"mesh", "retopo"}, {"points", {0}}, {"positions", {P(-1, -1)}}});
    CHECK(toolOutMesh("retopo").position(0) == glm::dvec3(-1, -1, 0));
    ops::execute("retopoDrawDelete", {{"mesh", "retopo"}, {"faces", {0}}});
    CHECK(toolOutMesh("retopo").numFaces() == 3);

    // With x symmetry each new face gets a welded mirror image.
    scene::Graph& g = scene::Graph::current();
    ops::execute("retopoDrawAddFace", {{"mesh", "sym"}, {"create", true}, {"points", {P(0, 0), P(10, 0), P(10, 10), P(0, 10)}}});
    g.setValue({g.find("sym")->id(), scene::MeshNode::kSymmetry, -1}, int64_t(1));
    ops::execute("retopoDrawAddFace", {{"mesh", "sym"}, {"points", {P(10, 0), P(20, 0), P(20, 10), P(10, 10)}}});
    {
        const geo::Mesh& m = toolOutMesh("sym");
        CHECK(m.numFaces() == 3);
        CHECK(geo::test::isValid(m));
    }
    scene::Graph::current().clear();
    UndoStack::get().clear();
}

TEST_CASE("commands: retopo draw move keeps a face across the symmetry plane mirrored")
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    UndoStack::get().clear();
    auto P = [](double x, double y) { return json::array({x, y, 0.0}); };
    ops::execute("retopoDrawAddFace", {{"mesh", "sym"}, {"create", true}, {"points", {P(-10, 0), P(10, 0), P(10, 10), P(-10, 10)}}});
    g.setValue({g.find("sym")->id(), scene::MeshNode::kSymmetry, -1}, int64_t(1));

    // Every corner shifted +5 in x, the grabbed left side listed first: the right side mirrors it instead.
    const geo::Mesh before = toolOutMesh("sym");
    json points = json::array(), positions = json::array();
    for (double side : {-10.0, 10.0})
        for (geo::Index p = 0; p < before.pointCapacity(); ++p)
            if (before.pointValid(p) && before.position(p).x == side)
            {
                points.push_back(p);
                positions.push_back(P(side + 5.0, before.position(p).y));
            }
    ops::execute("retopoDrawMove", {{"mesh", "sym"}, {"points", points}, {"positions", positions}});
    const geo::Mesh& m = toolOutMesh("sym");
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            CHECK(std::abs(m.position(p).x) == doctest::Approx(5.0));
    g.clear();
    UndoStack::get().clear();
}

TEST_CASE("commands: retopo draw insert loop puts the new points on the reference")
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    UndoStack::get().clear();
    const geo::Mesh ball = sphere(32);
    makeMeshNode("ref", ball);
    g.setValue({g.find("ref")->id(), scene::MeshNode::kSymmetry, -1}, int64_t(1));
    geo::MeshBvh bvh(ball);
    auto offSurface = [&](const glm::dvec3& p) {
        geo::BvhClosest c;
        REQUIRE(bvh.closestPoint(p, c));
        return glm::length(c.point - p);
    };
    // A quad on the front of the 50 cm sphere, to one side of the symmetry plane.
    auto S = [](double x, double y) {
        const glm::dvec3 p = glm::normalize(glm::dvec3(x, y, 1.0)) * 50.0;
        return json::array({p.x, p.y, p.z});
    };
    ops::execute("retopoDrawAddFace", {{"mesh", "ref_retopo"}, {"reference", "ref"}, {"create", true},
                                       {"points", {S(0.1, -0.4), S(0.9, -0.4), S(0.9, 0.4), S(0.1, 0.4)}}});
    const geo::Mesh before = toolOutMesh("ref_retopo");
    REQUIRE(before.numFaces() == 2);
    REQUIRE(before.numPoints() == 8);
    auto existed = [&](const glm::dvec3& p) {
        for (Index q = 0; q < before.pointCapacity(); ++q)
            if (before.pointValid(q) && glm::length(before.position(q) - p) < 1e-9)
                return true;
        return false;
    };

    ops::execute("retopoDrawInsertLoop", {{"mesh", "ref_retopo"}, {"edge", int(before.findEdge(0, 1))}});
    const geo::Mesh& m = toolOutMesh("ref_retopo");
    CHECK(m.numFaces() == 4);
    CHECK(geo::test::isValid(m));
    int added = 0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && !existed(m.position(p)))
        {
            ++added;
            CAPTURE(p);
            // The middle of a straight edge is centimetres under the surface.
            CHECK(offSurface(m.position(p)) < 1e-6);
        }
    // Two on the drawn quad, two on its mirror image, and the two sides still mirror each other.
    CHECK(added == 4);
    double sumX = 0.0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            sumX += m.position(p).x;
    CHECK(sumX == doctest::Approx(0.0).epsilon(1e-6));

    UndoStack::get().undo();
    CHECK(toolOutMesh("ref_retopo").numFaces() == 2);
    g.clear();
    UndoStack::get().clear();
}

TEST_CASE("commands: retopo draw dots undo, and fill consumes dots in one step")
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    UndoStack::get().clear();
    makeMeshNode("ref", sphere(16));
    g.setValue({g.find("ref")->id(), scene::MeshNode::kSymmetry, -1}, int64_t(1));
    UndoStack::get().clear();
    auto P = [](double x, double y) { return glm::dvec3(x, y, 0.0); };
    auto J = [](const std::vector<glm::dvec3>& ps) {
        json out = json::array();
        for (const glm::dvec3& p : ps)
            out.push_back({p.x, p.y, p.z});
        return out;
    };
    auto dots = [&] { return retopo::retopoDrawDots(*g.find("ref_retopo")); };
    auto setDots = [&](const std::vector<glm::dvec3>& ps) {
        ops::execute("retopoDrawSetDots", {{"mesh", "ref_retopo"}, {"reference", "ref"}, {"create", true}, {"dots", J(ps)}});
    };

    // The first dot creates the retopo mesh, connected to its reference and taking its symmetry.
    setDots({P(10, 0)});
    REQUIRE(g.find("ref_retopo"));
    CHECK(retopo::retopoDrawReference(g, g.find("ref_retopo")->id()) == g.find("ref")->id());
    CHECK(g.find("ref_retopo")->as<int64_t>(scene::MeshNode::kSymmetry) == 1);
    setDots({P(10, 0), P(20, 0)});
    setDots({P(10, 0), P(20, 0), P(20, 10)});
    setDots({P(10, 0), P(25, 0), P(20, 10)}); // a moved dot
    setDots({P(10, 0), P(25, 0)}); // a deleted dot
    CHECK(dots().size() == 2);
    UndoStack::get().undo();
    CHECK(dots() == std::vector<glm::dvec3>{P(10, 0), P(25, 0), P(20, 10)});
    UndoStack::get().undo();
    CHECK(dots() == std::vector<glm::dvec3>{P(10, 0), P(20, 0), P(20, 10)});
    UndoStack::get().redo();
    UndoStack::get().redo();
    CHECK(dots().size() == 2);

    // Dots are stored in object space, so they follow the mesh.
    g.setValue({g.find("ref_retopo")->id(), scene::Transform::kTranslate, -1}, glm::dvec3(0, 0, 5));
    g.evaluate();
    CHECK(dots()[0] == glm::dvec3(10, 0, 5));
    UndoStack::get().undo();
    g.evaluate();

    setDots({P(10, 0), P(20, 0), P(20, 10), P(10, 10), P(40, 40)});
    {
        UndoChunk chunk("Retopo Draw Fill");
        ops::execute("retopoDrawAddFace", {{"mesh", "ref_retopo"}, {"reference", "ref"}, {"create", true},
                                          {"points", J({P(10, 0), P(20, 0), P(20, 10), P(10, 10)})}});
        setDots({P(40, 40)});
    }
    CHECK(toolOutMesh("ref_retopo").numFaces() == 2); // with its x mirror
    CHECK(dots().size() == 1);
    UndoStack::get().undo();
    CHECK(toolOutMesh("ref_retopo").numFaces() == 0);
    CHECK(dots().size() == 5);

    // Undoing back past the first dot removes the mesh again.
    while (UndoStack::get().canUndo())
        UndoStack::get().undo();
    CHECK(g.find("ref_retopo") == nullptr);
    g.clear();
    UndoStack::get().clear();
}

TEST_CASE("retopo draw resumes on its retopo mesh after save and load")
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    UndoStack::get().clear();
    makeMeshNode("body", sphere(16));
    makeMeshNode("other", sphere(8));
    auto P = [](double x, double y) { return json::array({x, y, 0.0}); };
    ops::execute("retopoDrawAddFace", {{"mesh", "body_retopo"}, {"reference", "body"}, {"create", true},
                                      {"points", {P(0, 0), P(10, 0), P(10, 10), P(0, 10)}}});
    ops::execute("retopoDrawSetDots", {{"mesh", "body_retopo"}, {"dots", {P(30, 0)}}});
    g.rename(g.find("body_retopo")->id(), "topo");

    for (const char* ext : {".lila", ".lilaa"})
    {
        auto path = std::filesystem::temp_directory_path() / (std::string("lila_retopodraw_resume") + ext);
        io::save(path);
        g.clear();
        io::load(path);
        std::filesystem::remove(path);
        g.evaluate();

        scene::NodeId body = g.find("body")->id(), topo = g.find("topo")->id(), other = g.find("other")->id();
        CHECK(toolOutMesh("topo").numFaces() == 1);
        REQUIRE(retopo::retopoDrawDots(*g.find("topo")).size() == 1);
        CHECK(retopo::retopoDrawDots(*g.find("topo"))[0] == glm::dvec3(30, 0, 0));

        auto resolve = [&](std::vector<scene::NodeId> sel) {
            auto t = retopo::resolveRetopoDraw(g, sel);
            REQUIRE(t);
            return std::pair{t->reference, t->retopo};
        };
        CHECK(resolve({topo}) == std::pair{body, std::string("topo")});
        CHECK(resolve({body}) == std::pair{body, std::string("topo")});
        CHECK(resolve({body, topo}) == std::pair{body, std::string("topo")});
        CHECK(resolve({topo, body}) == std::pair{body, std::string("topo")});
        CHECK(resolve({other}) == std::pair{other, std::string("other_retopo")});
        CHECK(resolve({other, topo}) == std::pair{body, std::string("topo")});
        CHECK_FALSE(retopo::resolveRetopoDraw(g, {}));
    }

    SUBCASE("files with the quadDraw attr names still load")
    {
        auto path = std::filesystem::temp_directory_path() / "lila_retopodraw_oldnames.lilaa";
        io::save(path);
        std::string text;
        {
            std::ifstream in(path, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), {});
        }
        for (auto [from, to] : {std::pair{"retopoDrawReference", "quadDrawReference"}, std::pair{"retopoDrawDots", "quadDrawDots"}})
            for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at))
                text.replace(at, std::strlen(from), to);
        REQUIRE(text.find("quadDrawReference") != std::string::npos);
        std::ofstream(path, std::ios::binary) << text;
        g.clear();
        io::load(path);
        std::filesystem::remove(path);
        REQUIRE(retopo::retopoDrawDots(*g.find("topo")).size() == 1);
        CHECK(retopo::retopoDrawReference(g, g.find("topo")->id()) == g.find("body")->id());
    }

    // A "<name>_retopo" drawn before the pair was connected resumes on its source mesh.
    makeMeshNode("other_retopo", geo::Mesh());
    CHECK(retopo::resolveRetopoDraw(g, {g.find("other_retopo")->id()})->reference == g.find("other")->id());

    // Retargeting the surface is one undoable step.
    ops::execute("retopoDrawSetReference", {{"mesh", "topo"}, {"reference", "other"}});
    CHECK(retopo::retopoDrawReference(g, g.find("topo")->id()) == g.find("other")->id());
    UndoStack::get().undo();
    CHECK(retopo::retopoDrawReference(g, g.find("topo")->id()) == g.find("body")->id());
    CHECK_THROWS_AS(ops::execute("retopoDrawSetReference", {{"mesh", "topo"}, {"reference", "topo"}}), CommandError);
    CHECK_THROWS_AS(ops::execute("retopoDrawSetReference", {{"mesh", "missing"}, {"reference", "other"}}), CommandError);
    g.clear();
    UndoStack::get().clear();
}

TEST_CASE("retopo draw: a new retopo mesh gets a draw offset sized to its reference")
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    makeMeshNode("ref", geo::makeCube({200.0, 200.0, 200.0}));
    makeMeshNode("plain", geo::Mesh());
    CHECK(g.find("plain")->as<double>(scene::MeshNode::kDrawOffset) == 0.0);

    ops::execute("retopoDrawSetReference", {{"mesh", "ref_retopo"}, {"reference", "ref"}, {"create", true}});
    const double offset = g.find("ref_retopo")->as<double>(scene::MeshNode::kDrawOffset);
    CHECK(offset == doctest::Approx(std::sqrt(3.0) * 200.0 * 0.002));

    // Retargeting an existing retopo mesh keeps the offset it was given.
    ops::execute("setAttr", {{"args", {"ref_retopo.drawOffset", 1.5}}});
    ops::execute("retopoDrawSetReference", {{"mesh", "ref_retopo"}, {"reference", "plain"}});
    CHECK(g.find("ref_retopo")->as<double>(scene::MeshNode::kDrawOffset) == 1.5);
    g.clear();
    UndoStack::get().clear();
}

TEST_CASE("quad remesh: 500k triangle timing" * doctest::skip())
{
    geo::Mesh src = sphere(500);
    MESSAGE("input faces " << src.numFaces());
    for (int target : {2000, 10000})
    {
        retopo::QuadRemeshOptions o;
        o.targetFaces = target;
        auto t0 = std::chrono::steady_clock::now();
        retopo::QuadRemeshResult r = retopo::quadRemesh(src, o);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(r.ok());
        report("500k sphere to " + std::to_string(target), r);
        MESSAGE("wall " << sec << " s");
    }
}

TEST_CASE("commands: polyReduce, relaxMesh flags, polyTriangulate quadSplit, decimate modifier")
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    UndoStack::get().clear();
    makeMeshNode("ball", sphere(32));
    const Index before = toolOutMesh("ball").numFaces();
    ops::execute("setSymmetry", {{"args", {"ball"}}, {"axis", "x"}});
    ops::execute("polyReduce", {{"args", {"ball"}}, {"ratio", 0.3}});
    const geo::Mesh reduced = toolOutMesh("ball");
    CHECK(reduced.numFaces() < before);
    CHECK(reduced.numFaces() > before / 4);
    CHECK(geo::test::boundaryEdgeCount(reduced) == 0);
    // Symmetry on: every point has an exact mirror.
    geo::SymmetryMap map = geo::SymmetryMap::build(reduced, geo::SymmetryPlane{}, 1e-9);
    CHECK(map.unmatchedCount() == 0);

    ops::execute("relaxMesh", {{"args", {"ball"}}, {"iterations", 5}, {"sharpAngle", 20.0}, {"keepHardEdges", true}, {"project", true},
                                {"pinBoundary", false}});
    g.select({g.find("ball")->id()});
    ops::execute("polyTriangulate", {{"quadSplit", "first"}});
    for (Index f = 0; f < toolOutMesh("ball").faceCapacity(); ++f)
        if (toolOutMesh("ball").faceValid(f))
            CHECK(toolOutMesh("ball").faceSize(f) == 3);
    // Triangulate, select, relax, reduce.
    for (int i = 0; i < 4; ++i)
        UndoStack::get().undo();
    CHECK(toolOutMesh("ball").numFaces() == before);

    ops::execute("addModifier", {{"args", {"ball"}}, {"kind", "decimate"}, {"params", {{"ratio", 0.2}, {"symmetry", 1}, {"keepHardEdges", true}}}});
    const geo::Mesh modded = toolOutMesh("ball");
    CHECK(modded.numFaces() < before / 2);
    CHECK(modded.cornerAttrs().has("uv0"));
}
