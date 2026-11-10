#include "layout/Fit.h"

#include <glm/gtc/constants.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <random>

using namespace lila;
using namespace lila::layout;

namespace
{
std::vector<glm::dvec3> fitLine(glm::dvec3 a, glm::dvec3 b, int n)
{
    std::vector<glm::dvec3> out;
    for (int i = 0; i <= n; ++i)
        out.push_back(glm::mix(a, b, double(i) / n));
    return out;
}

/** Hand wobble: small sideways noise, the same every run. */
void wobble(std::vector<glm::dvec3>& pts, double amount, unsigned seed = 7)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> d(-amount, amount);
    for (size_t k = 1; k + 1 < pts.size(); ++k)
        pts[k].z += d(rng);
}
} // namespace

TEST_CASE("fit: a shaky straight stroke becomes one straight piece")
{
    auto pts = fitLine({0, 0, 0}, {50, 0, 0}, 200);
    wobble(pts, 0.1);
    const FitResult r = fitStroke(pts, {.tolerance = 0.5, .cleanup = 0.5});
    REQUIRE(r.pieces.size() == 1);
    CHECK(r.pieces[0].points.size() == 2);
    CHECK_FALSE(r.closed);
}

TEST_CASE("fit: an L becomes two pieces meeting at the corner")
{
    auto pts = fitLine({0, 0, 0}, {30, 0, 0}, 100);
    auto up = fitLine({30, 0, 0}, {30, 0, 30}, 100);
    pts.insert(pts.end(), up.begin() + 1, up.end());
    const FitResult r = fitStroke(pts, {.tolerance = 0.5});
    REQUIRE(r.pieces.size() == 2);
    CHECK(glm::length(r.pieces[0].points.back() - glm::dvec3(30, 0, 0)) < 1.0);
    CHECK(r.pieces[0].points.size() == 2);
    CHECK(r.pieces[1].points.size() == 2);
}

TEST_CASE("fit: a circle closes as one smooth loop")
{
    std::vector<glm::dvec3> pts;
    for (int i = 0; i <= 200; ++i)
    {
        const double t = glm::two_pi<double>() * i / 200.0;
        pts.push_back({20.0 * std::cos(t), 0.0, 20.0 * std::sin(t)});
    }
    const FitResult r = fitStroke(pts, {.tolerance = 0.5});
    CHECK(r.closed);
    CHECK_FALSE(r.closedCorner);
    REQUIRE(r.pieces.size() == 1);
    CHECK(r.pieces[0].points.size() >= 5);
}

TEST_CASE("fit: cleanup trades drawn detail for fewer points")
{
    std::vector<glm::dvec3> pts;
    for (int i = 0; i <= 300; ++i)
    {
        const double x = i * 0.2;
        pts.push_back({x, 0.0, 5.0 * std::sin(x * 0.2)});
    }
    wobble(pts, 0.2);
    const FitResult raw = fitStroke(pts, {.tolerance = 0.5, .cleanup = 0.0});
    const FitResult clean = fitStroke(pts, {.tolerance = 0.5, .cleanup = 1.0});
    REQUIRE(raw.pieces.size() == 1);
    REQUIRE(clean.pieces.size() == 1);
    CHECK(raw.pieces[0].points.size() > clean.pieces[0].points.size());
    CHECK(clean.pieces[0].points.size() >= 3);
}

TEST_CASE("fit: straight and straightenLast force straight pieces")
{
    std::vector<glm::dvec3> pts;
    for (int i = 0; i <= 100; ++i)
        pts.push_back({i * 0.5, 0.0, 4.0 * std::sin(i * 0.05)});
    CHECK(fitStroke(pts, {.tolerance = 0.5, .straight = true}).pieces[0].points.size() == 2);
    CHECK(fitStroke(pts, {.tolerance = 0.5, .straightenLast = true}).pieces.back().points.size() == 2);
    CHECK(fitStroke(pts, {.tolerance = 0.5}).pieces.back().points.size() > 2);
}

TEST_CASE("fit: a stroke shorter than the tolerance gives nothing")
{
    CHECK(fitStroke(fitLine({0, 0, 0}, {0.1, 0, 0}, 5), {.tolerance = 0.5}).pieces.empty());
}

