#include "ui/Overlay.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

using lila::ui::OverlayPainter;

TEST_CASE("overlay digits are tabular so counts never shift")
{
    CHECK(OverlayPainter::textWidth("1111111") == doctest::Approx(OverlayPainter::textWidth("8888888")));
    CHECK(OverlayPainter::textWidth("0") == doctest::Approx(OverlayPainter::textWidth("7")));
}

TEST_CASE("backdrop classification switches with hysteresis")
{
    std::vector<float> state;
    OverlayPainter::classifyBackdrop({0.05f, 0.5f, 0.19f}, state);
    CHECK(state == std::vector<float>{0.0f, 1.0f, 1.0f});

    // Small moves around the switch point keep the previous answer.
    OverlayPainter::classifyBackdrop({0.2f, 0.16f, 0.17f}, state);
    CHECK(state == std::vector<float>{0.0f, 1.0f, 1.0f});

    // Clear moves flip it.
    OverlayPainter::classifyBackdrop({0.3f, 0.1f, 0.12f}, state);
    CHECK(state == std::vector<float>{1.0f, 0.0f, 0.0f});
}

TEST_CASE("ink turns dark over a bright backdrop, per region")
{
    std::vector<lila::render::OverlayVertex> out;
    OverlayPainter p(out, 1.0);
    CHECK(p.ink(0.85f).r == doctest::Approx(0.85f)); // no backdrop counts as dark

    // Left half dark, right half bright.
    p.setBackdrop({0.0f, 1.0f, 0.0f, 1.0f}, 2, 2, QSizeF(200, 100));
    CHECK_FALSE(p.brightBackdrop(QRectF(10, 10, 50, 20)));
    CHECK(p.brightBackdrop(QRectF(120, 60, 50, 20)));
    CHECK(p.ink(0.85f, 1.0f, QRectF(10, 10, 50, 20)).r == doctest::Approx(0.85f));
    glm::vec4 dark = p.ink(0.85f, 0.9f, QRectF(120, 60, 50, 20));
    CHECK(dark.r < 0.1f);
    CHECK(dark.a == doctest::Approx(0.9f));
    // Dim chrome stays well clear of a mid-gray backdrop.
    CHECK(p.ink(0.45f, 1.0f, QRectF(120, 60, 50, 20)).r < 0.25f);
}

TEST_CASE("sector: solid inside, fading to clear over the feather, rounded ends pulled in")
{
    std::vector<lila::render::OverlayVertex> out;
    OverlayPainter p(out, 1.0);
    const glm::vec4 white{1.0f, 1.0f, 1.0f, 1.0f};
    p.sector(QPointF(0, 0), 20.0, 40.0, -45.0, 45.0, white, white, 0.0, 1.0f);
    REQUIRE(!out.empty());
    REQUIRE(out.size() % 3 == 0);
    float maxSolid = 0.0f, maxAny = 0.0f, minAny = 1e9f;
    for (const auto& v : out)
    {
        const float r = glm::length(v.pos);
        maxAny = std::max(maxAny, r);
        minAny = std::min(minAny, r);
        if (v.color.a > 0.99f)
            maxSolid = std::max(maxSolid, r);
    }
    CHECK(maxSolid == doctest::Approx(40.0f).epsilon(0.01));
    CHECK(maxAny == doctest::Approx(41.0f).epsilon(0.01));
    CHECK(minAny == doctest::Approx(19.0f).epsilon(0.01));

    // Round ends: no solid vertex sits on the outer radius at the very end of the span.
    out.clear();
    p.sector(QPointF(0, 0), 20.0, 40.0, -45.0, 45.0, white, white, 10.0, 1.0f);
    for (const auto& v : out)
        if (v.color.a > 0.99f && glm::degrees(std::atan2(v.pos.y, v.pos.x)) > 44.0f)
            CHECK(glm::length(v.pos) < 39.0f);
}

TEST_CASE("smooth line fringes are clear and the core solid")
{
    std::vector<lila::render::OverlayVertex> out;
    OverlayPainter p(out, 1.0);
    p.smoothLine(QPointF(0, 0), QPointF(10, 0), 2.0f, {1, 1, 1, 1}, 1.0f);
    REQUIRE(out.size() == 18);
    for (const auto& v : out)
        CHECK((std::abs(v.pos.y) > 1.5f ? v.color.a == 0.0f : v.color.a == 1.0f));
}

TEST_CASE("overlay: dashes projected from just in front of the eye cost no more than the view")
{
    // A remix seam outline whose corner sits a hair in front of the eye projects billions of pixels away.
    const std::vector<QPointF> outline = {{100, 100}, {4.0e9, 3.0e9}, {-2.0e9, 5.0e9}, {200, 300}};
    const glm::vec4 white{1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<lila::render::OverlayVertex> out;
    OverlayPainter p(out, 1.0);
    p.setClip(QRectF(0, 0, 1920, 1080));
    p.dashedPolyline(outline, 1.0f, 3.0f, white, true);
    p.dashedLine(QPointF(10, 10), QPointF(1.0e10, 10), 1.0f, 3.0f, white);
    p.line(QPointF(-1.0e12, 500), QPointF(1.0e12, 500), 2.0f, white);
    p.line(QPointF(5000, 5000), QPointF(9000, 9000), 2.0f, white); // wholly off the view
    CHECK(out.size() % 3 == 0);
    CHECK(out.size() < 200000);
    CHECK_FALSE(p.capped());
    for (const auto& v : out)
    {
        CHECK(v.pos.x > -50.0f);
        CHECK(v.pos.x < 1970.0f);
        CHECK(v.pos.y > -50.0f);
        CHECK(v.pos.y < 1130.0f);
    }
}

TEST_CASE("overlay: dashes cut by the view keep their pattern")
{
    const glm::vec4 white{1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<lila::render::OverlayVertex> whole, cut;
    {
        OverlayPainter p(whole, 1.0);
        p.dashedPolyline({{-1000, 50}, {1000, 50}}, 1.0f, 5.0f, white);
    }
    {
        OverlayPainter p(cut, 1.0);
        p.setClip(QRectF(0, 0, 400, 100));
        p.dashedPolyline({{-1000, 50}, {1000, 50}}, 1.0f, 5.0f, white);
    }
    // Every dash drawn inside the view is one of the uncut line's dashes.
    auto starts = [](const std::vector<lila::render::OverlayVertex>& v) {
        std::vector<float> xs;
        for (size_t i = 0; i < v.size(); i += 6)
            xs.push_back(std::round(std::min({v[i].pos.x, v[i + 1].pos.x, v[i + 2].pos.x})));
        return xs;
    };
    const std::vector<float> all = starts(whole);
    const std::vector<float> inView = starts(cut);
    REQUIRE(inView.size() > 30);
    CHECK(inView.size() < all.size());
    for (float x : inView)
        if (x > 10.0f && x < 390.0f)
            CHECK(std::find(all.begin(), all.end(), x) != all.end());
}

TEST_CASE("overlay: one painter stops at its vertex cap on a whole triangle")
{
    std::vector<lila::render::OverlayVertex> out;
    OverlayPainter p(out, 1.0);
    const glm::vec4 white{1.0f, 1.0f, 1.0f, 1.0f};
    while (!p.capped())
        p.line(QPointF(0, 0), QPointF(10, 10), 1.0f, white);
    const size_t atCap = out.size();
    CHECK(atCap >= OverlayPainter::kMaxVertices);
    CHECK(atCap < OverlayPainter::kMaxVertices + 6);
    p.line(QPointF(0, 0), QPointF(10, 10), 1.0f, white);
    CHECK(out.size() == atCap);
    p.cappedNote(QPointF(10, 10));
    CHECK(out.size() > atCap);
    CHECK(out.size() % 3 == 0);
}
