#include "uv/Layout.h"
#include "uv/Flow.h"

#include "core/Profile.h"
#include "uv/Unwrap.h"
#include "uv/UvCommon.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace lila::uv
{
namespace
{
using detail::edgeKey;

double orient2d(const glm::dvec2& o, const glm::dvec2& a, const glm::dvec2& b)
{
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

glm::dvec2 rotate(const glm::dvec2& p, double c, double s)
{
    return {p.x * c - p.y * s, p.x * s + p.y * c};
}

bool validUvs(const Chart& ch)
{
    return std::abs(ch.areaUv()) > 1e-12 && foldedTriangles(ch) == 0;
}

/** Integer grid coordinates per vertex for an all-quad disk chart; false when it is not a regular grid. */
bool gridCoords(const Chart& ch, std::vector<glm::ivec2>& coord)
{
    const size_t F = ch.faces.size();
    std::vector<std::array<Index, 4>> quad(F);
    for (size_t f = 0; f < F; ++f)
    {
        if (ch.faceStart[f + 1] - ch.faceStart[f] != 4)
            return false;
        for (int k = 0; k < 4; ++k)
            quad[f][k] = ch.cornerVertex[ch.faceStart[f] + k];
    }
    std::unordered_map<uint64_t, std::array<Index, 2>> edgeFaces;
    for (size_t f = 0; f < F; ++f)
        for (int k = 0; k < 4; ++k)
        {
            auto [it, fresh] = edgeFaces.try_emplace(edgeKey(quad[f][k], quad[f][(k + 1) % 4]), std::array<Index, 2>{Index(f), kInvalid});
            if (!fresh)
            {
                if (it->second[1] != kInvalid)
                    return false;
                it->second[1] = Index(f);
            }
        }

    const glm::ivec2 unset(std::numeric_limits<int>::min());
    coord.assign(ch.vertexCount(), unset);
    std::vector<uint8_t> done(F, 0);
    auto assign = [&](Index v, glm::ivec2 c) {
        if (coord[v] == unset)
        {
            coord[v] = c;
            return true;
        }
        return coord[v] == c;
    };
    const glm::ivec2 start[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int k = 0; k < 4; ++k)
        coord[quad[0][k]] = start[k];
    std::vector<Index> queue{0};
    done[0] = 1;
    for (size_t qi = 0; qi < queue.size(); ++qi)
    {
        const auto& q = quad[queue[qi]];
        for (int k = 0; k < 4; ++k)
        {
            Index a = q[k], b = q[(k + 1) % 4];
            const auto& ef = edgeFaces[edgeKey(a, b)];
            Index g = ef[0] == queue[qi] ? ef[1] : ef[0];
            if (g == kInvalid)
                continue;
            glm::ivec2 out = coord[a] - coord[q[(k + 3) % 4]];
            const auto& w = quad[g];
            int m = -1;
            for (int j = 0; j < 4; ++j)
                if (w[j] == b && w[(j + 1) % 4] == a)
                    m = j;
            if (m < 0)
                return false;
            if (!assign(w[(m + 2) % 4], coord[a] + out) || !assign(w[(m + 3) % 4], coord[b] + out))
                return false;
            if (!done[g])
            {
                done[g] = 1;
                queue.push_back(g);
            }
        }
    }
    if (queue.size() != F)
        return false;
    std::map<std::pair<int, int>, Index> seen;
    for (Index v = 0; v < ch.vertexCount(); ++v)
        if (!seen.emplace(std::make_pair(coord[v].x, coord[v].y), v).second)
            return false;
    return true;
}

void layGrid(Chart& ch, const std::vector<glm::ivec2>& coord)
{
    glm::ivec2 lo(std::numeric_limits<int>::max()), hi(std::numeric_limits<int>::min());
    for (const glm::ivec2& c : coord)
    {
        lo = glm::min(lo, c);
        hi = glm::max(hi, c);
    }
    std::vector<double> width(size_t(hi.x - lo.x) + 1, 0.0), wn(width.size(), 0.0);
    std::vector<double> height(size_t(hi.y - lo.y) + 1, 0.0), hn(height.size(), 0.0);
    for (size_t f = 0; f + 1 < ch.faceStart.size(); ++f)
        for (Index k = ch.faceStart[f]; k < ch.faceStart[f + 1]; ++k)
        {
            Index a = ch.cornerVertex[k];
            Index b = ch.cornerVertex[k + 1 < ch.faceStart[f + 1] ? k + 1 : ch.faceStart[f]];
            glm::ivec2 d = coord[b] - coord[a];
            double len = glm::length(ch.pos[b] - ch.pos[a]);
            if (d.y == 0)
            {
                size_t i = size_t(std::min(coord[a].x, coord[b].x) - lo.x);
                width[i] += len;
                wn[i] += 1.0;
            }
            else
            {
                size_t j = size_t(std::min(coord[a].y, coord[b].y) - lo.y);
                height[j] += len;
                hn[j] += 1.0;
            }
        }
    std::vector<double> u(width.size() + 1, 0.0), v(height.size() + 1, 0.0);
    for (size_t i = 0; i < width.size(); ++i)
        u[i + 1] = u[i] + (wn[i] > 0.0 ? width[i] / wn[i] : 0.0);
    for (size_t j = 0; j < height.size(); ++j)
        v[j + 1] = v[j] + (hn[j] > 0.0 ? height[j] / hn[j] : 0.0);
    for (Index p = 0; p < ch.vertexCount(); ++p)
        ch.uv[p] = {u[size_t(coord[p].x - lo.x)], v[size_t(coord[p].y - lo.y)]};
}

bool layRectangle(Chart& ch)
{
    auto loops = ch.boundaryLoops();
    if (loops.empty() || loops[0].size() < 4)
        return false;
    if (!validUvs(ch) && !unwrapChart(ch).ok)
        return false;
    const std::vector<Index>& loop = loops[0];
    const size_t n = loop.size();
    std::vector<double> turn(n);
    for (size_t i = 0; i < n; ++i)
    {
        glm::dvec2 a = ch.uv[loop[(i + n - 1) % n]], b = ch.uv[loop[i]], c = ch.uv[loop[(i + 1) % n]];
        glm::dvec2 d1 = b - a, d2 = c - b;
        turn[i] = std::atan2(d1.x * d2.y - d1.y * d2.x, glm::dot(d1, d2));
    }
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return turn[x] > turn[y]; });
    // The four sharpest left turns at least `gap` vertices apart, counting only turns sharper than minTurn.
    auto pick = [&](size_t gap, double minTurn) {
        std::vector<size_t> corners;
        for (size_t i : order)
        {
            if (turn[i] < minTurn)
                break;
            bool close = false;
            for (size_t c : corners)
            {
                size_t d = i > c ? i - c : c - i;
                close |= std::min(d, n - d) < gap;
            }
            if (!close)
                corners.push_back(i);
            if (corners.size() == 4)
                break;
        }
        return corners;
    };
    // Real corners first, however close: a long strip's short ends put two corners only a few vertices apart.
    // A round island has none, so its four are then spread around the loop.
    std::vector<size_t> corners = pick(2, glm::quarter_pi<double>());
    if (corners.size() < 4)
        corners = pick(std::max<size_t>(1, n / 8), -glm::pi<double>());
    if (corners.size() < 4)
        return false;
    std::sort(corners.begin(), corners.end());

    double side[4] = {};
    std::vector<double> along(n, 0.0);
    for (int s = 0; s < 4; ++s)
    {
        size_t from = corners[s], to = corners[(s + 1) % 4];
        double len = 0.0;
        for (size_t i = from; i != to; i = (i + 1) % n)
        {
            along[i] = len;
            len += glm::length(ch.pos[loop[(i + 1) % n]] - ch.pos[loop[i]]);
        }
        side[s] = len;
    }
    double w = 0.5 * (side[0] + side[2]), h = 0.5 * (side[1] + side[3]);
    if (w <= 0.0 || h <= 0.0)
        return false;
    const glm::dvec2 rect[5] = {{0.0, 0.0}, {w, 0.0}, {w, h}, {0.0, h}, {0.0, 0.0}};
    std::vector<uint8_t> savedPins = ch.pinned;
    std::fill(ch.pinned.begin(), ch.pinned.end(), uint8_t(0));
    for (int s = 0; s < 4; ++s)
    {
        size_t from = corners[s], to = corners[(s + 1) % 4];
        for (size_t i = from; i != to; i = (i + 1) % n)
        {
            double t = side[s] > 0.0 ? along[i] / side[s] : 0.0;
            ch.uv[loop[i]] = glm::mix(rect[s], rect[s + 1], t);
            ch.pinned[loop[i]] = 1;
        }
    }

    // Interior at neighbour averages: fold free on a convex boundary.
    const size_t V = ch.vertexCount();
    std::vector<Index> freeId(V, kInvalid);
    Index nf = 0;
    for (Index v = 0; v < V; ++v)
        if (!ch.pinned[v])
            freeId[v] = nf++;
    if (nf > 0)
    {
        std::vector<uint64_t> edges;
        for (const auto& t : ch.tris)
            for (int i = 0; i < 3; ++i)
                edges.push_back(edgeKey(t[i], t[(i + 1) % 3]));
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        std::vector<Eigen::Triplet<double>> trips;
        Eigen::VectorXd bu = Eigen::VectorXd::Zero(nf), bv = bu;
        for (uint64_t e : edges)
        {
            Index a = Index(e >> 32), b = Index(e & 0xffffffffu);
            for (auto [p, q] : {std::pair{a, b}, std::pair{b, a}})
            {
                if (freeId[p] == kInvalid)
                    continue;
                trips.emplace_back(int(freeId[p]), int(freeId[p]), 1.0);
                if (freeId[q] != kInvalid)
                    trips.emplace_back(int(freeId[p]), int(freeId[q]), -1.0);
                else
                {
                    bu[freeId[p]] += ch.uv[q].x;
                    bv[freeId[p]] += ch.uv[q].y;
                }
            }
        }
        Eigen::SparseMatrix<double> m(nf, nf);
        m.setFromTriplets(trips.begin(), trips.end());
        Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver(m);
        if (solver.info() == Eigen::Success)
        {
            Eigen::VectorXd xu = solver.solve(bu), xv = solver.solve(bv);
            for (Index v = 0; v < V; ++v)
                if (freeId[v] != kInvalid)
                    ch.uv[v] = {xu[freeId[v]], xv[freeId[v]]};
        }
        slim(ch, 12, true);
    }
    ch.pinned = std::move(savedPins);
    return true;
}
} // namespace

std::vector<glm::dvec2> convexHull(std::vector<glm::dvec2> pts)
{
    std::sort(pts.begin(), pts.end(), [](const glm::dvec2& a, const glm::dvec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3)
        return pts;
    std::vector<glm::dvec2> hull(pts.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        while (k >= 2 && orient2d(hull[k - 2], hull[k - 1], pts[i]) <= 0.0)
            --k;
        hull[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i)
    {
        while (k >= t && orient2d(hull[k - 2], hull[k - 1], pts[i - 1]) <= 0.0)
            --k;
        hull[k++] = pts[i - 1];
    }
    hull.resize(k - 1);
    return hull;
}

double minAreaRectAngle(std::span<const glm::dvec2> points, glm::dvec2* size)
{
    std::vector<glm::dvec2> hull = convexHull({points.begin(), points.end()});
    double bestArea = std::numeric_limits<double>::max(), bestAngle = 0.0;
    glm::dvec2 bestSize(0.0);
    for (size_t i = 0; i < hull.size(); ++i)
    {
        glm::dvec2 e = hull[(i + 1) % hull.size()] - hull[i];
        if (glm::length(e) <= 0.0)
            continue;
        double a = -std::atan2(e.y, e.x);
        double c = std::cos(a), s = std::sin(a);
        glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
        for (const glm::dvec2& p : hull)
        {
            glm::dvec2 r = rotate(p, c, s);
            lo = glm::min(lo, r);
            hi = glm::max(hi, r);
        }
        glm::dvec2 ext = hi - lo;
        // Ties go to the smaller rotation so the result is stable.
        double area = ext.x * ext.y;
        if (area < bestArea * (1.0 - 1e-9))
        {
            bestArea = area;
            bestAngle = a;
            bestSize = ext;
        }
    }
    if (bestSize.y > bestSize.x)
    {
        bestAngle += glm::half_pi<double>();
        std::swap(bestSize.x, bestSize.y);
    }
    // Keep the turn within a quarter either way: same rectangle, less rotation.
    while (bestAngle > glm::half_pi<double>() + 1e-9)
        bestAngle -= glm::pi<double>();
    while (bestAngle < -glm::half_pi<double>() - 1e-9)
        bestAngle += glm::pi<double>();
    if (size)
        *size = bestSize;
    return bestAngle;
}

void transformChart(Chart& ch, const glm::dvec2& pivot, double angle, const glm::dvec2& scale, const glm::dvec2& offset)
{
    double c = std::cos(angle), s = std::sin(angle);
    for (glm::dvec2& p : ch.uv)
        p = pivot + offset + rotate((p - pivot) * scale, c, s);
}

double gridAngle(const Chart& ch, double* agreement)
{
    // Each edge direction folded four ways, so edges along U and V (and their opposites) all add up.
    double s = 0.0, c = 0.0, total = 0.0;
    for (size_t f = 0; f + 1 < ch.faceStart.size(); ++f)
    {
        const Index first = ch.faceStart[f], end = ch.faceStart[f + 1];
        for (Index k = first; k < end; ++k)
        {
            const glm::dvec2 e = ch.uv[ch.cornerVertex[k + 1 < end ? k + 1 : first]] - ch.uv[ch.cornerVertex[k]];
            const double l = glm::length(e);
            if (l <= 0.0)
                continue;
            const double a = 4.0 * std::atan2(e.y, e.x);
            s += l * std::sin(a);
            c += l * std::cos(a);
            total += l;
        }
    }
    if (agreement)
        *agreement = total > 0.0 ? std::hypot(s, c) / total : 0.0;
    return total > 0.0 ? -0.25 * std::atan2(s, c) : 0.0;
}

void orientChart(Chart& ch, OrientMode mode)
{
    LILA_ZONE();

    if (ch.uv.size() < 2)
        return;
    glm::dvec2 pivot = ch.uvCentroid();
    double angle = 0.0;
    switch (mode)
    {
    case OrientMode::BoundingBox: angle = minAreaRectAngle(ch.uv); break;
    case OrientMode::Principal:
    {
        double sxx = 0.0, syy = 0.0, sxy = 0.0;
        for (const auto& t : ch.tris)
        {
            glm::dvec2 e1 = ch.uv[t[1]] - ch.uv[t[0]], e2 = ch.uv[t[2]] - ch.uv[t[0]];
            double a = std::abs(e1.x * e2.y - e1.y * e2.x);
            glm::dvec2 d = (ch.uv[t[0]] + ch.uv[t[1]] + ch.uv[t[2]]) / 3.0 - pivot;
            sxx += a * d.x * d.x;
            syy += a * d.y * d.y;
            sxy += a * d.x * d.y;
        }
        angle = -0.5 * std::atan2(2.0 * sxy, sxx - syy);
        break;
    }
    case OrientMode::Grid: angle = gridAngle(ch); break;
    case OrientMode::Flow: angle = flowAngle(ch); break;
    case OrientMode::Upright: angle = uprightAngle(ch); break;
    case OrientMode::LongestEdge:
    {
        double best = -1.0;
        for (const auto& loop : ch.boundaryLoops())
            for (size_t i = 0; i < loop.size(); ++i)
            {
                glm::dvec2 e = ch.uv[loop[(i + 1) % loop.size()]] - ch.uv[loop[i]];
                if (double l = glm::length(e); l > best)
                {
                    best = l;
                    double a = std::atan2(e.y, e.x);
                    // Snap to whichever axis is nearest.
                    angle = -(a - std::round(a / glm::half_pi<double>()) * glm::half_pi<double>());
                }
            }
        break;
    }
    }
    transformChart(ch, pivot, angle);
}

void fitChart(Chart& ch, const glm::dvec2& centroid, double area)
{
    double cur = std::abs(ch.areaUv());
    double s = area > 0.0 && cur > 0.0 ? std::sqrt(area / cur) : 1.0;
    glm::dvec2 c = ch.uvCentroid();
    for (glm::dvec2& p : ch.uv)
        p = centroid + (p - c) * s;
}

StraightenResult straightenChart(Chart& ch)
{
    LILA_ZONE();

    bool hadUvs = validUvs(ch);
    glm::dvec2 centroid = ch.uvCentroid();
    double area = std::abs(ch.areaUv());

    StraightenResult r = StraightenResult::Failed;
    std::vector<glm::ivec2> coord;
    if (ch.euler() == 1 && gridCoords(ch, coord))
    {
        layGrid(ch, coord);
        r = StraightenResult::Grid;
    }
    else if (ch.euler() == 1 && layRectangle(ch))
        r = StraightenResult::Rectangle;
    if (r == StraightenResult::Failed)
        return r;
    if (ch.areaUv() < 0.0)
        for (glm::dvec2& p : ch.uv)
            p.x = -p.x;
    if (hadUvs)
        fitChart(ch, centroid, area);
    return r;
}
} // namespace lila::uv
