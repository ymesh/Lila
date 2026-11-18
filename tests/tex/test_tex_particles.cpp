#include "TexTest.h"

#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "tex/PaintParticles.h"
#include "tex/PaintSurface.h"

using namespace textest;
using namespace lila::tex::paint;

namespace
{
/** A wall: a 100 x 100 plane standing in x and y, facing +z. */
std::shared_ptr<const PaintMesh> wall()
{
    static geo::Mesh m = [] {
        const int n = 8;
        std::vector<glm::dvec3> pts;
        std::vector<geo::Index> sizes, idx;
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i)
                pts.push_back({-50.0 + 100.0 * i / n, -50.0 + 100.0 * j / n, 0.0});
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
            {
                const geo::Index a = geo::Index(j * (n + 1) + i);
                sizes.push_back(4);
                idx.insert(idx.end(), {a, geo::Index(a + 1), geo::Index(a + n + 2), geo::Index(a + n + 1)});
            }
        geo::Mesh mesh = geo::Mesh::fromPolygons(pts, sizes, idx);
        auto uv = mesh.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        for (geo::Index f = 0; f < mesh.numFaces(); ++f)
            for (geo::Index c : mesh.faceCorners(f))
            {
                const glm::dvec3 p = mesh.position(mesh.cornerPoint(c));
                uv[c] = {float((p.x + 50.0) / 100.0), float((p.y + 50.0) / 100.0)};
            }
        return mesh;
    }();
    return PaintMesh::build(m, std::make_shared<const geo::MeshBvh>(m), "");
}

Dab dabAt(const PaintMesh& mesh, glm::dvec3 p, float radius)
{
    geo::BvhClosest hit;
    REQUIRE(mesh.bvh->closestPoint(p, hit, 1e9));
    Dab d;
    d.triangle = uint32_t(hit.triangle);
    d.u = float(hit.bary.y);
    d.v = float(hit.bary.z);
    d.radius = radius;
    return d;
}
} // namespace

TEST_CASE("tex particles: flow particles on a wall run down, the same seed gives the same dabs, presets parse")
{
    const auto mesh = wall();
    const Dab start = dabAt(*mesh, {0.0, 30.0, 0.0}, 3.0f);
    ParticleSpec spec = ParticleSpec::fromJson({{"kind", "leak"}, {"seed", 4}, {"rate", 1.0}});
    CHECK(spec.gravity == doctest::Approx(1.0f));
    CHECK(spec.pool > 1.0f);
    const std::vector<Dab> a = emitParticles(*mesh, start, 0, spec);
    REQUIRE(a.size() > 5);
    CHECK(a == emitParticles(*mesh, start, 0, spec));
    // Down the wall: the last dabs sit well below the start.
    CHECK(mesh->point(a.back()).y < 30.0f - 3.0f * 3.0f);
    float lowest = 1e9f;
    for (const Dab& d : a)
        lowest = std::min(lowest, mesh->point(d).y);
    CHECK(lowest < 10.0f);

    spec.seed = 5;
    CHECK(emitParticles(*mesh, start, 0, spec) != a);

    // Dust on a wall finds nothing flat to settle on: friction 0.65 is under a wall's pull, so it slides.
    const std::vector<Dab> dust = emitParticles(*mesh, start, 0, ParticleSpec::fromJson({{"kind", "dustSettle"}, {"rate", 1.0}}));
    CHECK(!dust.empty());
    // Scorch without gravity spreads out, not down.
    const std::vector<Dab> scorch = emitParticles(*mesh, start, 0, ParticleSpec::fromJson({{"kind", "scorch"}, {"rate", 1.0}, {"seed", 2}}));
    REQUIRE(!scorch.empty());
    float sumY = 0.0f;
    for (const Dab& d : scorch)
        sumY += mesh->point(d).y;
    CHECK(std::abs(sumY / float(scorch.size()) - 30.0f) < 8.0f);

    for (const char* k : {"rustRun", "leak", "rainStreaks", "dustSettle", "burn", "scorch", "flow"})
        CHECK_NOTHROW(ParticleSpec::fromJson({{"kind", k}}));
    CHECK_THROWS(ParticleSpec::fromJson({{"kind", "lava"}}));
}
