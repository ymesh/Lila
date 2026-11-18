#include "brush/Brush.h"
#include "brush/Stroke.h"

#include <doctest/doctest.h>

using namespace lila;

TEST_CASE("brush stroke methods: airbrush builds up over time without moving, anchored sizes one dab by the drag, drag dot lands on release")
{
    brush::BrushSettings s;
    s.mode = brush::StrokeMode::Airbrush;
    s.airRate = 20.0;
    brush::StrokeSampler air;
    air.begin(s, {{100.0, 100.0}, 1.0}, 10.0);
    CHECK(air.take().size() == 1);
    air.tick(500.0);
    std::vector<brush::StrokeSample> held = air.take();
    CHECK(held.size() == 10);
    for (const brush::StrokeSample& x : held)
        CHECK(x.position == glm::dvec2(100.0, 100.0));
    air.tick(25.0);
    air.tick(25.0);
    CHECK(air.take().size() == 1);

    s.mode = brush::StrokeMode::Anchored;
    brush::StrokeSampler anchored;
    anchored.begin(s, {{50.0, 50.0}, 1.0}, 10.0);
    anchored.move({{80.0, 50.0}, 1.0});
    anchored.move({{50.0, 90.0}, 1.0});
    CHECK(anchored.take().empty());
    anchored.end();
    std::vector<brush::StrokeSample> one = anchored.take();
    REQUIRE(one.size() == 1);
    CHECK(one[0].position == glm::dvec2(50.0, 50.0));
    CHECK(one[0].size * 10.0 == doctest::Approx(40.0).epsilon(0.05));

    s.mode = brush::StrokeMode::DragDot;
    brush::StrokeSampler dot;
    dot.begin(s, {{0.0, 0.0}, 1.0}, 10.0);
    dot.move({{30.0, 5.0}, 1.0});
    CHECK(dot.take().empty());
    dot.end();
    std::vector<brush::StrokeSample> last = dot.take();
    REQUIRE(last.size() == 1);
    CHECK(last[0].position == glm::dvec2(30.0, 5.0));

    CHECK(brush::strokeModeFromName("dragDot") == brush::StrokeMode::DragDot);
    CHECK(std::string(brush::strokeModeName(brush::StrokeMode::Airbrush)) == "airbrush");
    const brush::BrushSettings back = brush::BrushSettings::fromJson(s.toJson());
    CHECK(back.mode == brush::StrokeMode::DragDot);
}
