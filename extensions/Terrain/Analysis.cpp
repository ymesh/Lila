// Terrain analysis for the city contract: drainage into rivers, lakes and sea, the derived fields (hand, flood risk,
// buildability, solar, ...), the water graph, buildable regions and scored sites. All in the heightfield's own space.

#include "Analysis.h"
#include "ErosionGpu.h"
#include "Stamps.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <list>
#include <map>
#include <numeric>
#include <queue>
#include <set>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace
{
constexpr int kAx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
constexpr int kAz[8] = {0, 0, 1, -1, 1, -1, -1, 1};
constexpr double kDegPerRad = 57.29577951308232;

double phi(double z) { return 0.5 * (1.0 + std::erf(z / 1.4142135623730951)); }

struct Gradient
{
    double gx = 0.0, gz = 0.0;
};

Gradient gradientAt(const Heightfield& h, int x, int z)
{
    const int x0 = std::max(x - 1, 0), x1 = std::min(x + 1, h.nx - 1), z0 = std::max(z - 1, 0), z1 = std::min(z + 1, h.nz - 1);
    return {(h.at(x1, z) - h.at(x0, z)) / (h.cell * double(std::max(1, x1 - x0))), (h.at(x, z1) - h.at(x, z0)) / (h.cell * double(std::max(1, z1 - z0)))};
}

/** Window mean and standard deviation of a grid over a square of radius r samples, by summed-area tables. */
void windowStats(const std::vector<float>& v, int nx, int nz, int r, std::vector<float>& mean, std::vector<float>& sd)
{
    std::vector<double> s((size_t(nx) + 1) * (size_t(nz) + 1), 0.0), q(s.size(), 0.0);
    auto at = [&](int x, int z) { return size_t(z) * size_t(nx + 1) + size_t(x); };
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x)
        {
            const double a = v[size_t(z) * size_t(nx) + size_t(x)];
            s[at(x + 1, z + 1)] = a + s[at(x, z + 1)] + s[at(x + 1, z)] - s[at(x, z)];
            q[at(x + 1, z + 1)] = a * a + q[at(x, z + 1)] + q[at(x + 1, z)] - q[at(x, z)];
        }
    mean.assign(v.size(), 0.0f);
    sd.assign(v.size(), 0.0f);
    tbb::parallel_for(0, nz, [&](int z) {
        for (int x = 0; x < nx; ++x)
        {
            const int x0 = std::max(0, x - r), x1 = std::min(nx, x + r + 1), z0 = std::max(0, z - r), z1 = std::min(nz, z + r + 1);
            const double n = double(x1 - x0) * double(z1 - z0);
            const double sum = s[at(x1, z1)] - s[at(x0, z1)] - s[at(x1, z0)] + s[at(x0, z0)];
            const double sq = q[at(x1, z1)] - q[at(x0, z1)] - q[at(x1, z0)] + q[at(x0, z0)];
            const double m = sum / n;
            mean[size_t(z) * size_t(nx) + size_t(x)] = float(m);
            sd[size_t(z) * size_t(nx) + size_t(x)] = float(std::sqrt(std::max(0.0, sq / n - m * m)));
        }
    });
}

/** Nearest seed per sample (two-pass propagation of seed positions) and its distance in samples. */
void nearestSeeds(const std::vector<uint8_t>& seed, int nx, int nz, std::vector<int32_t>& nearest, std::vector<float>& dist)
{
    nearest.assign(seed.size(), -1);
    dist.assign(seed.size(), std::numeric_limits<float>::infinity());
    for (size_t i = 0; i < seed.size(); ++i)
        if (seed[i])
        {
            nearest[i] = int32_t(i);
            dist[i] = 0.0f;
        }
    auto relax = [&](int x, int z, int dx, int dz) {
        const int px = x + dx, pz = z + dz;
        if (px < 0 || pz < 0 || px >= nx || pz >= nz)
            return;
        const int32_t s = nearest[size_t(pz) * size_t(nx) + size_t(px)];
        if (s < 0)
            return;
        const double sx = s % nx, sz = s / nx;
        const float d = float(std::hypot(sx - x, sz - z));
        const size_t i = size_t(z) * size_t(nx) + size_t(x);
        if (d < dist[i])
        {
            dist[i] = d;
            nearest[i] = s;
        }
    };
    for (int z = 0; z < nz; ++z)
    {
        for (int x = 0; x < nx; ++x)
        {
            relax(x, z, -1, 0);
            relax(x, z, 0, -1);
            relax(x, z, -1, -1);
            relax(x, z, 1, -1);
        }
        for (int x = nx - 1; x >= 0; --x)
            relax(x, z, 1, 0);
    }
    for (int z = nz - 1; z >= 0; --z)
    {
        for (int x = nx - 1; x >= 0; --x)
        {
            relax(x, z, 1, 0);
            relax(x, z, 0, 1);
            relax(x, z, 1, 1);
            relax(x, z, -1, 1);
        }
        for (int x = 0; x < nx; ++x)
            relax(x, z, -1, 0);
    }
}

std::list<std::pair<std::string, std::shared_ptr<const Analysis>>>& analysisCache()
{
    static std::list<std::pair<std::string, std::shared_ptr<const Analysis>>> cache;
    return cache;
}

std::mutex& analysisMutex()
{
    static std::mutex m;
    return m;
}

json worldRingJson(const WorldMap& w, const Heightfield& h, const std::vector<glm::dvec2>& ring, double lift = 0.0)
{
    json out = json::array();
    for (const glm::dvec2& p : ring)
        out.push_back(w.point(p.x, h.heightAt(p.x, p.y) + lift, p.y));
    return out;
}

/** Keeps the best-scored samples, each at least spacing samples from those kept before. */
std::vector<uint32_t> pickBest(const std::vector<float>& score, int nx, int count, double spacingSamples, float minScore)
{
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < score.size(); ++i)
        if (score[i] > minScore)
            order.push_back(i);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return score[a] > score[b] || (score[a] == score[b] && a < b); });
    std::vector<uint32_t> kept;
    for (uint32_t i : order)
    {
        if (int(kept.size()) >= count)
            break;
        const double x = i % uint32_t(nx), z = i / uint32_t(nx);
        bool far = true;
        for (uint32_t k : kept)
            if (std::hypot(double(k % uint32_t(nx)) - x, double(k / uint32_t(nx)) - z) < spacingSamples)
            {
                far = false;
                break;
            }
        if (far)
            kept.push_back(i);
    }
    return kept;
}
} // namespace

AnalysisParams::AnalysisParams() : seaLevel(std::numeric_limits<double>::quiet_NaN()) {}

std::string AnalysisParams::key() const { return std::format("{}|{}|{}|{}", riverArea, lakeDepth, seaLevel, floodScale); }

json WorldMap::point(double x, double y, double z) const
{
    const glm::dvec3 p(m * glm::dvec4(x, y, z, 1.0));
    return json::array({p.x, p.y, p.z});
}

double WorldMap::height(double x, double y, double z) const { return glm::dot(glm::dvec3(m * glm::dvec4(x, y, z, 1.0)), up); }

int components(const std::vector<uint8_t>& set, int nx, int nz, std::vector<int32_t>& label)
{
    label.assign(set.size(), -1);
    int count = 0;
    std::vector<uint32_t> stack;
    for (uint32_t s = 0; s < set.size(); ++s)
    {
        if (!set[s] || label[s] >= 0)
            continue;
        label[s] = count;
        stack.push_back(s);
        while (!stack.empty())
        {
            const uint32_t i = stack.back();
            stack.pop_back();
            const int x = int(i % uint32_t(nx)), z = int(i / uint32_t(nx));
            for (int k = 0; k < 8; ++k)
            {
                const int px = x + kAx[k], pz = z + kAz[k];
                if (px < 0 || pz < 0 || px >= nx || pz >= nz)
                    continue;
                const uint32_t j = uint32_t(pz * nx + px);
                if (set[j] && label[j] < 0)
                {
                    label[j] = count;
                    stack.push_back(j);
                }
            }
        }
        ++count;
    }
    return count;
}

std::vector<std::vector<glm::dvec2>> contourLines(const std::vector<float>& g, int nx, int nz, float level)
{
    // Marching squares: segments keyed by the edge they cross, then chained.
    auto edgeKey = [&](int x, int z, int dir) { return (uint64_t(z) * uint64_t(nx) + uint64_t(x)) * 2 + uint64_t(dir); };
    auto edgePoint = [&](int x, int z, int dir) {
        const float a = g[size_t(z) * size_t(nx) + size_t(x)];
        const int x2 = dir == 0 ? x + 1 : x, z2 = dir == 0 ? z : z + 1;
        const float b = g[size_t(z2) * size_t(nx) + size_t(x2)];
        const double t = std::abs(b - a) > 1e-12f ? std::clamp(double(level - a) / double(b - a), 0.0, 1.0) : 0.5;
        return glm::dvec2(x + (x2 - x) * t, z + (z2 - z) * t);
    };
    std::multimap<uint64_t, uint64_t> links;
    std::map<uint64_t, glm::dvec2> points;
    for (int z = 0; z + 1 < nz; ++z)
        for (int x = 0; x + 1 < nx; ++x)
        {
            const bool c0 = g[size_t(z) * size_t(nx) + size_t(x)] >= level, c1 = g[size_t(z) * size_t(nx) + size_t(x + 1)] >= level;
            const bool c2 = g[size_t(z + 1) * size_t(nx) + size_t(x + 1)] >= level, c3 = g[size_t(z + 1) * size_t(nx) + size_t(x)] >= level;
            const int code = int(c0) | int(c1) << 1 | int(c2) << 2 | int(c3) << 3;
            if (code == 0 || code == 15)
                continue;
            // Edges: bottom (x,z)-(x+1,z), right (x+1,z)-(x+1,z+1), top (x,z+1)-(x+1,z+1), left (x,z)-(x,z+1).
            const uint64_t eb = edgeKey(x, z, 0), er = edgeKey(x + 1, z, 1), et = edgeKey(x, z + 1, 0), el = edgeKey(x, z, 1);
            auto seg = [&](uint64_t a, uint64_t b) {
                links.insert({a, b});
                links.insert({b, a});
            };
            switch (code)
            {
            case 1: case 14: seg(el, eb); break;
            case 2: case 13: seg(eb, er); break;
            case 3: case 12: seg(el, er); break;
            case 4: case 11: seg(er, et); break;
            case 6: case 9: seg(eb, et); break;
            case 7: case 8: seg(el, et); break;
            case 5: seg(el, et); seg(eb, er); break;
            case 10: seg(el, eb); seg(er, et); break;
            default: break;
            }
            for (uint64_t e : {eb, er, et, el})
                if (!points.count(e))
                {
                    const int ex = int((e / 2) % uint64_t(nx)), ez = int((e / 2) / uint64_t(nx)), dir = int(e % 2);
                    if ((dir == 0 && ex + 1 < nx) || (dir == 1 && ez + 1 < nz))
                        points[e] = edgePoint(ex, ez, dir);
                }
        }
    std::vector<std::vector<glm::dvec2>> out;
    std::set<std::pair<uint64_t, uint64_t>> used;
    auto unusedFrom = [&](uint64_t e) -> uint64_t {
        for (auto [it, end] = links.equal_range(e); it != end; ++it)
            if (!used.count({std::min(e, it->second), std::max(e, it->second)}))
                return it->second;
        return UINT64_MAX;
    };
    // Open lines first (start at an edge with one link), then the closed loops left.
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& [e, unusedTarget] : links)
        {
            if (pass == 0 && links.count(e) != 1)
                continue;
            uint64_t next = unusedFrom(e);
            if (next == UINT64_MAX)
                continue;
            std::vector<glm::dvec2> line{points[e]};
            uint64_t cur = e;
            while (next != UINT64_MAX)
            {
                used.insert({std::min(cur, next), std::max(cur, next)});
                line.push_back(points[next]);
                cur = next;
                next = unusedFrom(cur);
            }
            if (line.size() >= 2)
                out.push_back(std::move(line));
        }
    return out;
}

std::vector<std::vector<glm::dvec2>> outlines(const Heightfield& h, const std::vector<uint8_t>& set)
{
    // Pad by a ring of empty samples so outlines touching the edge close.
    const int nx = h.nx + 2, nz = h.nz + 2;
    std::vector<float> g(size_t(nx) * size_t(nz), 0.0f);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
            g[size_t(z + 1) * size_t(nx) + size_t(x + 1)] = set[h.index(x, z)] ? 1.0f : 0.0f;
    std::vector<std::vector<glm::dvec2>> out;
    for (auto& line : contourLines(g, nx, nz, 0.5f))
    {
        for (glm::dvec2& p : line)
            p = h.origin + (glm::clamp(p - glm::dvec2(1.0), glm::dvec2(0.0), glm::dvec2(h.nx - 1, h.nz - 1))) * h.cell;
        if (line.size() >= 2 && glm::length(line.front() - line.back()) < 1e-9)
            line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

Analysis::Analysis(std::shared_ptr<const Heightfield> hp, const AnalysisParams& p) : mH(std::move(hp)), mP(p)
{
    LILA_ZONE();

    const Heightfield& h = *mH;
    mD = drainAuto(h);
    const size_t n = h.size();
    mReachOf.assign(n, -1);
    mLakeOf.assign(n, -1);

    // Lakes: depressions deeper than lakeDepth; the sea: below sea level and joined to the edge.
    std::vector<uint8_t> lakeSet(n, 0);
    for (size_t i = 0; i < n; ++i)
        lakeSet[i] = mD.filled[i] - h.heights[i] > float(mP.lakeDepth) ? 1 : 0;
    std::vector<int32_t> label;
    const int lakeCount = components(lakeSet, h.nx, h.nz, label);
    mLakes.resize(size_t(lakeCount));
    for (int k = 0; k < lakeCount; ++k)
    {
        mLakes[size_t(k)].id = k;
        mLakes[size_t(k)].level = -1e30;
    }
    for (uint32_t i = 0; i < n; ++i)
        if (label[i] >= 0)
        {
            Lake& l = mLakes[size_t(label[i])];
            l.cells.push_back(i);
            l.level = std::max(l.level, double(mD.filled[i]));
            l.maxDepth = std::max(l.maxDepth, double(mD.filled[i] - h.heights[i]));
            mLakeOf[i] = label[i];
        }
    if (!std::isnan(mP.seaLevel))
    {
        std::vector<uint8_t> below(n, 0);
        std::deque<uint32_t> open;
        for (int z = 0; z < h.nz; ++z)
            for (int x = 0; x < h.nx; ++x)
                if ((x == 0 || z == 0 || x == h.nx - 1 || z == h.nz - 1) && h.at(x, z) < mP.seaLevel)
                {
                    below[h.index(x, z)] = 1;
                    open.push_back(uint32_t(h.index(x, z)));
                }
        while (!open.empty())
        {
            const uint32_t i = open.front();
            open.pop_front();
            const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
            for (int k = 0; k < 8; ++k)
            {
                const int px = x + kAx[k], pz = z + kAz[k];
                if (px < 0 || pz < 0 || px >= h.nx || pz >= h.nz)
                    continue;
                const uint32_t j = uint32_t(h.index(px, pz));
                if (!below[j] && h.heights[j] < mP.seaLevel)
                {
                    below[j] = 1;
                    open.push_back(j);
                }
            }
        }
        Lake sea;
        sea.id = int(mLakes.size());
        sea.sea = true;
        sea.level = mP.seaLevel;
        for (uint32_t i = 0; i < n; ++i)
            if (below[i])
            {
                sea.cells.push_back(i);
                sea.maxDepth = std::max(sea.maxDepth, mP.seaLevel - h.heights[i]);
                mLakeOf[i] = sea.id;
            }
        if (!sea.cells.empty())
            mLakes.push_back(std::move(sea));
    }
    for (Lake& l : mLakes)
        if (!l.sea)
            for (uint32_t i : l.cells)
            {
                // The lake drains out where its receiver chain first leaves it.
                uint32_t c = i;
                int guard = 0;
                while (mLakeOf[c] == l.id && mD.receiver[c] != c && guard++ < 1000000)
                    c = mD.receiver[c];
                if (mLakeOf[c] != l.id)
                {
                    l.outlet = c;
                    break;
                }
            }

    // Rivers: samples whose drainage area passes riverArea, outside lakes and sea.
    std::vector<uint8_t> river(n, 0);
    for (uint32_t i = 0; i < n; ++i)
        river[i] = areaM2(i) >= mP.riverArea && mLakeOf[i] < 0 ? 1 : 0;
    std::vector<int> donors(n, 0);
    for (uint32_t i = 0; i < n; ++i)
        if (river[i] && mD.receiver[i] != i && river[mD.receiver[i]])
            ++donors[mD.receiver[i]];
    // Reaches start at springs, confluences and lake outlets, and run down to the next of those or out.
    std::vector<int32_t> startReach(n, -1);
    for (auto it = mD.order.rbegin(); it != mD.order.rend(); ++it)
    {
        const uint32_t s = *it;
        if (!river[s] || mReachOf[s] >= 0)
            continue;
        const bool isLakeOutlet = std::any_of(mLakes.begin(), mLakes.end(), [&](const Lake& l) { return l.outlet == s; });
        if (donors[s] == 1 && !isLakeOutlet)
            continue;
        Reach r;
        r.id = int(mReaches.size());
        r.spring = donors[s] == 0 && !isLakeOutlet;
        uint32_t c = s;
        for (;;)
        {
            r.cells.push_back(c);
            mReachOf[c] = r.id;
            const uint32_t next = mD.receiver[c];
            if (next == c)
            {
                r.mouth = "edge";
                break;
            }
            if (mLakeOf[next] >= 0)
            {
                r.mouth = mLakes[size_t(mLakeOf[next])].sea ? "sea" : "lake";
                r.mouthLake = mLakeOf[next];
                break;
            }
            if (!river[next] || donors[next] > 1 || mReachOf[next] >= 0)
            {
                r.mouth = "confluence";
                break;
            }
            c = next;
        }
        startReach[s] = r.id;
        mReaches.push_back(std::move(r));
    }
    // Reaches above confluences were walked before the confluence's own reach existed: link them now.
    for (Reach& r : mReaches)
        if (r.mouth == "confluence")
        {
            const uint32_t next = mD.receiver[r.cells.back()];
            r.downstream = mReachOf[next];
            if (r.downstream < 0)
                r.mouth = "edge";
        }
    for (Reach& r : mReaches)
        if (r.downstream >= 0)
            mReaches[size_t(r.downstream)].upstream.push_back(r.id);
    // Strahler order, upstream first: reach ids grow downstream along the walk above.
    for (Reach& r : mReaches)
    {
        int best = 0, ties = 0;
        for (int u : r.upstream)
        {
            const int o = mReaches[size_t(u)].order;
            if (o > best)
            {
                best = o;
                ties = 1;
            }
            else if (o == best)
                ++ties;
        }
        r.order = r.upstream.empty() ? 1 : (ties > 1 ? best + 1 : best);
    }
    for (Lake& l : mLakes)
    {
        if (l.outlet != UINT32_MAX)
            l.outletReach = mReachOf[l.outlet];
        for (const Reach& r : mReaches)
            if (r.mouthLake == l.id)
                l.inlets.push_back(r.id);
    }
}

double Analysis::areaM2(uint32_t cell) const
{
    const double c = mH->cell * 0.01;
    return double(mD.area[cell]) * c * c;
}

double Analysis::discharge(uint32_t cell) const { return 0.02 * areaM2(cell) * 1e-6; }

const std::vector<std::string>& Analysis::fieldNames()
{
    static const std::vector<std::string> n = {"height", "slope", "aspect", "curvature", "roughness", "flow", "flowDir", "basinId", "streamOrder",
                                                "wet", "hand", "floodRisk", "distanceToWater", "nearestWaterId", "waterDepth", "rockHardness",
                                                "soilDepth", "excavationCost", "slopeClass", "buildability", "prominence", "viewshedScore",
                                                "solar", "windExposure"};
    return n;
}

const std::vector<float>& Analysis::field(const std::string& name) const
{
    static const std::vector<float> none;
    {
        std::lock_guard lock(mMutex);
        if (auto it = mFields.find(name); it != mFields.end())
            return *it->second;
    }
    std::vector<float> v = compute(name);
    std::lock_guard lock(mMutex);
    if (v.empty())
        return none;
    auto [it, fresh] = mFields.emplace(name, std::make_shared<const std::vector<float>>(std::move(v)));
    return *it->second;
}

double Analysis::sample(const std::string& name, double x, double z) const
{
    const std::vector<float>& f = field(name);
    if (f.empty())
        return 0.0;
    const Heightfield& h = *mH;
    const double fx = std::clamp((x - h.origin.x) / h.cell, 0.0, double(h.nx - 1)), fz = std::clamp((z - h.origin.y) / h.cell, 0.0, double(h.nz - 1));
    const int x0 = std::min(int(fx), h.nx - 2), z0 = std::min(int(fz), h.nz - 2);
    const double tx = fx - x0, tz = fz - z0;
    auto v = [&](int a, int b) { return double(f[h.index(a, b)]); };
    return (v(x0, z0) * (1 - tx) + v(x0 + 1, z0) * tx) * (1 - tz) + (v(x0, z0 + 1) * (1 - tx) + v(x0 + 1, z0 + 1) * tx) * tz;
}

std::vector<float> Analysis::compute(const std::string& name) const
{
    LILA_ZONE();

    const Heightfield& h = *mH;
    const size_t n = h.size();
    std::vector<float> out(n, 0.0f);
    auto each = [&](auto fn) {
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
                out[h.index(x, z)] = float(fn(x, z, h.index(x, z)));
        });
    };
    if (name == "height")
        return h.heights;
    if (name == "slope")
    {
        each([&](int x, int z, size_t) {
            const Gradient g = gradientAt(h, x, z);
            return std::atan(std::hypot(g.gx, g.gz)) * kDegPerRad;
        });
        return out;
    }
    if (name == "aspect")
    {
        // Degrees clockwise from north (-Z) to the downhill direction.
        each([&](int x, int z, size_t) {
            const Gradient g = gradientAt(h, x, z);
            if (std::hypot(g.gx, g.gz) < 1e-9)
                return -1.0;
            const double a = std::atan2(-g.gx, g.gz) * kDegPerRad;
            return a < 0.0 ? a + 360.0 : a;
        });
        return out;
    }
    if (name == "curvature")
    {
        // Positive on convex ground (ridges), per metre.
        each([&](int x, int z, size_t i) {
            const int x0 = std::max(x - 1, 0), x1 = std::min(x + 1, h.nx - 1), z0 = std::max(z - 1, 0), z1 = std::min(z + 1, h.nz - 1);
            const double lap = (h.at(x0, z) + h.at(x1, z) + h.at(x, z0) + h.at(x, z1) - 4.0 * h.heights[i]) / (h.cell * h.cell);
            return -lap * 100.0;
        });
        return out;
    }
    if (name == "roughness")
    {
        each([&](int x, int z, size_t) {
            double s = 0.0, q = 0.0;
            int c = 0;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const double v = h.at(std::clamp(x + dx, 0, h.nx - 1), std::clamp(z + dz, 0, h.nz - 1));
                    s += v;
                    q += v * v;
                    ++c;
                }
            return std::sqrt(std::max(0.0, q / c - (s / c) * (s / c)));
        });
        return out;
    }
    if (name == "flow")
    {
        float top = 1.0f;
        for (float a : mD.area)
            top = std::max(top, a);
        const float inv = top > 1.0f ? 1.0f / std::log(top) : 0.0f;
        each([&](int, int, size_t i) { return std::log(mD.area[i]) * inv; });
        return out;
    }
    if (name == "flowDir")
    {
        each([&](int x, int z, size_t i) {
            const uint32_t r = mD.receiver[i];
            if (r == i)
                return -1.0;
            const double dx = double(int(r % uint32_t(h.nx)) - x), dz = double(int(r / uint32_t(h.nx)) - z);
            const double a = std::atan2(dx, -dz) * kDegPerRad;
            return a < 0.0 ? a + 360.0 : a;
        });
        return out;
    }
    if (name == "basinId")
    {
        std::vector<int32_t> basin(n, -1);
        std::map<uint32_t, int32_t> ids;
        for (uint32_t i : mD.order)
        {
            const uint32_t r = mD.receiver[i];
            if (r == i)
                basin[i] = ids.emplace(i, int32_t(ids.size())).first->second;
            else
                basin[i] = basin[r];
        }
        for (size_t i = 0; i < n; ++i)
            out[i] = float(basin[i]);
        return out;
    }
    if (name == "streamOrder")
    {
        for (size_t i = 0; i < n; ++i)
            out[i] = mReachOf[i] >= 0 ? float(mReaches[size_t(mReachOf[i])].order) : 0.0f;
        return out;
    }
    if (name == "hand" || name == "floodRisk")
    {
        // Height above the drainage each sample reaches first, and that drainage's stream order.
        std::vector<float> base(n), order(n, 1.0f);
        for (uint32_t i : mD.order)
        {
            const uint32_t r = mD.receiver[i];
            if (mReachOf[i] >= 0 || mLakeOf[i] >= 0)
            {
                base[i] = mLakeOf[i] >= 0 ? float(mLakes[size_t(mLakeOf[i])].level) : h.heights[i];
                order[i] = mReachOf[i] >= 0 ? float(mReaches[size_t(mReachOf[i])].order) : 3.0f;
            }
            else if (r == i)
                base[i] = h.heights[i];
            else
            {
                base[i] = base[r];
                order[i] = order[r];
            }
        }
        if (name == "hand")
        {
            for (size_t i = 0; i < n; ++i)
                out[i] = std::max(0.0f, h.heights[i] - base[i]);
            return out;
        }
        for (size_t i = 0; i < n; ++i)
        {
            if (mLakeOf[i] >= 0)
            {
                out[i] = 1.0f;
                continue;
            }
            const double scale = mP.floodScale * 100.0 * (1.0 + 0.5 * (order[i] - 1.0));
            out[i] = float(std::exp(-std::max(0.0f, h.heights[i] - base[i]) / scale));
        }
        return out;
    }
    if (name == "distanceToWater" || name == "nearestWaterId")
    {
        std::vector<uint8_t> water(n, 0), land(n, 0);
        for (size_t i = 0; i < n; ++i)
        {
            water[i] = mReachOf[i] >= 0 || mLakeOf[i] >= 0 ? 1 : 0;
            land[i] = !water[i];
        }
        std::vector<int32_t> nearest, nearestLand;
        std::vector<float> d, dLand;
        nearestSeeds(water, h.nx, h.nz, nearest, d);
        if (name == "nearestWaterId")
        {
            for (size_t i = 0; i < n; ++i)
            {
                const int32_t s = nearest[i];
                out[i] = s < 0 ? -1.0f : mReachOf[size_t(s)] >= 0 ? float(mReachOf[size_t(s)]) : float(100000 + mLakeOf[size_t(s)]);
            }
            return out;
        }
        nearestSeeds(land, h.nx, h.nz, nearestLand, dLand);
        for (size_t i = 0; i < n; ++i)
            out[i] = water[i] ? -dLand[i] * float(h.cell) : (std::isfinite(d[i]) ? d[i] * float(h.cell) : 1e9f);
        return out;
    }
    if (name == "waterDepth")
    {
        for (size_t i = 0; i < n; ++i)
        {
            if (mLakeOf[i] >= 0)
                out[i] = float(std::max(0.0, mLakes[size_t(mLakeOf[i])].level - h.heights[i]));
            else if (mReachOf[i] >= 0)
                out[i] = float(30.0 * std::pow(discharge(uint32_t(i)), 0.4));
        }
        return out;
    }
    if (name == "rockHardness")
    {
        for (const char* m : {"rockHardness", "hardness", "rock"})
            if (const std::vector<float>* v = h.findMask(m); v && v->size() == n)
                return *v;
        const std::vector<float>& slope = field("slope");
        for (size_t i = 0; i < n; ++i)
            out[i] = float(std::clamp((slope[i] - 30.0) / 20.0, 0.0, 1.0));
        return out;
    }
    if (name == "soilDepth")
    {
        const std::vector<float>& slope = field("slope");
        const std::vector<float>* deposit = h.findMask("deposit");
        for (size_t i = 0; i < n; ++i)
            out[i] = float(std::clamp(1.0 - slope[i] / 40.0, 0.0, 1.0) * (0.4 + 0.6 * (deposit && deposit->size() == n ? (*deposit)[i] : 0.0f)));
        return out;
    }
    if (name == "excavationCost")
    {
        const std::vector<float>& rock = field("rockHardness");
        const std::vector<float>& soil = field("soilDepth");
        for (size_t i = 0; i < n; ++i)
            out[i] = float(std::clamp(0.15 + 0.85 * rock[i] * (1.0 - 0.7 * soil[i]), 0.0, 1.0));
        return out;
    }
    if (name == "slopeClass")
    {
        // 0 flat (<5%), 1 gentle (5-10%), 2 terraced (10-20%), 3 steep (20-35%), 4 unbuildable (>35% or cliff).
        const std::vector<float>& slope = field("slope");
        for (size_t i = 0; i < n; ++i)
        {
            const double pct = std::tan(slope[i] / kDegPerRad) * 100.0;
            out[i] = pct < 5.0 ? 0.0f : pct < 10.0 ? 1.0f : pct < 20.0 ? 2.0f : pct < 35.0 ? 3.0f : 4.0f;
        }
        return out;
    }
    if (name == "buildability")
    {
        const std::vector<float>& cls = field("slopeClass");
        const std::vector<float>& flood = field("floodRisk");
        const std::vector<float>& rock = field("rockHardness");
        const std::vector<float>& slope = field("slope");
        const std::vector<float>* deposit = h.findMask("deposit");
        constexpr float kByClass[5] = {1.0f, 0.85f, 0.6f, 0.3f, 0.0f};
        each([&](int x, int z, size_t i) {
            if (mLakeOf[i] >= 0 || mReachOf[i] >= 0)
                return 0.0;
            double cliff = 0.0;
            for (int k = 0; k < 8; ++k)
            {
                const int px = std::clamp(x + kAx[k], 0, h.nx - 1), pz = std::clamp(z + kAz[k], 0, h.nz - 1);
                cliff = std::max(cliff, double(slope[h.index(px, pz)]));
            }
            if (cliff > 45.0)
                return 0.0;
            const double talus = deposit && deposit->size() == n && (*deposit)[i] > 0.5f && slope[i] > 30.0f ? 0.5 : 1.0;
            return kByClass[std::clamp(int(cls[i]), 0, 4)] * (1.0 - 0.8 * flood[i]) * (1.0 - 0.3 * rock[i]) * talus;
        });
        return out;
    }
    if (name == "prominence" || name == "viewshedScore" || name == "windExposure")
    {
        const bool wind = name == "windExposure";
        const int r = std::max(2, int((wind ? 25000.0 : 50000.0) / h.cell));
        std::vector<float> mean, sd;
        windowStats(h.heights, h.nx, h.nz, r, mean, sd);
        if (name == "prominence")
            each([&](int, int, size_t i) { return std::clamp((h.heights[i] - mean[i]) / (3.0 * std::max(sd[i], 1.0f)), 0.0, 1.0); });
        else if (name == "viewshedScore")
            each([&](int, int, size_t i) { return phi((h.heights[i] - mean[i]) / std::max(sd[i], 1.0f)); });
        else
            each([&](int x, int z, size_t i) {
                // Prevailing wind from the west (-X): open ground and west-facing slopes catch it.
                const Gradient g = gradientAt(h, x, z);
                const glm::dvec3 nrm = glm::normalize(glm::dvec3(-g.gx, 1.0, -g.gz));
                return std::clamp(0.7 * phi((h.heights[i] - mean[i]) / std::max(sd[i], 1.0f)) + 0.3 * std::max(0.0, -nrm.x) * 2.0, 0.0, 1.0);
            });
        return out;
    }
    if (name == "solar")
    {
        // Sun from the south (+Z, north being -Z) at 40 degrees, averaged over south-east, south and south-west.
        const double e = 40.0 / kDegPerRad;
        std::vector<glm::dvec3> suns;
        for (double az : {-45.0, 0.0, 45.0})
            suns.push_back(glm::normalize(glm::dvec3(std::sin(az / kDegPerRad) * std::cos(e), std::sin(e), std::cos(az / kDegPerRad) * std::cos(e))));
        each([&](int x, int z, size_t) {
            const Gradient g = gradientAt(h, x, z);
            const glm::dvec3 nrm = glm::normalize(glm::dvec3(-g.gx, 1.0, -g.gz));
            double s = 0.0;
            for (const glm::dvec3& sun : suns)
                s += std::max(0.0, glm::dot(nrm, sun));
            return s / (3.0 * std::sin(e) / 1.0) * 0.75;
        });
        for (float& v : out)
            v = std::clamp(v, 0.0f, 1.0f);
        return out;
    }
    if (name == "wet")
    {
        const std::vector<float>& flow = field("flow");
        const std::vector<float>& hand = field("hand");
        for (size_t i = 0; i < n; ++i)
            out[i] = std::max(flow[i], float(std::exp(-hand[i] / 100.0)));
        return out;
    }
    if (const std::vector<float>* m = h.findMask(name == "sediment" ? "deposit" : name); m && m->size() == n)
        return *m;
    return {};
}

void forgetAnalyses()
{
    std::lock_guard lock(analysisMutex());
    analysisCache().clear();
}

std::shared_ptr<const Analysis> analyse(std::shared_ptr<const Heightfield> h, const AnalysisParams& p)
{
    const std::string key = revision(*h) + "|" + p.key();
    {
        std::lock_guard lock(analysisMutex());
        auto& cache = analysisCache();
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == key)
            {
                cache.splice(cache.begin(), cache, it);
                return cache.front().second;
            }
    }
    auto a = std::make_shared<const Analysis>(std::move(h), p);
    std::lock_guard lock(analysisMutex());
    auto& cache = analysisCache();
    cache.push_front({key, a});
    while (cache.size() > 3)
        cache.pop_back();
    return a;
}

json waterGraph(const Analysis& a, const WorldMap& w)
{
    LILA_ZONE();

    const Heightfield& h = a.heightfield();
    const std::vector<float>& slope = a.field("slope");
    const std::vector<float>& build = a.field("buildability");
    const std::vector<float>* deposit = h.findMask("deposit");
    auto pos = [&](uint32_t i) { return h.samplePos(int(i % uint32_t(h.nx)), int(i / uint32_t(h.nx))); };
    auto cellAt = [&](glm::dvec2 p) {
        const int x = std::clamp(int(std::lround((p.x - h.origin.x) / h.cell)), 0, h.nx - 1);
        const int z = std::clamp(int(std::lround((p.y - h.origin.y) / h.cell)), 0, h.nz - 1);
        return uint32_t(h.index(x, z));
    };

    json rivers = json::array(), fords = json::array(), bridgeSites = json::array(), confluences = json::array(), rapids = json::array(),
         waterfalls = json::array(), springs = json::array();
    for (const Reach& r : a.reaches())
    {
        json pts = json::array();
        bool navigable = true;
        std::vector<double> bridgeScore;
        std::vector<json> rows;
        const size_t m = r.cells.size();
        for (size_t k = 0; k < m; ++k)
        {
            const uint32_t i = r.cells[k];
            const glm::dvec2 p = pos(i);
            const glm::dvec2 prev = pos(r.cells[k > 0 ? k - 1 : k]), next = pos(r.cells[k + 1 < m ? k + 1 : k]);
            glm::dvec2 tan = next - prev;
            if (glm::length(tan) < 1e-9)
                tan = {1.0, 0.0};
            tan = glm::normalize(tan);
            const glm::dvec2 left(-tan.y, tan.x);
            const double q = a.discharge(i);
            const double width = 300.0 * std::sqrt(q), depth = 30.0 * std::pow(q, 0.4);
            const double speed = q / std::max(1e-6, width * 0.01 * depth * 0.01);
            const double bed = h.heights[i], level = bed + depth;
            json banks = json::object();
            double firm = 1.0, plain = 0.0, bankMin = 1e30;
            for (int side = 0; side < 2; ++side)
            {
                const glm::dvec2 dir = side == 0 ? left : -left;
                const glm::dvec2 edge = p + dir * (width * 0.5 + h.cell);
                const double bankTop = h.heightAt(edge.x, edge.y) - level;
                const glm::dvec2 far = p + dir * (width * 0.5 + 3.0 * h.cell);
                const double angle = std::atan2(h.heightAt(far.x, far.y) - level, glm::length(far - p) - width * 0.5) * kDegPerRad;
                const double dep = deposit && deposit->size() == h.size() ? (*deposit)[cellAt(edge)] : 0.0;
                const std::string type = angle > 60.0 ? "cliff" : angle > 25.0 ? "steep" : (dep > 0.4 && bankTop < 100.0) ? "gravelBar" : "low";
                double fp = 0.0;
                for (int s = 1; s <= 200; ++s)
                {
                    const glm::dvec2 q2 = p + dir * (width * 0.5 + double(s) * h.cell);
                    if (!h.contains(q2.x, q2.y) || h.heightAt(q2.x, q2.y) > level + 200.0)
                        break;
                    fp = double(s) * h.cell;
                }
                const char* sideName = side == 0 ? "L" : "R";
                banks[std::string("bankHeight") + sideName] = bankTop * w.vertical;
                banks[std::string("bankType") + sideName] = type;
                banks[std::string("floodplainWidth") + sideName] = fp * w.across;
                if (type == "low" || type == "gravelBar")
                    firm *= 0.4;
                plain = std::max(plain, fp);
                bankMin = std::min(bankMin, bankTop);
                const uint32_t bc = cellAt(edge);
                firm *= 0.3 + 0.7 * build[bc];
            }
            json pt = {{"position", w.point(p.x, level, p.y)},
                       {"width", width * w.across},
                       {"depth", depth * w.vertical},
                       {"bedHeight", w.height(p.x, bed, p.y)},
                       {"waterLevel", w.height(p.x, level, p.y)},
                       {"flowSpeed", speed},
                       {"discharge", q}};
            pt.update(banks);
            rows.push_back(pt);
            pts.push_back(pt);
            navigable = navigable && width >= 1500.0 && depth >= 150.0;
            // Bridge sites: narrow, firm high banks, straight, little floodplain.
            const glm::dvec2 far0 = pos(r.cells[k >= 3 ? k - 3 : 0]), far1 = pos(r.cells[std::min(m - 1, k + 3)]);
            const double straight = glm::length(far1 - far0) / std::max(1e-9, 6.0 * h.cell);
            bridgeScore.push_back(std::clamp(1.0 / (1.0 + width / 1000.0) * std::clamp(bankMin / 300.0, 0.0, 1.0) * firm * std::clamp(straight, 0.0, 1.0) /
                                                 (1.0 + plain / 5000.0),
                                             0.0, 1.0));
            if (depth < 70.0 && speed < 1.5 && width > 200.0 && k % 8 == 0)
                fords.push_back({{"position", pt["position"]}, {"river", r.id}, {"width", width * w.across}, {"depth", depth * w.vertical}});
        }
        // The best bridge sites per reach, a few widths apart.
        std::vector<size_t> idx(m);
        std::iota(idx.begin(), idx.end(), size_t(0));
        std::sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return bridgeScore[x] > bridgeScore[y]; });
        std::vector<size_t> kept;
        for (size_t k : idx)
        {
            if (kept.size() >= 3 || bridgeScore[k] <= 0.05)
                break;
            if (std::none_of(kept.begin(), kept.end(), [&](size_t o) { return (k > o ? k - o : o - k) < 10; }))
            {
                kept.push_back(k);
                bridgeSites.push_back({{"position", rows[k]["position"]}, {"river", r.id}, {"score", bridgeScore[k]}, {"span", rows[k]["width"]}});
            }
        }
        // Rapids where the bed falls steeply; waterfalls where it drops in one or two steps.
        for (size_t k = 0; k + 2 < m; ++k)
        {
            const double drop = h.heights[r.cells[k]] - h.heights[r.cells[k + 2]];
            const double run = glm::length(pos(r.cells[k + 2]) - pos(r.cells[k]));
            if (drop > 300.0 && run <= 3.0 * h.cell)
            {
                waterfalls.push_back({{"position", rows[k]["position"]}, {"river", r.id}, {"drop", drop * w.vertical}});
                k += 2;
            }
            else if (run > 0.0 && drop / run > 0.03 && k % 4 == 0)
                rapids.push_back({{"position", rows[k]["position"]}, {"river", r.id}, {"gradient", drop / run * 100.0}});
        }
        if (r.spring)
            springs.push_back({{"position", rows.front()["position"]}, {"river", r.id}});
        if (r.downstream >= 0)
            confluences.push_back({{"position", w.point(pos(a.reaches()[size_t(r.downstream)].cells.front()).x,
                                                        h.heights[a.reaches()[size_t(r.downstream)].cells.front()],
                                                        pos(a.reaches()[size_t(r.downstream)].cells.front()).y)},
                                   {"from", r.id},
                                   {"into", r.downstream}});
        json river = {{"id", r.id},
                      {"points", pts},
                      {"streamOrder", r.order},
                      {"navigable", navigable},
                      {"downstream", r.downstream},
                      {"tributaries", r.upstream},
                      {"mouth", r.mouth},
                      {"length", double(m) * h.cell * w.across}};
        if (r.mouthLake >= 0)
            river["mouthLake"] = r.mouthLake;
        rivers.push_back(river);
    }

    json lakes = json::array(), sea = nullptr;
    std::vector<uint8_t> set(h.size());
    for (const Lake& l : a.lakes())
    {
        std::fill(set.begin(), set.end(), 0);
        double depthSum = 0.0;
        for (uint32_t i : l.cells)
        {
            set[i] = 1;
            depthSum += l.level - h.heights[i];
        }
        json shores = json::array(), segments = json::array(), harbours = json::array();
        std::vector<float> score(h.size(), 0.0f);
        for (const auto& ring : outlines(h, set))
        {
            shores.push_back(worldRingJson(w, h, ring));
            std::string runType;
            json run = json::array();
            for (size_t k = 0; k < ring.size(); ++k)
            {
                const glm::dvec2 p = ring[k];
                const uint32_t c = cellAt(p);
                // Classify the shore by the ground just inland.
                std::string type = "rocky";
                double inland = 0.0;
                glm::dvec2 out(0.0);
                for (int s = 0; s < 8; ++s)
                {
                    const glm::dvec2 d(std::cos(s * 0.785398), std::sin(s * 0.785398));
                    const glm::dvec2 q2 = p + d * 3.0 * h.cell;
                    if (h.contains(q2.x, q2.y) && !set[cellAt(q2)] && h.heightAt(q2.x, q2.y) > inland)
                    {
                        inland = h.heightAt(q2.x, q2.y);
                        out = d;
                    }
                }
                const double sl = slope[c];
                if (sl < 2.0 && a.field("wet")[c] > 0.6f)
                    type = "marsh";
                else if (sl < 8.0)
                    type = "beach";
                else if (sl > 45.0)
                    type = "cliff";
                if (type != runType && !run.empty())
                {
                    segments.push_back({{"type", runType}, {"points", run}});
                    run = json::array();
                }
                runType = type;
                run.push_back(w.point(p.x, l.level, p.y));
                // Harbours: sheltered, deep a little offshore, flat land behind, a river mouth nearby.
                if (glm::length(out) > 0.0)
                {
                    int land = 0;
                    for (int s = 0; s < 16; ++s)
                    {
                        const glm::dvec2 d(std::cos(s * 0.3927), std::sin(s * 0.3927));
                        for (int t = 1; t <= 40; ++t)
                        {
                            const glm::dvec2 q2 = p - out * h.cell + d * double(t) * h.cell;
                            if (!h.contains(q2.x, q2.y))
                                break;
                            if (!set[cellAt(q2)])
                            {
                                ++land;
                                break;
                            }
                        }
                    }
                    const glm::dvec2 water = p - out * 3.0 * h.cell, behind = p + out * 3.0 * h.cell;
                    const double deep = h.contains(water.x, water.y) ? std::clamp((l.level - h.heightAt(water.x, water.y)) / 300.0, 0.0, 1.0) : 0.0;
                    const double flat = std::clamp(1.0 - slope[cellAt(behind)] / 10.0, 0.0, 1.0);
                    double mouth = 0.0;
                    for (int inlet : l.inlets)
                        if (glm::length(pos(a.reaches()[size_t(inlet)].cells.back()) - p) < 10.0 * h.cell)
                            mouth = 0.3;
                    score[c] = std::max(score[c], float((double(land) / 16.0) * deep * flat + mouth));
                }
            }
            if (!run.empty())
                segments.push_back({{"type", runType}, {"points", run}});
        }
        for (uint32_t c : pickBest(score, h.nx, 5, 20.0, 0.05f))
        {
            const glm::dvec2 p = pos(c);
            harbours.push_back({{"position", w.point(p.x, l.level, p.y)}, {"score", score[c]}});
        }
        json lake = {{"id", l.id},
                     {"waterLevel", w.height(pos(l.cells.front()).x, l.level, pos(l.cells.front()).y)},
                     {"area", double(l.cells.size()) * h.cell * h.cell * w.across * w.across * 1e-4},
                     {"maxDepth", l.maxDepth * w.vertical},
                     {"meanDepth", depthSum / double(l.cells.size()) * w.vertical},
                     {"shoreline", shores},
                     {"shore", segments},
                     {"harbourSites", harbours},
                     {"inlets", l.inlets}};
        if (l.outlet != UINT32_MAX)
        {
            const glm::dvec2 o = pos(l.outlet);
            lake["outlet"] = {{"position", w.point(o.x, h.heights[l.outlet], o.y)}, {"river", l.outletReach}};
        }
        if (l.sea)
            sea = lake;
        else
            lakes.push_back(lake);
    }

    // Wetlands: low (within half a metre of the drainage), flat and wet ground outside the water.
    const std::vector<float>& hand = a.field("hand");
    const std::vector<float>& flow = a.field("flow");
    std::vector<uint8_t> wet(h.size(), 0);
    for (size_t i = 0; i < h.size(); ++i)
        wet[i] = a.reachOf()[i] < 0 && a.lakeOf()[i] < 0 && hand[i] < 50.0f && slope[i] < 2.0f && flow[i] > 0.3f ? 1 : 0;
    std::vector<int32_t> label;
    const int count = components(wet, h.nx, h.nz, label);
    std::vector<size_t> sizes(size_t(count), 0);
    for (int32_t l : label)
        if (l >= 0)
            ++sizes[size_t(l)];
    json wetlands = json::array();
    for (int c = 0; c < count; ++c)
    {
        if (double(sizes[size_t(c)]) * h.cell * h.cell * 1e-4 < 5000.0)
            continue;
        std::fill(set.begin(), set.end(), 0);
        for (size_t i = 0; i < h.size(); ++i)
            set[i] = label[i] == c;
        json rings = json::array();
        for (const auto& ring : outlines(h, set))
            rings.push_back(worldRingJson(w, h, ring));
        wetlands.push_back({{"outline", rings}, {"area", double(sizes[size_t(c)]) * h.cell * h.cell * w.across * w.across * 1e-4}});
    }

    return {{"rivers", rivers},     {"lakes", lakes},           {"sea", sea},           {"springs", springs},     {"wetlands", wetlands},
            {"fords", fords},       {"bridgeSites", bridgeSites}, {"confluences", confluences}, {"rapids", rapids}, {"waterfalls", waterfalls}};
}

json buildableRegions(const Analysis& a, const WorldMap& w, double minAreaM2, double maxSlope, bool avoidFlood, double floodTolerance)
{
    LILA_ZONE();

    const Heightfield& h = a.heightfield();
    const std::vector<float>& slope = a.field("slope");
    const std::vector<float>& flood = a.field("floodRisk");
    const std::vector<float>& build = a.field("buildability");
    std::vector<uint8_t> ok(h.size(), 0);
    for (size_t i = 0; i < h.size(); ++i)
        ok[i] = slope[i] <= maxSlope && build[i] > 0.2f && (!avoidFlood || flood[i] < floodTolerance) ? 1 : 0;
    std::vector<int32_t> label;
    const int count = components(ok, h.nx, h.nz, label);
    struct Stat
    {
        size_t n = 0;
        double slope = 0.0, height = 0.0, build = 0.0;
        glm::dvec2 c{0.0};
    };
    std::vector<Stat> stats(static_cast<size_t>(count));
    for (size_t i = 0; i < h.size(); ++i)
        if (label[i] >= 0)
        {
            Stat& s = stats[size_t(label[i])];
            ++s.n;
            s.slope += slope[i];
            s.height += h.heights[i];
            s.build += build[i];
            s.c += h.samplePos(int(i % size_t(h.nx)), int(i / size_t(h.nx)));
        }
    const double cellM2 = h.cell * h.cell * 1e-4 * w.across * w.across;
    std::vector<int> order;
    for (int c = 0; c < count; ++c)
        if (double(stats[size_t(c)].n) * cellM2 >= minAreaM2)
            order.push_back(c);
    std::sort(order.begin(), order.end(), [&](int x, int y) { return stats[size_t(x)].n > stats[size_t(y)].n; });
    json out = json::array();
    std::vector<uint8_t> set(h.size());
    for (int c : order)
    {
        const Stat& s = stats[size_t(c)];
        std::fill(set.begin(), set.end(), 0);
        for (size_t i = 0; i < h.size(); ++i)
            set[i] = label[i] == c;
        // The longest ring is the outside; the others are holes.
        auto rings = outlines(h, set);
        std::sort(rings.begin(), rings.end(), [](const auto& x, const auto& y) { return x.size() > y.size(); });
        json holes = json::array();
        for (size_t k = 1; k < rings.size(); ++k)
            holes.push_back(worldRingJson(w, h, rings[k]));
        const glm::dvec2 centre = s.c / double(s.n);
        out.push_back({{"outline", rings.empty() ? json::array() : worldRingJson(w, h, rings[0])},
                       {"holes", holes},
                       {"area", double(s.n) * cellM2},
                       {"centroid", w.point(centre.x, h.heightAt(centre.x, centre.y), centre.y)},
                       {"meanSlope", s.slope / double(s.n)},
                       {"meanHeight", w.height(centre.x, s.height / double(s.n), centre.y)},
                       {"meanBuildability", s.build / double(s.n)}});
    }
    return out;
}

const std::vector<std::string>& siteKinds()
{
    static const std::vector<std::string> k = {"townCentre", "harbour", "bridge", "ford", "mill", "castle", "temple", "farmland", "quarry"};
    return k;
}

json sites(const Analysis& a, const WorldMap& w, const std::string& kind, int count, double spacing)
{
    LILA_ZONE();

    const Heightfield& h = a.heightfield();
    const size_t n = h.size();
    if (kind == "harbour" || kind == "bridge" || kind == "ford" || kind == "mill")
    {
        // These come from the water graph: take its scored points, best first.
        const json g = waterGraph(a, w);
        json pts = json::array();
        auto add = [&](const json& list, const char* scoreKey) {
            for (const json& p : list)
                pts.push_back({{"position", p["position"]}, {"score", scoreKey ? p.value(scoreKey, 0.5) : 0.5}, {"detail", p}});
        };
        if (kind == "harbour")
        {
            for (const json& l : g["lakes"])
                add(l["harbourSites"], "score");
            if (g["sea"].is_object())
                add(g["sea"]["harbourSites"], "score");
        }
        else if (kind == "bridge")
            add(g["bridgeSites"], "score");
        else if (kind == "ford")
            add(g["fords"], nullptr);
        else
        {
            for (const json& p : g["waterfalls"])
                pts.push_back({{"position", p["position"]}, {"score", 1.0}, {"detail", p}});
            for (const json& p : g["rapids"])
                pts.push_back({{"position", p["position"]}, {"score", std::clamp(p["gradient"].get<double>() / 10.0, 0.0, 0.9)}, {"detail", p}});
        }
        std::stable_sort(pts.begin(), pts.end(), [](const json& x, const json& y) { return x["score"].get<double>() > y["score"].get<double>(); });
        json out = json::array();
        for (const json& p : pts)
        {
            if (int(out.size()) >= count)
                break;
            const glm::dvec3 q(p["position"][0].get<double>(), p["position"][1].get<double>(), p["position"][2].get<double>());
            bool far = true;
            for (const json& o : out)
                if (glm::length(glm::dvec3(o["position"][0].get<double>(), o["position"][1].get<double>(), o["position"][2].get<double>()) - q) <
                    spacing * w.across)
                    far = false;
            if (far)
                out.push_back(p);
        }
        return out;
    }
    std::vector<float> score(n, 0.0f);
    const std::vector<float>& build = a.field("buildability");
    const std::vector<float>& slope = a.field("slope");
    if (kind == "townCentre")
    {
        const std::vector<float>& dist = a.field("distanceToWater");
        const std::vector<float>& flood = a.field("floodRisk");
        const std::vector<float>& order = a.field("streamOrder");
        // Crossings: bigger rivers make better places to cross and trade.
        std::vector<uint8_t> big(n, 0);
        for (size_t i = 0; i < n; ++i)
            big[i] = order[i] >= 2.0f;
        std::vector<int32_t> near;
        std::vector<float> dBig;
        nearestSeeds(big, h.nx, h.nz, near, dBig);
        for (size_t i = 0; i < n; ++i)
        {
            const double water = dist[i] > 0.0f ? std::exp(-double(dist[i]) / 50000.0) : 0.0;
            const double crossing = std::isfinite(dBig[i]) ? std::exp(-double(dBig[i]) * h.cell / 80000.0) : 0.0;
            score[i] = float(build[i] * (1.0 - slope[i] / 15.0 > 0.0 ? 1.0 - slope[i] / 15.0 : 0.0) * (0.4 + 0.3 * water + 0.3 * crossing) * (1.0 - flood[i]));
        }
    }
    else if (kind == "castle" || kind == "temple")
    {
        const std::vector<float>& prom = a.field("prominence");
        const std::vector<float>& view = a.field("viewshedScore");
        for (int z = 0; z < h.nz; ++z)
            for (int x = 0; x < h.nx; ++x)
            {
                const size_t i = h.index(x, z);
                // Defensible: steep ground around a buildable top.
                double around = 0.0;
                for (int k = 0; k < 8; ++k)
                    around += slope[h.index(std::clamp(x + 3 * kAx[k], 0, h.nx - 1), std::clamp(z + 3 * kAz[k], 0, h.nz - 1))];
                const double defend = std::clamp(around / 8.0 / 30.0, 0.0, 1.0);
                score[i] = float((kind == "castle" ? prom[i] * 0.6 + defend * 0.4 : prom[i] * 0.4 + view[i] * 0.6) * (build[i] > 0.3f ? 1.0 : 0.0));
            }
    }
    else if (kind == "farmland")
    {
        const std::vector<float>& wet = a.field("wet");
        const std::vector<float>& soil = a.field("soilDepth");
        const std::vector<float>& flood = a.field("floodRisk");
        for (size_t i = 0; i < n; ++i)
            score[i] = float(std::clamp(1.0 - slope[i] / 12.0, 0.0, 1.0) * (0.5 + 0.5 * wet[i]) * soil[i] * (1.0 - 0.5 * flood[i]) * (build[i] > 0.0f ? 1.0 : 0.0));
    }
    else if (kind == "quarry")
    {
        const std::vector<float>& rock = a.field("rockHardness");
        const std::vector<float>& soil = a.field("soilDepth");
        for (size_t i = 0; i < n; ++i)
            score[i] = float(rock[i] * (1.0 - soil[i]) * std::clamp(slope[i] / 30.0, 0.0, 1.0));
    }
    else
        throw std::invalid_argument("unknown site kind " + kind);
    json out = json::array();
    for (uint32_t c : pickBest(score, h.nx, count, spacing / h.cell, 0.01f))
    {
        const glm::dvec2 p = h.samplePos(int(c % uint32_t(h.nx)), int(c / uint32_t(h.nx)));
        out.push_back({{"position", w.point(p.x, h.heights[c], p.y)}, {"score", score[c]}});
    }
    return out;
}

bool raycast(const Heightfield& h, glm::dvec3 o, glm::dvec3 d, glm::dvec3* hit, double maxDistance)
{
    if (h.empty() || glm::length(d) < 1e-12)
        return false;
    d = glm::normalize(d);
    // Clip the ray to the grid's box, then march at half a cell and bisect the crossing.
    const glm::dvec2 lo = h.origin, hi = h.origin + h.extent();
    const glm::dvec2 r = h.range();
    double t0 = 0.0, t1 = maxDistance;
    const double bmin[3] = {lo.x, r.x - 1.0, lo.y}, bmax[3] = {hi.x, r.y + 1.0, hi.y};
    for (int k = 0; k < 3; ++k)
    {
        if (std::abs(d[k]) < 1e-12)
        {
            if (o[k] < bmin[k] || o[k] > bmax[k])
                return false;
            continue;
        }
        double a = (bmin[k] - o[k]) / d[k], b = (bmax[k] - o[k]) / d[k];
        if (a > b)
            std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    if (t0 > t1)
        return false;
    const double step = h.cell * 0.5;
    auto above = [&](double t) {
        const glm::dvec3 p = o + d * t;
        return p.y - h.heightAt(p.x, p.z);
    };
    double prev = t0, fprev = above(t0);
    if (fprev <= 0.0)
    {
        *hit = o + d * t0;
        return true;
    }
    for (double t = t0 + step; t <= t1 + step; t += step)
    {
        const double tc = std::min(t, t1);
        const double f = above(tc);
        if (f <= 0.0)
        {
            double a = prev, b = tc;
            for (int k = 0; k < 40; ++k)
            {
                const double m = 0.5 * (a + b);
                (above(m) > 0.0 ? a : b) = m;
            }
            *hit = o + d * b;
            return true;
        }
        prev = tc;
        fprev = f;
        if (tc >= t1)
            break;
    }
    return false;
}
} // namespace terrain
