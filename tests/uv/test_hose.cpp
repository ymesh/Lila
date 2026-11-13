#include "TestUtil.h"
#include "curve/Pipe.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "uv/Uv.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

using namespace lila;
using geo::Index;
using D3 = glm::dvec3;

namespace
{
curve::Spline line(std::vector<D3> pts)
{
    curve::Spline s;
    s.kind = curve::Kind::Poly;
    s.points = std::move(pts);
    s.normalize();
    return s;
}

std::vector<uv::Chart> hoseCharts(const geo::Mesh& m)
{
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    return uv::buildCharts(m, {}, co);
}

size_t folded(const geo::Mesh& m)
{
    size_t n = 0;
    for (const uv::Chart& ch : hoseCharts(m))
        n += uv::foldedTriangles(ch);
    return n;
}

std::vector<Index> seamEdges(const geo::Mesh& m)
{
    std::vector<Index> out;
    std::span<const bool> seam = m.edgeAttrs().get<bool>(geo::attr::kSeam);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && !m.isBoundaryEdge(e) && seam[e])
            out.push_back(e);
    return out;
}

/** Islands whose uvs fill their bounding box: straightened strips, square to the axes. */
size_t squareStrips(const geo::Mesh& m)
{
    size_t n = 0;
    for (const uv::Chart& ch : hoseCharts(m))
    {
        glm::dvec2 size = ch.uvMax() - ch.uvMin();
        n += std::abs(std::abs(ch.areaUv()) - size.x * size.y) < 1e-6 * size.x * size.y;
    }
    return n;
}

/** Mean position of the seams that run along Z (the rail of a tube along Z). */
D3 railCenter(const geo::Mesh& m)
{
    D3 c(0.0);
    int n = 0;
    for (Index e : seamEdges(m))
    {
        D3 d = m.position(m.edgePoint(e, 1)) - m.position(m.edgePoint(e, 0));
        if (std::abs(glm::normalize(d).z) > 0.9)
        {
            c += 0.5 * (m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1)));
            ++n;
        }
    }
    return n ? c / double(n) : c;
}

geo::Mesh tube(double length, int rings, bool caps)
{
    geo::CylinderOptions o;
    o.radius = 5.0;
    o.height = length;
    o.subdivAxis = 16;
    o.subdivHeight = rings;
    o.caps = caps;
    o.capFill = geo::CapFill::Quads;
    geo::Mesh m = geo::makeCylinder(o);
    // Lay it along Z so "up" (Y) is across the tube.
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            D3 q = m.position(p);
            m.setPosition(p, D3(q.x, -q.z, q.y));
        }
    return m;
}
} // namespace

TEST_CASE("hose: capped tube becomes one straight strip and two round caps")
{
    geo::Mesh m = tube(40.0, 8, true);
    uv::HoseOptions o;
    o.visibility = false;
    uv::HoseResult r = uv::hoseUv(m, {}, "uv0", o);
    CHECK(r.strips == 1);
    CHECK(r.caps == 2);
    CHECK(r.otherFaces == 0);
    CHECK(r.lengthCuts == 0);
    CHECK(r.unstraightened == 0);
    CHECK(hoseCharts(m).size() == 3);
    CHECK(folded(m) == 0);
    // The strip is laid square to the axes, so packing keeps it straight.
    for (const uv::Chart& ch : hoseCharts(m))
        if (ch.faces.size() == 16 * 8)
        {
            glm::dvec2 size = ch.uvMax() - ch.uvMin();
            CHECK(std::abs(ch.areaUv()) == doctest::Approx(size.x * size.y).epsilon(1e-6));
        }
    // One rail of 8 edges along the tube; the rest of the seams are the two cap rings.
    size_t along = 0;
    for (Index e : seamEdges(m))
        along += std::abs(glm::normalize(m.position(m.edgePoint(e, 1)) - m.position(m.edgePoint(e, 0))).z) > 0.9;
    CHECK(along == 8);
}

TEST_CASE("hose: a torus gets one ring cut and one rail")
{
    geo::TorusOptions o;
    o.radius = 20.0;
    o.sectionRadius = 4.0;
    geo::Mesh m = geo::makeTorus(o);
    uv::HoseOptions ho;
    ho.maxAspect = 0.0;
    uv::HoseResult r = uv::hoseUv(m, {}, "uv0", ho);
    CHECK(r.strips == 1);
    CHECK(r.caps == 0);
    CHECK(r.otherFaces == 0);
    CHECK(hoseCharts(m).size() == 1);
    CHECK(seamEdges(m).size() == size_t(o.subdivAxis + o.subdivHeight));
    CHECK(folded(m) == 0);
}

TEST_CASE("hose: long runs are cut into strips no longer than the limit")
{
    geo::Mesh m = tube(400.0, 80, false);
    uv::HoseOptions o;
    o.visibility = false;
    o.maxAspect = 3.0; // circumference ~31, so strips under ~94 long
    uv::HoseResult r = uv::hoseUv(m, {}, "uv0", o);
    CHECK(r.lengthCuts == 4);
    CHECK(r.strips == 5);
    for (const uv::Chart& ch : hoseCharts(m))
    {
        double lo = 1e9, hi = -1e9;
        for (const D3& p : ch.pos)
            lo = std::min(lo, p.z), hi = std::max(hi, p.z);
        CHECK(hi - lo <= 3.0 * 2.0 * glm::pi<double>() * 5.0 + 1e-6);
    }
}

TEST_CASE("hose: rails and cuts hide from views and planes")
{
    SUBCASE("a plane above puts the rail underneath")
    {
        geo::Mesh m = tube(40.0, 8, false);
        uv::HoseOptions o;
        o.visibility = false;
        o.views.push_back({.position = D3(0, 50, 0), .weight = 1.0, .planeNormal = D3(0, 1, 0)});
        uv::hoseSeams(m, {}, o);
        CHECK(railCenter(m).y < -4.0);
    }
    SUBCASE("a camera below puts the rail on top")
    {
        geo::Mesh m = tube(40.0, 8, false);
        uv::HoseOptions o;
        o.visibility = false;
        o.views.push_back({.position = D3(0, -60, 20), .weight = 1.0, .planeNormal = std::nullopt});
        uv::hoseSeams(m, {}, o);
        CHECK(railCenter(m).y > 4.0);
    }
    SUBCASE("length cuts move away from what a camera sees")
    {
        geo::Mesh m = tube(200.0, 40, false);
        uv::HoseOptions o;
        o.visibility = false;
        o.maxLength = 120.0; // one cut, anywhere from 80 to 120
        o.views.push_back({.position = D3(0, 30, 85), .weight = 4.0, .planeNormal = std::nullopt});
        uv::hoseSeams(m, {}, o);
        double cutZ = 0.0;
        int n = 0;
        for (Index e : seamEdges(m))
        {
            D3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
            if (std::abs(a.z - b.z) < 1e-6)
                cutZ += a.z, ++n;
        }
        REQUIRE(n > 0);
        CHECK(std::abs(cutZ / n - 85.0) > 20.0);
    }
}

TEST_CASE("hose: pipe tool presets lay out with no leftovers")
{
    const curve::Spline path = line({D3(0), D3(100, 0, 0), D3(100, 80, 0), D3(200, 80, 40)});
    struct Case
    {
        const char* name;
        curve::PipeOptions o;
        size_t caps = 0;
    };
    std::vector<Case> cases;
    {
        curve::PipeOptions o;
        o.startCap = o.endCap = curve::CapMode::Grid;
        cases.push_back({"grid caps", o, 2});
    }
    {
        curve::PipeOptions o;
        o.startCap = o.endCap = curve::CapMode::Round;
        cases.push_back({"round caps", o, 2});
    }
    {
        curve::PipeOptions o;
        o.wall = 1.0;
        cases.push_back({"wall with rims", o, 2});
    }
    {
        curve::PipeOptions o;
        o.corrugationDepth = 0.15;
        o.startCap = o.endCap = curve::CapMode::Ngon;
        cases.push_back({"corrugated hose", o, 2});
    }
    {
        curve::PipeOptions o;
        o.coupling = curve::CouplingKind::Collar;
        o.splitEvery = 60.0;
        cases.push_back({"collars", o, 0});
    }
    for (Case& c : cases)
    {
        CAPTURE(c.name);
        geo::Mesh m = curve::makePipe(path, c.o).mesh;
        uv::HoseResult r = uv::hoseUv(m, {}, "uv0");
        CHECK(r.strips >= 1);
        CHECK(r.caps == c.caps);
        CHECK(r.otherFaces == 0);
        CHECK(r.unstraightened == 0);
        CHECK(folded(m) == 0);
        CHECK(squareStrips(m) == r.strips);
    }
}

TEST_CASE("hose: triangles are not tubes and fall back to auto seams")
{
    geo::Mesh m = tube(40.0, 8, false);
    geo::triangulateFaces(m, uv::liveFaces(m));
    uv::HoseResult r = uv::hoseUv(m, {}, "uv0");
    CHECK(r.strips == 0);
    CHECK(r.otherFaces == m.numFaces());
    CHECK(folded(m) == 0);
}
