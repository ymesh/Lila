#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "gn/Exec.h"
#include "gn/Gn.h"

using namespace lila;
using json = nlohmann::json;

namespace deform2_tests
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

N node(const char* name, const char* type, json values = json::object()) { return N{name, type, {}, std::move(values)}; }
json v3(double x, double y, double z) { return json::array({x, y, z}); }

TEST_CASE("lattice deforms by an offset field and by a cage")
{
    // A constant offset moves every lattice point, so the whole cube moves with it.
    auto cube = node("C", "MeshCube");
    auto moved = evalNodes({cube, node("Lt", "Lattice", {{"offset", v3(0, 20, 0)}, {"padding", 0.0}})},
                           {L{"C", "mesh", "Lt", "geometry"}, L{"Lt", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(moved->mesh);
    CHECK(moved->mesh->bounds().min.y == doctest::Approx(-30.0));
    CHECK(moved->mesh->bounds().max.y == doctest::Approx(70.0));

    // The lattice output, raised and fed back in as the cage, does the same.
    auto lat = node("L1", "Lattice", {{"resolution", v3(2, 2, 2)}});
    auto lift = node("Tr", "TransformGeometry", {{"translation", v3(0, 10, 0)}});
    auto use = node("L2", "Lattice", {{"resolution", v3(2, 2, 2)}});
    auto caged = evalNodes({cube, lat, lift, use}, {L{"C", "mesh", "L1", "geometry"}, L{"L1", "lattice", "Tr", "geometry"}, L{"C", "mesh", "L2", "geometry"},
                                                    L{"Tr", "geometry", "L2", "cage"}, L{"L2", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(caged->mesh);
    CHECK(caged->mesh->bounds().min.y == doctest::Approx(-40.0));

    // No offset: the rest lattice leaves the geometry alone.
    auto rest = evalNodes({cube, node("Lt", "Lattice", {{"resolution", v3(4, 3, 2)}})}, {L{"C", "mesh", "Lt", "geometry"}, L{"Lt", "geometry", "GroupOutput", "geometry"}});
    CHECK(rest->mesh->bounds().max.x == doctest::Approx(50.0));
    auto latOut = evalNodes({cube, node("Lt", "Lattice", {{"resolution", v3(4, 3, 2)}})}, {L{"C", "mesh", "Lt", "geometry"}, L{"Lt", "lattice", "GroupOutput", "geometry"}});
    CHECK(latOut->mesh->numPoints() == 24);

    // A cage with the wrong count is an error.
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.push_back(cube);
    g.nodes.push_back(node("Lt", "Lattice"));
    g.links = {L{"C", "mesh", "Lt", "geometry"}, L{"C", "mesh", "Lt", "cage"}, L{"Lt", "geometry", "GroupOutput", "geometry"}};
    gn::PlanPtr plan = gn::compile(g);
    CHECK_THROWS(gn::execute(*plan, {}));
}

TEST_CASE("point deform follows a proxy")
{
    // The grid follows a copy of itself moved up 30 and turned.
    auto grid = node("G", "MeshGrid", {{"verticesX", 5}, {"verticesY", 5}});
    auto proxy = node("P", "MeshGrid", {{"verticesX", 3}, {"verticesY", 3}});
    auto move = node("Tr", "TransformGeometry", {{"translation", v3(0, 30, 0)}});
    auto pd = node("PD", "PointDeform", {{"radius", 200.0}});
    auto out = evalNodes({grid, proxy, move, pd}, {L{"G", "mesh", "PD", "geometry"}, L{"P", "mesh", "PD", "rest"}, L{"P", "mesh", "Tr", "geometry"},
                                                   L{"Tr", "geometry", "PD", "deformed"}, L{"PD", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(out->mesh);
    CHECK(out->mesh->bounds().min.y == doctest::Approx(30.0));
    CHECK(out->mesh->bounds().max.y == doctest::Approx(30.0));
    CHECK(out->mesh->bounds().max.x == doctest::Approx(50.0));

    // Turned 90 degrees about Y, rigid mode carries the points around with it.
    auto turn = node("Tr", "TransformGeometry", {{"rotation", v3(0, 90, 0)}});
    auto rot = evalNodes({grid, proxy, turn, node("PD", "PointDeform", {{"radius", 200.0}})},
                         {L{"G", "mesh", "PD", "geometry"}, L{"P", "mesh", "PD", "rest"}, L{"P", "mesh", "Tr", "geometry"}, L{"Tr", "geometry", "PD", "deformed"},
                          L{"PD", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(rot->mesh);
    glm::dvec3 p0 = out->mesh->position(0) - glm::dvec3(0, 30, 0);
    glm::dvec3 r0 = rot->mesh->position(0);
    CHECK(glm::length(r0) == doctest::Approx(glm::length(p0)).epsilon(0.01));
    CHECK(rot->mesh->bounds().max.x == doctest::Approx(50.0).epsilon(0.01));

    // Out of reach of every rest point: nothing moves.
    auto far = evalNodes({grid, proxy, move, node("PD", "PointDeform", {{"radius", 0.5}})},
                         {L{"G", "mesh", "PD", "geometry"}, L{"P", "mesh", "PD", "rest"}, L{"P", "mesh", "Tr", "geometry"}, L{"Tr", "geometry", "PD", "deformed"},
                          L{"PD", "geometry", "GroupOutput", "geometry"}});
    CHECK(far->mesh->bounds().max.y == doctest::Approx(30.0));
    CHECK(far->mesh->bounds().min.y == doctest::Approx(0.0));
}

TEST_CASE("attribute transfer copies by distance")
{
    // The source grid stores heat 10 on every point; a finer target grid picks it up.
    auto src = node("S", "MeshGrid");
    auto store = node("St", "StoreNamedAttribute", {{"name", "heat"}, {"value", 10.0}});
    auto target = node("G", "MeshGrid", {{"verticesX", 5}, {"verticesY", 5}});
    auto xf = node("Tr", "AttributeTransfer", {{"attributes", "heat"}, {"count", 1}});
    auto out = evalNodes({src, store, target, xf}, {L{"S", "mesh", "St", "geometry"}, L{"St", "geometry", "Tr", "source"}, L{"G", "mesh", "Tr", "geometry"},
                                                   L{"Tr", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(out->mesh);
    auto heat = out->mesh->pointAttrs().get<float>("heat");
    REQUIRE(heat.size() == 25);
    for (float h : heat)
        CHECK(h == doctest::Approx(10.0));

    // Half blend mixes with the target's own (missing, so zero) value.
    auto half = evalNodes({src, store, target, node("Tr", "AttributeTransfer", {{"attributes", "heat"}, {"blend", 0.5}})},
                          {L{"S", "mesh", "St", "geometry"}, L{"St", "geometry", "Tr", "source"}, L{"G", "mesh", "Tr", "geometry"}, L{"Tr", "geometry", "GroupOutput", "geometry"}});
    CHECK(half->mesh->pointAttrs().get<float>("heat")[0] == doctest::Approx(5.0));

    // Source moved 100 up and a max distance of 50: nothing reaches.
    auto up = node("Up", "TransformGeometry", {{"translation", v3(0, 100, 0)}});
    auto none = evalNodes({src, store, up, target, node("Tr", "AttributeTransfer", {{"attributes", "heat"}, {"maxDistance", 50.0}})},
                          {L{"S", "mesh", "St", "geometry"}, L{"St", "geometry", "Up", "geometry"}, L{"Up", "geometry", "Tr", "source"}, L{"G", "mesh", "Tr", "geometry"},
                           L{"Tr", "geometry", "GroupOutput", "geometry"}});
    auto zero = none->mesh->pointAttrs().get<float>("heat");
    for (float h : zero)
        CHECK(h == doctest::Approx(0.0));

    // Faces: every target face takes the value too.
    auto faces = evalNodes({src, store, target, node("Tr", "AttributeTransfer", {{"attributes", "heat"}, {"domain", "face"}})},
                           {L{"S", "mesh", "St", "geometry"}, L{"St", "geometry", "Tr", "source"}, L{"G", "mesh", "Tr", "geometry"}, L{"Tr", "geometry", "GroupOutput", "geometry"}});
    auto fh = faces->mesh->faceAttrs().get<float>("heat");
    REQUIRE(fh.size() == 16);
    CHECK(fh[3] == doctest::Approx(10.0));
}

TEST_CASE("edge damage chips convex edges")
{
    auto cube = node("C", "MeshCube");
    auto dmg = node("D", "EdgeDamage", {{"amount", 4.0}, {"size", 10.0}, {"coverage", 1.0}, {"subdivisions", 3}});
    auto out = evalNodes({cube, dmg}, {L{"C", "mesh", "D", "mesh"}, L{"D", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(out->mesh);
    CHECK(out->mesh->numFaces() == 6 * 64);
    // Corners get pushed in; face centres, far from every edge, stay on the faces.
    geo::Bounds b = out->mesh->bounds();
    CHECK(b.max.x <= 50.0 + 1e-9);
    bool moved = false;
    for (glm::dvec3 p : out->mesh->positions())
        if (std::abs(std::abs(p.x) - 50.0) > 1e-6 && std::abs(std::abs(p.y) - 50.0) > 1e-6 && std::abs(std::abs(p.z) - 50.0) > 1e-6)
            moved = true;
    CHECK(moved);

    auto clean = evalNodes({cube, node("D", "EdgeDamage", {{"amount", 0.0}, {"subdivisions", 1}})}, {L{"C", "mesh", "D", "mesh"}, L{"D", "mesh", "GroupOutput", "geometry"}});
    CHECK(clean->mesh->bounds().max.x == doctest::Approx(50.0));
    CHECK(clean->mesh->bounds().min.y == doctest::Approx(-50.0));

    auto cut = evalNodes({cube, node("D", "EdgeDamage", {{"cut", true}, {"subdivisions", 0}})}, {L{"C", "mesh", "D", "mesh"}, L{"D", "mesh", "GroupOutput", "geometry"}});
    CHECK(cut->mesh->numFaces() == 26);

    // A flat grid has no sharp edges, so nothing happens.
    auto flat = evalNodes({node("G", "MeshGrid"), node("D", "EdgeDamage")}, {L{"G", "mesh", "D", "mesh"}, L{"D", "mesh", "GroupOutput", "geometry"}});
    for (glm::dvec3 p : flat->mesh->positions())
        CHECK(p.y == doctest::Approx(0.0));
}

TEST_CASE("flowmap stores a UV-space direction")
{
    // A flat grid with a +X direction: the flow runs along U (the grid's X), stored as colour red above 0.5.
    auto grid = node("G", "MeshGrid");
    auto fm = node("F", "Flowmap", {{"mode", "direction"}});
    auto out = evalNodes({grid, fm}, {L{"G", "mesh", "F", "mesh"}, L{"F", "mesh", "GroupOutput", "geometry"}});
    REQUIRE(out->mesh);
    auto flow = out->mesh->cornerAttrs().get<glm::vec2>("flow");
    REQUIRE(flow.size() == out->mesh->numCorners());
    CHECK(glm::length(flow[0]) == doctest::Approx(1.0));
    CHECK(std::abs(flow[0].x) == doctest::Approx(1.0).epsilon(0.01));
    auto color = out->mesh->cornerAttrs().get<glm::vec4>("color");
    REQUIRE(color.size() == flow.size());
    CHECK(std::abs(color[0].x - 0.5f) == doctest::Approx(0.5).epsilon(0.01));

    // Slope on a flat grid has no downhill, so it keeps the direction.
    auto slope = evalNodes({grid, node("F", "Flowmap")}, {L{"G", "mesh", "F", "mesh"}, L{"F", "mesh", "GroupOutput", "geometry"}});
    CHECK(std::abs(slope->mesh->cornerAttrs().get<glm::vec2>("flow")[0].x) == doctest::Approx(1.0).epsilon(0.01));

    // A tilted grid: downhill is along -Y in the world, which on the grid runs along its V.
    auto tilt = node("Tr", "TransformGeometry", {{"rotation", v3(45, 0, 0)}});
    auto down = evalNodes({grid, tilt, node("F", "Flowmap")}, {L{"G", "mesh", "Tr", "geometry"}, L{"Tr", "geometry", "F", "mesh"}, L{"F", "mesh", "GroupOutput", "geometry"}});
    glm::vec2 d = down->mesh->cornerAttrs().get<glm::vec2>("flow")[0];
    CHECK(std::abs(d.x) < 0.01);
    CHECK(std::abs(d.y) == doctest::Approx(1.0).epsilon(0.01));

    // Guide curve along Z: the flow turns to follow it.
    auto guide = node("Ln", "CurveLine", {{"start", v3(0, 0, -50)}, {"end", v3(0, 0, 50)}});
    auto guided = evalNodes({grid, guide, node("F", "Flowmap", {{"mode", "curves"}, {"radius", 1000.0}})},
                            {L{"G", "mesh", "F", "mesh"}, L{"Ln", "curve", "F", "guides"}, L{"F", "mesh", "GroupOutput", "geometry"}});
    glm::vec2 g = guided->mesh->cornerAttrs().get<glm::vec2>("flow")[0];
    CHECK(std::abs(g.y) > std::abs(g.x));
}
} // namespace deform2_tests
