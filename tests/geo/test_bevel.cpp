#include "TestUtil.h"

#include "geo/FoldEdges.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "geo/Triangulate.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/component_wise.hpp>

#include <cmath>
#include <limits>
#include <set>

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
Index edgeBetween(const Mesh& m, const glm::dvec3& a, const glm::dvec3& b)
{
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            const glm::dvec3 p = m.position(m.edgePoint(e, 0)), q = m.position(m.edgePoint(e, 1));
            if ((glm::distance(p, a) < 1e-6 && glm::distance(q, b) < 1e-6)
                || (glm::distance(p, b) < 1e-6 && glm::distance(q, a) < 1e-6))
                return e;
        }
    return kInvalid;
}

std::vector<Index> allEdges(const Mesh& m)
{
    std::vector<Index> out;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
            out.push_back(e);
    return out;
}

/** Segment pq crosses the triangle's interior, ignoring touches within tol of its border or the segment ends. */
bool segmentHitsTriangle(const glm::dvec3& p, const glm::dvec3& q, const glm::dvec3& a, const glm::dvec3& b,
                         const glm::dvec3& c, double tol)
{
    const glm::dvec3 d = q - p, e1 = b - a, e2 = c - a;
    const glm::dvec3 h = glm::cross(d, e2);
    const double det = glm::dot(e1, h);
    if (std::abs(det) < 1e-12 * glm::dot(d, d) * glm::dot(e1, e1))
        return false;
    const double inv = 1.0 / det;
    const glm::dvec3 s = p - a;
    const double u = glm::dot(s, h) * inv;
    const glm::dvec3 qv = glm::cross(s, e1);
    const double v = glm::dot(d, qv) * inv;
    const double t = glm::dot(e2, qv) * inv;
    return u > tol && v > tol && u + v < 1.0 - tol && t > tol && t < 1.0 - tol;
}

/** Pairs of triangles that pierce each other, skipping pairs that share an edge. Shared-point pairs test their far sides. */
int selfIntersections(const Mesh& m)
{
    const Triangulation tri = triangulate(m);
    const Index n = tri.triangleCount();
    std::vector<std::array<Index, 3>> pts(n);
    std::vector<Bounds> box(n);
    double size = 0.0;
    for (Index t = 0; t < n; ++t)
    {
        for (int k = 0; k < 3; ++k)
        {
            pts[t][k] = m.cornerPoint(tri.corners[3 * t + k]);
            box[t].extend(m.position(pts[t][k]));
        }
        size = std::max(size, glm::length(box[t].size()));
    }
    const double pad = 1e-6 * size;
    int hits = 0;
    for (Index i = 0; i < n; ++i)
        for (Index j = i + 1; j < n; ++j)
        {
            if (glm::any(glm::greaterThan(box[i].min, box[j].max + pad)) || glm::any(glm::greaterThan(box[j].min, box[i].max + pad)))
                continue;
            int shared = 0;
            for (Index a : pts[i])
                for (Index b : pts[j])
                    shared += a == b;
            if (shared >= 2)
                continue;
            auto test = [&](Index ti, Index tj) {
                const auto& A = pts[ti];
                const auto& B = pts[tj];
                for (int k = 0; k < 3; ++k)
                {
                    const Index p = A[k], q = A[(k + 1) % 3];
                    if (std::find(B.begin(), B.end(), p) != B.end() || std::find(B.begin(), B.end(), q) != B.end())
                        continue;
                    if (segmentHitsTriangle(m.position(p), m.position(q), m.position(B[0]), m.position(B[1]), m.position(B[2]), 1e-7))
                        return true;
                }
                return false;
            };
            if (test(i, j) || test(j, i))
            {
                if (hits < 5)
                    MESSAGE("triangles of faces " << tri.faces[i] << " and " << tri.faces[j] << " intersect");
                ++hits;
            }
        }
    return hits;
}

/** Largest turn between neighbouring face normals, degrees. A face folded back over its neighbour shows near 180. */
double maxFold(const Mesh& m)
{
    double worst = 0.0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isManifoldEdge(e))
        {
            const Index c = m.edgeCorner(e);
            const double d = glm::dot(m.faceNormal(m.cornerFace(c)), m.faceNormal(m.cornerFace(m.cornerRadial(c))));
            worst = std::max(worst, glm::degrees(std::acos(std::clamp(d, -1.0, 1.0))));
        }
    return worst;
}

Index smallFaces(const Mesh& m, double minArea)
{
    Index n = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceArea(f) < minArea)
        {
            if (n < 5)
                MESSAGE("face " << f << " of " << m.faceSize(f) << " corners has area " << m.faceArea(f));
            ++n;
        }
    return n;
}

/** Every point lies behind every other face's plane: the solid is convex and all faces point outward. */
bool convexOutward(const Mesh& m, double tol)
{
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const glm::dvec3 n = m.faceNormal(f), c = m.faceCentroid(f);
        const std::vector<Index> own = m.facePoints(f);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && glm::dot(m.position(p) - c, n) > tol && std::find(own.begin(), own.end(), p) == own.end())
            {
                MESSAGE("point " << p << " is " << glm::dot(m.position(p) - c, n) << " in front of face " << f << " ("
                                 << m.faceSize(f) << " corners)");
                return false;
            }
    }
    return true;
}

bool uvsFinite(const Mesh& m)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    for (Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c) && (!std::isfinite(uv[c].x) || !std::isfinite(uv[c].y)))
            return false;
    return true;
}

/** The checks every bevel result must pass: valid, manifold, consistently wound, no tiny faces, no overlaps. */
void checkSound(const Mesh& m, double offset, bool closed = true)
{
    CHECK(isValid(m, kValidateDefault | kValidateDegenerate));
    if (closed)
    {
        CHECK(boundaryEdgeCount(m) == 0);
        CHECK(signedVolume(m) > 0.0);
    }
    CHECK(smallFaces(m, 1e-4 * offset * offset) == 0);
    CHECK(selfIntersections(m) == 0);
    CHECK(maxFold(m) < 135.0);
}

void checkClosedSound(const Mesh& m, double offset)
{
    checkSound(m, offset);
    CHECK(euler(m) == 2);
}

/** Faces a corner of three bevelled edges turns into: Blender's quad grid, with a centre triangle for odd counts. */
Index cornerFaces(int seg)
{
    const Index h = Index(seg / 2);
    return seg % 2 ? 3 * h * (h + 1) + 1 : 3 * h * h;
}

Mesh lBracket()
{
    const glm::dvec2 profile[6] = {{0, 0}, {10, 0}, {10, 1}, {1, 1}, {1, 10}, {0, 10}};
    std::vector<glm::dvec3> pts;
    for (double z : {0.0, 10.0})
        for (const glm::dvec2& p : profile)
            pts.push_back({p.x, p.y, z});
    std::vector<Index> sizes, idx;
    sizes.push_back(6);
    for (Index i = 6; i-- > 0;)
        idx.push_back(i);
    sizes.push_back(6);
    for (Index i = 0; i < 6; ++i)
        idx.push_back(6 + i);
    for (Index i = 0; i < 6; ++i)
    {
        const Index j = (i + 1) % 6;
        sizes.push_back(4);
        for (Index k : {i, j, 6 + j, 6 + i})
            idx.push_back(k);
    }
    return Mesh::fromPolygons(pts, sizes, idx);
}

/** Ground grid 7 x 4 with two 2 x 2 blocks raised 1, one quad apart, as in the auto-bevel tests. */
Mesh raisedBlocks()
{
    const int W = 7, H = 4;
    std::vector<glm::dvec3> pts;
    for (int y = 0; y <= H; ++y)
        for (int x = 0; x <= W; ++x)
            pts.push_back({double(x), double(y), 0.0});
    std::vector<Index> sizes, idx;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            sizes.push_back(4);
            for (int k : {y * (W + 1) + x, y * (W + 1) + x + 1, (y + 1) * (W + 1) + x + 1, (y + 1) * (W + 1) + x})
                idx.push_back(Index(k));
        }
    Mesh m = Mesh::fromPolygons(pts, sizes, idx);
    std::vector<Index> blocks;
    for (int y : {1, 2})
        for (int x : {1, 2, 4, 5})
            blocks.push_back(Index(y * W + x));
    extrudeFaces(m, blocks, {.offset = 1.0});
    m.compact();
    return m;
}

std::vector<Mesh> randomShapes()
{
    std::vector<Mesh> out = {makeCube({.subdivX = 3, .subdivY = 2, .subdivZ = 2}), makeCylinder({.subdivAxis = 12, .subdivHeight = 2}),
                             makeCone({.subdivAxis = 10}), makeSphere({.subdivAxis = 10, .subdivHeight = 8}),
                             makeTorus({.subdivAxis = 12, .subdivHeight = 8}), makeIcoSphere({.subdivisions = 2}), lBracket(), raisedBlocks(),
                             makeCylinder({.subdivAxis = 6, .subdivCaps = 2})};
    Mesh tri = makeCube({.subdivX = 2, .subdivY = 2, .subdivZ = 2});
    triangulateFaces(tri, std::vector<Index>{});
    std::vector<Index> all;
    for (Index f = 0; f < tri.faceCapacity(); ++f)
        if (tri.faceValid(f))
            all.push_back(f);
    triangulateFaces(tri, all);
    tri.compact();
    out.push_back(std::move(tri));
    return out;
}

const glm::dvec3 kCorner(50.0, 50.0, 50.0);
} // namespace

TEST_CASE("bevel: cube, every edge, 1 to 4 segments")
{
    for (int seg = 1; seg <= 4; ++seg)
    {
        CAPTURE(seg);
        Mesh m = makeCube();
        BevelOptions bo;
        bo.offset = 10.0;
        bo.segments = seg;
        const std::vector<Index> made = bevelEdges(m, allEdges(m), bo);
        checkClosedSound(m, bo.offset);
        CHECK(m.numFaces() == 6 + 12 * Index(seg) + 8 * cornerFaces(seg));
        CHECK(made.size() == 12 * size_t(seg) + 8 * cornerFaces(seg));
        CHECK(convexOutward(m, 1e-6));
        CHECK(uvsFinite(m));
        if (seg >= 2)
        {
            // Round profile: strips lie on cylinders and corners on spheres of radius offset.
            for (Index p = 0; p < m.pointCapacity(); ++p)
            {
                if (!m.pointValid(p))
                    continue;
                const glm::dvec3 q = m.position(p);
                const glm::dvec3 inner = glm::clamp(q, glm::dvec3(-40.0), glm::dvec3(40.0));
                CHECK(glm::distance(q, inner) == doctest::Approx(10.0).epsilon(1e-9));
            }
            // All quads, plus one centre triangle per corner for odd counts.
            Index tris = 0;
            for (Index f = 0; f < m.faceCapacity(); ++f)
                if (m.faceValid(f))
                    tris += m.faceSize(f) == 3;
            CHECK(tris == (seg % 2 ? 8 : 0));
        }
    }
}

TEST_CASE("bevel: profiles stay consistent across the corners")
{
    for (double profile : {0.25, 0.7, 0.95})
        for (int seg : {2, 3})
        {
            CAPTURE(profile);
            CAPTURE(seg);
            Mesh m = makeCube();
            BevelOptions bo;
            bo.offset = 10.0;
            bo.segments = seg;
            bo.profile = profile;
            bevelEdges(m, allEdges(m), bo);
            checkClosedSound(m, bo.offset);
            // Near-square profiles bend corner quads sharply, so neighbours may sit a little above their mean plane.
            CHECK(convexOutward(m, profile > 0.9 ? 0.02 * bo.offset : 1e-6));
            if (profile == 0.25)
                // A straight profile chamfers each corner with a flat triangle: every corner point on x + y + z = 130.
                for (Index p = 0; p < m.pointCapacity(); ++p)
                {
                    const glm::dvec3 q = m.position(p);
                    if (m.pointValid(p) && q.x > 40.0 && q.y > 40.0 && q.z > 40.0)
                        CHECK(q.x + q.y + q.z == doctest::Approx(130.0));
                }
        }
}

TEST_CASE("bevel: three edges at one corner")
{
    for (int seg = 1; seg <= 4; ++seg)
    {
        CAPTURE(seg);
        Mesh m = makeCube();
        const std::vector<Index> edges = {edgeBetween(m, kCorner, {50, 50, -50}), edgeBetween(m, kCorner, {-50, 50, 50}),
                                          edgeBetween(m, kCorner, {50, -50, 50})};
        BevelOptions bo;
        bo.offset = 10.0;
        bo.segments = seg;
        bevelEdges(m, edges, bo);
        checkClosedSound(m, bo.offset);
        CHECK(m.numFaces() == 6 + 3 * Index(seg) + cornerFaces(seg));
        CHECK(convexOutward(m, 1e-6));
        // The far ends stop on the neighbouring cube corners, which stay where they were.
        for (const glm::dvec3& c : {glm::dvec3(50, 50, -50), glm::dvec3(-50, 50, 50), glm::dvec3(50, -50, 50)})
        {
            bool found = false;
            for (Index p = 0; p < m.pointCapacity(); ++p)
                found |= m.pointValid(p) && glm::distance(m.position(p), c) < 1e-9;
            CHECK_FALSE(found);
        }
    }
}

TEST_CASE("bevel: two edges meeting at a corner miter cleanly")
{
    for (int seg = 1; seg <= 4; ++seg)
    {
        CAPTURE(seg);
        Mesh m = makeCube();
        const std::vector<Index> edges = {edgeBetween(m, kCorner, {50, 50, -50}), edgeBetween(m, kCorner, {-50, 50, 50})};
        BevelOptions bo;
        bo.offset = 10.0;
        bo.segments = seg;
        bevelEdges(m, edges, bo);
        checkClosedSound(m, bo.offset);
        // The two rounded strips meet along their miter: no patch between them.
        CHECK(m.numFaces() == 6 + 2 * Index(seg));
        CHECK(convexOutward(m, 1e-6));
        // Exact round fillets: every point of the strips sits on its cylinder.
        for (Index p = 0; p < m.pointCapacity(); ++p)
        {
            const glm::dvec3 q = m.position(p);
            if (!m.pointValid(p) || q.y <= 40.0 + 1e-9)
                continue;
            if (q.x > 40.0 && q.z > 40.0)
                CHECK(std::min(std::hypot(q.x - 40.0, q.y - 40.0), std::hypot(q.y - 40.0, q.z - 40.0)) == doctest::Approx(10.0));
        }
    }
}

TEST_CASE("bevel: single edge ending at cube corners")
{
    for (int seg = 1; seg <= 4; ++seg)
    {
        CAPTURE(seg);
        Mesh m = makeCube();
        BevelOptions bo;
        bo.offset = 10.0;
        bo.segments = seg;
        const std::vector<Index> made = bevelEdges(m, std::vector<Index>{edgeBetween(m, kCorner, {50, 50, -50})}, bo);
        checkClosedSound(m, bo.offset);
        CHECK(made.size() == size_t(seg));
        CHECK(m.numFaces() == 6 + Index(seg));
        CHECK(convexOutward(m, 1e-6));
        // The end caps take the profile and stay flat.
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f) && std::abs(m.faceNormal(f).z) > 0.999)
                m.forEachFaceCorner(f, [&](Index c) { CHECK(std::abs(m.position(m.cornerPoint(c)).z) == doctest::Approx(50.0)); });
    }
}

TEST_CASE("bevel: terminal and L corners on valence four points")
{
    // A 2x2x2 cube: edge midpoints have four edges, face centres four flat ones.
    for (int seg = 1; seg <= 4; ++seg)
    {
        CAPTURE(seg);
        BevelOptions bo;
        bo.offset = 8.0;
        bo.segments = seg;

        // One edge from a face centre to an edge midpoint, in the flat top face.
        Mesh one = makeCube({.subdivX = 2, .subdivY = 2, .subdivZ = 2});
        bevelEdges(one, std::vector<Index>{edgeBetween(one, {0, 50, 0}, {50, 50, 0})}, bo);
        checkClosedSound(one, bo.offset);
        CHECK(convexOutward(one, 1e-6));

        // Two edges making an L in the flat top face.
        Mesh ell = makeCube({.subdivX = 2, .subdivY = 2, .subdivZ = 2});
        const std::vector<Index> l = {edgeBetween(ell, {0, 50, 0}, {50, 50, 0}), edgeBetween(ell, {0, 50, 0}, {0, 50, 50})};
        bevelEdges(ell, l, bo);
        checkClosedSound(ell, bo.offset);
        CHECK(convexOutward(ell, 1e-6));
        // Everything that was in the top face stays in it.
        for (Index p = 0; p < ell.pointCapacity(); ++p)
            if (ell.pointValid(p) && ell.position(p).y > 49.0)
                CHECK(ell.position(p).y == doctest::Approx(50.0));

        // A whole cube edge, both halves, with a T: one top-face edge leaves its midpoint.
        Mesh tee = makeCube({.subdivX = 2, .subdivY = 2, .subdivZ = 2});
        const std::vector<Index> t = {edgeBetween(tee, {50, 50, 50}, {50, 50, 0}), edgeBetween(tee, {50, 50, 0}, {50, 50, -50}),
                                      edgeBetween(tee, {50, 50, 0}, {0, 50, 0})};
        bevelEdges(tee, t, bo);
        checkClosedSound(tee, bo.offset);
        // The long edge stays round through the junction: everything past the fillet's axis lies on its cylinder.
        for (Index p = 0; p < tee.pointCapacity(); ++p)
        {
            const glm::dvec3 q = tee.position(p);
            CHECK(glm::all(glm::lessThanEqual(glm::abs(q), glm::dvec3(50.0 + 1e-9))));
            if (tee.pointValid(p) && q.x > 42.0 + 1e-9 && q.y > 42.0 + 1e-9)
                CHECK(std::hypot(q.x - 42.0, q.y - 42.0) == doctest::Approx(8.0));
        }

        // Every edge: valence four points get four-sided corner grids.
        Mesh all = makeCube({.subdivX = 2, .subdivY = 2, .subdivZ = 2});
        bevelEdges(all, allEdges(all), bo);
        checkClosedSound(all, bo.offset);
        CHECK(convexOutward(all, 0.01 * bo.offset));
    }
}

TEST_CASE("bevel: L bracket folds, concave edges and reflex corners")
{
    for (int seg = 1; seg <= 4; ++seg)
    {
        CAPTURE(seg);
        // The outer and inner fold.
        Mesh m = lBracket();
        const std::vector<Index> folds = {edgeBetween(m, {0, 0, 0}, {0, 0, 10}), edgeBetween(m, {1, 1, 0}, {1, 1, 10})};
        const double before = signedVolume(m);
        bevelEdges(m, folds, {.offset = 0.3, .segments = seg});
        checkClosedSound(m, 0.3);
        CHECK(m.numFaces() == 8 + 2 * Index(seg));
        // The outer fold loses what the inner gains when both are round.
        if (seg >= 3)
            CHECK(signedVolume(m) == doctest::Approx(before).epsilon(1e-3));
        // Caps stay flat.
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
            {
                const double z = m.position(p).z;
                CHECK((std::abs(z) < 1e-9 || std::abs(z - 10.0) < 1e-9));
            }

        // Every edge: concave folds, reflex cap corners and three-edge corners mixing both.
        Mesh all = lBracket();
        bevelEdges(all, allEdges(all), {.offset = 0.3, .segments = seg});
        checkClosedSound(all, 0.3);
    }
}

TEST_CASE("bevel: auto-bevel fold corners on raised blocks")
{
    for (int seg = 1; seg <= 3; ++seg)
    {
        CAPTURE(seg);
        Mesh m = raisedBlocks();
        FoldEdgeOptions o;
        o.completeCorners = true;
        const std::vector<Index> folds = foldEdges(m, o);
        REQUIRE(folds.size() == 42);
        bevelEdges(m, folds, {.offset = 0.2, .segments = seg});
        checkSound(m, 0.2, false);
        // The open border is untouched.
        CHECK(boundaryEdgeCount(m) == 22);

        Mesh plain = raisedBlocks();
        o.completeCorners = false;
        bevelEdges(plain, foldEdges(plain, o), {.offset = 0.2, .segments = seg});
        checkSound(plain, 0.2, false);
    }
}

TEST_CASE("bevel: cylinder rim")
{
    for (int seg = 1; seg <= 4; ++seg)
    {
        CAPTURE(seg);
        Mesh m = makeCylinder({.radius = 50.0, .height = 100.0, .subdivAxis = 20, .capFill = CapFill::Ngon});
        const Bounds b = m.bounds();
        std::vector<Index> rim;
        for (Index e : allEdges(m))
        {
            const glm::dvec3 p = m.position(m.edgePoint(e, 0)), q = m.position(m.edgePoint(e, 1));
            if (std::abs(p.y - b.max.y) < 1e-9 && std::abs(q.y - b.max.y) < 1e-9)
                rim.push_back(e);
        }
        REQUIRE(rim.size() == 20);
        const Index before = m.numFaces();
        bevelEdges(m, rim, {.offset = 5.0, .segments = seg});
        checkClosedSound(m, 5.0);
        CHECK(m.numFaces() == before + 20 * Index(seg));
        CHECK(convexOutward(m, 1e-6));

        // Every edge: n-gon caps meet the sides at corners that are not square.
        Mesh all = makeCylinder({.radius = 50.0, .height = 100.0, .subdivAxis = 20, .capFill = CapFill::Ngon});
        bevelEdges(all, allEdges(all), {.offset = 5.0, .segments = seg});
        checkClosedSound(all, 5.0);
        CHECK(convexOutward(all, 0.02 * 5.0));
    }
}

TEST_CASE("bevel: pocket with concave three-edge corners")
{
    for (int seg = 1; seg <= 4; ++seg)
    {
        CAPTURE(seg);
        Mesh m = makeCube();
        Index top = kInvalid;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f) && m.faceNormal(f).y > 0.999)
                top = f;
        insetFaces(m, std::vector<Index>{top}, {.thickness = 20.0, .depth = -30.0});
        m.compact();
        REQUIRE(isValid(m));
        const double before = signedVolume(m);
        bevelEdges(m, allEdges(m), {.offset = 5.0, .segments = seg});
        checkClosedSound(m, 5.0);
        // Rounding the pocket's inside corners adds material, rounding the outside takes it away.
        CHECK(signedVolume(m) < before);
    }
}

TEST_CASE("bevel: clamp keeps neighbours from crossing")
{
    for (int seg : {1, 3})
    {
        CAPTURE(seg);
        Mesh m = makeCube({.width = 100.0, .height = 20.0, .depth = 100.0});
        BevelOptions bo;
        bo.offset = 40.0;
        bo.segments = seg;
        bevelEdges(m, allEdges(m), bo);
        checkClosedSound(m, 1.0);
        CHECK(convexOutward(m, 1e-6));
        CHECK(m.bounds().size().y == doctest::Approx(20.0));

        Mesh l = lBracket();
        bevelEdges(l, allEdges(l), {.offset = 5.0, .segments = seg});
        checkClosedSound(l, 0.05);
    }
}

TEST_CASE("bevel: edges between faces folded flat onto each other")
{
    // An unclamped inset of a 4 wide face by 7 crosses its rim over itself: the short rim faces become bowties lying
    // back to back with the long ones.
    Mesh m = makeCube({.width = 100.0, .height = 100.0, .depth = 4.0});
    Index side = kInvalid;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceNormal(f).x > 0.5)
            side = f;
    REQUIRE(side != kInvalid);
    const ExtrudeResult inset = insetFaces(m, std::span(&side, 1), {.thickness = 7.0, .individual = true, .clamp = false});
    extrudeFaces(m, inset.faces, {.offset = -2.5});
    REQUIRE(maxFold(m) > 179.9);

    BevelOptions bo;
    bo.offset = 1.2;
    bevelEdges(m, allEdges(m), bo);
    CHECK(isValid(m));
    CHECK(boundaryEdgeCount(m) == 0);
    // Heights under 0.01 over the longest edge, as meshCheck reports zero-area faces.
    Index slivers = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            double longest = 0.0;
            m.forEachFaceCorner(f, [&](Index c) { longest = std::max(longest, m.edgeLength(m.cornerEdge(c))); });
            if (m.faceArea(f) <= 0.01 * longest)
            {
                MESSAGE("face " << f << " of " << m.faceSize(f) << " corners has area " << m.faceArea(f));
                ++slivers;
            }
        }
    CHECK(slivers == 0);
}

TEST_CASE("bevel: clamp closes faces that run out of room")
{
    // Shortest edge over the mesh size, so slivers show however small they are.
    auto shortest = [](const Mesh& m) {
        double s = 1e300;
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e))
                s = std::min(s, m.edgeLength(e));
        return s / glm::length(m.bounds().size());
    };
    for (int seg : {1, 3})
    {
        CAPTURE(seg);
        // The 20 high sides close to a line where the top and bottom bevels meet.
        Mesh m = makeCube({.width = 100.0, .height = 20.0, .depth = 100.0});
        double applied = 0.0;
        bevelEdges(m, allEdges(m), {.offset = 40.0, .segments = seg, .appliedOffset = &applied});
        CHECK(applied == doctest::Approx(10.0));
        checkClosedSound(m, 10.0);
        CHECK(shortest(m) > 1e-3);
        CHECK(convexOutward(m, 1e-6));

        // A narrow side inset to its medial line leaves triangles too small for the offset: they close to points.
        Mesh w = makeCube({.width = 100.0, .height = 100.0, .depth = 2.5});
        std::vector<Index> sel;
        for (Index f = 0; f < w.faceCapacity(); ++f)
            if (w.faceValid(f) && std::abs(w.faceNormal(f).x) > 0.5)
                sel.push_back(f);
        insetFaces(w, sel, {.thickness = 7.0, .individual = true});
        bevelEdges(w, allEdges(w), {.offset = 1.2, .segments = seg});
        checkClosedSound(w, 0.1);
        CHECK(shortest(w) > 1e-4);
    }

    // Sides that taper close only where they are narrowest; elsewhere their bevels meet a hair apart, and must still weld
    // rather than leave a strip of no width that shades as a hard edge.
    for (int seg : {1, 2})
    {
        CAPTURE(seg);
        Mesh m = makeCube({.width = 100.0, .height = 20.0, .depth = 100.0});
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && m.position(p).y > 0.0)
                m.setPosition(p, m.position(p) + glm::dvec3(0.0, 0.0002 * (m.position(p).x + 50.0), 0.0));
        bevelEdges(m, allEdges(m), {.offset = 40.0, .segments = seg});
        checkClosedSound(m, 10.0);
        CHECK(shortest(m) > 1e-3);
    }

    // With room to spare the clamp changes nothing.
    Mesh a = makeCube(), b = makeCube();
    bevelEdges(a, allEdges(a), {.offset = 10.0, .segments = 2});
    bevelEdges(b, allEdges(b), {.offset = 10.0, .segments = 2, .clamp = false});
    REQUIRE(a.pointCapacity() == b.pointCapacity());
    for (Index p = 0; p < a.pointCapacity(); ++p)
        if (a.pointValid(p))
            CHECK(glm::distance(a.position(p), b.position(p)) < 1e-12);
}

TEST_CASE("bevel: clamp reports which edges hold the offset back")
{
    // A narrow chamfer on one top edge: its two edges limit a wide bevel of every edge, edges far from it don't.
    Mesh m = makeCube({.width = 100.0, .height = 100.0, .depth = 100.0});
    const Index top = edgeBetween(m, {-50.0, 50.0, 50.0}, {50.0, 50.0, 50.0});
    REQUIRE(top != kInvalid);
    bevelEdges(m, std::vector<Index>{top}, {.offset = 1.0});
    std::vector<double> limits;
    double applied = 0.0;
    const Mesh before = m;
    bevelEdges(m, allEdges(before), {.offset = 20.0, .appliedOffset = &applied, .edgeLimits = &limits});
    REQUIRE(limits.size() == before.edgeCapacity());
    double least = std::numeric_limits<double>::infinity();
    for (Index e = 0; e < before.edgeCapacity(); ++e)
        if (before.edgeValid(e))
            least = std::min(least, limits[e]);
    CHECK(applied < 1.0);
    CHECK(least == doctest::Approx(applied).epsilon(0.05));
    const Index far = edgeBetween(before, {-50.0, -50.0, -50.0}, {50.0, -50.0, -50.0});
    REQUIRE(far != kInvalid);
    CHECK(limits[far] > 20.0);
}

TEST_CASE("bevel: symmetry")
{
    for (int seg = 1; seg <= 3; ++seg)
    {
        CAPTURE(seg);
        Mesh m = makeCube({.subdivX = 2});
        SymmetryMap map = SymmetryMap::build(m);
        REQUIRE(map.unmatchedCount() == 0);
        // Three edges at one +x corner and the top edge crossing the plane.
        const std::vector<Index> edges = {edgeBetween(m, kCorner, {50, 50, -50}), edgeBetween(m, kCorner, {0, 50, 50}),
                                          edgeBetween(m, kCorner, {50, -50, 50})};
        BevelOptions bo;
        bo.offset = 10.0;
        bo.segments = seg;
        bo.symmetry = &map;
        bevelEdges(m, edges, bo);
        checkClosedSound(m, bo.offset);
        CHECK(convexOutward(m, 1e-6));
        const SymmetryMap after = SymmetryMap::build(m, {}, 1e-6);
        CHECK(after.unmatchedCount() == 0);
    }
}


TEST_CASE("bevel: random edge sets on assorted meshes stay sound")
{
    const std::vector<Mesh> shapes = randomShapes();
    uint32_t seed = 12345;
    auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    int shape = 0;
    for (const Mesh& base : shapes)
    {
        CAPTURE(shape);
        const double scale = glm::length(base.bounds().size());
        for (int trial = 0; trial < 40; ++trial)
        {
            CAPTURE(trial);
            Mesh m = base;
            std::vector<Index> edges;
            for (Index e : allEdges(m))
                if (next() % 3 == 0)
                    edges.push_back(e);
            const int seg = 1 + int(next() % 4);
            CAPTURE(seg);
            bevelEdges(m, edges, {.offset = scale * (next() % 2 ? 0.02 : 0.06), .segments = seg, .profile = (next() % 2) ? 0.5 : 0.3});
            CHECK(isValid(m, kValidateDefault | kValidateDegenerate));
            CHECK(euler(m) == euler(base));
            CHECK(selfIntersections(m) == 0);
            CHECK(maxFold(m) < 150.0);
        }
        ++shape;
    }
}


TEST_CASE("bevel: symmetry, every face border of a split cube")
{
    // Borders meeting the plane make T junctions on it, whose corner grids must come out mirror images.
    for (int seg = 1; seg <= 3; ++seg)
        for (Index f0 = 0; f0 < 10; ++f0)
        {
            CAPTURE(seg);
            CAPTURE(f0);
            Mesh m = makeCube({.subdivX = 2});
            SymmetryMap map = SymmetryMap::build(m);
            std::vector<Index> edges;
            m.forEachFaceCorner(f0, [&](Index c) { edges.push_back(m.cornerEdge(c)); });
            BevelOptions bo;
            bo.offset = 5.0;
            bo.segments = seg;
            bo.symmetry = &map;
            bevelEdges(m, edges, bo);
            checkClosedSound(m, bo.offset);
            CHECK(SymmetryMap::build(m, {}, 1e-6).unmatchedCount() == 0);
        }
}
