// Settlement: polygons in the XZ plane and the ground everything stands on.

#include "SettlementCore.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace settlement
{
namespace
{
bool segmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
    const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a);
    const double d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}

/** Drops points closer than eps to the next; the side leaving the dropped point goes with it. */
void dropDuplicates(Polygon& p, double eps = 1e-6)
{
    for (size_t k = 0; k < p.pts.size() && p.pts.size() > 2;)
    {
        const size_t n = (k + 1) % p.pts.size();
        if (glm::length(p.pts[n] - p.pts[k]) < eps)
        {
            p.pts.erase(p.pts.begin() + long(k));
            p.tag.erase(p.tag.begin() + long(k));
        }
        else
            ++k;
    }
}

Polygon reversed(const Polygon& p)
{
    Polygon r;
    const size_t n = p.pts.size();
    for (size_t i = 0; i < n; ++i)
    {
        r.pts.push_back(p.pts[n - 1 - i]);
        r.tag.push_back(p.tag[(2 * n - 2 - i) % n]);
    }
    return r;
}

Polygon clipTagged(const Polygon& p, Vec2 origin, Vec2 normal, int cutTag)
{
    Polygon out;
    const size_t n = p.pts.size();
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2 a = p.pts[k], b = p.pts[(k + 1) % n];
        const double da = glm::dot(a - origin, normal), db = glm::dot(b - origin, normal);
        const bool ina = da >= 0.0, inb = db >= 0.0;
        if (ina)
        {
            out.pts.push_back(a);
            out.tag.push_back(p.tag[k]);
        }
        if (ina != inb)
        {
            out.pts.push_back(a + (b - a) * (da / (da - db)));
            out.tag.push_back(ina ? cutTag : p.tag[k]);
        }
    }
    dropDuplicates(out);
    if (out.pts.size() < 3 || signedArea(out.pts) <= 1e-6)
        return {};
    return out;
}
} // namespace

double signedArea(std::span<const Vec2> pts)
{
    double a = 0.0;
    for (size_t k = 0; k < pts.size(); ++k)
        a += cross(pts[k], pts[(k + 1) % pts.size()]);
    return 0.5 * a;
}

double area(const Polygon& p) { return std::abs(signedArea(p.pts)); }

Vec2 centroid(const Polygon& p)
{
    double a = 0.0;
    Vec2 c{0.0};
    for (size_t k = 0; k < p.pts.size(); ++k)
    {
        const Vec2 u = p.pts[k], v = p.pts[(k + 1) % p.pts.size()];
        const double w = cross(u, v);
        a += w;
        c += (u + v) * w;
    }
    if (std::abs(a) < 1e-12)
    {
        Vec2 m{0.0};
        for (const Vec2& q : p.pts)
            m += q;
        return p.pts.empty() ? m : m / double(p.pts.size());
    }
    return c / (3.0 * a);
}

bool contains(std::span<const Vec2> pts, Vec2 q)
{
    bool in = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++)
        if ((pts[i].y > q.y) != (pts[j].y > q.y) &&
            q.x < (pts[j].x - pts[i].x) * (q.y - pts[i].y) / (pts[j].y - pts[i].y) + pts[i].x)
            in = !in;
    return in;
}

Polygon clipHalfPlane(const Polygon& p, Vec2 origin, Vec2 normal, int cutTag) { return clipTagged(p, origin, normal, cutTag); }

bool selfIntersects(std::span<const Vec2> pts)
{
    const size_t n = pts.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 2; j < n; ++j)
        {
            if (i == 0 && j == n - 1)
                continue;
            if (segmentsCross(pts[i], pts[(i + 1) % n], pts[j], pts[(j + 1) % n]))
                return true;
        }
    return false;
}

/**
 * p's sides moved in by d with every side whose offset turns back on itself dropped, shortest first, its neighbours
 * meeting in its place (the edge events of a straight skeleton). Empty when what is left still crosses itself.
 */
static Polygon insetDropping(const Polygon& p, std::span<const double> d)
{
    const size_t n = p.pts.size();
    std::vector<Vec2> dir(n), base(n);
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2 e = p.pts[(k + 1) % n] - p.pts[k];
        const double len = glm::length(e);
        dir[k] = len > 1e-12 ? e / len : Vec2(1.0, 0.0);
        base[k] = p.pts[k] + leftNormal(dir[k]) * d[k];
    }
    std::vector<size_t> keep(n);
    std::iota(keep.begin(), keep.end(), size_t(0));
    std::vector<Vec2> v;
    while (keep.size() >= 3)
    {
        const size_t m = keep.size();
        v.assign(m, Vec2(0.0));
        for (size_t i = 0; i < m; ++i)
        {
            const size_t j = keep[(i + m - 1) % m], k = keep[i];
            const double den = cross(dir[j], dir[k]);
            // Nearly parallel neighbours (a street changing width along a straight run) step across at the corner.
            if (std::abs(den) < 0.05)
            {
                const Vec2 q = p.pts[k];
                v[i] = 0.5 * (base[j] + dir[j] * glm::dot(q - base[j], dir[j]) + base[k] + dir[k] * glm::dot(q - base[k], dir[k]));
            }
            else
                v[i] = base[j] + dir[j] * (cross(base[k] - base[j], dir[k]) / den);
        }
        size_t worst = m;
        double shortest = 0.0;
        for (size_t i = 0; i < m; ++i)
            if (const double along = glm::dot(v[(i + 1) % m] - v[i], dir[keep[i]]); along <= 0.0 && along < shortest + 1e-9)
            {
                shortest = along;
                worst = i;
            }
        if (worst == m)
            break;
        keep.erase(keep.begin() + long(worst));
    }
    if (keep.size() < 3 || signedArea(v) <= 1e-6 || selfIntersects(v))
        return {};
    // Sides that met across a narrow neck leave corners outside, or nearer a side than its distance: not an inset.
    const double nearest = *std::min_element(d.begin(), d.end());
    for (const Vec2& q : v)
        if (!contains(p.pts, q) || boundaryDistance(p.pts, q) < nearest - 1.0)
            return {};
    Polygon out;
    out.pts = v;
    for (size_t k : keep)
        out.tag.push_back(p.tag[k]);
    return out;
}

Polygon inset(const Polygon& p, std::span<const double> d)
{
    const size_t n = p.pts.size();
    if (n < 3)
        return {};
    std::vector<Vec2> dir(n), nrm(n);
    double maxD = 0.0;
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2 e = p.pts[(k + 1) % n] - p.pts[k];
        const double len = glm::length(e);
        dir[k] = len > 1e-12 ? e / len : Vec2(1.0, 0.0);
        nrm[k] = leftNormal(dir[k]);
        maxD = std::max(maxD, d[k]);
    }
    Polygon out;
    out.tag = p.tag;
    bool ok = true;
    for (size_t k = 0; k < n; ++k)
    {
        const size_t j = (k + n - 1) % n;
        const Vec2 pj = p.pts[j] + nrm[j] * d[j], pk = p.pts[k] + nrm[k] * d[k];
        const double den = cross(dir[j], dir[k]);
        Vec2 v = pk;
        if (std::abs(den) > 1e-9)
            v = pj + dir[j] * (cross(pk - pj, dir[k]) / den);
        if (glm::length(v - p.pts[k]) > 4.0 * maxD + 1e-6)
            ok = false;
        out.pts.push_back(v);
    }
    if (ok)
        for (size_t k = 0; k < n && ok; ++k)
            ok = glm::dot(out.pts[(k + 1) % n] - out.pts[k], dir[k]) > 0.0;
    if (ok && signedArea(out.pts) > 1e-6 && !selfIntersects(out.pts))
        return out;
    // A short side between bends turns back once moved in; clipping by every side would cut a concave face away.
    if (Polygon dropped = insetDropping(p, d); !dropped.pts.empty())
        return dropped;

    Polygon cut = p;
    for (size_t k = 0; k < n && !cut.pts.empty(); ++k)
        cut = clipTagged(cut, p.pts[k] + nrm[k] * d[k], nrm[k], p.tag[k]);
    return cut;
}

Polygon makePolygon(std::vector<Vec2> pts, int tag)
{
    Polygon p;
    p.pts = std::move(pts);
    p.tag.assign(p.pts.size(), tag);
    dropDuplicates(p, 1e-3);
    for (size_t k = 0; k < p.pts.size() && p.pts.size() > 3;)
    {
        const size_t n = p.pts.size();
        const Vec2 a = p.pts[(k + n - 1) % n], b = p.pts[k], c = p.pts[(k + 1) % n];
        const double len = glm::length(c - a);
        if (len > 1e-9 && std::abs(cross(b - a, c - a)) / len < 1e-3)
        {
            p.pts.erase(p.pts.begin() + long(k));
            p.tag.erase(p.tag.begin() + long(k));
        }
        else
            ++k;
    }
    if (p.pts.size() < 3)
        return {};
    if (signedArea(p.pts) < 0.0)
        p = reversed(p);
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------

Ground Ground::plane(double height)
{
    Ground g;
    g.mPlane = height;
    return g;
}

/** Whether every face is an axis-aligned quad of one size on a grid from origin, all split from the same corner pair. */
static bool findLattice(const geo::Mesh& mesh, Vec2 origin, Ground::Lattice& out)
{
    bool first = true;
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const std::vector<geo::Index> pts = mesh.facePoints(f);
        if (pts.size() != 4)
            return false;
        glm::dvec3 c[4];
        for (size_t k = 0; k < 4; ++k)
            c[k] = mesh.position(pts[k]);
        Vec2 lo(std::min({c[0].x, c[1].x, c[2].x, c[3].x}), std::min({c[0].z, c[1].z, c[2].z, c[3].z}));
        Vec2 hi(std::max({c[0].x, c[1].x, c[2].x, c[3].x}), std::max({c[0].z, c[1].z, c[2].z, c[3].z}));
        const Vec2 size = hi - lo;
        // The split runs from corner 0 to corner 2: the main diagonal when they differ the same way in x and z.
        const bool main = (c[0].x - c[2].x) * (c[0].z - c[2].z) > 0.0;
        if (first)
        {
            if (size.x <= 1e-3 || size.y <= 1e-3)
                return false;
            out = {origin, size, main};
            first = false;
        }
        const double tol = 1e-3 * std::min(out.cell.x, out.cell.y);
        if (std::abs(size.x - out.cell.x) > tol || std::abs(size.y - out.cell.y) > tol || main != out.mainDiagonal)
            return false;
        for (size_t k = 0; k < 4; ++k)
            if ((std::abs(c[k].x - lo.x) > tol && std::abs(c[k].x - hi.x) > tol) || (std::abs(c[k].z - lo.y) > tol && std::abs(c[k].z - hi.y) > tol))
                return false;
        const Vec2 at = (lo - origin) / out.cell;
        if (std::abs(at.x - std::round(at.x)) * out.cell.x > tol || std::abs(at.y - std::round(at.y)) * out.cell.y > tol)
            return false;
    }
    return !first;
}

Ground Ground::fromMesh(const geo::Mesh& mesh, double cell, int maxSide)
{
    LILA_ZONE();

    Ground g;
    const geo::Bounds b = mesh.bounds();
    if (!b.valid() || mesh.numFaces() == 0)
        return g;
    const double ex = b.max.x - b.min.x, ez = b.max.z - b.min.z;
    const double side = double(std::max(maxSide, 2) - 1);
    g.mCell = std::max({cell, ex / side, ez / side, 1e-3});
    g.mOrigin = {b.min.x, b.min.z};
    g.mW = int(std::ceil(ex / g.mCell)) + 1;
    g.mH = int(std::ceil(ez / g.mCell)) + 1;
    g.mHeights.assign(size_t(g.mW) * size_t(g.mH), std::numeric_limits<float>::quiet_NaN());
    // Each triangle writes the grid points under it; the highest surface wins, as a ray cast down would find.
    auto put = [&](const glm::dvec3& a, const glm::dvec3& c1, const glm::dvec3& c2) {
        const double den = (c1.z - c2.z) * (a.x - c2.x) + (c2.x - c1.x) * (a.z - c2.z);
        if (std::abs(den) < 1e-12)
            return;
        const double lx = std::min({a.x, c1.x, c2.x}), hx = std::max({a.x, c1.x, c2.x});
        const double lz = std::min({a.z, c1.z, c2.z}), hz = std::max({a.z, c1.z, c2.z});
        const int x0 = std::max(0, int(std::ceil((lx - g.mOrigin.x) / g.mCell - 1e-9)));
        const int x1 = std::min(g.mW - 1, int(std::floor((hx - g.mOrigin.x) / g.mCell + 1e-9)));
        const int z0 = std::max(0, int(std::ceil((lz - g.mOrigin.y) / g.mCell - 1e-9)));
        const int z1 = std::min(g.mH - 1, int(std::floor((hz - g.mOrigin.y) / g.mCell + 1e-9)));
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
            {
                const double px = g.mOrigin.x + x * g.mCell, pz = g.mOrigin.y + z * g.mCell;
                const double w0 = ((c1.z - c2.z) * (px - c2.x) + (c2.x - c1.x) * (pz - c2.z)) / den;
                const double w1 = ((c2.z - a.z) * (px - c2.x) + (a.x - c2.x) * (pz - c2.z)) / den;
                const double w2 = 1.0 - w0 - w1;
                if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9)
                    continue;
                const float y = float(w0 * a.y + w1 * c1.y + w2 * c2.y);
                float& h = g.mHeights[size_t(z) * size_t(g.mW) + size_t(x)];
                if (std::isnan(h) || y > h)
                    h = y;
            }
    };
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const std::vector<geo::Index> pts = mesh.facePoints(f);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
            put(mesh.position(pts[0]), mesh.position(pts[k]), mesh.position(pts[k + 1]));
    }
    // Quads split from their first corner, as the grid above and the heightfield renderer split them: a ribbon draped on the
    // shorter diagonal sinks under the terrain drawn wherever the two differ.
    g.mHasLattice = findLattice(mesh, {b.min.x, b.min.z}, g.mLattice);
    geo::TriangulateOptions split;
    split.quads = geo::QuadSplit::FirstCorner;
    auto bvh = std::make_shared<geo::MeshBvh>();
    bvh->build(mesh, geo::triangulate(mesh, split));
    g.mBvh = std::move(bvh);
    g.mTop = b.max.y + 100.0;
    g.mDepth = (b.max.y - b.min.y) + 200.0;
    g.mPlane = b.min.y;
    return g;
}

double Ground::surfaceHeight(Vec2 p) const
{
    if (!mBvh)
        return height(p);
    geo::BvhHit hit;
    if (mBvh->raycast({p.x, mTop, p.y}, {0.0, -1.0, 0.0}, hit, mDepth))
        return hit.point.y;
    return height(p);
}

double Ground::height(Vec2 p) const
{
    if (mHeights.empty())
        return mPlane;
    const double fx = std::clamp((p.x - mOrigin.x) / mCell, 0.0, double(mW - 1));
    const double fz = std::clamp((p.y - mOrigin.y) / mCell, 0.0, double(mH - 1));
    const int x0 = std::min(int(fx), mW - 2 < 0 ? 0 : mW - 2), z0 = std::min(int(fz), mH - 2 < 0 ? 0 : mH - 2);
    const int x1 = std::min(x0 + 1, mW - 1), z1 = std::min(z0 + 1, mH - 1);
    const double tx = fx - x0, tz = fz - z0;
    const float h[4] = {mHeights[size_t(z0) * mW + x0], mHeights[size_t(z0) * mW + x1], mHeights[size_t(z1) * mW + x0],
                        mHeights[size_t(z1) * mW + x1]};
    const double w[4] = {(1 - tx) * (1 - tz), tx * (1 - tz), (1 - tx) * tz, tx * tz};
    double sum = 0.0, wsum = 0.0;
    for (int k = 0; k < 4; ++k)
        if (!std::isnan(h[k]))
        {
            sum += h[k] * w[k];
            wsum += w[k];
        }
    if (wsum <= 1e-12)
    {
        for (float v : h)
            if (!std::isnan(v))
                return v;
        return mPlane;
    }
    return sum / wsum;
}

double Ground::grade(Vec2 p) const
{
    if (mHeights.empty())
        return 0.0;
    const double c = mCell;
    const double gx = (height(p + Vec2(c, 0)) - height(p - Vec2(c, 0))) / (2.0 * c);
    const double gz = (height(p + Vec2(0, c)) - height(p - Vec2(0, c))) / (2.0 * c);
    return std::sqrt(gx * gx + gz * gz);
}

double Ground::gradeAlong(Vec2 a, Vec2 b) const
{
    if (mHeights.empty())
        return 0.0;
    const double len = glm::length(b - a);
    if (len < 1e-6)
        return 0.0;
    const int steps = std::clamp(int(len / mCell), 1, 16);
    double worst = 0.0, prev = height(a);
    for (int k = 1; k <= steps; ++k)
    {
        const double h = height(a + (b - a) * (double(k) / steps));
        worst = std::max(worst, std::abs(h - prev) / (len / steps));
        prev = h;
    }
    return worst;
}

bool Ground::solid(Vec2 p) const
{
    if (mHeights.empty())
        return true;
    const double fx = (p.x - mOrigin.x) / mCell, fz = (p.y - mOrigin.y) / mCell;
    if (fx < 0.0 || fz < 0.0 || fx > mW - 1 || fz > mH - 1)
        return false;
    const int x = int(std::lround(fx)), z = int(std::lround(fz));
    return !std::isnan(mHeights[size_t(z) * mW + x]);
}

uint64_t hash64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

double unitRandom(uint64_t k) { return double(hash64(k) >> 11) * (1.0 / 9007199254740992.0); }
} // namespace settlement
