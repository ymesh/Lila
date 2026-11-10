#include "retopo/Construct.h"

#include "core/Profile.h"
#include "geo/CrossSection.h"
#include "geo/Ops.h"
#include "retopo/Patch.h"
#include "retopo/Retopo.h"
#include "retopo/SurfaceStroke.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/rotate_vector.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace lila::retopo
{
namespace
{
glm::dvec3 closest(const GenContext& ctx, const glm::dvec3& p, Index* face = nullptr)
{
    geo::BvhClosest c;
    if (!ctx.surface || ctx.surface->empty() || !ctx.surface->closestPoint(p, c))
        return p;
    if (face)
        *face = c.face;
    return c.point;
}

/** The surface normal under p, else fallback. */
glm::dvec3 normalAt(const GenContext& ctx, const glm::dvec3& p, const glm::dvec3& fallback)
{
    Index f = geo::kInvalid;
    closest(ctx, p, &f);
    if (!ctx.surfaceMesh || f == geo::kInvalid || f >= ctx.surfaceMesh->faceCapacity() || !ctx.surfaceMesh->faceValid(f))
        return fallback;
    std::vector<Index> pts = ctx.surfaceMesh->facePoints(f);
    glm::dvec3 n(0.0);
    for (size_t k = 0; k < pts.size(); ++k)
        n += glm::cross(ctx.surfaceMesh->position(pts[k]), ctx.surfaceMesh->position(pts[(k + 1) % pts.size()]));
    double len = glm::length(n);
    return len > 0.0 ? n / len : fallback;
}

int countFor(double length, double edge) { return std::max(1, int(std::lround(length / std::max(edge, 1e-9)))); }

/** Splits total into parts proportional to weights, each at least 1, by largest remainder. */
std::vector<int> apportion(const std::vector<double>& weights, int total)
{
    const size_t n = weights.size();
    std::vector<int> out(n, 1);
    total = std::max(total, int(n));
    double sum = std::accumulate(weights.begin(), weights.end(), 0.0);
    if (sum <= 0.0)
    {
        for (size_t i = 0; i < n; ++i)
            out[i] = total / int(n) + (int(i) < total % int(n) ? 1 : 0);
        return out;
    }
    std::vector<double> rem(n);
    int used = 0;
    for (size_t i = 0; i < n; ++i)
    {
        double exact = weights[i] / sum * total;
        out[i] = std::max(1, int(std::floor(exact)));
        rem[i] = exact - out[i];
        used += out[i];
    }
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return rem[a] > rem[b]; });
    for (size_t k = 0; used < total; k = (k + 1) % n, ++used)
        ++out[order[k]];
    for (size_t k = n; used > total && k-- > 0;)
        if (out[order[k]] > 1)
        {
            --out[order[k]];
            --used;
        }
    return out;
}

/** Centripetal Catmull-Rom point between p1 and p2 at t in [0, 1]. */
glm::dvec3 catmullRom(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& p2, const glm::dvec3& p3, double t)
{
    auto knot = [](double ti, const glm::dvec3& a, const glm::dvec3& b) { return ti + std::max(1e-9, std::sqrt(glm::length(b - a))); };
    double t0 = 0.0, t1 = knot(t0, p0, p1), t2 = knot(t1, p1, p2), t3 = knot(t2, p2, p3);
    double u = t1 + (t2 - t1) * t;
    glm::dvec3 a1 = (t1 - u) / (t1 - t0) * p0 + (u - t0) / (t1 - t0) * p1;
    glm::dvec3 a2 = (t2 - u) / (t2 - t1) * p1 + (u - t1) / (t2 - t1) * p2;
    glm::dvec3 a3 = (t3 - u) / (t3 - t2) * p2 + (u - t2) / (t3 - t2) * p3;
    glm::dvec3 b1 = (t2 - u) / (t2 - t0) * a1 + (u - t0) / (t2 - t0) * a2;
    glm::dvec3 b2 = (t3 - u) / (t3 - t1) * a2 + (u - t1) / (t3 - t1) * a3;
    return (t2 - u) / (t2 - t1) * b1 + (u - t1) / (t2 - t1) * b2;
}

GenResult genPatch(const PatchSpec& s, const GenContext& ctx)
{
    GenResult r;
    if (s.sides.size() < 2 || s.sides.size() > 6)
    {
        r.error = "a patch needs 2 to 6 sides";
        return r;
    }
    std::vector<int> counts(s.sides.size());
    for (size_t i = 0; i < s.sides.size(); ++i)
    {
        const PatchSide& side = s.sides[i];
        if (side.points.size() < 2)
        {
            r.error = "a patch side has fewer than two points";
            return r;
        }
        counts[i] = side.fixed ? int(side.points.size()) - 1 : side.count > 0 ? side.count : countFor(polylineLength(side.points), ctx.edgeLength);
    }
    if (std::accumulate(counts.begin(), counts.end(), 0) % 2 != 0)
    {
        int best = -1;
        double bestRatio = -1.0;
        for (size_t i = 0; i < s.sides.size(); ++i)
            if (!s.sides[i].fixed)
                if (double ratio = polylineLength(s.sides[i].points) / counts[i]; ratio > bestRatio)
                {
                    bestRatio = ratio;
                    best = int(i);
                }
        if (best < 0)
        {
            r.error = "the border has an odd number of edges, so it can't be filled with quads; add or remove one";
            return r;
        }
        ++counts[best];
    }

    PatchBoundary b;
    std::vector<uint8_t> fixed;
    for (size_t i = 0; i < s.sides.size(); ++i)
    {
        const PatchSide& side = s.sides[i];
        std::vector<glm::dvec3> pts = side.fixed ? side.points : resamplePolyline(side.points, counts[i], false, ctx.surface);
        b.corners.push_back(int(b.points.size()));
        for (size_t k = 0; k + 1 < pts.size(); ++k)
        {
            b.points.push_back(pts[k]);
            bool corner = k == 0;
            fixed.push_back(side.fixed || (corner && s.sides[(i + s.sides.size() - 1) % s.sides.size()].fixed));
        }
    }
    PatchOptions o;
    o.padding = s.padding;
    o.variant = s.variant;
    o.pull = s.pull;
    o.lift = s.lift;
    PatchResult p = fillPatch(b, o, ctx.surface);
    if (!p.ok())
    {
        r.error = p.error;
        return r;
    }
    r.positions = std::move(p.positions);
    r.faces = std::move(p.faces);
    r.poles = std::move(p.poles);
    r.variants = p.variants;
    r.sideCounts = counts;
    r.fixed.assign(r.positions.size(), 0);
    std::copy(fixed.begin(), fixed.end(), r.fixed.begin());
    return r;
}

/** A strip's centre line, densely sampled: positions with the knot parameter (knot index plus fraction) of each. */
struct Centre
{
    std::vector<glm::dvec3> points;
    std::vector<double> param;
};

Centre stripCentre(const StripSpec& s, const GenContext& ctx)
{
    Centre c;
    const int n = int(s.knots.size());
    const int segs = s.closed ? n : n - 1;
    auto corner = [&](int i) { return i >= 0 && i < int(s.corners.size()) && s.corners[i]; };
    auto knot = [&](int i) {
        if (s.closed)
            return s.knots[((i % n) + n) % n];
        return s.knots[std::clamp(i, 0, n - 1)];
    };
    const int steps = 12;
    for (int i = 0; i < segs; ++i)
    {
        glm::dvec3 p1 = knot(i), p2 = knot(i + 1);
        // A corner (or an open end) stops the curve from reaching across it: mirror the point beyond.
        glm::dvec3 p0 = (!s.closed && i == 0) || corner(i) ? 2.0 * p1 - p2 : knot(i - 1);
        glm::dvec3 p3 = (!s.closed && i + 1 == n - 1) || corner((i + 1) % n) ? 2.0 * p2 - p1 : knot(i + 2);
        for (int k = 0; k < steps; ++k)
        {
            double t = double(k) / steps;
            c.points.push_back(closest(ctx, catmullRom(p0, p1, p2, p3, t)));
            c.param.push_back(i + t);
        }
    }
    if (!s.closed)
    {
        c.points.push_back(s.knots.back());
        c.param.push_back(n - 1);
    }
    return c;
}

GenResult genStrip(const StripSpec& s, const GenContext& ctx)
{
    LILA_ZONE();

    GenResult r;
    const int n = int(s.knots.size());
    if (n < 2 || (s.closed && n < 3))
    {
        r.error = "a strip needs at least two knots";
        return r;
    }
    const int rows = std::max(1, s.rows);
    Centre c = stripCentre(s, ctx);
    const size_t m = c.points.size();
    std::vector<double> arc(m + 1, 0.0);
    for (size_t i = 1; i <= m; ++i)
        arc[i] = arc[i - 1] + (i < m ? glm::length(c.points[i] - c.points[i - 1]) : s.closed ? glm::length(c.points[0] - c.points[m - 1]) : 0.0);
    const double total = s.closed ? arc[m] : arc[m - 1];

    // Rows of points fall exactly on corner knots, so corners stay sharp; quads are split between corner spans by length.
    std::vector<int> breaks;
    for (int i = 0; i < n; ++i)
        if ((i < int(s.corners.size()) && s.corners[i]) || (!s.closed && (i == 0 || i == n - 1)))
            breaks.push_back(i);
    if (s.closed && breaks.empty())
        breaks.push_back(0);
    auto knotArc = [&](int k) {
        for (size_t i = 0; i < m; ++i)
            if (c.param[i] >= k - 1e-9)
                return arc[i];
        return total;
    };
    std::vector<double> spanFrom, spanLen;
    const size_t spans = s.closed ? breaks.size() : breaks.size() - 1;
    for (size_t i = 0; i < spans; ++i)
    {
        double a = knotArc(breaks[i]);
        double b = i + 1 < breaks.size() ? knotArc(breaks[i + 1]) : total + knotArc(breaks[0]);
        spanFrom.push_back(a);
        spanLen.push_back(b - a);
    }
    double quadLength = s.width / rows;
    int along = s.divisions > 0 ? s.divisions : countFor(total, quadLength);
    std::vector<int> per = apportion(spanLen, along);

    auto sampleAt = [&](double at, double& param) {
        if (s.closed)
            at = std::fmod(std::fmod(at, total) + total, total);
        else
            at = std::clamp(at, 0.0, total);
        size_t i = size_t(std::upper_bound(arc.begin(), arc.begin() + m, at) - arc.begin());
        i = std::clamp<size_t>(i, 1, m);
        double len = arc[i] - arc[i - 1];
        double t = len > 0.0 ? (at - arc[i - 1]) / len : 0.0;
        const glm::dvec3& a = c.points[i - 1];
        const glm::dvec3& b = i < m ? c.points[i] : c.points[0];
        double pb = i < m ? c.param[i] : double(n);
        param = glm::mix(c.param[i - 1], pb, t);
        return glm::mix(a, b, t);
    };
    struct Row
    {
        glm::dvec3 centre;
        double param;
        bool corner;
    };
    std::vector<Row> rowsAlong;
    for (size_t sp = 0; sp < spans; ++sp)
        for (int k = 0; k < per[sp]; ++k)
        {
            Row row;
            row.centre = sampleAt(spanFrom[sp] + spanLen[sp] * k / per[sp], row.param);
            row.corner = k == 0;
            rowsAlong.push_back(row);
        }
    if (!s.closed)
    {
        double param = 0.0;
        rowsAlong.push_back({sampleAt(total, param), double(n - 1), true});
    }
    else
    {
        // Spans started at the first break, so the loop's rows are already in order around it.
    }

    const size_t count = rowsAlong.size();
    auto knotValue = [&](const std::vector<double>& v, double param, double fallback) {
        if (v.empty())
            return fallback;
        int i = int(std::floor(param));
        double t = param - i;
        auto at = [&](int k) {
            k = s.closed ? ((k % n) + n) % n : std::clamp(k, 0, n - 1);
            return k < int(v.size()) ? v[k] : fallback;
        };
        return glm::mix(at(i), at(i + 1), t);
    };
    for (size_t i = 0; i < count; ++i)
    {
        const Row& row = rowsAlong[i];
        glm::dvec3 prev = i > 0 ? rowsAlong[i - 1].centre : s.closed ? rowsAlong[count - 1].centre : row.centre;
        glm::dvec3 next = i + 1 < count ? rowsAlong[i + 1].centre : s.closed ? rowsAlong[0].centre : row.centre;
        glm::dvec3 tin = row.centre - prev, tout = next - row.centre;
        if (glm::length(tin) < 1e-12)
            tin = tout;
        if (glm::length(tout) < 1e-12)
            tout = tin;
        tin = glm::normalize(tin);
        tout = glm::normalize(tout);
        glm::dvec3 tangent = glm::length(tin + tout) > 1e-9 ? glm::normalize(tin + tout) : tout;
        glm::dvec3 normal = normalAt(ctx, row.centre, glm::dvec3(0.0, 1.0, 0.0));
        glm::dvec3 cross = glm::cross(normal, tangent);
        if (glm::length(cross) < 1e-12)
            cross = glm::cross(glm::dvec3(0.0, 0.0, 1.0), tangent);
        cross = glm::normalize(cross);
        double twist = knotValue(s.twists, row.param, 0.0);
        if (twist != 0.0)
            cross = glm::rotate(cross, glm::radians(twist), normal);
        double half = s.width * 0.5 * knotValue(s.scales, row.param, 1.0);
        // A mitre keeps the strip's width through a corner.
        if (row.corner)
            half /= std::max(0.35, glm::dot(tangent, tout));
        for (int j = 0; j <= rows; ++j)
        {
            glm::dvec3 p = row.centre + cross * (-half + 2.0 * half * j / rows);
            r.positions.push_back(closest(ctx, p));
        }
    }
    r.fixed.assign(r.positions.size(), 0);
    const size_t stride = size_t(rows) + 1;
    auto joinRow = [&](size_t row, const std::array<glm::dvec3, 2>& edge) {
        glm::dvec3& a = r.positions[row * stride];
        glm::dvec3& b = r.positions[row * stride + 1];
        bool swap = glm::length(a - edge[1]) + glm::length(b - edge[0]) < glm::length(a - edge[0]) + glm::length(b - edge[1]);
        a = swap ? edge[1] : edge[0];
        b = swap ? edge[0] : edge[1];
        r.fixed[row * stride] = r.fixed[row * stride + 1] = 1;
    };
    if (rows == 1 && !s.closed)
    {
        if (s.startRow)
            joinRow(0, *s.startRow);
        if (s.endRow)
            joinRow(count - 1, *s.endRow);
    }
    const size_t quadsAlong = s.closed ? count : count - 1;
    r.sideCounts = {int(quadsAlong), rows};
    for (size_t i = 0; i < quadsAlong; ++i)
    {
        size_t a = i, b = (i + 1) % count;
        for (int j = 0; j < rows; ++j)
            r.faces.push_back({Index(a * stride + j), Index(a * stride + j + 1), Index(b * stride + j + 1), Index(b * stride + j)});
    }
    return r;
}

GenResult genBridge(const BridgeSpec& s, const GenContext& ctx)
{
    GenResult r;
    if (s.a.size() < 2 || s.b.size() < 2)
    {
        r.error = "a bridge needs two sides of at least two points";
        return r;
    }
    std::vector<glm::dvec3> a = s.a, b = s.b;
    bool aFixed = s.aFixed, bFixed = s.bFixed;
    // Same direction along both sides: compare the end pairings.
    double straight = glm::length(a.front() - b.front()) + glm::length(a.back() - b.back());
    double crossed = glm::length(a.front() - b.back()) + glm::length(a.back() - b.front());
    if (crossed < straight)
        std::reverse(b.begin(), b.end());
    int segments = 0;
    if (aFixed && bFixed)
    {
        if (a.size() != b.size())
        {
            r.error = "the two borders have different numbers of edges (" + std::to_string(a.size() - 1) + " and " + std::to_string(b.size() - 1) + ")";
            return r;
        }
        segments = int(a.size()) - 1;
    }
    else if (aFixed)
        segments = int(a.size()) - 1;
    else if (bFixed)
        segments = int(b.size()) - 1;
    else
        segments = s.divisions > 0 ? s.divisions : countFor((polylineLength(a) + polylineLength(b)) * 0.5, ctx.edgeLength);
    if (!aFixed)
        a = resamplePolyline(a, segments, false, ctx.surface);
    if (!bFixed)
        b = resamplePolyline(b, segments, false, ctx.surface);
    double gap = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        gap += glm::length(a[i] - b[i]) / double(a.size());
    double along = (polylineLength(a) + polylineLength(b)) * 0.5 / segments;
    int rows = s.rows > 0 ? s.rows : countFor(gap, along);
    r = gridBetween(a, b, rows, ctx.surface);
    if (!r.ok())
        return r;
    r.sideCounts = {segments, rows};
    const size_t n = a.size();
    for (size_t i = 0; i < n; ++i)
    {
        r.fixed[i] = aFixed;
        r.fixed[size_t(rows) * n + i] = bFixed;
    }
    return r;
}

GenResult genSweep(const SweepSpec& s, const GenContext& ctx)
{
    GenResult r;
    const int n = int(s.border.size());
    if (n < 2 || s.anchor < 0 || s.anchor >= n || s.path.size() < 2 || s.span == 0)
    {
        r.error = "a sweep needs a border, an anchor on it and a path";
        return r;
    }
    std::vector<glm::dvec3> run{s.border[s.anchor]};
    int step = s.span > 0 ? 1 : -1;
    for (int k = 1; k <= std::abs(s.span); ++k)
    {
        int i = s.anchor + step * k;
        if (s.borderClosed)
            i = ((i % n) + n) % n;
        else if (i < 0 || i >= n)
            break;
        if (s.borderClosed && i == s.anchor)
            break;
        run.push_back(s.border[i]);
    }
    if (run.size() < 2)
    {
        r.error = "the border ends at the anchor on that side";
        return r;
    }
    double edge = polylineLength(run) / double(run.size() - 1);
    int rows = s.rows > 0 ? s.rows : countFor(polylineLength(s.path), edge);
    std::vector<glm::dvec3> path = resamplePolyline(s.path, rows, false, ctx.surface);
    path.front() = run.front();
    r.sideCounts = {rows};
    const size_t w = run.size();
    for (int i = 0; i <= rows; ++i)
        for (size_t j = 0; j < w; ++j)
            r.positions.push_back(i == 0 ? run[j] : j == 0 ? path[i] : closest(ctx, run[j] + (path[i] - path[0])));
    r.fixed.assign(r.positions.size(), 0);
    for (size_t j = 0; j < w; ++j)
        r.fixed[j] = 1;
    for (int i = 0; i < rows; ++i)
        for (size_t j = 0; j + 1 < w; ++j)
            r.faces.push_back({Index(i * w + j), Index(i * w + j + 1), Index((i + 1) * w + j + 1), Index((i + 1) * w + j)});
    return r;
}

/** A unit vector perpendicular to n, from the world axis least aligned with it. */
glm::dvec3 perpendicular(const glm::dvec3& n)
{
    glm::dvec3 axis = std::abs(n.x) < std::abs(n.y) ? (std::abs(n.x) < std::abs(n.z) ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1))
                                                     : (std::abs(n.y) < std::abs(n.z) ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1));
    return glm::normalize(axis - n * glm::dot(axis, n));
}

GenResult genCylinder(const CylinderSpec& s, const GenContext& ctx)
{
    LILA_ZONE();

    GenResult r;
    if (!ctx.surfaceMesh || s.rings.empty())
    {
        r.error = "a cylinder needs a reference surface and at least one ring";
        return r;
    }
    const int points = std::max(3, s.points);
    std::vector<std::vector<glm::dvec3>> rings;
    std::vector<glm::dvec3> centres;
    glm::dvec3 startDir(0.0);
    for (size_t k = 0; k < s.rings.size(); ++k)
    {
        const CylinderRing& ring = s.rings[k];
        glm::dvec3 normal = glm::length(ring.normal) > 0.0 ? glm::normalize(ring.normal) : glm::dvec3(0, 1, 0);
        geo::SectionMesh sm{ctx.surfaceMesh, glm::dmat4(1.0)};
        std::vector<geo::SectionContour> contours = geo::crossSections(std::span(&sm, 1), ring.origin, normal);
        const geo::SectionContour* best = nullptr;
        double bestD = 0.0;
        for (const geo::SectionContour& c : contours)
            for (const glm::dvec3& p : c.points)
                if (double d = glm::length(p - ring.pick); !best || d < bestD)
                {
                    best = &c;
                    bestD = d;
                }
        if (!best || best->points.size() < 3)
        {
            r.error = "ring " + std::to_string(k + 1) + " doesn't cut the reference";
            return r;
        }
        std::vector<glm::dvec3> loop = best->points;
        glm::dvec3 centre = best->centroid;
        glm::dvec3 area(0.0);
        for (size_t i = 0; i < loop.size(); ++i)
            area += glm::cross(loop[i] - centre, loop[(i + 1) % loop.size()] - centre);
        if (glm::dot(area, k == 0 ? normal : glm::normalize(centre - centres.back() + normal * 1e-9)) < 0.0)
            std::reverse(loop.begin(), loop.end());
        if (k == 0)
            startDir = glm::rotate(perpendicular(normal), glm::radians(s.rotate), normal);
        else
        {
            startDir -= normal * glm::dot(startDir, normal);
            startDir = glm::length(startDir) > 1e-12 ? glm::normalize(startDir) : perpendicular(normal);
            if (s.twist != 0.0)
                startDir = glm::rotate(startDir, glm::radians(s.twist), normal);
        }
        int start = 0;
        double bestDot = -1e300;
        for (size_t i = 0; i < loop.size(); ++i)
            if (double d = glm::dot(glm::normalize(loop[i] - centre), startDir); d > bestDot)
            {
                bestDot = d;
                start = int(i);
            }
        std::rotate(loop.begin(), loop.begin() + start, loop.end());
        rings.push_back(resamplePolyline(loop, points, true, nullptr));
        centres.push_back(centre);
    }
    // Every ring winds the same way round the cylinder's run, judged against the previous ring.
    for (size_t k = 1; k < rings.size(); ++k)
    {
        auto cost = [&](const std::vector<glm::dvec3>& ring) {
            double c = 0.0;
            for (size_t i = 0; i < ring.size(); ++i)
                c += glm::length((ring[i] - centres[k]) - (rings[k - 1][i] - centres[k - 1]));
            return c;
        };
        std::vector<glm::dvec3> flipped(rings[k].size());
        for (size_t i = 0; i < rings[k].size(); ++i)
            flipped[i] = rings[k][(rings[k].size() - i) % rings[k].size()];
        if (cost(flipped) < cost(rings[k]))
            rings[k] = flipped;
    }

    auto addRing = [&](const std::vector<glm::dvec3>& ring) {
        for (const glm::dvec3& p : ring)
            r.positions.push_back(p);
    };
    std::vector<size_t> ringStart;
    for (size_t k = 0; k < rings.size(); ++k)
    {
        if (k > 0)
        {
            double edge = (polylineLength(rings[k - 1], true) + polylineLength(rings[k], true)) * 0.5 / points;
            int rows = std::max(1, int(std::lround(glm::length(centres[k] - centres[k - 1]) / std::max(edge, 1e-9) * std::max(0.05, s.rowDensity))));
            for (int j = 1; j < rows; ++j)
            {
                std::vector<glm::dvec3> mid(points);
                for (int i = 0; i < points; ++i)
                    mid[i] = closest(ctx, glm::mix(rings[k - 1][i], rings[k][i], double(j) / rows));
                ringStart.push_back(r.positions.size());
                addRing(mid);
            }
        }
        ringStart.push_back(r.positions.size());
        addRing(rings[k]);
    }
    for (size_t k = 0; k + 1 < ringStart.size(); ++k)
        for (int i = 0; i < points; ++i)
        {
            Index a = Index(ringStart[k] + i), b = Index(ringStart[k] + (i + 1) % points);
            Index c = Index(ringStart[k + 1] + (i + 1) % points), d = Index(ringStart[k + 1] + i);
            r.faces.push_back({a, b, c, d});
        }
    auto cap = [&](size_t start) {
        if (points % 2 != 0)
        {
            r.error = "capping needs an even number of points around";
            return;
        }
        PatchBoundary b;
        for (int i = 0; i < points; ++i)
            b.points.push_back(r.positions[start + i]);
        for (int i = 0; i < 4; ++i)
            b.corners.push_back(i * points / 4);
        std::sort(b.corners.begin(), b.corners.end());
        b.corners.erase(std::unique(b.corners.begin(), b.corners.end()), b.corners.end());
        PatchResult p = fillPatch(b, {}, ctx.surface);
        if (!p.ok())
        {
            r.error = "cap: " + p.error;
            return;
        }
        std::vector<Index> map(p.positions.size());
        for (size_t i = 0; i < p.positions.size(); ++i)
        {
            if (i < size_t(points))
                map[i] = Index(start + i);
            else
            {
                map[i] = Index(r.positions.size());
                r.positions.push_back(p.positions[i]);
            }
        }
        for (const std::vector<Index>& f : p.faces)
        {
            std::vector<Index> face;
            for (Index q : f)
                face.push_back(map[q]);
            r.faces.push_back(face);
        }
    };
    if (s.capStart)
        cap(ringStart.front());
    if (s.capEnd && ringStart.size() > 1)
        cap(ringStart.back());
    r.fixed.assign(r.positions.size(), 0);
    return r;
}
} // namespace

GenResult generateRegion(const RegionSpec& s, const GenContext& ctx);

GenResult generate(const ConstructSpec& spec, const GenContext& ctx)
{
    LILA_ZONE();

    return std::visit(
        [&](const auto& s) -> GenResult {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, PatchSpec>)
                return genPatch(s, ctx);
            else if constexpr (std::is_same_v<T, StripSpec>)
                return genStrip(s, ctx);
            else if constexpr (std::is_same_v<T, StrokeSpec>)
                return GenResult{};
            else if constexpr (std::is_same_v<T, BridgeSpec>)
                return genBridge(s, ctx);
            else if constexpr (std::is_same_v<T, SweepSpec>)
                return genSweep(s, ctx);
            else if constexpr (std::is_same_v<T, CylinderSpec>)
                return genCylinder(s, ctx);
            else
                return generateRegion(s, ctx);
        },
        spec);
}
} // namespace lila::retopo

namespace lila::retopo
{
namespace
{
/** The reference faces as their own mesh, compacted. */
geo::Mesh regionMesh(const geo::Mesh& src, std::span<const Index> faces)
{
    geo::Mesh out;
    std::vector<Index> map(src.pointCapacity(), geo::kInvalid);
    for (Index f : faces)
    {
        if (f >= src.faceCapacity() || !src.faceValid(f))
            continue;
        std::vector<Index> pts = src.facePoints(f);
        for (Index& p : pts)
        {
            if (map[p] == geo::kInvalid)
                map[p] = out.addPoint(src.position(p));
            p = map[p];
        }
        out.addFace(pts);
    }
    return out;
}

/** The point on a closed polyline nearest p. */
glm::dvec3 closestOnLoop(std::span<const glm::dvec3> loop, const glm::dvec3& p)
{
    glm::dvec3 best = loop.empty() ? p : loop[0];
    double bestD = std::numeric_limits<double>::max();
    for (size_t i = 0; i < loop.size(); ++i)
    {
        const glm::dvec3& a = loop[i];
        const glm::dvec3& b = loop[(i + 1) % loop.size()];
        glm::dvec3 ab = b - a;
        double t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-300), 0.0, 1.0);
        glm::dvec3 q = a + ab * t;
        if (double d = glm::length(q - p); d < bestD)
        {
            bestD = d;
            best = q;
        }
    }
    return best;
}

/**
 * Makes q's border run exactly through the loop points: each loop point takes the border point that is mutually nearest,
 * else splits the nearest border edge; border points left unpaired fold into a paired neighbour. False when a loop point
 * is farther than reach from q's border.
 */
bool stitchToLoop(geo::Mesh& q, std::span<const glm::dvec3> loop, double reach)
{
    auto border = [&]() {
        std::vector<Index> out;
        for (Index v = 0; v < q.pointCapacity(); ++v)
            if (q.pointValid(v) && q.isBoundaryPoint(v))
                out.push_back(v);
        return out;
    };
    auto nearestLoop = [&](const glm::dvec3& at) {
        size_t best = 0;
        double d = std::numeric_limits<double>::max();
        for (size_t i = 0; i < loop.size(); ++i)
            if (double l = glm::length(loop[i] - at); l < d)
            {
                d = l;
                best = i;
            }
        return best;
    };
    std::vector<Index> paired(loop.size(), geo::kInvalid);
    std::vector<Index> b = border();
    for (size_t i = 0; i < loop.size(); ++i)
    {
        Index best = geo::kInvalid;
        double d = reach * 0.6;
        for (Index v : b)
            if (double l = glm::length(q.position(v) - loop[i]); l < d)
            {
                d = l;
                best = v;
            }
        if (best != geo::kInvalid && nearestLoop(q.position(best)) == i)
            paired[i] = best;
    }
    for (size_t i = 0; i < loop.size(); ++i)
    {
        if (paired[i] != geo::kInvalid)
            continue;
        Index bestEdge = geo::kInvalid;
        double bestD = std::numeric_limits<double>::max(), bestT = 0.5;
        for (Index e = 0; e < q.edgeCapacity(); ++e)
        {
            if (!q.edgeValid(e) || !q.isBoundaryEdge(e))
                continue;
            glm::dvec3 a0 = q.position(q.edgePoint(e, 0)), a1 = q.position(q.edgePoint(e, 1));
            double t = std::clamp(glm::dot(loop[i] - a0, a1 - a0) / std::max(glm::dot(a1 - a0, a1 - a0), 1e-300), 0.0, 1.0);
            if (double d = glm::length(a0 + t * (a1 - a0) - loop[i]); d < bestD)
            {
                bestD = d;
                bestEdge = e;
                bestT = t;
            }
        }
        if (bestEdge == geo::kInvalid || bestD > reach)
            return false;
        paired[i] = q.splitEdge(bestEdge, std::clamp(bestT, 0.05, 0.95));
    }
    std::vector<uint8_t> isPaired(q.pointCapacity(), 0);
    for (size_t i = 0; i < loop.size(); ++i)
    {
        q.setPosition(paired[i], loop[i]);
        isPaired[paired[i]] = 1;
    }
    std::vector<Index> target(q.pointCapacity(), geo::kInvalid);
    for (Index v : border())
        if (!isPaired[v])
        {
            Index into = geo::kInvalid;
            double d = std::numeric_limits<double>::max();
            q.forEachPointEdge(v, [&](Index e) {
                Index w = q.edgeOtherPoint(e, v);
                if (q.isBoundaryEdge(e) && isPaired[w] && q.edgeLength(e) < d)
                {
                    d = q.edgeLength(e);
                    into = w;
                }
            });
            if (into != geo::kInvalid)
                target[v] = into;
        }
    geo::weldPoints(q, target);
    return true;
}
} // namespace

GenResult generateRegion(const RegionSpec& s, const GenContext& ctx)
{
    LILA_ZONE();

    GenResult r;
    if (!ctx.surfaceMesh)
    {
        r.error = "an auto region needs a reference surface";
        return r;
    }
    if (s.referenceFaces != ctx.surfaceMesh->numFaces())
    {
        r.error = "the reference has changed since the region was marked; mark it again";
        return r;
    }
    geo::Mesh source = regionMesh(*ctx.surfaceMesh, s.faces);
    if (source.numFaces() == 0)
    {
        r.error = "the region has no faces";
        return r;
    }
    double area = 0.0;
    for (Index f = 0; f < source.faceCapacity(); ++f)
        if (source.faceValid(f))
            area += source.faceArea(f);
    const double edge = s.targetFaces > 0 ? std::sqrt(area / s.targetFaces) : std::max(s.edgeLength, 1e-6);
    // Checked up front: a remesh this dense runs for minutes before its first progress step.
    constexpr double kMaxFaces = 50000.0;
    if (double estimate = area / (edge * edge); estimate > kMaxFaces)
    {
        r.error = "about " + std::to_string(int64_t(estimate)) + " faces is over the region limit of 50000; lower Faces or raise Edge Length";
        return r;
    }
    QuadRemeshOptions o;
    o.targetEdgeLength = edge;
    o.guides = s.guides;
    o.timeout = ctx.timeout;
    o.adaptivity = 0.0;
    o.pureQuads = false;
    if (s.border.size() >= 3)
    {
        // The region's jagged border is slid onto the retopo loop, and each loop point claims a source point to pin.
        std::vector<Index> borderPts;
        for (Index p = 0; p < source.pointCapacity(); ++p)
            if (source.pointValid(p) && source.isBoundaryPoint(p))
                borderPts.push_back(p);
        for (Index p : borderPts)
            source.setPosition(p, closestOnLoop(s.border, source.position(p)));
        std::vector<uint8_t> taken(source.pointCapacity(), 0);
        for (const glm::dvec3& lp : s.border)
        {
            Index best = geo::kInvalid;
            double d = std::numeric_limits<double>::max();
            for (Index p : borderPts)
                if (!taken[p])
                    if (double l = glm::length(source.position(p) - lp); l < d)
                    {
                        d = l;
                        best = p;
                    }
            if (best == geo::kInvalid)
                break;
            taken[best] = 1;
            source.setPosition(best, lp);
            o.pinnedPoints.push_back(lp);
        }
    }
    QuadRemeshResult q = quadRemesh(source, o);
    if (!q.ok())
    {
        r.error = q.error;
        return r;
    }
    if (s.border.size() >= 3 && !stitchToLoop(q.mesh, s.border, o.targetEdgeLength * 1.5))
    {
        r.error = "couldn't join the region to the border around it; try an edge length closer to the border's";
        return r;
    }
    q.mesh.compact();
    std::vector<Index> map(q.mesh.pointCapacity(), geo::kInvalid);
    for (Index p = 0; p < q.mesh.pointCapacity(); ++p)
        if (q.mesh.pointValid(p))
        {
            map[p] = Index(r.positions.size());
            r.positions.push_back(q.mesh.position(p));
        }
    r.fixed.assign(r.positions.size(), 0);
    for (const glm::dvec3& lp : s.border)
        for (size_t i = 0; i < r.positions.size(); ++i)
            if (r.positions[i] == lp)
                r.fixed[i] = 1;
    for (Index f = 0; f < q.mesh.faceCapacity(); ++f)
        if (q.mesh.faceValid(f))
        {
            std::vector<Index> pts = q.mesh.facePoints(f);
            for (Index& p : pts)
                p = map[p];
            r.faces.push_back(pts);
        }
    r.sideCounts = {int(r.faces.size())};
    return r;
}
} // namespace lila::retopo
