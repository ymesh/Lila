#include <doctest/doctest.h>

#include "Blocks.h"
#include "ClothGpu.h"
#include "TailorCore.h"

#include "render/Renderer.h"

#include <chrono>
#include <cmath>

using namespace tailor;

namespace
{
bool gpuAvailable()
{
    auto& r = lila::render::Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for Tailor GPU tests");
    return false;
}

float dist(const Vec3& a, const Vec3& b) { return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }

ClothMesh grid(int nu, int nv, float w, float h, const Vec3& origin)
{
    ClothMesh m;
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i)
        {
            const float u = w * float(i) / float(nu - 1), v = h * float(j) / float(nv - 1);
            m.uv.push_back({u, v});
            m.rest3d.push_back({origin[0] + u, origin[1], origin[2] + v});
            m.vertPanel.push_back(0);
        }
    for (int j = 0; j + 1 < nv; ++j)
        for (int i = 0; i + 1 < nu; ++i)
        {
            const int a = j * nu + i;
            if ((i + j) % 2 == 0)
            {
                m.tris.push_back({a, a + 1, a + nu + 1});
                m.tris.push_back({a, a + nu + 1, a + nu});
            }
            else
            {
                m.tris.push_back({a, a + 1, a + nu});
                m.tris.push_back({a + 1, a + nu + 1, a + nu});
            }
            m.triPanel.push_back(0);
            m.triPanel.push_back(0);
        }
    return m;
}
} // namespace

TEST_CASE("gpu: hanging sheet settles where the CPU solver's does")
{
    if (!gpuAvailable())
        return;
    const int nu = 21, nv = 21;
    const ClothMesh m = grid(nu, nv, 0.5f, 0.5f, {-0.25f, 1.f, -0.25f});
    Garment g;
    g.panels.resize(1);
    const std::vector<Pin> pins{{0, m.rest3d[0]}, {nu - 1, m.rest3d[nu - 1]}};

    ClothSolver cpu;
    cpu.reset(m, g, SolverSettings{});
    cpu.setPins(pins);
    ClothGpu gpuSolver;
    std::string err;
    REQUIRE_MESSAGE(gpuSolver.reset(m, g, SolverSettings{}, &err), err);
    gpuSolver.setPins(pins);
    for (int f = 0; f < 240; ++f)
    {
        cpu.step();
        REQUIRE_MESSAGE(gpuSolver.step(&err), err);
    }
    const auto &a = cpu.positions(), &b = gpuSolver.positions();
    REQUIRE(a.size() == b.size());
    float sum = 0.f, edge = 0.f;
    for (size_t i = 0; i < a.size(); ++i)
    {
        REQUIRE(std::isfinite(b[i][0]));
        sum += dist(a[i], b[i]);
    }
    for (int i = 0; i < nu; ++i)
        edge += b[size_t((nv - 1) * nu + i)][1] / float(nu);
    CHECK(sum / float(a.size()) < 0.02f);
    CHECK(edge < 0.85f);
    for (const Pin& p : pins)
        CHECK(dist(b[size_t(p.vertex)], p.target) < 0.01f);
}

TEST_CASE("gpu: tunic drapes on the mannequin without passing through")
{
    if (!gpuAvailable())
        return;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(Body{}, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);
    Garment g;
    REQUIRE(buildBlock("tunic", g));
    for (Panel& p : g.panels)
        p.particleDistance = 0.03f;
    const ClothMesh cm = buildClothMesh(g);
    ClothGpu solver;
    std::string err;
    solver.setColliders({&sdf});
    REQUIRE_MESSAGE(solver.reset(cm, g, SolverSettings{}, &err), err);
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < 90; ++f)
        REQUIRE_MESSAGE(solver.step(&err), err);
    MESSAGE(cm.rest3d.size() << " verts: " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 90.0
                             << " ms/frame");
    int inside = 0;
    float lowest = 10.f;
    for (const Vec3& p : solver.positions())
    {
        REQUIRE(std::isfinite(p[0]));
        inside += sdf.sample(p) < -0.005f;
        lowest = std::min(lowest, p[1]);
    }
    CHECK(inside < int(cm.rest3d.size()) / 100);
    CHECK(lowest > 0.4f);
    float maxGap = 0.f;
    for (const auto& pr : cm.seamPairs)
        maxGap = std::max(maxGap, dist(solver.positions()[size_t(pr[0])], solver.positions()[size_t(pr[1])]));
    CHECK(maxGap < 0.01f);

    ClothGpu again;
    again.setColliders({&sdf});
    REQUIRE_MESSAGE(again.reset(cm, g, SolverSettings{}, &err), err);
    for (int f = 0; f < 90; ++f)
        REQUIRE_MESSAGE(again.step(&err), err);
    CHECK(again.positions() == solver.positions());
}

TEST_CASE("perf: gpu outfit drape" * doctest::skip())
{
    if (!gpuAvailable())
        return;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(Body{}, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);
    Garment g;
    REQUIRE(buildBlock("cloakTunicBelt", g));
    const ClothMesh cm = buildClothMesh(g);
    ClothGpu solver;
    std::string err;
    solver.setColliders({&sdf});
    REQUIRE_MESSAGE(solver.reset(cm, g, SolverSettings{}, &err), err);
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < 20; ++f)
        REQUIRE_MESSAGE(solver.step(&err), err);
    MESSAGE(cm.rest3d.size() << " verts, " << solver.colourCount() << " colours: "
                             << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20.0 << " ms/frame");
}

TEST_CASE("gpu: a folded seam folds like on the CPU")
{
    if (!gpuAvailable())
        return;
    // Two 0.2 x 0.3 m panels side by side, sewn along x = 0 at 90 degrees.
    const int nu = 7, nv = 9;
    ClothMesh m;
    for (int panel = 0; panel < 2; ++panel)
    {
        const ClothMesh half = grid(nu, nv, 0.2f, 0.3f, {panel == 0 ? -0.2f : 0.f, 0.f, 0.f});
        const int base = int(m.rest3d.size());
        for (size_t i = 0; i < half.rest3d.size(); ++i)
        {
            m.rest3d.push_back({half.rest3d[i][0], half.rest3d[i][2], 0.f});
            m.uv.push_back(half.uv[i]);
            m.vertPanel.push_back(panel);
        }
        for (const auto& t : half.tris)
        {
            m.tris.push_back({base + t[0], base + t[1], base + t[2]});
            m.triPanel.push_back(panel);
        }
    }
    for (int j = 0; j < nv; ++j)
    {
        m.seamPairs.push_back({j * nu + nu - 1, nu * nv + j * nu});
        m.seamRestLength.push_back(0.f);
        m.seamOfPair.push_back(0);
    }
    Garment g;
    g.panels.resize(2);
    g.fabrics = {*findFabric("Denim")};
    g.seams.resize(1);
    g.seams[0].foldAngle = 1.5707963f;
    SolverSettings s;
    s.gravity = {0.f, 0.f, 0.f};
    for (Vec3& p : m.rest3d)
        p[2] += p[0] > 0.f ? 0.01f * p[0] : 0.f;

    ClothSolver cpu;
    cpu.reset(m, g, s);
    ClothGpu gs;
    std::string err;
    REQUIRE_MESSAGE(gs.reset(m, g, s, &err), err);
    for (int f = 0; f < 300; ++f)
    {
        cpu.step();
        REQUIRE_MESSAGE(gs.step(&err), err);
    }
    float sum = 0.f;
    for (size_t i = 0; i < m.rest3d.size(); ++i)
        sum += dist(cpu.positions()[i], gs.positions()[i]);
    CHECK(sum / float(m.rest3d.size()) < 0.01f);
}

TEST_CASE("gpu: crossing edges collide where no vertex meets a triangle")
{
    if (!gpuAvailable())
        return;
    // Two crossed ribbons a centimetre wide, every point well away from the other ribbon: only their edges can meet.
    ClothMesh m;
    auto ribbon = [&](int panel, const Vec3& origin, const Vec3& along, const Vec3& across) {
        const int base = int(m.rest3d.size());
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 12; ++i)
            {
                const float u = 1.1f * float(i) / 11.f, v = 0.01f * float(j);
                m.uv.push_back({u, v});
                m.rest3d.push_back({origin[0] + along[0] * u + across[0] * v, origin[1] + along[1] * u + across[1] * v,
                                    origin[2] + along[2] * u + across[2] * v});
                m.vertPanel.push_back(panel);
            }
        for (int i = 0; i < 11; ++i)
        {
            m.tris.push_back({base + i, base + i + 1, base + 12 + i + 1});
            m.tris.push_back({base + i, base + 12 + i + 1, base + 12 + i});
            m.triPanel.push_back(panel);
            m.triPanel.push_back(panel);
        }
        return base;
    };
    ribbon(0, {-0.55f, 0.f, -0.005f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f});
    const int b = ribbon(1, {-0.005f, 0.03f, -0.55f}, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f});
    Garment g;
    g.panels.resize(2);
    std::vector<Pin> pins;
    for (int i = 0; i < b; ++i)
        pins.push_back({i, m.rest3d[size_t(i)]});
    ClothGpu solver;
    std::string err;
    REQUIRE_MESSAGE(solver.reset(m, g, SolverSettings{}, &err), err);
    solver.setPins(pins);
    for (int f = 0; f < 60; ++f)
        REQUIRE_MESSAGE(solver.step(&err), err);
    float lowest = 1.f;
    for (size_t i = size_t(b); i < m.rest3d.size(); ++i)
    {
        REQUIRE(std::isfinite(solver.positions()[i][0]));
        if (std::abs(solver.positions()[i][2]) < 0.06f)
            lowest = std::min(lowest, solver.positions()[i][1]);
    }
    CHECK(lowest > -0.15f);
    CHECK(lowest < 0.02f);
}
