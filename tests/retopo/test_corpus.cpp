#include "Corpus.h"

#include "geo/Reduce.h"
#include "geo/Triangulate.h"
#include "retopo/Retopo.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>

using namespace lila;
using namespace lila::corpus;

namespace
{
void dump(const Mesh& m, const std::string& name)
{
    if (std::getenv("LILA_RETOPO_DUMP"))
        geo::writeObjFile((std::filesystem::temp_directory_path() / (name + ".obj")).string(), m);
}

std::multiset<std::array<Index, 3>> pointTriples(const Mesh& m, std::span<const Index> corners)
{
    std::multiset<std::array<Index, 3>> out;
    for (size_t t = 0; t + 2 < corners.size(); t += 3)
    {
        std::array<Index, 3> a = {m.cornerPoint(corners[t]), m.cornerPoint(corners[t + 1]), m.cornerPoint(corners[t + 2])};
        std::rotate(a.begin(), std::min_element(a.begin(), a.end()), a.end());
        out.insert(a);
    }
    return out;
}

/** A quad folded over itself in projection: either diagonal leaves one triangle facing backwards, so no split is right. */
bool isBowtie(const Mesh& m, Index f)
{
    if (m.faceSize(f) != 4)
        return false;
    const glm::dvec3 n = m.faceAreaVector(f);
    const std::vector<Index> p = m.facePoints(f);
    const auto tri = [&](int a, int b, int c) {
        return glm::dot(glm::cross(m.position(p[b]) - m.position(p[a]), m.position(p[c]) - m.position(p[a])), n) > 0.0;
    };
    return !(tri(0, 1, 2) && tri(0, 2, 3)) && !(tri(1, 2, 3) && tri(1, 3, 0));
}

/** Farthest output border point from the source's border edges. */
double offBorder(const Mesh& out, const Mesh& src)
{
    std::vector<std::array<glm::dvec3, 2>> segs;
    for (Index e : allEdges(src))
        if (src.edgeFaceCount(e) == 1)
            segs.push_back({src.position(src.edgePoint(e, 0)), src.position(src.edgePoint(e, 1))});
    double worst = 0.0;
    for (Index p = 0; p < out.pointCapacity(); ++p)
    {
        if (!out.pointValid(p) || !out.isBoundaryPoint(p) || segs.empty())
            continue;
        double best = 1e300;
        for (const auto& [a, b] : segs)
        {
            const glm::dvec3 ab = b - a;
            const double t = glm::dot(ab, ab) > 0.0 ? std::clamp(glm::dot(out.position(p) - a, ab) / glm::dot(ab, ab), 0.0, 1.0) : 0.0;
            best = std::min(best, glm::distance(out.position(p), a + ab * t));
        }
        worst = std::max(worst, best);
    }
    return worst;
}

std::vector<std::string> checkTriangulate(const Mesh& in, const geo::TriangulateOptions& o)
{
    std::vector<std::string> p;
    Mesh m = in;
    std::span<int32_t> src = m.faceAttrs().getOrAdd<int32_t>("corpusSource");
    std::map<Index, glm::dvec3> newell;
    std::map<Index, Index> sizes;
    Index expected = 0;
    std::set<Index> bowties;
    for (Index f : allFaces(m))
    {
        src[f] = int32_t(f);
        if (isBowtie(m, f))
            bowties.insert(f);
        newell[f] = m.faceAreaVector(f);
        sizes[f] = m.faceSize(f);
        expected += m.faceSize(f) - 2;
    }
    const std::multiset<std::array<Index, 3>> shown = pointTriples(m, geo::triangulate(m, o).corners);
    const Index points = m.numPoints(), border = boundaryEdges(m);
    geo::triangulateFaces(m, allFaces(m), o);
    m.compact();
    p = introduced(in, m);
    if (m.numFaces() != expected)
        p.push_back("triangles " + std::to_string(m.numFaces()) + " expected " + std::to_string(expected));
    if (m.numPoints() != points)
        p.push_back("points changed");
    if (boundaryEdges(m) != border)
        p.push_back("border edges " + std::to_string(border) + " -> " + std::to_string(boundaryEdges(m)));
    std::vector<Index> corners;
    for (Index f : allFaces(m))
        m.forEachFaceCorner(f, [&](Index c) { corners.push_back(c); });
    if (pointTriples(m, corners) != shown)
        p.push_back("differs from the displayed triangulation");

    std::span<const int32_t> nsrc = std::as_const(m).faceAttrs().get<int32_t>("corpusSource");
    std::map<Index, double> projected;
    Index backwards = 0;
    for (Index f : allFaces(m))
    {
        const Index s = Index(nsrc[f]);
        const glm::dvec3 n = glm::normalize(newell[s]);
        const double a = glm::dot(m.faceAreaVector(f), n);
        backwards += a < -1e-9 * glm::length(newell[s]) && !bowties.count(s);
        projected[s] += a;
    }
    if (backwards)
        p.push_back(std::to_string(backwards) + " triangles face against their polygon");
    Index overlapping = 0;
    for (auto& [s, a] : projected)
        overlapping += std::abs(a - glm::length(newell[s])) > 1e-6 * glm::length(newell[s]) + 1e-12;
    if (overlapping)
        p.push_back(std::to_string(overlapping) + " polygons whose triangles overlap or leave gaps");
    return p;
}
} // namespace

TEST_CASE("corpus: triangulate")
{
    for (const Case& c : all())
        for (bool beauty : {true, false})
            for (geo::QuadSplit split : {geo::QuadSplit::ShortestDiagonal, geo::QuadSplit::FirstCorner})
            {
                geo::TriangulateOptions o;
                o.beauty = beauty;
                o.quads = split;
                std::vector<std::string> p = checkTriangulate(c.mesh, o);
                INFO(c.name << " beauty=" << beauty << " split=" << int(split) << ": " << join(p));
                CHECK(p.empty());
            }
}

TEST_CASE("corpus: relax")
{
    for (const Case& c : all())
    {
        Mesh m = c.mesh;
        retopo::RelaxOptions o;
        o.iterations = 20;
        retopo::relaxMesh(m, nullptr, o);
        std::vector<std::string> p = introduced(c.mesh, m);
        Index movedBorder = 0;
        for (Index q = 0; q < m.pointCapacity(); ++q)
            if (m.pointValid(q) && c.mesh.isBoundaryPoint(q) && m.position(q) != c.mesh.position(q))
                ++movedBorder;
        if (movedBorder)
            p.push_back(std::to_string(movedBorder) + " border points moved");
        if (Index f = flippedFaces(m, c.mesh))
            p.push_back(std::to_string(f) + " flipped faces");
        const double h = hausdorff(c.mesh, m);
        if (h > 0.01)
            p.push_back("hausdorff " + std::to_string(h));
        if (boundaryEdges(c.mesh) == 0)
        {
            const double v0 = signedVolume(c.mesh), v1 = signedVolume(m);
            if (std::abs(v1 - v0) > 0.01 * std::abs(v0))
                p.push_back("volume " + std::to_string(v0) + " -> " + std::to_string(v1));
        }
        dump(m, "corpus_relax_" + c.name);
        if (std::getenv("LILA_CORPUS_VERBOSE"))
            MESSAGE(c.name << " relax hausdorff " << h << " volume " << signedVolume(c.mesh) << " -> " << signedVolume(m));
        INFO(c.name << ": " << join(p));
        CHECK(p.empty());
    }
}

TEST_CASE("corpus: quad remesh")
{
    const char* only = std::getenv("LILA_CORPUS_ONLY");
    for (const Case& c : all())
    {
        if (only && c.name != only)
            continue;
        dump(c.mesh, "corpus_source_" + c.name);
        retopo::QuadRemeshOptions o;
        o.targetFaces = 600;
        retopo::QuadRemeshResult r = retopo::quadRemesh(c.mesh, o);
        const Mesh& m = r.mesh;
        std::vector<std::string> p = r.ok() ? introduced(c.mesh, m) : std::vector<std::string>{"error: " + r.error};
        if (r.metrics.quadPercent < 90.0)
            p.push_back("quads " + std::to_string(r.metrics.quadPercent) + "%");
        if (Index f = flippedFaces(m, c.mesh))
            p.push_back(std::to_string(f) + " flipped faces");
        // A crease can still be cut across by up to about half a lattice cell where extraction misses it.
        const double h = hausdorff(c.mesh, m);
        if (h > 0.035)
            p.push_back("hausdorff " + std::to_string(h));
        if ((boundaryEdges(c.mesh) == 0) != (boundaryEdges(m) == 0))
            p.push_back("border edges " + std::to_string(boundaryEdges(c.mesh)) + " -> " + std::to_string(boundaryEdges(m)));
        if (const double off = offBorder(m, c.mesh); off > 1e-6 * diagonal(c.mesh))
            p.push_back("border points off the source border by " + std::to_string(off));
        // Strips narrower than the requested edge length retry finer, so they may come out denser.
        const bool thin = c.name == "slivers" || c.name == "spiralFace";
        if (m.numFaces() < 200 || m.numFaces() > (thin ? 8000u : 1500u))
            p.push_back("faces " + std::to_string(m.numFaces()));
        dump(m, "corpus_remesh_" + c.name);
        INFO(c.name << ": " << r.metrics.summary() << " | " << join(p));
        CHECK(p.empty());
    }
}

namespace
{
Index triangleCount(const Mesh& m)
{
    Index n = 0;
    for (Index f : allFaces(m))
        n += m.faceSize(f) - 2;
    return n;
}

/** Corners whose uv isn't one the source had at that point: interpolated or swapped uvs make textures swim. */
Index foreignUvs(const Mesh& src, const Mesh& out)
{
    std::span<const glm::vec2> a = src.cornerAttrs().get<glm::vec2>("uv0"), b = out.cornerAttrs().get<glm::vec2>("uv0");
    if (a.empty())
        return 0;
    if (b.empty())
        return 1;
    const auto key = [](const glm::dvec3& p) { return std::array<double, 3>{p.x, p.y, p.z}; };
    std::map<std::array<double, 3>, std::vector<glm::vec2>> at;
    for (Index f : allFaces(src))
        src.forEachFaceCorner(f, [&](Index c) { at[key(src.position(src.cornerPoint(c)))].push_back(a[c]); });
    Index bad = 0;
    for (Index f : allFaces(out))
        out.forEachFaceCorner(f, [&](Index c) {
            const auto& list = at[key(out.position(out.cornerPoint(c)))];
            bad += std::find(list.begin(), list.end(), b[c]) == list.end();
        });
    return bad;
}

/** Faces whose uv triangle winds against the majority, i.e. mirrored or folded in uv space. */
Index uvFlips(const Mesh& m)
{
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    if (uv.empty())
        return 0;
    Index pos = 0, neg = 0;
    for (Index f : allFaces(m))
    {
        double area = 0.0;
        const Index c0 = m.faceCorner(f);
        for (Index c = m.cornerNext(c0); m.cornerNext(c) != c0; c = m.cornerNext(c))
        {
            const glm::dvec2 p = uv[c0], q = uv[c], r = uv[m.cornerNext(c)];
            area += (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
        }
        pos += area > 1e-12;
        neg += area < -1e-12;
    }
    return std::min(pos, neg);
}

std::set<std::array<double, 3>> borderPoints(const Mesh& m)
{
    std::set<std::array<double, 3>> out;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && m.isBoundaryPoint(p))
            out.insert({m.position(p).x, m.position(p).y, m.position(p).z});
    return out;
}
} // namespace

TEST_CASE("corpus: reduce")
{
    for (const Case& c : all())
        for (auto [quads, ratio] : {std::pair{false, 0.5}, std::pair{true, 0.5}, std::pair{false, 0.15}})
        {
            geo::ReduceOptions o;
            o.ratio = ratio;
            o.quads = quads;
            const geo::ReduceResult r = geo::reduceMesh(c.mesh, o);
            const Mesh& m = r.mesh;
            std::vector<std::string> p = introduced(c.mesh, m);
            const Index before = triangleCount(c.mesh), after = triangleCount(m);
            if (after > before)
                p.push_back("more triangles " + std::to_string(before) + " -> " + std::to_string(after));
            if (!quads && boundaryEdges(c.mesh) == 0 && before >= 200 && std::abs(double(after) - ratio * before) > 0.05 * before)
                p.push_back("missed target: " + std::to_string(before) + " -> " + std::to_string(after));
            // A thin wall cut down to a handful of triangles crosses itself, which reads as flipped.
            if (Index f = after >= 100 ? flippedFaces(m, c.mesh) : 0)
                p.push_back(std::to_string(f) + " flipped faces");
            // Halving a coarse mesh has to cut corners; a dense one must keep its shape.
            const double h = hausdorff(c.mesh, m);
            if (h > (before >= 500 ? (ratio < 0.5 ? 0.04 : 0.015) : 0.3))
                p.push_back("hausdorff " + std::to_string(h));
            if (borderPoints(m) != borderPoints(c.mesh))
                p.push_back("border points changed");
            if (std::abs(boundaryLength(m) - boundaryLength(c.mesh)) > 1e-9 * (1.0 + boundaryLength(c.mesh)))
                p.push_back("border length changed");
            if (Index u = foreignUvs(c.mesh, m))
                p.push_back(std::to_string(u) + " corners with uvs the source didn't have");
            Mesh triangulated = c.mesh;
            geo::triangulateFaces(triangulated, allFaces(triangulated));
            if (!quads && uvFlips(m) > uvFlips(triangulated))
                p.push_back("uv flips " + std::to_string(uvFlips(triangulated)) + " -> " + std::to_string(uvFlips(m)));
            if (std::getenv("LILA_CORPUS_VERBOSE"))
                MESSAGE(c.name << " reduce quads=" << quads << " " << before << " -> " << after << " tris, " << m.numFaces() << " faces, hausdorff " << h);
            dump(m, "corpus_reduce_" + c.name + (quads ? "_quads" : ""));
            INFO(c.name << " quads=" << quads << " ratio=" << ratio << ": " << join(p));
            CHECK(p.empty());
        }
}

TEST_CASE("reduce: hits counts, keeps hard edges and symmetry")
{
    Mesh sphere = geo::makeSphere({50, 64, 64});
    sphere.compact();
    for (Index target : {2000, 500, 100})
    {
        geo::ReduceOptions o;
        o.targetTriangles = target;
        const Index got = triangleCount(geo::reduceMesh(sphere, o).mesh);
        INFO(target << " -> " << got);
        CHECK(std::abs(double(got) - double(target)) <= 0.05 * target + 2);
    }

    // Faces of a hard-edged cube stay flat-shaded: every corner normal matches its face.
    Mesh cube = hardCube();
    geo::ReduceOptions o;
    o.ratio = 0.3;
    Mesh reduced = geo::reduceMesh(cube, o).mesh;
    const std::vector<glm::vec3> normals = geo::cornerNormals(reduced);
    Index soft = 0;
    for (Index f : allFaces(reduced))
        reduced.forEachFaceCorner(f, [&](Index c) { soft += glm::dot(glm::dvec3(normals[c]), reduced.faceNormal(f)) < 0.999; });
    CHECK(soft == 0);
    CHECK(triangleCount(reduced) < triangleCount(cube));

    // Symmetric: one half reduced and mirrored, so every point has an exact mirror image.
    geo::ReduceOptions so;
    so.ratio = 0.3;
    so.symmetryAxis = 0;
    Mesh symSrc = geo::makeSphere({50, 40, 30});
    geo::ReduceResult sr = geo::reduceMesh(symSrc, so);
    CHECK_FALSE(sr.symmetryIgnored);
    std::set<std::array<double, 3>> pts;
    for (Index p = 0; p < sr.mesh.pointCapacity(); ++p)
        pts.insert({sr.mesh.position(p).x, sr.mesh.position(p).y, sr.mesh.position(p).z});
    Index unmatched = 0;
    for (const auto& q : pts)
        unmatched += !pts.count({q[0] == 0.0 ? 0.0 : -q[0], q[1], q[2]});
    CHECK(unmatched == 0);
    CHECK(boundaryEdges(sr.mesh) == 0);
    CHECK(introduced(symSrc, sr.mesh).empty());
}

TEST_CASE("reduce: dense mesh timing")
{
    Mesh dense = noisySphere(360, 0.002, 2);
    dense.compact();
    const auto start = std::chrono::steady_clock::now();
    geo::ReduceOptions o;
    o.ratio = 0.1;
    const Mesh m = geo::reduceMesh(dense, o).mesh;
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    MESSAGE(triangleCount(dense) << " -> " << triangleCount(m) << " triangles in " << sec << " s");
    CHECK(introduced(dense, m).empty());
    CHECK(flippedFaces(m, dense) == 0);
    CHECK(hausdorff(dense, m) < 0.01);
}


TEST_CASE("relax: evens spacing, slides borders along themselves")
{
    // A grid with its inner points shoved around in the plane.
    Mesh grid = geo::makePlane({100, 100, 10, 10});
    grid.compact();
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> d(-3.5, 3.5);
    for (Index p = 0; p < grid.pointCapacity(); ++p)
        if (!grid.isBoundaryPoint(p))
            grid.setPosition(p, grid.position(p) + glm::dvec3(d(rng), 0.0, d(rng)));
    const auto spread = [](const Mesh& m) {
        double sum = 0.0, sum2 = 0.0;
        Index n = 0;
        for (Index e : allEdges(m))
        {
            sum += m.edgeLength(e);
            sum2 += m.edgeLength(e) * m.edgeLength(e);
            ++n;
        }
        const double mean = sum / n;
        return std::sqrt(std::max(0.0, sum2 / n - mean * mean)) / mean;
    };
    Mesh relaxed = grid;
    retopo::RelaxOptions o;
    o.iterations = 30;
    retopo::relaxMesh(relaxed, nullptr, o);
    CHECK(spread(relaxed) < 0.5 * spread(grid));
    CHECK(flippedFaces(relaxed, grid) == 0);
    CHECK(hausdorff(grid, relaxed) < 1e-9);

    // Border points left free slide along the border, the corners stay, and nothing leaves the outline.
    for (const Case& c : {Case{"grid", grid}, Case{"hemisphere", hemisphere()}, Case{"twistedGrid", twistedGrid(6)}})
    {
        Mesh m = c.mesh;
        m.compact();
        retopo::RelaxOptions free;
        free.iterations = 20;
        free.pinBoundary = false;
        retopo::relaxMesh(m, nullptr, free);
        INFO(c.name);
        CHECK(offBorder(m, c.mesh) < 1e-6 * diagonal(c.mesh));
        CHECK(std::abs(boundaryLength(m) - boundaryLength(c.mesh)) < 0.02 * boundaryLength(c.mesh));
        CHECK(flippedFaces(m, c.mesh) == 0);
        CHECK(introduced(c.mesh, m).empty());
    }
    Mesh corners = grid;
    retopo::RelaxOptions free;
    free.pinBoundary = false;
    retopo::relaxMesh(corners, nullptr, free);
    for (const glm::dvec3 corner : {glm::dvec3(-50, 0, -50), glm::dvec3(50, 0, -50), glm::dvec3(50, 0, 50), glm::dvec3(-50, 0, 50)})
    {
        bool found = false;
        for (Index p = 0; p < corners.pointCapacity(); ++p)
            found = found || glm::distance(corners.position(p), corner) < 1e-9;
        CHECK(found);
    }
}

TEST_CASE("triangulate: mirrored quads split the mirrored way")
{
    // Squares on both sides of x = 0 tie on diagonal length; the split must still mirror.
    Mesh m = geo::makeCube({100, 100, 100, 4, 4, 4});
    m.compact();
    geo::triangulateFaces(m, allFaces(m));
    std::set<std::array<std::array<double, 3>, 3>> tris;
    const auto key = [&](Index f, bool mirror) {
        std::array<std::array<double, 3>, 3> t;
        int k = 0;
        for (Index p : m.facePoints(f))
        {
            const glm::dvec3 q = m.position(p);
            t[k++] = {mirror ? (q.x == 0.0 ? 0.0 : -q.x) : q.x, q.y, q.z};
        }
        std::sort(t.begin(), t.end());
        return t;
    };
    for (Index f : allFaces(m))
        tris.insert(key(f, false));
    Index unmatched = 0;
    for (Index f : allFaces(m))
        unmatched += !tris.count(key(f, true));
    CHECK(unmatched == 0);
}
