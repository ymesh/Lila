// Footprint stamps (building pads, plazas, yards), sculpt strokes and route finding on heightfields. Pure functions of a
// heightfield in its own space; the Terrain Stamps and Terrain Sculpt nodes and the terrain commands call them.

#include "Stamps.h"

#include "Brushes.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <queue>
#include <set>
#include <stdexcept>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace
{
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

template <class E> const char* nameOf(const std::vector<std::string>& names, E e) { return names[size_t(e)].c_str(); }

template <class E> E parseName(const std::vector<std::string>& names, const json& j, const char* key, E fallback)
{
    if (!j.contains(key))
        return fallback;
    const std::string v = j[key].get<std::string>();
    for (size_t k = 0; k < names.size(); ++k)
        if (names[k] == v)
            return E(k);
    std::string all;
    for (const std::string& n : names)
        all += (all.empty() ? "" : ", ") + n;
    throw std::invalid_argument(std::string(key) + " '" + v + "' is not one of " + all);
}

json ringJson(const std::vector<glm::dvec2>& r)
{
    json out = json::array();
    for (const glm::dvec2& p : r)
        out.push_back(json::array({p.x, p.y}));
    return out;
}

std::vector<glm::dvec2> ringFrom(const json& j)
{
    std::vector<glm::dvec2> out;
    if (j.is_array())
        for (const json& p : j)
            out.push_back({p.at(0).get<double>(), p.at(1).get<double>()});
    return out;
}

double smoothstep(double e0, double e1, double x)
{
    if (e1 <= e0)
        return x < e0 ? 0.0 : 1.0;
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double signedArea(const std::vector<glm::dvec2>& r)
{
    double a = 0.0;
    for (size_t k = 0; k < r.size(); ++k)
    {
        const glm::dvec2& p = r[k];
        const glm::dvec2& q = r[(k + 1) % r.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a * 0.5;
}

std::vector<glm::dvec2> convexHull(std::vector<glm::dvec2> p)
{
    std::sort(p.begin(), p.end(), [](const glm::dvec2& a, const glm::dvec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    p.erase(std::unique(p.begin(), p.end()), p.end());
    if (p.size() < 3)
        return p;
    auto cross = [](const glm::dvec2& o, const glm::dvec2& a, const glm::dvec2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
    std::vector<glm::dvec2> h(p.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < p.size(); ++i)
    {
        while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0.0)
            --k;
        h[k++] = p[i];
    }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; --i)
    {
        while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0.0)
            --k;
        h[k++] = p[i - 1];
    }
    h.resize(k - 1);
    return h;
}

/** A rectangle of samples, x0..x1 and z0..z1 inclusive, with per-sample scratch. */
struct Region
{
    int x0 = 0, z0 = 0, x1 = -1, z1 = -1;
    int w() const { return x1 - x0 + 1; }
    int d() const { return z1 - z0 + 1; }
    bool empty() const { return x1 < x0 || z1 < z0; }
    size_t size() const { return empty() ? 0 : size_t(w()) * size_t(d()); }
    size_t at(int x, int z) const { return size_t(z - z0) * size_t(w()) + size_t(x - x0); }
};

Region regionAround(const Heightfield& h, glm::dvec2 lo, glm::dvec2 hi, double margin)
{
    Region r;
    r.x0 = std::max(0, int(std::floor((lo.x - margin - h.origin.x) / h.cell)));
    r.z0 = std::max(0, int(std::floor((lo.y - margin - h.origin.y) / h.cell)));
    r.x1 = std::min(h.nx - 1, int(std::ceil((hi.x + margin - h.origin.x) / h.cell)));
    r.z1 = std::min(h.nz - 1, int(std::ceil((hi.y + margin - h.origin.y) / h.cell)));
    return r;
}

/** Squared distances (in samples) to the nearest set sample, exact Euclidean (Felzenszwalb and Huttenlocher). */
std::vector<float> distanceTransform(const std::vector<uint8_t>& set, int w, int d)
{
    const float inf = 1e20f;
    std::vector<float> f(size_t(w) * size_t(d));
    for (size_t k = 0; k < f.size(); ++k)
        f[k] = set[k] ? 0.0f : inf;
    auto pass = [&](int n, auto get, auto put) {
        const size_t count = static_cast<size_t>(n);
        std::vector<float> src(count), out(count), zz(count + 1);
        std::vector<int> v(count);
        for (int q = 0; q < n; ++q)
            src[size_t(q)] = get(q);
        int k = 0;
        v[0] = 0;
        zz[0] = -inf;
        zz[1] = inf;
        for (int q = 1; q < n; ++q)
        {
            float s;
            for (;;)
            {
                const int p = v[size_t(k)];
                s = ((src[size_t(q)] + float(q) * float(q)) - (src[size_t(p)] + float(p) * float(p))) / (2.0f * float(q - p));
                if (s <= zz[size_t(k)] && k > 0)
                    --k;
                else
                    break;
            }
            if (s <= zz[size_t(k)])
            {
                v[size_t(k)] = q;
                zz[size_t(k)] = -inf;
                zz[size_t(k) + 1] = inf;
                continue;
            }
            ++k;
            v[size_t(k)] = q;
            zz[size_t(k)] = s;
            zz[size_t(k) + 1] = inf;
        }
        k = 0;
        for (int q = 0; q < n; ++q)
        {
            while (zz[size_t(k) + 1] < float(q))
                ++k;
            const int p = v[size_t(k)];
            out[size_t(q)] = float(q - p) * float(q - p) + src[size_t(p)];
        }
        for (int q = 0; q < n; ++q)
            put(q, out[size_t(q)]);
    };
    for (int x = 0; x < w; ++x)
        pass(d, [&](int q) { return f[size_t(q) * size_t(w) + size_t(x)]; }, [&](int q, float v) { f[size_t(q) * size_t(w) + size_t(x)] = v; });
    for (int z = 0; z < d; ++z)
        pass(w, [&](int q) { return f[size_t(z) * size_t(w) + size_t(q)]; }, [&](int q, float v) { f[size_t(z) * size_t(w) + size_t(q)] = v; });
    return f;
}

/** Samples inside any ring, plus every corner of each cell a ring's edge passes through, so the bilinear surface over the whole footprint comes from marked samples. */
std::vector<uint8_t> rasterize(const Heightfield& h, const Region& r, const Footprint& f)
{
    std::vector<uint8_t> core(r.size(), 0);
    for (const std::vector<glm::dvec2>& ring : f.rings)
    {
        if (ring.size() < 2)
            continue;
        double zlo = ring[0].y, zhi = ring[0].y;
        for (const glm::dvec2& p : ring)
        {
            zlo = std::min(zlo, p.y);
            zhi = std::max(zhi, p.y);
        }
        const int za = std::max(r.z0, int(std::ceil((zlo - h.origin.y) / h.cell)));
        const int zb = std::min(r.z1, int(std::floor((zhi - h.origin.y) / h.cell)));
        std::vector<double> xs;
        for (int z = za; z <= zb && ring.size() >= 3; ++z)
        {
            const double wz = h.origin.y + double(z) * h.cell;
            xs.clear();
            for (size_t k = 0; k < ring.size(); ++k)
            {
                const glm::dvec2& a = ring[k];
                const glm::dvec2& b = ring[(k + 1) % ring.size()];
                if ((a.y <= wz) != (b.y <= wz))
                    xs.push_back(a.x + (wz - a.y) / (b.y - a.y) * (b.x - a.x));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2)
            {
                const int xa = std::max(r.x0, int(std::ceil((xs[k] - h.origin.x) / h.cell)));
                const int xb = std::min(r.x1, int(std::floor((xs[k + 1] - h.origin.x) / h.cell)));
                for (int x = xa; x <= xb; ++x)
                    core[r.at(x, z)] = 1;
            }
        }
        for (size_t k = 0; k < ring.size(); ++k)
        {
            const glm::dvec2& a = ring[k];
            const glm::dvec2& b = ring[(k + 1) % ring.size()];
            const int n = std::max(1, int(std::ceil(glm::length(b - a) / (h.cell * 0.5))));
            for (int s = 0; s <= n; ++s)
            {
                const glm::dvec2 p = (a + (b - a) * (double(s) / double(n)) - h.origin) / h.cell;
                const int cx = int(std::floor(p.x)), cz = int(std::floor(p.y));
                for (int dz = 0; dz <= 1; ++dz)
                    for (int dx = 0; dx <= 1; ++dx)
                    {
                        const int x = cx + dx, z = cz + dz;
                        if (x >= r.x0 && x <= r.x1 && z >= r.z0 && z <= r.z1)
                            core[r.at(x, z)] = 1;
                    }
            }
        }
    }
    return core;
}

/** Least-squares plane through the samples: (height at centroid, gradient dh/dx, dh/dz), centroid. */
struct Plane
{
    glm::dvec2 centroid{0.0};
    glm::dvec2 gradient{0.0};
};

Plane fitPlane(const Heightfield& h, const Region& r, const std::vector<uint8_t>& core)
{
    double n = 0.0, sx = 0.0, sz = 0.0, sh = 0.0, sxx = 0.0, szz = 0.0, sxz = 0.0, sxh = 0.0, szh = 0.0;
    for (int z = r.z0; z <= r.z1; ++z)
        for (int x = r.x0; x <= r.x1; ++x)
            if (core[r.at(x, z)])
            {
                const glm::dvec2 p = h.samplePos(x, z);
                const double v = h.at(x, z);
                n += 1.0;
                sx += p.x;
                sz += p.y;
                sh += v;
            }
    Plane pl;
    if (n <= 0.0)
        return pl;
    pl.centroid = {sx / n, sz / n};
    const double mh = sh / n;
    for (int z = r.z0; z <= r.z1; ++z)
        for (int x = r.x0; x <= r.x1; ++x)
            if (core[r.at(x, z)])
            {
                const glm::dvec2 p = h.samplePos(x, z) - pl.centroid;
                const double v = h.at(x, z) - mh;
                sxx += p.x * p.x;
                szz += p.y * p.y;
                sxz += p.x * p.y;
                sxh += p.x * v;
                szh += p.y * v;
            }
    const double det = sxx * szz - sxz * sxz;
    if (std::abs(det) > 1e-9)
        pl.gradient = {(sxh * szz - szh * sxz) / det, (szh * sxx - sxh * sxz) / det};
    return pl;
}
} // namespace

std::string revision(const Heightfield& h)
{
    LILA_ZONE();

    auto mix = [](uint64_t x) {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33;
        return x;
    };
    auto words = [&](const std::vector<float>& v) -> uint64_t {
        const size_t chunk = 1 << 16, chunks = (v.size() + chunk - 1) / chunk;
        std::vector<uint64_t> part(chunks);
        tbb::parallel_for(size_t(0), chunks, [&](size_t c) {
            uint64_t s = 0x9e3779b97f4a7c15ull * (c + 1);
            const size_t end = std::min(v.size(), (c + 1) * chunk);
            for (size_t k = c * chunk; k < end; ++k)
            {
                uint32_t b;
                std::memcpy(&b, &v[k], 4);
                s = (s ^ b) * 0x100000001b3ull;
            }
            part[c] = s;
        });
        uint64_t s = v.size();
        for (uint64_t p : part)
            s = mix(s ^ p);
        return s;
    };
    uint64_t s = mix(uint64_t(h.nx) << 32 | uint32_t(h.nz));
    for (double d : {h.cell, h.origin.x, h.origin.y})
    {
        uint64_t b;
        std::memcpy(&b, &d, 8);
        s = mix(s ^ b);
    }
    s = mix(s ^ words(h.heights));
    for (const auto& [name, values] : h.masks)
    {
        s = mix(s ^ std::hash<std::string>{}(name));
        s = mix(s ^ words(values));
    }
    return std::format("{:016x}", s);
}

Footprint::Footprint() : lowestY(kNaN) {}

Footprint footprintFromPolygon(std::vector<glm::dvec2> ring)
{
    Footprint f;
    if (ring.size() >= 2 && ring.front() == ring.back())
        ring.pop_back();
    if (ring.empty())
        return f;
    f.rings.push_back(ring);
    f.outline.push_back(std::move(ring));
    return f;
}

Footprint footprintFromMesh(const geo::Mesh& m, const glm::dmat4& toLocal)
{
    Footprint f;
    std::vector<glm::dvec3> local(m.pointCapacity());
    double lowest = std::numeric_limits<double>::infinity();
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            local[p] = glm::dvec3(toLocal * glm::dvec4(m.position(p), 1.0));
            lowest = std::min(lowest, local[p].y);
        }
    for (geo::Index fi = 0; fi < m.faceCapacity(); ++fi)
        if (m.faceValid(fi))
        {
            std::vector<glm::dvec2> ring;
            for (geo::Index p : m.facePoints(fi))
                ring.push_back({local[p].x, local[p].z});
            f.rings.push_back(std::move(ring));
        }
    if (std::isfinite(lowest))
        f.lowestY = lowest;
    // The border: boundary edges chained into loops; a closed mesh has none, so its hull stands in.
    std::multimap<geo::Index, geo::Index> next;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
            next.insert({m.edgePoint(e, 0), m.edgePoint(e, 1)}), next.insert({m.edgePoint(e, 1), m.edgePoint(e, 0)});
    std::set<std::pair<geo::Index, geo::Index>> used;
    for (const auto& [start, firstNext] : next)
    {
        if (used.count({start, firstNext}) || used.count({firstNext, start}))
            continue;
        std::vector<glm::dvec2> loop{{local[start].x, local[start].z}};
        geo::Index prev = start, cur = firstNext;
        used.insert({start, firstNext});
        while (cur != start && loop.size() <= next.size())
        {
            loop.push_back({local[cur].x, local[cur].z});
            geo::Index to = geo::kInvalid;
            for (auto [it, end] = next.equal_range(cur); it != end; ++it)
                if (it->second != prev && !used.count({cur, it->second}) && !used.count({it->second, cur}))
                {
                    to = it->second;
                    break;
                }
            if (to == geo::kInvalid)
                break;
            used.insert({cur, to});
            prev = cur;
            cur = to;
        }
        if (loop.size() >= 3)
            f.outline.push_back(std::move(loop));
    }
    if (f.outline.empty())
    {
        std::vector<glm::dvec2> pts;
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                pts.push_back({local[p].x, local[p].z});
        std::vector<glm::dvec2> hull = convexHull(std::move(pts));
        if (hull.size() >= 3)
        {
            if (f.rings.empty())
                f.rings.push_back(hull);
            f.outline.push_back(std::move(hull));
        }
    }
    return f;
}

Footprint footprintFromCurves(const gn::CurveSet& c, const glm::dmat4& toLocal)
{
    Footprint f;
    const gn::CurveSet e = c.isPoly() ? c : c.evaluated();
    double lowest = std::numeric_limits<double>::infinity();
    for (size_t k = 0; k < e.numCurves(); ++k)
    {
        std::vector<glm::dvec2> ring;
        for (const glm::dvec3& p : e.curvePoints(k))
        {
            const glm::dvec3 q = glm::dvec3(toLocal * glm::dvec4(p, 1.0));
            lowest = std::min(lowest, q.y);
            ring.push_back({q.x, q.z});
        }
        if (ring.size() >= 2)
        {
            f.lines.push_back(ring);
            if (e.cyclic[k] && ring.front() != ring.back())
                f.lines.back().push_back(ring.front());
        }
        if (ring.size() >= 2 && ring.front() == ring.back())
            ring.pop_back();
        if (ring.size() >= 2)
        {
            f.rings.push_back(ring);
            f.outline.push_back(std::move(ring));
        }
    }
    if (std::isfinite(lowest))
        f.lowestY = lowest;
    return f;
}

std::vector<uint8_t> footprintMask(const Heightfield& h, const Footprint& f)
{
    Region r;
    r.x0 = 0;
    r.z0 = 0;
    r.x1 = h.nx - 1;
    r.z1 = h.nz - 1;
    return rasterize(h, r, f);
}

std::vector<float> squaredDistances(const std::vector<uint8_t>& set, int w, int d) { return distanceTransform(set, w, d); }

const std::vector<std::string>& stampModeNames()
{
    static const std::vector<std::string> n = {"flatten", "carve", "raise", "blend", "plinth", "terraced", "stilts"};
    return n;
}

const std::vector<std::string>& stampEdgeNames()
{
    static const std::vector<std::string> n = {"slope", "wall", "steps"};
    return n;
}

const std::vector<std::string>& stampLevelNames()
{
    static const std::vector<std::string> n = {"mean", "min", "max", "median", "object", "value"};
    return n;
}

json Stamp::toJson() const
{
    json j = {{"id", id},
              {"kind", kind},
              {"mode", nameOf(stampModeNames(), mode)},
              {"edge", nameOf(stampEdgeNames(), edge)},
              {"level", nameOf(stampLevelNames(), level)},
              {"height", height},
              {"offset", offset},
              {"falloff", falloff},
              {"skirtGrade", skirtGrade},
              {"strength", strength},
              {"grade", grade},
              {"direction", json::array({direction.x, direction.y})},
              {"steps", steps},
              {"stepHeight", stepHeight},
              {"masks", masks},
              {"maskValue", maskValue},
              {"wallHeight", wallHeight}};
    if (!object.empty())
        j["object"] = object;
    else if (isLine())
        j["path"] = ringJson(path);
    else
        j["polygon"] = ringJson(polygon);
    if (isLine() || kind == "dredge")
    {
        j["width"] = width;
        j["depth"] = depth;
        j["rise"] = rise;
        j["side"] = side;
        j["maxGrade"] = maxGrade;
        j["clearance"] = clearance;
    }
    return j;
}

bool Stamp::isLine() const { return std::find(lineKinds().begin(), lineKinds().end(), kind) != lineKinds().end(); }

const std::vector<std::string>& lineKinds()
{
    static const std::vector<std::string> k = {"street", "canal", "moat", "quay", "levee", "embankment", "seawall", "bridge", "culvert", "tunnel"};
    return k;
}

std::vector<std::string> defaultMasks(const std::string& kind)
{
    if (kind == "street" || kind == "quay")
        return {"cityFootprint", "paved"};
    if (kind == "canal" || kind == "moat")
        return {"water"};
    if (kind == "bridge")
        return {};
    if (kind == "culvert" || kind == "tunnel")
        return {kind};
    return {"cityFootprint"};
}

Stamp Stamp::fromJson(const json& j)
{
    Stamp s;
    s.id = j.value("id", std::string());
    s.kind = j.value("kind", std::string("pad"));
    s.mode = parseName(stampModeNames(), j, "mode", StampMode::Flatten);
    s.edge = parseName(stampEdgeNames(), j, "edge", StampEdge::Slope);
    s.level = parseName(stampLevelNames(), j, "level", StampLevel::Mean);
    s.height = j.value("height", 0.0);
    s.offset = j.value("offset", 0.0);
    s.falloff = std::max(0.0, j.value("falloff", 500.0));
    s.skirtGrade = std::max(0.0, j.value("skirtGrade", 0.0));
    s.strength = std::clamp(j.value("strength", 1.0), 0.0, 1.0);
    s.grade = j.value("grade", 0.0);
    if (j.contains("direction") && j["direction"].is_array() && j["direction"].size() == 2)
        s.direction = {j["direction"][0].get<double>(), j["direction"][1].get<double>()};
    s.steps = std::clamp(j.value("steps", 0), 0, 64);
    s.stepHeight = std::max(1.0, j.value("stepHeight", 150.0));
    if (j.contains("masks"))
        s.masks = j["masks"].get<std::vector<std::string>>();
    s.maskValue = std::clamp(j.value("maskValue", 1.0), 0.0, 1.0);
    s.wallHeight = std::max(0.0, j.value("wallHeight", 50.0));
    s.object = j.value("object", std::string());
    s.polygon = ringFrom(j.value("polygon", json::array()));
    s.path = ringFrom(j.value("path", json::array()));
    s.width = std::max(1.0, j.value("width", 600.0));
    s.depth = std::max(0.0, j.value("depth", 200.0));
    s.rise = j.value("rise", 200.0);
    s.side = j.value("side", std::string("left"));
    s.maxGrade = std::max(0.0, j.value("maxGrade", 8.0));
    s.clearance = std::max(0.0, j.value("clearance", 500.0));
    return s;
}

namespace
{
/** Points every step along a polyline, with their distance along it. */
void resample(const std::vector<glm::dvec2>& line, double step, std::vector<glm::dvec2>& pts, std::vector<double>& along)
{
    pts.clear();
    along.clear();
    if (line.empty())
        return;
    pts.push_back(line[0]);
    along.push_back(0.0);
    double total = 0.0;
    for (size_t k = 1; k < line.size(); ++k)
    {
        const glm::dvec2 a = line[k - 1], b = line[k];
        const double len = glm::length(b - a);
        if (len < 1e-9)
            continue;
        const int n = std::max(1, int(std::ceil(len / step)));
        for (int s = 1; s <= n; ++s)
        {
            pts.push_back(a + (b - a) * (double(s) / double(n)));
            along.push_back(total + len * double(s) / double(n));
        }
        total += len;
    }
}

/**
 * A line feature: the target surface along the line from its graded centreline, laid across width (on one side for
 * quays, embankments and seawalls), with a skirt of falloff back to the ground.
 */
StampResult applyLine(Heightfield& h, const Stamp& s, const std::vector<glm::dvec2>& line)
{
    StampResult res;
    std::vector<glm::dvec2> pts;
    std::vector<double> along;
    resample(line, h.cell * 0.5, pts, along);
    const size_t m = pts.size();
    if (m < 2)
        return res;
    res.length = along.back();
    std::vector<double> ground(m), deck(m);
    for (size_t k = 0; k < m; ++k)
        ground[k] = h.heightAt(pts[k].x, pts[k].y);
    const std::string& kind = s.kind;
    double level = s.level == StampLevel::Value ? s.height : 0.0;
    if (kind == "street")
    {
        // Smooth the ground along the line, then hold it to the grade both ways.
        const double window = std::max(s.width * 2.0, h.cell * 5.0);
        size_t lo = 0, hi = 0;
        double sum = 0.0;
        for (size_t k = 0; k < m; ++k)
        {
            while (hi < m && along[hi] <= along[k] + window * 0.5)
                sum += ground[hi++];
            while (along[lo] < along[k] - window * 0.5)
                sum -= ground[lo++];
            deck[k] = sum / double(hi - lo);
        }
        const double g = s.maxGrade * 0.01;
        for (int pass = 0; pass < 4; ++pass)
        {
            for (size_t k = 1; k < m; ++k)
                deck[k] = std::clamp(deck[k], deck[k - 1] - g * (along[k] - along[k - 1]), deck[k - 1] + g * (along[k] - along[k - 1]));
            for (size_t k = m - 1; k-- > 0;)
                deck[k] = std::clamp(deck[k], deck[k + 1] - g * (along[k + 1] - along[k]), deck[k + 1] + g * (along[k + 1] - along[k]));
        }
        for (size_t k = 0; k < m; ++k)
            deck[k] += s.offset;
    }
    else if (kind == "canal" || kind == "moat")
    {
        if (s.level != StampLevel::Value)
            level = *std::min_element(ground.begin(), ground.end()) + s.offset;
        res.waterLevel = level;
        std::fill(deck.begin(), deck.end(), level - s.depth);
    }
    else if (kind == "quay" || kind == "seawall")
    {
        if (s.level != StampLevel::Value)
            level = std::accumulate(ground.begin(), ground.end(), 0.0) / double(m) + s.offset;
        if (kind == "seawall")
            level = std::max(level, *std::max_element(ground.begin(), ground.end()) + s.rise);
        std::fill(deck.begin(), deck.end(), level);
    }
    else if (kind == "levee" || kind == "embankment")
    {
        for (size_t k = 0; k < m; ++k)
            deck[k] = ground[k] + s.rise;
    }
    else if (kind == "bridge")
    {
        // A straight deck between the ends' ground (or at the given height), the ground cut below it to the clearance.
        for (size_t k = 0; k < m; ++k)
            deck[k] = (s.level == StampLevel::Value ? level : ground[0] + (ground[m - 1] - ground[0]) * along[k] / res.length) - s.clearance;
    }
    else
        deck = ground;

    // Streets leave hollows to bridges and rises to tunnels, rather than filling or cutting them.
    std::vector<uint8_t> keep(m, 0);
    if (kind == "street")
    {
        for (size_t k = 0; k < m; ++k)
        {
            const char* span = deck[k] - ground[k] > std::max(s.clearance, 300.0) ? "bridge" : ground[k] - deck[k] > 1500.0 ? "tunnel" : nullptr;
            if (span)
            {
                keep[k] = 1;
                if (res.spans.empty() || res.spans.back().kind != span || res.spans.back().to + 1 != k)
                    res.spans.push_back({k, k, span});
                else
                    res.spans.back().to = k;
            }
        }
        if (const std::vector<float>* flow = h.findMask("flow"); flow && flow->size() == h.size())
            for (size_t k = 1; k < m; ++k)
            {
                const double a = h.maskAt("flow", pts[k - 1].x, pts[k - 1].y), b = h.maskAt("flow", pts[k].x, pts[k].y);
                if (!keep[k] && a < 0.55 && b >= 0.55 && b < 0.75)
                    res.culverts.push_back(pts[k]);
            }
    }
    for (size_t k = 0; k < m; ++k)
    {
        res.centreline.push_back({pts[k].x, keep[k] ? deck[k] : deck[k] + (kind == "bridge" ? s.clearance : 0.0), pts[k].y});
        if (k > 0 && along[k] > along[k - 1])
            res.maxGrade = std::max(res.maxGrade, std::abs(deck[k] - deck[k - 1]) / (along[k] - along[k - 1]) * 100.0);
    }
    const bool registerOnly = kind == "culvert" || kind == "tunnel";
    const bool oneSided = kind == "quay" || kind == "seawall" || kind == "embankment";
    const double sideSign = s.side == "right" ? -1.0 : 1.0;
    const double half = s.width * 0.5;
    // A quay narrower than a cell still sets the land samples next to its face, which the grid draws as a one-cell slope.
    const double landHalf = kind == "quay" ? std::max(half, h.cell) : half;
    const double reach = landHalf + s.falloff + h.cell;

    // Nearest centreline point per sample, segment by segment over each one's box.
    glm::dvec2 bl(std::numeric_limits<double>::infinity()), bh(-std::numeric_limits<double>::infinity());
    for (const glm::dvec2& p : pts)
    {
        bl = glm::min(bl, p);
        bh = glm::max(bh, p);
    }
    const Region r = regionAround(h, bl, bh, reach + h.cell);
    if (r.empty())
        return res;
    std::vector<float> dist(r.size(), std::numeric_limits<float>::infinity()), cross(r.size(), 0.0f);
    std::vector<uint32_t> nearestK(r.size(), 0);
    for (size_t k = 0; k + 1 < m; ++k)
    {
        const glm::dvec2 a = pts[k], b = pts[k + 1], ab = b - a;
        const double len2 = std::max(1e-12, glm::dot(ab, ab));
        const Region sr = regionAround(h, glm::min(a, b), glm::max(a, b), reach);
        for (int z = std::max(sr.z0, r.z0); z <= std::min(sr.z1, r.z1); ++z)
            for (int x = std::max(sr.x0, r.x0); x <= std::min(sr.x1, r.x1); ++x)
            {
                const glm::dvec2 p = h.samplePos(x, z);
                const double t = std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0);
                const glm::dvec2 q = a + ab * t;
                const double d = glm::length(p - q);
                const size_t i = r.at(x, z);
                if (d < dist[i])
                {
                    dist[i] = float(d);
                    cross[i] = float(ab.x * (p - a).y - ab.y * (p - a).x);
                    nearestK[i] = uint32_t(t < 0.5 ? k : k + 1);
                }
            }
    }
    std::vector<std::vector<float>*> masks;
    for (const std::string& name : s.masks)
        if (!name.empty())
            masks.push_back(&h.mask(name));
    double cut = 0.0, fill = 0.0;
    size_t covered = 0;
    for (int z = r.z0; z <= r.z1; ++z)
        for (int x = r.x0; x <= r.x1; ++x)
        {
            const size_t i = r.at(x, z);
            const double d = dist[i];
            if (!(d < reach))
                continue;
            // Counterclockwise in (X, Z): positive cross is to the left of travel.
            const bool landSide = (cross[i] * sideSign) >= 0.0;
            const double core = landSide ? landHalf : half;
            double w = d <= core ? 1.0 : (s.falloff > 0.0 ? 1.0 - smoothstep(0.0, s.falloff, d - core) : 0.0);
            if (oneSided && !landSide)
                w = kind == "quay" && d <= half ? 1.0 : 0.0;
            if (w <= 0.0)
                continue;
            if (d <= core)
                ++covered;
            const size_t hi = h.index(x, z);
            for (std::vector<float>* mk : masks)
                (*mk)[hi] = std::max((*mk)[hi], float(w * s.maskValue));
            const uint32_t k = nearestK[i];
            if (registerOnly || keep[k])
                continue;
            const double old = h.heights[hi];
            double t = deck[k];
            if (kind == "quay" && !landSide)
                t = deck[k] - s.depth;
            if (kind == "levee" || kind == "embankment")
                t = deck[k] - (oneSided ? 0.0 : std::max(0.0, d - half * 0.5) / std::max(half, 1.0) * s.rise);
            double v = old;
            if (kind == "canal" || kind == "moat" || kind == "bridge" || (kind == "quay" && !landSide))
                v = old + (std::min(old, t) - old) * w;
            else if (kind == "levee" || kind == "embankment" || kind == "seawall")
                v = old + (std::max(old, t) - old) * w;
            else
                v = old + (t - old) * w;
            h.heights[hi] = float(v);
            cut += std::max(0.0, old - v);
            fill += std::max(0.0, v - old);
        }
    res.samples = covered;
    res.area = double(covered) * h.cell * h.cell;
    res.cut = cut * h.cell * h.cell;
    res.fill = fill * h.cell * h.cell;
    res.centroid = pts[m / 2];
    res.padHeight = deck[m / 2];
    res.groundMin = *std::min_element(ground.begin(), ground.end());
    res.groundMax = *std::max_element(ground.begin(), ground.end());
    res.groundMean = std::accumulate(ground.begin(), ground.end(), 0.0) / double(m);
    return res;
}
} // namespace

namespace
{
/**
 * The falloff a footprint stamp's skirt needs to fall no steeper than its skirtGrade: the level it will take, the most the
 * ground round it differs from that, and the smoothstep's peak slope of 1.5 times the mean. Between the given falloff
 * and four times it.
 */
double skirtFalloff(const Heightfield& h, const Stamp& s, const Footprint& f, glm::dvec2 lo, glm::dvec2 hi)
{
    const Region r0 = regionAround(h, lo, hi, 2.0 * h.cell);
    const std::vector<uint8_t> core = rasterize(h, r0, f);
    std::vector<float> ground;
    for (int z = r0.z0; z <= r0.z1; ++z)
        for (int x = r0.x0; x <= r0.x1; ++x)
            if (core[r0.at(x, z)])
                ground.push_back(h.at(x, z));
    if (ground.empty())
        return s.falloff;
    double base = std::accumulate(ground.begin(), ground.end(), 0.0) / double(ground.size());
    switch (s.level)
    {
    case StampLevel::Min: base = *std::min_element(ground.begin(), ground.end()); break;
    case StampLevel::Max: base = *std::max_element(ground.begin(), ground.end()); break;
    case StampLevel::Median:
        std::nth_element(ground.begin(), ground.begin() + ground.size() / 2, ground.end());
        base = ground[ground.size() / 2];
        break;
    case StampLevel::Object: base = std::isnan(f.lowestY) ? *std::min_element(ground.begin(), ground.end()) : f.lowestY; break;
    case StampLevel::Value: base = s.height; break;
    default: break;
    }
    base += s.offset;
    double falloff = s.falloff;
    for (int pass = 0; pass < 2; ++pass)
    {
        const Region ring = regionAround(h, lo, hi, falloff);
        double drop = 0.0;
        for (int z = ring.z0; z <= ring.z1; ++z)
            for (int x = ring.x0; x <= ring.x1; ++x)
            {
                const glm::dvec2 p = h.samplePos(x, z);
                const glm::dvec2 out = glm::max(glm::max(lo - p, p - hi), glm::dvec2(0.0));
                if (glm::length(out) <= falloff && (out.x > 0.0 || out.y > 0.0))
                    drop = std::max(drop, std::abs(double(h.at(x, z)) - base));
            }
        falloff = std::clamp(1.5 * drop / s.skirtGrade, s.falloff, 4.0 * s.falloff);
    }
    return falloff;
}
} // namespace

StampResult applyStamp(Heightfield& h, const Stamp& s, const Footprint& f)
{
    LILA_ZONE();

    StampResult res;
    if (h.empty())
        return res;
    if (s.isLine())
    {
        if (!s.path.empty())
            return applyLine(h, s, s.path);
        return f.lines.empty() ? res : applyLine(h, s, f.lines.front());
    }
    if (f.empty())
        return res;
    glm::dvec2 lo(std::numeric_limits<double>::infinity()), hi(-std::numeric_limits<double>::infinity());
    for (const auto& ring : f.rings)
        for (const glm::dvec2& p : ring)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    if (s.skirtGrade > 0.0 && s.edge != StampEdge::Wall && s.falloff > 0.0 && s.mode != StampMode::Plinth && s.mode != StampMode::Stilts)
    {
        Stamp widened = s;
        widened.skirtGrade = 0.0;
        widened.falloff = skirtFalloff(h, s, f, lo, hi);
        StampResult r = applyStamp(h, widened, f);
        r.falloff = widened.falloff;
        return r;
    }
    const bool skirt = s.edge != StampEdge::Wall && s.falloff > 0.0;
    const Region r = regionAround(h, lo, hi, (skirt ? s.falloff : 0.0) + 2.0 * h.cell);
    if (r.empty())
        return res;
    const std::vector<uint8_t> core = rasterize(h, r, f);

    std::vector<float> ground;
    for (int z = r.z0; z <= r.z1; ++z)
        for (int x = r.x0; x <= r.x1; ++x)
            if (core[r.at(x, z)])
                ground.push_back(h.at(x, z));
    if (ground.empty())
        return res;
    res.samples = ground.size();
    res.area = double(ground.size()) * h.cell * h.cell;
    res.groundMin = *std::min_element(ground.begin(), ground.end());
    res.groundMax = *std::max_element(ground.begin(), ground.end());
    res.groundMean = std::accumulate(ground.begin(), ground.end(), 0.0) / double(ground.size());
    std::nth_element(ground.begin(), ground.begin() + ground.size() / 2, ground.end());
    const double median = ground[ground.size() / 2];

    double base = res.groundMean;
    switch (s.level)
    {
    case StampLevel::Mean: base = res.groundMean; break;
    case StampLevel::Min: base = res.groundMin; break;
    case StampLevel::Max: base = res.groundMax; break;
    case StampLevel::Median: base = median; break;
    case StampLevel::Object: base = std::isnan(f.lowestY) ? res.groundMin : f.lowestY; break;
    case StampLevel::Value: base = s.height; break;
    }
    base += s.offset;
    res.padHeight = base;
    res.plinthDepth = base - res.groundMin;

    const Plane plane = fitPlane(h, r, core);
    res.centroid = plane.centroid;
    glm::dvec2 down = glm::length(s.direction) > 1e-12 ? glm::normalize(s.direction)
                      : glm::length(plane.gradient) > 1e-12 ? -glm::normalize(plane.gradient)
                                                            : glm::dvec2(1.0, 0.0);

    // Terraced: bands across the fall line, each at the mean ground inside it.
    double sLo = 0.0, sHi = 0.0;
    int bands = 1;
    std::vector<double> bandLevel;
    if (s.mode == StampMode::Terraced)
    {
        sLo = std::numeric_limits<double>::infinity();
        sHi = -sLo;
        for (int z = r.z0; z <= r.z1; ++z)
            for (int x = r.x0; x <= r.x1; ++x)
                if (core[r.at(x, z)])
                {
                    const double t = glm::dot(h.samplePos(x, z) - plane.centroid, down);
                    sLo = std::min(sLo, t);
                    sHi = std::max(sHi, t);
                }
        bands = s.steps > 0 ? s.steps : std::clamp(int(std::ceil((res.groundMax - res.groundMin) / s.stepHeight)), 1, 32);
        std::vector<double> sum(size_t(bands), 0.0), count(size_t(bands), 0.0);
        auto bandOf = [&](double t) { return sHi > sLo ? std::clamp(int((t - sLo) / (sHi - sLo) * bands), 0, bands - 1) : 0; };
        for (int z = r.z0; z <= r.z1; ++z)
            for (int x = r.x0; x <= r.x1; ++x)
                if (core[r.at(x, z)])
                {
                    const int b = bandOf(glm::dot(h.samplePos(x, z) - plane.centroid, down));
                    sum[size_t(b)] += h.at(x, z);
                    count[size_t(b)] += 1.0;
                }
        bandLevel.resize(size_t(bands));
        for (int b = 0; b < bands; ++b)
            bandLevel[size_t(b)] = (count[size_t(b)] > 0.0 ? sum[size_t(b)] / count[size_t(b)] : base) + s.offset;
        res.levels = bandLevel;
    }
    auto target = [&](glm::dvec2 p) {
        const double t = glm::dot(p - plane.centroid, down);
        if (s.mode == StampMode::Terraced)
            return bandLevel[size_t(sHi > sLo ? std::clamp(int((t - sLo) / (sHi - sLo) * bands), 0, bands - 1) : 0)];
        return base - s.grade * 0.01 * t;
    };

    std::vector<float> dist;
    if (skirt)
        dist = distanceTransform(core, r.w(), r.d());
    const bool moves = s.mode != StampMode::Plinth && s.mode != StampMode::Stilts;
    std::vector<std::vector<float>*> masks;
    for (const std::string& m : s.masks)
        if (!m.empty())
            masks.push_back(&h.mask(m));
    double cut = 0.0, fill = 0.0;
    for (int z = r.z0; z <= r.z1; ++z)
        for (int x = r.x0; x <= r.x1; ++x)
        {
            const size_t k = r.at(x, z);
            double w = core[k] ? 1.0 : 0.0;
            if (!core[k] && skirt)
                w = 1.0 - smoothstep(0.0, s.falloff, std::sqrt(double(dist[k])) * h.cell);
            if (w <= 0.0)
                continue;
            const size_t i = h.index(x, z);
            for (std::vector<float>* m : masks)
                (*m)[i] = std::max((*m)[i], float(w * s.maskValue));
            if (!moves)
                continue;
            const double old = h.heights[i];
            const double t = target(h.samplePos(x, z));
            double v = old;
            switch (s.mode)
            {
            case StampMode::Carve: v = old + (std::min(old, t) - old) * w; break;
            case StampMode::Raise: v = old + (std::max(old, t) - old) * w; break;
            case StampMode::Blend: v = old + (t - old) * w * s.strength; break;
            default: v = old + (t - old) * w; break;
            }
            if (s.kind == "dredge")
                v = old - s.depth * w;
            if (s.edge == StampEdge::Steps && !core[k])
                v = t + std::round((v - t) / s.stepHeight) * s.stepHeight;
            h.heights[i] = float(v);
            cut += std::max(0.0, old - v);
            fill += std::max(0.0, v - old);
        }
    res.cut = cut * h.cell * h.cell;
    res.fill = fill * h.cell * h.cell;

    if (s.mode == StampMode::Stilts || s.mode == StampMode::Plinth)
        for (const auto& ring : f.outline)
            for (const glm::dvec2& p : ring)
                res.piles.push_back({p, s.mode == StampMode::Plinth ? base : target(p), h.heightAt(p.x, p.y)});

    // Retaining walls: along the outline, where the pad and the ground beyond the skirt part by more than wallHeight
    // and the skirt would be steeper than 1:1.
    if (moves)
        for (const auto& ring : f.outline)
        {
            if (ring.size() < 2)
                continue;
            const double turn = signedArea(ring) >= 0.0 ? 1.0 : -1.0;
            const double reach = (skirt ? s.falloff : 0.0) + h.cell;
            std::vector<StampResult::WallPoint> run;
            auto flush = [&] {
                if (run.size() >= 2)
                    res.walls.push_back(run);
                run.clear();
            };
            for (size_t k = 0; k < ring.size(); ++k)
            {
                const glm::dvec2 a = ring[k], b = ring[(k + 1) % ring.size()];
                const double len = glm::length(b - a);
                if (len < 1e-9)
                    continue;
                const glm::dvec2 dir = (b - a) / len;
                // Counterclockwise rings (in X, Z) have their outside on the right of travel.
                const glm::dvec2 out = glm::dvec2(dir.y, -dir.x) * turn;
                const int n = std::max(1, int(std::ceil(len / h.cell)));
                for (int sidx = 0; sidx < n; ++sidx)
                {
                    const glm::dvec2 p = a + dir * (len * double(sidx) / double(n));
                    const glm::dvec2 q = p + out * reach;
                    const double pad = target(p), g = h.heightAt(q.x, q.y);
                    const double gap = std::abs(pad - g);
                    if (gap > s.wallHeight && gap > reach - h.cell)
                        run.push_back({p, std::max(pad, g), std::min(pad, g)});
                    else
                        flush();
                }
            }
            flush();
        }
    return res;
}

namespace
{
struct Published
{
    std::mutex mutex;
    std::map<std::pair<uint64_t, std::string>, std::map<std::string, StampResult>> results;
};

Published& published()
{
    static Published p;
    return p;
}
} // namespace

void publishStampResults(uint64_t modifier, const std::string& node, std::map<std::string, StampResult> results)
{
    Published& p = published();
    std::lock_guard lock(p.mutex);
    p.results[{modifier, node}] = std::move(results);
}

std::map<std::string, StampResult> stampResults(uint64_t modifier, const std::string& node)
{
    Published& p = published();
    std::lock_guard lock(p.mutex);
    auto it = p.results.find({modifier, node});
    return it == p.results.end() ? std::map<std::string, StampResult>{} : it->second;
}

const std::vector<std::string>& sculptOpNames()
{
    static const std::vector<std::string> n = {"raise", "lower", "smooth", "flatten", "noise", "ramp", "terrace", "stamp", "erode", "uplift", "relax", "mask", "patch"};
    return n;
}

SculptStroke::SculptStroke() : height(kNaN) {}

json SculptStroke::toJson() const
{
    json j = {{"op", nameOf(sculptOpNames(), op)}, {"points", ringJson(points)}, {"path", path},     {"radius", radius},
              {"strength", strength},             {"spacing", spacing},         {"noiseSize", noiseSize}, {"seed", seed}};
    if (!std::isnan(height))
        j["height"] = height;
    if (!pressure.empty())
        j["pressure"] = pressure;
    if (!falloff.empty())
        j["falloff"] = falloff;
    if (!mirror.empty())
        j["mirror"] = mirror;
    if (!settings.is_null() && !settings.empty())
        j["settings"] = settings;
    return j;
}

SculptStroke SculptStroke::fromJson(const json& j)
{
    SculptStroke s;
    s.op = parseName(sculptOpNames(), j, "op", SculptOp::Raise);
    s.points = ringFrom(j.value("points", json::array()));
    s.path = j.value("path", false);
    s.radius = std::max(1e-3, j.value("radius", 2000.0));
    s.strength = j.value("strength", 100.0);
    s.spacing = std::max(0.0, j.value("spacing", 0.0));
    s.height = j.contains("height") && j["height"].is_number() ? j["height"].get<double>() : kNaN;
    s.noiseSize = std::max(1e-3, j.value("noiseSize", 1000.0));
    s.seed = j.value("seed", 0u);
    if (j.contains("pressure") && j["pressure"].is_array())
        s.pressure = j["pressure"].get<std::vector<float>>();
    s.falloff = j.value("falloff", std::string());
    s.mirror = j.value("mirror", std::string());
    if (j.contains("settings") && j["settings"].is_object())
        s.settings = j["settings"];
    return s;
}

void applyStroke(Heightfield& h, const SculptStroke& s)
{
    LILA_ZONE();

    if (h.empty() || s.points.empty())
        return;
    if (usesBrushPath(s))
        return applyBrushStroke(h, s);
    std::vector<glm::dvec2> dabs;
    if (s.path && s.points.size() >= 2)
    {
        const double step = s.spacing > 0.0 ? s.spacing : std::max(s.radius * 0.25, h.cell * 0.5);
        dabs.push_back(s.points[0]);
        double carry = 0.0;
        for (size_t k = 1; k < s.points.size(); ++k)
        {
            const glm::dvec2 a = s.points[k - 1], b = s.points[k];
            const double len = glm::length(b - a);
            double t = step - carry;
            for (; t <= len; t += step)
                dabs.push_back(a + (b - a) * (t / len));
            carry = len - (t - step);
        }
        if (dabs.back() != s.points.back())
            dabs.push_back(s.points.back());
    }
    else
        dabs = s.points;

    glm::dvec2 lo(std::numeric_limits<double>::infinity()), hi(-std::numeric_limits<double>::infinity());
    for (const glm::dvec2& p : dabs)
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const int blurR = std::max(1, int(std::lround(s.radius * 0.15 / h.cell)));
    const Region r = regionAround(h, lo, hi, s.radius + h.cell * double(2 * blurR + 1));
    if (r.empty())
        return;
    // The stroke's reach: the strongest dab at each sample, so spacing never changes how much a path does.
    std::vector<float> w(r.size(), 0.0f);
    for (const glm::dvec2& d : dabs)
    {
        const Region dr = regionAround(h, d, d, s.radius);
        for (int z = std::max(dr.z0, r.z0); z <= std::min(dr.z1, r.z1); ++z)
            for (int x = std::max(dr.x0, r.x0); x <= std::min(dr.x1, r.x1); ++x)
            {
                const double t = glm::length(h.samplePos(x, z) - d) / s.radius;
                if (t >= 1.0)
                    continue;
                // Full strength over the inner 30%, so a dab's centre reaches its target between samples too.
                const double u = std::max(0.0, (t - 0.3) / 0.7);
                const double f = (1.0 - u * u) * (1.0 - u * u);
                float& v = w[r.at(x, z)];
                v = std::max(v, float(f));
            }
    }

    std::vector<float> blurred;
    if (s.op == SculptOp::Smooth)
    {
        // Two box passes each way: close to a Gaussian, over a margin wide enough that the edge never matters.
        std::vector<float> a(r.size()), b(r.size());
        for (int z = r.z0; z <= r.z1; ++z)
            for (int x = r.x0; x <= r.x1; ++x)
                a[r.at(x, z)] = h.at(x, z);
        auto box = [&](std::vector<float>& src, std::vector<float>& dst, bool alongX) {
            const int n = alongX ? r.w() : r.d(), m = alongX ? r.d() : r.w();
            for (int j = 0; j < m; ++j)
                for (int i = 0; i < n; ++i)
                {
                    double sum = 0.0;
                    int c = 0;
                    for (int o = -blurR; o <= blurR; ++o)
                    {
                        const int q = std::clamp(i + o, 0, n - 1);
                        sum += alongX ? src[size_t(j) * size_t(r.w()) + size_t(q)] : src[size_t(q) * size_t(r.w()) + size_t(j)];
                        ++c;
                    }
                    (alongX ? dst[size_t(j) * size_t(r.w()) + size_t(i)] : dst[size_t(i) * size_t(r.w()) + size_t(j)]) = float(sum / c);
                }
        };
        for (int pass = 0; pass < 2; ++pass)
        {
            box(a, b, true);
            box(b, a, false);
        }
        blurred = std::move(a);
    }
    double flatTarget = s.height;
    if (s.op == SculptOp::Flatten && std::isnan(flatTarget))
    {
        double sum = 0.0, sw = 0.0;
        for (int z = r.z0; z <= r.z1; ++z)
            for (int x = r.x0; x <= r.x1; ++x)
            {
                const double v = w[r.at(x, z)];
                sum += v * h.at(x, z);
                sw += v;
            }
        flatTarget = sw > 0.0 ? sum / sw : 0.0;
    }
    const double mix01 = std::clamp(s.strength, 0.0, 1.0);
    for (int z = r.z0; z <= r.z1; ++z)
        for (int x = r.x0; x <= r.x1; ++x)
        {
            const size_t k = r.at(x, z);
            const double v = w[k];
            if (v <= 0.0)
                continue;
            float& hv = h.heights[h.index(x, z)];
            switch (s.op)
            {
            case SculptOp::Raise: hv = float(hv + s.strength * v); break;
            case SculptOp::Lower: hv = float(hv - s.strength * v); break;
            case SculptOp::Smooth: hv = float(hv + (blurred[k] - hv) * v * mix01); break;
            case SculptOp::Flatten: hv = float(hv + (flatTarget - hv) * v * mix01); break;
            case SculptOp::Noise:
            {
                const glm::dvec2 p = h.samplePos(x, z) / s.noiseSize;
                double n = 0.0, amp = 1.0, freq = 1.0, norm = 0.0;
                for (int o = 0; o < 3; ++o)
                {
                    n += amp * gradientNoise(p.x * freq, p.y * freq, s.seed + uint32_t(o) * 977u);
                    norm += amp;
                    amp *= 0.5;
                    freq *= 2.0;
                }
                hv = float(hv + s.strength * v * n / norm);
                break;
            }
            }
        }
}

CostWeights CostWeights::fromJson(const json& j)
{
    CostWeights w;
    if (!j.is_object())
        return w;
    for (const auto& [k, v] : j.items())
    {
        if (!v.is_number())
            throw std::invalid_argument("weight " + k + " must be a number");
        if (k == "slope")
            w.slope = v.get<double>();
        else if (k == "water")
            w.water = v.get<double>();
        else if (k == "waterFlow")
            w.waterFlow = v.get<double>();
        else
            w.masks[k] = v.get<double>();
    }
    return w;
}

std::vector<float> costField(const Heightfield& h, const CostWeights& w)
{
    LILA_ZONE();

    std::vector<float> out(h.size(), 1.0f);
    const std::vector<float>* flow = h.findMask("flow");
    const std::vector<float>* lakes = h.findMask("lakes");
    std::vector<std::pair<const std::vector<float>*, double>> masks;
    for (const auto& [name, weight] : w.masks)
        if (const std::vector<float>* m = h.findMask(name); m && m->size() == h.size())
            masks.push_back({m, weight});
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t i = h.index(x, z);
            const double gx = (h.at(std::min(x + 1, h.nx - 1), z) - h.at(std::max(x - 1, 0), z)) / (h.cell * double(std::min(x + 1, h.nx - 1) - std::max(x - 1, 0)));
            const double gz = (h.at(x, std::min(z + 1, h.nz - 1)) - h.at(x, std::max(z - 1, 0))) / (h.cell * double(std::min(z + 1, h.nz - 1) - std::max(z - 1, 0)));
            const double slope = std::atan(std::sqrt(gx * gx + gz * gz)) * 57.29577951308232;
            double c = 1.0 + w.slope * slope / 10.0;
            const bool wet = (flow && flow->size() == h.size() && (*flow)[i] >= w.waterFlow) || (lakes && lakes->size() == h.size() && (*lakes)[i] > 0.0f);
            if (wet)
                c += w.water;
            for (const auto& [m, weight] : masks)
                c += weight * (*m)[i];
            out[i] = float(std::max(0.05, c));
        }
    });
    return out;
}

Route route(const Heightfield& src, glm::dvec2 a, glm::dvec2 b, const CostWeights& w, double maxGrade, int maxResolution)
{
    LILA_ZONE();

    Route out;
    if (src.empty())
        return out;
    const Heightfield* hp = &src;
    Heightfield coarse;
    const int longest = std::max(src.nx, src.nz);
    if (longest > maxResolution)
    {
        const double cell = src.cell * double(longest - 1) / double(maxResolution - 1);
        const glm::dvec2 e = src.extent();
        coarse = geo::resampled(src, int(e.x / cell) + 1, int(e.y / cell) + 1, cell, src.origin);
        hp = &coarse;
    }
    const Heightfield& h = *hp;
    const std::vector<float> cost = costField(h, w);
    auto cellOf = [&](glm::dvec2 p) {
        const int x = std::clamp(int(std::lround((p.x - h.origin.x) / h.cell)), 0, h.nx - 1);
        const int z = std::clamp(int(std::lround((p.y - h.origin.y) / h.cell)), 0, h.nz - 1);
        return uint32_t(h.index(x, z));
    };
    const uint32_t start = cellOf(a), goal = cellOf(b);
    static constexpr int kRouteDx[24] = {1, -1, 0, 0, 1, 1, -1, -1, 2, 2, -2, -2, 1, 1, -1, -1, 3, 3, -3, -3, 1, 1, -1, -1};
    static constexpr int kRouteDz[24] = {0, 0, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 2, -2, 2, -2, 1, -1, 1, -1, 3, -3, 3, -3};
    const glm::dvec2 goalPos = h.samplePos(int(goal % uint32_t(h.nx)), int(goal / uint32_t(h.nx)));

    auto search = [&](bool limitGrade, std::vector<uint32_t>& parent) -> bool {
        const size_t n = h.size();
        std::vector<float> g(n, std::numeric_limits<float>::infinity());
        parent.assign(n, UINT32_MAX);
        using Entry = std::pair<float, uint32_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
        g[start] = 0.0f;
        open.push({0.0f, start});
        while (!open.empty())
        {
            const auto [f, i] = open.top();
            open.pop();
            if (i == goal)
                return true;
            const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
            const glm::dvec2 pi = h.samplePos(x, z);
            if (f > g[i] + float(glm::length(goalPos - pi)) * 1.0001f)
                continue;
            for (int k = 0; k < 24; ++k)
            {
                const int nx = x + kRouteDx[k], nz = z + kRouteDz[k];
                if (nx < 0 || nz < 0 || nx >= h.nx || nz >= h.nz)
                    continue;
                const uint32_t j = uint32_t(h.index(nx, nz));
                const double run = std::sqrt(double(kRouteDx[k] * kRouteDx[k] + kRouteDz[k] * kRouteDz[k])) * h.cell;
                const double grade = std::abs(double(h.heights[j]) - double(h.heights[i])) / run * 100.0;
                if (limitGrade && grade > maxGrade)
                    continue;
                const double gradePenalty = maxGrade > 0.0 ? 1.0 + 0.5 * (grade / maxGrade) * (grade / maxGrade) : 1.0;
                const float ng = g[i] + float(run * 0.5 * double(cost[i] + cost[j]) * gradePenalty);
                if (ng < g[j])
                {
                    g[j] = ng;
                    parent[j] = i;
                    open.push({ng + float(glm::length(goalPos - h.samplePos(nx, nz))), j});
                }
            }
        }
        return false;
    };
    std::vector<uint32_t> parent;
    out.withinGrade = search(maxGrade > 0.0, parent);
    if (!out.withinGrade && !search(false, parent))
        return out;
    std::vector<uint32_t> cells;
    for (uint32_t c = goal; c != UINT32_MAX; c = parent[c])
    {
        cells.push_back(c);
        if (c == start)
            break;
    }
    std::reverse(cells.begin(), cells.end());
    std::vector<glm::dvec2> pts;
    pts.push_back(a);
    for (size_t k = 1; k + 1 < cells.size(); ++k)
        pts.push_back(h.samplePos(int(cells[k] % uint32_t(h.nx)), int(cells[k] / uint32_t(h.nx))));
    pts.push_back(b);
    // Drop points on a straight run so the path keeps only its turns.
    std::vector<glm::dvec2> kept{pts[0]};
    for (size_t k = 1; k + 1 < pts.size(); ++k)
    {
        const glm::dvec2 d0 = glm::normalize(pts[k] - kept.back()), d1 = pts[k + 1] - pts[k];
        if (glm::length(d1) < 1e-9 || std::abs(d0.x * d1.y - d0.y * d1.x) > 1e-6 * glm::length(d1))
            kept.push_back(pts[k]);
    }
    kept.push_back(pts.back());
    for (size_t k = 0; k < kept.size(); ++k)
    {
        const double y = src.heightAt(kept[k].x, kept[k].y);
        out.points.push_back({kept[k].x, y, kept[k].y});
        if (k > 0)
        {
            const glm::dvec3& p = out.points[k - 1];
            const double run = glm::length(kept[k] - kept[k - 1]);
            out.length += run;
            if (run > 1e-9)
                out.maxGrade = std::max(out.maxGrade, std::abs(y - p.y) / run * 100.0);
        }
    }
    for (size_t k = 1; k < cells.size(); ++k)
        out.cost += double(cost[cells[k]]) * glm::length(h.samplePos(int(cells[k] % uint32_t(h.nx)), int(cells[k] / uint32_t(h.nx))) -
                                                       h.samplePos(int(cells[k - 1] % uint32_t(h.nx)), int(cells[k - 1] / uint32_t(h.nx))));
    return out;
}
} // namespace terrain
