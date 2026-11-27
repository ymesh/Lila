// Landform processes: analytic stream power, amplification, debris flow, thermal erosion per material, droplet rills,
// deposition, snow and the Erode preset chain. Every grid pass is either a per-sample parallel loop, a sequential walk in
// drainage order, or (droplets) parallel over tiles whose reach never overlaps, so results are the same bytes at any
// thread count.

#include "Landform.h"

#include "ErosionGpu.h"
#include "LandformGpu.h"
#include "Shaping.h"

#include <lila/Profile.h>

#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace terrain
{
namespace landform_
{
constexpr int kDx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
constexpr int kDz[8] = {0, 0, 1, -1, 1, -1, -1, 1};
constexpr double kDist[8] = {1, 1, 1, 1, 1.4142135623730951, 1.4142135623730951, 1.4142135623730951, 1.4142135623730951};
constexpr double kDeg = 0.017453292519943295;

std::vector<float> maskOrValue(const Heightfield& h, const std::string& name, float fallback)
{
    if (!name.empty())
        if (const std::vector<float>* m = h.findMask(name))
            return *m;
    return std::vector<float>(h.size(), fallback);
}

double receiverDistance(const Heightfield& h, uint32_t i, uint32_t r)
{
    const int x = int(i % uint32_t(h.nx)), z = int(i / uint32_t(h.nx));
    const int rx = int(r % uint32_t(h.nx)), rz = int(r / uint32_t(h.nx));
    return std::sqrt(double((x - rx) * (x - rx) + (z - rz) * (z - rz))) * h.cell;
}

bool onBorder(const Heightfield& h, int x, int z) { return x == 0 || z == 0 || x == h.nx - 1 || z == h.nz - 1; }

/** Highest of the 8 neighbours. */
float neighbourMax(const Heightfield& h, const std::vector<float>& z, uint32_t i)
{
    const int x = int(i % uint32_t(h.nx)), zz = int(i / uint32_t(h.nx));
    float m = -std::numeric_limits<float>::infinity();
    for (int k = 0; k < 8; ++k)
    {
        const int nx = x + kDx[k], nz = zz + kDz[k];
        if (nx >= 0 && nz >= 0 && nx < h.nx && nz < h.nz)
            m = std::max(m, z[h.index(nx, nz)]);
    }
    return m;
}

double quantile(std::vector<double> v, double q)
{
    if (v.empty())
        return 0.0;
    const size_t k = std::min(v.size() - 1, size_t(q * double(v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
    return v[k];
}

double smooth01(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/** Wear and deposit from the change against start, max-combined into the masks. */
void changeMasks(Heightfield& h, const std::vector<float>& start)
{
    std::vector<float> lost(h.size()), gained(h.size());
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) {
        const float c = h.heights[k] - start[k];
        lost[k] = std::max(0.0f, -c);
        gained[k] = std::max(0.0f, c);
    });
    accumulateMask(h, "wear", lost);
    accumulateMask(h, "deposit", gained);
}

/** Puts back what erosion took from shapes wider than radius samples, so only the fine detail is carved. */
void restoreLarge(Heightfield& h, const std::vector<float>& shaped, int radius)
{
    std::vector<float> diff(h.size());
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) { diff[k] = shaped[k] - h.heights[k]; });
    diff = boxBlurred(diff, h.nx, h.nz, radius);
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) { h.heights[k] += diff[k]; });
}

class Timer
{
public:
    explicit Timer(PresetReport* r) : mReport(r) {}
    void step(const char* name)
    {
        const auto now = std::chrono::steady_clock::now();
        if (mReport)
            mReport->steps.push_back({name, std::chrono::duration<double, std::milli>(now - mLast).count()});
        mLast = now;
    }

private:
    PresetReport* mReport;
    std::chrono::steady_clock::time_point mLast = std::chrono::steady_clock::now();
};

Drainage drainWith(const Heightfield& h, DrainSource* drains) { return drains ? (*drains)(h) : drainAuto(h); }

/** Prefetches a sample's row and its neighbours' rows of z, as neighbourMax reads them. */
void prefetchAround(const Heightfield& h, const std::vector<float>& z, uint32_t i)
{
    prefetchLine(&z[i]);
    if (i >= uint32_t(h.nx))
        prefetchLine(&z[i - uint32_t(h.nx)]);
    if (i + uint32_t(h.nx) < z.size())
        prefetchLine(&z[i + uint32_t(h.nx)]);
}
} // namespace landform_

Drainage DrainSource::operator()(const Heightfield& h)
{
    if (gpu)
    {
        Drainage d;
        std::string err;
        if (drainGpu(h, d, &err))
            return d;
        if (fallback.empty())
            fallback = "drainage ran on the CPU: " + err;
    }
    return drain(h);
}

void accumulateMask(Heightfield& h, const std::string& name, const std::vector<float>& src)
{
    float peak = 0.0f;
    for (float v : src)
        peak = std::max(peak, v);
    std::vector<float>& m = h.mask(name);
    if (peak <= 0.0f)
        return;
    const float inv = 1.0f / peak;
    tbb::parallel_for(size_t(0), std::min(m.size(), src.size()), [&](size_t k) { m[k] = std::max(m[k], src[k] * inv); });
}

uint64_t heightfieldHash(const Heightfield& h)
{
    uint64_t x = 1469598103934665603ull;
    auto bytes = [&](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t k = 0; k < n; ++k)
        {
            x ^= b[k];
            x *= 1099511628211ull;
        }
    };
    bytes(h.heights.data(), h.heights.size() * sizeof(float));
    for (const auto& [name, values] : h.masks)
    {
        bytes(name.data(), name.size());
        bytes(values.data(), values.size() * sizeof(float));
    }
    return x;
}

// ------------------------------------------------------------------------------------------------- analytic

void erodeAnalytic(Heightfield& h, const AnalyticParams& p, DrainSource* drains)
{
    LILA_ZONE();
    using namespace landform_;

    if (h.empty() || p.age <= 0.0)
        return;
    const size_t n = h.size();
    const glm::dvec2 range0 = h.range();
    const std::vector<float> start = h.heights;
    const Drainage d = drainWith(h, drains);
    const std::vector<float> hardness = maskOrValue(h, p.hardnessMask, 0.0f);
    const std::vector<float> uplift = maskOrValue(h, p.upliftMask, 1.0f);
    const double cellArea = (h.cell * 0.01) * (h.cell * 0.01);

    // Travel time of a base-level change from the outlet up to each sample: dtau = distance / (K A^m).
    std::vector<double> dtau(n, 0.0), tau(n, 0.0);
    tbb::parallel_for(size_t(0), n, [&](size_t i) {
        const uint32_t r = d.receiver[i];
        if (r == i)
            return;
        const double k = std::max(1e-9, p.strength * (1.0 - 0.9 * std::clamp(double(hardness[i]), 0.0, 1.0)));
        const double c = k * std::pow(double(d.area[i]) * cellArea, p.areaExponent);
        dtau[i] = receiverDistance(h, uint32_t(i), r) * 0.01 / std::max(c, 1e-12);
    });
    walkDrainage(
        d.order, d.receiver, false,
        [&](uint32_t i) {
            prefetchLine(&dtau[i]);
            prefetchLine(&tau[d.receiver[i]]);
        },
        [&](uint32_t i) {
            if (const uint32_t r = d.receiver[i]; r != i)
                tau[i] = tau[r] + dtau[i];
        });
    std::vector<double> inner;
    inner.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if (d.receiver[i] != i)
            inner.push_back(tau[i]);
    const double tauRef = quantile(inner, 0.98);
    if (tauRef <= 0.0)
        return;
    const double t = std::clamp(p.age, 0.0, 4.0) * tauRef;
    float lo = std::numeric_limits<float>::infinity(), hi = -lo;
    for (float v : d.filled)
    {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    // The uplift that makes the steady state as tall as the input, so age moves between the two at one relief.
    const double rate = double(hi - lo) / tauRef;
    std::vector<double> potential(n, 0.0);
    std::vector<double>& rise = dtau;
    tbb::parallel_for(size_t(0), n, [&](size_t i) { rise[i] = rate * double(uplift[i]) * dtau[i]; });
    walkDrainage(
        d.order, d.receiver, false,
        [&](uint32_t i) {
            prefetchLine(&rise[i]);
            prefetchLine(&potential[d.receiver[i]]);
        },
        [&](uint32_t i) {
            if (const uint32_t r = d.receiver[i]; r != i)
                potential[i] = potential[r] + rise[i];
        });

    // Children of each sample, in index order, for a walk down every outlet's tree with the path kept on a stack.
    std::vector<uint32_t> first(n + 1, 0), kids(n);
    std::vector<uint32_t> roots;
    for (size_t i = 0; i < n; ++i)
    {
        if (d.receiver[i] == i)
            roots.push_back(uint32_t(i));
        else
            ++first[d.receiver[i] + 1];
    }
    for (size_t i = 0; i < n; ++i)
        first[i + 1] += first[i];
    {
        std::vector<uint32_t> fillAt(first.begin(), first.end() - 1);
        for (size_t i = 0; i < n; ++i)
            if (d.receiver[i] != i)
                kids[fillAt[d.receiver[i]]++] = uint32_t(i);
    }

    struct Scratch
    {
        std::vector<uint32_t> node, next;
        std::vector<double> tau, height, pot;
    };
    tbb::enumerable_thread_specific<Scratch> scratch;
    std::vector<float> out(n);
    tbb::parallel_for(size_t(0), roots.size(), [&](size_t ri) {
        Scratch& s = scratch.local();
        s.node.clear();
        s.next.clear();
        s.tau.clear();
        s.height.clear();
        s.pot.clear();
        auto enter = [&](uint32_t v) {
            s.node.push_back(v);
            s.next.push_back(first[v]);
            s.tau.push_back(tau[v]);
            s.height.push_back(d.filled[v]);
            s.pot.push_back(potential[v]);
            const double target = tau[v] - t;
            double hv;
            if (target <= 0.0)
                hv = s.height[0] + potential[v];
            else
            {
                // The point downstream whose state reached here after time t: binary search the path's travel times.
                const size_t depth = s.tau.size();
                const size_t j = size_t(std::upper_bound(s.tau.begin(), s.tau.begin() + std::ptrdiff_t(depth), target) - s.tau.begin());
                const size_t a = j == 0 ? 0 : j - 1, b = std::min(j, depth - 1);
                const double span = s.tau[b] - s.tau[a];
                const double f = span > 0.0 ? (target - s.tau[a]) / span : 0.0;
                const double hd = s.height[a] + (s.height[b] - s.height[a]) * f;
                const double pd = s.pot[a] + (s.pot[b] - s.pot[a]) * f;
                hv = hd + potential[v] - pd;
            }
            out[v] = float(hv);
        };
        enter(roots[ri]);
        while (!s.node.empty())
        {
            const uint32_t v = s.node.back();
            uint32_t& c = s.next.back();
            if (c < first[v + 1])
            {
                const uint32_t child = kids[c++];
                enter(child);
                continue;
            }
            s.node.pop_back();
            s.next.pop_back();
            s.tau.pop_back();
            s.height.pop_back();
            s.pot.pop_back();
        }
    });

    if (p.keepRange)
    {
        float olo = std::numeric_limits<float>::infinity(), ohi = -olo;
        for (float v : out)
        {
            olo = std::min(olo, v);
            ohi = std::max(ohi, v);
        }
        const double scale = ohi > olo ? (range0.y - range0.x) / double(ohi - olo) : 1.0;
        tbb::parallel_for(size_t(0), n, [&](size_t k) { out[k] = float(range0.x + (double(out[k]) - olo) * scale); });
    }
    h.heights = std::move(out);
    if (p.writeMasks)
    {
        writeFlowMasks(h, drainWith(h, drains));
        changeMasks(h, start);
    }
}

// ------------------------------------------------------------------------------------------------- amplify

Heightfield upsampledCubic(const Heightfield& h, int nx, int nz, double cell, glm::dvec2 origin)
{
    LILA_ZONE();

    Heightfield out = lila::geo::resampled(h, nx, nz, cell, origin);
    auto cr = [](double p0, double p1, double p2, double p3, double t) {
        return p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
    };
    tbb::parallel_for(0, nz, [&](int z) {
        for (int x = 0; x < nx; ++x)
        {
            const glm::dvec2 q = (origin + glm::dvec2(x, z) * cell - h.origin) / h.cell;
            const double fx = std::clamp(q.x, 0.0, double(h.nx - 1)), fz = std::clamp(q.y, 0.0, double(h.nz - 1));
            const int ix = std::min(int(fx), h.nx - 2), iz = std::min(int(fz), h.nz - 2);
            const double tx = fx - ix, tz = fz - iz;
            double row[4];
            for (int j = 0; j < 4; ++j)
            {
                const int zz = std::clamp(iz - 1 + j, 0, h.nz - 1);
                auto at = [&](int xx) { return double(h.at(std::clamp(xx, 0, h.nx - 1), zz)); };
                row[j] = cr(at(ix - 1), at(ix), at(ix + 1), at(ix + 2), tx);
            }
            out.heights[out.index(x, z)] = float(cr(row[0], row[1], row[2], row[3], tz));
        }
    });
    return out;
}

Heightfield amplified(const Heightfield& h, const AmplifyParams& p)
{
    LILA_ZONE();

    const int f = std::clamp(p.factor, 1, 16);
    if (h.empty() || f == 1)
        return h;
    Heightfield hi = upsampledCubic(h, (h.nx - 1) * f + 1, (h.nz - 1) * f + 1, h.cell / f, h.origin);
    // Detail as tall as the ground drops over one input cell, so flats stay flat and slopes get texture.
    const double feature = h.cell * 1.5;
    tbb::parallel_for(0, hi.nz, [&](int z) {
        for (int x = 0; x < hi.nx; ++x)
        {
            const glm::dvec2 q = hi.samplePos(x, z);
            const glm::dvec3 nrm = h.normalAt(q.x, q.y);
            const double slope = std::sqrt(std::max(0.0, 1.0 - nrm.y * nrm.y)) / std::max(0.05, nrm.y);
            double sum = 0.0, amp = 1.0, norm = 0.0, fr = 1.0;
            for (int o = 0; o < 4; ++o)
            {
                sum += amp * gradientNoise(q.x / feature * fr, q.y / feature * fr, p.seed + 101u + uint32_t(o) * 7919u);
                norm += amp;
                amp *= 0.5;
                fr *= 2.0;
            }
            hi.heights[hi.index(x, z)] += float(p.detail * std::min(slope, 2.0) * h.cell * sum / norm);
        }
    });
    const std::vector<float> shaped = hi.heights;
    AnalyticParams a;
    a.age = p.age;
    erodeAnalytic(hi, a);
    landform_::restoreLarge(hi, shaped, 2 * f);
    return hi;
}

Heightfield amplifiedOnto(const Heightfield& coarse, const Heightfield& fine, double age, uint32_t seed, DrainSource* drains)
{
    LILA_ZONE();
    (void)seed;

    Heightfield out = upsampledCubic(coarse, fine.nx, fine.nz, fine.cell, fine.origin);
    // fine's own detail: itself minus what its coarse version looks like back on its grid.
    Heightfield fineCoarse = fine;
    fineCoarse.masks.clear();
    fineCoarse = lila::geo::resampled(fineCoarse, coarse.nx, coarse.nz, coarse.cell, coarse.origin);
    const Heightfield low = upsampledCubic(fineCoarse, fine.nx, fine.nz, fine.cell, fine.origin);
    tbb::parallel_for(size_t(0), out.size(), [&](size_t k) { out.heights[k] += fine.heights[k] - low.heights[k]; });
    out.masks = fine.masks;
    for (const auto& [name, values] : coarse.masks)
        if (!out.findMask(name))
            out.masks[name] = lila::geo::resampled(coarse, fine.nx, fine.nz, fine.cell, fine.origin).masks[name];
    if (age > 0.0)
    {
        const std::vector<float> shaped = out.heights;
        AnalyticParams a;
        a.age = age;
        a.writeMasks = false;
        erodeAnalytic(out, a, drains);
        landform_::restoreLarge(out, shaped, std::max(2, int(std::lround(2.0 * coarse.cell / fine.cell))));
    }
    return out;
}

// ------------------------------------------------------------------------------------------------- debris flow

void debrisFlow(Heightfield& h, const DebrisParams& p, DrainSource* drains)
{
    LILA_ZONE();
    using namespace landform_;

    if (h.empty() || p.strength <= 0.0)
        return;
    const double scour = std::tan(std::clamp(p.scourAngle, 1.0, 89.0) * kDeg);
    const double settle = std::tan(std::clamp(std::min(p.settleAngle, p.scourAngle - 0.5), 0.5, 89.0) * kDeg);
    const double cellArea = (h.cell * 0.01) * (h.cell * 0.01);
    std::vector<float> wear(h.size(), 0.0f), laid(h.size(), 0.0f), load(h.size());
    for (int it = 0; it < std::max(1, p.iterations); ++it)
    {
        const Drainage d = drainWith(h, drains);
        std::vector<float>& z = h.heights;
        z = d.filled;
        std::fill(load.begin(), load.end(), 0.0f);
        auto ahead = [&](uint32_t i) {
            const uint32_t r = d.receiver[i];
            prefetchAround(h, z, i);
            prefetchLine(&z[r]);
            prefetchLine(&d.area[i]);
            prefetchLine(&load[i]);
            prefetchLine(&load[r]);
        };
        walkDrainage(d.order, d.receiver, true, ahead, [&](uint32_t i) {
            const uint32_t r = d.receiver[i];
            if (r == i)
                return;
            const double dist = receiverDistance(h, i, r);
            const double s = (double(z[i]) - double(z[r])) / dist;
            const double area = double(d.area[i]) * cellArea;
            double q = load[i];
            if (s > scour && area < p.maxArea)
            {
                // Scour grows with the catchment until it is a river; it never cuts below the scour slope.
                const double reach = area / (area + 20.0 * cellArea);
                const double e = std::min(0.5 * (s - scour) * dist, p.strength * 0.25 * (s - scour) * dist * reach);
                z[i] = float(z[i] - e);
                q += e;
                wear[i] += float(e);
            }
            else if (s < settle && q > 0.0)
            {
                const double room = std::min(double(z[r]) + settle * dist, double(neighbourMax(h, z, i))) - double(z[i]);
                const double dep = std::clamp(q * 0.6 * (1.0 - s / settle), 0.0, std::max(0.0, room));
                z[i] = float(z[i] + dep);
                q -= dep;
                laid[i] += float(dep);
            }
            load[r] += float(q);
        });
    }
    accumulateMask(h, "wear", wear);
    accumulateMask(h, "deposit", laid);
    accumulateMask(h, "talus", laid);
}

// ------------------------------------------------------------------------------------------------- thermal

std::vector<float> talusAngles(const Heightfield& h, double baseAngle, const std::vector<std::pair<std::string, double>>& materials)
{
    std::vector<float> a(h.size(), float(std::clamp(baseAngle, 1.0, 89.0)));
    for (const auto& [mask, angle] : materials)
        if (const std::vector<float>* m = h.findMask(mask))
        {
            const float target = float(std::clamp(angle, 1.0, 89.0));
            for (size_t k = 0; k < a.size(); ++k)
                a[k] += (target - a[k]) * std::clamp((*m)[k], 0.0f, 1.0f);
        }
    return a;
}

void thermalMaterial(Heightfield& h, const std::vector<float>& angles, int iterations)
{
    LILA_ZONE();
    using namespace landform_;

    if (h.empty() || angles.size() != h.size())
        return;
    std::vector<float> talus(h.size());
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) { talus[k] = float(std::tan(std::clamp(double(angles[k]), 1.0, 89.0) * kDeg) * h.cell); });
    const std::vector<float> start = h.heights;
    std::vector<float> sum(h.size()), move(h.size()), delta(h.size());
    // The shedding sample's own angle decides, so loose sand next to a cliff still slumps.
    auto excess = [&](size_t i, int x, int z, int k) -> float {
        const int nx = x + kDx[k], nz = z + kDz[k];
        if (nx < 0 || nz < 0 || nx >= h.nx || nz >= h.nz)
            return 0.0f;
        // Float throughout, in this order: terrain_talus.slang matches it bit for bit.
        const float drop = h.heights[i] - h.heights[h.index(nx, nz)];
        const float hold = talus[i] * float(kDist[k]);
        return std::max(0.0f, drop - hold);
    };
    for (int it = 0; it < iterations; ++it)
    {
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const size_t i = h.index(x, z);
                float s = 0.0f, most = 0.0f;
                for (int k = 0; k < 8; ++k)
                {
                    const float e = excess(i, x, z, k);
                    s += e;
                    most = std::max(most, e);
                }
                sum[i] = s;
                move[i] = 0.5f * most;
            }
        });
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const size_t i = h.index(x, z);
                float in = 0.0f;
                for (int k = 0; k < 8; ++k)
                {
                    const int nx = x + kDx[k], nz = z + kDz[k];
                    if (nx < 0 || nz < 0 || nx >= h.nx || nz >= h.nz)
                        continue;
                    const size_t j = h.index(nx, nz);
                    if (sum[j] <= 0.0f)
                        continue;
                    in += move[j] * excess(j, nx, nz, k ^ 1) / sum[j];
                }
                delta[i] = in - (sum[i] > 0.0f ? move[i] : 0.0f);
            }
        });
        tbb::parallel_for(size_t(0), h.size(), [&](size_t i) { h.heights[i] += delta[i]; });
    }
    std::vector<float> gained(h.size());
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) { gained[k] = std::max(0.0f, h.heights[k] - start[k]); });
    accumulateMask(h, "talus", gained);
}

// ------------------------------------------------------------------------------------------------- droplets

namespace landform_
{
uint32_t mix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

double unitHash(uint32_t a, uint32_t b, uint32_t c) { return double(mix32(a * 0x9e3779b9u ^ mix32(b + 0x632be5abu) ^ mix32(c * 0x85ebca6bu)) >> 8) / double(1u << 24); }

struct Brush
{
    std::vector<int> dx, dz;
    std::vector<float> w;
};

Brush erosionBrush(int radius)
{
    Brush b;
    double total = 0.0;
    for (int z = -radius; z <= radius; ++z)
        for (int x = -radius; x <= radius; ++x)
        {
            const double d = std::sqrt(double(x * x + z * z));
            if (d >= double(radius) + 0.5)
                continue;
            const double w = double(radius) + 0.5 - d;
            b.dx.push_back(x);
            b.dz.push_back(z);
            b.w.push_back(float(w));
            total += w;
        }
    for (float& w : b.w)
        w = float(w / total);
    return b;
}

/** Height (in cells) and gradient at a position in sample coordinates, bilinear. */
void heightGradient(const std::vector<float>& z, int nx, double inv, double px, double pz, double& height, double& gx, double& gz)
{
    const int x = int(px), y = int(pz);
    const double u = px - x, v = pz - y;
    const size_t i = size_t(y) * size_t(nx) + size_t(x);
    const double a = z[i] * inv, b = z[i + 1] * inv, c = z[i + size_t(nx)] * inv, d = z[i + size_t(nx) + 1] * inv;
    gx = (b - a) * (1.0 - v) + (d - c) * v;
    gz = (c - a) * (1.0 - u) + (d - b) * u;
    height = a * (1.0 - u) * (1.0 - v) + b * u * (1.0 - v) + c * (1.0 - u) * v + d * u * v;
}
} // namespace landform_

void droplets(Heightfield& h, const DropletParams& p)
{
    LILA_ZONE();
    using namespace landform_;

    if (h.empty() || p.density <= 0.0 || h.nx < 8 || h.nz < 8)
        return;
    const int radius = std::clamp(p.radius, 1, 4);
    const Brush brush = erosionBrush(radius);
    const int steps = std::clamp(p.maxSteps, 1, 120);
    // Same-coloured tiles are a tile apart and a drop moves at most one sample a step, so tiles of one colour never
    // touch the same samples and can run in parallel in any order.
    const int tile = std::max(64, 2 * (steps + radius + 2));
    const int tx = (h.nx + tile - 1) / tile, tz = (h.nz + tile - 1) / tile;
    std::vector<float>& z = h.heights;
    const double inv = 1.0 / h.cell, scale = h.cell * std::max(0.0, p.strength);
    std::vector<float> wet(h.size(), 0.0f), wear(h.size(), 0.0f), laid(h.size(), 0.0f);
    const int nx = h.nx, nzc = h.nz;
    auto runDrop = [&](double px, double pz) {
        double dirX = 0.0, dirZ = 0.0, speed = 1.0, water = 1.0, sediment = 0.0;
        for (int s = 0; s < steps; ++s)
        {
            const int cx = int(px), cz = int(pz);
            const double u = px - cx, v = pz - cz;
            double height, gx, gz;
            heightGradient(z, nx, inv, px, pz, height, gx, gz);
            dirX = dirX * p.inertia - gx * (1.0 - p.inertia);
            dirZ = dirZ * p.inertia - gz * (1.0 - p.inertia);
            const double len = std::sqrt(dirX * dirX + dirZ * dirZ);
            if (len < 1e-12)
                break;
            dirX /= len;
            dirZ /= len;
            px += dirX;
            pz += dirZ;
            if (px < 1.0 || pz < 1.0 || px >= nx - 2 || pz >= nzc - 2)
                break;
            double nh, ngx, ngz;
            heightGradient(z, nx, inv, px, pz, nh, ngx, ngz);
            const double dh = nh - height;
            const double cap = std::max(-dh * speed * water * p.capacity, 0.01);
            const size_t i = size_t(cz) * size_t(nx) + size_t(cx);
            if (sediment > cap || dh > 0.0)
            {
                const double amount = dh > 0.0 ? std::min(dh, sediment) : (sediment - cap) * p.depositRate;
                sediment -= amount;
                const double a = amount * scale;
                const float w00 = float(a * (1 - u) * (1 - v)), w10 = float(a * u * (1 - v)), w01 = float(a * (1 - u) * v), w11 = float(a * u * v);
                z[i] += w00;
                z[i + 1] += w10;
                z[i + size_t(nx)] += w01;
                z[i + size_t(nx) + 1] += w11;
                laid[i] += w00;
                laid[i + 1] += w10;
                laid[i + size_t(nx)] += w01;
                laid[i + size_t(nx) + 1] += w11;
            }
            else
            {
                const double amount = std::min((cap - sediment) * p.erodeRate, -dh);
                for (size_t b = 0; b < brush.w.size(); ++b)
                {
                    const int bx = cx + brush.dx[b], bz = cz + brush.dz[b];
                    if (bx < 1 || bz < 1 || bx >= nx - 1 || bz >= nzc - 1)
                        continue;
                    const float e = float(amount * brush.w[b] * scale);
                    const size_t j = size_t(bz) * size_t(nx) + size_t(bx);
                    z[j] -= e;
                    wear[j] += e;
                }
                sediment += amount;
            }
            speed = std::sqrt(std::max(0.0, speed * speed - dh * 4.0));
            water *= 1.0 - p.evaporation;
            wet[i] += float(water);
        }
    };
    for (int colour = 0; colour < 4; ++colour)
    {
        std::vector<std::pair<int, int>> tiles;
        for (int j = colour / 2; j < tz; j += 2)
            for (int i = colour % 2; i < tx; i += 2)
                tiles.push_back({i, j});
        tbb::parallel_for(size_t(0), tiles.size(), [&](size_t t) {
            const int x0 = tiles[t].first * tile, z0 = tiles[t].second * tile;
            const int w = std::min(tile, nx - x0), d = std::min(tile, nzc - z0);
            const int64_t count = int64_t(std::llround(p.density * double(w) * double(d)));
            const uint32_t tileId = uint32_t(tiles[t].second * tx + tiles[t].first);
            for (int64_t k = 0; k < count; ++k)
            {
                const double px = x0 + unitHash(p.seed, tileId, uint32_t(2 * k)) * w;
                const double pz = z0 + unitHash(p.seed, tileId, uint32_t(2 * k + 1)) * d;
                if (px >= 1.0 && pz >= 1.0 && px < nx - 2 && pz < nzc - 2)
                    runDrop(px, pz);
            }
        });
    }
    std::vector<float> wetLog(h.size());
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) { wetLog[k] = std::log1p(wet[k]); });
    accumulateMask(h, "wet", wetLog);
    accumulateMask(h, "wear", wear);
    accumulateMask(h, "deposit", laid);
}

// ------------------------------------------------------------------------------------------------- deposition

void deposition(Heightfield& h, const DepositionParams& p, DrainSource* drains)
{
    LILA_ZONE();
    using namespace landform_;

    if (h.empty())
        return;
    const std::vector<float> hardness = maskOrValue(h, p.hardnessMask, 0.0f);
    const double cellArea = (h.cell * 0.01) * (h.cell * 0.01);
    std::vector<float> sediment(h.size(), 0.0f), worn(h.size(), 0.0f), load(h.size());
    std::vector<double> carry(h.size());
    std::vector<int32_t> lake(h.size());
    for (int it = 0; it < std::max(1, p.iterations); ++it)
    {
        const Drainage d = drainWith(h, drains);
        std::vector<float>& z = h.heights;
        std::fill(load.begin(), load.end(), 0.0f);
        // Lakes are the connected runs of filled samples; what flows into one settles from its floor up.
        std::vector<std::vector<uint32_t>> lakes;
        std::fill(lake.begin(), lake.end(), -1);
        if (p.fillLakes)
            for (uint32_t s = 0; s < uint32_t(h.size()); ++s)
            {
                if (lake[s] >= 0 || d.filled[s] - z[s] <= 1e-3f)
                    continue;
                const int32_t id = int32_t(lakes.size());
                lakes.push_back({s});
                lake[s] = id;
                for (size_t k = 0; k < lakes.back().size(); ++k)
                {
                    const uint32_t i = lakes.back()[k];
                    const int x = int(i % uint32_t(h.nx)), zz = int(i / uint32_t(h.nx));
                    for (int n = 0; n < 8; ++n)
                    {
                        const int nx = x + kDx[n], nz = zz + kDz[n];
                        if (nx < 0 || nz < 0 || nx >= h.nx || nz >= h.nz)
                            continue;
                        const uint32_t j = uint32_t(h.index(nx, nz));
                        if (lake[j] < 0 && d.filled[j] - z[j] > 1e-3f)
                        {
                            lake[j] = id;
                            lakes.back().push_back(j);
                        }
                    }
                }
            }
        std::vector<double> pool(lakes.size(), 0.0);
        tbb::parallel_for(size_t(0), h.size(), [&](size_t i) { carry[i] = p.transport * std::pow(double(d.area[i]) * cellArea, p.areaExponent); });
        auto ahead = [&](uint32_t i) {
            const uint32_t r = d.receiver[i];
            prefetchAround(h, z, i);
            prefetchLine(&d.filled[r]);
            prefetchLine(&carry[i]);
            prefetchLine(&lake[i]);
            prefetchLine(&load[i]);
            prefetchLine(&load[r]);
        };
        walkDrainage(d.order, d.receiver, true, ahead, [&](uint32_t i) {
            const uint32_t r = d.receiver[i];
            double q = load[i];
            if (lake[i] >= 0)
            {
                pool[size_t(lake[i])] += q;
                return;
            }
            if (r == i)
                return;
            const double dist = receiverDistance(h, i, r);
            const double s = std::max(0.0, (double(z[i]) - double(d.filled[r])) / dist);
            const double cap = carry[i] * s;
            if (q < cap)
            {
                const double e = std::min(p.supply * std::min(s, 1.5) * (1.0 - 0.8 * std::clamp(double(hardness[i]), 0.0, 1.0)), cap - q);
                z[i] = float(z[i] - e);
                q += e;
                worn[i] += float(e);
            }
            else
            {
                const double room = double(neighbourMax(h, z, i)) - double(z[i]);
                const double dep = std::clamp((q - cap) * p.depositRate, 0.0, std::max(0.0, room));
                z[i] = float(z[i] + dep);
                q -= dep;
                sediment[i] += float(dep);
            }
            load[r] += float(q);
        });
        // Each lake fills to the level that holds its pool (never past its spill level), lowest samples first.
        for (size_t l = 0; l < lakes.size(); ++l)
        {
            if (pool[l] <= 0.0)
                continue;
            std::vector<uint32_t>& cells = lakes[l];
            std::sort(cells.begin(), cells.end(), [&](uint32_t a, uint32_t b) { return z[a] != z[b] ? z[a] < z[b] : a < b; });
            double spill = std::numeric_limits<double>::infinity();
            for (uint32_t c : cells)
                spill = std::min(spill, double(d.filled[c]));
            double level = z[cells[0]], left = pool[l];
            size_t k = 1;
            while (left > 0.0 && level < spill)
            {
                const double next = k < cells.size() ? std::min(double(z[cells[k]]), spill) : spill;
                const double need = (next - level) * double(k);
                if (need >= left)
                {
                    level += left / double(k);
                    left = 0.0;
                    break;
                }
                left -= need;
                level = next;
                ++k;
            }
            for (uint32_t c : cells)
                if (z[c] < level)
                {
                    sediment[c] += float(level - z[c]);
                    z[c] = float(level);
                }
        }
    }
    accumulateMask(h, "sediment", sediment);
    accumulateMask(h, "deposit", sediment);
    accumulateMask(h, "wear", worn);
}

// ------------------------------------------------------------------------------------------------- snow

SnowParams::SnowParams() : snowline(std::numeric_limits<double>::quiet_NaN()) {}

void snow(Heightfield& h, const SnowParams& p)
{
    LILA_ZONE();
    using namespace landform_;

    if (h.empty())
        return;
    const glm::dvec2 range = h.range();
    double line = p.snowline;
    if (std::isnan(line))
        line = quantile(std::vector<double>(h.heights.begin(), h.heights.end()), 0.6);
    const double band = p.band > 0.0 ? p.band : std::max(1.0, (range.y - range.x) * 0.1);
    const double maxTan = std::tan(std::clamp(p.maxSlope, 1.0, 89.0) * kDeg);
    const double depth = std::max(1.0, p.depth);
    std::vector<float> s(h.size());
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const int x0 = std::max(0, x - 1), x1 = std::min(h.nx - 1, x + 1);
            const int z0 = std::max(0, z - 1), z1 = std::min(h.nz - 1, z + 1);
            const double gx = (h.at(x1, z) - h.at(x0, z)) / (std::max(1, x1 - x0) * h.cell);
            const double gz = (h.at(x, z1) - h.at(x, z0)) / (std::max(1, z1 - z0) * h.cell);
            const double slope = std::sqrt(gx * gx + gz * gz);
            const double hv = h.at(x, z);
            const double lap = 0.25 * (h.at(x0, z) + h.at(x1, z) + h.at(x, z0) + h.at(x, z1)) - hv;
            const double altitude = smooth01((hv - line + band * 0.5) / band);
            const double hold = 1.0 - smooth01((slope - maxTan * 0.8) / (maxTan * 0.4));
            const double hollow = 1.0 + p.hollows * std::clamp(lap / (h.cell * 0.1), 0.0, 1.0);
            s[h.index(x, z)] = float(depth * altitude * hold * hollow);
        }
    });
    // Snow on ground steeper than it can hold runs to the lowest neighbour, half the excess a step.
    const double holdDrop = maxTan * h.cell;
    std::vector<float> give(h.size()), next(h.size());
    std::vector<int8_t> to(h.size());
    for (int it = 0; it < std::max(0, p.slide); ++it)
    {
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const size_t i = h.index(x, z);
                const double top = h.heights[i] + s[i];
                double best = 0.0;
                int8_t dir = -1;
                for (int k = 0; k < 8; ++k)
                {
                    const int nx = x + kDx[k], nz = z + kDz[k];
                    if (nx < 0 || nz < 0 || nx >= h.nx || nz >= h.nz)
                        continue;
                    const size_t j = h.index(nx, nz);
                    const double drop = top - (h.heights[j] + s[j]) - holdDrop * kDist[k];
                    if (drop > best)
                    {
                        best = drop;
                        dir = int8_t(k);
                    }
                }
                to[i] = dir;
                give[i] = dir < 0 ? 0.0f : float(std::min(double(s[i]), best * 0.5));
            }
        });
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const size_t i = h.index(x, z);
                float v = s[i] - give[i];
                for (int k = 0; k < 8; ++k)
                {
                    const int nx = x + kDx[k], nz = z + kDz[k];
                    if (nx < 0 || nz < 0 || nx >= h.nx || nz >= h.nz)
                        continue;
                    const size_t j = h.index(nx, nz);
                    if (to[j] == int8_t(k ^ 1))
                        v += give[j];
                }
                next[i] = v;
            }
        });
        std::swap(s, next);
    }
    std::vector<float>& m = h.mask("snow");
    tbb::parallel_for(size_t(0), h.size(), [&](size_t k) {
        m[k] = float(std::clamp(double(s[k]) / depth, 0.0, 1.0));
        if (p.addDepth)
            h.heights[k] += s[k];
    });
}

// ------------------------------------------------------------------------------------------------- presets

namespace landform_
{
struct PresetDef
{
    const char* name;
    double macroAge;
    double fineAge;
    double debris;
    double scourAngle;
    double talus;
    double hardTalus;
    double droplets;
    double supply;
    bool snow;
};

constexpr PresetDef kPresets[] = {
    {"natural", 0.7, 0.10, 1.0, 32.0, 36.0, 55.0, 0.25, 30.0, false},
    {"alpine", 0.85, 0.12, 1.6, 30.0, 38.0, 62.0, 0.2, 25.0, true},
    {"desert", 0.45, 0.06, 0.6, 34.0, 33.0, 70.0, 0.08, 60.0, false},
    {"hills", 0.75, 0.08, 0.5, 36.0, 28.0, 45.0, 0.35, 35.0, false},
};

const PresetDef& presetDef(const std::string& name)
{
    for (const PresetDef& d : kPresets)
        if (name == d.name)
            return d;
    return kPresets[0];
}
} // namespace landform_

const std::vector<std::string>& erodePresetNames()
{
    static const std::vector<std::string> n = [] {
        std::vector<std::string> v;
        for (const landform_::PresetDef& d : landform_::kPresets)
            v.push_back(d.name);
        return v;
    }();
    return n;
}

bool isErodePreset(const std::string& name) { return std::ranges::find(erodePresetNames(), name) != erodePresetNames().end(); }

nlohmann::json PresetReport::toJson() const
{
    nlohmann::json s = nlohmann::json::object();
    double total = 0.0;
    for (const auto& [name, ms] : steps)
    {
        s[name] = ms;
        total += ms;
    }
    return {{"stepsMs", s}, {"totalMs", total}, {"fallbacks", fallbacks}};
}

void erodePreset(Heightfield& h, const PresetParams& p, PresetReport* report)
{
    LILA_ZONE();
    using namespace landform_;

    if (h.empty() || p.age <= 0.0)
        return;
    const PresetDef& def = presetDef(p.preset);
    const double age = std::clamp(p.age, 0.0, 2.0);
    const std::vector<float> start = h.heights;
    Timer timer(report);
    DrainSource drains;
    drains.gpu = p.gpu;

    // Macro form at low resolution, where the analytic solve is cheap, then amplified back keeping its drainage.
    const int longest = std::max(h.nx, h.nz);
    const int factor = longest <= 300 ? 1 : std::clamp(int(std::lround(double(longest - 1) / 256.0)), 2, 8);
    AnalyticParams macro;
    macro.age = def.macroAge * age;
    macro.hardnessMask = p.hardnessMask;
    macro.writeMasks = false;
    if (p.keepForm)
    {
        const std::vector<float> shaped = h.heights;
        AnalyticParams fine = macro;
        fine.age = def.fineAge * age;
        fine.keepRange = false;
        erodeAnalytic(h, fine, &drains);
        const double radius = p.formRadius > 0.0 ? p.formRadius : 30000.0;
        restoreLarge(h, shaped, std::max(1, int(std::lround(radius / h.cell))));
        timer.step("fine");
    }
    else if (factor == 1)
    {
        erodeAnalytic(h, macro);
        timer.step("macro");
    }
    else
    {
        const glm::dvec2 e = h.extent();
        const int cnx = std::max(3, (h.nx - 1) / factor + 1);
        const double cell = e.x / double(cnx - 1);
        const int cnz = std::max(3, int(std::lround(e.y / cell)) + 1);
        Heightfield coarse = lila::geo::resampled(h, cnx, cnz, cell, h.origin);
        erodeAnalytic(coarse, macro);
        timer.step("macro");
        h = amplifiedOnto(coarse, h, def.fineAge * age, p.seed, &drains);
        timer.step("amplify");
    }

    DebrisParams debris;
    debris.strength = def.debris * age;
    debris.scourAngle = def.scourAngle;
    debris.iterations = 2;
    debrisFlow(h, debris, &drains);
    timer.step("debris");

    std::vector<std::pair<std::string, double>> materials;
    if (!p.hardnessMask.empty())
        materials.push_back({p.hardnessMask, def.hardTalus});
    const std::vector<float> angles = talusAngles(h, def.talus, materials);
    const int thermalSteps = 4 + int(std::lround(12.0 * std::min(age, 1.0)));
    bool thermalDone = false;
    if (p.gpu)
    {
        std::string err;
        thermalDone = thermalMaterialGpu(h, angles, thermalSteps, &err);
        if (!thermalDone && report)
            report->fallbacks.push_back("thermal ran on the CPU: " + err);
    }
    if (!thermalDone)
        thermalMaterial(h, angles, thermalSteps);
    timer.step("thermal");

    DropletParams rills;
    rills.density = def.droplets * std::min(age, 1.0);
    rills.seed = p.seed;
    droplets(h, rills);
    timer.step("rills");

    DepositionParams dep;
    dep.supply = def.supply * age;
    dep.hardnessMask = p.hardnessMask;
    dep.iterations = 1;
    deposition(h, dep, &drains);
    timer.step("deposition");

    if (def.snow)
    {
        snow(h, SnowParams());
        timer.step("snow");
    }
    writeFlowMasks(h, drains(h));
    changeMasks(h, start);
    timer.step("masks");
    if (report && !drains.fallback.empty())
        report->fallbacks.push_back(drains.fallback);
}
} // namespace terrain
