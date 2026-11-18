#include "TexPaintTest.h"

#include "geo/Bvh.h"
#include "render/PaintGpu.h"
#include "tex/PaintParticles.h"
#include "tex/PaintSurface.h"

#include <chrono>
#include <cmath>

using namespace textest;
namespace tp = lila::tex::paint;
namespace gp = lila::render::paint;

namespace
{
/** A wall in x and y facing +z, n x n quads over 100 units, its height rippled by bumps (0 for flat). */
std::shared_ptr<const tp::PaintMesh> bumpyWall(int n, double bumps)
{
    std::vector<glm::dvec3> pts;
    std::vector<geo::Index> sizes, idx;
    pts.reserve(size_t(n + 1) * (n + 1));
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
        {
            const double x = -50.0 + 100.0 * i / n, y = -50.0 + 100.0 * j / n;
            pts.push_back({x, y, bumps * std::sin(x * 0.31) * std::cos(y * 0.23)});
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const geo::Index a = geo::Index(j * (n + 1) + i);
            sizes.push_back(4);
            idx.insert(idx.end(), {a, geo::Index(a + 1), geo::Index(a + n + 2), geo::Index(a + n + 1)});
        }
    auto mesh = std::make_shared<geo::Mesh>(geo::Mesh::fromPolygons(pts, sizes, idx));
    auto uv = mesh->cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < mesh->numFaces(); ++f)
        for (geo::Index c : mesh->faceCorners(f))
        {
            const glm::dvec3 p = mesh->position(mesh->cornerPoint(c));
            uv[c] = {float((p.x + 50.0) / 100.0), float((p.y + 50.0) / 100.0)};
        }
    static std::vector<std::shared_ptr<geo::Mesh>> keep;
    keep.push_back(mesh);
    return tp::PaintMesh::build(*mesh, std::make_shared<const geo::MeshBvh>(*mesh), "");
}

/** Brush dabs along a line across the wall's upper half. */
std::vector<tp::Dab> strokeDabs(const tp::PaintMesh& mesh, int count, float radius)
{
    std::vector<tp::Dab> out;
    for (int i = 0; i < count; ++i)
    {
        const double x = -40.0 + 80.0 * i / std::max(count - 1, 1);
        geo::BvhClosest hit;
        REQUIRE(mesh.bvh->closestPoint({x, 25.0, 0.0}, hit, 1e9));
        tp::Dab d;
        d.triangle = uint32_t(hit.triangle);
        d.u = float(hit.bary.y);
        d.v = float(hit.bary.z);
        d.radius = radius;
        d.opacity = 0.8f;
        out.push_back(d);
    }
    return out;
}

const char* const kFlowBrushes[] = {"rustRun", "leak", "rainStreaks", "dustSettle", "burn", "scorch"};
} // namespace

TEST_CASE("particle brushes on the GPU walk the same paths as the CPU reference")
{
    if (!gpu())
        return;
    const auto mesh = bumpyWall(24, 3.0);
    const std::vector<tp::Dab> dabs = strokeDabs(*mesh, 12, 3.0f);
    for (const char* kind : {"drips", "rain", "sparks", "flow", "rustRun", "leak", "rainStreaks", "dustSettle", "burn", "scorch"})
    {
        CAPTURE(kind);
        const tp::ParticleSpec spec = tp::ParticleSpec::fromJson({{"kind", kind}, {"seed", 7}, {"rate", 1.0}});
        const std::vector<tp::Dab> cpu = tp::withParticles(*mesh, dabs, 3, spec);
        const std::optional<std::vector<tp::Dab>> gpuDabs = gp::withParticlesGpu(mesh, dabs, 3, spec);
        REQUIRE(gpuDabs);
        // Float rounding may end a walk a step early or late; the paths themselves agree.
        CHECK(std::abs(double(gpuDabs->size()) - double(cpu.size())) <= 0.02 * double(cpu.size()) + 2.0);
        const size_t n = std::min(cpu.size(), gpuDabs->size());
        size_t close = 0;
        for (size_t i = 0; i < n; ++i)
            if (glm::distance(mesh->point(cpu[i]), mesh->point((*gpuDabs)[i])) < 0.05f && std::abs(cpu[i].radius - (*gpuDabs)[i].radius) < 1e-3f)
                ++close;
        CHECK(double(close) >= 0.95 * double(n));
        CHECK(cpu.size() > dabs.size());
    }
}

TEST_CASE("particle brushes: flow brush timing on a 500k-triangle mesh, CPU and GPU")
{
    if (!gpu())
        return;
    const auto mesh = bumpyWall(500, 4.0);
    REQUIRE(mesh->triangleCount() == 500000);
    const std::vector<tp::Dab> dabs = strokeDabs(*mesh, 200, 2.0f);
    for (const char* kind : kFlowBrushes)
    {
        const tp::ParticleSpec spec = tp::ParticleSpec::fromJson({{"kind", kind}, {"seed", 1}});
        auto t0 = std::chrono::steady_clock::now();
        const std::vector<tp::Dab> cpu = tp::withParticles(*mesh, dabs, 0, spec);
        auto t1 = std::chrono::steady_clock::now();
        gp::withParticlesGpu(mesh, dabs, 0, spec); // uploads the mesh and builds the pipeline
        auto t2 = std::chrono::steady_clock::now();
        const std::optional<std::vector<tp::Dab>> g = gp::withParticlesGpu(mesh, dabs, 0, spec);
        auto t3 = std::chrono::steady_clock::now();
        REQUIRE(g);
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        MESSAGE(std::string(kind) << ": CPU " << ms(t0, t1) << " ms, GPU " << ms(t2, t3) << " ms (first call " << ms(t1, t2) << " ms), dabs "
                     << cpu.size() << " / " << g->size());
    }
}
