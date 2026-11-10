#include "layout/Path.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>

namespace lila::layout
{
namespace
{
size_t segmentAt(const std::vector<double>& lengths, double s)
{
    if (lengths.size() < 2)
        return 0;
    auto it = std::upper_bound(lengths.begin(), lengths.end(), s);
    const size_t k = size_t(std::max<ptrdiff_t>(1, it - lengths.begin()));
    return std::min(k, lengths.size() - 1) - 1;
}

void dpRecurse(std::span<const glm::dvec3> pts, size_t a, size_t b, double eps, std::vector<char>& keep)
{
    if (b <= a + 1)
        return;
    const glm::dvec3 d = pts[b] - pts[a];
    const double len2 = glm::dot(d, d);
    double best = -1.0;
    size_t at = a;
    for (size_t k = a + 1; k < b; ++k)
    {
        const glm::dvec3 v = pts[k] - pts[a];
        const double t = len2 > 0.0 ? std::clamp(glm::dot(v, d) / len2, 0.0, 1.0) : 0.0;
        const double dist = glm::length(v - d * t);
        if (dist > best)
        {
            best = dist;
            at = k;
        }
    }
    if (best > eps)
    {
        keep[at] = 1;
        dpRecurse(pts, a, at, eps, keep);
        dpRecurse(pts, at, b, eps, keep);
    }
}

/** Centripetal Catmull-Rom between p1 and p2. */
glm::dvec3 catmullRom(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& p2, const glm::dvec3& p3, double u)
{
    auto knot = [](double t, const glm::dvec3& a, const glm::dvec3& b) { return t + std::max(1e-9, std::sqrt(glm::length(b - a))); };
    const double t0 = 0.0, t1 = knot(t0, p0, p1), t2 = knot(t1, p1, p2), t3 = knot(t2, p2, p3);
    const double t = t1 + (t2 - t1) * u;
    const glm::dvec3 a1 = (t1 - t) / (t1 - t0) * p0 + (t - t0) / (t1 - t0) * p1;
    const glm::dvec3 a2 = (t2 - t) / (t2 - t1) * p1 + (t - t1) / (t2 - t1) * p2;
    const glm::dvec3 a3 = (t3 - t) / (t3 - t2) * p2 + (t - t2) / (t3 - t2) * p3;
    const glm::dvec3 b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2;
    const glm::dvec3 b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3;
    return (t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2;
}

/** Projects the midpoint between two surface samples and recurses while they are further apart than step. */
void fillGap(const Surface& surface, const SurfaceHit& a, const SurfaceHit& b, double step, int depth, Path& out)
{
    if (depth > 12 || glm::length(b.point - a.point) <= step)
    {
        out.points.push_back(b.point);
        out.normals.push_back(b.normal);
        return;
    }
    const SurfaceHit m = surface.project((a.point + b.point) * 0.5);
    fillGap(surface, a, m, step, depth + 1, out);
    fillGap(surface, m, b, step, depth + 1, out);
}
} // namespace

void Path::measure()
{
    lengths.assign(points.size(), 0.0);
    for (size_t k = 1; k < points.size(); ++k)
        lengths[k] = lengths[k - 1] + glm::length(points[k] - points[k - 1]);
}

glm::dvec3 Path::pointAt(double s) const
{
    if (points.empty())
        return glm::dvec3(0.0);
    if (points.size() == 1)
        return points[0];
    const size_t k = segmentAt(lengths, s);
    const double seg = lengths[k + 1] - lengths[k];
    const double t = seg > 0.0 ? std::clamp((s - lengths[k]) / seg, 0.0, 1.0) : 0.0;
    return glm::mix(points[k], points[k + 1], t);
}

glm::dvec3 Path::normalAt(double s) const
{
    if (normals.empty())
        return glm::dvec3(0.0, 1.0, 0.0);
    if (normals.size() == 1 || lengths.size() != normals.size())
        return normals[0];
    const size_t k = segmentAt(lengths, s);
    const double seg = lengths[k + 1] - lengths[k];
    const double t = seg > 0.0 ? std::clamp((s - lengths[k]) / seg, 0.0, 1.0) : 0.0;
    const glm::dvec3 n = glm::mix(normals[k], normals[k + 1], t);
    const double len = glm::length(n);
    return len > 1e-12 ? n / len : normals[k];
}

glm::dvec3 Path::tangentAt(double s) const
{
    if (points.size() < 2)
        return glm::dvec3(1.0, 0.0, 0.0);
    const size_t k = segmentAt(lengths, s);
    const glm::dvec3 d = points[k + 1] - points[k];
    const double len = glm::length(d);
    return len > 1e-15 ? d / len : glm::dvec3(1.0, 0.0, 0.0);
}

PathNearest nearestOnPath(std::span<const glm::dvec3> points, std::span<const double> lengths, const glm::dvec3& p)
{
    PathNearest best;
    if (points.empty())
        return best;
    if (points.size() == 1)
    {
        best.distance = glm::length(p - points[0]);
        best.point = points[0];
        return best;
    }
    for (size_t k = 0; k + 1 < points.size(); ++k)
    {
        const glm::dvec3 d = points[k + 1] - points[k];
        const double len2 = glm::dot(d, d);
        const double t = len2 > 0.0 ? std::clamp(glm::dot(p - points[k], d) / len2, 0.0, 1.0) : 0.0;
        const glm::dvec3 q = points[k] + d * t;
        const double dist = glm::length(p - q);
        if (dist < best.distance)
        {
            best.distance = dist;
            best.segment = k;
            best.point = q;
            const double l0 = k < lengths.size() ? lengths[k] : 0.0;
            const double l1 = k + 1 < lengths.size() ? lengths[k + 1] : l0 + std::sqrt(len2);
            best.s = l0 + (l1 - l0) * t;
        }
    }
    return best;
}

double polylineLength(std::span<const glm::dvec3> points)
{
    double l = 0.0;
    for (size_t k = 1; k < points.size(); ++k)
        l += glm::length(points[k] - points[k - 1]);
    return l;
}

std::vector<glm::dvec3> resample(std::span<const glm::dvec3> points, double step)
{
    std::vector<glm::dvec3> out;
    if (points.empty())
        return out;
    const double total = polylineLength(points);
    if (total <= 0.0 || step <= 0.0)
    {
        out.push_back(points.front());
        if (points.size() > 1)
            out.push_back(points.back());
        return out;
    }
    const int n = std::max(1, int(std::ceil(total / step)));
    const double each = total / n;
    out.push_back(points.front());
    double walked = 0.0, next = each;
    for (size_t k = 1; k < points.size() && int(out.size()) < n; ++k)
    {
        const double seg = glm::length(points[k] - points[k - 1]);
        while (seg > 0.0 && walked + seg >= next && int(out.size()) < n)
        {
            out.push_back(glm::mix(points[k - 1], points[k], (next - walked) / seg));
            next += each;
        }
        walked += seg;
    }
    out.push_back(points.back());
    return out;
}

std::vector<size_t> simplify(std::span<const glm::dvec3> points, double eps)
{
    std::vector<size_t> out;
    if (points.empty())
        return out;
    std::vector<char> keep(points.size(), 0);
    keep.front() = keep.back() = 1;
    dpRecurse(points, 0, points.size() - 1, eps, keep);
    for (size_t k = 0; k < points.size(); ++k)
        if (keep[k])
            out.push_back(k);
    return out;
}

double chordDeviation(std::span<const glm::dvec3> points)
{
    if (points.size() < 3)
        return 0.0;
    const glm::dvec3 a = points.front(), d = points.back() - a;
    const double len2 = glm::dot(d, d);
    double worst = 0.0;
    for (size_t k = 1; k + 1 < points.size(); ++k)
    {
        const glm::dvec3 v = points[k] - a;
        const double t = len2 > 0.0 ? std::clamp(glm::dot(v, d) / len2, 0.0, 1.0) : 0.0;
        worst = std::max(worst, glm::length(v - d * t));
    }
    return worst;
}

Path surfaceCurve(const Surface& surface, std::span<const glm::dvec3> through, double spacing, bool closed)
{
    LILA_ZONE();
    Path out;
    if (through.empty())
        return out;
    spacing = std::max(spacing, surface.size() * 1e-5);
    std::vector<glm::dvec3> pts(through.begin(), through.end());
    if (closed && pts.size() > 2 && glm::length(pts.front() - pts.back()) > 1e-12)
        pts.push_back(pts.front());
    const size_t n = pts.size();
    std::vector<glm::dvec3> raw;
    raw.push_back(pts[0]);
    for (size_t k = 0; k + 1 < n; ++k)
    {
        // Outside the ends, mirror the neighbour so the curve leaves each end heading at the next point; closed curves wrap.
        glm::dvec3 p0, p3;
        if (k > 0)
            p0 = pts[k - 1];
        else
            p0 = closed && n > 3 ? pts[n - 2] : pts[0] * 2.0 - pts[1];
        if (k + 2 < n)
            p3 = pts[k + 2];
        else
            p3 = closed && n > 3 ? pts[1] : pts[n - 1] * 2.0 - pts[n - 2];
        const double seg = glm::length(pts[k + 1] - pts[k]);
        const int steps = std::max(1, int(std::ceil(seg / spacing)));
        for (int i = 1; i <= steps; ++i)
            raw.push_back(i == steps ? pts[k + 1] : catmullRom(p0, pts[k], pts[k + 1], p3, double(i) / steps));
    }
    SurfaceHit prev = surface.project(raw[0]);
    out.points.push_back(prev.point);
    out.normals.push_back(prev.normal);
    for (size_t k = 1; k < raw.size(); ++k)
    {
        const SurfaceHit h = surface.project(raw[k]);
        fillGap(surface, prev, h, spacing * 1.5, 0, out);
        prev = h;
    }
    out.measure();
    return out;
}

std::vector<glm::dvec3> runThrough(const PanelLayout& layout, const Run& run, const Surface& surface)
{
    std::vector<glm::dvec3> pts;
    const Junction* a = layout.junction(run.a);
    const Junction* b = layout.junction(run.b);
    if (!a || !b)
        return pts;
    pts.reserve(run.controls.size() + 2);
    pts.push_back(surface.resolve(a->at));
    for (const Anchor& c : run.controls)
        pts.push_back(surface.resolve(c));
    pts.push_back(surface.resolve(b->at));
    return pts;
}

Path runPath(const PanelLayout& layout, const Run& run, const Surface& surface, double spacing)
{
    const std::vector<glm::dvec3> pts = runThrough(layout, run, surface);
    if (pts.size() < 2)
        return {};
    // A loop starting and ending on one junction is a closed curve through its controls.
    return surfaceCurve(surface, pts, spacing, run.a == run.b && pts.size() > 3);
}

double defaultSpacing(const PanelLayout& layout, const Surface& surface)
{
    double narrowest = std::numeric_limits<double>::max();
    for (const Run& r : layout.runs)
        narrowest = std::min(narrowest, layout.widthOf(r));
    const double s = surface.size();
    if (narrowest == std::numeric_limits<double>::max())
        narrowest = s / 400.0;
    return std::clamp(narrowest * 0.5, s / 4000.0, s / 300.0);
}
} // namespace lila::layout
