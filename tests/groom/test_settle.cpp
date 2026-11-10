#include <doctest/doctest.h>

#include "GroomTestUtil.h"

#include "groom/Locks.h"
#include "groom/RodSettle.h"
#include "groom/Settle.h"

#include <cmath>

using namespace lila;
using namespace lila::groom;
using namespace groomtest;

TEST_CASE("settle: a lock sticking straight out falls, keeps its root and length, and rests outside the sphere")
{
    const Surface scalp = sphereScalp(10.0);
    // From the side of the head straight out horizontally.
    Splines s{guide(D3(10, 0, 0), D3(1, 0, 0), 12.0, 16, 1.0)};
    const double len = attr::length(s[0]);
    const std::vector<std::vector<double>> radii{std::vector<double>(16, 0.3)};
    SettleOptions o;
    o.steps = 240;
    o.bend = 0.1;
    o.holdShape = 0.0;
    o.rootStiffness = 0.2;
    SettleStats st;
    settle(s, radii, &scalp, o, &st);
    CHECK(glm::distance(s[0].points.front(), D3(10, 0, 0)) < 1e-9);
    CHECK(s[0].points.back().y < -6.0);
    CHECK(attr::length(s[0]) == doctest::Approx(len).epsilon(0.03));
    CHECK(deepest(s, 10.0, 0.3) < 0.05);
    CHECK(st.maxPenetration < 0.05);
}

TEST_CASE("settle: locks draped over the crown stay outside the head and apart from each other")
{
    const Surface scalp = sphereScalp(10.0);
    Splines s;
    // Two locks drawn through each other.
    s.push_back(lyingGuide(10.0, 0.0, 16, 1.0));
    s.push_back(lyingGuide(10.0, 0.02, 16, 2.0));
    for (Spline& sp : s)
        for (D3& p : sp.points)
            p *= 0.98;
    s[0].points.front() = glm::normalize(s[0].points.front()) * 10.0;
    s[1].points.front() = glm::normalize(s[1].points.front()) * 10.0;
    const std::vector<std::vector<double>> radii(2, std::vector<double>(16, 0.25));
    SettleOptions o;
    o.steps = 120;
    settle(s, radii, &scalp, o);
    CHECK(deepest(s, 10.0, 0.25) < 0.06);
    double closest = 1e9;
    for (size_t i = 4; i < s[0].size(); ++i)
        for (size_t j = 4; j < s[1].size(); ++j)
            closest = std::min(closest, glm::distance(s[0].points[i], s[1].points[j]));
    CHECK(closest > 0.4);
}

TEST_CASE("settle: deterministic, and weights hold points where they are")
{
    const Surface scalp = sphereScalp(10.0);
    Splines a{guide(D3(0, 10, 0), D3(1, 1, 0), 10.0, 12, 1.0), guide(D3(0, 10, 0.5), D3(1, 1, 0), 10.0, 12, 2.0)};
    Splines b = a;
    const std::vector<std::vector<double>> radii(2, std::vector<double>(12, 0.2));
    SettleOptions o;
    o.steps = 30;
    settle(a, radii, &scalp, o);
    settle(b, radii, &scalp, o);
    CHECK(a[0].points == b[0].points);
    CHECK(a[1].points == b[1].points);
    Splines held{guide(D3(0, 10, 0), D3(1, 1, 0), 10.0, 12, 1.0)};
    const Spline before = held[0];
    o.weights = {std::vector<double>(12, 0.0)};
    settle(held, {radii[0]}, &scalp, o);
    for (size_t i = 0; i < held[0].size(); ++i)
        CHECK(glm::distance(held[0].points[i], before.points[i]) < 1e-6);
}

TEST_CASE("collisionRadii: half the thickness, tubes their half width")
{
    LockOptions o;
    o.width = 2.0;
    o.thickness = 0.25;
    Spline a = guide(D3(0), D3(0, 1, 0), 5.0, 4);
    Spline t = a;
    attr::set(t, attr::kProfile, double(int(LockProfile::Tube)));
    const auto r = collisionRadii({a, t}, o);
    CHECK(r[0][0] == doctest::Approx(0.25));
    CHECK(r[1][0] == doctest::Approx(1.0));
}

namespace
{
/** Sum of turning angles along a curve, radians. */
double turning(const Spline& s)
{
    double a = 0.0;
    for (size_t i = 1; i + 1 < s.size(); ++i)
    {
        const D3 u = s.points[i] - s.points[i - 1], v = s.points[i + 1] - s.points[i];
        if (glm::length(u) > 1e-9 && glm::length(v) > 1e-9)
            a += std::acos(std::clamp(glm::dot(glm::normalize(u), glm::normalize(v)), -1.0, 1.0));
    }
    return a;
}
} // namespace

TEST_CASE("settle: a fan of flat locks from the crown lies down smooth, without crumpling")
{
    const Surface scalp = sphereScalp(10.0);
    Splines s;
    for (int k = 0; k < 16; ++k)
        s.push_back(lyingGuide(10.0, k * 0.25, 16, double(k + 1)));
    // Drawn a little inside the head, as strokes on an approximate surface are.
    for (Spline& sp : s)
        for (size_t i = 1; i < sp.size(); ++i)
            sp.points[i] *= 0.95;
    std::vector<double> before;
    for (const Spline& sp : s)
        before.push_back(turning(sp));
    LockOptions lo;
    lo.width = 3.0;
    lo.thickness = 0.3;
    SettleOptions o;
    SettleStats st;
    settle(s, collisionRadii(s, lo), &scalp, o, &st);
    for (size_t k = 0; k < s.size(); ++k)
        CHECK(turning(s[k]) < before[k] * 1.5 + 0.5);
    CHECK(deepest(s, 10.0) < 0.05);
    CHECK(st.lastMotion < 0.5);
}

TEST_CASE("settleOnRods: GroomLab's rod solver drops a lock straight out, root and length kept, resting outside the head")
{
    const Surface scalp = sphereScalp(10.0);
    Splines s{guide(D3(10, 0, 0), D3(1, 0, 0), 12.0, 16, 1.0)};
    const double len = attr::length(s[0]);
    const std::vector<std::vector<double>> radii{std::vector<double>(16, 0.3)};
    SettleOptions o;
    o.steps = 240;
    o.bend = 0.1;
    o.holdShape = 0.0;
    o.rootStiffness = 0.2;
    SettleStats st;
    settleOnRods(s, radii, &scalp, o, &st);
    CHECK(glm::distance(s[0].points.front(), D3(10, 0, 0)) < 1e-3);
    CHECK(s[0].points.back().y < -6.0);
    CHECK(attr::length(s[0]) == doctest::Approx(len).epsilon(0.03));
    CHECK(deepest(s, 10.0, 0.3) < 0.05);
    CHECK(st.maxPenetration < 0.05);
}

TEST_CASE("settleOnRods: draped locks drawn through each other end apart and outside the head, the same every run")
{
    const Surface scalp = sphereScalp(10.0);
    auto make = [] {
        Splines s;
        s.push_back(lyingGuide(10.0, 0.0, 16, 1.0));
        s.push_back(lyingGuide(10.0, 0.02, 16, 2.0));
        for (Spline& sp : s)
            for (D3& p : sp.points)
                p *= 0.98;
        s[0].points.front() = glm::normalize(s[0].points.front()) * 10.0;
        s[1].points.front() = glm::normalize(s[1].points.front()) * 10.0;
        return s;
    };
    const std::vector<std::vector<double>> radii(2, std::vector<double>(16, 0.25));
    SettleOptions o;
    o.steps = 120;
    Splines a = make(), b = make();
    settleOnRods(a, radii, &scalp, o);
    settleOnRods(b, radii, &scalp, o);
    CHECK(deepest(a, 10.0, 0.25) < 0.06);
    double closest = 1e9;
    for (size_t i = 4; i < a[0].size(); ++i)
        for (size_t j = 4; j < a[1].size(); ++j)
            closest = std::min(closest, glm::distance(a[0].points[i], a[1].points[j]));
    CHECK(closest > 0.4);
    CHECK(a[0].points == b[0].points);
}
