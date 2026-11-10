#include "TestUtil.h"

#include "geo/Bridge.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"

#include <cmath>
#include <functional>
#include <numbers>

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
/** A mesh with some faces deleted, the points that went with them, and the border edges left round the gap. */
struct Gap
{
    Mesh mesh;
    std::vector<glm::dvec3> removed;
    std::vector<Index> border;
    Index pointsBefore = 0;
    Index facesBefore = 0;
};

Gap cut(Mesh m, const std::function<bool(const glm::dvec3&)>& del, const std::function<bool(const glm::dvec3&)>& keepBorder = {})
{
    Gap g;
    g.facesBefore = m.numFaces();
    std::vector<Index> faces;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && del(m.faceCentroid(f)))
            faces.push_back(f);
    std::vector<glm::dvec3> before;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        before.push_back(m.position(p));
    deleteFaces(m, faces);
    for (Index p = 0; p < Index(before.size()); ++p)
        if (!m.pointValid(p))
            g.removed.push_back(before[p]);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
        {
            const glm::dvec3 mid = (m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1))) * 0.5;
            if (!keepBorder || keepBorder(mid))
                g.border.push_back(e);
        }
    g.pointsBefore = m.pointCapacity();
    g.mesh = std::move(m);
    return g;
}

/** Farthest any new point lies from the nearest point the cut removed. */
double worstRebuild(const Gap& g)
{
    double worst = 0.0;
    for (Index p = g.pointsBefore; p < g.mesh.pointCapacity(); ++p)
    {
        if (!g.mesh.pointValid(p))
            continue;
        double best = 1e30;
        for (const glm::dvec3& q : g.removed)
            best = std::min(best, glm::distance(g.mesh.position(p), q));
        worst = std::max(worst, best);
    }
    return worst;
}

BridgeOptions autoOptions()
{
    BridgeOptions o;
    o.curve = BridgeCurve::Auto;
    o.divisions = -1;
    return o;
}

/** Two open tubes on the y axis facing each other across a gap, with sides sides each. */
Mesh twoTubes(int sidesA, int sidesB, double gap = 100.0)
{
    Mesh a = makeCylinder({50.0, 100.0, sidesA, 1, 0, false});
    Mesh b = makeCylinder({50.0, 100.0, sidesB, 1, 0, false});
    for (glm::dvec3& p : a.positions())
        p.y -= 50.0 + gap / 2.0;
    for (glm::dvec3& p : b.positions())
        p.y += 50.0 + gap / 2.0;
    a.append(b);
    return a;
}

std::vector<Index> innerRims(const Mesh& m)
{
    std::vector<Index> out;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e) && std::abs(m.position(m.edgePoint(e, 0)).y) < 60.0)
            out.push_back(e);
    return out;
}

/** Points of new faces at height y, as an average distance from the y axis. */
double radiusAt(const Mesh& m, Index firstNewPoint, double y)
{
    double sum = 0.0;
    int n = 0;
    for (Index p = firstNewPoint; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && std::abs(m.position(p).y - y) < 1e-6)
        {
            sum += std::hypot(m.position(p).x, m.position(p).z);
            ++n;
        }
    return n ? sum / n : -1.0;
}
} // namespace

TEST_CASE("bridge: Auto rebuilds a deleted strip of a cylinder")
{
    Gap g = cut(makeCylinder({50.0, 400.0, 16, 8, 0, false}), [](const glm::dvec3& c) { return std::abs(c.y) < 50.0; },
                [](const glm::dvec3& mid) { return std::abs(mid.y) < 60.0; });
    REQUIRE(g.border.size() == 32);
    const BridgeResult r = bridge(g.mesh, g.border, autoOptions());
    CHECK(r.rows == 2);
    CHECK(r.faces.size() == 32);
    CHECK(isValid(g.mesh));
    CHECK(boundaryEdgeCount(g.mesh) == 32);
    CHECK(worstRebuild(g) < 1e-6);
}

TEST_CASE("bridge: Auto rebuilds a deleted band of a sphere")
{
    Gap g = cut(makeSphere({50.0, 16, 16}), [](const glm::dvec3& c) { return std::abs(c.y) < 12.0; });
    REQUIRE(g.border.size() == 32);
    const BridgeResult r = bridge(g.mesh, g.border, autoOptions());
    CHECK(r.rows == 2);
    CHECK(isValid(g.mesh));
    CHECK(boundaryEdgeCount(g.mesh) == 0);
    CHECK(worstRebuild(g) < 1e-3);
}

TEST_CASE("bridge: Auto rebuilds deleted strips of a torus both ways")
{
    // Across the tube: a wedge of the ring.
    Gap ring = cut(makeTorus({50.0, 20.0, 24, 12}), [](const glm::dvec3& c) {
        return std::abs(std::atan2(c.z, c.x)) < glm::radians(20.0);
    });
    REQUIRE(ring.border.size() == 24);
    BridgeResult r = bridge(ring.mesh, ring.border, autoOptions());
    CHECK(r.rows == 2);
    CHECK(isValid(ring.mesh));
    CHECK(boundaryEdgeCount(ring.mesh) == 0);
    CHECK(worstRebuild(ring) < 1e-3);

    // Along the tube: a band round the top of it.
    Gap band = cut(makeTorus({50.0, 20.0, 24, 12}), [](const glm::dvec3& c) { return c.y > 15.0; });
    REQUIRE(band.border.size() == 48);
    r = bridge(band.mesh, band.border, autoOptions());
    CHECK(r.pairsBridged == 1);
    CHECK(isValid(band.mesh));
    CHECK(boundaryEdgeCount(band.mesh) == 0);
    CHECK(worstRebuild(band) < 1e-3);
}

TEST_CASE("bridge: open ends weld to the border between the sides")
{
    // A 4 x 2 hole in a sheet; bridging only its long sides lands the rows on the short sides' middle points.
    auto hole = [] {
        return cut(makePlane({600, 500, 6, 5}), [](const glm::dvec3& c) { return std::abs(c.x) < 200 && c.z > -50 && c.z < 150; },
                   [](const glm::dvec3& mid) { return std::abs(mid.x) < 199 && (std::abs(mid.z + 50) < 1 || std::abs(mid.z - 150) < 1); });
    };
    Gap g = hole();
    REQUIRE(g.border.size() == 8);
    BridgeResult r = bridge(g.mesh, g.border, autoOptions());
    CHECK(r.rows == 2);
    CHECK(r.welded == 2);
    CHECK(r.split == 0);
    CHECK(r.unwelded == 0);
    CHECK(isValid(g.mesh));
    CHECK(g.mesh.numFaces() == g.facesBefore);
    CHECK(boundaryEdgeCount(g.mesh) == 22);
    CHECK(worstRebuild(g) < 1e-6);

    // Three rows against one border point each end: two rows split the end borders, the point stays as a corner.
    Gap h = hole();
    BridgeOptions o = autoOptions();
    o.divisions = 2;
    r = bridge(h.mesh, h.border, o);
    CHECK(r.rows == 3);
    CHECK(r.split + r.welded == 4);
    CHECK(r.unwelded + r.welded == 2);
    CHECK(isValid(h.mesh));
    CHECK(boundaryEdgeCount(h.mesh) == 22);
}

TEST_CASE("bridge: a sliver between two curves meeting at both ends leaves no loose points")
{
    // An upper arc of 7 points and a lower arc of 5, sharing their end points, with a row of quads beyond each.
    std::vector<glm::dvec3> pts;
    std::vector<Index> sizes, idx;
    auto upper = [](double t) { return glm::dvec3(-300 + 600 * t, 0, 40 * std::sin(std::numbers::pi * t)); };
    auto lower = [](double t) { return glm::dvec3(-300 + 600 * t, 0, -30 * std::sin(std::numbers::pi * t)); };
    std::vector<Index> U, L, UO, LO;
    for (int i = 0; i <= 6; ++i)
    {
        U.push_back(Index(pts.size()));
        pts.push_back(upper(i / 6.0));
    }
    L.push_back(U.front());
    for (int i = 1; i < 4; ++i)
    {
        L.push_back(Index(pts.size()));
        pts.push_back(lower(i / 4.0));
    }
    L.push_back(U.back());
    for (Index p : U)
    {
        UO.push_back(Index(pts.size()));
        pts.push_back(pts[p] + glm::dvec3(0, 0, 100));
    }
    for (Index p : L)
    {
        LO.push_back(Index(pts.size()));
        pts.push_back(pts[p] - glm::dvec3(0, 0, 100));
    }
    for (size_t i = 0; i + 1 < U.size(); ++i)
    {
        sizes.push_back(4);
        idx.insert(idx.end(), {U[i], UO[i], UO[i + 1], U[i + 1]});
    }
    for (size_t i = 0; i + 1 < L.size(); ++i)
    {
        sizes.push_back(4);
        idx.insert(idx.end(), {L[i], L[i + 1], LO[i + 1], LO[i]});
    }
    Mesh m = Mesh::fromPolygons(pts, sizes, idx);
    REQUIRE(isValid(m));
    std::vector<Index> sliver;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e) && std::abs(m.position(m.edgePoint(e, 0)).z) < 50
            && std::abs(m.position(m.edgePoint(e, 1)).z) < 50)
            sliver.push_back(e);
    REQUIRE(sliver.size() == 10);

    for (BridgeCurve curve : {BridgeCurve::Linear, BridgeCurve::Blend, BridgeCurve::Auto})
    {
        CAPTURE(int(curve));
        Mesh b = m;
        BridgeOptions o;
        o.curve = curve;
        o.divisions = 3;
        const BridgeResult r = bridge(b, sliver, o);
        CHECK(r.pairsBridged == 1);
        CHECK(isValid(b, kValidateDefault | kValidateLoose));
        // Only the outer rim stays open: no hole at the tips, no point left hanging there.
        CHECK(boundaryEdgeCount(b) == 6 + 4 + 4);
        for (Index p = m.pointCapacity(); p < b.pointCapacity(); ++p)
            if (b.pointValid(p))
            {
                CHECK(glm::distance(b.position(p), b.position(U.front())) > 1e-6);
                CHECK(glm::distance(b.position(p), b.position(U.back())) > 1e-6);
            }
    }
}

TEST_CASE("bridge: loops of different lengths join with quads")
{
    for (auto [a, b, tris] : {std::tuple{12, 16, 0}, std::tuple{12, 18, 0}, std::tuple{12, 13, 1}, std::tuple{8, 16, 0}})
    {
        CAPTURE(a);
        CAPTURE(b);
        Mesh m = twoTubes(a, b);
        BridgeOptions o;
        o.divisions = 3;
        const BridgeResult r = bridge(m, innerRims(m), o);
        CHECK(r.pairsBridged == 1);
        CHECK(isValid(m));
        CHECK(boundaryEdgeCount(m) == Index(a + b));
        CHECK(r.triangles == tris);
        int quads = 0, other = 0;
        for (Index f : r.faces)
            (m.faceSize(f) == 4 ? quads : other) += 1;
        CHECK(other == tris);
    }
    // No rows to spread over: triangles make up the difference.
    Mesh m = twoTubes(12, 16);
    BridgeOptions o;
    o.transition = BridgeTransition::Triangles;
    const BridgeResult r = bridge(m, innerRims(m), o);
    CHECK(isValid(m));
    CHECK(r.triangles == 4);
}

TEST_CASE("bridge: shape options")
{
    const Index base = twoTubes(16, 16).pointCapacity();
    auto run = [&](const BridgeOptions& o) {
        Mesh m = twoTubes(16, 16);
        bridge(m, innerRims(m), o);
        REQUIRE(isValid(m));
        return m;
    };
    BridgeOptions o;
    o.divisions = 1;
    const double r0 = radiusAt(run(o), base, 0.0);
    CHECK(r0 == doctest::Approx(50.0).epsilon(1e-6));

    BridgeOptions taper = o;
    taper.taper = 0.5;
    CHECK(radiusAt(run(taper), base, 0.0) == doctest::Approx(25.0).epsilon(1e-6));

    BridgeOptions bulge = o;
    bulge.profileFactor = 0.5;
    CHECK(radiusAt(run(bulge), base, 0.0) == doctest::Approx(75.0).epsilon(1e-6));

    // Twist turns the middle row half way: 90 degrees at the target is 45 at the middle.
    BridgeOptions twist = o;
    twist.twist = 90.0;
    const Mesh straight = run(o), turned = run(twist);
    const glm::dvec3 a = straight.position(base), b = turned.position(base);
    const double angle = glm::degrees(std::acos(glm::dot(glm::normalize(glm::dvec3(a.x, 0, a.z)), glm::normalize(glm::dvec3(b.x, 0, b.z)))));
    CHECK(angle == doctest::Approx(45.0).epsilon(1e-6));

    // Offset steps the pairing one point round: every column leans by one side's step.
    BridgeOptions offset = o;
    offset.offset = 1;
    Mesh shifted = run(offset);
    const Mesh even = run(o);
    double leanEven = 0.0, leanShifted = 0.0;
    for (Index f = 0; f < even.faceCapacity(); ++f)
        if (even.faceValid(f))
            leanEven = std::max(leanEven, std::abs(even.faceNormal(f).y));
    for (Index f = 0; f < shifted.faceCapacity(); ++f)
        if (shifted.faceValid(f))
            leanShifted = std::max(leanShifted, std::abs(shifted.faceNormal(f).y));
    CHECK(leanShifted > leanEven + 0.01);

    // Blend leaves each border along its tube, so the middle row bulges less than straight only when it should; on
    // coaxial tubes of one radius it stays on the cylinder.
    BridgeOptions blend = o;
    blend.curve = BridgeCurve::Blend;
    CHECK(radiusAt(run(blend), base, 0.0) == doctest::Approx(50.0).epsilon(1e-6));

    // Path: the middle row follows an arc's middle, bowed out along x.
    BridgeOptions path = o;
    path.curve = BridgeCurve::Path;
    for (int i = 0; i <= 16; ++i)
    {
        const double t = i / 16.0;
        path.path.push_back({200.0 * std::sin(std::numbers::pi * t), -50.0 + 100.0 * t, 0.0});
    }
    const Mesh bowed = run(path);
    glm::dvec3 centre(0.0);
    int n = 0;
    for (Index p = base; p < bowed.pointCapacity(); ++p)
        if (bowed.pointValid(p))
        {
            centre += bowed.position(p);
            ++n;
        }
    centre /= double(n);
    CHECK(centre.x > 50.0);
}

TEST_CASE("bridge: merge, smoothing angle, faces")
{
    Mesh m = twoTubes(12, 12);
    const Index before = m.numPoints();
    BridgeOptions o;
    o.merge = true;
    o.mergeFactor = 0.5;
    bridge(m, innerRims(m), o);
    CHECK(isValid(m));
    CHECK(m.numPoints() == before - 12);
    CHECK(boundaryEdgeCount(m) == 24);
    bool atMiddle = true;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && std::abs(m.position(p).y) < 60.0)
            atMiddle = atMiddle && std::abs(m.position(p).y) < 1e-9;
    CHECK(atMiddle);

    // Hard where the strip folds sharper than the angle: a flat gap between two tubes has none.
    Mesh h = twoTubes(16, 16);
    BridgeOptions hard;
    hard.smoothingAngle = 30.0;
    bridge(h, innerRims(h), hard);
    CHECK(hardCount(h) == 0);
    Mesh h2 = twoTubes(16, 16);
    hard.smoothingAngle = 0.0;
    hard.taper = 0.3;
    hard.divisions = 1;
    bridge(h2, innerRims(h2), hard);
    CHECK(hardCount(h2) > 0);

    // Two faces through a block: deleted and joined by a tunnel.
    Mesh cube = makeCube({300, 300, 300, 3, 3, 3});
    std::vector<Index> ends;
    for (Index f = 0; f < cube.faceCapacity(); ++f)
        if (cube.faceValid(f))
        {
            const glm::dvec3 c = cube.faceCentroid(f);
            if (std::abs(c.x) < 1 && std::abs(c.z) < 1 && std::abs(std::abs(c.y) - 150) < 1)
                ends.push_back(f);
        }
    REQUIRE(ends.size() == 2);
    const Index facesBefore = cube.numFaces();
    const BridgeResult r = bridgeFaces(cube, ends, {});
    CHECK(r.faces.size() == 4);
    CHECK(isValid(cube));
    CHECK(boundaryEdgeCount(cube) == 0);
    CHECK(cube.numFaces() == facesBefore + 2);
    CHECK(signedVolume(cube) == doctest::Approx(300.0 * 300.0 * 300.0 - 100.0 * 100.0 * 300.0));
}

TEST_CASE("bridge: chain runs one bridge through several wire rings")
{
    Mesh m;
    for (int ring = 0; ring < 3; ++ring)
    {
        std::vector<Index> ids;
        for (int i = 0; i < 8; ++i)
        {
            const double a = 2.0 * std::numbers::pi * i / 8;
            ids.push_back(m.addPoint({50 * std::cos(a), 100.0 * ring, 50 * std::sin(a)}));
        }
        for (int i = 0; i < 8; ++i)
            m.addEdge(ids[size_t(i)], ids[size_t((i + 1) % 8)]);
    }
    std::vector<Index> all;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        all.push_back(e);
    BridgeOptions o;
    o.connect = BridgeConnect::Chain;
    const BridgeResult r = bridge(m, all, o);
    CHECK(r.pairsBridged == 2);
    CHECK(r.faces.size() == 16);
    CHECK(isValid(m));
    CHECK(boundaryEdgeCount(m) == 16);
}

TEST_CASE("bridge: explicit start points pair as asked")
{
    Mesh m = twoTubes(12, 12);
    const std::vector<Index> rims = innerRims(m);
    // The lowest-id point on each inner rim.
    Index a = kInvalid, b = kInvalid;
    for (Index e : rims)
        for (int i = 0; i < 2; ++i)
        {
            const Index p = m.edgePoint(e, i);
            Index& slot = m.position(p).y < 0.0 ? a : b;
            slot = std::min(slot == kInvalid ? p : slot, p);
        }
    // Point b's neighbour three steps round, so the pairing has to turn.
    Index target = b;
    for (int step = 0; step < 3; ++step)
    {
        Index next = kInvalid;
        m.forEachPointEdge(target, [&](Index e) {
            const Index o = m.edgeOtherPoint(e, target);
            if (next == kInvalid && m.isBoundaryEdge(e) && o > target && std::abs(m.position(o).y - m.position(b).y) < 1e-6)
                next = o;
        });
        if (next != kInvalid)
            target = next;
    }
    REQUIRE(target != b);
    BridgeOptions o;
    o.pairs = {{a, target}};
    bridge(m, rims, o);
    CHECK(isValid(m));
    CHECK(m.findEdge(a, target) != kInvalid);
}

TEST_CASE("bridge: with mirror symmetry a border never pairs with its mirror image")
{
    // A gap down the middle: its two sides face each other across x = 0.
    Gap g = cut(makePlane({400.0, 100.0, 8, 2}), [](const glm::dvec3& c) { return std::abs(c.x) < 50.0; },
                [](const glm::dvec3& mid) { return std::abs(std::abs(mid.x) - 50.0) < 1e-6; });
    REQUIRE(g.border.size() == 4);

    Mesh plain = g.mesh;
    BridgeOptions o = autoOptions();
    CHECK(bridge(plain, g.border, o).pairsBridged == 1);

    const SymmetryMap map = SymmetryMap::build(g.mesh, axisPlane(0), 1e-3);
    o.symmetry = &map;
    const BridgeResult r = bridge(g.mesh, g.border, o);
    for (Index f : r.faces)
    {
        bool neg = false, pos = false;
        g.mesh.forEachFaceCorner(f, [&](Index c) {
            const double x = g.mesh.position(g.mesh.cornerPoint(c)).x;
            neg = neg || x < -1e-6;
            pos = pos || x > 1e-6;
        });
        CHECK_FALSE((neg && pos));
    }
}
