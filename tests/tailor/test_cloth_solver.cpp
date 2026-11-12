#include <doctest/doctest.h>

#include "TailorCore.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace tailor;

namespace
{
Vec3 vAdd(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 vScale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float vLen(const Vec3& a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
float vDist(const Vec3& a, const Vec3& b) { return vLen(vAdd(a, vScale(b, -1.f))); }

bool allFinite(const std::vector<Vec3>& xs)
{
    for (const Vec3& p : xs)
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            return false;
    return true;
}

/** Appends an nu x nv grid panel: pattern (u, v) in metres, placed at origin + right * u + up * v. Returns its first vertex. */
int clothGrid(ClothMesh& mesh, int panel, int nu, int nv, float width, float height, const Vec3& origin, const Vec3& right,
            const Vec3& up)
{
    const int base = int(mesh.rest3d.size());
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i)
        {
            const float u = width * float(i) / float(nu - 1), v = height * float(j) / float(nv - 1);
            mesh.uv.push_back({u, v});
            mesh.rest3d.push_back(vAdd(origin, vAdd(vScale(right, u), vScale(up, v))));
            mesh.vertPanel.push_back(panel);
        }
    auto id = [&](int i, int j) { return base + j * nu + i; };
    for (int j = 0; j + 1 < nv; ++j)
        for (int i = 0; i + 1 < nu; ++i)
        {
            // Alternate the diagonal so the grid has no preferred shear direction.
            if ((i + j) % 2 == 0)
            {
                mesh.tris.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
                mesh.tris.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
            }
            else
            {
                mesh.tris.push_back({id(i, j), id(i + 1, j), id(i, j + 1)});
                mesh.tris.push_back({id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)});
            }
            mesh.triPanel.push_back(panel);
            mesh.triPanel.push_back(panel);
        }
    return base;
}

/** UV sphere mesh, outward facing. */
void clothSphere(float radius, int rings, int segments, std::vector<Vec3>& points, std::vector<std::array<int, 3>>& tris)
{
    constexpr float kPi = 3.14159265358979f;
    points.push_back({0.f, radius, 0.f});
    for (int r = 1; r < rings; ++r)
    {
        const float phi = kPi * float(r) / float(rings);
        for (int s = 0; s < segments; ++s)
        {
            const float theta = 2.f * kPi * float(s) / float(segments);
            points.push_back({radius * std::sin(phi) * std::cos(theta), radius * std::cos(phi),
                              radius * std::sin(phi) * std::sin(theta)});
        }
    }
    points.push_back({0.f, -radius, 0.f});
    const int bottom = int(points.size()) - 1;
    auto ring = [&](int r, int s) { return 1 + (r - 1) * segments + (s % segments); };
    for (int s = 0; s < segments; ++s)
    {
        tris.push_back({0, ring(1, s + 1), ring(1, s)});
        tris.push_back({bottom, ring(rings - 1, s), ring(rings - 1, s + 1)});
    }
    for (int r = 1; r + 1 < rings; ++r)
        for (int s = 0; s < segments; ++s)
        {
            tris.push_back({ring(r, s), ring(r, s + 1), ring(r + 1, s + 1)});
            tris.push_back({ring(r, s), ring(r + 1, s + 1), ring(r + 1, s)});
        }
}
} // namespace

TEST_CASE("fabric presets")
{
    CHECK(fabricPresets().size() == 19);
    CHECK(findFabric("Spandex")->stretchWarp < findFabric("Cotton")->stretchWarp);
    CHECK(findFabric("plush")->thickness > findFabric("Cotton")->thickness);
    REQUIRE(findFabric("denim") != nullptr);
    CHECK(findFabric("Denim")->name == "Denim");
    CHECK(findFabric("Heavy")->density > findFabric("Chiffon")->density);
    CHECK(findFabric("Velvet") == nullptr);
}

TEST_CASE("hanging pinned sheet sags and stays stable")
{
    const int nu = 21, nv = 21;
    ClothMesh mesh;
    clothGrid(mesh, 0, nu, nv, 0.5f, 0.5f, {-0.25f, 1.f, -0.25f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    Garment g;
    g.panels.resize(1);
    g.fabrics = {*findFabric("Cotton")};

    ClothSolver solver;
    solver.reset(mesh, g, SolverSettings{});
    const std::vector<Pin> pins{{0, mesh.rest3d[0]}, {nu - 1, mesh.rest3d[nu - 1]}};
    solver.setPins(pins);

    auto freeEdgeY = [&] {
        float sum = 0.f;
        for (int i = 0; i < nu; ++i)
            sum += solver.positions()[(nv - 1) * nu + i][1];
        return sum / float(nu);
    };

    float lateFreeEdge = 0.f;
    std::vector<Vec3> before;
    for (int frame = 0; frame < 240; ++frame)
    {
        before = solver.positions();
        solver.step();
        if (frame >= 180)
            lateFreeEdge += freeEdgeY() / 60.f;
    }
    const std::vector<Vec3>& x = solver.positions();
    REQUIRE(allFinite(x));

    for (const Pin& p : pins)
        CHECK(vDist(x[p.vertex], p.target) < 0.01f);
    CHECK(lateFreeEdge < 0.85f);

    float maxMove = 0.f, maxReach = 0.f;
    for (size_t i = 0; i < x.size(); ++i)
    {
        maxMove = std::max(maxMove, vDist(x[i], before[i]));
        maxReach = std::max(maxReach, vDist(x[i], {0.f, 1.f, -0.25f}));
    }
    CHECK(maxMove < 0.08f);
    CHECK(maxReach < 0.9f);

    const std::vector<float> strain = solver.strainMap();
    REQUIRE(strain.size() == x.size());
    CHECK(*std::max_element(strain.begin(), strain.end()) < 1.3f);
}

TEST_CASE("sheet draped on a sphere stays outside it")
{
    const float radius = 0.3f;
    std::vector<Vec3> points;
    std::vector<std::array<int, 3>> tris;
    clothSphere(radius, 24, 48, points, tris);
    const Sdf sdf = sdfFromMesh(points, tris, 0.02f, 0.1f);
    CHECK(sdf.sample({0.f, 0.f, 0.f}) < -0.25f);
    CHECK(sdf.sample({0.f, 0.5f, 0.f}) == doctest::Approx(0.2f).epsilon(0.1));
    CHECK(sdf.sample({0.4f, 0.f, 0.f}) > 0.05f);
    Vec3 grad;
    sdf.sample({0.f, 0.32f, 0.f}, &grad);
    CHECK(grad[1] / vLen(grad) > 0.9f);

    const int nu = 25, nv = 25;
    ClothMesh mesh;
    clothGrid(mesh, 0, nu, nv, 0.8f, 0.8f, {-0.4f, 0.36f, -0.4f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    Garment g;
    g.panels.resize(1);
    g.fabrics = {*findFabric("Cotton")};

    ClothSolver solver;
    solver.reset(mesh, g, SolverSettings{});
    solver.setColliders({&sdf});
    float minRadius = 1.f;
    for (int frame = 0; frame < 120; ++frame)
    {
        solver.step();
        for (const Vec3& p : solver.positions())
            minRadius = std::min(minRadius, vLen(p));
    }
    const std::vector<Vec3>& x = solver.positions();
    REQUIRE(allFinite(x));
    CHECK(minRadius > radius - 0.01f);

    const int corners[4] = {0, nu - 1, (nv - 1) * nu, nv * nu - 1};
    for (int c : corners)
        CHECK(x[c][1] < 0.2f);
    const int centre = (nv / 2) * nu + nu / 2;
    CHECK(x[centre][1] > radius - 0.01f);
    CHECK(std::abs(x[centre][0]) < 0.05f);
}

TEST_CASE("two panels sewn into a tube close their seam")
{
    const int nu = 17, nv = 13;
    const float width = 0.4f, height = 0.3f;
    ClothMesh mesh;
    const int front = clothGrid(mesh, 0, nu, nv, width, height, {-0.2f, 0.f, 0.15f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    const int back = clothGrid(mesh, 1, nu, nv, width, height, {-0.2f, 0.f, -0.15f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    for (int j = 0; j < nv; ++j)
    {
        mesh.seamPairs.push_back({front + j * nu, back + j * nu});
        mesh.seamPairs.push_back({front + j * nu + nu - 1, back + j * nu + nu - 1});
        mesh.seamRestLength.push_back(0.f);
        mesh.seamRestLength.push_back(0.f);
    }
    Garment g;
    g.panels.resize(2);
    g.panels[1].name = "Back";
    g.fabrics = {*findFabric("Cotton")};

    SolverSettings settings;
    settings.gravity = {0.f, 0.f, 0.f};
    ClothSolver solver;
    solver.reset(mesh, g, settings);
    for (int frame = 0; frame < 120; ++frame)
        solver.step();
    const std::vector<Vec3>& x = solver.positions();
    REQUIRE(allFinite(x));

    float maxGap = 0.f;
    for (const auto& pr : mesh.seamPairs)
        maxGap = std::max(maxGap, vDist(x[pr[0]], x[pr[1]]));
    CHECK(maxGap < 0.005f);

    // Still a tube: the two middles stay apart instead of collapsing onto each other.
    const int mid = (nv / 2) * nu + nu / 2;
    CHECK(vDist(x[front + mid], x[back + mid]) > 0.05f);

    const std::vector<float> strain = solver.strainMap();
    CHECK(*std::max_element(strain.begin(), strain.end()) < 1.3f);
}

TEST_CASE("solver is deterministic")
{
    ClothMesh mesh;
    clothGrid(mesh, 0, 11, 11, 0.3f, 0.3f, {-0.15f, 0.5f, -0.15f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    Garment g;
    g.panels.resize(1);
    auto run = [&] {
        ClothSolver solver;
        solver.reset(mesh, g, SolverSettings{});
        solver.setPins({{0, mesh.rest3d[0]}});
        for (int frame = 0; frame < 30; ++frame)
            solver.step();
        return solver.positions();
    };
    const std::vector<Vec3> a = run(), b = run();
    REQUIRE(a.size() == b.size());
    bool same = true;
    for (size_t i = 0; i < a.size(); ++i)
        same = same && a[i] == b[i];
    CHECK(same);
}

TEST_CASE("cloth rests on the floor")
{
    ClothMesh mesh;
    clothGrid(mesh, 0, 11, 11, 0.4f, 0.4f, {-0.2f, 0.3f, -0.2f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    Garment g;
    g.panels.resize(1);
    SolverSettings s;
    s.floorY = 0.1f;
    ClothSolver solver;
    solver.reset(mesh, g, s);
    for (int frame = 0; frame < 90; ++frame)
        solver.step();
    float lowest = 1.f;
    for (const Vec3& p : solver.positions())
        lowest = std::min(lowest, p[1]);
    CHECK(lowest > 0.1f - 0.002f);
    CHECK(lowest < 0.12f);
}

TEST_CASE("crossing edges collide where no vertex meets a triangle")
{
    // Two ribbons a centimetre wide, crossed: their points all lie well away from the other ribbon, so only their edges meet.
    ClothMesh mesh;
    clothGrid(mesh, 0, 12, 2, 1.1f, 0.01f, {-0.55f, 0.f, -0.005f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const int b = clothGrid(mesh, 1, 12, 2, 1.1f, 0.01f, {-0.005f, 0.03f, -0.55f}, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f});
    Garment g;
    g.panels.resize(2);
    std::vector<Pin> pins;
    for (int i = 0; i < b; ++i)
        pins.push_back({i, mesh.rest3d[size_t(i)]});
    ClothSolver solver;
    solver.reset(mesh, g, SolverSettings{});
    solver.setPins(pins);
    for (int frame = 0; frame < 60; ++frame)
        solver.step();
    REQUIRE(allFinite(solver.positions()));
    // Ribbon B hangs over A: its points either side of the crossing stay up by it.
    float lowest = 1.f;
    for (size_t i = size_t(b); i < mesh.rest3d.size(); ++i)
        if (std::abs(solver.positions()[i][2]) < 0.06f)
            lowest = std::min(lowest, solver.positions()[i][1]);
    CHECK(lowest > -0.15f);
    CHECK(lowest < 0.02f);
}

TEST_CASE("plain mesh with no pattern drapes at its own edge lengths")
{
    const float radius = 0.3f;
    std::vector<Vec3> points;
    std::vector<std::array<int, 3>> tris;
    clothSphere(radius, 24, 48, points, tris);
    const Sdf sdf = sdfFromMesh(points, tris, 0.02f, 0.1f);

    const int nu = 21, nv = 21;
    ClothMesh mesh;
    clothGrid(mesh, 0, nu, nv, 0.8f, 0.8f, {-0.4f, 0.36f, -0.4f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    mesh.uv.clear();
    mesh.vertPanel.clear();
    mesh.triPanel.clear();
    const ClothMesh rest = mesh;
    Garment g;

    ClothSolver solver;
    SolverSettings s;
    s.substeps = 4;
    solver.reset(mesh, g, s);
    solver.setColliders({&sdf});
    for (int frame = 0; frame < 150; ++frame)
        solver.step();
    const std::vector<Vec3>& x = solver.positions();
    REQUIRE(allFinite(x));
    for (const Vec3& p : x)
        CHECK(vLen(p) > radius - 0.01f);
    CHECK(x[0][1] < 0.2f);
    float worst = 0.f;
    for (const auto& t : rest.tris)
        for (int k = 0; k < 3; ++k)
        {
            const float r = vDist(rest.rest3d[t[k]], rest.rest3d[t[(k + 1) % 3]]);
            worst = std::max(worst, vDist(x[t[k]], x[t[(k + 1) % 3]]) / r);
        }
    CHECK(worst < 1.15f);
}

TEST_CASE("a clipped distance field matches the full one inside its box")
{
    std::vector<Vec3> points;
    std::vector<std::array<int, 3>> tris;
    clothSphere(0.5f, 24, 48, points, tris);
    const Sdf full = sdfFromMesh(points, tris, 0.02f, 0.1f);
    const Vec3 lo{-0.15f, 0.3f, -0.15f}, hi{0.15f, 0.7f, 0.15f};
    const Sdf clipped = sdfFromMesh(points, tris, 0.02f, 0.1f, &lo, &hi);
    CHECK(size_t(clipped.nx) * clipped.ny * clipped.nz < size_t(full.nx) * full.ny * full.nz / 10);
    for (const Vec3& p : {Vec3{0.f, 0.45f, 0.f}, Vec3{0.05f, 0.52f, -0.05f}, Vec3{-0.1f, 0.6f, 0.1f}})
        CHECK(clipped.sample(p) == doctest::Approx(full.sample(p)).epsilon(0.05));
    const Vec3 farLo{3.f, 3.f, 3.f}, farHi{4.f, 4.f, 4.f};
    CHECK(sdfFromMesh(points, tris, 0.02f, 0.1f, &farLo, &farHi).nx == 0);
}
