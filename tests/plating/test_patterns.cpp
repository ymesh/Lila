#include "Patterns.h"

#include "geo/Primitives.h"

#include <doctest/doctest.h>

using namespace lila;
using namespace lila::layout;

namespace
{
std::vector<glm::dvec3> patternLine(glm::dvec3 a, glm::dvec3 b)
{
    std::vector<glm::dvec3> pts;
    const int n = std::max(1, int(glm::length(b - a) / 0.25));
    for (int i = 0; i <= n; ++i)
        pts.push_back(glm::mix(a, b, double(i) / n));
    return pts;
}
} // namespace

TEST_CASE("patterns: every kind has both pressed and untouched cells")
{
    for (int k = 0; k <= int(plating::PatternKind::Grille); ++k)
    {
        int on = 0, off = 0;
        for (int i = 0; i < 40; ++i)
            for (int j = 0; j < 40; ++j)
                (plating::patternHeight(plating::PatternKind(k), i * 0.071, j * 0.067, 0.15) > 0.5 ? on : off) += 1;
        CHECK(on > 20);
        CHECK(off > 20);
    }
}

TEST_CASE("patterns: pressed into a dense panel, faded at the lines, picked panels only when asked")
{
    auto mesh = std::make_shared<const geo::Mesh>(geo::makePlane({40.0, 40.0, 120, 120}));
    Surface s(*mesh);
    EditContext ctx{s, 0.5, 0.25};
    PanelLayout l = PanelLayout::withDefaultSets();
    addStroke(l, ctx, patternLine({-20, 0, 0}, {20, 0, 0}));
    auto r = Resolved::build(l, mesh, {.spacing = 0.25});
    plating::PatternOptions o;
    o.scale = 2.0;
    o.depth = 0.2;
    o.margin = 1.0;
    plating::PatternResult p = plating::applyPattern(*r, *mesh, o);
    CHECK(p.moved > 100);
    CHECK(p.warnings.empty());
    CHECK(p.mesh.bounds().min.y == doctest::Approx(-0.2).epsilon(0.05));
    // Within the margin of the line nothing moves.
    for (geo::Index i = 0; i < p.mesh.pointCapacity(); ++i)
        if (std::abs(p.mesh.position(i).z) < 0.1)
            CHECK(p.mesh.position(i).y == doctest::Approx(0.0));

    o.pickedOnly = true;
    CHECK(plating::applyPattern(*r, *mesh, o).moved == 0);
    l.panels.push_back({s.anchorAt({0, 0, 10}), {{"pattern", true}}});
    auto picked = Resolved::build(l, mesh, {.spacing = 0.25});
    const plating::PatternResult q = plating::applyPattern(*picked, *mesh, o);
    CHECK(q.moved > 50);
    for (geo::Index i = 0; i < q.mesh.pointCapacity(); ++i)
        if (q.mesh.position(i).z < -0.5)
            CHECK(q.mesh.position(i).y == doctest::Approx(0.0));
}
