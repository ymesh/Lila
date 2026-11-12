#include <doctest/doctest.h>

#include "TestUtil.h"

#include "curve/CurveNodes.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "scene/TransformOps.h"

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;
using curve::PointRole;
using namespace curvetest;

namespace
{
const curve::Splines& splines(const std::string& name) { return curve::shapeSplines(scene::Graph::current(), idOf(name)); }

D3 positionOf(const std::string& name, uint32_t id)
{
    for (auto& [c, p] : curve::componentPositions(splines(name)))
        if (c == id)
            return p;
    FAIL("no component");
    return D3(0.0);
}

void checkOrthonormal(const glm::dmat3& f)
{
    for (int i = 0; i < 3; ++i)
        CHECK(glm::length(f[i]) == doctest::Approx(1.0));
    CHECK(glm::dot(f[0], f[1]) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(glm::dot(f[0], f[2]) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(glm::length(glm::cross(f[0], f[1]) - f[2]) == doctest::Approx(0.0).epsilon(1e-9));
}
} // namespace

TEST_CASE("curve frames: circle tangent, in-plane normal, plane binormal")
{
    run("newScene");
    std::string c = run("circle", {{"radius", 10.0}, {"normal", {0, 0, 1}}, {"name", "Ring"}});
    const curve::Spline& s = splines(c)[0];
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < s.size(); ++i)
        ids.push_back(curve::componentId(0, i, PointRole::Point));
    std::vector<glm::dmat3> frames = curve::componentFrames(splines(c), ids);
    REQUIRE(frames.size() == ids.size());
    double side = 0.0;
    for (size_t k = 0; k < ids.size(); ++k)
    {
        const glm::dmat3& f = frames[k];
        checkOrthonormal(f);
        // Control points off the circle take the frame where it passes closest, which is on the same ray.
        D3 radial = glm::normalize(D3(s.points[k].x, s.points[k].y, 0.0));
        CHECK(glm::dot(f[0], radial) == doctest::Approx(0.0).epsilon(1e-6));
        CHECK(std::abs(glm::dot(f[1], D3(0, 0, 1))) == doctest::Approx(1.0));
        double toward = glm::dot(f[2], radial);
        CHECK(std::abs(toward) == doctest::Approx(1.0).epsilon(1e-6));
        if (k == 0)
            side = toward;
        CHECK(toward * side > 0.0);
    }
}

TEST_CASE("curve frames: Bezier handles take their anchor's frame along their own direction")
{
    run("newScene");
    std::string c = run("bezierCurve", {{"points", pts({D3(10, 0, 0), D3(0, 0, -10), D3(-10, 0, 0), D3(0, 0, 10)})}, {"closed", true}, {"name", "B"}});
    const curve::Spline& s = splines(c)[0];
    for (size_t i = 0; i < s.size(); ++i)
    {
        auto f = curve::componentFrames(splines(c), {curve::componentId(0, i, PointRole::Point), curve::componentId(0, i, PointRole::HandleL),
                                                       curve::componentId(0, i, PointRole::HandleR)});
        for (const glm::dmat3& m : f)
            checkOrthonormal(m);
        CHECK(glm::dot(f[0][0], glm::normalize(s.handleR[i] - s.points[i])) == doctest::Approx(1.0));
        CHECK(std::abs(glm::dot(f[0][2], glm::normalize(s.points[i]))) == doctest::Approx(1.0).epsilon(1e-6));
        // Auto handles are smooth, so both share the anchor's frame.
        for (int h = 1; h < 3; ++h)
            for (int axis = 0; axis < 3; ++axis)
                CHECK(glm::length(f[h][axis] - f[0][axis]) == doctest::Approx(0.0).epsilon(1e-6));
    }
}

TEST_CASE("curve frames: straight lines get a stable frame")
{
    run("newScene");
    for (D3 dir : {D3(1, 0, 0), D3(0, 1, 0), D3(1, 1, 1)})
    {
        std::string c = run("curve", {{"type", "poly"}, {"points", pts({D3(0), dir * 5.0, dir * 10.0})}});
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < 3; ++i)
            ids.push_back(curve::componentId(0, i, PointRole::Point));
        std::vector<glm::dmat3> frames = curve::componentFrames(splines(c), ids);
        for (const glm::dmat3& f : frames)
        {
            checkOrthonormal(f);
            CHECK(glm::dot(f[0], glm::normalize(dir)) == doctest::Approx(1.0));
            for (int axis = 0; axis < 3; ++axis)
                CHECK(glm::length(f[axis] - frames[0][axis]) == doctest::Approx(0.0).epsilon(1e-9));
        }
    }
}

TEST_CASE("curve points move along their own frames, and undo")
{
    run("newScene");
    std::string c = run("bezierCurve", {{"points", pts({D3(10, 0, 0), D3(0, 0, -10), D3(-10, 0, 0), D3(0, 0, 10)})}, {"closed", true}, {"name", "B"}});
    const uint32_t p0 = curve::componentId(0, 0, PointRole::Point), p2 = curve::componentId(0, 2, PointRole::Point);
    const glm::dmat3 f0 = curve::componentFrames(splines(c), {p0})[0];

    // The manipulator frame of one point is that point's frame.
    run("selectComponents", {{"args", {c}}, {"vertices", json::array({p0})}});
    glm::dmat3 manip = scene::selectionNormalFrame();
    for (int axis = 0; axis < 3; ++axis)
        CHECK(glm::length(manip[axis] - f0[axis]) == doctest::Approx(0.0).epsilon(1e-9));

    // N pushes opposite points of the loop the same way relative to the centre.
    run("selectComponents", {{"args", {c}}, {"vertices", json::array({p0, p2})}});
    json start = run("curveData", {{"args", {c}}});
    run("moveComponents", {{"uvn", {0, 0, 2}}});
    const double r0 = glm::length(positionOf(c, p0)), r2 = glm::length(positionOf(c, p2));
    CHECK(std::abs(r0 - 10.0) == doctest::Approx(2.0));
    CHECK(r2 == doctest::Approx(r0));
    run("undo");
    CHECK(run("curveData", {{"args", {c}}}) == start);

    // V lifts out of the plane; normal= alone takes the same path as uvn= along N.
    run("moveComponents", {{"uvn", {0, 3, 0}}});
    CHECK(std::abs(positionOf(c, p0).y) == doctest::Approx(3.0));
    CHECK(positionOf(c, p2).y == doctest::Approx(positionOf(c, p0).y));
    run("undo");
    run("moveComponents", {{"normal", 2.0}});
    CHECK(glm::length(positionOf(c, p0)) == doctest::Approx(r0));
    run("undo");
    CHECK(run("curveData", {{"args", {c}}}) == start);

    // U on a handle slides it along itself.
    const uint32_t h = curve::componentId(0, 1, PointRole::HandleR), a = curve::componentId(0, 1, PointRole::Point);
    const D3 anchor = positionOf(c, a), before = positionOf(c, h);
    run("selectComponents", {{"args", {c}}, {"vertices", json::array({h})}});
    run("moveComponents", {{"uvn", {1.5, 0, 0}}});
    const D3 after = positionOf(c, h);
    CHECK(glm::length(after - anchor) == doctest::Approx(glm::length(before - anchor) + 1.5));
    CHECK(glm::dot(glm::normalize(after - anchor), glm::normalize(before - anchor)) == doctest::Approx(1.0));
    CHECK(positionOf(c, a) == anchor);
    run("undo");
    CHECK(run("curveData", {{"args", {c}}}) == start);
}

TEST_CASE("orientPivot on curve points: two aim first to second, one along its tangent")
{
    run("newScene");
    std::string c = run("curve", {{"type", "poly"}, {"points", pts({D3(0), D3(10, 0, 0), D3(10, 20, -4)})}});
    scene::Graph& g = scene::Graph::current();
    const scene::NodeId id = idOf(c);
    const uint32_t p0 = curve::componentId(0, 0, PointRole::Point), p1 = curve::componentId(0, 1, PointRole::Point),
                   p2 = curve::componentId(0, 2, PointRole::Point);

    run("selectComponents", {{"args", {c}}, {"vertices", json::array({p1})}});
    run("selectComponents", {{"args", {c}}, {"vertices", json::array({p0})}, {"add", true}});
    run("orientPivot", {{"mode", "pivot"}});
    CHECK(glm::length(scene::xformops::pivotFrame(g, id)[0] - D3(-1, 0, 0)) < 1e-9);
    CHECK(glm::length(scene::xformops::worldPivot(g, id) - D3(10, 0, 0)) < 1e-9);

    run("selectComponents", {{"args", {c}}, {"vertices", json::array({p2})}});
    const glm::dmat3 f2 = curve::componentFrames(splines(c), {p2})[0];
    const D3 world2 = D3(g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix) * glm::dvec4(positionOf(c, p2), 1.0));
    run("orientPivot");
    const glm::dmat4 w = g.node(id)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    CHECK(glm::length(D3(w[0]) - glm::normalize(f2[0])) < 1e-6);
    CHECK(glm::length(D3(w[3]) - world2) < 1e-6);
    CHECK(glm::length(D3(w * glm::dvec4(positionOf(c, p2), 1.0)) - world2) < 1e-6);
}
