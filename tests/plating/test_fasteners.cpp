#include "Fasteners.h"
#include "Plates.h"

#include "geo/Ops.h"
#include "geo/Primitives.h"

#include <doctest/doctest.h>

using namespace lila;
using namespace lila::layout;

namespace
{
std::vector<glm::dvec3> line(glm::dvec3 a, glm::dvec3 b)
{
    std::vector<glm::dvec3> pts;
    const int n = std::max(1, int(glm::length(b - a) / 0.25));
    for (int i = 0; i <= n; ++i)
        pts.push_back(glm::mix(a, b, double(i) / n));
    return pts;
}

struct Cross
{
    std::shared_ptr<const geo::Mesh> mesh = std::make_shared<const geo::Mesh>(geo::makePlane({100.0, 100.0, 10, 10}));
    std::shared_ptr<const Resolved> layout;
    Cross()
    {
        Surface s(*mesh);
        EditContext ctx{s, 0.5, 0.25};
        PanelLayout l = PanelLayout::withDefaultSets();
        addStroke(l, ctx, line({-40, 0, 0}, {40, 0, 0}));
        addStroke(l, ctx, line({0, 0, -40}, {0, 0, 40}));
        layout = Resolved::build(l, mesh, {.spacing = 0.25});
    }
};
} // namespace

TEST_CASE("fasteners: spots along lines keep clear of the crossing and sit on both sides")
{
    Cross c;
    plating::FastenerOptions o;
    o.spacing = 5.0;
    o.inset = 1.0;
    o.placement = plating::Placement::BothSides;
    const auto spots = plating::fastenerSpots(*c.layout, o);
    REQUIRE(!spots.empty());
    for (const auto& s : spots)
    {
        const double fromX = std::abs(s.position.z), fromZ = std::abs(s.position.x);
        // Every spot is an inset away from the nearest line, never in the crossing.
        CHECK(std::min(fromX, fromZ) == doctest::Approx(1.0).epsilon(0.05));
    }
    o.placement = plating::Placement::OnLines;
    for (const auto& s : plating::fastenerSpots(*c.layout, o))
        CHECK(std::min(std::abs(s.position.z), std::abs(s.position.x)) < 0.01);
}

TEST_CASE("fasteners: a crossing has four corners")
{
    Cross c;
    plating::FastenerOptions o;
    o.placement = plating::Placement::Corners;
    o.inset = 2.0;
    const auto spots = plating::fastenerSpots(*c.layout, o);
    CHECK(spots.size() == 4);
    for (const auto& s : spots)
    {
        CHECK(std::abs(std::abs(s.position.x) - 2.0) < 0.05);
        CHECK(std::abs(std::abs(s.position.z) - 2.0) < 0.05);
    }
}

TEST_CASE("fasteners: heads sit on plate tops")
{
    Cross c;
    plating::PlateOptions p;
    p.thickness = 2.0;
    p.bevel = 0.0;
    const geo::Mesh plates = plating::makePlates(*c.layout, p).mesh;
    plating::FastenerOptions o;
    o.kind = plating::FastenerKind::HexBolt;
    o.height = 0.5;
    o.sink = 0.0;
    o.inset = 2.0;
    const plating::FastenersResult r = plating::makeFasteners(*c.layout, plates, o);
    REQUIRE(r.count > 0);
    CHECK(r.mesh.bounds().min.y == doctest::Approx(2.0).epsilon(0.01));
    CHECK(r.mesh.bounds().max.y == doctest::Approx(2.5).epsilon(0.01));
    CHECK(geo::separate(r.mesh).size() == size_t(r.count));
}

TEST_CASE("fasteners: recesses press round dips under each head into a dense surface")
{
    auto dense = std::make_shared<const geo::Mesh>(geo::makePlane({40.0, 40.0, 160, 160}));
    Surface s(*dense);
    EditContext ctx{s, 0.5, 0.25};
    PanelLayout l = PanelLayout::withDefaultSets();
    addStroke(l, ctx, line({-15, 0, 0}, {15, 0, 0}));
    auto r = Resolved::build(l, dense, {.spacing = 0.25});
    plating::FastenerOptions o;
    o.size = 1.0;
    o.spacing = 5.0;
    o.inset = 1.5;
    o.recess = 0.2;
    const plating::FastenersResult f = plating::makeFasteners(*r, *dense, o);
    REQUIRE(f.count > 0);
    CHECK(f.warnings.empty());
    REQUIRE(f.surface.numFaces() == dense->numFaces());
    CHECK(f.surface.bounds().min.y == doctest::Approx(-0.2).epsilon(0.02));
    CHECK(plating::fastenerPlacements(*r, *dense, o).size() == size_t(f.count));
    // Heads sit in their recesses: their base is at the recess floor.
    CHECK(f.mesh.bounds().min.y == doctest::Approx(-0.25).epsilon(0.02));
}
