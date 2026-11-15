#include "geo/Primitives.h"
#include "retopo/Internal.h"
#include "retopo/Retopo.h"

#include <doctest/doctest.h>

#include <cmath>
#include <functional>

using namespace lila;
using geo::Index;

namespace
{
geo::Mesh controlSphere(int subdiv = 64)
{
    geo::SphereOptions o;
    o.radius = 50.0;
    o.subdivAxis = subdiv;
    o.subdivHeight = subdiv;
    return geo::makeSphere(o);
}

geo::Mesh controlTorus()
{
    geo::TorusOptions o;
    o.radius = 50.0;
    o.sectionRadius = 20.0;
    o.subdivAxis = 96;
    o.subdivHeight = 48;
    return geo::makeTorus(o);
}

/** A bumpy open tile, 100 x 100 in xz. */
geo::Mesh controlTile()
{
    geo::PlaneOptions o;
    o.subdivX = 40;
    o.subdivZ = 40;
    geo::Mesh m = geo::makePlane(o);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            glm::dvec3 at = m.position(p);
            at.y = 5.0 * std::sin(at.x / 15.0) * std::cos(at.z / 20.0);
            m.setPosition(p, at);
        }
    return m;
}

double controlMeanEdgeNear(const geo::Mesh& m, const std::function<bool(const glm::dvec3&)>& where)
{
    double sum = 0.0;
    int count = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && where(0.5 * (m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1)))))
        {
            sum += m.edgeLength(e);
            ++count;
        }
    return count ? sum / count : 0.0;
}

double controlOffCount(const retopo::QuadRemeshResult& r, int target) { return std::abs(double(r.mesh.numFaces()) / target - 1.0); }
} // namespace

TEST_CASE("quad remesh lands near the asked face count")
{
    for (int target : {300, 1200})
    {
        retopo::QuadRemeshOptions o;
        o.targetFaces = target;
        retopo::QuadRemeshResult r = retopo::quadRemesh(controlSphere(), o);
        REQUIRE(r.ok());
        std::string tried;
        for (const auto& attempt : r.attempts)
            tried += " " + std::to_string(attempt.faces);
        INFO(target << " -> " << r.mesh.numFaces() << ", attempts" << tried);
        CHECK(controlOffCount(r, target) <= o.countTolerance);
        CHECK(!r.attempts.empty());
        CHECK(!r.layout.empty());
    }
}

TEST_CASE("quad remesh density handles change face size where they sit")
{
    for (double scale : {0.5, 2.0})
    {
        retopo::QuadRemeshOptions o;
        o.targetFaces = 1500;
        o.adaptivity = 0.0;
        o.densityHandles.push_back({glm::dvec3(50.0, 0.0, 0.0), 35.0, scale});
        retopo::QuadRemeshResult r = retopo::quadRemesh(controlSphere(), o);
        REQUIRE(r.ok());
        const double tip = controlMeanEdgeNear(r.mesh, [](const glm::dvec3& p) { return p.x > 47.0; });
        const double far = controlMeanEdgeNear(r.mesh, [](const glm::dvec3& p) { return p.x < -30.0; });
        INFO("scale " << scale << ": tip " << tip << " far " << far);
        // Relaxing evens edges out, so the change comes out a little gentler than asked.
        if (scale < 1.0)
            CHECK(tip < 0.8 * far);
        else
            CHECK(tip > 1.25 * far);
    }
}

TEST_CASE("quad remesh feature guides put an edge loop on the curve")
{
    // A loop round the tube at an angle nothing else picks out.
    const double phi = 0.37;
    const glm::dvec3 radial(std::cos(phi), 0.0, std::sin(phi)), centre = 50.0 * radial;
    std::vector<glm::dvec3> loop;
    for (int k = 0; k <= 64; ++k)
    {
        const double t = 6.283185307179586 * k / 64.0;
        loop.push_back(centre + 20.0 * (std::cos(t) * radial + std::sin(t) * glm::dvec3(0.0, 1.0, 0.0)));
    }
    const auto onLoop = [&](const geo::Mesh& m) {
        int count = 0;
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
            {
                const glm::dvec3 d = m.position(p) - centre;
                const double inPlane = std::abs(glm::dot(d, glm::cross(radial, glm::dvec3(0.0, 1.0, 0.0))));
                count += inPlane < 0.05;
            }
        return count;
    };
    retopo::QuadRemeshOptions o;
    o.targetFaces = 1500;
    retopo::QuadRemeshResult plain = retopo::quadRemesh(controlTorus(), o);
    o.featureGuides.push_back(loop);
    retopo::QuadRemeshResult guided = retopo::quadRemesh(controlTorus(), o);
    REQUIRE(plain.ok());
    REQUIRE(guided.ok());
    INFO("plain " << onLoop(plain.mesh) << " guided " << onLoop(guided.mesh));
    // About 2 pi 20 / 5 cm edges.
    CHECK(onLoop(guided.mesh) >= 20);
    CHECK(onLoop(plain.mesh) < 8);
    CHECK(guided.defects == 0);
}

TEST_CASE("quad remesh grid method lays a straight grid over an open sheet")
{
    retopo::QuadRemeshOptions o;
    o.method = retopo::RemeshMethod::Grid;
    o.targetFaces = 400;
    const geo::Mesh src = controlTile();
    retopo::QuadRemeshResult r = retopo::quadRemesh(src, o);
    REQUIRE(r.ok());
    CHECK(r.layout == "grid");
    CHECK(r.defects == 0);
    CHECK(r.metrics.irregular == 0);
    CHECK(r.metrics.quadPercent == 100.0);
    CHECK(controlOffCount(r, 400) <= 0.02);
    CHECK(r.mesh.validate().empty());
    // The tile's corners stay corners.
    for (glm::dvec3 corner : {glm::dvec3(-50, 0, -50), glm::dvec3(50, 0, -50), glm::dvec3(50, 0, 50), glm::dvec3(-50, 0, 50)})
    {
        corner.y = 5.0 * std::sin(corner.x / 15.0) * std::cos(corner.z / 20.0);
        double best = 1e9;
        for (Index p = 0; p < r.mesh.pointCapacity(); ++p)
            if (r.mesh.pointValid(p))
                best = std::min(best, glm::distance(r.mesh.position(p), corner));
        CHECK(best < 1e-6);
    }

    // A closed shape has no single side to look down from, so it gets the field layout.
    retopo::QuadRemeshResult closed = retopo::quadRemesh(controlSphere(), o);
    REQUIRE(closed.ok());
    CHECK(closed.layout != "grid");
}

TEST_CASE("quad remesh reports where each requested pole landed")
{
    const geo::Mesh src = controlSphere();
    retopo::QuadRemeshOptions o;
    o.targetFaces = 800;
    retopo::PoleResult found = retopo::findPoles(src, o);
    REQUIRE(found.ok());
    REQUIRE(!found.poles.empty());
    o.poles = found.poles;
    retopo::QuadRemeshResult r = retopo::quadRemesh(src, o);
    REQUIRE(r.ok());
    REQUIRE(r.poleMatches.size() == found.poles.size());
    int matched = 0;
    for (const retopo::PoleMatch& m : r.poleMatches)
        matched += m.point != geo::kInvalid && m.valenceMatched;
    CHECK(matched >= int(found.poles.size()) - 1);
}

TEST_CASE("quad remesh keeps an inside-out source's winding")
{
    geo::Mesh src = controlSphere(48);
    for (Index f = 0; f < src.faceCapacity(); ++f)
        if (src.faceValid(f))
            src.flipFace(f);
    retopo::QuadRemeshOptions o;
    o.targetFaces = 600;
    retopo::QuadRemeshResult r = retopo::quadRemesh(src, o);
    REQUIRE(r.ok());
    CHECK(r.defects == 0);
    int inward = 0;
    for (Index f = 0; f < r.mesh.faceCapacity(); ++f)
        if (r.mesh.faceValid(f))
            inward += glm::dot(r.mesh.faceAreaVector(f), r.mesh.faceCentroid(f)) < 0.0;
    CHECK(inward == int(r.mesh.numFaces()));
}

TEST_CASE("crease junctions split their fan into sectors of whole quarter turns")
{
    // A triangular prism: each top corner has a 60 degree face corner and two 90 degree side corners.
    geo::Mesh m;
    std::vector<Index> top, bottom;
    for (int k = 0; k < 3; ++k)
    {
        const double a = 6.283185307179586 * k / 3.0;
        top.push_back(m.addPoint({50.0 * std::cos(a), 50.0, 50.0 * std::sin(a)}));
        bottom.push_back(m.addPoint({50.0 * std::cos(a), 0.0, 50.0 * std::sin(a)}));
    }
    m.addFace({top[0], top[2], top[1]});
    m.addFace({bottom[0], bottom[1], bottom[2]});
    for (int k = 0; k < 3; ++k)
        m.addFace({top[k], top[(k + 1) % 3], bottom[(k + 1) % 3], bottom[k]});
    retopo::detail::Surface s = retopo::detail::surfaceFromMesh(m, true);
    retopo::detail::refineSurface(s, 8.0);
    s.buildTopology();
    s.buildGeometry();
    s.detectFeatures(40.0);
    const std::vector<retopo::detail::CreaseJunction> junctions = retopo::detail::creaseJunctions(s);
    REQUIRE(junctions.size() == 6);
    for (const retopo::detail::CreaseJunction& j : junctions)
    {
        double total = 0.0, stretched = 0.0;
        for (const auto& w : j.fan)
        {
            total += w.angle;
            stretched += w.angle * w.alpha;
        }
        CHECK(std::abs(total - glm::radians(240.0)) < 1e-6);
        // 60 degrees becomes one quarter turn, each 90 stays one: three quarter turns round the corner.
        CHECK(std::abs(stretched - glm::radians(270.0)) < 1e-6);
    }
}

TEST_CASE("quad remesh puts enough edges round thin parts")
{
    // A fat body and a thin arm beside it, which on its own gets few edges round it.
    geo::CapsuleOptions bodyOptions;
    bodyOptions.radius = 30.0;
    bodyOptions.height = 120.0;
    bodyOptions.subdivAxis = 48;
    bodyOptions.subdivHeight = 24;
    geo::Mesh m = geo::makeCapsule(bodyOptions);
    geo::CapsuleOptions armOptions;
    armOptions.radius = 3.0;
    armOptions.height = 80.0;
    armOptions.subdivAxis = 24;
    armOptions.subdivHeight = 40;
    const geo::Mesh arm = geo::makeCapsule(armOptions);
    std::vector<Index> id(arm.pointCapacity(), geo::kInvalid);
    for (Index p = 0; p < arm.pointCapacity(); ++p)
        if (arm.pointValid(p))
            id[p] = m.addPoint(glm::dvec3(arm.position(p).y + 80.0, -arm.position(p).x, arm.position(p).z));
    for (Index f = 0; f < arm.faceCapacity(); ++f)
        if (arm.faceValid(f))
        {
            std::vector<Index> pts;
            for (Index p : arm.facePoints(f))
                pts.push_back(id[p]);
            m.addFace(pts);
        }
    // Edges crossing the plane x = 80, halfway along the arm.
    const auto around = [](const geo::Mesh& r) {
        int count = 0;
        for (Index e = 0; e < r.edgeCapacity(); ++e)
            if (r.edgeValid(e))
                count += (r.position(r.edgePoint(e, 0)).x < 80.0) != (r.position(r.edgePoint(e, 1)).x < 80.0);
        return count;
    };
    retopo::QuadRemeshOptions o;
    o.targetFaces = 600;
    o.edgesAround = 0;
    retopo::QuadRemeshResult plain = retopo::quadRemesh(m, o);
    o.edgesAround = 12;
    retopo::QuadRemeshResult capped = retopo::quadRemesh(m, o);
    REQUIRE(plain.ok());
    REQUIRE(capped.ok());
    INFO("plain " << around(plain.mesh) << " capped " << around(capped.mesh));
    CHECK(around(capped.mesh) > around(plain.mesh));
    CHECK(around(capped.mesh) >= 12);
}
