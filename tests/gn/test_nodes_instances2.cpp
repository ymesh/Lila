#include "Nodes2Util.h"

#include "gn/Volume.h"

using namespace nodes2test;
using gn::Domain;
using gn::SocketType;

namespace
{
/** A mesh grid of 2x2 faces split by the sign of each face centre's X. */
Run splitGrid()
{
    return run({N{"G", "MeshGrid", {}, {}}, N{"P", "Position", {}, {}}, N{"Sep", "SeparateXYZ", {}, {}}, N{"Gt", "Compare", {}, {{"operation", "greaterThan"}, {"b", 0.0}}},
                N{"S", "SplitToInstances", {}, {{"domain", "face"}}}},
               {L{"G", "mesh", "S", "geometry"}, L{"P", "value", "Sep", "vector"}, L{"Sep", "x", "Gt", "a"}, L{"Gt", "result", "S", "groupId"},
                L{"S", "instances", "GroupOutput", "geometry"}, L{"S", "groupId", "GroupOutput", "id"}},
               {{"id", SocketType::Int}});
}
} // namespace

TEST_CASE("split to instances")
{
    Run r = splitGrid();
    REQUIRE(r.geometry->instances);
    const gn::Instances& in = *r.geometry->instances;
    REQUIRE(in.size() == 2);
    for (int k = 0; k < 2; ++k)
    {
        REQUIRE(in.references[size_t(in.reference[k])]->mesh);
        CHECK(in.references[size_t(in.reference[k])]->mesh->numFaces() == 2);
    }
    CHECK(gn::evaluateAll<int64_t>(r.values[0].field, gn::FieldContext::forInstances(in)) == std::vector<int64_t>{0, 1});
    // The x > 0 group's faces all sit on the positive side.
    CHECK(in.references[size_t(in.reference[1])]->mesh->bounds().min.x == doctest::Approx(0.0));
}

TEST_CASE("instance bounds")
{
    Run r = run({N{"C", "MeshCube", {}, {{"size", {10, 20, 30}}}}, N{"Pts", "Points", {}, {{"count", 1}, {"position", {100, 0, 0}}}},
                 N{"I", "InstanceOnPoints", {}, {}}, N{"B", "InstanceBounds", {}, {}}, N{"BL", "InstanceBounds", {}, {{"local", true}}}},
                {L{"Pts", "points", "I", "points"}, L{"C", "mesh", "I", "instance"}, L{"I", "instances", "GroupOutput", "geometry"}, L{"B", "min", "GroupOutput", "min"},
                 L{"B", "max", "GroupOutput", "max"}, L{"BL", "min", "GroupOutput", "lmin"}},
                {{"min", SocketType::Vector}, {"max", SocketType::Vector}, {"lmin", SocketType::Vector}});
    REQUIRE(r.geometry->instances);
    gn::FieldContext ctx = gn::FieldContext::forInstances(*r.geometry->instances);
    CHECK(near(gn::evaluateAll<D3>(r.values[0].field, ctx)[0], D3(95, -10, -15)));
    CHECK(near(gn::evaluateAll<D3>(r.values[1].field, ctx)[0], D3(105, 10, 15)));
    CHECK(near(gn::evaluateAll<D3>(r.values[2].field, ctx)[0], D3(-5, -10, -15)));
}

TEST_CASE("set id and index of nearest")
{
    auto in = points({D3(0, 0, 0), D3(1, 0, 0), D3(10, 0, 0), D3(12, 0, 0)});
    Run r = run({N{"I", "Index", {}, {}}, N{"M", "IntegerMath", {}, {{"operation", "multiply"}, {"b", 10}}}, N{"S", "SetID", {}, {}}, N{"Id", "ID", {}, {}},
                 N{"Nn", "IndexOfNearest", {}, {}}, N{"Grp", "IndexOfNearest", {}, {}}, N{"Mod", "IntegerMath", {}, {{"operation", "modulo"}, {"b", 2}}}},
                {L{"I", "value", "M", "a"}, L{"M", "value", "S", "id"}, L{"GroupInput", "geometry", "S", "geometry"}, L{"S", "geometry", "GroupOutput", "geometry"},
                 L{"Id", "value", "GroupOutput", "id"}, L{"Nn", "index", "GroupOutput", "nearest"}, L{"I", "value", "Mod", "a"}, L{"Mod", "value", "Grp", "groupId"},
                 L{"Grp", "index", "GroupOutput", "inGroup"}, L{"Grp", "hasNeighbor", "GroupOutput", "has"}},
                {{"id", SocketType::Int}, {"nearest", SocketType::Int}, {"inGroup", SocketType::Int}, {"has", SocketType::Bool}}, in);
    REQUIRE(r.geometry->points);
    const geo::Mesh& m = *r.geometry->points;
    CHECK(onPoints<int64_t>(r.values[0], m) == std::vector<int64_t>{0, 10, 20, 30});
    CHECK(onPoints<int64_t>(r.values[1], m) == std::vector<int64_t>{1, 0, 3, 2});
    CHECK(onPoints<int64_t>(r.values[2], m) == std::vector<int64_t>{2, 3, 0, 1});
    CHECK(onPoints<uint8_t>(r.values[3], m) == std::vector<uint8_t>{1, 1, 1, 1});

    auto lone = run({N{"Nn", "IndexOfNearest", {}, {}}}, {L{"GroupInput", "geometry", "GroupOutput", "geometry"}, L{"Nn", "index", "GroupOutput", "i"}},
                    {{"i", SocketType::Int}}, points({D3(0)}));
    CHECK(onPoints<int64_t>(lone.values[0], *lone.geometry->points) == std::vector<int64_t>{-1});
}

TEST_CASE("geometry name")
{
    Run r = run({N{"C", "MeshCube", {}, {}}, N{"S", "SetGeometryName", {}, {{"name", "crate"}}}, N{"T", "TransformGeometry", {}, {{"translation", {1, 0, 0}}}},
                 N{"J", "JoinGeometry", {}, {}}, N{"Nm", "GeometryName", {}, {}}},
                {L{"C", "mesh", "S", "geometry"}, L{"S", "geometry", "T", "geometry"}, L{"T", "geometry", "J", "geometry"}, L{"J", "geometry", "Nm", "geometry"},
                 L{"J", "geometry", "GroupOutput", "geometry"}, L{"Nm", "name", "GroupOutput", "name"}},
                {{"name", SocketType::String}});
    CHECK(r.values[0].string == "crate");
    CHECK(r.geometry->name == "crate");
}

TEST_CASE("distribute points in grid")
{
    gn::Volume v;
    v.dims = {3, 1, 1};
    v.values = {0.0f, 1.0f, 0.5f};
    v.indexToWorld = glm::dmat4(10.0);
    v.indexToWorld[3] = glm::dvec4(0, 0, 0, 1);
    gn::GeometryPtr vol = gn::volumeGeometry(v);
    Run r = run({N{"D", "DistributePointsInGrid", {}, {{"threshold", 0.2}}}}, {L{"GroupInput", "geometry", "D", "volume"}, L{"D", "points", "GroupOutput", "geometry"}}, {},
                vol);
    REQUIRE(r.geometry->points);
    REQUIRE(r.geometry->points->numPoints() == 2);
    CHECK(near(r.geometry->points->position(0), D3(10, 0, 0)));
    CHECK(r.geometry->points->pointAttrs().get<float>("value")[1] == doctest::Approx(0.5));

    Run rnd = run({N{"D", "DistributePointsInGrid", {}, {{"mode", "random"}, {"density", 4.0}, {"threshold", 0.2}}}},
                  {L{"GroupInput", "geometry", "D", "volume"}, L{"D", "points", "GroupOutput", "geometry"}}, {}, vol);
    REQUIRE(rnd.geometry->points);
    // 4 points in the full voxel, 2 in the half one, each inside its voxel.
    CHECK(rnd.geometry->points->numPoints() == 6);
    for (const D3& p : rnd.geometry->points->positions())
        CHECK(p.x >= 5.0 - 1e-9);
}

TEST_CASE("cluster points")
{
    std::vector<D3> pts;
    for (int k = 0; k < 10; ++k)
    {
        pts.push_back(D3(k * 0.1, 0, 0));
        pts.push_back(D3(100 + k * 0.1, 0, 0));
    }
    Run r = run({N{"K", "ClusterPoints", {}, {{"count", 2}}}},
                {L{"GroupInput", "geometry", "K", "geometry"}, L{"K", "geometry", "GroupOutput", "geometry"}, L{"K", "cluster", "GroupOutput", "c"},
                 L{"K", "centroids", "GroupOutput", "cents"}},
                {{"c", SocketType::Int}, {"cents", SocketType::Geometry}}, points(pts));
    std::vector<int64_t> c = onPoints<int64_t>(r.values[0], *r.geometry->points);
    for (int k = 0; k < 10; ++k)
    {
        CHECK(c[size_t(2 * k)] == c[0]);
        CHECK(c[size_t(2 * k + 1)] == c[1]);
    }
    CHECK(c[0] != c[1]);
    const geo::Mesh& cents = *r.values[1].geometry->points;
    REQUIRE(cents.numPoints() == 2);
    CHECK(cents.position(geo::Index(c[0])).x == doctest::Approx(0.45));
    CHECK(cents.position(geo::Index(c[1])).x == doctest::Approx(100.45));
    CHECK(cents.pointAttrs().get<int32_t>("size")[0] == 10);
}

TEST_CASE("self prune")
{
    // Spheres of radius 1 every 1.5 cm overlap their neighbours: keeping by weight (higher first) keeps every other one.
    std::vector<D3> pts;
    for (int k = 0; k < 6; ++k)
        pts.push_back(D3(k * 1.5, 0, 0));
    Run r = run({N{"I", "Index", {}, {}}, N{"P", "SelfPrune", {}, {{"radius", 1.0}}}},
                {L{"I", "value", "P", "weight"}, L{"GroupInput", "geometry", "P", "geometry"}, L{"P", "geometry", "GroupOutput", "geometry"}}, {}, points(pts));
    REQUIRE(r.geometry->points);
    const geo::Mesh& kept = *r.geometry->points;
    REQUIRE(kept.numPoints() == 3);
    CHECK(kept.position(0).x == doctest::Approx(1.5));
    CHECK(kept.position(2).x == doctest::Approx(7.5));

    // Boxes 1 cm wide do not overlap at 1.5 cm spacing.
    Run box = run({N{"P", "SelfPrune", {}, {{"shape", "box"}, {"size", {1, 1, 1}}}}},
                  {L{"GroupInput", "geometry", "P", "geometry"}, L{"P", "geometry", "GroupOutput", "geometry"}}, {}, points(pts));
    CHECK(box.geometry->points->numPoints() == 6);

    // Instances prune too.
    Run inst = run({N{"C", "MeshCube", {}, {}}, N{"Pts", "MeshLine", {}, {{"count", 4}, {"offset", {1, 0, 0}}}}, N{"I", "InstanceOnPoints", {}, {}},
                    N{"P", "SelfPrune", {}, {{"radius", 1.0}, {"priority", "random"}}}},
                   {L{"Pts", "mesh", "I", "points"}, L{"C", "mesh", "I", "instance"}, L{"I", "instances", "P", "geometry"}, L{"P", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(inst.geometry->instances);
    CHECK(inst.geometry->instances->size() == 2);
}
