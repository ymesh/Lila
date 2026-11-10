#include "Plates.h"

#include "geo/Ops.h"
#include "geo/Primitives.h"

#include <doctest/doctest.h>

using namespace lila;
using namespace lila::layout;

namespace
{
std::shared_ptr<const geo::Mesh> plane(int divisions)
{
    return std::make_shared<const geo::Mesh>(geo::makePlane({100.0, 100.0, divisions, divisions}));
}

std::vector<glm::dvec3> stroke(std::initializer_list<glm::dvec3> corners)
{
    std::vector<glm::dvec3> pts;
    const std::vector<glm::dvec3> c(corners);
    for (size_t k = 0; k + 1 < c.size(); ++k)
    {
        const int n = std::max(1, int(glm::length(c[k + 1] - c[k]) / 0.25));
        for (int i = (k == 0 ? 0 : 1); i <= n; ++i)
            pts.push_back(glm::mix(c[k], c[k + 1], double(i) / n));
    }
    return pts;
}

/** A plane split by a cross into four panels. */
std::shared_ptr<const Resolved> cross(std::shared_ptr<const geo::Mesh> mesh, PanelLayout* out = nullptr)
{
    Surface s(*mesh);
    EditContext ctx{s, 0.5, 0.25};
    PanelLayout l = PanelLayout::withDefaultSets();
    addStroke(l, ctx, stroke({{-50, 0, 0}, {50, 0, 0}}));
    addStroke(l, ctx, stroke({{0, 0, -50}, {0, 0, 50}}));
    if (out)
        *out = l;
    return Resolved::build(l, mesh, {.spacing = 0.25});
}
} // namespace

TEST_CASE("plates: a cross makes four closed plates standing at their thickness")
{
    for (int divisions : {1, 8})
    {
        auto r = cross(plane(divisions));
        REQUIRE(r->panels().size() == 4);
        plating::PlateOptions o;
        o.gap = 0.5;
        o.thickness = 2.0;
        o.bevel = 0.2;
        const plating::PlatesResult p = plating::makePlates(*r, o);
        CHECK(p.plates == 4);
        CHECK(p.warnings.empty());
        CHECK(p.mesh.bounds().max.y == doctest::Approx(2.0));
        CHECK(p.mesh.bounds().min.y == doctest::Approx(0.0));
        CHECK(geo::separate(p.mesh).size() == 4);
        CHECK(p.mesh.validate(geo::kValidateDefault).empty());
        // The gap runs along the lines: nothing of any plate within it.
        for (geo::Index v = 0; v < p.mesh.pointCapacity(); ++v)
            if (p.mesh.pointValid(v))
                CHECK(std::min(std::abs(p.mesh.position(v).x), std::abs(p.mesh.position(v).z)) >= 0.24);
    }
}

TEST_CASE("plates: panel values hide, raise and thicken one panel")
{
    PanelLayout l;
    auto mesh = plane(8);
    cross(mesh, &l);
    Surface s(*mesh);
    l.panels.push_back({s.anchorAt({25, 0, 25}), {{"hidden", true}}});
    l.panels.push_back({s.anchorAt({-25, 0, 25}), {{"elevation", 3.0}, {"thickness", 0.5}}});
    auto r = Resolved::build(l, mesh, {.spacing = 0.25});
    plating::PlateOptions o;
    o.thickness = 1.0;
    o.bevel = 0.0;
    const plating::PlatesResult p = plating::makePlates(*r, o);
    CHECK(p.plates == 3);
    CHECK(p.mesh.bounds().max.y == doctest::Approx(3.5));
}

