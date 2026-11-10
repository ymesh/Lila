#include "layout/Fit.h"

#include "layout/Path.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace lila::layout
{
namespace
{
using StrokePoints = std::vector<glm::dvec3>;

/** Laplacian smoothing; open ends stay put, closed rings wrap. */
void smooth(StrokePoints& p, int iterations, bool closed)
{
    const size_t n = p.size();
    if (n < 3)
        return;
    StrokePoints next = p;
    for (int it = 0; it < iterations; ++it)
    {
        for (size_t k = 0; k < n; ++k)
        {
            if (!closed && (k == 0 || k + 1 == n))
                continue;
            const glm::dvec3& a = p[(k + n - 1) % n];
            const glm::dvec3& b = p[(k + 1) % n];
            next[k] = glm::mix(p[k], (a + b) * 0.5, 0.5);
        }
        std::swap(p, next);
    }
}

double turn(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    const glm::dvec3 u = b - a, v = c - b;
    const double lu = glm::length(u), lv = glm::length(v);
    if (lu < 1e-15 || lv < 1e-15)
        return 0.0;
    return std::acos(std::clamp(glm::dot(u, v) / (lu * lv), -1.0, 1.0));
}

/** Indices of corners: local maxima of the turn over a window, sharper than the threshold, at least a window apart. */
std::vector<size_t> findCorners(const StrokePoints& p, size_t window, double threshold, bool closed)
{
    const size_t n = p.size();
    std::vector<double> angle(n, 0.0);
    for (size_t k = 0; k < n; ++k)
    {
        if (!closed && (k < window || k + window >= n))
            continue;
        angle[k] = turn(p[(k + n - window) % n], p[k], p[(k + window) % n]);
    }
    std::vector<size_t> out;
    for (size_t k = 0; k < n; ++k)
    {
        if (angle[k] < threshold)
            continue;
        bool peak = true;
        for (size_t d = 1; d <= window && peak; ++d)
        {
            // Ties go to the first of a flat top.
            if ((closed || k >= d) && angle[(k + n - d) % n] >= angle[k])
                peak = false;
            if ((closed || k + d < n) && angle[(k + d) % n] > angle[k])
                peak = false;
        }
        if (peak && (out.empty() || k - out.back() > window))
            out.push_back(k);
    }
    if (closed && out.size() > 1 && out.front() + n - out.back() <= window)
        out.pop_back();
    return out;
}

FitPiece fitPiece(StrokePoints pts, const FitOptions& o, bool forceStraight)
{
    FitPiece piece;
    if (pts.size() < 2)
        return piece;
    smooth(pts, int(std::round(o.cleanup * 10.0)), false);
    const double straightTol = o.tolerance * (0.4 + 1.6 * o.cleanup);
    if (forceStraight || o.straight || chordDeviation(pts) < straightTol)
        piece.points = {pts.front(), pts.back()};
    else
        piece.points = fitPoints(pts, o.tolerance, o.cleanup);
    return piece;
}
} // namespace

std::vector<glm::dvec3> fitPoints(std::span<const glm::dvec3> points, double tolerance, double cleanup)
{
    const double eps = tolerance * (0.2 + 2.0 * std::clamp(cleanup, 0.0, 1.0));
    std::vector<glm::dvec3> out;
    for (size_t k : simplify(points, eps))
        out.push_back(points[k]);
    return out;
}

FitResult fitStroke(std::span<const glm::dvec3> stroke, const FitOptions& opts)
{
    FitResult out;
    FitOptions o = opts;
    o.tolerance = std::max(o.tolerance, 1e-9);
    o.cleanup = std::clamp(o.cleanup, 0.0, 1.0);

    StrokePoints clean;
    for (const glm::dvec3& p : stroke)
        if (clean.empty() || glm::length(p - clean.back()) > o.tolerance * 0.05)
            clean.push_back(p);
    const double total = polylineLength(clean);
    if (clean.size() < 2 || total < o.tolerance)
        return out;

    out.closed = total > 8.0 * o.tolerance && glm::length(clean.front() - clean.back()) < 2.0 * o.tolerance;
    const double h = o.tolerance * 0.25;
    StrokePoints p = resample(clean, h);
    if (out.closed)
    {
        p.back() = p.front();
        p.pop_back();
    }
    const size_t window = std::max<size_t>(2, size_t(std::round(1.5 * o.tolerance / h)));
    // Smoothed over about a window, so hand jitter never reads as a corner while real corners still do.
    StrokePoints light = p;
    smooth(light, int(window), out.closed);

    const double threshold = glm::radians(o.straight ? std::min(o.cornerAngle, 25.0) : o.cornerAngle);
    std::vector<size_t> corners = (light.size() > 2 * window + 1) ? findCorners(light, window, threshold, out.closed) : std::vector<size_t>{};

    if (!out.closed)
    {
        std::vector<size_t> cuts = {0};
        cuts.insert(cuts.end(), corners.begin(), corners.end());
        cuts.push_back(p.size() - 1);
        for (size_t k = 0; k + 1 < cuts.size(); ++k)
        {
            StrokePoints piece(p.begin() + ptrdiff_t(cuts[k]), p.begin() + ptrdiff_t(cuts[k + 1]) + 1);
            out.pieces.push_back(fitPiece(std::move(piece), o, o.straightenLast && k + 2 == cuts.size()));
        }
        return out;
    }

    const size_t n = p.size();
    if (corners.empty())
    {
        StrokePoints ring = p;
        ring.push_back(p.front());
        FitPiece piece;
        smooth(ring, int(std::round(o.cleanup * 10.0)), false);
        piece.points = fitPoints(ring, o.tolerance, o.cleanup);
        // A ring needs a few points to stay a ring.
        if (piece.points.size() < 4)
        {
            piece.points.clear();
            for (int k = 0; k <= 4; ++k)
                piece.points.push_back(ring[std::min(ring.size() - 1, size_t(k) * (ring.size() - 1) / 4)]);
        }
        out.pieces.push_back(std::move(piece));
        return out;
    }
    out.closedCorner = true;
    for (size_t k = 0; k < corners.size(); ++k)
    {
        const size_t from = corners[k], to = corners[(k + 1) % corners.size()];
        StrokePoints piece;
        for (size_t i = from;; i = (i + 1) % n)
        {
            piece.push_back(p[i]);
            if (i == to && piece.size() > 1)
                break;
        }
        out.pieces.push_back(fitPiece(std::move(piece), o, false));
    }
    return out;
}
} // namespace lila::layout
