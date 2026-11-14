#include "uv/Uv.h"

#include "core/Profile.h"
#include "geo/Topology.h"
#include "uv/Layout.h"
#include "uv/Metrics.h"
#include "uv/UvCommon.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace lila::uv
{
namespace
{
using detail::usable;

size_t pinCount(const Chart& ch)
{
    size_t n = 0;
    for (uint8_t p : ch.pinned)
        n += p ? 1 : 0;
    return n;
}

/** Turns the chart about its centroid to best match a previous layout of the same vertices. */
void matchRotation(Chart& ch, const std::vector<glm::dvec2>& old)
{
    glm::dvec2 cn(0.0), co(0.0);
    for (size_t i = 0; i < old.size(); ++i)
    {
        cn += ch.uv[i];
        co += old[i];
    }
    cn /= double(old.size());
    co /= double(old.size());
    double s = 0.0, c = 0.0;
    for (size_t i = 0; i < old.size(); ++i)
    {
        glm::dvec2 a = ch.uv[i] - cn, b = old[i] - co;
        s += a.x * b.y - a.y * b.x;
        c += glm::dot(a, b);
    }
    if (s == 0.0 && c == 0.0)
        return;
    transformChart(ch, cn, std::atan2(s, c));
}

/** Uv area over surface area of the mesh's faces outside the selection: the square of their texel density. 0 without any. */
double densityOutside(const geo::Mesh& mesh, std::string_view uvSet, std::span<const uint8_t> selected)
{
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(uvSet);
    if (uv.empty())
        return 0.0;
    double uvArea = 0.0, surface = 0.0;
    std::vector<Index> corners;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f) || selected[f])
            continue;
        corners.clear();
        mesh.forEachFaceCorner(f, [&](Index c) { corners.push_back(c); });
        double a = 0.0;
        for (size_t k = 1; k + 1 < corners.size(); ++k)
        {
            glm::dvec2 e1 = glm::dvec2(uv[corners[k]] - uv[corners[0]]), e2 = glm::dvec2(uv[corners[k + 1]] - uv[corners[0]]);
            a += e1.x * e2.y - e1.y * e2.x;
            glm::dvec3 p0 = mesh.position(mesh.cornerPoint(corners[0]));
            surface += 0.5 * glm::length(glm::cross(mesh.position(mesh.cornerPoint(corners[k])) - p0, mesh.position(mesh.cornerPoint(corners[k + 1])) - p0));
        }
        uvArea += 0.5 * std::abs(a);
    }
    return surface > 0.0 ? uvArea / surface : 0.0;
}

std::string& currentSet()
{
    static std::string name = "uv0";
    return name;
}

/** True when the charts kept where they are (all but those to pack) overlap each other in uv. */
bool keptOverlap(std::span<const Chart> charts, std::span<Chart* const> toPack)
{
    std::vector<std::array<glm::dvec2, 3>> tris;
    std::vector<Index> group;
    for (size_t i = 0; i < charts.size(); ++i)
    {
        if (std::ranges::find(toPack, &charts[i]) != toPack.end())
            continue;
        for (const auto& t : charts[i].tris)
        {
            tris.push_back({charts[i].uv[t[0]], charts[i].uv[t[1]], charts[i].uv[t[2]]});
            group.push_back(Index(i));
        }
    }
    std::vector<uint8_t> flags;
    return triangleOverlaps(tris, flags, group, true) > 0;
}
} // namespace

std::vector<Index> uvCornersOfFaces(const geo::Mesh& mesh, std::string_view uvSet, std::span<const Index> faces)
{
    std::vector<Index> corners;
    for (Index f : liveFaces(mesh, faces))
        mesh.forEachFaceCorner(f, [&](Index c) { corners.push_back(c); });
    return geo::uvVertexCorners(mesh, uvSet, corners);
}

std::vector<Index> cornersOfPoints(const geo::Mesh& mesh, std::span<const Index> points)
{
    std::vector<Index> out;
    for (Index p : points)
        if (mesh.pointValid(p))
            mesh.forEachPointCorner(p, [&](Index c) { out.push_back(c); });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const std::string& currentUvSet()
{
    return currentSet();
}

void setCurrentUvSet(std::string name)
{
    currentSet() = name.empty() ? std::string("uv0") : std::move(name);
}

UnwrapFacesResult unwrapFaces(geo::Mesh& mesh, std::span<const Index> faceIds, std::string_view uvSet, const UnwrapFacesOptions& opts)
{
    LILA_ZONE();

    UnwrapFacesResult res;
    std::vector<Index> faces = liveFaces(mesh, faceIds);
    if (faces.empty())
        return res;
    if (opts.openIslands)
        res.seamsAdded = cutToDisks(mesh, faces);
    ChartOptions co;
    co.uvSet = uvSet;
    std::vector<Chart> charts = buildCharts(mesh, faces, co);
    res.islands = charts.size();
    if (!opts.openIslands)
    {
        for (const Chart& ch : charts)
            res.closed += ch.boundaryLoops().size() != 1 || ch.euler() != 1;
        if (res.closed)
            return res;
    }
    std::vector<uint8_t> selected(mesh.faceCapacity(), 0);
    for (Index f : faces)
        selected[f] = 1;
    double uvArea = densityOutside(mesh, uvSet, selected), surface = uvArea > 0.0 ? 1.0 : 0.0;

    std::vector<uint8_t> ok(charts.size(), 0), hadUvs(charts.size(), 0);
    std::vector<std::vector<glm::dvec2>> old(charts.size());
    std::vector<glm::dvec2> oldCentroid(charts.size());
    std::vector<double> oldArea(charts.size());
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        Chart& ch = charts[i];
        hadUvs[i] = usable(ch) ? 1 : 0;
        old[i] = ch.uv;
        oldCentroid[i] = ch.uvCentroid();
        oldArea[i] = std::abs(ch.areaUv());
        ok[i] = unwrapChart(ch, opts.unwrap).ok ? 1 : 0;
        if (!ok[i])
            ch.uv = old[i];
    });
    for (size_t i = 0; i < charts.size(); ++i)
        if (hadUvs[i])
        {
            uvArea += oldArea[i];
            surface += charts[i].area3d();
        }
    const double density = surface > 0.0 ? uvArea / surface : 0.0;

    std::vector<Chart*> toPack;
    for (size_t i = 0; i < charts.size(); ++i)
    {
        if (!ok[i])
        {
            ++res.failed;
            continue;
        }
        Chart& ch = charts[i];
        if (opts.unwrap.usePins && pinCount(ch) >= 2)
            continue;
        if (!opts.pack && opts.keepPlacement && hadUvs[i])
        {
            matchRotation(ch, old[i]);
            fitChart(ch, oldCentroid[i], oldArea[i]);
        }
        else if (!opts.pack && opts.keepPlacement && density > 0.0)
        {
            // Folded or collapsed before: the island comes out at the density of the uvs around it, in place when it had a place.
            glm::dvec2 lo(1e300), hi(-1e300);
            for (const glm::dvec2& p : old[i])
                lo = glm::min(lo, p), hi = glm::max(hi, p);
            const bool placed = glm::length(hi - lo) > 1e-9;
            fitChart(ch, placed ? oldCentroid[i] : ch.uvCentroid(), density * ch.area3d());
            if (!placed)
                toPack.push_back(&ch);
        }
        else
            toPack.push_back(&ch);
    }
    if (!opts.pack && opts.keepPlacement && toPack.size() < charts.size() && keptOverlap(charts, toPack))
    {
        toPack.clear();
        for (size_t i = 0; i < charts.size(); ++i)
            if (ok[i] && !(opts.unwrap.usePins && pinCount(charts[i]) >= 2))
                toPack.push_back(&charts[i]);
    }
    if (!toPack.empty())
    {
        std::vector<Chart> packed;
        packed.reserve(toPack.size());
        for (Chart* c : toPack)
            packed.push_back(std::move(*c));
        PackOptions po = opts.packing;
        po.lockScale = po.lockScale || (!opts.pack && opts.keepPlacement && density > 0.0);
        packCharts(packed, po);
        for (size_t i = 0; i < toPack.size(); ++i)
            *toPack[i] = std::move(packed[i]);
    }
    writeCharts(mesh, charts, uvSet);
    return res;
}

AutoUvResult autoUv(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const AutoUvOptions& opts)
{
    LILA_ZONE();

    AutoUvResult res;
    AutoSeamResult seams = autoSeams(mesh, faces, opts.seams);
    res.seams = seams.seamEdges;
    if (const Index added = opts.afterSeams ? opts.afterSeams(mesh, faces) : 0)
    {
        res.seams += added;
        seams.charts = buildCharts(mesh, faces);
        // Unwrapped from scratch below: the old uvs belong to other islands.
        for (Chart& ch : seams.charts)
            std::fill(ch.uv.begin(), ch.uv.end(), glm::dvec2(0.0));
    }
    std::vector<Chart>& charts = seams.charts;
    res.islands = charts.size();
    UnwrapOptions uo = opts.unwrap;
    uo.usePins = false;
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        Chart& ch = charts[i];
        std::fill(ch.pinned.begin(), ch.pinned.end(), uint8_t(0));
        // Islands come back already flattened once; refine those, start over on the rest.
        if (!(uo.method == UnwrapMethod::Slim && usable(ch) && slim(ch, uo.iterations, false) >= 0))
            unwrapChart(ch, uo);
        // A fresh island's turn is arbitrary: its edge grid, its flow or its bounding box on the axes.
        orientChart(ch, OrientMode::Upright);
    });
    res.pack = packCharts(charts, opts.pack);
    writeCharts(mesh, charts, uvSet);
    return res;
}

LightmapResult generateLightmapUvs(geo::Mesh& mesh, const LightmapOptions& opts)
{
    LILA_ZONE();

    LightmapResult res;
    std::vector<Chart> charts;
    if (!mesh.cornerAttrs().get<glm::vec2>(opts.source).empty())
    {
        ChartOptions co;
        co.source = IslandSource::Uvs;
        co.uvSet = opts.source;
        charts = buildCharts(mesh, {}, co);
    }
    else
    {
        // Seams are worked out on a copy so the mesh's own seams stay as they are.
        geo::Mesh scratch = mesh;
        charts = autoSeams(scratch).charts;
    }
    res.islands = charts.size();
    std::atomic<size_t> rewrapped{0};
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        Chart& ch = charts[i];
        std::fill(ch.pinned.begin(), ch.pinned.end(), uint8_t(0));
        if (usable(ch) && !chartOverlaps(ch))
            return;
        if (ch.boundaryLoops().size() == 1 && ch.euler() == 1 && unwrapChart(ch).ok)
            ++rewrapped;
    });
    res.rewrapped = rewrapped;

    PackOptions po;
    po.resolution = std::max(4, opts.resolution);
    po.padding = opts.padding;
    po.margin = opts.padding * 0.5;
    po.rotateStep = opts.rotateStep;
    po.effort = opts.effort;
    po.normalizeScale = true;
    po.workingResolution = std::min(po.resolution, 4096);
    res.pack = packCharts(charts, po);
    if (!res.pack.ok)
        return res;
    writeCharts(mesh, charts, opts.target);
    res.ok = true;
    return res;
}
} // namespace lila::uv
