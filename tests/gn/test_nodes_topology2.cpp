#include <doctest/doctest.h>

#include "gn/Exec.h"
#include "gn/Field.h"
#include "gn/Gn.h"
#include "geo/Mesh.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace lila;
using json = nlohmann::json;

namespace topology2_tests
{
using N = gn::GNode;
using L = gn::GLink;

gn::GeometryPtr evalNodes(std::vector<N> nodes, std::vector<L> links)
{
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = std::move(links);
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    return gn::execute(*plan, {});
}

/** Evaluates with the last node's output socket routed to the group output. */
gn::GeometryPtr evalTo(std::vector<N> nodes, std::vector<L> links, const char* node, const char* socket)
{
    links.push_back(L{node, socket, "GroupOutput", "geometry"});
    return evalNodes(std::move(nodes), std::move(links));
}

/** Stores a field on the mesh out of meshNode.meshSocket and reads it back on the domain. */
std::vector<double> probe(std::vector<N> nodes, std::vector<L> links, const char* meshNode, const char* meshSocket, const char* fieldNode, const char* fieldSocket,
                          const char* dataType, const char* domain, gn::GeometryPtr* geometry = nullptr)
{
    nodes.push_back(N{"Probe", "StoreNamedAttribute", {}, {{"name", "t"}, {"dataType", dataType}, {"domain", domain}}});
    links.push_back(L{meshNode, meshSocket, "Probe", "geometry"});
    links.push_back(L{fieldNode, fieldSocket, "Probe", "value"});
    links.push_back(L{"Probe", "geometry", "GroupOutput", "geometry"});
    gn::GeometryPtr g = evalNodes(std::move(nodes), std::move(links));
    REQUIRE(g->mesh);
    gn::Domain d = gn::Domain::Point;
    gn::parseDomain(domain, &d);
    gn::FieldContext ctx = gn::FieldContext::forMesh(*g->mesh, d);
    std::optional<gn::Column> col = gn::readAttribute(ctx, "t", &d);
    REQUIRE(col);
    if (geometry)
        *geometry = g;
    return gn::expand<double>(gn::convert(*col, gn::SocketType::Float), ctx.size);
}

size_t countOf(const std::vector<double>& v, double x)
{
    return size_t(std::count_if(v.begin(), v.end(), [x](double y) { return std::abs(x - y) < 1e-6; }));
}

int boundaryEdges(const geo::Mesh& m)
{
    int n = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        n += m.edgeValid(e) && m.edgeFaceCount(e) != 2 ? 1 : 0;
    return n;
}

/** Enclosed volume of a closed, consistently wound mesh. */
double volume(const geo::Mesh& m)
{
    double v = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<geo::Index> p = m.facePoints(f);
        for (size_t i = 1; i + 1 < p.size(); ++i)
            v += glm::dot(m.position(p[0]), glm::cross(m.position(p[i]), m.position(p[i + 1]))) / 6.0;
    }
    return v;
}

N compareInt(const char* name, int value) { return N{name, "Compare", {}, {{"dataType", "int"}, {"operation", "equal"}, {"b", value}}}; }

TEST_CASE("clip cuts and caps a mesh")
{
    gn::GeometryPtr g = evalTo({N{"C", "MeshCube", {}, {}}, N{"X", "Clip", {}, {}}}, {L{"C", "mesh", "X", "geometry"}}, "X", "geometry");
    REQUIRE(g->mesh);
    const geo::Mesh& m = *g->mesh;
    CHECK(boundaryEdges(m) == 0);
    CHECK(m.bounds().min.y == doctest::Approx(0.0));
    CHECK(m.bounds().max.y == doctest::Approx(50.0));
    CHECK(volume(m) == doctest::Approx(500000.0).epsilon(1e-6));

    gn::GeometryPtr both = evalTo({N{"C", "MeshCube", {}, {}}, N{"X", "Clip", {}, {{"keep", "both"}}}}, {L{"C", "mesh", "X", "geometry"}}, "X", "geometry");
    CHECK(boundaryEdges(*both->mesh) == 0);
    CHECK(volume(*both->mesh) == doctest::Approx(1000000.0).epsilon(1e-6));
    CHECK(both->mesh->bounds().min.y == doctest::Approx(-50.0));

    gn::GeometryPtr open = evalTo({N{"C", "MeshCube", {}, {}}, N{"X", "Clip", {}, {{"keep", "below"}, {"cap", false}}}}, {L{"C", "mesh", "X", "geometry"}}, "X", "geometry");
    CHECK(boundaryEdges(*open->mesh) == 4);
    CHECK(open->mesh->bounds().max.y == doctest::Approx(0.0));
}

TEST_CASE("clip caps around holes")
{
    gn::GeometryPtr g;
    std::vector<double> cap = probe({N{"T", "MeshTorus", {}, {}}, N{"X", "Clip", {}, {}}}, {L{"T", "mesh", "X", "geometry"}}, "X", "geometry", "X", "capFaces", "bool", "face", &g);
    const geo::Mesh& m = *g->mesh;
    CHECK(boundaryEdges(m) == 0);
    double area = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (cap[f] > 0.5)
        {
            area += m.faceArea(f);
            // Nothing covers the hole in the middle of the ring.
            glm::dvec3 c = m.faceCentroid(f);
            CHECK(std::hypot(c.x, c.z) > 25.0);
        }
    CHECK(area == doctest::Approx(3.14159265 * (70.0 * 70.0 - 30.0 * 30.0)).epsilon(0.05));
}

TEST_CASE("clip cuts curves")
{
    gn::GeometryPtr g = evalTo({N{"L", "CurveLine", {}, {{"start", json::array({0, -50, 0})}, {"end", json::array({0, 50, 0})}}}, N{"X", "Clip", {}, {}}},
                               {L{"L", "curve", "X", "geometry"}}, "X", "geometry");
    REQUIRE(g->curves);
    REQUIRE(g->curves->numCurves() == 1);
    auto pts = g->curves->curvePoints(0);
    double lo = 1e9;
    for (const auto& p : pts)
        lo = std::min(lo, p.y);
    CHECK(lo == doctest::Approx(0.0));

    gn::GeometryPtr circle = evalTo({N{"O", "CurveCircle", {}, {}}, N{"X", "Clip", {}, {{"normal", json::array({1, 0, 0})}, {"keep", "both"}}}},
                                    {L{"O", "curve", "X", "geometry"}}, "X", "geometry");
    CHECK(circle->curves->numCurves() == 2);
}

TEST_CASE("fill holes")
{
    auto cylinder = [](json fill) {
        return std::vector<N>{N{"C", "MeshCylinder", {}, {{"fillType", "none"}, {"vertices", 8}}}, N{"F", "FillHoles", {}, std::move(fill)}};
    };
    std::vector<L> links = {L{"C", "mesh", "F", "mesh"}};
    gn::GeometryPtr g = evalTo(cylinder(json::object()), links, "F", "mesh");
    CHECK(boundaryEdges(*g->mesh) == 0);
    CHECK(g->mesh->numFaces() == 10);

    CHECK(evalTo(cylinder({{"method", "triangles"}}), links, "F", "mesh")->mesh->numFaces() == 8 + 12);
    CHECK(evalTo(cylinder({{"method", "fan"}}), links, "F", "mesh")->mesh->numFaces() == 8 + 16);
    CHECK(boundaryEdges(*evalTo(cylinder({{"method", "grid"}}), links, "F", "mesh")->mesh) == 0);
    CHECK(boundaryEdges(*evalTo(cylinder({{"maxSides", 6}}), links, "F", "mesh")->mesh) == 16);
    CHECK(boundaryEdges(*evalTo(cylinder({{"maxPerimeter", 10.0}}), links, "F", "mesh")->mesh) == 16);

    // Planar caps take a spread of uvs at about the sides' density rather than collapsing onto the border.
    gn::GeometryPtr capped;
    std::vector<double> fill = probe(cylinder(json::object()), links, "F", "mesh", "F", "fill", "bool", "face", &capped);
    const geo::Mesh& m = *capped->mesh;
    auto uv = m.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!uv.empty());
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (fill[f] < 0.5)
            continue;
        glm::vec2 lo(1e9f), hi(-1e9f);
        m.forEachFaceCorner(f, [&](geo::Index k) {
            lo = glm::min(lo, uv[k]);
            hi = glm::max(hi, uv[k]);
        });
        CHECK(hi.x - lo.x > 0.01f);
        CHECK(hi.y - lo.y > 0.01f);
    }
}

TEST_CASE("clean mesh welds and drops duplicates")
{
    std::vector<N> nodes = {N{"C", "MeshCube", {}, {}}, N{"J", "JoinGeometry", {}, {}}, N{"K", "CleanMesh", {}, {}}};
    std::vector<L> links = {L{"C", "mesh", "J", "geometry"}, L{"C", "mesh", "J", "geometry"}, L{"J", "geometry", "K", "mesh"}};
    gn::GeometryPtr g = evalTo(nodes, links, "K", "mesh");
    CHECK(g->mesh->numPoints() == 8);
    CHECK(g->mesh->numFaces() == 6);

    nodes[2].values["reportOnly"] = true;
    gn::GeometryPtr report = evalTo(nodes, links, "K", "mesh");
    CHECK(report->mesh->numPoints() == 16);
    CHECK(report->mesh->numFaces() == 12);
    std::vector<double> coincident = probe(nodes, links, "K", "mesh", "K", "coincident", "bool", "point");
    CHECK(countOf(coincident, 1.0) >= 8);

    gn::GeometryPtr clean = evalTo({N{"C", "MeshCube", {}, {}}, N{"K", "CleanMesh", {}, {}}}, {L{"C", "mesh", "K", "mesh"}}, "K", "mesh");
    CHECK(clean->mesh->numFaces() == 6);

    // Loose edges and their points go.
    std::vector<N> loose = {N{"C", "MeshCube", {}, {}}, N{"M", "MeshLine", {}, {{"count", 4}, {"start", json::array({200, 0, 0})}}}, N{"J", "JoinGeometry", {}, {}},
                            N{"K", "CleanMesh", {}, {}}};
    std::vector<L> looseLinks = {L{"C", "mesh", "J", "geometry"}, L{"M", "mesh", "J", "geometry"}, L{"J", "geometry", "K", "mesh"}};
    CHECK(evalTo(loose, looseLinks, "K", "mesh")->mesh->numPoints() == 8);
}

TEST_CASE("isotropic remesh evens edges and keeps features")
{
    gn::GeometryPtr s = evalTo({N{"S", "MeshUVSphere", {}, {}}, N{"R", "IsotropicRemesh", {}, {{"edgeLength", 10.0}}}}, {L{"S", "mesh", "R", "mesh"}}, "R", "mesh");
    const geo::Mesh& m = *s->mesh;
    REQUIRE(m.numFaces() > 100);
    double sum = 0.0, lo = 1e9, hi = 0.0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            double l = m.edgeLength(e);
            sum += l;
            lo = std::min(lo, l);
            hi = std::max(hi, l);
        }
    double mean = sum / m.numEdges();
    CHECK(mean > 7.5);
    CHECK(mean < 12.5);
    CHECK(hi < 24.0);
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            CHECK(m.faceSize(f) == 3);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        CHECK(glm::length(m.position(p)) == doctest::Approx(50.0).epsilon(0.02));
    CHECK(boundaryEdges(m) == 0);

    gn::GeometryPtr c = evalTo({N{"C", "MeshCube", {}, {}}, N{"R", "IsotropicRemesh", {}, {{"edgeLength", 12.0}}}}, {L{"C", "mesh", "R", "mesh"}}, "R", "mesh");
    const geo::Mesh& cube = *c->mesh;
    CHECK(cube.numFaces() > 200);
    CHECK(boundaryEdges(cube) == 0);
    for (geo::Index p = 0; p < cube.pointCapacity(); ++p)
    {
        glm::dvec3 a = glm::abs(cube.position(p));
        CHECK(std::max({a.x, a.y, a.z}) == doctest::Approx(50.0).epsilon(1e-6));
    }
    CHECK(volume(cube) == doctest::Approx(1000000.0).epsilon(1e-3));

    gn::GeometryPtr adaptive = evalTo({N{"S", "MeshUVSphere", {}, {}}, N{"R", "IsotropicRemesh", {}, {{"edgeLength", 10.0}, {"adaptivity", 1.0}}}},
                                      {L{"S", "mesh", "R", "mesh"}}, "R", "mesh");
    CHECK(adaptive->mesh->numFaces() > m.numFaces());
}

TEST_CASE("triangulate 2D respects holes")
{
    std::vector<N> nodes = {N{"A", "CurveCircle", {}, {{"radius", 50.0}}}, N{"B", "CurveCircle", {}, {{"radius", 20.0}}}, N{"J", "JoinGeometry", {}, {}},
                            N{"T", "Triangulate2D", {}, {}}};
    std::vector<L> links = {L{"A", "curve", "J", "geometry"}, L{"B", "curve", "J", "geometry"}, L{"J", "geometry", "T", "geometry"}};
    gn::GeometryPtr g = evalTo(nodes, links, "T", "mesh");
    REQUIRE(g->mesh);
    const geo::Mesh& m = *g->mesh;
    CHECK(m.numFaces() >= 64);
    double area = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        glm::dvec3 c = m.faceCentroid(f);
        CHECK(std::hypot(c.x, c.z) > 19.0);
        CHECK(m.faceNormal(f).y == doctest::Approx(1.0));
        area += m.faceArea(f);
    }
    CHECK(area == doctest::Approx(3.14159265 * (2500.0 - 400.0)).epsilon(0.03));

    nodes[3].values["fill"] = "hull";
    gn::GeometryPtr hull = evalTo(nodes, links, "T", "mesh");
    CHECK(hull->mesh->numFaces() > m.numFaces());

    gn::GeometryPtr grid = evalTo({N{"G", "MeshGrid", {}, {}}, N{"T", "Triangulate2D", {}, {}}}, {L{"G", "mesh", "T", "geometry"}}, "T", "mesh");
    CHECK(grid->mesh->numFaces() == 8);
}

TEST_CASE("tetrahedralize fills the volume")
{
    for (double spacing : {0.0, 30.0})
    {
        gn::GeometryPtr g = evalTo({N{"C", "MeshCube", {}, {}}, N{"T", "Tetrahedralize", {}, {{"spacing", spacing}}}}, {L{"C", "mesh", "T", "mesh"}}, "T", "mesh");
        const geo::Mesh& m = *g->mesh;
        CHECK(m.numFaces() >= 20);
        CHECK(m.numFaces() % 4 == 0);
        CHECK(volume(m) == doctest::Approx(1000000.0).epsilon(0.01));
        auto tet = m.faceAttrs().get<int32_t>("tetIndex");
        REQUIRE(tet.size() == m.numFaces());
        CHECK(*std::max_element(tet.begin(), tet.end()) == int32_t(m.numFaces() / 4 - 1));
    }
    std::vector<double> surface = probe({N{"C", "MeshCube", {}, {}}, N{"T", "Tetrahedralize", {}, {{"spacing", 30.0}}}}, {L{"C", "mesh", "T", "mesh"}}, "T", "mesh", "T",
                                        "surface", "bool", "face");
    CHECK(countOf(surface, 1.0) >= 12);
    CHECK(countOf(surface, 0.0) > 0);

    gn::GeometryPtr sphere = evalTo({N{"S", "MeshIcoSphere", {}, {}}, N{"T", "Tetrahedralize", {}, {{"spacing", 20.0}}}}, {L{"S", "mesh", "T", "mesh"}}, "T", "mesh");
    gn::GeometryPtr ico = evalTo({N{"S", "MeshIcoSphere", {}, {}}}, {}, "S", "mesh");
    CHECK(volume(*sphere->mesh) == doctest::Approx(volume(*ico->mesh)).epsilon(0.02));
}

TEST_CASE("dissolve flat edges")
{
    std::vector<N> nodes = {N{"G", "MeshGrid", {}, {{"verticesX", 5}, {"verticesY", 5}}}, N{"D", "DissolveFlatEdges", {}, {{"keepBorders", false}}}};
    std::vector<L> links = {L{"G", "mesh", "D", "mesh"}};
    CHECK(evalTo(nodes, links, "D", "mesh")->mesh->numFaces() == 1);
    nodes[1].values["keepBorders"] = true;
    CHECK(evalTo(nodes, links, "D", "mesh")->mesh->numFaces() > 1);
    CHECK(evalTo({N{"C", "MeshCube", {}, {}}, N{"D", "DissolveFlatEdges", {}, {}}}, {L{"C", "mesh", "D", "mesh"}}, "D", "mesh")->mesh->numFaces() == 6);
}

TEST_CASE("split by point attribute")
{
    std::vector<N> nodes = {N{"G", "MeshGrid", {}, {{"verticesX", 2}, {"verticesY", 2}}}, N{"P", "Position", {}, {}}, N{"X", "SeparateXYZ", {}, {}},
                            N{"S", "SplitByPointAttribute", {}, {{"threshold", 10.0}}}};
    std::vector<L> links = {L{"G", "mesh", "S", "mesh"}, L{"P", "value", "X", "vector"}, L{"X", "x", "S", "value"}};
    gn::GeometryPtr g;
    std::vector<double> split = probe(nodes, links, "S", "mesh", "S", "split", "bool", "edge", &g);
    CHECK(g->mesh->numFaces() == 2);
    CHECK(g->mesh->numPoints() == 6);
    CHECK(countOf(split, 1.0) == 1);
    for (geo::Index p = 4; p < 6; ++p)
        CHECK(g->mesh->position(p).x == doctest::Approx(10.0));

    // Change: the right column of a 3 x 3 grid reads 1, so the two quads beside it split down the middle.
    nodes[0].values = {{"verticesX", 3}, {"verticesY", 3}};
    nodes[3].values = {{"mode", "change"}};
    nodes.push_back(N{"Gt", "Compare", {}, {{"dataType", "float"}, {"operation", "greaterThan"}, {"b", 1.0}}});
    links = {L{"G", "mesh", "S", "mesh"}, L{"P", "value", "X", "vector"}, L{"X", "x", "Gt", "a"}, L{"Gt", "result", "S", "value"}};
    gn::GeometryPtr c = evalTo(nodes, links, "S", "mesh");
    CHECK(c->mesh->numFaces() == 6);
}

TEST_CASE("grow, shrink and expand selections")
{
    auto run = [](N node, const char* socket) {
        std::vector<N> nodes = {N{"G", "MeshGrid", {}, {{"verticesX", 5}, {"verticesY", 5}}}, N{"I", "Index", {}, {}}, compareInt("Eq", 12), std::move(node)};
        std::vector<L> links = {L{"I", "value", "Eq", "a"}, L{"Eq", "result", "Op", "selection"}};
        return probe(nodes, links, "G", "mesh", "Op", socket, "float", "point");
    };
    CHECK(countOf(run(N{"Op", "GrowSelection", {}, {{"domain", "point"}}}, "selection"), 1.0) == 5);
    CHECK(countOf(run(N{"Op", "GrowSelection", {}, {{"domain", "point"}, {"across", "corners"}}}, "selection"), 1.0) == 9);
    CHECK(countOf(run(N{"Op", "GrowSelection", {}, {{"domain", "point"}, {"steps", 2}}}, "selection"), 1.0) == 13);
    CHECK(countOf(run(N{"Op", "GrowSelection", {}, {{"domain", "point"}, {"steps", 0}}}, "selection"), 1.0) == 1);
    CHECK(countOf(run(N{"Op", "ShrinkSelection", {}, {{"domain", "point"}}}, "selection"), 1.0) == 0);

    std::vector<N> all = {N{"G", "MeshGrid", {}, {{"verticesX", 5}, {"verticesY", 5}}}, N{"Op", "ShrinkSelection", {}, {{"domain", "face"}, {"selection", true}}}};
    std::vector<double> shrunk = probe(all, {}, "G", "mesh", "Op", "selection", "float", "face");
    CHECK(countOf(shrunk, 1.0) == 16);
    std::vector<N> some = {N{"G", "MeshGrid", {}, {{"verticesX", 5}, {"verticesY", 5}}}, N{"I", "Index", {}, {}},
                           N{"Lt", "Compare", {}, {{"dataType", "int"}, {"operation", "lessThan"}, {"b", 8}}},
                           N{"Op", "ShrinkSelection", {}, {{"domain", "face"}}}};
    std::vector<double> half = probe(some, {L{"I", "value", "Lt", "a"}, L{"Lt", "result", "Op", "selection"}}, "G", "mesh", "Op", "selection", "float", "face");
    CHECK(countOf(half, 1.0) == 4);

    std::vector<double> dist = run(N{"Op", "ExpandSelection", {}, {{"distance", 30.0}}}, "distance");
    CHECK(countOf(dist, 0.0) == 1);
    CHECK(countOf(dist, 25.0) == 4);
    CHECK(countOf(dist, -1.0) == 20);
    std::vector<double> fall = run(N{"Op", "ExpandSelection", {}, {{"distance", 30.0}}}, "falloff");
    CHECK(countOf(fall, 1.0) == 1);
    CHECK(countOf(fall, 1.0 - 25.0 / 30.0) == 4);
    std::vector<double> steps = run(N{"Op", "ExpandSelection", {}, {{"mode", "topology"}, {"steps", 2}}}, "selection");
    CHECK(countOf(steps, 1.0) == 13);
}

TEST_CASE("corners of edge")
{
    gn::GeometryPtr g;
    std::vector<double> total = probe({N{"C", "MeshCube", {}, {}}, N{"E", "CornersOfEdge", {}, {}}}, {}, "C", "mesh", "E", "total", "int", "edge", &g);
    CHECK(countOf(total, 2.0) == 12);
    const geo::Mesh& m = *g->mesh;
    std::vector<double> first = probe({N{"C", "MeshCube", {}, {}}, N{"E", "CornersOfEdge", {}, {}}}, {}, "C", "mesh", "E", "cornerIndex", "int", "edge");
    std::vector<double> second = probe({N{"C", "MeshCube", {}, {}}, N{"E", "CornersOfEdge", {}, {{"sortIndex", 1}}}}, {}, "C", "mesh", "E", "cornerIndex", "int", "edge");
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
    {
        CHECK(m.cornerEdge(geo::Index(first[e])) == e);
        CHECK(m.cornerEdge(geo::Index(second[e])) == e);
        CHECK(first[e] < second[e]);
    }
}

TEST_CASE("auto uv seams")
{
    std::vector<double> seam = probe({N{"C", "MeshCube", {}, {}}, N{"A", "AutoUVSeams", {}, {}}}, {L{"C", "mesh", "A", "mesh"}}, "A", "mesh", "A", "seam", "bool", "edge");
    CHECK(countOf(seam, 1.0) == 12);
    std::vector<double> sphere = probe({N{"S", "MeshUVSphere", {}, {}}, N{"A", "AutoUVSeams", {}, {}}}, {L{"S", "mesh", "A", "mesh"}}, "A", "mesh", "A", "seam", "bool", "edge");
    CHECK(countOf(sphere, 1.0) > 0);
    CHECK(countOf(sphere, 0.0) > countOf(sphere, 1.0));
}

TEST_CASE("edge relax restores reference lengths")
{
    auto lengthError = [](const geo::Mesh& m) {
        double err = 0.0;
        for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e))
                err += std::abs(m.edgeLength(e) - 25.0);
        return err;
    };
    std::vector<N> nodes = {N{"G", "MeshGrid", {}, {{"verticesX", 5}, {"verticesY", 5}}},
                            N{"R", "RandomValue", {}, {{"dataType", "vector"}, {"min", json::array({-5, 0, -5})}, {"max", json::array({5, 0, 5})}}},
                            N{"P", "SetPosition", {}, {}}, N{"E", "EdgeRelax", {}, {{"iterations", 200}}}};
    std::vector<L> links = {L{"G", "mesh", "P", "geometry"}, L{"R", "value", "P", "offset"}};
    gn::GeometryPtr before = evalTo(nodes, links, "P", "geometry");
    links.push_back(L{"P", "geometry", "E", "mesh"});
    links.push_back(L{"G", "mesh", "E", "reference"});
    gn::GeometryPtr after = evalTo(nodes, links, "E", "mesh");
    CHECK(lengthError(*after->mesh) < lengthError(*before->mesh) * 0.2);

    nodes.push_back(N{"Big", "MeshGrid", {}, {{"verticesX", 6}}});
    links.back() = L{"Big", "mesh", "E", "reference"};
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = links;
    g.links.push_back(L{"E", "mesh", "GroupOutput", "geometry"});
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE(plan->error.empty());
    CHECK_THROWS(gn::execute(*plan, {}));
}
} // namespace topology2_tests
