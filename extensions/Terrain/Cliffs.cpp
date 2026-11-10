// Cliffs and slopes: slope regions, cliff meshes shaped by strata and noise, the ground edits under them, extruded walls,
// kit fitting, cutting on a World Partition grid, tiered scatter and the slope-aware texture projection.

#include "Cliffs.h"

#include "Analysis.h"

#include <lila/MeshOps.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

using namespace lila;

namespace terrain::cliffs
{
using json = nlohmann::json;

namespace
{
constexpr double kPi = 3.141592653589793;
constexpr double kDeg = kPi / 180.0;

double smooth01(double a, double b, double x)
{
    if (b <= a)
        return x >= b ? 1.0 : 0.0;
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

glm::dvec3 up() { return {0.0, 1.0, 0.0}; }

uint64_t edgeKey(uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; }

glm::dvec3 horizontal(const glm::dvec3& v, const glm::dvec3& fallback)
{
    const glm::dvec3 hz(v.x, 0.0, v.z);
    const double l = glm::length(hz);
    return l > 1e-9 ? hz / l : fallback;
}

/** Gradient (dh/dx, dh/dz) at a sample from central differences, one-sided at the edges. */
glm::dvec2 gradientAt(const Heightfield& h, int x, int z)
{
    const int x0 = std::max(0, x - 1), x1 = std::min(h.nx - 1, x + 1);
    const int z0 = std::max(0, z - 1), z1 = std::min(h.nz - 1, z + 1);
    const double dx = (h.at(x1, z) - h.at(x0, z)) / (std::max(1, x1 - x0) * h.cell);
    const double dz = (h.at(x, z1) - h.at(x, z0)) / (std::max(1, z1 - z0) * h.cell);
    return {dx, dz};
}

bool insideRings(const std::vector<std::vector<glm::dvec2>>& rings, glm::dvec2 p, double tol = 0.0)
{
    // Outlines run along the grid's edge samples, so points on a ring count as inside.
    if (tol > 0.0)
        for (const auto& r : rings)
            for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
            {
                const glm::dvec2 a = r[j], d = r[i] - r[j];
                const double l2 = glm::dot(d, d);
                const double t = l2 > 0.0 ? std::clamp(glm::dot(p - a, d) / l2, 0.0, 1.0) : 0.0;
                if (glm::length(p - (a + d * t)) <= tol)
                    return true;
            }
    bool in = false;
    for (const auto& r : rings)
        for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
            if ((r[i].y > p.y) != (r[j].y > p.y) && p.x < (r[j].x - r[i].x) * (p.y - r[i].y) / (r[j].y - r[i].y) + r[i].x)
                in = !in;
    return in;
}

void simplifyRange(const std::vector<glm::dvec2>& r, size_t a, size_t b, double tol, std::vector<uint8_t>& keep)
{
    if (b <= a + 1)
        return;
    const glm::dvec2 p = r[a], q = r[b % r.size()];
    const glm::dvec2 d = q - p;
    const double l = glm::length(d);
    double worst = -1.0;
    size_t at = a;
    for (size_t i = a + 1; i < b; ++i)
    {
        const glm::dvec2 v = r[i] - p;
        const double dist = l > 1e-12 ? std::abs(d.x * v.y - d.y * v.x) / l : glm::length(v);
        if (dist > worst)
        {
            worst = dist;
            at = i;
        }
    }
    if (worst > tol)
    {
        keep[at] = 1;
        simplifyRange(r, a, at, tol, keep);
        simplifyRange(r, at, b, tol, keep);
    }
}

std::vector<glm::dvec2> simplifyRing(const std::vector<glm::dvec2>& r, double tol)
{
    if (r.size() < 8)
        return r;
    std::vector<uint8_t> keep(r.size(), 0);
    const size_t half = r.size() / 2;
    keep[0] = keep[half] = 1;
    simplifyRange(r, 0, half, tol, keep);
    simplifyRange(r, half, r.size(), tol, keep);
    std::vector<glm::dvec2> out;
    for (size_t i = 0; i < r.size(); ++i)
        if (keep[i])
            out.push_back(r[i]);
    return out.size() >= 3 ? out : r;
}

double ringArea(const std::vector<glm::dvec2>& r)
{
    double a = 0.0;
    for (size_t i = 0; i < r.size(); ++i)
    {
        const glm::dvec2 p = r[i], q = r[(i + 1) % r.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

std::vector<uint8_t> dilate(const Heightfield& h, std::vector<uint8_t> set, int steps)
{
    for (int s = 0; s < steps; ++s)
    {
        std::vector<uint8_t> next = set;
        for (int z = 0; z < h.nz; ++z)
            for (int x = 0; x < h.nx; ++x)
            {
                if (set[h.index(x, z)])
                    continue;
                for (int dz = -1; dz <= 1 && !next[h.index(x, z)]; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int xx = x + dx, zz = z + dz;
                        if (xx >= 0 && zz >= 0 && xx < h.nx && zz < h.nz && set[h.index(xx, zz)])
                        {
                            next[h.index(x, z)] = 1;
                            break;
                        }
                    }
            }
        set = std::move(next);
    }
    return set;
}

std::vector<float> chamfer(const Heightfield& h, std::vector<float> d)
{
    const float a = float(h.cell), b = float(h.cell * std::sqrt(2.0));
    auto relax = [&](int x, int z, int dx, int dz, float w) {
        const int xx = x + dx, zz = z + dz;
        if (xx < 0 || zz < 0 || xx >= h.nx || zz >= h.nz)
            return;
        float& v = d[h.index(x, z)];
        v = std::min(v, d[h.index(xx, zz)] + w);
    };
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            relax(x, z, -1, 0, a);
            relax(x, z, 0, -1, a);
            relax(x, z, -1, -1, b);
            relax(x, z, 1, -1, b);
        }
    for (int z = h.nz - 1; z >= 0; --z)
        for (int x = h.nx - 1; x >= 0; --x)
        {
            relax(x, z, 1, 0, a);
            relax(x, z, 0, 1, a);
            relax(x, z, 1, 1, b);
            relax(x, z, -1, 1, b);
        }
    return d;
}
} // namespace

// ---------------------------------------------------------------------------------------------------- shared

uint32_t hash3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x8da6b343u ^ b * 0xd8163841u ^ c * 0xcb1ab31fu ^ 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

double unit(uint32_t h) { return double(h >> 8) / double(1u << 24); }

double noise3(const glm::dvec3& p, uint32_t seed)
{
    const glm::dvec3 f = glm::floor(p);
    const glm::dvec3 t = p - f;
    const glm::dvec3 s = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    const int x = int(f.x), y = int(f.y), z = int(f.z);
    auto grad = [&](int i, int j, int k) {
        const uint32_t hh = hash3(uint32_t(x + i) + seed * 31u, uint32_t(y + j), uint32_t(z + k));
        const double th = unit(hh) * 2.0 * kPi, ph = std::acos(2.0 * unit(hash3(hh, 7u, seed)) - 1.0);
        const glm::dvec3 g(std::sin(ph) * std::cos(th), std::cos(ph), std::sin(ph) * std::sin(th));
        return glm::dot(g, t - glm::dvec3(i, j, k));
    };
    auto lerp = [](double a, double b, double w) { return a + (b - a) * w; };
    const double x00 = lerp(grad(0, 0, 0), grad(1, 0, 0), s.x), x10 = lerp(grad(0, 1, 0), grad(1, 1, 0), s.x);
    const double x01 = lerp(grad(0, 0, 1), grad(1, 0, 1), s.x), x11 = lerp(grad(0, 1, 1), grad(1, 1, 1), s.x);
    return std::clamp(1.6 * lerp(lerp(x00, x10, s.y), lerp(x01, x11, s.y), s.z), -1.0, 1.0);
}

double fbm3(const glm::dvec3& p, uint32_t seed, int octaves)
{
    double sum = 0.0, amp = 0.5, norm = 0.0;
    glm::dvec3 q = p;
    for (int o = 0; o < octaves; ++o)
    {
        sum += amp * noise3(q, seed + uint32_t(o) * 101u);
        norm += amp;
        amp *= 0.5;
        q = q * 2.03 + glm::dvec3(17.1, 3.7, 9.2);
    }
    return sum / norm;
}

json ringsJson(const std::vector<std::vector<glm::dvec2>>& rings)
{
    json out = json::array();
    for (const auto& r : rings)
    {
        json ring = json::array();
        for (const glm::dvec2& p : r)
            ring.push_back(json::array({p.x, p.y}));
        out.push_back(ring);
    }
    return out;
}

std::vector<std::vector<glm::dvec2>> ringsFrom(const json& j)
{
    std::vector<std::vector<glm::dvec2>> out;
    if (!j.is_array())
        return out;
    for (const json& r : j)
    {
        std::vector<glm::dvec2> ring;
        for (const json& p : r)
            if (p.is_array() && p.size() >= 2)
                ring.push_back({p[0].get<double>(), p[1].get<double>()});
        if (ring.size() >= 3)
            out.push_back(std::move(ring));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------- C0 regions

std::vector<float> slopeDegrees(const Heightfield& h)
{
    std::vector<float> s(h.size(), 0.0f);
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
            s[h.index(x, z)] = float(std::atan(glm::length(gradientAt(h, x, z))) / kDeg);
    });
    return s;
}

std::vector<float> distanceInside(const Heightfield& h, const std::vector<uint8_t>& set)
{
    std::vector<float> d(h.size(), std::numeric_limits<float>::infinity());
    for (size_t i = 0; i < d.size(); ++i)
        if (!set[i])
            d[i] = 0.0f;
    d = chamfer(h, std::move(d));
    // A set that fills the grid has no outside: its edge samples count as the border.
    for (float& v : d)
        if (!std::isfinite(v))
            v = float(h.cell * std::max(h.nx, h.nz));
    return d;
}

std::vector<float> distanceTo(const Heightfield& h, const std::vector<uint8_t>& set)
{
    std::vector<float> d(h.size(), std::numeric_limits<float>::infinity());
    for (size_t i = 0; i < d.size(); ++i)
        if (set[i])
            d[i] = 0.0f;
    return chamfer(h, std::move(d));
}

std::vector<uint8_t> ringSamples(const Heightfield& h, const std::vector<std::vector<glm::dvec2>>& rings, int margin)
{
    std::vector<uint8_t> set(h.size(), 0);
    if (rings.empty())
        return set;
    glm::dvec2 lo(1e300), hi(-1e300);
    for (const auto& r : rings)
        for (const glm::dvec2& p : r)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    const int x0 = std::max(0, int(std::floor((lo.x - h.origin.x) / h.cell))), x1 = std::min(h.nx - 1, int(std::ceil((hi.x - h.origin.x) / h.cell)));
    const int z0 = std::max(0, int(std::floor((lo.y - h.origin.y) / h.cell))), z1 = std::min(h.nz - 1, int(std::ceil((hi.y - h.origin.y) / h.cell)));
    tbb::parallel_for(z0, z1 + 1, [&](int z) {
        for (int x = x0; x <= x1; ++x)
            if (insideRings(rings, h.samplePos(x, z), 1e-6 * h.cell))
                set[h.index(x, z)] = 1;
    });
    return margin > 0 ? dilate(h, std::move(set), margin) : set;
}

double sampleGrid(const Heightfield& h, const std::vector<float>& g, double x, double z)
{
    const double fx = std::clamp((x - h.origin.x) / h.cell, 0.0, double(h.nx - 1)), fz = std::clamp((z - h.origin.y) / h.cell, 0.0, double(h.nz - 1));
    const int x0 = std::min(int(fx), h.nx - 2), z0 = std::min(int(fz), h.nz - 2);
    const double tx = fx - x0, tz = fz - z0;
    auto v = [&](int a, int b) { return double(g[h.index(a, b)]); };
    return (v(x0, z0) * (1 - tx) + v(x0 + 1, z0) * tx) * (1 - tz) + (v(x0, z0 + 1) * (1 - tx) + v(x0 + 1, z0 + 1) * tx) * tz;
}

std::vector<Region> findRegions(const Heightfield& h, const RegionParams& p, size_t* dropped)
{
    LILA_ZONE();

    if (dropped)
        *dropped = 0;
    std::vector<Region> out;
    if (h.empty())
        return out;
    const std::vector<float> slope = slopeDegrees(h);
    std::vector<uint8_t> steep(h.size(), 0);
    for (size_t i = 0; i < steep.size(); ++i)
        steep[i] = slope[i] >= float(p.slope);
    // Closing joins patches closer than the merge distance: grow by it, then shrink back.
    std::vector<uint8_t> closed = steep;
    if (p.mergeDistance > 0.0)
    {
        const std::vector<float> to = distanceTo(h, steep);
        std::vector<uint8_t> grown(h.size(), 0);
        for (size_t i = 0; i < grown.size(); ++i)
            grown[i] = to[i] <= float(0.5 * p.mergeDistance);
        const std::vector<float> in = distanceInside(h, grown);
        for (size_t i = 0; i < closed.size(); ++i)
            closed[i] = steep[i] || in[i] > float(0.5 * p.mergeDistance + 1e-6);
    }
    std::vector<int32_t> label;
    const int n = components(closed, h.nx, h.nz, label);
    std::vector<std::vector<size_t>> comps(static_cast<size_t>(n));
    for (size_t i = 0; i < label.size(); ++i)
        if (label[i] >= 0)
            comps[size_t(label[i])].push_back(i);
    std::stable_sort(comps.begin(), comps.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    const double cellM2 = h.cell * h.cell * 1e-4;
    const double dip = p.strataDip * kDeg;
    for (auto& cells : comps)
    {
        double lo = 1e300, hi = -1e300, slopeSum = 0.0, slopeMax = 0.0;
        glm::dvec2 c(0.0), downhill(0.0);
        for (size_t i : cells)
        {
            const int x = int(i % size_t(h.nx)), z = int(i / size_t(h.nx));
            lo = std::min(lo, double(h.heights[i]));
            hi = std::max(hi, double(h.heights[i]));
            slopeSum += slope[i];
            slopeMax = std::max(slopeMax, double(slope[i]));
            c += h.samplePos(x, z);
            downhill -= gradientAt(h, x, z);
        }
        const double area = double(cells.size()) * cellM2;
        if (area < p.minArea || hi - lo < p.minHeight || int(out.size()) >= p.maxRegions)
        {
            if (dropped)
                ++*dropped;
            continue;
        }
        Region r;
        r.cells = std::move(cells);
        r.centroid = c / double(r.cells.size());
        r.areaM2 = area;
        r.low = lo;
        r.high = hi;
        r.meanSlope = slopeSum / double(r.cells.size());
        r.maxSlope = slopeMax;
        r.facing = horizontal(glm::dvec3(downhill.x, 0.0, downhill.y), glm::dvec3(1.0, 0.0, 0.0));
        r.facingDegrees = std::fmod(std::atan2(r.facing.x, -r.facing.z) / kDeg + 360.0, 360.0);
        r.strata = glm::normalize(up() * std::cos(dip) + r.facing * std::sin(dip));
        double best = 1e300;
        for (size_t i : r.cells)
            if (const double d = glm::length(h.samplePos(int(i % size_t(h.nx)), int(i / size_t(h.nx))) - r.centroid); d < best)
            {
                best = d;
                r.point = h.samplePos(int(i % size_t(h.nx)), int(i / size_t(h.nx)));
            }
        std::vector<uint8_t> one(h.size(), 0);
        for (size_t i : r.cells)
            one[i] = 1;
        for (auto& ring : outlines(h, one))
        {
            ring = simplifyRing(ring, h.cell * 0.3);
            if (ring.size() >= 3)
                r.rings.push_back(std::move(ring));
        }
        std::stable_sort(r.rings.begin(), r.rings.end(), [](const auto& a, const auto& b) { return std::abs(ringArea(a)) > std::abs(ringArea(b)); });
        if (r.rings.empty())
            continue;
        out.push_back(std::move(r));
    }
    for (size_t k = 0; k < out.size(); ++k)
        out[k].name = "cliff_" + std::to_string(k + 1);
    return out;
}

// ---------------------------------------------------------------------------------------------------- triangle meshes

size_t TriMesh::addVertex(const glm::dvec3& q)
{
    p.push_back(q);
    va.resize(va.size() + size_t(vertexStride), 0.0f);
    return p.size() - 1;
}

int TriMesh::vertexIndex(const std::string& name) const
{
    for (size_t i = 0; i < vertexNames.size(); ++i)
        if (vertexNames[i] == name)
            return int(i);
    return -1;
}

int TriMesh::cornerIndex(const std::string& name) const
{
    for (size_t i = 0; i < cornerNames.size(); ++i)
        if (cornerNames[i] == name)
            return int(i);
    return -1;
}

int TriMesh::addVertexAttr(const std::string& name, int n, float fill)
{
    if (const int at = vertexIndex(name); at >= 0)
        return at;
    const int first = vertexStride;
    std::vector<float> next(p.size() * size_t(vertexStride + n), fill);
    for (size_t v = 0; v < p.size(); ++v)
        std::copy_n(va.data() + v * size_t(vertexStride), vertexStride, next.data() + v * size_t(vertexStride + n));
    va = std::move(next);
    vertexStride += n;
    for (int k = 0; k < n; ++k)
        vertexNames.push_back(n == 1 ? name : k == 0 ? name : name + "." + std::to_string(k));
    return first;
}

int TriMesh::addCornerAttr(const std::string& name, int n, float fill)
{
    if (const int at = cornerIndex(name); at >= 0)
        return at;
    const int first = cornerStride;
    const size_t corners = t.size() * 3;
    std::vector<float> next(corners * size_t(cornerStride + n), fill);
    for (size_t c = 0; c < corners; ++c)
        std::copy_n(ca.data() + c * size_t(cornerStride), cornerStride, next.data() + c * size_t(cornerStride + n));
    ca = std::move(next);
    cornerStride += n;
    for (int k = 0; k < n; ++k)
        cornerNames.push_back(n == 1 ? name : k == 0 ? name : name + "." + std::to_string(k));
    return first;
}

geo::Bounds TriMesh::bounds() const
{
    geo::Bounds b;
    for (const glm::dvec3& q : p)
        b.extend(q);
    return b;
}

namespace
{
/** A corner of a triangle being split: a vertex and its barycentric weights in the source triangle. */
struct LocalPoint
{
    uint32_t v;
    glm::dvec3 bary;
};

void emit(TriMesh& out, std::vector<float>& outCa, const TriMesh& in, size_t srcTri, const LocalPoint& a, const LocalPoint& b, const LocalPoint& c)
{
    out.t.push_back({a.v, b.v, c.v});
    const int cs = in.cornerStride;
    if (cs == 0)
        return;
    const float* s0 = in.cattr(srcTri, 0);
    const float* s1 = in.cattr(srcTri, 1);
    const float* s2 = in.cattr(srcTri, 2);
    for (const LocalPoint* lp : {&a, &b, &c})
        for (int k = 0; k < cs; ++k)
            outCa.push_back(float(lp->bary.x * s0[k] + lp->bary.y * s1[k] + lp->bary.z * s2[k]));
}
} // namespace

void refine(TriMesh& m, double maxEdge, int passes)
{
    LILA_ZONE();

    if (maxEdge <= 0.0)
        return;
    const double max2 = maxEdge * maxEdge;
    for (int pass = 0; pass < passes; ++pass)
    {
        std::unordered_map<uint64_t, uint32_t> mids;
        std::vector<uint64_t> order;
        for (const glm::uvec3& tri : m.t)
            for (int e = 0; e < 3; ++e)
            {
                const uint32_t a = tri[e], b = tri[(e + 1) % 3];
                const glm::dvec3 d = m.p[a] - m.p[b];
                if (glm::dot(d, d) > max2)
                {
                    const uint64_t k = edgeKey(a, b);
                    if (mids.emplace(k, 0u).second)
                        order.push_back(k);
                }
            }
        if (order.empty())
            return;
        for (uint64_t k : order)
        {
            const uint32_t a = uint32_t(k >> 32), b = uint32_t(k & 0xffffffffu);
            const size_t v = m.addVertex(0.5 * (m.p[a] + m.p[b]));
            for (int s = 0; s < m.vertexStride; ++s)
                m.vattr(v)[s] = 0.5f * (m.vattr(a)[s] + m.vattr(b)[s]);
            mids[k] = uint32_t(v);
        }
        TriMesh next;
        next.p = std::move(m.p);
        next.va = std::move(m.va);
        next.vertexStride = m.vertexStride;
        next.cornerStride = m.cornerStride;
        next.vertexNames = m.vertexNames;
        next.cornerNames = m.cornerNames;
        std::vector<float> nextCa;
        nextCa.reserve(m.ca.size() * 2);
        next.t.reserve(m.t.size() * 2);
        for (size_t ti = 0; ti < m.t.size(); ++ti)
        {
            const glm::uvec3 tri = m.t[ti];
            LocalPoint corner[3] = {{tri[0], {1, 0, 0}}, {tri[1], {0, 1, 0}}, {tri[2], {0, 0, 1}}};
            LocalPoint mid[3];
            int mask = 0;
            for (int e = 0; e < 3; ++e)
            {
                auto it = mids.find(edgeKey(tri[e], tri[(e + 1) % 3]));
                if (it != mids.end())
                {
                    mask |= 1 << e;
                    mid[e] = {it->second, 0.5 * (corner[e].bary + corner[(e + 1) % 3].bary)};
                }
            }
            const int count = (mask & 1) + ((mask >> 1) & 1) + ((mask >> 2) & 1);
            if (count == 0)
            {
                emit(next, nextCa, m, ti, corner[0], corner[1], corner[2]);
                continue;
            }
            if (count == 3)
            {
                emit(next, nextCa, m, ti, corner[0], mid[0], mid[2]);
                emit(next, nextCa, m, ti, mid[0], corner[1], mid[1]);
                emit(next, nextCa, m, ti, mid[2], mid[1], corner[2]);
                emit(next, nextCa, m, ti, mid[0], mid[1], mid[2]);
                continue;
            }
            // Rotate so the split edges come first: e0 for one, e0 and e1 for two.
            int r = 0;
            if (count == 1)
                r = (mask & 1) ? 0 : (mask & 2) ? 1 : 2;
            else
                r = !(mask & 4) ? 0 : !(mask & 1) ? 1 : 2;
            const LocalPoint c0 = corner[r], c1 = corner[(r + 1) % 3], c2 = corner[(r + 2) % 3];
            const LocalPoint m0 = mid[r], m1 = mid[(r + 1) % 3];
            if (count == 1)
            {
                emit(next, nextCa, m, ti, c0, m0, c2);
                emit(next, nextCa, m, ti, m0, c1, c2);
            }
            else
            {
                emit(next, nextCa, m, ti, m0, c1, m1);
                const double d1 = glm::length(next.p[c0.v] - next.p[m1.v]), d2 = glm::length(next.p[m0.v] - next.p[c2.v]);
                if (d1 <= d2)
                {
                    emit(next, nextCa, m, ti, c0, m0, m1);
                    emit(next, nextCa, m, ti, c0, m1, c2);
                }
                else
                {
                    emit(next, nextCa, m, ti, c0, m0, c2);
                    emit(next, nextCa, m, ti, m0, m1, c2);
                }
            }
        }
        next.ca = std::move(nextCa);
        m = std::move(next);
    }
}

std::vector<glm::dvec3> vertexNormals(const TriMesh& m)
{
    std::vector<glm::dvec3> n(m.p.size(), glm::dvec3(0.0));
    for (const glm::uvec3& t : m.t)
    {
        const glm::dvec3 a = glm::cross(m.p[t[1]] - m.p[t[0]], m.p[t[2]] - m.p[t[0]]);
        n[t[0]] += a;
        n[t[1]] += a;
        n[t[2]] += a;
    }
    for (glm::dvec3& v : n)
    {
        const double l = glm::length(v);
        v = l > 1e-12 ? v / l : up();
    }
    return n;
}

std::vector<uint8_t> borderVertices(const TriMesh& m)
{
    std::unordered_map<uint64_t, int> count;
    count.reserve(m.t.size() * 3);
    for (const glm::uvec3& t : m.t)
        for (int e = 0; e < 3; ++e)
            ++count[edgeKey(t[e], t[(e + 1) % 3])];
    std::vector<uint8_t> out(m.p.size(), 0);
    for (const auto& [k, c] : count)
        if (c == 1)
        {
            out[k >> 32] = 1;
            out[k & 0xffffffffu] = 1;
        }
    return out;
}

TriMesh fromMesh(const geo::Mesh& mesh)
{
    TriMesh out;
    std::vector<uint32_t> remap(mesh.pointCapacity(), ~0u);
    for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
        {
            remap[p] = uint32_t(out.p.size());
            out.p.push_back(mesh.position(p));
        }
    struct Src
    {
        std::string name;
        std::span<const float> f;
        std::span<const glm::vec4> f4;
    };
    std::vector<Src> srcs;
    for (const std::string& name : mesh.pointAttrs().names())
    {
        if (const auto f = mesh.pointAttrs().get<float>(name); !f.empty())
            srcs.push_back({name, f, {}});
        else if (const auto f4 = mesh.pointAttrs().get<glm::vec4>(name); !f4.empty() && name == geo::attr::kColor)
            srcs.push_back({name, {}, f4});
    }
    for (const Src& s : srcs)
        out.addVertexAttr(s.name, s.f4.empty() ? 1 : 4);
    for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
        if (remap[p] != ~0u)
        {
            float* a = out.vattr(remap[p]);
            int k = 0;
            for (const Src& s : srcs)
            {
                if (!s.f4.empty())
                {
                    for (int c = 0; c < 4; ++c)
                        a[k++] = s.f4[p][c];
                }
                else
                    a[k++] = s.f[p];
            }
        }
    const geo::Triangulation tris = geo::triangulate(mesh);
    const auto uv = mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    const auto nrm = mesh.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (!uv.empty())
    {
        out.cornerNames = {"u0", "v0"};
        out.cornerStride = 2;
    }
    if (!nrm.empty())
    {
        out.cornerNames.insert(out.cornerNames.end(), {"nx", "ny", "nz"});
        out.cornerStride += 3;
    }
    for (size_t t = 0; t < tris.faces.size(); ++t)
    {
        glm::uvec3 tri;
        for (int c = 0; c < 3; ++c)
        {
            const geo::Index corner = tris.corners[t * 3 + size_t(c)];
            tri[c] = remap[mesh.cornerPoint(corner)];
            if (!uv.empty())
            {
                out.ca.push_back(uv[corner].x);
                out.ca.push_back(uv[corner].y);
            }
            if (!nrm.empty())
                for (int k = 0; k < 3; ++k)
                    out.ca.push_back(nrm[corner][k]);
        }
        out.t.push_back(tri);
    }
    return out;
}

geo::Mesh toMesh(const TriMesh& m)
{
    geo::Mesh out;
    out.reserve(m.p.size(), m.t.size() * 3, m.t.size(), m.t.size() * 3);
    for (const glm::dvec3& q : m.p)
        out.addPoint(q);
    std::vector<geo::Index> faces;
    faces.reserve(m.t.size());
    for (const glm::uvec3& t : m.t)
    {
        const geo::Index pts[3] = {t[0], t[1], t[2]};
        faces.push_back(out.addFace(pts));
    }
    for (size_t i = 0; i < m.vertexNames.size(); ++i)
    {
        const std::string& name = m.vertexNames[i];
        if (name.find('.') != std::string::npos)
            continue;
        const bool four = i + 3 < m.vertexNames.size() && m.vertexNames[i + 1] == name + ".1";
        if (four)
        {
            auto dst = out.pointAttrs().getOrAdd<glm::vec4>(name);
            for (size_t v = 0; v < m.p.size(); ++v)
                dst[v] = {m.vattr(v)[i], m.vattr(v)[i + 1], m.vattr(v)[i + 2], m.vattr(v)[i + 3]};
        }
        else
        {
            auto dst = out.pointAttrs().getOrAdd<float>(name);
            for (size_t v = 0; v < m.p.size(); ++v)
                dst[v] = m.vattr(v)[i];
        }
    }
    const int u = m.cornerIndex("u0"), nx = m.cornerIndex("nx");
    if (u >= 0 || nx >= 0)
    {
        std::span<glm::vec2> uv = u >= 0 ? out.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0)) : std::span<glm::vec2>{};
        std::span<glm::vec3> nrm = nx >= 0 ? out.cornerAttrs().getOrAdd<glm::vec3>(geo::attr::kNormal) : std::span<glm::vec3>{};
        for (size_t t = 0; t < m.t.size(); ++t)
        {
            if (faces[t] == geo::kInvalid)
                continue;
            for (geo::Index c : out.faceCorners(faces[t]))
            {
                const geo::Index pt = out.cornerPoint(c);
                const int k = pt == m.t[t][0] ? 0 : pt == m.t[t][1] ? 1 : 2;
                const float* a = m.cattr(t, k);
                if (u >= 0)
                    uv[c] = {a[u], a[u + 1]};
                if (nx >= 0)
                    nrm[c] = {a[nx], a[nx + 1], a[nx + 2]};
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------- C1 displacement

double RockMap::sample(double u, double w) const
{
    if (v.empty())
        return 0.5;
    const double fx = (u - std::floor(u)) * width, fy = (w - std::floor(w)) * height;
    const int x0 = int(fx) % width, y0 = int(fy) % height;
    const int x1 = (x0 + 1) % width, y1 = (y0 + 1) % height;
    const double tx = fx - std::floor(fx), ty = fy - std::floor(fy);
    auto at = [&](int x, int y) { return double(v[size_t(y) * size_t(width) + size_t(x)]); };
    return (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
}

namespace
{
/** Layer k's lower boundary in strata units, jittered so layers vary in thickness. */
double layerBase(int k, const Shape& s) { return double(k) + s.strataJitter * (unit(hash3(uint32_t(k + 100000), s.seed, 0x5a71u)) - 0.5); }

/** How far a layer stands out (1 flush with the surface). */
double layerOut(int k, const Shape& s)
{
    const double contrast = 0.15 + 0.6 * std::clamp(s.ledges, 0.0, 1.0);
    return 1.0 - contrast * (1.0 - unit(hash3(uint32_t(k + 100000), s.seed, 0x1ed6u)));
}
} // namespace

DisplaceSample displace(const Shape& s, const RockMap* rock, const glm::dvec3& p, const glm::dvec3& n, double heightFraction)
{
    DisplaceSample out;
    const double layerH = std::max(1.0, s.strataHeight);
    double t = glm::dot(p, s.strata) / layerH;
    t += 0.12 * noise3(p / (layerH * 6.0), s.seed ^ 0x77u);
    int k = int(std::floor(t));
    if (t < layerBase(k, s))
        --k;
    else if (t >= layerBase(k + 1, s))
        ++k;
    const double b0 = layerBase(k, s), b1 = layerBase(k + 1, s);
    out.layer = k;
    out.strataPhase = std::clamp((t - b0) / std::max(1e-6, b1 - b0), 0.0, 1.0);
    const double here = layerOut(k, s), above = layerOut(k + 1, s);
    out.ledgeBelow = here - above > 0.12;
    double v = here * (1.0 - std::clamp(s.undercut, 0.0, 1.0) * (1.0 - smooth01(0.0, 0.45, out.strataPhase)));
    v *= 1.0 - std::clamp(s.overhang, 0.0, 1.0) * 0.6 * (1.0 - std::clamp(heightFraction, 0.0, 1.0));
    if (s.noise > 0.0)
        v += 0.5 * s.noise * fbm3(p / std::max(1.0, s.noiseSize), s.seed ^ 0x3c1u);
    if (rock && !rock->empty() && s.rock > 0.0)
    {
        const glm::dvec3 w = glm::pow(glm::abs(n), glm::dvec3(4.0));
        const double sum = w.x + w.y + w.z + 1e-9;
        const glm::dvec3 q = p / std::max(1.0, s.rockSize);
        const double r = (w.x * rock->sample(q.z, q.y) + w.y * rock->sample(q.x, q.z) + w.z * rock->sample(q.x, q.y)) / sum;
        v -= s.rock * (1.0 - r) * 0.6;
    }
    out.out = std::clamp(v, 0.0, 1.0);
    return out;
}

void bakeMasks(TriMesh& m, const std::vector<DisplaceSample>& samples, const std::vector<glm::dvec3>& baseNormals, const MeshParams& p)
{
    LILA_ZONE();

    const std::vector<glm::dvec3> fn = vertexNormals(m);
    const int weightAt = m.vertexIndex("weight");
    const int heightAt = m.vertexIndex("heightFraction");
    // Concavity: how far a vertex sits behind the mean of its neighbours, along its normal.
    std::vector<glm::dvec3> nsum(m.p.size(), glm::dvec3(0.0));
    std::vector<double> ncount(m.p.size(), 0.0), elen(m.p.size(), 0.0);
    for (const glm::uvec3& t : m.t)
        for (int e = 0; e < 3; ++e)
        {
            const uint32_t a = t[e], b = t[(e + 1) % 3];
            nsum[a] += m.p[b];
            nsum[b] += m.p[a];
            ncount[a] += 1.0;
            ncount[b] += 1.0;
            const double l = glm::length(m.p[a] - m.p[b]);
            elen[a] += l;
            elen[b] += l;
        }
    const int strata = m.addVertexAttr("strata", 1);
    const int cavity = m.addVertexAttr("cavity", 1);
    const int upFacing = m.addVertexAttr("upFacing", 1);
    const int ledge = m.addVertexAttr("ledge", 1);
    const int scatter = m.addVertexAttr("scatter", 1);
    const int talus = m.addVertexAttr("talus", 1);
    const int color = m.addVertexAttr("color", 4, 1.0f);
    for (size_t v = 0; v < m.p.size(); ++v)
    {
        const DisplaceSample& s = v < samples.size() ? samples[v] : DisplaceSample{};
        const double w = weightAt >= 0 ? m.vattr(v)[weightAt] : 1.0;
        const double hf = heightAt >= 0 ? m.vattr(v)[heightAt] : 0.5;
        double concave = 0.0;
        if (ncount[v] > 0.0)
        {
            const glm::dvec3 mean = nsum[v] / ncount[v];
            concave = glm::dot(mean - m.p[v], fn[v]) / std::max(1e-6, elen[v] / ncount[v]);
        }
        const double cav = std::clamp(0.6 * (1.0 - s.out) * w + 1.5 * concave, 0.0, 1.0);
        const double upf = std::clamp((fn[v].y - 0.45) / 0.4, 0.0, 1.0);
        const double led = upf > 0.5 && w > 0.5 && hf > 0.04 && hf < 0.96 ? 1.0 : 0.0;
        const double tal = std::clamp(1.0 - hf / 0.12, 0.0, 1.0) * std::clamp((fn[v].y - 0.2) / 0.4, 0.0, 1.0);
        const double sca = std::clamp(std::max(led, tal) * (1.0 - 0.5 * cav), 0.0, 1.0);
        float* a = m.vattr(v);
        a[strata] = float(s.strataPhase);
        a[cavity] = float(cav);
        a[upFacing] = float(upf);
        a[ledge] = float(led);
        a[scatter] = float(sca);
        a[talus] = float(tal);
        a[color + 0] = float(s.strataPhase);
        a[color + 1] = float(1.0 - cav);
        a[color + 2] = float(upf);
        a[color + 3] = float(sca);
    }
    (void)baseNormals;

    // uv0: projected along the cliff's facing, each layer its own island so the projection's stretch stays per band.
    const glm::dvec3 f = horizontal(p.shape.facing, glm::dvec3(1.0, 0.0, 0.0));
    const glm::dvec3 tangent = glm::normalize(glm::cross(up(), f));
    double ulo = 1e300, uhi = -1e300;
    for (const glm::dvec3& q : m.p)
    {
        ulo = std::min(ulo, glm::dot(q, tangent));
        uhi = std::max(uhi, glm::dot(q, tangent));
    }
    const double scale = 1.0 / std::max(1.0, p.uvScale);
    const double span = (uhi - ulo) * scale + 0.05;
    int minLayer = std::numeric_limits<int>::max();
    for (const glm::uvec3& t : m.t)
        for (int c = 0; c < 3; ++c)
            if (t[c] < samples.size())
                minLayer = std::min(minLayer, samples[t[c]].layer);
    if (minLayer == std::numeric_limits<int>::max())
        minLayer = 0;
    const int u0 = m.addCornerAttr("u0", 2);
    const int nx = m.addCornerAttr("nx", 3);
    for (size_t ti = 0; ti < m.t.size(); ++ti)
    {
        const glm::uvec3 tri = m.t[ti];
        int layer = 0;
        if (tri[0] < samples.size())
        {
            std::array<int, 3> l = {samples[tri[0]].layer, samples[tri[1]].layer, samples[tri[2]].layer};
            std::sort(l.begin(), l.end());
            layer = l[1] - minLayer;
        }
        for (int c = 0; c < 3; ++c)
        {
            const glm::dvec3 q = m.p[tri[c]];
            float* a = m.cattr(ti, c);
            a[u0] = float((glm::dot(q, tangent) - ulo) * scale + double(layer) * span);
            a[u0 + 1] = float(q.y * scale);
            a[nx] = float(fn[tri[c]].x);
            a[nx + 1] = float(fn[tri[c]].y);
            a[nx + 2] = float(fn[tri[c]].z);
        }
    }
}

Footprint footprintOf(const Heightfield& h, const std::vector<std::vector<glm::dvec2>>& rings, const MeshParams& p)
{
    Footprint f;
    f.cover = ringSamples(h, rings, std::max(0, p.margin));
    const std::vector<float> in = distanceInside(h, f.cover);
    f.weight.assign(h.size(), 0.0f);
    f.low = 1e300;
    f.high = -1e300;
    for (size_t i = 0; i < h.size(); ++i)
        if (f.cover[i])
        {
            // The grid's own edge is an edge of the mesh too: the shape fades out there.
            const int x = int(i % size_t(h.nx)), z = int(i / size_t(h.nx));
            const double edge = std::min({x, z, h.nx - 1 - x, h.nz - 1 - z}) * h.cell;
            f.weight[i] = float(smooth01(0.0, std::max(1e-3, p.blend), std::min(double(in[i]), edge)));
            f.low = std::min(f.low, double(h.heights[i]));
            f.high = std::max(f.high, double(h.heights[i]));
        }
    if (f.low > f.high)
        f.low = f.high = 0.0;
    return f;
}

namespace
{
/** Displaces the refined base mesh: in along its normals by depth times the shape's weight, then the shape back out. */
std::vector<DisplaceSample> displaceMesh(TriMesh& m, const MeshParams& p, const RockMap* rock, std::vector<glm::dvec3>& baseNormals)
{
    baseNormals = vertexNormals(m);
    const int weightAt = m.vertexIndex("weight");
    const int heightAt = m.vertexIndex("heightFraction");
    std::vector<DisplaceSample> samples(m.p.size());
    tbb::parallel_for(size_t(0), m.p.size(), [&](size_t v) {
        const double w = weightAt >= 0 ? m.vattr(v)[weightAt] : 1.0;
        const double hf = heightAt >= 0 ? m.vattr(v)[heightAt] : 0.5;
        samples[v] = displace(p.shape, rock, m.p[v], baseNormals[v], hf);
        m.p[v] -= baseNormals[v] * (p.shape.depth * w * (1.0 - samples[v].out));
    });
    return samples;
}

/** Triangles refine(m, edge) makes: each triangle splits about (longest edge / edge) squared times. */
double refinedCount(const TriMesh& m, double edge)
{
    double n = 0.0;
    for (const glm::uvec3& t : m.t)
    {
        const double l = std::max({glm::length(m.p[t[0]] - m.p[t[1]]), glm::length(m.p[t[1]] - m.p[t[2]]), glm::length(m.p[t[2]] - m.p[t[0]])});
        const double k = std::max(1.0, std::exp2(std::ceil(std::log2(std::max(1.0, l / edge)))));
        n += k * k;
    }
    return n;
}

/** The edge length asked for, or the shortest that stays under the budget, with a warning. */
double edgeForBudget(const TriMesh& m, double edge, size_t maxTriangles, std::vector<std::string>& warnings)
{
    if (refinedCount(m, edge) <= double(maxTriangles))
        return edge;
    double lo = edge, hi = edge;
    while (refinedCount(m, hi) > double(maxTriangles) && hi < 1e7)
        hi *= 2.0;
    for (int i = 0; i < 24; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (refinedCount(m, mid) > double(maxTriangles) ? lo : hi) = mid;
    }
    warnings.push_back("detail lowered to stay under " + std::to_string(maxTriangles) + " triangles (edge " + std::to_string(int(std::round(hi))) + " cm)");
    return hi;
}

void setCornerNormal(TriMesh& m, uint32_t v, const glm::dvec3& n)
{
    const int nx = m.cornerIndex("nx");
    if (nx < 0)
        return;
    for (size_t t = 0; t < m.t.size(); ++t)
        for (int c = 0; c < 3; ++c)
            if (m.t[t][c] == v)
            {
                float* a = m.cattr(t, c);
                a[nx] = float(n.x);
                a[nx + 1] = float(n.y);
                a[nx + 2] = float(n.z);
            }
}
} // namespace

CliffMesh buildFromRegion(const Heightfield& ground, const std::vector<std::vector<glm::dvec2>>& rings, const MeshParams& p, const RockMap* rock)
{
    LILA_ZONE();

    CliffMesh out;
    TriMesh& m = out.mesh;
    const Footprint fp = footprintOf(ground, rings, p);
    const int nx = ground.nx;
    std::vector<int32_t> vid(ground.size(), -1);
    const int weightAt = m.addVertexAttr("weight", 1);
    const int heightAt = m.addVertexAttr("heightFraction", 1);
    const double range = std::max(1.0, fp.high - fp.low);
    auto vertex = [&](int x, int z) {
        int32_t& id = vid[ground.index(x, z)];
        if (id < 0)
        {
            id = int32_t(m.addVertex(ground.position(x, z)));
            m.vattr(size_t(id))[weightAt] = fp.weight[ground.index(x, z)];
            m.vattr(size_t(id))[heightAt] = float((ground.at(x, z) - fp.low) / range);
        }
        return uint32_t(id);
    };
    for (int z = 0; z + 1 < ground.nz; ++z)
        for (int x = 0; x + 1 < nx; ++x)
        {
            const bool any = fp.cover[ground.index(x, z)] || fp.cover[ground.index(x + 1, z)] || fp.cover[ground.index(x, z + 1)] || fp.cover[ground.index(x + 1, z + 1)];
            if (!any)
                continue;
            const uint32_t a = vertex(x, z), b = vertex(x + 1, z), c = vertex(x + 1, z + 1), d = vertex(x, z + 1);
            // Wound so the faces look up the Y axis.
            if (std::abs(m.p[a].y - m.p[c].y) <= std::abs(m.p[b].y - m.p[d].y))
            {
                m.t.push_back({a, c, b});
                m.t.push_back({a, d, c});
            }
            else
            {
                m.t.push_back({a, d, b});
                m.t.push_back({b, d, c});
            }
        }
    if (m.t.empty())
    {
        out.stats.warnings.push_back("the region covers no cells");
        return out;
    }
    const std::vector<uint8_t> border = borderVertices(m);
    double edge = ground.cell / std::max(0.25, p.detail);
    edge = edgeForBudget(m, edge, p.maxTriangles, out.stats.warnings);
    out.stats.detail = ground.cell / edge;
    refine(m, edge);
    std::vector<uint8_t> isBorder = borderVertices(m);

    MeshParams mp = p;
    mp.shape.facing = p.shape.facing;
    std::vector<glm::dvec3> baseNormals;
    std::vector<DisplaceSample> samples = displaceMesh(m, mp, rock, baseNormals);

    // The border: proud sinks the bottom under the ground and the top under the plateau; inset keeps it on the ground.
    if (p.mode == Mode::Proud)
    {
        for (size_t v = 0; v < m.p.size(); ++v)
        {
            const double w = m.vattr(v)[weightAt];
            const double hf = m.vattr(v)[heightAt];
            const double t = smooth01(0.35, 0.65, hf);
            const double fall = (1.0 - w) * (1.0 - w);
            m.p[v].y -= fall * (p.baseSink * (1.0 - t) + p.lipSink * t);
        }
    }
    for (size_t v = 0; v < m.p.size(); ++v)
    {
        const DisplaceSample& s = samples[v];
        out.stats.maxOut = std::max(out.stats.maxOut, p.shape.depth * m.vattr(v)[weightAt] * (1.0 - s.out));
        if (s.ledgeBelow)
            ++out.stats.ledgeVertices;
    }
    std::set<int> layers;
    for (const DisplaceSample& s : samples)
        layers.insert(s.layer);
    out.stats.layers = layers.size();
    bakeMasks(m, samples, baseNormals, mp);

    if (p.mode == Mode::Inset)
    {
        // The seam: border vertices stay on the ground with its normals, and a skirt drops below them.
        std::vector<uint32_t> borderList;
        for (size_t v = 0; v < m.p.size(); ++v)
            if (isBorder[v])
            {
                m.p[v].y = ground.heightAt(m.p[v].x, m.p[v].z);
                setCornerNormal(m, uint32_t(v), ground.normalAt(m.p[v].x, m.p[v].z));
                borderList.push_back(uint32_t(v));
            }
        if (p.skirt > 0.0)
        {
            std::unordered_map<uint64_t, int> count;
            std::vector<std::pair<uint32_t, uint32_t>> edges;
            for (const glm::uvec3& t : m.t)
                for (int e = 0; e < 3; ++e)
                    ++count[edgeKey(t[e], t[(e + 1) % 3])];
            for (const glm::uvec3& t : m.t)
                for (int e = 0; e < 3; ++e)
                    if (count[edgeKey(t[e], t[(e + 1) % 3])] == 1)
                        edges.push_back({t[e], t[(e + 1) % 3]});
            std::unordered_map<uint32_t, uint32_t> below;
            auto drop = [&](uint32_t v) {
                auto it = below.find(v);
                if (it != below.end())
                    return it->second;
                const uint32_t d = uint32_t(m.addVertex(m.p[v] - up() * p.skirt));
                for (int s = 0; s < m.vertexStride; ++s)
                    m.vattr(d)[s] = m.vattr(v)[s];
                samples.push_back(samples[v]);
                below[v] = d;
                return d;
            };
            const int cs = m.cornerStride;
            for (const auto& [a, b] : edges)
            {
                const uint32_t da = drop(a), db = drop(b);
                // The border edge runs a -> b in its triangle; the skirt faces outward, so it runs b -> a on top.
                for (const glm::uvec3 tri : {glm::uvec3(b, a, da), glm::uvec3(b, da, db)})
                {
                    m.t.push_back(tri);
                    for (int c = 0; c < 3; ++c)
                    {
                        const uint32_t v = tri[c];
                        const glm::dvec3 n = ground.normalAt(m.p[v].x, m.p[v].z);
                        for (int k = 0; k < cs; ++k)
                            m.ca.push_back(0.0f);
                        float* a2 = m.cattr(m.t.size() - 1, c);
                        const int u0 = m.cornerIndex("u0"), nxi = m.cornerIndex("nx");
                        a2[u0] = float(m.p[v].x / std::max(1.0, p.uvScale));
                        a2[u0 + 1] = float(m.p[v].y / std::max(1.0, p.uvScale));
                        a2[nxi] = float(n.x);
                        a2[nxi + 1] = float(n.y);
                        a2[nxi + 2] = float(n.z);
                    }
                }
            }
        }
    }

    // Stats against the ground as the cliff's edit leaves it.
    Heightfield after = ground;
    GroundEdit e;
    e.mode = p.mode;
    e.rings = rings;
    e.depth = p.shape.depth;
    e.recess = p.recess;
    e.blend = p.blend;
    e.margin = p.margin;
    e.talusWidth = p.talusWidth;
    applyGroundEdits(after, {e}, {});
    size_t interior = 0, hidden = 0;
    double clearance = -1e300;
    for (size_t v = 0; v < m.p.size(); ++v)
    {
        const double w = m.vattr(v)[weightAt];
        const double g = after.heightAt(m.p[v].x, m.p[v].z);
        if (p.mode == Mode::Proud && v < isBorder.size() && isBorder[v] && m.vattr(v)[heightAt] < 0.5)
            clearance = std::max(clearance, m.p[v].y - g);
        if (w > 0.5)
        {
            ++interior;
            if (p.mode == Mode::Proud && m.p[v].y < g - 1.0)
                ++hidden;
        }
    }
    out.stats.baseClearance = clearance > -1e299 ? clearance : 0.0;
    out.stats.hidden = interior ? double(hidden) / double(interior) : 0.0;
    if (p.mode == Mode::Proud && out.stats.hidden > 0.1)
        out.stats.warnings.push_back(std::to_string(int(std::round(out.stats.hidden * 100.0))) +
                                     "% of the cliff sits behind the ground: the heightfield is too coarse to set back this steep; "
                                     "raise the terrain's resolution or use mode='inset'");
    (void)border;
    out.stats.vertices = m.p.size();
    out.stats.triangles = m.t.size();
    return out;
}

CliffMesh buildFromMesh(const geo::Mesh& blockout, const Heightfield* ground, const MeshParams& p, const RockMap* rock)
{
    LILA_ZONE();

    CliffMesh out;
    TriMesh m = fromMesh(blockout);
    m.va.clear();
    m.vertexStride = 0;
    m.vertexNames.clear();
    m.va.assign(0, 0.0f);
    m.ca.clear();
    m.cornerStride = 0;
    m.cornerNames.clear();
    if (m.t.empty())
    {
        out.stats.warnings.push_back("the blockout has no faces");
        return out;
    }
    const geo::Bounds b = m.bounds();
    const double low = b.min.y, high = b.max.y, range = std::max(1.0, high - low);
    const int weightAt = m.addVertexAttr("weight", 1, 1.0f);
    const int heightAt = m.addVertexAttr("heightFraction", 1);
    for (size_t v = 0; v < m.p.size(); ++v)
        m.vattr(v)[heightAt] = float((m.p[v].y - low) / range);
    double edge = ground ? ground->cell / std::max(0.25, p.detail) : std::max(10.0, p.shape.strataHeight / std::max(0.25, p.detail));
    edge = edgeForBudget(m, edge, p.maxTriangles, out.stats.warnings);
    out.stats.detail = p.detail;
    refine(m, edge);
    // Open borders keep the blockout's silhouette: no displacement there.
    const std::vector<uint8_t> border = borderVertices(m);
    for (size_t v = 0; v < m.p.size(); ++v)
        if (border[v])
            m.vattr(v)[weightAt] = 0.0f;
    std::vector<glm::dvec3> baseNormals;
    std::vector<DisplaceSample> samples = displaceMesh(m, p, rock, baseNormals);
    if (ground)
        for (size_t v = 0; v < m.p.size(); ++v)
            if (m.vattr(v)[heightAt] < 0.02)
                m.p[v].y = std::min(m.p[v].y, ground->heightAt(m.p[v].x, m.p[v].z) - p.baseSink);
    std::set<int> layers;
    for (size_t v = 0; v < samples.size(); ++v)
    {
        layers.insert(samples[v].layer);
        out.stats.maxOut = std::max(out.stats.maxOut, p.shape.depth * m.vattr(v)[weightAt] * (1.0 - samples[v].out));
        if (samples[v].ledgeBelow)
            ++out.stats.ledgeVertices;
    }
    out.stats.layers = layers.size();
    bakeMasks(m, samples, baseNormals, p);
    out.stats.vertices = m.p.size();
    out.stats.triangles = m.t.size();
    out.mesh = std::move(m);
    return out;
}

// ---------------------------------------------------------------------------------------------------- ground edits

json GroundEdit::toJson() const
{
    return {{"name", name},   {"mode", mode == Mode::Inset ? "inset" : "proud"},
            {"rings", ringsJson(rings)}, {"depth", depth}, {"recess", recess}, {"blend", blend}, {"margin", margin},
            {"talusWidth", talusWidth}, {"detail", detail}};
}

GroundEdit GroundEdit::fromJson(const json& j)
{
    GroundEdit e;
    e.name = j.value("name", std::string());
    e.mode = j.value("mode", std::string("proud")) == "inset" ? Mode::Inset : Mode::Proud;
    e.rings = ringsFrom(j.value("rings", json::array()));
    e.depth = j.value("depth", e.depth);
    e.recess = j.value("recess", e.recess);
    e.blend = j.value("blend", e.blend);
    e.margin = j.value("margin", e.margin);
    e.talusWidth = j.value("talusWidth", e.talusWidth);
    e.detail = j.value("detail", e.detail);
    return e;
}

namespace
{
/** Distance from p to the polyline, which side it lies on (positive on the left of its direction) and where along it (segment + t). */
struct LineHit
{
    double distance = 1e300;
    double side = 0.0;
    size_t segment = 0;
    double t = 0.0;
};

LineHit lineDistance(const std::vector<glm::dvec2>& line, glm::dvec2 p)
{
    LineHit best;
    for (size_t i = 0; i + 1 < line.size(); ++i)
    {
        const glm::dvec2 a = line[i], b = line[i + 1], d = b - a;
        const double l2 = glm::dot(d, d);
        const double t = l2 > 0.0 ? std::clamp(glm::dot(p - a, d) / l2, 0.0, 1.0) : 0.0;
        const double dist = glm::length(p - (a + d * t));
        if (dist < best.distance)
            best = {dist, d.x * (p.y - a.y) - d.y * (p.x - a.x), i, t};
    }
    return best;
}
} // namespace

GroundResult applyGroundEdits(Heightfield& h, const std::vector<GroundEdit>& edits, const std::vector<WallStamp>& walls)
{
    LILA_ZONE();

    GroundResult r;
    const Heightfield before = h;
    std::vector<float>& cliffMask = h.mask("cliff");
    std::vector<float>& talusMask = h.mask("cliffTalus");
    std::vector<float>& topMask = h.mask("cliffTop");
    std::fill(cliffMask.begin(), cliffMask.end(), 0.0f);
    std::fill(talusMask.begin(), talusMask.end(), 0.0f);
    std::fill(topMask.begin(), topMask.end(), 0.0f);
    std::vector<float>* holes = nullptr;
    for (const GroundEdit& e : edits)
    {
        MeshParams mp;
        mp.margin = e.margin;
        mp.blend = e.blend;
        const Footprint fp = footprintOf(before, e.rings, mp);
        const std::vector<float> near = distanceTo(before, fp.cover);
        const double mid = 0.5 * (fp.low + fp.high);
        for (size_t i = 0; i < h.size(); ++i)
        {
            if (fp.cover[i])
            {
                cliffMask[i] = std::max(cliffMask[i], fp.weight[i] > 0.0f ? std::max(0.5f, fp.weight[i]) : 0.25f);
                continue;
            }
            const double d = near[i];
            if (d < e.talusWidth && before.heights[i] < mid)
                talusMask[i] = std::max(talusMask[i], float(1.0 - d / e.talusWidth));
            if (d < 0.5 * e.talusWidth && before.heights[i] >= mid)
                topMask[i] = std::max(topMask[i], float(1.0 - d / (0.5 * e.talusWidth)));
        }
        if (e.mode == Mode::Inset)
        {
            if (!holes)
                holes = &h.mask("holes");
            for (size_t i = 0; i < h.size(); ++i)
                if (fp.cover[i] && fp.weight[i] > 0.0f)
                {
                    (*holes)[i] = 1.0f;
                    ++r.holes;
                }
            continue;
        }
        // Proud: the ground under the cliff moves back along its downhill direction, so the mesh stands in front of it.
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const size_t i = h.index(x, z);
                if (!fp.cover[i] || fp.weight[i] <= 0.0f)
                    continue;
                const glm::dvec2 g = gradientAt(before, x, z);
                const double gl = glm::length(g);
                if (gl < 1e-6)
                    continue;
                const double sinT = gl / std::sqrt(1.0 + gl * gl);
                const double shift = e.depth * e.recess * fp.weight[i] / std::max(0.3, sinT);
                const glm::dvec2 p = before.samplePos(x, z) - g / gl * shift;
                const double back = before.heightAt(p.x, p.y);
                h.heights[i] = float(std::min(double(h.heights[i]), back));
            }
        });
        for (size_t i = 0; i < h.size(); ++i)
            if (h.heights[i] < before.heights[i])
                ++r.recessed;
    }
    for (const WallStamp& w : walls)
    {
        if (w.line.size() < 2)
            continue;
        std::vector<double> base(w.line.size());
        for (size_t i = 0; i < w.line.size(); ++i)
            base[i] = before.heightAt(w.line[i].x, w.line[i].y);
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const glm::dvec2 p = h.samplePos(x, z);
                const LineHit hit = lineDistance(w.line, p);
                const double b = base[hit.segment] + (base[hit.segment + 1] - base[hit.segment]) * hit.t;
                const bool behind = (hit.side > 0.0) != w.flip;
                const size_t i = h.index(x, z);
                if (behind && hit.distance < w.plateau + w.falloff)
                {
                    const double f = 1.0 - smooth01(w.plateau, w.plateau + w.falloff, hit.distance);
                    const double top = b + w.height;
                    if (h.heights[i] < top)
                        h.heights[i] = float(h.heights[i] + (top - h.heights[i]) * f);
                }
                else if (!behind && hit.distance < w.cut + w.falloff)
                {
                    const double f = 1.0 - smooth01(w.cut, w.cut + w.falloff, hit.distance);
                    if (h.heights[i] > b)
                        h.heights[i] = float(h.heights[i] + (b - h.heights[i]) * f);
                }
            }
        });
    }
    for (size_t i = 0; i < h.size() && !walls.empty(); ++i)
        if (h.heights[i] != before.heights[i] && !(cliffMask[i] > 0.0f))
            ++r.stamped;
    return r;
}

// ---------------------------------------------------------------------------------------------------- C2 extrusion

bool parseProfile(const std::string& s, Profile* out)
{
    static const std::map<std::string, Profile> names = {
        {"vertical", Profile::Vertical}, {"battered", Profile::Battered}, {"overhang", Profile::Overhang}, {"stepped", Profile::Stepped}};
    auto it = names.find(s);
    if (it == names.end())
        return false;
    *out = it->second;
    return true;
}

std::vector<glm::dvec2> profilePoints(const ExtrudeParams& p)
{
    if (p.custom.size() >= 2)
        return p.custom;
    std::vector<glm::dvec2> pts;
    const int rows = 12;
    for (int i = 0; i <= rows; ++i)
    {
        const double u = double(i) / rows;
        double out = 0.0;
        switch (p.profile)
        {
        case Profile::Vertical:
            out = 0.0;
            break;
        case Profile::Battered:
            out = -p.batter * u;
            break;
        case Profile::Overhang:
            out = p.batter * u * u;
            break;
        case Profile::Stepped:
        {
            const int steps = std::max(1, p.steps);
            const double k = std::floor(u * steps - 1e-9);
            out = -p.batter * std::clamp(k, 0.0, double(steps - 1)) / double(std::max(1, steps - 1));
            break;
        }
        }
        pts.push_back({out, u});
    }
    if (p.profile == Profile::Stepped)
    {
        // Each step's ledge: a short flat before the next riser.
        std::vector<glm::dvec2> stepped;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            if (i > 0 && pts[i].x != pts[i - 1].x)
                stepped.push_back({pts[i].x, pts[i - 1].y});
            stepped.push_back(pts[i]);
        }
        return stepped;
    }
    return pts;
}

CliffMesh extrudeWall(const std::vector<glm::dvec2>& lineIn, const Heightfield* ground, const ExtrudeParams& p, bool flip, const RockMap* rock)
{
    LILA_ZONE();

    CliffMesh out;
    if (lineIn.size() < 2)
    {
        out.stats.warnings.push_back("the line needs two points");
        return out;
    }
    std::vector<glm::dvec2> line = lineIn;
    const double step = std::max(1.0, p.cell / std::max(0.25, p.detail));
    // Resample along the line at the mesh's edge length.
    std::vector<glm::dvec2> pts{line.front()};
    for (size_t i = 0; i + 1 < line.size(); ++i)
    {
        const double l = glm::length(line[i + 1] - line[i]);
        const int n = std::max(1, int(std::ceil(l / step)));
        for (int k = 1; k <= n; ++k)
            pts.push_back(line[i] + (line[i + 1] - line[i]) * (double(k) / n));
    }
    const std::vector<glm::dvec2> prof = profilePoints(p);
    std::vector<double> profLen(prof.size(), 0.0);
    for (size_t i = 1; i < prof.size(); ++i)
        profLen[i] = profLen[i - 1] + glm::length((prof[i] - prof[i - 1]) * p.height);
    const int rows = std::max(2, int(std::ceil(profLen.back() / step)));
    TriMesh& m = out.mesh;
    const int weightAt = m.addVertexAttr("weight", 1);
    const int heightAt = m.addVertexAttr("heightFraction", 1);
    double low = 1e300, high = -1e300;
    std::vector<double> bases(pts.size());
    std::vector<double> tops(pts.size());
    std::vector<glm::dvec2> outward(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const glm::dvec2 a = pts[i == 0 ? 0 : i - 1], b = pts[std::min(pts.size() - 1, i + 1)];
        glm::dvec2 d = b - a;
        d = glm::length(d) > 1e-9 ? glm::normalize(d) : glm::dvec2(1.0, 0.0);
        // Outward is the right of the direction as seen from above (+X right, +Z down), flipped on request.
        outward[i] = flip ? glm::dvec2(d.y, -d.x) : glm::dvec2(-d.y, d.x);
        if (ground)
        {
            // A stamped terrain is cut to the base in front and raised to the top behind; read both there.
            const double e = 0.5 * ground->cell;
            bases[i] = std::min(ground->heightAt(pts[i].x, pts[i].y), ground->heightAt(pts[i].x + outward[i].x * e, pts[i].y + outward[i].y * e));
            const double behind = ground->heightAt(pts[i].x - outward[i].x * e, pts[i].y - outward[i].y * e);
            tops[i] = std::max(bases[i] + p.height, std::min(behind, bases[i] + 1.5 * p.height));
        }
        else
        {
            bases[i] = 0.0;
            tops[i] = p.height;
        }
        low = std::min(low, bases[i]);
        high = std::max(high, tops[i]);
    }
    glm::dvec2 facing(0.0);
    const size_t cols = pts.size();
    for (size_t i = 0; i < cols; ++i)
    {
        const glm::dvec2 n = outward[i];
        const double wallH = tops[i] - bases[i];
        facing += n;
        for (int r = 0; r <= rows; ++r)
        {
            const double s = profLen.back() * double(r) / rows;
            size_t k = 1;
            while (k + 1 < prof.size() && profLen[k] < s)
                ++k;
            const double t = profLen[k] > profLen[k - 1] ? (s - profLen[k - 1]) / (profLen[k] - profLen[k - 1]) : 0.0;
            const glm::dvec2 q = prof[k - 1] + (prof[k] - prof[k - 1]) * t;
            const double y = bases[i] - p.sink + (wallH + p.sink) * q.y;
            const glm::dvec2 g = pts[i] + n * (q.x * wallH);
            const size_t v = m.addVertex({g.x, y, g.y});
            const double edgeFade = std::min({double(i) / std::max<size_t>(1, cols - 1), double(cols - 1 - i) / std::max<size_t>(1, cols - 1)}) * double(cols - 1) * step;
            m.vattr(v)[weightAt] = float(smooth01(0.0, std::max(1.0, p.mesh.blend), std::min(edgeFade, std::min(y - (bases[i] - p.sink), tops[i] - y))));
            m.vattr(v)[heightAt] = float((y - low) / std::max(1.0, high - low));
        }
    }
    const uint32_t stride = uint32_t(rows + 1);
    for (size_t i = 0; i + 1 < cols; ++i)
        for (int r = 0; r < rows; ++r)
        {
            const uint32_t a = uint32_t(i) * stride + uint32_t(r), b = a + stride, c = b + 1, d = a + 1;
            if (flip)
            {
                m.t.push_back({a, d, c});
                m.t.push_back({a, c, b});
            }
            else
            {
                m.t.push_back({a, b, c});
                m.t.push_back({a, c, d});
            }
        }
    MeshParams mp = p.mesh;
    mp.shape.facing = glm::length(facing) > 1e-9 ? glm::normalize(glm::dvec3(facing.x, 0.0, facing.y)) : glm::dvec3(1.0, 0.0, 0.0);
    std::vector<glm::dvec3> baseNormals;
    std::vector<DisplaceSample> samples = displaceMesh(m, mp, rock, baseNormals);
    std::set<int> layers;
    for (size_t v = 0; v < samples.size(); ++v)
    {
        layers.insert(samples[v].layer);
        out.stats.maxOut = std::max(out.stats.maxOut, mp.shape.depth * m.vattr(v)[weightAt] * (1.0 - samples[v].out));
        if (samples[v].ledgeBelow)
            ++out.stats.ledgeVertices;
    }
    out.stats.layers = layers.size();
    bakeMasks(m, samples, baseNormals, mp);
    if (ground)
    {
        double clearance = -1e300;
        for (size_t i = 0; i < cols; ++i)
        {
            const glm::dvec3 q = m.p[i * stride];
            clearance = std::max(clearance, q.y - ground->heightAt(q.x, q.z));
        }
        out.stats.baseClearance = clearance;
    }
    out.stats.vertices = m.p.size();
    out.stats.triangles = m.t.size();
    out.stats.detail = p.detail;
    return out;
}

// ---------------------------------------------------------------------------------------------------- C3 kits

KitResult fitKit(const Heightfield& ground, const Region& region, const std::vector<KitPiece>& kit, const KitParams& p)
{
    LILA_ZONE();

    KitResult out;
    if (kit.empty() || region.rings.empty())
        return out;
    // The face line: the outer ring's stretch along the bottom of the cliff, walked at even steps.
    const std::vector<glm::dvec2>& ring = region.rings.front();
    std::vector<glm::dvec2> walk;
    std::vector<double> dist{0.0};
    for (size_t i = 0; i <= ring.size(); ++i)
    {
        walk.push_back(ring[i % ring.size()]);
        if (i > 0)
            dist.push_back(dist.back() + glm::length(walk.back() - walk[walk.size() - 2]));
    }
    const double total = dist.back();
    if (total <= 0.0)
        return out;
    auto at = [&](double s, glm::dvec2* dir) {
        s = std::fmod(std::fmod(s, total) + total, total);
        size_t k = size_t(std::upper_bound(dist.begin(), dist.end(), s) - dist.begin());
        k = std::clamp<size_t>(k, 1, dist.size() - 1);
        const double t = (s - dist[k - 1]) / std::max(1e-9, dist[k] - dist[k - 1]);
        *dir = glm::normalize(walk[k] - walk[k - 1] + glm::dvec2(1e-12, 0.0));
        return walk[k - 1] + (walk[k] - walk[k - 1]) * t;
    };
    const double height = std::max(1.0, region.high - region.low);
    std::vector<size_t> order(kit.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return kit[a].size.x > kit[b].size.x; });
    std::vector<std::pair<double, double>> covered;

    auto tryPlace = [&](size_t pieceIndex, double s, bool filler, double scaleCap, uint32_t salt) -> bool {
        const KitPiece& piece = kit[pieceIndex];
        glm::dvec2 dir;
        const glm::dvec2 g = at(s, &dir);
        // Outward: toward lower ground on either side of the outline.
        glm::dvec2 n(-dir.y, dir.x);
        if (ground.heightAt(g.x + n.x * ground.cell * 2.0, g.y + n.y * ground.cell * 2.0) > ground.heightAt(g.x - n.x * ground.cell * 2.0, g.y - n.y * ground.cell * 2.0))
            n = -n;
        const uint32_t hsh = hash3(uint32_t(std::lround(s * 10.0)), p.seed, salt);
        double scale = std::min(scaleCap, height / std::max(1.0, piece.size.y));
        scale *= 1.0 + p.scaleJitter * (unit(hsh) * 2.0 - 1.0);
        scale = std::max(0.05, scale);
        const double turn = p.rotate * kDeg * (unit(hash3(hsh, 1u, 3u)) * 2.0 - 1.0);
        const double tilt = 0.5 * p.rotate * kDeg * (unit(hash3(hsh, 2u, 5u)) * 2.0 - 1.0);
        const glm::dvec3 fwd = glm::normalize(glm::dvec3(n.x, 0.0, n.y));
        const glm::dvec3 right = glm::normalize(glm::cross(up(), fwd));
        // Each piece's +Z faces out, base at the bottom of the cliff, sunk by a fraction of its height.
        const double baseY = ground.heightAt(g.x, g.y) - p.sink * piece.size.y * scale;
        glm::dmat4 m(1.0);
        m[0] = glm::dvec4(right, 0.0);
        m[1] = glm::dvec4(up(), 0.0);
        m[2] = glm::dvec4(fwd, 0.0);
        m[3] = glm::dvec4(g.x, baseY, g.y, 1.0);
        const glm::dmat4 rot = glm::rotate(glm::dmat4(1.0), turn, glm::dvec3(0, 1, 0)) * glm::rotate(glm::dmat4(1.0), tilt, glm::dvec3(1, 0, 0));
        m = m * rot * glm::scale(glm::dmat4(1.0), glm::dvec3(scale)) * glm::translate(glm::dmat4(1.0), glm::dvec3(-(piece.lo.x + 0.5 * piece.size.x), -piece.lo.y, -(piece.lo.z + 0.5 * piece.size.z)));
        // Floating: the lowest samples well above the ground; penetrating: buried deeper than maxPenetration of its depth.
        double lowest = 1e300;
        size_t inside = 0;
        std::vector<glm::dvec3> placed;
        placed.reserve(piece.samples.size());
        for (const glm::dvec3& q : piece.samples)
        {
            const glm::dvec3 w = glm::dvec3(m * glm::dvec4(q, 1.0));
            placed.push_back(w);
            const double gh = ground.heightAt(w.x, w.z);
            lowest = std::min(lowest, w.y - gh);
            if (w.y < gh - 1.0)
                ++inside;
        }
        if (!piece.samples.empty())
        {
            if (lowest > p.floatTolerance)
            {
                ++out.rejectedFloating;
                return false;
            }
            bool buried = false;
            if (p.depthOf)
            {
                // The base is sunk on purpose and the ground under a wide piece is never level: each point is measured
                // from the ground at its own place on the piece's front, so only the face rising behind it counts.
                const double sunk = std::max(1.0, p.sink * piece.size.y * scale);
                double front = -1e300;
                for (const glm::dvec3& q : placed)
                    front = std::max(front, glm::dot(q, fwd));
                for (glm::dvec3& q : placed)
                {
                    const glm::dvec3 atFront = q + fwd * (front - glm::dot(q, fwd));
                    q.y = std::max(q.y, ground.heightAt(atFront.x, atFront.z) + sunk);
                }
                buried = p.depthOf(placed, fwd) > p.maxPenetration * piece.size.z * scale;
            }
            else
                buried = double(inside) / double(piece.samples.size()) > p.maxPenetration;
            if (buried)
            {
                ++out.rejectedPenetrating;
                return false;
            }
        }
        out.placed.push_back({int(pieceIndex), m, s, filler});
        const double half = 0.5 * piece.size.x * scale;
        covered.push_back({s - half, s + half});
        if (filler)
            ++out.fillers;
        return true;
    };

    // The foot of the cliff: the stretches of the outline in the lower half of its height, where the pieces stand.
    const double mid = region.low + 0.5 * (region.high - region.low);
    std::vector<std::pair<double, double>> foot;
    const double probe = std::max(1.0, std::min(p.gap * 0.5, ground.cell));
    for (double s = 0.0; s < total; s += probe)
    {
        glm::dvec2 dir;
        const glm::dvec2 q = at(s, &dir);
        const bool low = ground.heightAt(q.x, q.y) <= mid;
        if (low && (foot.empty() || foot.back().second < s - probe * 0.5))
            foot.push_back({s, std::min(total, s + probe)});
        else if (low)
            foot.back().second = std::min(total, s + probe);
    }
    double footLength = 0.0;
    for (const auto& [a, b] : foot)
        footLength += b - a;
    if (footLength <= 0.0)
        return out;

    // Main pass: along each stretch, the largest pieces stepped by overlap times their width, picked by hash.
    uint32_t salt = 0;
    for (const auto& [a, b] : foot)
    {
        double s = a;
        while (s < b && int(out.placed.size()) < p.maxPieces)
        {
            const size_t pick = order[hash3(uint32_t(std::lround(s)), p.seed, 0x9u) % uint32_t(std::min<size_t>(order.size(), 3))];
            const double scale = height / std::max(1.0, kit[pick].size.y);
            const double width = std::max(10.0, kit[pick].size.x * scale);
            tryPlace(pick, s, false, 1e9, ++salt);
            s += width * std::clamp(p.overlap, 0.2, 2.0);
        }
    }
    // Gaps along the foot get smaller pieces, smallest last.
    std::sort(covered.begin(), covered.end());
    for (const auto& [a, b] : foot)
    {
        std::vector<std::pair<double, double>> gaps;
        double reach = a;
        for (const auto& [ca, cb] : covered)
        {
            if (cb <= a || ca >= b)
                continue;
            if (ca - reach > p.gap)
                gaps.push_back({reach, ca});
            reach = std::max(reach, cb);
        }
        if (b - reach > p.gap)
            gaps.push_back({reach, b});
        for (const auto& [ga, gb] : gaps)
            for (double gpos = ga + 0.5 * p.gap; gpos < gb && int(out.placed.size()) < p.maxPieces; gpos += p.gap)
                for (auto it = order.rbegin(); it != order.rend(); ++it)
                    if (tryPlace(*it, gpos, true, 0.5 * height / std::max(1.0, kit[*it].size.y), ++salt))
                        break;
    }
    // Coverage: how much of the foot some piece spans.
    std::sort(covered.begin(), covered.end());
    double cov = 0.0;
    for (const auto& [a, b] : foot)
    {
        double end = a;
        for (const auto& [ca, cb] : covered)
        {
            const double lo = std::max({ca, end, a}), hi = std::min(cb, b);
            if (hi > lo)
            {
                cov += hi - lo;
                end = hi;
            }
        }
    }
    const double totalFoot = footLength;
    out.coverage = std::clamp(cov / totalFoot, 0.0, 1.0);
    return out;
}

// ---------------------------------------------------------------------------------------------------- C5 cells

namespace
{
/** Slices every triangle crossing the plane coord(axis) = value; edges crossing it get one shared vertex. */
void slicePlane(TriMesh& m, int axis, double value)
{
    auto coord = [&](uint32_t v) { return axis == 0 ? m.p[v].x : m.p[v].z; };
    std::unordered_map<uint64_t, uint32_t> cut;
    auto cutVertex = [&](uint32_t a, uint32_t b) -> uint32_t {
        const uint64_t k = edgeKey(a, b);
        auto it = cut.find(k);
        if (it != cut.end())
            return it->second;
        const uint32_t lo = std::min(a, b), hi = std::max(a, b);
        const double t = (value - coord(lo)) / (coord(hi) - coord(lo));
        glm::dvec3 q = m.p[lo] + (m.p[hi] - m.p[lo]) * t;
        (axis == 0 ? q.x : q.z) = value;
        const size_t v = m.addVertex(q);
        for (int s = 0; s < m.vertexStride; ++s)
            m.vattr(v)[s] = float(m.vattr(lo)[s] + (m.vattr(hi)[s] - m.vattr(lo)[s]) * t);
        cut[k] = uint32_t(v);
        return uint32_t(v);
    };
    // Triangles that cross are replaced in place by their first piece, the other pieces appended; the rest stay put.
    const size_t triCount = m.t.size();
    const int cs = m.cornerStride;
    std::vector<float> src(size_t(3 * cs));
    for (size_t ti = 0; ti < triCount; ++ti)
    {
        const glm::uvec3 tri = m.t[ti];
        int side[3];
        for (int c = 0; c < 3; ++c)
        {
            const double d = coord(tri[c]) - value;
            side[c] = d > 1e-9 ? 1 : d < -1e-9 ? -1 : 0;
        }
        if (!((side[0] * side[1] < 0) || (side[1] * side[2] < 0) || (side[2] * side[0] < 0)))
            continue;
        std::copy_n(m.ca.data() + ti * 3 * size_t(cs), 3 * cs, src.data());
        LocalPoint corner[3] = {{tri[0], {1, 0, 0}}, {tri[1], {0, 1, 0}}, {tri[2], {0, 0, 1}}};
        std::vector<LocalPoint> poly[2];
        for (int c = 0; c < 3; ++c)
        {
            const int n = (c + 1) % 3;
            if (side[c] >= 0)
                poly[1].push_back(corner[c]);
            if (side[c] <= 0)
                poly[0].push_back(corner[c]);
            if (side[c] * side[n] < 0)
            {
                const uint32_t v = cutVertex(tri[c], tri[n]);
                const double t = (value - coord(tri[c])) / (coord(tri[n]) - coord(tri[c]));
                const LocalPoint lp{v, corner[c].bary + (corner[n].bary - corner[c].bary) * t};
                poly[0].push_back(lp);
                poly[1].push_back(lp);
            }
        }
        bool first = true;
        for (const auto& pl : poly)
            for (size_t k = 1; k + 1 < pl.size(); ++k)
            {
                const LocalPoint* lps[3] = {&pl[0], &pl[k], &pl[k + 1]};
                const size_t at = first ? ti : m.t.size();
                if (first)
                    m.t[ti] = {lps[0]->v, lps[1]->v, lps[2]->v};
                else
                {
                    m.t.push_back({lps[0]->v, lps[1]->v, lps[2]->v});
                    m.ca.resize(m.ca.size() + size_t(3 * cs));
                }
                for (int c = 0; c < 3; ++c)
                    for (int k2 = 0; k2 < cs; ++k2)
                        m.ca[(at * 3 + size_t(c)) * size_t(cs) + size_t(k2)] =
                            float(lps[c]->bary.x * src[size_t(k2)] + lps[c]->bary.y * src[size_t(cs + k2)] + lps[c]->bary.z * src[size_t(2 * cs + k2)]);
                first = false;
            }
    }
}
} // namespace

void sliceOnGrid(TriMesh& m, glm::dvec2 origin, double cell)
{
    LILA_ZONE();

    if (cell <= 0.0 || m.t.empty())
        return;
    const geo::Bounds b = m.bounds();
    for (int axis = 0; axis < 2; ++axis)
    {
        const double lo = axis == 0 ? b.min.x : b.min.z, hi = axis == 0 ? b.max.x : b.max.z;
        const double o = axis == 0 ? origin.x : origin.y;
        for (int64_t k = int64_t(std::floor((lo - o) / cell)) + 1; o + double(k) * cell < hi; ++k)
            slicePlane(m, axis, o + double(k) * cell);
    }
}

std::map<std::pair<int, int>, TriMesh> splitCells(const TriMesh& m, glm::dvec2 origin, double cell)
{
    std::map<std::pair<int, int>, TriMesh> out;
    std::map<std::pair<int, int>, std::unordered_map<uint32_t, uint32_t>> remap;
    for (size_t ti = 0; ti < m.t.size(); ++ti)
    {
        const glm::uvec3 tri = m.t[ti];
        const glm::dvec3 c = (m.p[tri[0]] + m.p[tri[1]] + m.p[tri[2]]) / 3.0;
        const std::pair<int, int> key{int(std::floor((c.x - origin.x) / cell)), int(std::floor((c.z - origin.y) / cell))};
        TriMesh& o = out[key];
        auto& rm = remap[key];
        if (o.vertexStride == 0 && o.cornerStride == 0 && o.p.empty())
        {
            o.vertexStride = m.vertexStride;
            o.cornerStride = m.cornerStride;
            o.vertexNames = m.vertexNames;
            o.cornerNames = m.cornerNames;
        }
        glm::uvec3 nt;
        for (int k = 0; k < 3; ++k)
        {
            auto [it, fresh] = rm.emplace(tri[k], uint32_t(o.p.size()));
            if (fresh)
            {
                o.addVertex(m.p[tri[k]]);
                std::copy_n(m.vattr(tri[k]), m.vertexStride, o.vattr(o.p.size() - 1));
            }
            nt[k] = it->second;
        }
        o.t.push_back(nt);
        for (int k = 0; k < 3; ++k)
            for (int s = 0; s < m.cornerStride; ++s)
                o.ca.push_back(m.cattr(ti, k)[s]);
    }
    return out;
}

std::vector<Tile> cutTiles(const TriMesh& mesh, const TileParams& p)
{
    LILA_ZONE();

    TriMesh m = mesh;
    sliceOnGrid(m, p.origin, p.cell);
    std::map<std::pair<int, int>, TriMesh> cells = splitCells(m, p.origin, p.cell);
    std::vector<std::pair<std::pair<int, int>, TriMesh*>> list;
    for (auto& [k, t] : cells)
        list.push_back({k, &t});
    std::vector<Tile> out(list.size());
    tbb::parallel_for(size_t(0), list.size(), [&](size_t i) {
        Tile& tile = out[i];
        tile.cell = list[i].first;
        TriMesh& tm = *list[i].second;
        tile.bounds = tm.bounds();
        tile.centre = tile.bounds.center();
        for (glm::dvec3& q : tm.p)
            q -= tile.centre;
        geo::Mesh lod0 = toMesh(tm);
        tile.triangles = tm.t.size();
        tile.nanite = p.nanite == "on" || (p.nanite == "auto" && tile.triangles > p.naniteTriangles);
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* data, size_t n) {
            const auto* b = static_cast<const unsigned char*>(data);
            for (size_t k = 0; k < n; ++k)
                h = (h ^ b[k]) * 1099511628211ull;
        };
        mix(tm.p.data(), tm.p.size() * sizeof(glm::dvec3));
        mix(tm.t.data(), tm.t.size() * sizeof(glm::uvec3));
        mix(tm.va.data(), tm.va.size() * sizeof(float));
        mix(&tile.centre, sizeof(tile.centre));
        const double params[] = {double(p.lods), p.lodRatio, p.collisionRatio, tile.nanite ? 1.0 : 0.0};
        mix(params, sizeof(params));
        tile.hash = h;
        const double tris0 = double(tile.triangles);
        tile.lods.push_back(lod0);
        for (int level = 1; level < std::max(1, p.lods); ++level)
        {
            geo::ReduceOptions ro;
            ro.keepBorders = true;
            ro.targetTriangles = geo::Index(std::max(2.0, std::round(tris0 * std::pow(std::clamp(p.lodRatio, 0.001, 1.0), level))));
            tile.lods.push_back(geo::reduceMesh(lod0, ro).mesh);
        }
        geo::ReduceOptions ro;
        ro.keepBorders = true;
        ro.targetTriangles = geo::Index(std::max(2.0, std::round(tris0 * std::clamp(p.collisionRatio, 0.001, 1.0))));
        tile.collision = geo::reduceMesh(lod0, ro).mesh;
    });
    return out;
}

std::vector<ScatterPoint> scatterTiers(const TriMesh& m, const std::vector<Tier>& tiers, uint32_t seed, glm::dvec2 origin)
{
    LILA_ZONE();

    std::vector<ScatterPoint> out;
    if (m.t.empty())
        return out;
    const std::vector<glm::dvec3> vn = vertexNormals(m);
    for (size_t ti = 0; ti < tiers.size(); ++ti)
    {
        const Tier& tier = tiers[ti];
        const int maskAt = tier.mask.empty() ? -1 : m.vertexIndex(tier.mask);
        // Triangles by cell of the tier's grid, so each cell scatters from its own seed alone.
        std::map<std::pair<int, int>, std::vector<size_t>> cells;
        for (size_t t = 0; t < m.t.size(); ++t)
        {
            const glm::dvec3 c = (m.p[m.t[t][0]] + m.p[m.t[t][1]] + m.p[m.t[t][2]]) / 3.0;
            cells[{int(std::floor((c.x - origin.x) / tier.grid)), int(std::floor((c.z - origin.y) / tier.grid))}].push_back(t);
        }
        for (const auto& [cell, list] : cells)
        {
            std::vector<double> cum;
            double area = 0.0;
            for (size_t t : list)
            {
                const glm::uvec3 tri = m.t[t];
                double a = 0.5 * glm::length(glm::cross(m.p[tri[1]] - m.p[tri[0]], m.p[tri[2]] - m.p[tri[0]]));
                if (maskAt >= 0)
                    a *= std::clamp((m.vattr(tri[0])[maskAt] + m.vattr(tri[1])[maskAt] + m.vattr(tri[2])[maskAt]) / 3.0, 0.0, 1.0);
                area += a;
                cum.push_back(area);
            }
            const uint32_t cellSeed = hash3(uint32_t(cell.first) * 73856093u, uint32_t(cell.second) * 19349663u, seed ^ hash3(uint32_t(ti), 0x71u, 0u));
            const double expected = area * 1e-4 * tier.density;
            int count = int(std::floor(expected));
            if (unit(hash3(cellSeed, 0u, 1u)) < expected - count)
                ++count;
            for (int k = 0; k < count && area > 0.0; ++k)
            {
                const uint32_t hsh = hash3(cellSeed, uint32_t(k), 2u);
                const double pick = unit(hsh) * area;
                const size_t idx = size_t(std::lower_bound(cum.begin(), cum.end(), pick) - cum.begin());
                const glm::uvec3 tri = m.t[list[std::min(idx, list.size() - 1)]];
                double u = unit(hash3(hsh, 3u, 4u)), v = unit(hash3(hsh, 5u, 6u));
                if (u + v > 1.0)
                {
                    u = 1.0 - u;
                    v = 1.0 - v;
                }
                ScatterPoint sp;
                sp.tier = int(ti);
                sp.item = tier.items.empty() ? 0 : int(hash3(hsh, 7u, 8u) % uint32_t(tier.items.size()));
                sp.position = m.p[tri[0]] + (m.p[tri[1]] - m.p[tri[0]]) * u + (m.p[tri[2]] - m.p[tri[0]]) * v;
                sp.normal = glm::normalize(vn[tri[0]] * (1.0 - u - v) + vn[tri[1]] * u + vn[tri[2]] * v);
                sp.downhill = horizontal(sp.normal, glm::dvec3(1.0, 0.0, 0.0));
                sp.slope = std::acos(std::clamp(sp.normal.y, -1.0, 1.0)) / kDeg;
                sp.scale = tier.scaleMin + (tier.scaleMax - tier.scaleMin) * unit(hash3(hsh, 9u, 10u));
                sp.turn = (unit(hash3(hsh, 11u, 12u)) * 2.0 - 1.0) * kPi;
                sp.cell = cell;
                out.push_back(sp);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------- C4 projection

glm::dvec3 projectionWeights(const glm::dvec3& nIn, double slopeStart, double blend, bool biplanar)
{
    const glm::dvec3 n = glm::normalize(nIn);
    const double slope = std::acos(std::clamp(std::abs(n.y), 0.0, 1.0)) / kDeg;
    const double side = smooth01(slopeStart, slopeStart + std::max(1e-3, blend), slope);
    double wx = n.x * n.x, wz = n.z * n.z;
    if (biplanar)
    {
        if (wx >= wz)
            wz = 0.0;
        else
            wx = 0.0;
    }
    const double s = wx + wz;
    if (s <= 1e-12)
        return {0.0, 1.0, 0.0};
    return {side * wx / s, 1.0 - side, side * wz / s};
}

double texelDensity(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, double tile, double slopeStart, double blend, bool biplanar)
{
    const glm::dvec3 nrm = glm::cross(b - a, c - a);
    const double area = 0.5 * glm::length(nrm);
    if (area <= 0.0)
        return 0.0;
    const glm::dvec3 w = projectionWeights(nrm, slopeStart, blend, biplanar);
    // Each projection maps the face onto its plane; the face's texels per cm is sqrt(projected area / area) / tile.
    auto projected = [&](int axis) {
        const glm::dvec3 n = glm::abs(nrm) * 0.5;
        return axis == 0 ? n.x : axis == 1 ? n.y : n.z;
    };
    double density = 0.0, wsum = 0.0;
    for (int axis = 0; axis < 3; ++axis)
    {
        const double wk = axis == 0 ? w.x : axis == 1 ? w.y : w.z;
        if (wk <= 0.0)
            continue;
        density += wk * std::sqrt(projected(axis) / area) / tile;
        wsum += wk;
    }
    return wsum > 0.0 ? density / wsum : 0.0;
}
} // namespace terrain::cliffs
