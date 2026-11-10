#include "brush/Brush.h"
#include "brush/Stroke.h"
#include "brush/Symmetry.h"
#include "core/PrefStore.h"
#include "geo/Primitives.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace lila;
using brush::BrushSettings;
using brush::StrokeInput;
using brush::StrokeSample;
using brush::StrokeSampler;

namespace
{
constexpr double kPi = 3.14159265358979323846;

bool near(glm::dvec3 a, glm::dvec3 b, double eps = 1e-9) { return glm::length(a - b) < eps; }
bool near(glm::dvec2 a, glm::dvec2 b, double eps = 1e-9) { return glm::length(a - b) < eps; }

/** A flat surface at z = 0 seen from +z: one object unit per two pixels. */
struct Plane
{
    double pixelSize = 0.5;
    bool missRight = false;

    bool operator()(glm::dvec2 screen, brush::SurfaceHit& out) const
    {
        if (missRight && screen.x > 100.0)
            return false;
        out.center = glm::dvec3(screen.x * pixelSize, -screen.y * pixelSize, 0.0);
        out.normal = glm::dvec3(0, 0, 1);
        out.viewDir = glm::dvec3(0, 0, -1);
        out.eye = out.center + glm::dvec3(0, 0, 100);
        out.right = glm::dvec3(1, 0, 0);
        out.pixelSize = pixelSize;
        out.worldToLocal = 2.0;
        return true;
    }
};

struct Recorder : brush::DabSink
{
    std::vector<brush::Dab> dabs;
    void dab(const brush::Dab& d) override { dabs.push_back(d); }
};
} // namespace

TEST_CASE("falloff shapes, tables and names")
{
    for (int i = 0; i < int(brush::Shape::Count); ++i)
    {
        auto s = brush::Shape(i);
        CHECK(brush::shapeFromName(brush::shapeName(s)) == s);
        CHECK(brush::shapeWeight(s, 0.0) == doctest::Approx(1.0));
        CHECK(brush::shapeWeight(s, 1.0) == 0.0);
        CHECK(brush::shapeWeight(s, 1.5) == 0.0);
    }
    CHECK(brush::shapeWeight(brush::Shape::Bell, 0.5) == doctest::Approx(0.5625));
    CHECK(brush::shapeWeight(brush::Shape::Smooth, 0.5) == doctest::Approx(0.5));

    brush::Falloff sphere = brush::Falloff::fromName("sphere");
    CHECK(sphere.shape == brush::Shape::Sphere);
    brush::FalloffTable table(sphere);
    for (double t = 0.0; t < 0.99; t += 0.037)
        CHECK(table(t) == doctest::Approx(sphere.evaluate(t)).epsilon(0.01));
    CHECK(table(1.2) == 0.0f);

    // Soft select presets by name come through as curves.
    brush::Falloff linear = brush::Falloff::fromName("Linear");
    CHECK(linear.shape == brush::Shape::Curve);
    CHECK(linear.evaluate(0.25) == doctest::Approx(0.75));
    CHECK(brush::Falloff::fromName("no such preset").shape == brush::Shape::Curve);
}

TEST_CASE("pressure curves")
{
    brush::PressureCurve c;
    CHECK(c.map(0.2) == 1.0);
    c.enabled = true;
    CHECK(c.map(0.0) == 0.0);
    CHECK(c.map(0.5) == doctest::Approx(0.5));
    CHECK(c.map(1.0) == doctest::Approx(1.0));
    CHECK(c.map(2.0) == doctest::Approx(1.0));
    c.floor = 0.2;
    CHECK(c.map(0.0) == doctest::Approx(0.2));
    CHECK(c.map(0.5) == doctest::Approx(0.6));
    c.floor = 0.0;
    c.exponent = 2.0;
    CHECK(c.map(0.5) == doctest::Approx(0.25));
    c.exponent = 0.5;
    CHECK(c.map(0.25) == doctest::Approx(0.5));
}

TEST_CASE("spacing: even dabs along the drag, the remainder carried")
{
    BrushSettings s;
    s.spacing = 0.1;
    StrokeSampler sm;
    sm.begin(s, {{0, 0}, 1.0}, 100.0);
    CHECK(sm.spacingPx() == doctest::Approx(10.0));
    auto first = sm.take();
    REQUIRE(first.size() == 1);
    CHECK(near(first[0].position, {0, 0}));

    sm.move({{95, 0}, 0.0});
    auto out = sm.take();
    REQUIRE(out.size() == 9);
    for (int k = 0; k < 9; ++k)
    {
        CHECK(near(out[k].position, {10.0 * (k + 1), 0}));
        CHECK(out[k].index == uint32_t(k + 1));
        CHECK(near(out[k].direction, {1, 0}));
    }
    // Pressure runs from the press to the new event along the segment.
    CHECK(out[0].pressure == doctest::Approx(1.0 - 10.0 / 95.0));
    CHECK(out.back().distance == doctest::Approx(90.0));

    // Short moves add nothing until a whole step is covered from the last dab.
    sm.move({{97, 0}, 0.0});
    CHECK(sm.take().empty());
    sm.move({{100, 0}, 0.0});
    out = sm.take();
    REQUIRE(out.size() == 1);
    CHECK(near(out[0].position, {100, 0}));

    // A tiny brush still steps at least a pixel.
    sm.begin(s, {{0, 0}, 1.0}, 2.0);
    CHECK(sm.spacingPx() == doctest::Approx(1.0));
    sm.take();
    sm.move({{0, 5}, 1.0});
    CHECK(sm.take().size() == 5);
}

TEST_CASE("stabiliser: lazy mouse and smoothing")
{
    BrushSettings s;
    s.spacing = 0.1;
    s.lazyRadius = 20.0;
    StrokeSampler sm;
    sm.begin(s, {{0, 0}, 1.0}, 100.0);
    sm.take();

    // Inside the string's length the brush stays put.
    sm.move({{15, 0}, 1.0});
    CHECK(sm.take().empty());
    CHECK(near(sm.brush(), {0, 0}));

    // Pulled taut, it follows at the string's length behind the cursor.
    sm.move({{50, 0}, 1.0});
    CHECK(near(sm.brush(), {30, 0}));
    auto out = sm.take();
    REQUIRE(out.size() == 3);
    CHECK(near(out.back().position, {30, 0}));

    // Turning back inside the string leaves the brush where it was.
    sm.move({{40, 5}, 1.0});
    CHECK(near(sm.brush(), {30, 0}));

    BrushSettings smooth;
    smooth.spacing = 0.1;
    smooth.smoothing = 0.5;
    sm.begin(smooth, {{0, 0}, 1.0}, 100.0);
    sm.move({{100, 0}, 1.0});
    CHECK(near(sm.brush(), {50, 0}));
    sm.move({{100, 0}, 1.0});
    CHECK(near(sm.brush(), {75, 0}));
}

TEST_CASE("line strokes lay their dabs on release")
{
    BrushSettings s;
    s.spacing = 0.25;
    s.mode = brush::StrokeMode::Line;
    StrokeSampler sm;
    sm.begin(s, {{10, 10}, 1.0}, 40.0);
    CHECK(sm.take().empty());
    sm.move({{30, 10}, 1.0});
    sm.move({{10, 60}, 1.0});
    CHECK(sm.take().empty());
    sm.end();
    auto out = sm.take();
    REQUIRE(out.size() == 6);
    for (size_t k = 0; k < out.size(); ++k)
        CHECK(near(out[k].position, {10, 10 + 10.0 * k}));
    CHECK(!sm.active());
}

TEST_CASE("jitter is deterministic per seed and stays in range")
{
    BrushSettings s;
    s.spacing = 0.1;
    s.jitter = {.position = 0.5, .size = 0.4, .opacity = 0.3, .angle = 20.0};
    auto run = [&](uint32_t seed) {
        s.seed = seed;
        StrokeSampler sm;
        sm.begin(s, {{0, 0}, 1.0}, 50.0);
        sm.move({{200, 0}, 1.0});
        return sm.take();
    };
    auto a = run(7), b = run(7), c = run(8);
    REQUIRE(a.size() == b.size());
    bool differs = false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        CHECK(near(a[i].position, b[i].position));
        CHECK(a[i].size == b[i].size);
        differs = differs || !near(a[i].position, c[i].position);
        CHECK(a[i].size >= 0.6 - 1e-9);
        CHECK(a[i].size <= 1.0);
        CHECK(a[i].opacity >= 0.7 - 1e-9);
        CHECK(std::abs(a[i].angle) <= 20.0 * kPi / 180.0 + 1e-9);
        const glm::dvec2 unjittered(5.0 * i, 0.0);
        CHECK(glm::length(a[i].position - unjittered) <= 25.0 + 1e-9);
    }
    CHECK(differs);
}

TEST_CASE("tip angle follows the stroke")
{
    BrushSettings s;
    s.spacing = 0.1;
    s.followStroke = true;
    s.angle = 90.0;
    StrokeSampler sm;
    sm.begin(s, {{0, 0}, 1.0}, 100.0);
    sm.move({{0, -20}, 1.0});
    auto out = sm.take();
    REQUIRE(out.size() == 3);
    // Up the screen is a quarter turn counterclockwise, plus the tip's own quarter turn.
    CHECK(out.back().angle == doctest::Approx(kPi));
}

TEST_CASE("stroke driver: radius, pressure curves, flow and direction into dabs")
{
    BrushSettings s;
    s.radius = 40.0;
    s.spacing = 0.25;
    s.flow = 0.5;
    s.pressureSize.enabled = true;
    s.pressureOpacity.enabled = true;
    s.pressureOpacity.exponent = 2.0;
    Plane plane;
    Recorder sink;
    brush::Stroke stroke;
    REQUIRE(stroke.begin(s, {{0, 0}, 0.5}, plane, sink));
    stroke.move({{20, 0}, 0.5});
    stroke.end();
    REQUIRE(sink.dabs.size() == 3);
    const brush::Dab& d = sink.dabs[1];
    CHECK(d.radius == doctest::Approx(40.0 * 0.5 * 0.5));
    CHECK(d.pressure == doctest::Approx(0.5));
    CHECK(d.opacity == doctest::Approx(0.25));
    CHECK(d.flow == doctest::Approx(0.5));
    CHECK(near(d.center, {5, 0, 0}));
    CHECK(near(d.direction, {5, 0, 0}));
    CHECK(near(sink.dabs[0].direction, {0, 0, 0}));
    CHECK(stroke.dabs().size() == 3);

    // A freehand press off the surface starts nothing.
    plane.missRight = true;
    Recorder none;
    CHECK(!stroke.begin(s, {{150, 0}, 1.0}, plane, none));
    CHECK(none.dabs.empty());

    // A world radius is the same size on the surface whatever the pixel size, and spaces dabs by it.
    BrushSettings w;
    w.radiusUnit = brush::RadiusUnit::World;
    w.worldRadius = 10.0;
    w.spacing = 0.5;
    plane.missRight = false;
    plane.pixelSize = 2.0;
    Recorder world;
    REQUIRE(stroke.begin(w, {{0, 0}, 1.0}, plane, world));
    CHECK(stroke.sampler().spacingPx() == doctest::Approx(5.0));
    stroke.move({{10, 0}, 1.0});
    stroke.end();
    REQUIRE(world.dabs.size() == 3);
    CHECK(world.dabs[0].radius == doctest::Approx(20.0));
}

TEST_CASE("tip roundness and angle")
{
    brush::Dab d;
    d.radius = 10.0;
    d.normal = glm::dvec3(0, 0, 1);
    brush::FalloffTable linear(brush::Shape::Linear);
    CHECK(brush::tipWeight(d, linear, {5, 0, 0}) == doctest::Approx(0.5));
    CHECK(brush::tipWeight(d, linear, {0, 6, 0}) == doctest::Approx(0.4));

    d.roundness = 0.5;
    d.tangent = glm::dvec3(1, 0, 0);
    CHECK(brush::tipWeight(d, linear, {6, 0, 0}) == doctest::Approx(0.4));
    CHECK(brush::tipWeight(d, linear, {0, 6, 0}) == 0.0f);
    CHECK(brush::tipWeight(d, linear, {0, 4, 0}) == doctest::Approx(0.2));

    // makeDab lays the screen angle into the surface.
    BrushSettings s;
    s.roundness = 0.5;
    brush::StrokeSample sample;
    sample.angle = kPi / 2.0;
    brush::SurfaceHit hit;
    Plane()({0, 0}, hit);
    brush::Dab m = brush::makeDab(s, sample, hit, nullptr);
    CHECK(near(m.tangent, {0, 1, 0}));
}

TEST_CASE("symmetry: mirror and radial copies, mirrored hits")
{
    brush::Dab d;
    d.center = glm::dvec3(3, 1, 2);
    d.normal = glm::dvec3(1, 0, 0);
    d.direction = glm::dvec3(0, 0, 1);
    d.eye = glm::dvec3(10, 1, 2);

    brush::Dab m = brush::mirrorDab(d, geo::axisPlane(0));
    CHECK(near(m.center, {-3, 1, 2}));
    CHECK(near(m.normal, {-1, 0, 0}));
    CHECK(near(m.eye, {-10, 1, 2}));
    CHECK(near(m.direction, {0, 0, 1}));

    auto radial = brush::radialDabs(d, {.count = 4, .axis = 1});
    REQUIRE(radial.size() == 4);
    CHECK(near(radial[0].center, d.center));
    CHECK(near(radial[1].center, {2, 1, -3}, 1e-9));
    CHECK(near(radial[2].center, {-3, 1, -2}, 1e-9));
    CHECK(near(radial[1].normal, {0, 0, -1}, 1e-9));
    CHECK(radial[3].copy == 3);
    CHECK(brush::radialDabs(d, {}).size() == 1);

    const geo::SymmetryPlane plane = geo::axisPlane(0);
    auto all = brush::symmetricDabs(d, &plane, {.count = 3, .axis = 1});
    REQUIRE(all.size() == 6);
    for (size_t i = 0; i < all.size(); ++i)
        CHECK(all[i].copy == i);
    CHECK(near(all[3].center, {-3, 1, 2}));

    // Point-level mirroring through a topological map keeps the strongest weight once per point.
    geo::Mesh grid = geo::makePlane({.width = 10, .depth = 10, .subdivX = 4, .subdivZ = 4});
    geo::SymmetryMap map = geo::SymmetryMap::build(grid, geo::axisPlane(0), 1e-3);
    REQUIRE(!map.empty());
    geo::Index p = 0;
    while (map.mirrorPoint(p) == p || map.mirrorPoint(p) == geo::kInvalid)
        ++p;
    const geo::Index q = map.mirrorPoint(p);
    std::vector<std::pair<geo::Index, float>> hits = {{p, 0.5f}, {q, 0.25f}};
    brush::mirrorHits(hits, map);
    REQUIRE(hits.size() == 2);
    for (auto [pt, w] : hits)
        CHECK(w == 0.5f);
    hits = {{p, 0.5f}};
    std::vector<uint8_t> mask(grid.pointCapacity(), 0);
    mask[p] = 1;
    brush::mirrorHits(hits, map, mask);
    CHECK(hits.size() == 1);
}

TEST_CASE("settings round trip through json and prefs, reading the tools' plain keys")
{
    BrushSettings s;
    s.radius = 77.0;
    s.radiusUnit = brush::RadiusUnit::World;
    s.falloff = "Dome";
    s.pressureFlow = {.enabled = true, .floor = 0.1, .exponent = 1.5};
    s.jitter.angle = 30.0;
    s.mode = brush::StrokeMode::Line;
    s.lazyRadius = 12.0;
    s.radial = {.count = 6, .axis = 2};
    s.alphaPath = "missing.png";
    BrushSettings back = BrushSettings::fromJson(s.toJson());
    CHECK(back.toJson() == s.toJson());

    // Out of range values clamp.
    CHECK(BrushSettings::fromJson({{"strength", 4.0}, {"radial", {{"count", 0}}}}).strength == 1.0);
    CHECK(BrushSettings::fromJson({{"radial", {{"count", 0}}}}).radial.count == 1);

    PrefStore& p = PrefStore::get();
    p.set("general", "/brushTest/radius", 33.0);
    p.set("general", "/brushTest/pressureSize", true);
    p.set("general", "/brushTest/falloff", "Smooth");
    BrushSettings loaded;
    loaded.spacing = 0.3;
    loaded.loadPrefs("/brushTest");
    CHECK(loaded.radius == 33.0);
    CHECK(loaded.pressureSize.enabled);
    CHECK(loaded.falloff == "Smooth");
    CHECK(loaded.spacing == 0.3);
    loaded.lazyRadius = 9.0;
    loaded.savePrefs("/brushTest");
    BrushSettings again;
    again.loadPrefs("/brushTest");
    CHECK(again.lazyRadius == 9.0);
    CHECK(p.value<double>("general", "/brushTest/radius", 0.0) == 33.0);
}

TEST_CASE("symmetry: radial orbits copy dabs and hits to every wedge")
{
    geo::Mesh disc = geo::makeDisc({.radius = 10.0, .subdivAxis = 24, .subdivRings = 2});
    for (bool mirror : {false, true})
    {
        CAPTURE(mirror);
        const geo::RadialSymmetry r = geo::axisRadial(1, glm::dvec3(0.0), 6, mirror);
        geo::SymmetryMap map = geo::SymmetryMap::buildRadial(disc, r, 1e-6);
        REQUIRE(map.unmatchedCount() == 0);

        brush::Dab d;
        d.center = glm::dvec3(3, 0, 2);
        d.normal = glm::dvec3(0, 1, 0);
        d.direction = glm::dvec3(1, 0, 0);
        d.angle = 0.3;
        auto copies = brush::orbitDabs(d, map);
        REQUIRE(copies.size() == size_t(r.order()));
        for (int g = 0; g < r.order(); ++g)
        {
            CHECK(copies[size_t(g)].copy == uint32_t(g));
            CHECK(near(copies[size_t(g)].center, r.apply(g, d.center), 1e-9));
            CHECK(copies[size_t(g)].angle == (r.reflects(g) ? -0.3 : 0.3));
        }

        // A point off every plane gets its whole orbit, once each at its strongest weight.
        geo::Index p = 0;
        while (p < disc.pointCapacity() && (map.fixed(p) || map.wedge(p) < 0))
            ++p;
        REQUIRE(p < disc.pointCapacity());
        std::vector<std::pair<geo::Index, float>> hits = {{p, 0.5f}, {map.image(1, p), 0.25f}};
        brush::mirrorHits(hits, map);
        CHECK(hits.size() == map.expandPoints(std::vector<geo::Index>{p}).size());
        for (auto [pt, w] : hits)
            CHECK(w == 0.5f);
    }
}

TEST_CASE("curve stroke: nothing while drawing, then even dabs along the smoothed path")
{
    BrushSettings s;
    s.spacing = 0.1;
    s.mode = brush::StrokeMode::Curve;
    StrokeSampler sm;
    sm.begin(s, {{0, 0}, 1.0}, 100.0);
    CHECK(sm.take().empty());
    // A shaky horizontal drag: alternating 4 px up and down every 5 px.
    for (int k = 1; k <= 60; ++k)
        sm.move({{k * 5.0, (k % 2 ? 4.0 : -4.0)}, 1.0});
    CHECK(sm.take().empty());
    CHECK(sm.path().size() > 50);
    sm.end();
    const auto out = sm.take();
    REQUIRE(out.size() > 20);
    // Smoothed: the wobble is mostly gone, and the dabs run from the start to the end.
    for (const auto& d : out)
        CHECK(std::abs(d.position.y) < 2.5);
    CHECK(std::abs(out.front().position.x) < 0.5);
    CHECK(out.back().position.x > 280.0);
    for (size_t k = 1; k < out.size(); ++k)
        CHECK(out[k].position.x > out[k - 1].position.x);
}
