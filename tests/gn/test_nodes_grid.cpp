#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "gn/Exec.h"
#include "gn/Gn.h"
#include "gn/GeometryIO.h"
#include "gn/Volume.h"

using namespace lila;
using json = nlohmann::json;

namespace grid_tests
{
using N = gn::GNode;
using L = gn::GLink;

gn::GeometryPtr run(std::vector<gn::GNode> nodes, std::vector<gn::GLink> links, gn::GeometryPtr input = nullptr)
{
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = std::move(links);
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    gn::ExecEnv env;
    env.input = input;
    return gn::execute(*plan, env);
}

/** A density grid over [-10, 10]^3 at voxel size 1 holding f(world position). */
gn::Volume densityOf(const std::function<float(glm::dvec3)>& f, double half = 10.0, double voxel = 1.0)
{
    gn::Volume v = gn::Volume::grid({glm::dvec3(-half), glm::dvec3(half)}, voxel, gn::Volume::Kind::Density, 0.0f);
    for (int k = 0; k < v.dims.z; ++k)
        for (int j = 0; j < v.dims.y; ++j)
            for (int i = 0; i < v.dims.x; ++i)
                v.values[v.index(i, j, k)] = f(v.worldOf({i, j, k}));
    return v;
}

gn::Volume vectorsOf(const std::function<glm::vec3(glm::dvec3)>& f, double half = 10.0, double voxel = 1.0)
{
    gn::Volume v = gn::Volume::grid({glm::dvec3(-half), glm::dvec3(half)}, voxel, gn::Volume::Kind::Density, 0.0f);
    std::vector<glm::vec3> vectors(v.size());
    for (int k = 0; k < v.dims.z; ++k)
        for (int j = 0; j < v.dims.y; ++j)
            for (int i = 0; i < v.dims.x; ++i)
                vectors[v.index(i, j, k)] = f(v.worldOf({i, j, k}));
    v.setVectors(std::move(vectors));
    return v;
}

/** Runs one node on the input geometry's grid and returns the grid it makes. */
std::shared_ptr<const gn::Volume> apply(const char* type, gn::Volume in, json values = json::object(), json props = json::object())
{
    N node{"G", type, {}, {}};
    for (auto& [k, v] : values.items())
        node.values[k] = v;
    for (auto& [k, v] : props.items())
        node.values[k] = v;
    auto out = run({node}, {L{"GroupInput", "geometry", "G", "grid"}, L{"G", "grid", "GroupOutput", "geometry"}}, gn::volumeGeometry(std::move(in)));
    REQUIRE(out->volume);
    return out->volume;
}
} // namespace grid_tests

using namespace grid_tests;

TEST_CASE("grid: mesh to volume fills and shells")
{
    auto filled = run({N{"C", "MeshCube", {}, {}}, N{"V", "MeshToVolume", {}, {{"voxelSize", 5.0}, {"density", 2.0}}}},
                      {L{"C", "mesh", "V", "mesh"}, L{"V", "grid", "GroupOutput", "geometry"}});
    REQUIRE(filled->volume);
    const gn::Volume& f = *filled->volume;
    CHECK(f.kind == gn::Volume::Kind::Density);
    CHECK(f.sampleNearest({0, 0, 0}) == doctest::Approx(2.0));
    CHECK(f.sampleNearest({0, 0, 45}) == doctest::Approx(2.0));
    CHECK(f.sampleNearest({0, 0, 70}) == doctest::Approx(0.0));

    auto shell = run({N{"C", "MeshCube", {}, {}}, N{"V", "MeshToVolume", {}, {{"voxelSize", 5.0}, {"fill", false}, {"interiorBand", 10.0}}}},
                     {L{"C", "mesh", "V", "mesh"}, L{"V", "grid", "GroupOutput", "geometry"}});
    const gn::Volume& s = *shell->volume;
    CHECK(s.sampleNearest({0, 0, 45}) == doctest::Approx(1.0));
    CHECK(s.sampleNearest({0, 0, 0}) == doctest::Approx(0.0));
}

TEST_CASE("grid: points to volume")
{
    geo::Mesh pts;
    pts.addPoint({0, 0, 0});
    auto out = run({N{"P", "PointsToVolume", {}, {{"radius", 6.0}, {"voxelSize", 1.0}, {"density", 0.5}}}},
                   {L{"GroupInput", "geometry", "P", "points"}, L{"P", "grid", "GroupOutput", "geometry"}}, gn::pointGeometry(std::move(pts)));
    REQUIRE(out->volume);
    CHECK(out->volume->sampleNearest({0, 0, 0}) == doctest::Approx(0.5));
    CHECK(out->volume->sampleNearest({5, 0, 0}) == doctest::Approx(0.5));
    CHECK(out->volume->sampleNearest({0, 7, 0}) == doctest::Approx(0.0));
}

TEST_CASE("grid: field to grid, sample grid and grid to points")
{
    gn::Volume topo = densityOf([](glm::dvec3) { return 0.0f; }, 4.0);
    // Value = x of the voxel centre; vector = position.
    auto scalar = run({N{"P", "Position", {}, {}}, N{"S", "SeparateXYZ", {}, {}}, N{"F", "FieldToGrid", {}, {}}},
                      {L{"GroupInput", "geometry", "F", "topology"}, L{"P", "value", "S", "vector"}, L{"S", "x", "F", "value"},
                       L{"F", "grid", "GroupOutput", "geometry"}},
                      gn::volumeGeometry(topo));
    REQUIRE(scalar->volume);
    CHECK(scalar->volume->sample({2.5, 0, 0}) == doctest::Approx(2.5));

    N vecNode{"F", "FieldToGrid", {}, {}};
    vecNode.values["type"] = "vector";
    auto vector = run({N{"P", "Position", {}, {}}, vecNode},
                      {L{"GroupInput", "geometry", "F", "topology"}, L{"P", "value", "F", "vector"}, L{"F", "grid", "GroupOutput", "geometry"}},
                      gn::volumeGeometry(topo));
    REQUIRE(vector->volume);
    CHECK(vector->volume->isVector());
    glm::dvec3 s = vector->volume->sampleVector({1.5, -2.0, 3.0});
    CHECK(s.x == doctest::Approx(1.5));
    CHECK(s.y == doctest::Approx(-2.0));
    CHECK(s.z == doctest::Approx(3.0));
    CHECK(vector->volume->sample({3, 4, 0}) == doctest::Approx(5.0).epsilon(0.01));

    // Grid to Points keeps the voxels above the threshold and carries their value.
    gn::Volume ramp = densityOf([](glm::dvec3 p) { return p.x > 2.5 ? float(p.x) : 0.0f; }, 4.0);
    auto points = run({N{"G", "GridToPoints", {}, {}}, N{"A", "StoreNamedAttribute", {}, {{"name", "v"}}}},
                      {L{"GroupInput", "geometry", "G", "grid"}, L{"G", "points", "A", "geometry"}, L{"G", "value", "A", "value"},
                       L{"A", "geometry", "GroupOutput", "geometry"}},
                      gn::volumeGeometry(ramp));
    REQUIRE(points->points);
    CHECK(points->points->numPoints() == 2 * 9 * 9);
    auto v = points->points->pointAttrs().get<float>("v");
    REQUIRE(v.size() == 2 * 9 * 9);
    for (geo::Index p = 0; p < points->points->numPoints(); ++p)
        CHECK(v[p] == doctest::Approx(points->points->position(p).x));
}

TEST_CASE("grid: sample grid index")
{
    gn::Volume ramp = densityOf([](glm::dvec3 p) { return float(p.y); }, 4.0);
    geo::Mesh one;
    one.addPoint({0, 0, 0});
    gn::Geometry both;
    both.points = std::make_shared<const geo::Mesh>(std::move(one));
    both.volume = std::make_shared<const gn::Volume>(ramp);
    auto out = run({N{"I", "SampleGridIndex", {}, {{"x", 0}, {"y", 6}, {"z", 0}}}, N{"A", "StoreNamedAttribute", {}, {{"name", "v"}}}},
                   {L{"GroupInput", "geometry", "I", "grid"}, L{"GroupInput", "geometry", "A", "geometry"}, L{"I", "value", "A", "value"},
                    L{"A", "geometry", "GroupOutput", "geometry"}},
                   gn::makeGeometry(std::move(both)));
    REQUIRE(out->points);
    CHECK(out->points->pointAttrs().get<float>("v")[0] == doctest::Approx(2.0));
}

TEST_CASE("grid: gradient, laplacian, curl and divergence")
{
    // f = x^2 + 2y: gradient (2x, 2, 0), laplacian 2.
    gn::Volume f = densityOf([](glm::dvec3 p) { return float(p.x * p.x + 2.0 * p.y); }, 6.0, 0.5);
    auto grad = apply("GridGradient", f);
    CHECK(grad->isVector());
    glm::dvec3 g = grad->sampleVector({1.0, 0.5, 0.0});
    CHECK(g.x == doctest::Approx(2.0).epsilon(1e-3));
    CHECK(g.y == doctest::Approx(2.0).epsilon(1e-3));
    CHECK(g.z == doctest::Approx(0.0));
    auto lap = apply("GridLaplacian", f);
    CHECK(lap->sample({1.0, 1.0, 1.0}) == doctest::Approx(2.0).epsilon(1e-3));

    // Rotation about z: v = (-y, x, 0) has curl (0, 0, 2) and no divergence; v = position diverges by 3.
    gn::Volume swirl = vectorsOf([](glm::dvec3 p) { return glm::vec3(float(-p.y), float(p.x), 0.0f); });
    glm::dvec3 curl = apply("GridCurl", swirl)->sampleVector({1, 2, 0});
    CHECK(curl.x == doctest::Approx(0.0));
    CHECK(curl.z == doctest::Approx(2.0).epsilon(1e-4));
    CHECK(apply("GridDivergence", swirl)->sample({1, 2, 0}) == doctest::Approx(0.0));
    gn::Volume out = vectorsOf([](glm::dvec3 p) { return glm::vec3(p); });
    CHECK(apply("GridDivergence", out)->sample({1, 2, 3}) == doctest::Approx(3.0).epsilon(1e-4));

    // Scaled and rotated grids still give world-space derivatives.
    gn::Geometry moved = gn::transformed(*gn::volumeGeometry(f), glm::dmat4(glm::dvec4(0, 2, 0, 0), glm::dvec4(-2, 0, 0, 0), glm::dvec4(0, 0, 2, 0), glm::dvec4(0, 0, 0, 1)));
    auto movedGrad = apply("GridGradient", *moved.volume);
    // The field moved with the grid: world (x, y) holds f(y/2, -x/2) = y^2/4 - x, whose gradient at (2, 2) is (-1, 1).
    glm::dvec3 mg = movedGrad->sampleVector({2.0, 2.0, 0.0});
    CHECK(mg.x == doctest::Approx(-1.0).epsilon(1e-3));
    CHECK(mg.y == doctest::Approx(1.0).epsilon(1e-3));
}

TEST_CASE("grid: advect carries values along the velocity")
{
    gn::Volume blob = densityOf([](glm::dvec3 p) { return float(std::exp(-glm::dot(p, p) / 4.0)); });
    for (const char* mode : {"euler", "midpoint", "rk4"})
    {
        // The velocity is a constant (2, 0, 0) written onto the blob's own voxels.
        N wind{"W", "FieldToGrid", {}, {{"type", "vector"}, {"vector", json::array({2, 0, 0})}}};
        N a{"A", "AdvectGrid", {}, {{"time", 1.5}, {"substeps", 3}, {"integration", mode}}};
        auto out = run({wind, a},
                       {L{"GroupInput", "geometry", "W", "topology"}, L{"GroupInput", "geometry", "A", "grid"}, L{"W", "grid", "A", "velocity"},
                        L{"A", "grid", "GroupOutput", "geometry"}},
                       gn::volumeGeometry(blob));
        REQUIRE(out->volume);
        CAPTURE(mode);
        CHECK(out->volume->sample({3, 0, 0}) == doctest::Approx(1.0).epsilon(1e-3));
        CHECK(out->volume->sample({0, 0, 0}) == doctest::Approx(std::exp(-9.0 / 4.0)).epsilon(1e-3));
    }
}

TEST_CASE("grid: dilate, erode, mean and median")
{
    gn::Volume dot = densityOf([](glm::dvec3 p) { return glm::length(p) < 0.1 ? 1.0f : 0.0f; }, 4.0);
    auto grown = apply("GridDilate", dot, {{"steps", 2}});
    CHECK(grown->sampleNearest({2, 0, 0}) == doctest::Approx(1.0));
    CHECK(grown->sampleNearest({1, 1, 0}) == doctest::Approx(1.0));
    CHECK(grown->sampleNearest({1, 1, 1}) == doctest::Approx(0.0));
    auto cube = apply("GridDilate", dot, {{"steps", 1}}, {{"connectivity", "vertex"}});
    CHECK(cube->sampleNearest({1, 1, 1}) == doctest::Approx(1.0));
    auto back = apply("GridErode", *cube, {{"steps", 1}}, {{"connectivity", "vertex"}});
    CHECK(back->sampleNearest({0, 0, 0}) == doctest::Approx(1.0));
    CHECK(back->sampleNearest({1, 0, 0}) == doctest::Approx(0.0));

    // A signed distance grows outward under dilate.
    gn::Volume sphere = gn::pointsToSdf(std::vector<glm::dvec3>{glm::dvec3(0.0)}, std::vector<double>{3.0}, 1.0, 4);
    auto fat = apply("GridDilate", sphere, {{"steps", 1}});
    CHECK(fat->isSdf());
    CHECK(fat->sample({3.5, 0, 0}) < 0.0);

    auto mean = apply("GridMean", dot, {{"width", 1}});
    CHECK(mean->sampleNearest({0, 0, 0}) == doctest::Approx(1.0 / 27.0));
    CHECK(mean->sampleNearest({1, 1, 1}) == doctest::Approx(1.0 / 27.0));
    auto median = apply("GridMedian", dot, {{"width", 1}});
    CHECK(median->sampleNearest({0, 0, 0}) == doctest::Approx(0.0));
    gn::Volume block = densityOf([](glm::dvec3 p) { return p.x >= 0.0 ? 1.0f : 0.0f; }, 4.0);
    auto kept = apply("GridMedian", block, {{"width", 1}});
    CHECK(kept->sampleNearest({1, 0, 0}) == doctest::Approx(1.0));
    CHECK(kept->sampleNearest({-1, 0, 0}) == doctest::Approx(0.0));
}

TEST_CASE("grid: voxelize, prune and info")
{
    gn::Volume ramp = densityOf([](glm::dvec3 p) { return float(p.x); }, 4.0);
    auto coarse = apply("VoxelizeGrid", ramp, {{"voxelSize", 2.0}});
    CHECK(coarse->dims == glm::ivec3(5, 5, 5));
    CHECK(coarse->sample({2, 0, 0}) == doctest::Approx(2.0));

    gn::Volume dot = densityOf([](glm::dvec3 p) { return glm::length(p - glm::dvec3(2, 0, 0)) < 0.1 ? 1.0f : 0.0f; }, 4.0);
    auto pruned = apply("PruneGrid", dot);
    CHECK(pruned->dims == glm::ivec3(3, 3, 3));
    CHECK(pruned->sampleNearest({2, 0, 0}) == doctest::Approx(1.0));
    CHECK(pruned->bounds().min.x == doctest::Approx(1.0));

    gn::Volume sphere = gn::pointsToSdf(std::vector<glm::dvec3>{glm::dvec3(0.0)}, std::vector<double>{3.0}, 1.0, 2);
    auto band = apply("PruneGrid", sphere, {}, {{"mode", "sdf"}});
    CHECK(band->size() < sphere.size());
    CHECK(band->sample({3, 0, 0}) == doctest::Approx(sphere.sample({3, 0, 0})));

    auto info = run({N{"I", "GridInfo", {}, {}}, N{"P", "Points", {}, {{"count", 1}}}, N{"A", "StoreNamedAttribute", {}, {{"name", "bg"}}},
                     N{"B", "StoreNamedAttribute", {}, {{"name", "n"}}}},
                    {L{"GroupInput", "geometry", "I", "grid"}, L{"P", "points", "A", "geometry"}, L{"I", "background", "A", "value"},
                     L{"A", "geometry", "B", "geometry"}, L{"I", "activeVoxels", "B", "value"}, L{"B", "geometry", "GroupOutput", "geometry"}},
                    gn::volumeGeometry(sphere));
    REQUIRE(info->points);
    auto bg = info->points->pointAttrs().get<float>("bg");
    REQUIRE(bg.size() == 1);
    CHECK(bg[0] == doctest::Approx(2.0));
}

TEST_CASE("grid: vector grids survive transforms, joins and saving")
{
    gn::Volume v = vectorsOf([](glm::dvec3) { return glm::vec3(1.0f, 0.0f, 0.0f); }, 2.0);
    gn::Geometry turned = gn::transformed(*gn::volumeGeometry(v), glm::dmat4(glm::dvec4(0, 1, 0, 0), glm::dvec4(-1, 0, 0, 0), glm::dvec4(0, 0, 1, 0), glm::dvec4(0, 0, 0, 1)));
    glm::dvec3 t = turned.volume->sampleVector({0, 0, 0});
    CHECK(t.x == doctest::Approx(0.0));
    CHECK(t.y == doctest::Approx(1.0));

    gn::GeometryPtr parts[2] = {gn::volumeGeometry(v), gn::volumeGeometry(v)};
    gn::Geometry joined = gn::join(parts);
    CHECK(joined.volume->sampleVector({0, 0, 0}).x == doctest::Approx(2.0));
    CHECK(joined.volume->sample({0, 0, 0}) == doctest::Approx(2.0));

    gn::GeometryPtr back = gn::geometryFromJson(gn::geometryToJson(*gn::volumeGeometry(v)));
    REQUIRE(back->volume);
    CHECK(back->volume->isVector());
    CHECK(back->volume->vectors.size() == v.vectors.size());
    CHECK(back->volume->sampleVector({0.5, 0, 0}).x == doctest::Approx(1.0));
}
