#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "uv/FinalMesh.h"
#include "uv/LiveSolver.h"
#include "uv/Pack.h"
#include "uv/Unwrap.h"
#include "uv/Project.h"
#include "uv/Uv.h"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <map>
#include <cmath>
#include <set>
#include <string>
#include <utility>

using namespace lila;
using geo::Index;

namespace
{
/** A 10 x 10 cm plate 0.4 cm thick, box projected: each side a thin strip island next to the big top and bottom. */
geo::Mesh plate(double thick = 0.4, int grid = 6)
{
    geo::CubeOptions co;
    co.width = 10.0;
    co.height = thick;
    co.depth = 10.0;
    co.subdivX = grid;
    co.subdivZ = grid;
    geo::Mesh m = geo::makeCube(co);
    uv::ProjectOptions po;
    po.type = uv::Projection::Box;
    REQUIRE(uv::project(m, {}, "uv0", po));
    // Box projection stacks the islands; packed apart they no longer share uvs at shared points.
    REQUIRE(uv::packIslands(m, {}, "uv0").ok);
    return m;
}

/**
 * The plate unfolded like a box lid: the top and its four thin sides in one island (seams round the bottom and up the
 * corners), the bottom in another, packed. The sides are thin strips beside the big top, as on a game-art panel. With a
 * slit, a seam also runs from one side's edge to the middle of the top, ending inside the island.
 */
geo::Mesh lidPlate(double thick = 0.4, int grid = 6, bool slit = false)
{
    geo::CubeOptions co;
    co.width = 10.0;
    co.height = thick;
    co.depth = 10.0;
    co.subdivX = grid;
    co.subdivZ = grid;
    geo::Mesh m = geo::makeCube(co);
    std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        const glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
        const bool bottom = a.y < 0.0 && b.y < 0.0 && (std::abs(a.x) > 4.99 || std::abs(a.z) > 4.99) &&
                            (std::abs(b.x) > 4.99 || std::abs(b.z) > 4.99) && (std::abs(a.x - b.x) < 1e-9 || std::abs(a.z - b.z) < 1e-9) &&
                            ((std::abs(a.x) > 4.99 && std::abs(b.x) > 4.99 && std::abs(a.x - b.x) < 1e-9) ||
                             (std::abs(a.z) > 4.99 && std::abs(b.z) > 4.99 && std::abs(a.z - b.z) < 1e-9));
        const bool corner = std::abs(a.x) > 4.99 && std::abs(a.z) > 4.99 && std::abs(b.x) > 4.99 && std::abs(b.z) > 4.99 && glm::length(glm::dvec2(a.x - b.x, a.z - b.z)) < 1e-9;
        const bool cut = slit && a.y > 0.0 && b.y > 0.0 && std::abs(a.x) < 1e-9 && std::abs(b.x) < 1e-9 && std::max(a.z, b.z) < 1e-9;
        seam[e] = bottom || corner || cut;
    }
    uv::UnwrapFacesOptions uo;
    REQUIRE(uv::unwrapFaces(m, {}, "uv0", uo).islands == 2);
    REQUIRE(uv::packIslands(m, {}, "uv0").ok);
    return m;
}

/** A 6 x 1.2 x 6 cm box on a 3 x 3 grid, seamed where seam(a, b) says, unwrapped and packed. */
template <class Seam> geo::Mesh seamedBox(Seam&& seamAt, double scale = 1.0)
{
    geo::CubeOptions co;
    co.width = 6.0 * scale;
    co.height = 1.2 * scale;
    co.depth = 6.0 * scale;
    co.subdivX = 3;
    co.subdivZ = 3;
    geo::Mesh m = geo::makeCube(co);
    std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
            seam[e] = seamAt(m.position(m.edgePoint(e, 0)) / scale, m.position(m.edgePoint(e, 1)) / scale);
    uv::UnwrapFacesOptions uo;
    uv::unwrapFaces(m, {}, "uv0", uo);
    REQUIRE(uv::packIslands(m, {}, "uv0").ok);
    return m;
}

/**
 * A unit grid sheet, nx by nz cells, its cells in islands by islandOf(cell x, cell z), laid out flat, as a bevel leaves a
 * cage: the given points of island 0 take another point's uvs in it, uvs and spacing proxy alike.
 */
template <class IslandOf>
geo::Mesh bunchedSheet(int nx, int nz, IslandOf&& islandOf, const std::vector<std::pair<glm::ivec2, glm::ivec2>>& bunch,
                       std::vector<double> xs = {}, std::vector<double> zs = {}, bool creased = false)
{
    for (int i = int(xs.size()); i <= nx; ++i)
        xs.push_back(double(i));
    for (int i = int(zs.size()); i <= nz; ++i)
        zs.push_back(double(i));
    geo::Mesh m = geo::makePlane({.width = double(nx), .depth = double(nz), .subdivX = nx, .subdivZ = nz});
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    std::span<glm::vec2> proxy = m.cornerAttrs().getOrAdd<glm::vec2>("__fitSpacing@uv0");
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const glm::dvec3 c = m.faceCentroid(f) + glm::dvec3(0.5 * nx, 0.0, 0.5 * nz);
        const int island = islandOf(int(std::floor(c.x)), int(std::floor(c.z)));
        m.forEachFaceCorner(f, [&](Index k) {
            const glm::dvec3 p = m.position(m.cornerPoint(k)) + glm::dvec3(0.5 * nx, 0.0, 0.5 * nz);
            glm::ivec2 at(int(std::lround(p.x)), int(std::lround(p.z)));
            if (island == 0)
                for (const auto& [from, to] : bunch)
                    if (at == from)
                        at = to;
            const glm::vec2 q{float(xs[size_t(at.x)]), float(zs[size_t(at.y)])};
            proxy[k] = q;
            uv[k] = q * 0.02f + glm::vec2(0.05f + 0.3f * float(island), 0.05f);
        });
    }
    if (creased)
        for (float& c : m.edgeAttrs().getOrAdd<float>(geo::attr::kCrease))
            c = 10.0f;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            const glm::dvec3 q = m.position(p) + glm::dvec3(0.5 * nx, 0.0, 0.5 * nz);
            m.setPosition(p, glm::dvec3(xs[size_t(std::lround(q.x))], 0.0, zs[size_t(std::lround(q.z))]));
        }
    return m;
}

/** Triangles of the mesh's faces whose uv area is under kCollapsedFraction of their surface area at their island's density, or turned over. */
size_t collapsedOn(const geo::Mesh& m)
{
    const std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    size_t out = 0;
    for (const std::vector<Index>& faces : uv::islandFaces(m, {}, co))
    {
        std::vector<std::pair<double, double>> tris;
        double a3 = 0.0, auv = 0.0;
        for (Index f : faces)
        {
            const std::vector<Index> fc = m.faceCorners(f);
            for (size_t k = 1; k + 1 < fc.size(); ++k)
            {
                const glm::dvec3 p0 = m.position(m.cornerPoint(fc[0]));
                const double s = 0.5 * glm::length(glm::cross(m.position(m.cornerPoint(fc[k])) - p0, m.position(m.cornerPoint(fc[k + 1])) - p0));
                const glm::dvec2 u1 = glm::dvec2(uv[fc[k]] - uv[fc[0]]), u2 = glm::dvec2(uv[fc[k + 1]] - uv[fc[0]]);
                tris.push_back({s, 0.5 * (u1.x * u2.y - u1.y * u2.x)});
                a3 += s;
                auv += tris.back().second;
            }
        }
        const double density = auv / std::max(a3, 1e-300);
        for (auto [s, a] : tris)
            out += s > 1e-12 * a3 && a / density < uv::kCollapsedFraction * s;
    }
    return out;
}

/** Every edge bevelled 0.13 cm, then two levels of Catmull-Clark: the game-art stack that drags rim islands. */
uv::Stack bevelSubdivStack(double offset = 0.13, int segments = 1, int levels = 2, geo::SubdivideUvs uvs = geo::SubdivideUvs::FitSurface)
{
    uv::Stack s;
    s.push_back({"bevel", [=](const geo::Mesh& in) {
                     geo::Mesh m = in;
                     std::vector<Index> edges;
                     for (Index e = 0; e < m.edgeCapacity(); ++e)
                         if (m.edgeValid(e) && uv::dihedralAngle(m, e) > 30.0)
                             edges.push_back(e);
                     geo::BevelOptions o;
                     o.offset = offset;
                     o.segments = segments;
                     geo::bevelEdges(m, edges, o);
                     return m;
                 }});
    s.push_back({"subdivide", [=](const geo::Mesh& in) {
                     geo::SubdivideOptions o;
                     o.levels = levels;
                     o.uvs = uvs;
                     return geo::subdivide(in, o);
                 }, true});
    return s;
}

std::vector<uv::Chart> islandsOf(const geo::Mesh& m)
{
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    return uv::buildCharts(m, {}, co);
}

/** Area weighted angle distortion of the exported triangles, each capped at 1. */
double cappedAngle(const uv::FinalIsland& isl, const uv::Chart& ch, double* areaOut = nullptr)
{
    const std::vector<glm::dvec2> y = isl.uvs(ch);
    const double density = std::abs(isl.areaUv(ch)) / std::max(isl.area3d(), 1e-300);
    double a = 0.0, w = 0.0, ar = 0.0;
    for (const auto& t : isl.tris)
    {
        const glm::dvec3 p0 = isl.pos[t[0]], e1 = isl.pos[t[1]] - p0, e2 = isl.pos[t[2]] - p0;
        const double l1 = glm::length(e1), area = 0.5 * glm::length(glm::cross(e1, e2));
        if (l1 <= 0.0 || area <= 1e-12)
            continue;
        const glm::dvec3 x = e1 / l1;
        const double qx = glm::dot(e2, x), qy = 2.0 * area / l1;
        const glm::dvec2 d1 = y[t[1]] - y[t[0]], d2 = y[t[2]] - y[t[0]];
        const glm::dvec2 c0 = d1 / l1, c1 = (d2 - qx * c0) / qy;
        const double E = glm::dot(c0, c0), F = glm::dot(c0, c1), G = glm::dot(c1, c1);
        const double disc = std::sqrt(std::max(0.0, (E - G) * (E - G) + 4.0 * F * F));
        const double s1 = std::sqrt(std::max(0.0, 0.5 * (E + G + disc))), s2 = std::sqrt(std::max(0.0, 0.5 * (E + G - disc)));
        a += area * std::min(1.0, s2 > 1e-12 ? s1 / s2 - 1.0 : 1.0);
        ar += area * std::min(2.0, std::abs(std::log2(std::max(s1 * s2 / density, 1e-6))));
        w += area;
    }
    if (areaOut)
        *areaOut = w > 0.0 ? ar / w : 0.0;
    return w > 0.0 ? a / w : 0.0;
}

struct ExportState
{
    size_t collapsed = 0;
    double angle = 0.0; // area weighted
    size_t folded = 0;
    double capped = 0.0; // area weighted, each triangle's angle distortion capped at 1 so slivers don't swamp it
    double area = 0.0;   // area weighted |log2| of each triangle's area against the island's density, capped at 2
};

ExportState exportState(const geo::Mesh& cage, const uv::Stack& stack)
{
    uv::FinalMap map = uv::buildFinalMap(cage, "uv0", stack);
    INFO(map.warning);
    REQUIRE(map.active());
    std::vector<uv::Chart> charts = islandsOf(cage);
    std::vector<uv::FinalIsland> isl = uv::finalIslands(map, cage, charts, "uv0");
    ExportState s;
    double w = 0.0;
    for (size_t i = 0; i < charts.size(); ++i)
    {
        if (isl[i].tris.empty())
            continue;
        s.collapsed += uv::collapsedTriangles(isl[i], charts[i]);
        uv::ChartStats st = uv::finalStats(isl[i], charts[i]);
        s.folded += st.folded;
        s.angle += isl[i].area3d() * st.angle;
        w += isl[i].area3d();
        double ar = 0.0;
        s.capped += cappedAngle(isl[i], charts[i], &ar) * isl[i].area3d();
        s.area += ar * isl[i].area3d();
    }
    s.angle /= std::max(w, 1e-300);
    s.capped /= std::max(w, 1e-300);
    s.area /= std::max(w, 1e-300);
    return s;
}

/** Angle distortion of the exported mesh unwrapped on its own islands: the best a cage solve can hope to approach. */
double exportUnwrapAngle(const geo::Mesh& exported)
{
    std::vector<uv::Chart> charts = islandsOf(exported);
    double a = 0.0, w = 0.0;
    for (uv::Chart& ch : charts)
    {
        uv::unwrapChart(ch);
        uv::ChartStats st = uv::chartStats(ch);
        a += ch.area3d() * st.angle;
        w += ch.area3d();
    }
    return a / std::max(w, 1e-300);
}
} // namespace

/** Largest distance between the map's exported uvs and the exported mesh's own. */
double worstMiss(const uv::FinalMap& map, const geo::Mesh& cage)
{
    const std::vector<glm::dvec2> uv = uv::exportedUvs(map, cage, "uv0");
    const std::span<const glm::vec2> exUv = map.exported->cornerAttrs().get<glm::vec2>("uv0");
    double worst = 0.0;
    for (Index f = 0; f < map.exported->faceCapacity(); ++f)
        if (map.exported->faceValid(f))
            map.exported->forEachFaceCorner(f, [&](Index c) { worst = std::max(worst, glm::length(uv[c] - glm::dvec2(exUv[c]))); });
    return worst;
}

TEST_CASE("final mesh: the map reproduces the exported uvs")
{
    geo::Mesh m = plate();
    uv::FinalMap map = uv::buildFinalMap(m, "uv0", bevelSubdivStack());
    INFO(map.warning);
    REQUIRE(map.active());
    CHECK(map.warning.empty());
    CHECK(map.rings >= 2);
    CHECK(worstMiss(map, m) < 1e-4);
}

TEST_CASE("final mesh: a dragged rim island is re-solved until nothing collapses")
{
    // 0.4 cm plate, its sides in the top's island; three-segment 0.13 cm bevels, then two levels of subdivision.
    geo::Mesh m = lidPlate(0.4, 2);
    const uv::Stack stack = bevelSubdivStack(0.13, 3, 2);
    const ExportState before = exportState(m, stack);
    MESSAGE("collapsed before " << before.collapsed << ", folded " << before.folded << ", angle " << before.capped << ", area " << before.area);

    uv::FinalRefineResult r = uv::refineFinal(m, {}, "uv0", stack);
    CHECK(r.method == uv::FinalMethod::Probed);
    CHECK(r.collapsedBefore == before.collapsed);
    const ExportState after = exportState(m, stack);
    const double reference = exportUnwrapAngle(*uv::buildFinalMap(m, "uv0", stack).exported);
    MESSAGE("collapsed after " << after.collapsed << ", folded " << after.folded << ", angle " << after.capped << ", area " << after.area << ", export unwrap " << reference);
    CHECK(after.collapsed == 0);
    CHECK(after.folded <= before.folded);
    // Opening the slivers costs some evenness elsewhere: the cage's few uvs per island can't match an unwrap of the export.
    CHECK(after.capped < 0.6);
    CHECK(after.area < 0.6);
}

TEST_CASE("final mesh: fit surface opens the rim that blend collapses, before any solve")
{
    geo::Mesh m = lidPlate(0.4, 2);
    const ExportState blend = exportState(m, bevelSubdivStack(0.13, 3, 2, geo::SubdivideUvs::Blend));
    const ExportState fit = exportState(m, bevelSubdivStack(0.13, 3, 2));
    MESSAGE("collapsed blend " << blend.collapsed << ", fit " << fit.collapsed << "; angle blend " << blend.capped << ", fit " << fit.capped);
    CHECK(blend.collapsed > 0);
    CHECK(fit.collapsed == 0);
    CHECK(fit.folded == 0);
}

/**
 * Along every uv border of the mesh away from its corners, the steps between neighbouring edges' uv per surface length
 * that change it by more than a factor of two, and the largest such change (log2).
 */
std::pair<size_t, double> borderSpacingJumps(const geo::Mesh& m)
{
    const std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    auto cornerAt = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
    auto same = [&](Index a, Index b) { return glm::length(uv[a] - uv[b]) < 1e-6f; };
    std::map<Index, std::pair<Index, double>> next; // border corner -> next border corner and the edge's uv per length
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            m.forEachFaceCorner(f, [&](Index c) {
                const Index cn = m.cornerNext(c), e = m.cornerEdge(c);
                if (m.isManifoldEdge(e))
                {
                    const Index ec = m.edgeCorner(e), rc = m.cornerFace(ec) == f ? m.cornerRadial(ec) : ec;
                    if (same(cornerAt(rc, m.cornerPoint(c)), c) && same(cornerAt(rc, m.cornerPoint(cn)), cn))
                        return;
                }
                const double l = glm::length(m.position(m.cornerPoint(cn)) - m.position(m.cornerPoint(c)));
                next[c] = {cn, l > 0.0 ? glm::length(glm::dvec2(uv[cn]) - glm::dvec2(uv[c])) / l : 0.0};
            });
    // The border corner leaving the point where an edge arrives: same point, same uv.
    std::map<Index, std::vector<Index>> leaving;
    for (const auto& [c, unused] : next)
        leaving[m.cornerPoint(c)].push_back(c);
    size_t jumps = 0;
    double worst = 0.0;
    for (const auto& [c, nx] : next)
    {
        Index follow = geo::kInvalid;
        for (Index o : leaving[m.cornerPoint(nx.first)])
            if (same(o, nx.first))
                follow = o;
        if (follow == geo::kInvalid || nx.second <= 0.0 || next[follow].second <= 0.0)
            continue;
        // Island corners turn the border; spacing may change there.
        const glm::dvec2 d0 = glm::dvec2(uv[nx.first]) - glm::dvec2(uv[c]), d1 = glm::dvec2(uv[next[follow].first]) - glm::dvec2(uv[follow]);
        if (std::atan2(std::abs(d0.x * d1.y - d0.y * d1.x), glm::dot(d0, d1)) > 0.5)
            continue;
        const double step = std::abs(std::log2(next[follow].second / nx.second));
        worst = std::max(worst, step);
        jumps += step > 1.0;
    }
    return {jumps, worst};
}

TEST_CASE("final mesh: respaced borders ease into the spacing beside them")
{
    // Bevel corners bunch the border; respaced runs must not end in a step the interior fill shears against.
    for (auto [grid, segments] : {std::pair{2, 1}, std::pair{3, 3}, std::pair{6, 3}})
    {
        const uv::Stack stack = bevelSubdivStack(0.13, segments, 2);
        for (const geo::Mesh& m : {lidPlate(0.4, grid), plate(0.4, grid)})
        {
            const auto [jumps, worst] = borderSpacingJumps(uv::runStack(m, stack));
            INFO("grid " << grid << ", segments " << segments << ", worst " << worst);
            CHECK(jumps == 0);
            CHECK(worst < 0.6);
        }
    }
}

TEST_CASE("final mesh: a map stays exact after the cage's uvs are rewritten")
{
    // A tool solves against the map of the layout it starts from; respacing and rebuilt corners must not move with the
    // layout it writes, a bent one or one stretched 2.5 times along v.
    for (int shape = 0; shape < 8; ++shape)
    {
        const int grid = shape % 4 == 0 ? 3 : 6;
        const bool slits = shape % 4 >= 2;
        geo::Mesh m = slits ? lidPlate(0.4, grid, shape % 4 == 3) : plate(0.4, grid);
        const uv::Stack stack = bevelSubdivStack(0.13, 3, 2);
        const uv::FinalMap map = uv::buildFinalMap(m, "uv0", stack);
        REQUIRE(map.active());
        std::span<glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
        for (glm::vec2& v : uv)
            v = shape < 4 ? glm::vec2(1.7f * v.x + 0.3f * v.y * v.y, v.y + 0.2f * v.x * v.x) : glm::vec2(v.x, 2.5f * v.y);
        const std::vector<glm::dvec2> predicted = uv::exportedUvs(map, m, "uv0");
        const geo::Mesh exported = uv::runStack(m, stack);
        const std::span<const glm::vec2> actual = exported.cornerAttrs().get<glm::vec2>("uv0");
        REQUIRE(actual.size() == predicted.size());
        double worst = 0.0;
        for (Index f = 0; f < exported.faceCapacity(); ++f)
            if (exported.faceValid(f))
                exported.forEachFaceCorner(f, [&](Index c) { worst = std::max(worst, glm::length(predicted[c] - glm::dvec2(actual[c]))); });
        INFO("shape " << shape << ", worst " << worst);
        CHECK(worst < 1e-5);
    }
}

TEST_CASE("final mesh: rebuilt island corners turn with their border, whatever the layout")
{
    // A respaced corner rises off its border square to its neighbours' edge: a rotation of that edge, so the map carries
    // it however the layout turns the border (here up to a radian across the sheet).
    geo::Mesh m = plate(0.4, 2);
    const uv::Stack stack = bevelSubdivStack(0.13, 3, 2);
    const uv::FinalMap map = uv::buildFinalMap(m, "uv0", stack);
    REQUIRE(map.active());
    CHECK(std::ranges::any_of(map.weights, [](const uv::FinalWeight& w) { return std::abs(w.imag()) > 1e-6; }));
    std::span<glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    for (glm::vec2& v : uv)
    {
        const float a = v.x;
        v = glm::vec2(v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a));
    }
    const std::vector<glm::dvec2> predicted = uv::exportedUvs(map, m, "uv0");
    const geo::Mesh exported = uv::runStack(m, stack);
    const std::span<const glm::vec2> actual = exported.cornerAttrs().get<glm::vec2>("uv0");
    double worst = 0.0;
    for (Index f = 0; f < exported.faceCapacity(); ++f)
        if (exported.faceValid(f))
            exported.forEachFaceCorner(f, [&](Index c) { worst = std::max(worst, glm::length(predicted[c] - glm::dvec2(actual[c]))); });
    INFO("worst " << worst);
    CHECK(worst < 1e-5);
}

TEST_CASE("final mesh: a fit surface map reaches across whole islands")
{
    // Islands far wider than the probe rings: every interior uv of the export reads the whole island border, through the
    // fill the subdivision recorded rather than rows of their own.
    geo::Mesh m = plate(0.4, 24);
    const uv::Stack stack = bevelSubdivStack();
    uv::FinalMap map = uv::buildFinalMap(m, "uv0", stack);
    INFO(map.warning);
    REQUIRE(map.active());
    const double worst = worstMiss(map, m);
    MESSAGE("runs " << map.runs << ", rings " << map.rings << ", entries " << map.cols.size() << ", fills " << map.fills.size() << ", worst " << worst);
    CHECK(worst < 1e-3);
    CHECK(!map.fills.empty());
    CHECK(map.cols.size() < 20 * map.exported->numCorners());
    const uv::FinalRefineResult r = uv::refineFinal(m, {}, "uv0", stack);
    MESSAGE("collapsed " << r.collapsedBefore << " -> " << r.collapsedAfter << ", angle " << r.distortionBefore << " -> " << r.distortionAfter);
    CHECK(r.collapsedAfter == 0);
    CHECK(r.distortionAfter < r.distortionBefore);
}

TEST_CASE("final mesh: a fill the stack changes after the subdivision is probed over whole islands")
{
    geo::Mesh m = plate(0.4, 6);
    uv::Stack stack = bevelSubdivStack(0.13, 1, 1);
    stack.push_back({"smooth", [](const geo::Mesh& in) { return geo::subdivide(in, {.levels = 1, .uvs = geo::SubdivideUvs::Blend}); }, true});
    const uv::FinalMap map = uv::buildFinalMap(m, "uv0", stack);
    INFO(map.warning);
    REQUIRE(map.active());
    CHECK(map.fills.empty());
    CHECK(worstMiss(map, m) < 1e-3);
}

TEST_CASE("final mesh: refine reports the slivers the written uvs leave, and repair solves only islands with slivers")
{
    // The lid island's thin sides folded flat onto the top's edge: its rim strips collapse on the export.
    geo::Mesh m = lidPlate(0.4, 3);
    const uv::Stack stack = bevelSubdivStack(0.13, 3, 2);
    std::vector<uv::Chart> charts = islandsOf(m);
    REQUIRE(charts.size() == 2);
    const size_t lid = charts[0].area3d() > charts[1].area3d() ? 0 : 1;
    uv::Chart& ch = charts[lid];
    for (size_t v = 0; v < ch.pos.size(); ++v)
        if (ch.pos[v].y < 0.0)
            for (size_t t = 0; t < ch.pos.size(); ++t)
                if (ch.pos[t].y > 0.0 && glm::length(glm::dvec2(ch.pos[t].x - ch.pos[v].x, ch.pos[t].z - ch.pos[v].z)) < 1e-6)
                    ch.uv[v] = glm::mix(ch.uv[t], ch.uv[v], 0.001);
    uv::writeCharts(m, charts, "uv0");
    const std::vector<glm::dvec2> other = islandsOf(m)[1 - lid].uv;
    REQUIRE(exportState(m, stack).collapsed > 0);

    const uv::FinalRefineResult r = uv::repairFinal(m, {}, "uv0", stack);
    const ExportState after = exportState(m, stack);
    MESSAGE("collapsed " << r.collapsedBefore << " -> " << r.collapsedAfter << ", export shows " << after.collapsed);
    CHECK(r.collapsedAfter == after.collapsed);
    CHECK(after.collapsed == 0);
    CHECK(islandsOf(m)[1 - lid].uv == other);
}

TEST_CASE("final mesh: a turn a bevel moved along a respaced border stays at the layout's turn")
{
    // An L-shaped island whose bevel gave the cell inside its reflex corner to its neighbour: the export's border runs on
    // past the cage's corner and turns at the far side of that cell, and Blend leaves the cell's border points on the
    // corner, spacing proxy included. Spread by surface length, the turning point lands down the next leg and the strip
    // beside it turns with it.
    using Bunch = std::vector<std::pair<glm::ivec2, glm::ivec2>>;
    const geo::Mesh m = bunchedSheet(5, 4, [](int x, int z) { return (z <= 1 || x <= 1) && !(x == 2 && z == 1) ? 0 : 1; }, Bunch{{{2, 1}, {2, 2}}, {{3, 1}, {2, 2}}},
                                     {0, 4, 8, 8.15, 12.15, 16.15}, {0, 4, 4.15, 8.15, 12.15});
    CHECK(collapsedOn(geo::subdivide(m, {.levels = 2, .uvs = geo::SubdivideUvs::FitSurface})) == 0);
}

/**
 * A sheet whose bevel cut a tongue of island 1 into island 0 where the cage's border runs straight: island 0's border runs
 * along z = 2, turns up at (4, 2) beside the tongue and back along its top, with bevel-sized strips on both legs of the
 * corner. In the cage (uvs and spacing proxy) the corner's strip ends bunch on it and the tongue's sides lie on along the
 * straight border; the layout bends that straight run bend radians away from the island at the corner.
 */
geo::Mesh notchSheet(double bend, bool creased, int top)
{
    const std::vector<double> xs{0, 2, 4, 4.35, 4.43, 4.51, 4.64, 4.87, 6.87, 8.87};
    const std::vector<double> zs{0, 2, 3.12, 3.2, 3.28, 3.83, 5.33, 7.33, 9.33};
    const int nx = int(xs.size()) - 1, nz = int(zs.size()) - 1;
    auto islandOf = [&](int x, int z) { return z == 0 || (x < 2 && z < top) ? 1 : 0; };
    const glm::dvec2 corner(4.0, 2.0);
    std::map<std::pair<int, int>, glm::dvec2> cage;
    const double leg1[] = {0.0, 0.039, 0.078, 0.117, 0.216, 0.43};
    for (int x = 2; x <= 7; ++x)
        cage[{x, 1}] = corner + glm::dvec2(leg1[x - 2], 0.0);
    double s = 0.0;
    for (int z = 2; z <= top; ++z)
        cage[{2, z}] = corner - glm::dvec2(s += z <= 4 ? 0.04 : zs[size_t(z)] - zs[size_t(z - 1)], 0.0);
    for (int x = 1; x >= 0; --x)
        cage[{x, top}] = corner - glm::dvec2(s += xs[size_t(x + 1)] - xs[size_t(x)], 0.0);
    for (int z = top + 1; z <= nz; ++z)
        cage[{0, z}] = corner + glm::dvec2(-s, zs[size_t(z)] - zs[size_t(top)]);
    geo::Mesh m = geo::makePlane({.width = double(nx), .depth = double(nz), .subdivX = nx, .subdivZ = nz});
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    std::span<glm::vec2> proxy = m.cornerAttrs().getOrAdd<glm::vec2>("__fitSpacing@uv0");
    const glm::dvec2 turn(std::cos(bend), std::sin(bend));
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const glm::dvec3 c = m.faceCentroid(f) + glm::dvec3(0.5 * nx, 0.0, 0.5 * nz);
        const int island = islandOf(int(std::floor(c.x)), int(std::floor(c.z)));
        m.forEachFaceCorner(f, [&](Index k) {
            const glm::dvec3 p = m.position(m.cornerPoint(k)) + glm::dvec3(0.5 * nx, 0.0, 0.5 * nz);
            const auto it = cage.find({int(std::lround(p.x)), int(std::lround(p.z))});
            const bool moved = island == 0 && it != cage.end();
            const glm::dvec2 q = moved ? it->second : glm::dvec2(xs[size_t(std::lround(p.x))], zs[size_t(std::lround(p.z))]);
            proxy[k] = glm::vec2(q);
            glm::dvec2 u = q;
            if (moved && q.x < corner.x)
            {
                const glm::dvec2 d = q - corner;
                u = corner + glm::dvec2(d.x * turn.x - d.y * turn.y, d.x * turn.y + d.y * turn.x);
            }
            uv[k] = glm::vec2(u * 0.02 + glm::dvec2(0.05 + 0.3 * island, 0.05));
        });
    }
    if (creased)
        for (float& cr : m.edgeAttrs().getOrAdd<float>(geo::attr::kCrease))
            cr = 10.0f;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            const glm::dvec3 q = m.position(p) + glm::dvec3(0.5 * nx, 0.0, 0.5 * nz);
            m.setPosition(p, glm::dvec3(xs[size_t(std::lround(q.x))], 0.0, zs[size_t(std::lround(q.z))]));
        }
    return m;
}

TEST_CASE("final mesh: a notch a bevel cut into a straight cage border opens, however the layout bends there")
{
    // The fill must not fold the strips beside the notch's first corner onto the border, even where the layout bends
    // the straight run away from the island.
    for (double bend : {0.0, 0.26, 0.52})
        for (bool creased : {false, true})
            for (int top : {5, 6})
            {
                const geo::Mesh m = notchSheet(bend, creased, top);
                INFO("bend " << bend << ", creased " << creased << ", top " << top);
                CHECK(collapsedOn(geo::subdivide(m, {.levels = 2, .uvs = geo::SubdivideUvs::FitSurface})) == 0);
            }
}

TEST_CASE("final mesh: a border point between two bunched runs is respaced with them")
{
    // A bevel's corner patch left the island's border points round a reflex corner on two cage uvs, so two bunched runs
    // meet at one point. Respaced as two windows, that point stays where Blend left it, on the corner, and the cells
    // beside it can't open.
    using Bunch = std::vector<std::pair<glm::ivec2, glm::ivec2>>;
    const geo::Mesh m = bunchedSheet(5, 4, [](int x, int z) { return z >= 2 || x >= 3 ? 0 : 1; },
                                     Bunch{{{1, 2}, {2, 1}}, {{2, 2}, {2, 1}}, {{3, 2}, {2, 1}}, {{3, 1}, {2, 1}}, {{3, 0}, {2, 0}}}, {}, {}, true);
    CHECK(collapsedOn(geo::subdivide(m, {.levels = 2, .uvs = geo::SubdivideUvs::FitSurface})) == 0);
}

TEST_CASE("final mesh: islands whose export reads a neighbour that moved solve again")
{
    // A sheet in two islands split down x = 0 with a hair's gap between them; the stack bends it and welds the seam, so
    // the seam's exported uvs read both islands. The columns beside the seam are squeezed and everything but the seam is
    // pinned: each island opens its column by pushing the weld, solved first against the other where it started.
    geo::Mesh m = geo::makePlane({.width = 8.0, .depth = 4.0, .subdivX = 8, .subdivZ = 4});
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    std::span<bool> pin = m.cornerAttrs().getOrAdd<bool>("uvPin");
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const double side = m.faceCentroid(f).x;
        m.forEachFaceCorner(f, [&](Index c) {
            const glm::dvec3 p = m.position(m.cornerPoint(c));
            const double x = std::abs(std::abs(p.x) - 1.0) < 1e-9 ? 0.01 * p.x : p.x;
            pin[c] = std::abs(p.x) > 1e-9;
            uv[c] = glm::vec2(float(x / 10.0 + 0.5 + (side > 0.0 ? 0.002 : 0.0)), float(0.5 - p.z / 10.0));
        });
    }
    REQUIRE(islandsOf(m).size() == 2);
    uv::Stack stack;
    stack.push_back({"bend", [](const geo::Mesh& in) {
                         geo::Mesh out = in;
                         for (Index p = 0; p < out.pointCapacity(); ++p)
                             if (out.pointValid(p))
                                 out.setPosition(p, out.position(p) + glm::dvec3(0.0, 0.02 * out.position(p).z * out.position(p).z, 0.0));
                         return out;
                     }});
    stack.push_back({"weldSeam", [](const geo::Mesh& in) {
                         geo::Mesh out = in;
                         for (size_t l = 0; l < out.cornerAttrs().layerCount(); ++l)
                         {
                             geo::Attribute& a = out.cornerAttrs().layer(l);
                             if (a.type() != geo::AttrType::Float2)
                                 continue;
                             const std::span<glm::vec2> v = a.values<glm::vec2>();
                             for (Index p = 0; p < out.pointCapacity(); ++p)
                             {
                                 if (!out.pointValid(p) || std::abs(out.position(p).x) > 1e-9)
                                     continue;
                                 glm::dvec2 sum(0.0);
                                 double n = 0.0;
                                 out.forEachPointCorner(p, [&](Index c) {
                                     sum += glm::dvec2(v[c]);
                                     n += 1.0;
                                 });
                                 out.forEachPointCorner(p, [&](Index c) { v[c] = glm::vec2(sum / n); });
                             }
                         }
                         return out;
                     }});
    const size_t before = exportState(m, stack).collapsed;
    REQUIRE(before > 0);
    const uv::FinalRefineResult r = uv::refineFinal(m, {}, "uv0", stack);
    MESSAGE("collapsed " << before << " -> " << r.collapsedAfter);
    CHECK(r.collapsedAfter == exportState(m, stack).collapsed);
    CHECK(r.collapsedAfter == 0);
}

TEST_CASE("final mesh: islands stay where they were")
{
    geo::Mesh m = plate();
    std::vector<uv::Chart> before = islandsOf(m);
    uv::refineFinal(m, {}, "uv0", bevelSubdivStack());
    std::vector<uv::Chart> after = islandsOf(m);
    REQUIRE(before.size() == after.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        // Islands that grew past the tile border are moved back inside it.
        CHECK(glm::length(before[i].uvCentroid() - after[i].uvCentroid()) < 0.2);
        CHECK(glm::all(glm::greaterThanEqual(after[i].uvMin(), glm::dvec2(-1e-6))));
        CHECK(glm::all(glm::lessThanEqual(after[i].uvMax(), glm::dvec2(1.0 + 1e-6))));
    }
}

/** Raster cells (1024 per uv unit) the exported islands share with another island, counted at cell centres as bake lint does. */
size_t exportOverlap(const uv::FinalMap& map, const geo::Mesh& cage)
{
    const std::vector<uv::Chart> charts = islandsOf(cage);
    std::vector<int> islandOf(cage.faceCapacity(), -1);
    for (size_t i = 0; i < charts.size(); ++i)
        for (Index f : charts[i].faces)
            islandOf[f] = int(i);
    const std::vector<glm::dvec2> uv = uv::exportedUvs(map, cage, "uv0");
    std::map<std::pair<int, int>, int> owner;
    size_t shared = 0;
    const geo::Mesh& ex = *map.exported;
    for (Index f = 0; f < ex.faceCapacity(); ++f)
    {
        if (!ex.faceValid(f))
            continue;
        const int island = islandOf[map.sourceFace[f]];
        const std::vector<Index> cs = ex.faceCorners(f);
        for (size_t k = 1; k + 1 < cs.size(); ++k)
        {
            const glm::dvec2 p[3] = {uv[cs[0]] * 1024.0, uv[cs[k]] * 1024.0, uv[cs[k + 1]] * 1024.0};
            auto side = [](const glm::dvec2& s, const glm::dvec2& t, const glm::dvec2& q) { return (t.x - s.x) * (q.y - s.y) - (t.y - s.y) * (q.x - s.x); };
            const double sign = side(p[0], p[1], p[2]) > 0.0 ? 1.0 : -1.0;
            const glm::dvec2 lo = glm::floor(glm::min(p[0], glm::min(p[1], p[2]))), hi = glm::ceil(glm::max(p[0], glm::max(p[1], p[2])));
            for (int y = int(lo.y); y < int(hi.y); ++y)
                for (int x = int(lo.x); x < int(hi.x); ++x)
                {
                    const glm::dvec2 q(x + 0.5, y + 0.5);
                    if (sign * side(p[0], p[1], q) < 0.0 || sign * side(p[1], p[2], q) < 0.0 || sign * side(p[2], p[0], q) < 0.0)
                        continue;
                    auto [it, added] = owner.try_emplace({x, y}, island);
                    shared += !added && it->second != island;
                }
        }
    }
    return shared;
}

TEST_CASE("final mesh: an island moved onto another comes back clear at its size and shape, the rest untouched")
{
    const uv::Stack stack = bevelSubdivStack();
    for (const bool boxedIn : {false, true})
    {
        CAPTURE(boxedIn);
        geo::Mesh m = plate();
        const uv::FinalMap map = uv::buildFinalMap(m, "uv0", stack);
        REQUIRE(map.active());
        REQUIRE(exportOverlap(map, m) == 0);
        const std::span<const glm::vec2> was = std::as_const(m).cornerAttrs().get<glm::vec2>("uv0");
        const std::vector<glm::vec2> before(was.begin(), was.end());

        // A thin side strip, moved just into its nearest neighbour or into the middle of the biggest island.
        std::vector<uv::Chart> charts = islandsOf(m);
        size_t strip = 0, big = 0;
        for (size_t i = 1; i < charts.size(); ++i)
        {
            if (charts[i].areaUv() < charts[strip].areaUv())
                strip = i;
            if (charts[i].areaUv() > charts[big].areaUv())
                big = i;
        }
        size_t near = strip == 0 ? 1 : 0;
        for (size_t i = 0; i < charts.size(); ++i)
            if (i != strip && glm::length(charts[i].uvCentroid() - charts[strip].uvCentroid()) <
                                  glm::length(charts[near].uvCentroid() - charts[strip].uvCentroid()))
                near = i;
        const glm::dvec2 to = charts[boxedIn ? big : near].uvCentroid(), from = charts[strip].uvCentroid();
        glm::dvec2 shift = to - from;
        if (!boxedIn)
        {
            const glm::dvec2 step = glm::normalize(shift) * 0.002;
            shift = glm::dvec2(0.0);
            uv::Chart moved = charts[strip];
            for (int k = 0; k < 400 && exportOverlap(map, m) < 20; ++k)
            {
                shift += step;
                for (size_t v = 0; v < moved.uv.size(); ++v)
                    moved.uv[v] = charts[strip].uv[v] + shift;
                uv::writeCharts(m, std::span(&moved, 1), "uv0");
            }
        }
        else
        {
            uv::Chart moved = charts[strip];
            for (glm::dvec2& p : moved.uv)
                p += shift;
            uv::writeCharts(m, std::span(&moved, 1), "uv0");
        }
        REQUIRE(exportOverlap(map, m) > 0);
        const std::span<const glm::vec2> placed = std::as_const(m).cornerAttrs().get<glm::vec2>("uv0");
        const std::vector<glm::vec2> onTop(placed.begin(), placed.end());

        CHECK(uv::keepIslandsApart(m, map, "uv0", before) == 1);
        CHECK(exportOverlap(map, m) == 0);
        const std::span<const glm::vec2> now = std::as_const(m).cornerAttrs().get<glm::vec2>("uv0");
        std::set<Index> stripCorners(charts[strip].corners.begin(), charts[strip].corners.end());
        const Index first = charts[strip].corners[0];
        const glm::dvec2 moveBy = glm::dvec2(now[first] - onTop[first]);
        for (Index c = 0; c < now.size(); ++c)
            if (stripCorners.count(c))
                CHECK(glm::length(glm::dvec2(now[c] - onTop[c]) - moveBy) < 1e-5);
            else
                CHECK(now[c] == onTop[c]);
        // A short move clears an island just into its neighbour.
        if (!boxedIn)
            CHECK(glm::length(moveBy) < glm::length(shift));
        const uv::Chart after = islandsOf(m)[strip];
        CHECK(glm::all(glm::greaterThanEqual(after.uvMin(), glm::dvec2(0.0))));
        CHECK(glm::all(glm::lessThanEqual(after.uvMax(), glm::dvec2(1.0))));
    }
}

TEST_CASE("final mesh: a stack that leaves the surface alone changes nothing")
{
    geo::Mesh m = plate();
    const geo::Mesh orig = m;
    uv::Stack same;
    same.push_back({"identity", [](const geo::Mesh& in) { return in; }});
    uv::FinalRefineResult r = uv::refineFinal(m, {}, "uv0", same);
    CHECK(r.method == uv::FinalMethod::Cage);
    CHECK(r.warning.empty());
    const std::span<const glm::vec2> a = orig.cornerAttrs().get<glm::vec2>("uv0"), b = std::as_const(m).cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
        CHECK(a[i] == b[i]);
    CHECK(uv::refineFinal(m, {}, "uv0", {}).method == uv::FinalMethod::Cage);
}

TEST_CASE("final mesh: a stack that rewrites uvs is reported and left on the cage")
{
    geo::Mesh m = plate();
    uv::Stack stack = bevelSubdivStack();
    stack.push_back({"planarUvs", [](const geo::Mesh& in) {
                         geo::Mesh out = in;
                         std::span<glm::vec2> uv = out.cornerAttrs().get<glm::vec2>("uv0");
                         for (Index c = 0; c < out.cornerCapacity(); ++c)
                             if (c < uv.size())
                                 uv[c] = glm::vec2(out.position(out.cornerPoint(c)).x * 0.1, out.position(out.cornerPoint(c)).z * 0.1);
                         return out;
                     }});
    uv::FinalMap map = uv::buildFinalMap(m, "uv0", stack);
    CHECK_FALSE(map.active());
    CHECK(map.warning.find("planarUvs") != std::string::npos);
}

TEST_CASE("final mesh: pins hold")
{
    geo::Mesh m = plate();
    std::vector<uv::Chart> charts = islandsOf(m);
    // Pin two vertices of the largest island.
    size_t big = 0;
    for (size_t i = 1; i < charts.size(); ++i)
        if (charts[i].area3d() > charts[big].area3d())
            big = i;
    std::span<bool> pin = m.cornerAttrs().getOrAdd<bool>("uvPin");
    const uv::Chart& ch = charts[big];
    const Index v0 = ch.cornerVertex[0], v1 = ch.cornerVertex[2];
    for (size_t k = 0; k < ch.corners.size(); ++k)
        if (ch.cornerVertex[k] == v0 || ch.cornerVertex[k] == v1)
            pin[ch.corners[k]] = true;
    const glm::dvec2 p0 = ch.uv[v0], p1 = ch.uv[v1];
    uv::refineFinal(m, {}, "uv0", bevelSubdivStack());
    std::vector<uv::Chart> after = islandsOf(m);
    bool found = false;
    for (const uv::Chart& a : after)
        for (size_t k = 0; k < a.corners.size(); ++k)
            if (a.corners[k] == ch.corners[0])
            {
                found = true;
                CHECK(glm::length(a.uv[a.cornerVertex[k]] - p0) < 1e-6);
            }
    CHECK(found);
    (void)p1;
}

TEST_CASE("final mesh: pack measures density on the exported mesh")
{
    // Two 10 cm quads side by side; the stack scales the right one up 2x, so its export has 4x the cage area.
    const glm::dvec3 pts[8] = {{0, 0, 0}, {10, 0, 0}, {10, 0, 10}, {0, 0, 10}, {20, 0, 0}, {30, 0, 0}, {30, 0, 10}, {20, 0, 10}};
    const Index sizes[2] = {4, 4};
    const Index idx[8] = {0, 3, 2, 1, 4, 7, 6, 5};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    uv::ProjectOptions po;
    po.axis = uv::ProjectAxis::Y;
    REQUIRE(uv::project(m, {}, "uv0", po));
    uv::Stack stack;
    stack.push_back({"grow", [](const geo::Mesh& in) {
                         geo::Mesh out = in;
                         for (Index p = 0; p < out.pointCapacity(); ++p)
                             if (out.pointValid(p) && out.position(p).x > 15.0)
                                 out.setPosition(p, glm::dvec3(25.0, 0.0, 5.0) + (out.position(p) - glm::dvec3(25.0, 0.0, 5.0)) * 2.0);
                         return out;
                     }});
    std::vector<uv::Chart> charts = islandsOf(m);
    REQUIRE(charts.size() == 2);
    uv::FinalMap map = uv::applyDensityAreas(m, charts, "uv0", stack);
    REQUIRE(map.active());
    uv::PackOptions pk;
    REQUIRE(uv::packCharts(charts, pk).ok);
    uv::writeCharts(m, charts, "uv0");
    // Density on the export: uv area over exported surface area, per island.
    uv::FinalMap after = uv::buildFinalMap(m, "uv0", stack);
    std::vector<uv::Chart> c2 = islandsOf(m);
    std::vector<uv::FinalIsland> isl = uv::finalIslands(after, m, c2, "uv0");
    const double d0 = std::abs(isl[0].areaUv(c2[0])) / isl[0].area3d(), d1 = std::abs(isl[1].areaUv(c2[1])) / isl[1].area3d();
    MESSAGE("export densities " << d0 << " " << d1);
    CHECK(std::abs(std::sqrt(d0 / d1) - 1.0) < 0.1);
}

TEST_CASE("final mesh: a live relax on the export keeps cage uvs it barely reads with their neighbours")
{
    geo::Mesh m = lidPlate(0.4, 3);
    const std::vector<uv::Chart> before = islandsOf(m);
    std::vector<uv::LiveIsland> islands = uv::liveIslands(m, {}, "uv0");
    uv::attachFinal(islands, m, "uv0", bevelSubdivStack(0.13, 3, 2));
    for (uv::LiveIsland& is : islands)
        is.solve(200);
    uv::writeLiveIslands(m, islands, "uv0", false);
    const std::vector<uv::Chart> after = islandsOf(m);
    REQUIRE(before.size() == after.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        const glm::dvec2 sb = before[i].uvMax() - before[i].uvMin(), sa = after[i].uvMax() - after[i].uvMin();
        INFO("island " << i << ", grows " << std::max(sa.x / sb.x, sa.y / sb.y));
        CHECK(std::max(sa.x / sb.x, sa.y / sb.y) < 1.1);
    }
}

TEST_CASE("final mesh: a live relax on the export lowers the export's distortion, fills and rotated corners included")
{
    // The live solver reads each fill as a dense block over the chart vertices its border reads, both ways; a wrong
    // block or transpose would leave the export as distorted as it started.
    geo::Mesh m = plate(0.4, 2);
    const uv::Stack stack = bevelSubdivStack(0.13, 3, 2);
    uv::FinalMap map = uv::buildFinalMap(m, "uv0", stack);
    REQUIRE(map.active());
    REQUIRE(!map.fills.empty());
    REQUIRE(std::ranges::any_of(map.weights, [](const uv::FinalWeight& w) { return std::abs(w.imag()) > 1e-6; }));
    auto distortion = [&](const geo::Mesh& mesh) {
        const std::vector<uv::Chart> charts = islandsOf(mesh);
        const std::vector<uv::FinalIsland> isl = uv::finalIslands(map, mesh, charts, "uv0");
        double a = 0.0, w = 0.0;
        for (size_t i = 0; i < charts.size(); ++i)
            if (!isl[i].tris.empty())
            {
                a += isl[i].area3d() * uv::finalStats(isl[i], charts[i]).angle;
                w += isl[i].area3d();
            }
        return a / w;
    };
    const double before = distortion(m);
    std::vector<uv::LiveIsland> islands = uv::liveIslands(m, {}, "uv0");
    uv::attachFinal(islands, m, "uv0", stack);
    for (uv::LiveIsland& is : islands)
        is.solve(100);
    uv::writeLiveIslands(m, islands, "uv0", false);
    const double after = distortion(m);
    INFO("angle distortion " << before << " -> " << after);
    CHECK(after < 0.8 * before);
}

TEST_CASE("final mesh: solve time on a big export" * doctest::skip())
{
    geo::CubeOptions co;
    co.width = 10.0;
    co.height = 0.4;
    co.depth = 10.0;
    co.subdivX = 40;
    co.subdivZ = 40;
    geo::Mesh m = geo::makeCube(co);
    uv::ProjectOptions po;
    po.type = uv::Projection::Box;
    REQUIRE(uv::project(m, {}, "uv0", po));
    uv::Stack stack = bevelSubdivStack();
    auto t0 = std::chrono::steady_clock::now();
    uv::FinalMap map = uv::buildFinalMap(m, "uv0", stack);
    auto t1 = std::chrono::steady_clock::now();
    uv::refineFinal(m, {}, "uv0", stack);
    auto t2 = std::chrono::steady_clock::now();
    MESSAGE("export faces " << map.exported->numFaces() << ", map " << std::chrono::duration<double>(t1 - t0).count() << " s, refine "
                            << std::chrono::duration<double>(t2 - t1).count() << " s");
    // Live relax: one session's islands, stepped as the uv editor does.
    for (int final = 0; final < 2; ++final)
    {
        std::vector<uv::LiveIsland> islands = uv::liveIslands(m, {}, "uv0");
        auto a0 = std::chrono::steady_clock::now();
        if (final)
            uv::attachFinal(islands, m, "uv0", stack);
        auto a1 = std::chrono::steady_clock::now();
        for (uv::LiveIsland& is : islands)
        {
            is.wake();
            is.step();
        }
        auto a2 = std::chrono::steady_clock::now();
        for (int k = 0; k < 10; ++k)
            for (uv::LiveIsland& is : islands)
                is.step();
        auto a3 = std::chrono::steady_clock::now();
        const std::string label = final ? "live on export" : "live on cage";
        MESSAGE(label << ": attach " << std::chrono::duration<double>(a1 - a0).count() << " s, first step "
                      << std::chrono::duration<double>(a2 - a1).count() * 1000.0 << " ms, step " << std::chrono::duration<double>(a3 - a2).count() * 100.0
                      << " ms");
    }
}

TEST_CASE("final mesh: fit surface cost on a 50k face export" * doctest::skip())
{
    geo::CubeOptions co;
    co.width = 10.0;
    co.height = 0.4;
    co.depth = 10.0;
    co.subdivX = 40;
    co.subdivZ = 40;
    geo::Mesh m = geo::makeCube(co);
    uv::ProjectOptions po;
    po.type = uv::Projection::Box;
    REQUIRE(uv::project(m, {}, "uv0", po));
    REQUIRE(uv::packIslands(m, {}, "uv0").ok);
    const geo::Mesh bevelled = uv::runStack(m, {bevelSubdivStack().front()});
    auto time = [&](geo::SubdivideUvs uvs) {
        const auto t0 = std::chrono::steady_clock::now();
        const geo::Mesh out = geo::subdivide(bevelled, {.levels = 2, .uvs = uvs});
        MESSAGE("faces " << out.numFaces());
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1000.0;
    };
    const double blend = time(geo::SubdivideUvs::Blend), cold = time(geo::SubdivideUvs::FitSurface), warm = time(geo::SubdivideUvs::FitSurface);
    MESSAGE("subdivide ms: blend " << blend << ", fit surface " << cold << ", fit surface with cached factorisations " << warm);
    const auto t0 = std::chrono::steady_clock::now();
    const uv::FinalMap map = uv::buildFinalMap(m, "uv0", bevelSubdivStack());
    MESSAGE("probe s " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << ", runs " << map.runs << ", entries "
                       << map.cols.size() << ", active " << map.active() << " " << map.warning);
}

TEST_CASE("final mesh: the spacing proxy bunches like the uvs under a bevel, slit tips and box corners included")
{
    // Built at the cage from the surface alone, the proxy's border spacing after the bevel matches the uvs' up to the
    // island's density, where a bevel strip spans a slit's opening as much as anywhere else.
    for (int shape = 0; shape < 3; ++shape)
    {
        geo::Mesh m = shape == 0 ? plate(0.4, 2) : lidPlate(0.4, shape == 1 ? 2 : 6, shape == 2);
        geo::addFitSpacing(m);
        const geo::Mesh b = bevelSubdivStack(0.13, 3, 2).front().apply(m);
        const auto uv = b.cornerAttrs().get<glm::vec2>("uv0");
        const auto px = b.cornerAttrs().get<glm::vec2>("__fitSpacing@uv0");
        REQUIRE(!px.empty());
        auto cornerAt = [&](Index c, Index p) { return b.cornerPoint(c) == p ? c : b.cornerNext(c); };
        auto same = [&](Index a, Index c) { return glm::length(uv[a] - uv[c]) < 1e-6f; };
        std::vector<std::pair<double, double>> edges;
        double su = 0.0, sp = 0.0;
        for (Index f = 0; f < b.faceCapacity(); ++f)
            if (b.faceValid(f))
                b.forEachFaceCorner(f, [&](Index c) {
                    const Index cn = b.cornerNext(c), e = b.cornerEdge(c);
                    if (b.isManifoldEdge(e))
                    {
                        const Index ec = b.edgeCorner(e), rc = b.cornerFace(ec) == f ? b.cornerRadial(ec) : ec;
                        if (same(cornerAt(rc, b.cornerPoint(c)), c) && same(cornerAt(rc, b.cornerPoint(cn)), cn))
                            return;
                    }
                    edges.push_back({glm::length(glm::dvec2(uv[cn]) - glm::dvec2(uv[c])), glm::length(glm::dvec2(px[cn]) - glm::dvec2(px[c]))});
                    su += edges.back().first;
                    sp += edges.back().second;
                });
        size_t off = 0;
        for (auto [lu, lp] : edges)
        {
            const double q = (lp * su / sp + 1e-9) / (lu + 1e-9);
            off += (q > 2.0 || q < 0.5) && std::max(lu, lp * su / sp) > 1e-4;
        }
        INFO("shape " << shape << ", border edges " << edges.size());
        CHECK(off == 0);
    }
}

TEST_CASE("final mesh: a bevel gives one wedge's corners at a point the same uvs, however the layout bends")
{
    // Only the lid's top rim bevelled: strip ends meet corner patches, two blends of the same point that agree only where
    // the layout is flat. A bent layout must not crack the wedge open, or the island splits wherever its layout bends and
    // a Fit Surface map stops being a fixed blend.
    geo::Mesh m = lidPlate(0.4, 3);
    for (glm::vec2& v : m.cornerAttrs().get<glm::vec2>("uv0"))
        v = glm::vec2(v.x + 0.4f * v.y * v.y, v.y + 0.3f * v.x * v.x);
    std::vector<Index> edges;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && uv::dihedralAngle(m, e) > 30.0 && m.position(m.edgePoint(e, 0)).y > 0.0 && m.position(m.edgePoint(e, 1)).y > 0.0)
            edges.push_back(e);
    geo::BevelOptions o;
    o.offset = 0.13;
    o.segments = 3;
    geo::bevelEdges(m, edges, o);
    const std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    const std::span<const bool> seam = m.edgeAttrs().get<bool>(geo::attr::kSeam);
    auto cornerAt = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
    size_t cracks = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || !m.isManifoldEdge(e) || (e < seam.size() && seam[e]))
            continue;
        const Index c = m.edgeCorner(e), rc = m.cornerRadial(c);
        for (int k = 0; k < 2; ++k)
        {
            const Index p = m.edgePoint(e, k);
            cracks += glm::length(uv[cornerAt(c, p)] - uv[cornerAt(rc, p)]) > 1e-6f;
        }
    }
    CHECK(cracks == 0);
}

TEST_CASE("final mesh: cage uvs the export barely reads stay with their neighbours")
{
    // The lid's corner points far from any rim are barely read by the export; free, the solve swings them out as levers
    // (the island's box grows by a quarter) and they land on whatever is packed beside it.
    geo::Mesh m = lidPlate(0.4, 3);
    const std::vector<uv::Chart> before = islandsOf(m);
    const uv::FinalRefineResult r = uv::refineFinal(m, {}, "uv0", bevelSubdivStack(0.13, 3, 2));
    const std::vector<uv::Chart> after = islandsOf(m);
    REQUIRE(before.size() == after.size());
    CHECK(r.collapsedAfter == 0);
    for (size_t i = 0; i < before.size(); ++i)
    {
        const glm::dvec2 sb = before[i].uvMax() - before[i].uvMin(), sa = after[i].uvMax() - after[i].uvMin();
        INFO("island " << i << ", grows " << std::max(sa.x / sb.x, sa.y / sb.y));
        CHECK(std::max(sa.x / sb.x, sa.y / sb.y) < 1.1);
    }
}

namespace
{
/**
 * A stepped block beside the X plane and its mirror image, every face an island of its own sized by its edges, the
 * mirror's islands laid out as the mirror of the block's. Thin faces sit next to big ones, so two levels of
 * subdivision stretch their borders and Fit Surface respaces them.
 */
geo::Mesh mirroredSteps()
{
    const glm::dvec3 points[] = {{2.512, -0.283, 1.45},  {9.503, -0.267, 1.595},  {9.41, 0.275, 1.392},   {2.486, 0.261, 1.49},
                                 {9.536, 0.281, -1.458}, {2.522, 0.26, -1.486},   {9.442, -0.307, -1.552}, {2.546, -0.305, -1.582},
                                 {2.511, 0.574, 1.454},  {9.433, 0.541, 1.605},   {9.614, 0.564, -1.403}, {2.583, 0.54, -1.598},
                                 {2.537, -0.295, 1.667}, {9.434, -0.308, 1.693},  {9.445, 0.283, 1.69},   {2.587, 0.307, 1.648}};
    const Index faces[][4] = {{12, 13, 14, 15}, {8, 9, 10, 11}, {5, 4, 6, 7},  {7, 6, 1, 0},   {1, 6, 4, 2},   {7, 0, 3, 5},   {3, 2, 9, 8},
                              {2, 4, 10, 9},    {4, 5, 11, 10}, {5, 3, 8, 11}, {0, 1, 13, 12}, {1, 2, 14, 13}, {2, 3, 15, 14}, {3, 0, 12, 15}};
    const Index n = Index(std::size(points));
    geo::Mesh m;
    for (const double side : {1.0, -1.0})
        for (const glm::dvec3& p : points)
            m.addPoint(p * glm::dvec3(side, 1.0, 1.0));
    std::vector<std::array<glm::vec2, 4>> uvs;
    float x = 0.0f;
    for (const auto& f : faces)
    {
        const float w = float(glm::length(points[f[1]] - points[f[0]]) * 0.02), h = float(glm::length(points[f[3]] - points[f[0]]) * 0.02);
        uvs.push_back({glm::vec2(x, 0.0f), glm::vec2(x + w, 0.0f), glm::vec2(x + w, h), glm::vec2(x, h)});
        x += w + 0.01f;
    }
    std::vector<Index> made;
    for (const auto& f : faces)
        made.push_back(m.addFace({f[0], f[1], f[2], f[3]}));
    // The mirror turns each face round, so its corners run the other way.
    for (const auto& f : faces)
        made.push_back(m.addFace({f[3] + n, f[2] + n, f[1] + n, f[0] + n}));
    std::span<glm::vec2> uv = m.cornerAttrs().add("uv0", geo::AttrType::Float2).values<glm::vec2>();
    for (size_t k = 0; k < made.size(); ++k)
    {
        const std::vector<Index> fc = m.faceCorners(made[k]);
        const bool mirror = k >= std::size(faces);
        for (size_t a = 0; a < 4; ++a)
        {
            const glm::vec2 v = uvs[k % std::size(faces)][mirror ? 3 - a : a];
            uv[fc[a]] = mirror ? glm::vec2(2.0f - v.x, v.y) : v;
        }
    }
    return m;
}

/** Face corners whose uv turns against their face's winding: a folded or concave face. */
size_t uvFoldedCorners(const geo::Mesh& m)
{
    const std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    size_t folded = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<Index> fc = m.faceCorners(f);
        auto turn = [&](size_t a) {
            const glm::dvec2 p = uv[fc[(a + fc.size() - 1) % fc.size()]], q = uv[fc[a]], r = uv[fc[(a + 1) % fc.size()]];
            return (q.x - p.x) * (r.y - q.y) - (q.y - p.y) * (r.x - q.x);
        };
        double area = 0.0;
        for (size_t a = 0; a < fc.size(); ++a)
            area += turn(a);
        for (size_t a = 0; a < fc.size(); ++a)
            folded += turn(a) * area < -1e-12 ? 1 : 0;
    }
    return folded;
}
} // namespace

TEST_CASE("final mesh: fit surface gives a mirrored island the mirror of its uvs")
{
    const geo::Mesh cage = mirroredSteps();
    const geo::Mesh blend = geo::subdivide(cage, {.levels = 2, .uvs = geo::SubdivideUvs::Blend});
    const geo::Mesh fit = geo::subdivide(cage, {.levels = 2, .uvs = geo::SubdivideUvs::FitSurface});
    REQUIRE(fit.cornerCapacity() == blend.cornerCapacity());
    const std::span<const glm::vec2> ub = blend.cornerAttrs().get<glm::vec2>("uv0"), uf = fit.cornerAttrs().get<glm::vec2>("uv0");

    auto key = [](const glm::dvec3& p) { return std::array<long long, 3>{std::llround(p.x * 1e5), std::llround(p.y * 1e5), std::llround(p.z * 1e5)}; };
    std::map<std::array<long long, 3>, Index> pointAt;
    for (Index p = 0; p < fit.pointCapacity(); ++p)
        if (fit.pointValid(p))
            pointAt[key(fit.position(p))] = p;
    // Per point and face, the corner there: a mirrored corner is the one at the mirrored point of the mirrored face.
    std::map<std::set<Index>, Index> faceOf;
    for (Index f = 0; f < fit.faceCapacity(); ++f)
        if (fit.faceValid(f))
        {
            std::set<Index> pts;
            fit.forEachFaceCorner(f, [&](Index c) { pts.insert(fit.cornerPoint(c)); });
            faceOf[pts] = f;
        }
    double moved = 0.0, worst = 0.0;
    size_t paired = 0;
    for (Index f = 0; f < fit.faceCapacity(); ++f)
    {
        if (!fit.faceValid(f) || fit.position(fit.cornerPoint(fit.faceCorner(f))).x < 0.0)
            continue;
        std::set<Index> pts;
        std::map<Index, Index> mirrorOf;
        bool ok = true;
        fit.forEachFaceCorner(f, [&](Index c) {
            const auto it = pointAt.find(key(fit.position(fit.cornerPoint(c)) * glm::dvec3(-1.0, 1.0, 1.0)));
            ok = ok && it != pointAt.end();
            if (ok)
            {
                pts.insert(it->second);
                mirrorOf[fit.cornerPoint(c)] = it->second;
            }
        });
        REQUIRE(ok);
        REQUIRE(faceOf.count(pts));
        const Index g = faceOf[pts];
        fit.forEachFaceCorner(f, [&](Index c) {
            fit.forEachFaceCorner(g, [&](Index d) {
                if (fit.cornerPoint(d) != mirrorOf[fit.cornerPoint(c)])
                    return;
                const glm::dvec2 a = glm::dvec2(uf[c]) - glm::dvec2(ub[c]), b = glm::dvec2(uf[d]) - glm::dvec2(ub[d]);
                moved = std::max(moved, glm::length(a));
                worst = std::max(worst, glm::length(a - glm::dvec2(-b.x, b.y)));
                ++paired;
            });
        });
    }
    MESSAGE("fit moved uvs by up to " << moved << "; mirrored corners disagree by up to " << worst << " over " << paired << " corners");
    CHECK(paired > 800);
    // The fit has to be doing something here for the comparison to mean anything.
    CHECK(moved > 0.005);
    CHECK(worst < 1e-4);
}

TEST_CASE("final mesh: fit surface never folds a face blend left unfolded")
{
    for (const geo::Mesh& cage : {mirroredSteps(), lidPlate(0.4, 2), plate(0.4, 2)})
    {
        REQUIRE(uvFoldedCorners(geo::subdivide(cage, {.levels = 2, .uvs = geo::SubdivideUvs::Blend})) == 0);
        CHECK(uvFoldedCorners(geo::subdivide(cage, {.levels = 2, .uvs = geo::SubdivideUvs::FitSurface})) == 0);
    }
}
