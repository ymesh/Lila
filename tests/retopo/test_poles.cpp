#include "TestUtil.h"
#include "ops/Command.h"
#include "geo/Primitives.h"
#include "retopo/Internal.h"
#include "retopo/Retopo.h"

#include <doctest/doctest.h>

#include <numeric>

using namespace lila;
using namespace retopotest;

namespace
{
int balance(const std::vector<retopo::Pole>& poles)
{
    return std::accumulate(poles.begin(), poles.end(), 0, [](int sum, const retopo::Pole& p) { return sum + 4 - p.valence; });
}

/** The pole nearest p, and its distance. */
std::pair<const retopo::Pole*, double> nearest(const std::vector<retopo::Pole>& poles, const glm::dvec3& p)
{
    const retopo::Pole* best = nullptr;
    double d = 1e300;
    for (const retopo::Pole& q : poles)
        if (glm::length(q.position - p) < d)
        {
            d = glm::length(q.position - p);
            best = &q;
        }
    return {best, d};
}
} // namespace

TEST_CASE("poles balance to the shell's total turning")
{
    retopo::QuadRemeshOptions o{.targetFaces = 600};
    retopo::PoleResult s = retopo::findPoles(sphere(48), o);
    REQUIRE(s.ok());
    REQUIRE(s.shellTotals.size() == 1);
    REQUIRE(s.shellTotals[0].has_value());
    // A sphere's field turns a whole turn twice over: eight three-edge poles' worth, like a cube's corners.
    CHECK(*s.shellTotals[0] == 8);
    CHECK(balance(s.poles) == 8);
    MESSAGE("sphere poles ", s.poles.size());

    retopo::PoleResult t = retopo::findPoles(geo::makeTorus({.subdivAxis = 48, .subdivHeight = 24}), o);
    REQUIRE(t.ok());
    REQUIRE(t.shellTotals[0].has_value());
    CHECK(*t.shellTotals[0] == 0);
    CHECK(balance(t.poles) == 0);
}

TEST_CASE("a cube's poles sit on its corners")
{
    retopo::PoleResult r = retopo::findPoles(geo::makeCube({.subdivX = 8, .subdivY = 8, .subdivZ = 8}), {.targetFaces = 600});
    REQUIRE(r.ok());
    CHECK(r.poles.size() == 8);
    for (const retopo::Pole& p : r.poles)
        CHECK(glm::length(glm::abs(p.position) - glm::dvec3(50.0)) < 15.0);
}

TEST_CASE("moved poles are where the field turns")
{
    geo::Mesh m = sphere(48);
    retopo::QuadRemeshOptions o{.targetFaces = 600};
    retopo::PoleResult before = retopo::findPoles(m, o);
    REQUIRE(before.ok());
    REQUIRE(!before.poles.empty());

    // Drag one pole a quarter of the way round the sphere.
    std::vector<retopo::Pole> moved = before.poles;
    glm::dvec3 from = moved[0].position;
    glm::dvec3 axis = glm::normalize(glm::cross(from, std::abs(from.y) < 40.0 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
    glm::dvec3 to = glm::normalize(from * std::cos(0.6) + glm::cross(axis, from) * std::sin(0.6)) * 50.0;
    moved[0].position = to;
    o.poles = moved;
    retopo::PoleResult after = retopo::findPoles(m, o);
    REQUIRE(after.ok());
    CHECK(after.poles.size() == moved.size());
    // Poles sit on the working triangles' corners, about a quad apart here.
    CHECK(nearest(after.poles, to).second < 8.0);
    for (const retopo::Pole& p : moved)
        CHECK(nearest(after.poles, p.position).second < 8.0);

    // A pair of opposite poles can be added anywhere; a lone one can't.
    std::vector<retopo::Pole> paired = before.poles;
    paired.push_back({glm::dvec3(0, 0, 50), 3});
    paired.push_back({glm::normalize(glm::dvec3(0, 0.3, 1)) * 50.0, 5});
    o.poles = paired;
    retopo::PoleResult withPair = retopo::findPoles(m, o);
    REQUIRE(withPair.ok());
    CHECK(withPair.poles.size() == paired.size());

    std::vector<retopo::Pole> lone = before.poles;
    lone.push_back({glm::dvec3(0, 0, 50), 3});
    o.poles = lone;
    CHECK(!retopo::findPoles(m, o).ok());
}

TEST_CASE("quad remesh honours prescribed poles")
{
    geo::Mesh m = sphere(48);
    retopo::QuadRemeshOptions o{.targetFaces = 600};
    retopo::PoleResult auto_ = retopo::findPoles(m, o);
    REQUIRE(auto_.ok());
    o.poles = auto_.poles;
    o.poles->front().position = glm::normalize(o.poles->front().position + glm::dvec3(0, 20, 0)) * 50.0;
    retopo::QuadRemeshResult r = retopo::quadRemesh(m, o);
    REQUIRE(r.ok());
    MESSAGE(r.metrics.summary());
    CHECK(r.metrics.quadPercent > 99.0);
}

TEST_CASE("retopoPoles and quadRemesh poles= round trip through commands")
{
    auto run = [](const char* name, nlohmann::json call) { return ops::execute(name, call); };
    run("newScene", nlohmann::json::object());
    std::string ball = run("polySphere", {{"name", "Ball"}, {"generator", false}, {"subdivisionsAxis", 40}, {"subdivisionsHeight", 40}})[0];
    run("move", {{"args", {ball}}, {"by", {100, 0, 0}}});
    nlohmann::json found = run("retopoPoles", {{"args", {ball}}, {"targetFaces", 600}});
    REQUIRE(found["poles"].size() == 8);
    CHECK(found["shellTotals"][0] == 8);
    // World space: the poles sit around the moved sphere.
    for (const auto& p : found["poles"])
        CHECK(p["position"][0].get<double>() > 40.0);
    nlohmann::json made = run("quadRemesh", {{"args", {ball}}, {"targetFaces", 600}, {"symmetry", "off"}, {"poles", found["poles"]}});
    CHECK(made["metrics"]["faces"].get<int>() > 300);

    nlohmann::json lone = found["poles"];
    lone.erase(lone.begin());
    CHECK_THROWS(run("quadRemesh", {{"args", {ball}}, {"targetFaces", 600}, {"symmetry", "off"}, {"poles", lone}}));
}

TEST_CASE("quadRemesh hides the kept source and errors without changing the scene on timeout")
{
    auto run = [](const char* name, nlohmann::json call) { return ops::execute(name, call); };
    run("newScene", nlohmann::json::object());
    std::string ball = run("polySphere", {{"name", "Ball"}, {"generator", false}})[0];
    nlohmann::json made = run("quadRemesh", {{"args", {ball}}});
    CHECK(!run("getAttr", {{"args", {ball + ".visibility"}}}).get<bool>());
    CHECK(made["metrics"]["faces"].get<int>() < 6000);
    run("undo", nlohmann::json::object());
    CHECK(run("getAttr", {{"args", {ball + ".visibility"}}}).get<bool>());

    size_t before = run("ls", {{"type", "mesh"}}).size();
    CHECK_THROWS_WITH(run("quadRemesh", {{"args", {ball}}, {"timeout", 1e-6}}), doctest::Contains("gave up"));
    CHECK(run("ls", {{"type", "mesh"}}).size() == before);
}

TEST_CASE("close opposite poles cancel and crowded poles move apart")
{
    namespace rd = retopo::detail;
    rd::Surface s = rd::surfaceFromMesh(sphere(48), false);
    s.buildTopology();
    s.buildGeometry();
    const std::vector<double> scale(s.vertexCount(), 10.0);
    auto centre = [&](geo::Index t) { return (s.pos[s.tris[t][0]] + s.pos[s.tris[t][1]] + s.pos[s.tris[t][2]]) / 3.0; };
    // A triangle, one beside it, and one on the far side.
    const geo::Index a = 0;
    geo::Index beside = geo::kInvalid, far = geo::kInvalid;
    double farthest = 0.0;
    for (geo::Index t = 1; t < geo::Index(s.tris.size()); ++t)
    {
        int shared = 0;
        for (geo::Index v : s.tris[t])
            shared += v == s.tris[a][0] || v == s.tris[a][1] || v == s.tris[a][2];
        if (shared == 2 && beside == geo::kInvalid)
            beside = t;
        if (const double d = glm::length(centre(t) - centre(a)); d > farthest)
        {
            farthest = d;
            far = t;
        }
    }
    REQUIRE(beside != geo::kInvalid);

    const std::vector<rd::FieldSingularity> left = rd::cancelClosePoles(s, {{a, 1}, {beside, -1}, {far, 1}}, scale, 1.5);
    REQUIRE(left.size() == 1);
    CHECK(left[0].tri == far);
    CHECK(left[0].index == 1);

    const std::vector<rd::FieldSingularity> apart = rd::spreadPoles(s, {{a, 1}, {beside, 1}, {far, -1}}, scale, 1.0);
    REQUIRE(apart.size() == 3);
    CHECK(apart[0].tri == a);
    CHECK(apart[2].tri == far);
    CHECK(apart[1].index == 1);
    CHECK(glm::length(centre(apart[1].tri) - centre(a)) > 0.8 * scale[0]);
}
