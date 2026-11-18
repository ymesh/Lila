#include "brush/Brush.h"
#include "brush/Stroke.h"
#include "tex/PaintStrokes.h"

#include <doctest/doctest.h>

#include <numbers>

using namespace lila;

TEST_CASE("brush tilt: pen tilt and rotation shape each dab through the shared core")
{
    brush::BrushSettings s;
    s.radius = 10.0;
    s.tiltSize = {true, 0.2, 1.0};
    s.tiltOpacity = {true, 0.5, 1.0};
    s.tiltRoundness = {true, 0.25, 1.0};
    s.tiltAngle = true;
    s.rotationAngle = true;
    brush::SurfaceHit hit;
    hit.right = {1.0, 0.0, 0.0};
    brush::StrokeSample sample;
    sample.tilt = 0.5;
    sample.tiltDirection = 0.75;
    sample.rotation = 0.25;
    const brush::Dab d = brush::makeDab(s, sample, hit, nullptr);
    CHECK(d.radius == doctest::Approx(10.0 * (0.2 + 0.8 * 0.5)));
    CHECK(d.opacity == doctest::Approx(0.5 + 0.5 * 0.5));
    CHECK(d.roundness == doctest::Approx(0.25 + 0.75 * 0.5));
    CHECK(d.angle == doctest::Approx(0.75 + 0.25));
    CHECK(d.tilt == doctest::Approx(0.5));

    // A mouse (no tilt) leaves the dab as the brush makes it.
    sample.tilt = 0.0;
    sample.rotation = 0.0;
    const brush::Dab m = brush::makeDab(s, sample, hit, nullptr);
    CHECK(m.radius == doctest::Approx(10.0));
    CHECK(m.opacity == doctest::Approx(1.0));
}

TEST_CASE("brush tilt: settings round-trip through JSON, and the sampler hands the pen to every sample")
{
    brush::BrushSettings s;
    s.tiltSize = {true, 0.3, 1.0};
    s.tiltRoundness.enabled = true;
    s.rotationAngle = true;
    const brush::BrushSettings back = brush::BrushSettings::fromJson(s.toJson());
    CHECK(back.tiltSize.enabled);
    CHECK(back.tiltSize.floor == doctest::Approx(0.3));
    CHECK(back.tiltRoundness.enabled);
    CHECK_FALSE(back.tiltOpacity.enabled);
    CHECK(back.rotationAngle);
    CHECK_FALSE(back.tiltAngle);

    brush::StrokeSampler sampler;
    brush::BrushSettings b;
    b.spacing = 0.1;
    sampler.begin(b, {{0.0, 0.0}, 1.0, 0.7, 1.0, 0.3}, 10.0);
    sampler.move({{50.0, 0.0}, 1.0, 0.4, 2.0, 0.6});
    const std::vector<brush::StrokeSample> samples = sampler.take();
    REQUIRE(samples.size() > 3);
    CHECK(samples.front().tilt == doctest::Approx(0.7));
    CHECK(samples.back().tilt == doctest::Approx(0.4));
    CHECK(samples.back().tiltDirection == doctest::Approx(2.0));
    CHECK(samples.back().rotation == doctest::Approx(0.6));
}

TEST_CASE("brush tilt: a texture stroke record keeps each dab's tilt")
{
    tex::paint::Stroke s;
    s.dabs = {{3, 0.2f, 0.3f, 1.5f, 0.8f, 0.9f, 0.4f, 0, 0.65f}, {4, 0.1f, 0.1f, 1.0f, 1.0f, 1.0f, 0.0f, 1, 0.0f}};
    for (bool packed : {true, false})
    {
        const tex::paint::Stroke back = tex::paint::Stroke::fromJson(s.toJson(packed));
        REQUIRE(back.dabs.size() == 2);
        CHECK(back.dabs[0].tilt == doctest::Approx(0.65f));
        CHECK(back.dabs == s.dabs);
    }
}
