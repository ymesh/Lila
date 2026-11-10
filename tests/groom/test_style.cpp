#include <doctest/doctest.h>

#include "GroomTestUtil.h"

#include "groom/Shapes.h"
#include "groom/Style.h"

#include <cmath>
#include <set>

using namespace lila;
using namespace lila::groom;
using namespace groomtest;

namespace
{
Splines row(int n, double len = 20.0)
{
    Splines s;
    for (int i = 0; i < n; ++i)
        s.push_back(guide(D3(i * 2.0, 0, 0), D3(0, -1, 0), len, 12, double(i + 1)));
    return s;
}

/** Largest distance of the curve's points from the straight line through its root and tip. */
double bulge(const Spline& s)
{
    const D3 a = s.points.front(), b = s.points.back(), ab = glm::normalize(b - a);
    double out = 0.0;
    for (const D3& p : s.points)
    {
        const D3 rel = p - a;
        out = std::max(out, glm::length(rel - ab * glm::dot(rel, ab)));
    }
    return out;
}
} // namespace

TEST_CASE("styles: names round trip")
{
    for (int k = 0; k < int(Style::Count); ++k)
    {
        Style back;
        REQUIRE(parseStyle(styleName(Style(k)), &back));
        CHECK(back == Style(k));
    }
}

TEST_CASE("curl: helix of the given radius, root untouched, same result for the same ids")
{
    Splines s = row(3);
    StyleOptions o;
    o.kind = Style::Curl;
    o.radius = 1.5;
    o.frequency = 0.5;
    o.randomness = 0.0;
    applyStyle(s, o, nullptr);
    for (const Spline& sp : s)
    {
        CHECK(sp.size() > 12);
        CHECK(glm::distance(sp.points.front(), D3(sp.points.front().x, 0, 0)) < 1e-9);
        // Offset from the axis never exceeds twice the radius (the helix is measured from where it starts).
        for (const D3& p : sp.points)
            CHECK(glm::length(D3(p.x, 0.0, p.z) - D3(sp.points.front().x, 0, 0)) <= 3.0 + 1e-6);
        CHECK(bulge(sp) > 2.0);
    }
    Splines again = row(3);
    applyStyle(again, o, nullptr);
    CHECK(again[1].points == s[1].points);
}

TEST_CASE("frizz and wave: amount moves points, zero leaves curves straight; painted frizz adds")
{
    StyleOptions o;
    o.kind = Style::Frizz;
    o.amount = 0.0;
    Splines s = row(2);
    applyStyle(s, o, nullptr);
    CHECK(bulge(s[0]) < 1e-6);
    attr::points(s[1], attr::kFrizz, 0.5);
    applyStyle(s, o, nullptr);
    CHECK(bulge(s[1]) > 0.05);
    o.kind = Style::Wave;
    o.amount = 1.0;
    Splines w = row(1);
    applyStyle(w, o, nullptr);
    CHECK(bulge(w[0]) > 0.5);
    CHECK(bulge(w[0]) < 2.1);
}

TEST_CASE("split: sub-locks per lock with unique ids, thinner, sharing a clump; clump pulls them back")
{
    Splines s = row(2);
    StyleOptions o;
    o.kind = Style::Split;
    o.count = 4;
    o.spread = 2.0;
    o.widthScale = 0.5;
    applyStyle(s, o, nullptr);
    REQUIRE(s.size() == 8);
    std::set<double> ids, clumps;
    for (const Spline& sp : s)
    {
        ids.insert(attr::get(sp, attr::kId, 0.0));
        clumps.insert(attr::get(sp, attr::kClump, -1.0));
        CHECK(attr::get(sp, attr::kWidth, 1.0) < 0.7);
    }
    CHECK(ids.size() == 8);
    CHECK(clumps.size() == 2);
    // Fanned apart at the tips.
    const double tipGap = glm::distance(s[0].points.back(), s[3].points.back());
    CHECK(tipGap > 1.5);
    StyleOptions c;
    c.kind = Style::Clump;
    c.amount = 1.0;
    c.count = 0;
    c.ramp = [](double) { return 1.0; };
    applyStyle(s, c, nullptr);
    CHECK(glm::distance(s[0].points.back(), s[3].points.back()) < tipGap * 0.05);
}

TEST_CASE("length, cut and flyaways")
{
    Splines s = row(10, 20.0);
    StyleOptions o;
    o.kind = Style::Cut;
    o.length = 12.0;
    applyStyle(s, o, nullptr);
    for (const Spline& sp : s)
        CHECK(attr::length(sp) == doctest::Approx(12.0).epsilon(0.01));
    o.kind = Style::Length;
    o.amount = 0.5;
    applyStyle(s, o, nullptr);
    double lo = 1e9, hi = 0.0;
    for (const Spline& sp : s)
    {
        lo = std::min(lo, attr::length(sp));
        hi = std::max(hi, attr::length(sp));
    }
    CHECK(lo >= 6.0 - 1e-6);
    CHECK(hi - lo > 1.0);
    o.kind = Style::Flyaways;
    o.amount = 2.0;
    applyStyle(s, o, nullptr);
    int strays = 0;
    for (const Spline& sp : s)
        if (attr::get(sp, attr::kStray, 0.0) > 0.5)
        {
            ++strays;
            CHECK(attr::length(sp) < 12.0);
        }
    CHECK(strays == 20);
}

TEST_CASE("shapes: braid three strands, rope two, ringlet one; strands stay apart")
{
    std::vector<D3> stroke;
    for (int i = 0; i <= 40; ++i)
        stroke.push_back(D3(0, -i * 0.5, 0));
    ShapeOptions o;
    o.shape = LockShape::Braid;
    o.size = 1.0;
    o.period = 4.0;
    const Splines braid = shapeAlong(stroke, {}, D3(0, 0, 1), o);
    REQUIRE(braid.size() == 3);
    // After the root, no two strands meet.
    double closest = 1e9;
    for (size_t i = 12; i < braid[0].size(); ++i)
        for (int a = 0; a < 3; ++a)
            for (int b = a + 1; b < 3; ++b)
                closest = std::min(closest, glm::distance(braid[size_t(a)].points[i], braid[size_t(b)].points[i]));
    CHECK(closest > 0.3);
    o.shape = LockShape::Rope;
    CHECK(shapeAlong(stroke, {}, D3(0, 0, 1), o).size() == 2);
    o.shape = LockShape::Ringlet;
    const Splines ringlet = shapeAlong(stroke, {}, D3(0, 0, 1), o);
    REQUIRE(ringlet.size() == 1);
    CHECK(bulge(ringlet[0]) > 1.5);
    o.shape = LockShape::Lock;
    const Splines lock = shapeAlong(stroke, std::vector<double>(stroke.size(), 0.5), D3(0, 0, 1), o);
    REQUIRE(lock.size() == 1);
    REQUIRE(lock[0].radius.size() == lock[0].size());
    CHECK(lock[0].radius[3] == doctest::Approx(0.625));
}

TEST_CASE("presets: roots on the scalp, hair falls")
{
    const Surface scalp = sphereScalp(10.0);
    for (Preset p : {Preset::Ponytail, Preset::Bun, Preset::Bangs})
    {
        PresetOptions o;
        o.preset = p;
        o.at = p == Preset::Bangs ? D3(-4, 5, 8) : D3(0, 3, -10);
        o.to = D3(4, 5, 8);
        o.count = 12;
        o.length = 15.0;
        o.radius = 4.0;
        const Splines s = preset(scalp, o);
        REQUIRE(s.size() == 12);
        for (const Spline& sp : s)
        {
            CHECK(std::abs(glm::length(sp.points.front()) - 10.0) < 0.2);
            if (p != Preset::Bun)
                CHECK(sp.points.back().y < sp.points.front().y);
        }
    }
}
