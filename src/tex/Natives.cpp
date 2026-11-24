#include "tex/Natives.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "tex/Prelude.h"
#include "tex/SlangRt.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace lila::tex
{
namespace fs = std::filesystem;

namespace
{
/** Helpers every kernel file can call, after the fused-pass prelude. */
constexpr const char* kNativePrelude = R"slang(
int2 nativeSize(uint slot)
{
    float4 d = gU[slot];
    return int2(int(asuint(d.y)), int(asuint(d.z)));
}

int2 nativeTexel() { return int2(gPass.res.xy); }

void nativeStore(int2 p, float4 v) { gOut[uint(p.y) * uint(gPass.res.x) + uint(p.x)] = v; }

/** Texel p of the output read from a buffer of another size, nearest. */
float4 nativeFitted(uint slot, int2 p)
{
    int2 s = nativeSize(slot);
    int2 o = nativeTexel();
    return texFetchBuf(slot, int2(p.x * s.x / o.x, p.y * s.y / o.y));
}
)slang";

int wrapi(int i, int n) { return ((i % n) + n) % n; }

float* px(std::vector<float>& v, int w, int x, int y) { return &v[(size_t(y) * size_t(w) + size_t(x)) * 4]; }
const float* px(const std::vector<float>& v, int w, int x, int y) { return &v[(size_t(y) * size_t(w) + size_t(x)) * 4]; }

/** The input on the output's grid (nearest), as the kernels read it. */
std::vector<float> fitted(const NativeCall& c, const char* id)
{
    const int w = c.output.width, h = c.output.height;
    std::vector<float> out(size_t(w) * size_t(h) * 4, 0.0f);
    auto it = c.inputs.find(id);
    if (it == c.inputs.end())
        return out;
    const NativeBuffer& in = it->second;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int sx = x * in.width / w, sy = y * in.height / h;
            std::memcpy(px(out, w, x, y), &in.rgba[(size_t(sy) * in.width + sx) * 4], sizeof(float) * 4);
        }
    return out;
}

void store(const NativeCall& c, const std::vector<float>& v) { std::memcpy(c.output.rgba, v.data(), v.size() * sizeof(float)); }

void storeGray(const NativeCall& c, int x, int y, float v)
{
    float* t = &c.output.rgba[(size_t(y) * c.output.width + x) * 4];
    t[0] = t[1] = t[2] = v;
    t[3] = 1.0f;
}

int texelRadius(float radius, int size) { return std::max(0, int(std::floor(radius * float(size) + 0.5f))); }

// Flood fill -----------------------------------------------------------------------------------------------------

/**
 * Per texel: its 4-connected region's bounding box relative to itself, regions joined across the wrap. A region that
 * wraps around the texture spans all of it on that axis.
 */
void floodFillCpu(const NativeCall& c)
{
    LILA_ZONE();
    const int w = c.output.width, h = c.output.height;
    std::vector<float> in = fitted(c, "input");
    const size_t n = size_t(w) * size_t(h);
    std::vector<int> comp(n, -1), ux(n, 0), uy(n, 0);
    std::vector<float> out(n * 4);
    for (size_t i = 0; i < n; ++i)
    {
        float* o = &out[i * 4];
        o[0] = o[1] = 0.0f;
        o[2] = o[3] = -1.0f;
    }
    auto inside = [&](size_t i) { return in[i * 4] > 0.5f; };
    std::vector<size_t> members;
    std::deque<size_t> queue;
    for (size_t s = 0; s < n; ++s)
    {
        if (!inside(s) || comp[s] >= 0)
            continue;
        const int id = int(s);
        comp[s] = id;
        ux[s] = int(s % size_t(w));
        uy[s] = int(s / size_t(w));
        int lo[2] = {ux[s], uy[s]}, hi[2] = {ux[s], uy[s]};
        bool wraps[2] = {false, false};
        members.clear();
        queue.push_back(s);
        while (!queue.empty())
        {
            const size_t i = queue.front();
            queue.pop_front();
            members.push_back(i);
            static constexpr int kDir[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& d : kDir)
            {
                const int nx = ux[i] + d[0], ny = uy[i] + d[1];
                const size_t j = size_t(wrapi(ny, h)) * size_t(w) + size_t(wrapi(nx, w));
                if (!inside(j))
                    continue;
                if (comp[j] == id)
                {
                    wraps[0] = wraps[0] || ux[j] != nx;
                    wraps[1] = wraps[1] || uy[j] != ny;
                    continue;
                }
                comp[j] = id;
                ux[j] = nx;
                uy[j] = ny;
                lo[0] = std::min(lo[0], nx), hi[0] = std::max(hi[0], nx);
                lo[1] = std::min(lo[1], ny), hi[1] = std::max(hi[1], ny);
                queue.push_back(j);
            }
        }
        for (size_t i : members)
        {
            const int x = int(i % size_t(w)), y = int(i / size_t(w));
            float* o = &out[i * 4];
            o[0] = wraps[0] ? float(-x) : float(lo[0] - ux[i]);
            o[2] = wraps[0] ? float(w - 1 - x) : float(hi[0] - ux[i]);
            o[1] = wraps[1] ? float(-y) : float(lo[1] - uy[i]);
            o[3] = wraps[1] ? float(h - 1 - y) : float(hi[1] - uy[i]);
        }
    }
    store(c, out);
}

/** Flood fill regions numbered in row order of their keys (helpers/fill.slang), as (index + 1) / count. */
void fillToIndexCpu(const NativeCall& c)
{
    LILA_ZONE();
    const int w = c.output.width, h = c.output.height;
    std::vector<float> in = fitted(c, "input");
    const size_t n = size_t(w) * size_t(h);
    std::vector<int> key(n, -1);
    std::vector<uint8_t> marked(n, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float* e = px(in, w, x, y);
            const float b[4] = {std::round(e[0]), std::round(e[1]), std::round(e[2]), std::round(e[3])};
            if (b[2] < b[0] || b[3] < b[1])
                continue;
            const int k = wrapi(y + int(b[1]), h) * w + wrapi(x + int(b[0]), w);
            key[size_t(y) * w + x] = k;
            marked[size_t(k)] = 1;
        }
    std::vector<int> index(n, 0);
    int count = 0;
    for (size_t i = 0; i < n; ++i)
    {
        index[i] = count;
        count += marked[i];
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int k = key[size_t(y) * w + x];
            if (k < 0)
                storeGray(c, x, y, 0.0f);
            else
                storeGray(c, x, y, (float(index[size_t(k)]) + 1.0f) / float(std::max(count, 1)));
        }
}

// Auto levels and equalize ---------------------------------------------------------------------------------------

/** The input on the output's grid with its luminance in alpha, as the kernels read it. */
std::vector<float> withLuminance(const NativeCall& c)
{
    std::vector<float> v = fitted(c, "input");
    for (size_t i = 0; i < v.size(); i += 4)
        v[i + 3] = v[i] * 0.2126f + v[i + 1] * 0.7152f + v[i + 2] * 0.0722f;
    return v;
}

float stretch(float v, float lo, float hi) { return hi - lo > 1e-6f ? (v - lo) / (hi - lo) : v; }

void autoLevelsCpu(const NativeCall& c)
{
    LILA_ZONE();
    std::vector<float> src = fitted(c, "input");
    std::vector<float> in = withLuminance(c);
    float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < in.size(); i += 4)
        for (int ch = 0; ch < 4; ++ch)
        {
            lo[ch] = std::min(lo[ch], in[i + ch]);
            hi[ch] = std::max(hi[ch], in[i + ch]);
        }
    const bool luminance = int(c.param("mode")) == 1;
    for (size_t i = 0; i < src.size(); i += 4)
    {
        float* o = &c.output.rgba[i];
        for (int ch = 0; ch < 3; ++ch)
            o[ch] = luminance ? std::clamp(stretch(src[i + ch], lo[3], hi[3]), 0.0f, 1.0f) : stretch(src[i + ch], lo[ch], hi[ch]);
        o[3] = src[i + 3];
    }
}

constexpr int kEqualizeBins = 256;

int equalizeBin(float v) { return std::clamp(int(std::floor(v * float(kEqualizeBins))), 0, kEqualizeBins - 1); }

void equalizeCpu(const NativeCall& c)
{
    LILA_ZONE();
    std::vector<float> src = fitted(c, "input");
    std::vector<float> in = withLuminance(c);
    std::vector<std::array<float, 4>> count(kEqualizeBins, {0.0f, 0.0f, 0.0f, 0.0f}), below = count;
    for (size_t i = 0; i < in.size(); i += 4)
        for (int ch = 0; ch < 4; ++ch)
            count[size_t(equalizeBin(in[i + ch]))][size_t(ch)] += 1.0f;
    for (int b = 1; b < kEqualizeBins; ++b)
        for (int ch = 0; ch < 4; ++ch)
            below[size_t(b)][size_t(ch)] = below[size_t(b - 1)][size_t(ch)] + count[size_t(b - 1)][size_t(ch)];
    const float total = float(c.output.width * c.output.height);
    auto map = [&](float v, int ch) {
        const int b = equalizeBin(v);
        const float t = std::clamp(v * float(kEqualizeBins) - float(b), 0.0f, 1.0f);
        return (below[size_t(b)][size_t(ch)] + t * count[size_t(b)][size_t(ch)]) / total;
    };
    const bool luminance = int(c.param("mode")) == 0;
    for (size_t i = 0; i < in.size(); i += 4)
    {
        float* o = &c.output.rgba[i];
        const float l = map(in[i + 3], 3);
        for (int ch = 0; ch < 3; ++ch)
        {
            if (!luminance)
                o[ch] = map(in[i + ch], ch);
            else
                o[ch] = in[i + 3] > 1e-5f ? std::clamp(in[i + ch] * (l / in[i + 3]), 0.0f, 1.0f) : l;
        }
        o[3] = src[i + 3];
    }
}

// Distance -------------------------------------------------------------------------------------------------------

/** Squared distance transform of f along a line of n samples spaced s apart, wrapping (three copies). */
void distance1d(const double* f, int n, double s, double* d)
{
    const int m = 3 * n;
    std::vector<double> g(static_cast<size_t>(m)), z(static_cast<size_t>(m) + 1);
    std::vector<int> v(static_cast<size_t>(m));
    for (int i = 0; i < m; ++i)
        g[size_t(i)] = f[i % n] / (s * s);
    int k = 0;
    v[0] = 0;
    z[0] = -1e300;
    z[1] = 1e300;
    for (int q = 1; q < m; ++q)
    {
        double sp;
        // Terminates at k = 0 at the latest, since z[0] is below any intersection.
        for (;;)
        {
            const int r = v[size_t(k)];
            sp = ((g[size_t(q)] + double(q) * q) - (g[size_t(r)] + double(r) * r)) / (2.0 * q - 2.0 * r);
            if (sp > z[size_t(k)])
                break;
            --k;
        }
        ++k;
        v[size_t(k)] = q;
        z[size_t(k)] = sp;
        z[size_t(k) + 1] = 1e300;
    }
    k = 0;
    for (int q = 0; q < m; ++q)
    {
        while (z[size_t(k) + 1] < q)
            ++k;
        const int r = v[size_t(k)];
        if (q >= n && q < 2 * n)
            d[q - n] = (double(q - r) * (q - r) + g[size_t(r)]) * s * s;
    }
}

/** Squared uv distance from every texel to the nearest seed texel, wrapping. Large where there are no seeds. */
std::vector<double> distanceField(const std::vector<uint8_t>& seed, int w, int h)
{
    const double kFar = 1e20;
    std::vector<double> f(size_t(w) * size_t(h));
    for (size_t i = 0; i < f.size(); ++i)
        f[i] = seed[i] ? 0.0 : kFar;
    tbb::parallel_for(0, w, [&](int x) {
        std::vector<double> col(static_cast<size_t>(h)), d(static_cast<size_t>(h));
        for (int y = 0; y < h; ++y)
            col[size_t(y)] = f[size_t(y) * w + x];
        distance1d(col.data(), h, 1.0 / h, d.data());
        for (int y = 0; y < h; ++y)
            f[size_t(y) * w + x] = std::min(d[size_t(y)], kFar);
    });
    tbb::parallel_for(0, h, [&](int y) {
        std::vector<double> d(static_cast<size_t>(w));
        distance1d(&f[size_t(y) * w], w, 1.0 / w, d.data());
        for (int x = 0; x < w; ++x)
            f[size_t(y) * w + x] = std::min(d[size_t(x)], kFar);
    });
    return f;
}

/** Shared with the kernel: the value a distance node writes from the outside and inside distances (uv). */
float distanceValue(int mode, float maxDistance, bool in, float dOut, float dIn, float halfTexel)
{
    if (mode == 1)
        return dOut;
    if (mode == 2)
        return in ? -(dIn - halfTexel) : dOut - halfTexel;
    return in ? 1.0f : std::clamp(1.0f - dOut / std::max(maxDistance, 1e-6f), 0.0f, 1.0f);
}

void distanceCpu(const NativeCall& c)
{
    LILA_ZONE();
    const int w = c.output.width, h = c.output.height;
    std::vector<float> in = fitted(c, "input");
    std::vector<uint8_t> mask(size_t(w) * size_t(h)), outside(mask.size());
    for (size_t i = 0; i < mask.size(); ++i)
    {
        mask[i] = in[i * 4] > 0.5f;
        outside[i] = !mask[i];
    }
    const int mode = int(c.param("mode"));
    std::vector<double> dOut = distanceField(mask, w, h);
    std::vector<double> dIn = mode == 2 ? distanceField(outside, w, h) : std::vector<double>(mask.size(), 0.0);
    const float halfTexel = 0.5f * std::min(1.0f / float(w), 1.0f / float(h));
    const float maxDistance = c.param("maxDistance");
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * w + x;
            const float o = float(std::min(std::sqrt(dOut[i]), 2.0)), n = float(std::min(std::sqrt(dIn[i]), 2.0));
            storeGray(c, x, y, distanceValue(mode, maxDistance, mask[i] != 0, o, n, halfTexel));
        }
    });
}

// Dilate and erode -----------------------------------------------------------------------------------------------

template <bool Max>
void morphologyCpu(const NativeCall& c)
{
    LILA_ZONE();
    const int w = c.output.width, h = c.output.height;
    const int rx = texelRadius(c.param("radius"), w), ry = texelRadius(c.param("radius"), h);
    std::vector<float> src = fitted(c, "input"), tmp(src.size());
    auto pick = [](float a, float b) { return Max ? std::max(a, b) : std::min(a, b); };
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
        {
            float acc[4];
            std::memcpy(acc, px(src, w, x, y), sizeof(acc));
            for (int k = -rx; k <= rx; ++k)
            {
                const float* s = px(src, w, wrapi(x + k, w), y);
                for (int ch = 0; ch < 4; ++ch)
                    acc[ch] = pick(acc[ch], s[ch]);
            }
            std::memcpy(px(tmp, w, x, y), acc, sizeof(acc));
        }
    });
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
        {
            float acc[4];
            std::memcpy(acc, px(tmp, w, x, y), sizeof(acc));
            for (int k = -ry; k <= ry; ++k)
            {
                const float* s = px(tmp, w, x, wrapi(y + k, h));
                for (int ch = 0; ch < 4; ++ch)
                    acc[ch] = pick(acc[ch], s[ch]);
            }
            std::memcpy(&c.output.rgba[(size_t(y) * w + x) * 4], acc, sizeof(acc));
        }
    });
}

// Ambient occlusion ----------------------------------------------------------------------------------------------

/** Channel 0 at uv, bilinear and wrapping, exactly as texReadBuf. */
float sampleRed(const NativeBuffer& b, float u, float v)
{
    const float fx = u * float(b.width) - 0.5f, fy = (1.0f - v) * float(b.height) - 0.5f;
    const float x0 = std::floor(fx), y0 = std::floor(fy);
    const float tx = fx - x0, ty = fy - y0;
    auto at = [&](int x, int y) { return b.rgba[(size_t(wrapi(y, b.height)) * b.width + size_t(wrapi(x, b.width))) * 4]; };
    const int ix = int(x0), iy = int(y0);
    const float a = at(ix, iy), bb = at(ix + 1, iy), cc = at(ix, iy + 1), e = at(ix + 1, iy + 1);
    const float top = a + (bb - a) * tx, bottom = cc + (e - cc) * tx;
    return top + (bottom - top) * ty;
}

/** Horizon-based: per direction, the steepest rise within the radius; occlusion is the mean sine of those angles. */
void aoCpu(const NativeCall& c)
{
    LILA_ZONE();
    auto it = c.inputs.find("height");
    if (it == c.inputs.end())
        return;
    const NativeBuffer& hb = it->second;
    const int w = c.output.width, h = c.output.height;
    const float radius = std::max(c.param("radius"), 1e-5f), depth = c.param("depth"), strength = c.param("strength");
    const int dirs = std::clamp(int(c.param("directions")), 1, 64), steps = std::clamp(int(c.param("steps")), 1, 64);
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
        {
            const float u = (float(x) + 0.5f) / float(w), v = 1.0f - (float(y) + 0.5f) / float(h);
            const float h0 = sampleRed(hb, u, v);
            float occlusion = 0.0f;
            for (int d = 0; d < dirs; ++d)
            {
                const float a = 6.28318530718f * (float(d) + 0.5f) / float(dirs);
                const float dx = std::cos(a), dy = std::sin(a);
                float steepest = 0.0f;
                for (int s = 1; s <= steps; ++s)
                {
                    const float t = radius * float(s) / float(steps);
                    const float rise = (sampleRed(hb, u + dx * t, v + dy * t) - h0) * depth / t;
                    steepest = std::max(steepest, rise);
                }
                occlusion += steepest / std::sqrt(1.0f + steepest * steepest);
            }
            storeGray(c, x, y, std::clamp(1.0f - strength * occlusion / float(dirs), 0.0f, 1.0f));
        }
    });
}

// Normal to height -----------------------------------------------------------------------------------------------

struct Grid
{
    int w = 0, h = 0;
    std::vector<float> v;
    float at(int x, int y) const { return v[size_t(wrapi(y, h)) * w + size_t(wrapi(x, w))]; }
};

/** Level sizes, finest first: halved while both sides are at least 2. */
std::vector<std::array<int, 2>> pyramid(int w, int h)
{
    std::vector<std::array<int, 2>> out = {{w, h}};
    while (out.back()[0] >= 2 && out.back()[1] >= 2)
        out.push_back({(out.back()[0] + 1) / 2, (out.back()[1] + 1) / 2});
    return out;
}

/** dh per texel along x and along rows (down), from a normal as Height to Normal writes it. */
std::array<float, 2> slopeTexel(const float* n, float intensity, int w, int h)
{
    const float nx = n[0] * 2.0f - 1.0f, ny = n[1] * 2.0f - 1.0f, nz = std::max(n[2] * 2.0f - 1.0f, 0.05f);
    const float k = 1.0f / (0.05f * std::max(intensity, 1e-4f));
    return {-nx / nz * k / float(w), ny / nz * k / float(h)};
}

void jacobi(Grid& hgt, const Grid& rhs, int iterations)
{
    Grid tmp = hgt;
    for (int i = 0; i < iterations; ++i)
    {
        tbb::parallel_for(0, hgt.h, [&](int y) {
            for (int x = 0; x < hgt.w; ++x)
                tmp.v[size_t(y) * hgt.w + x] = (hgt.at(x - 1, y) + hgt.at(x + 1, y) + hgt.at(x, y - 1) + hgt.at(x, y + 1) - rhs.at(x, y)) * 0.25f;
        });
        std::swap(tmp.v, hgt.v);
    }
}

/** Poisson integration of the normal's slopes, coarse to fine with Jacobi relaxation; zero mean, around 0.5. */
void normalToHeightCpu(const NativeCall& c)
{
    LILA_ZONE();
    const int w = c.output.width, h = c.output.height;
    const float intensity = c.param("intensity");
    const int iterations = std::clamp(int(c.param("iterations")), 1, 1000);
    std::vector<float> nrm = fitted(c, "input");
    const auto sizes = pyramid(w, h);
    std::vector<Grid> rhs(sizes.size());
    rhs[0] = {w, h, std::vector<float>(size_t(w) * h)};
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
        {
            const auto r = slopeTexel(px(nrm, w, wrapi(x + 1, w), y), intensity, w, h);
            const auto l = slopeTexel(px(nrm, w, wrapi(x - 1, w), y), intensity, w, h);
            const auto d = slopeTexel(px(nrm, w, x, wrapi(y + 1, h)), intensity, w, h);
            const auto u = slopeTexel(px(nrm, w, x, wrapi(y - 1, h)), intensity, w, h);
            rhs[0].v[size_t(y) * w + x] = 0.5f * (r[0] - l[0]) + 0.5f * (d[1] - u[1]);
        }
    });
    for (size_t k = 1; k < sizes.size(); ++k)
    {
        const Grid& f = rhs[k - 1];
        Grid& g = rhs[k];
        g = {sizes[k][0], sizes[k][1], std::vector<float>(size_t(sizes[k][0]) * sizes[k][1])};
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
                g.v[size_t(y) * g.w + x] = (f.at(2 * x, 2 * y) + f.at(2 * x + 1, 2 * y) + f.at(2 * x, 2 * y + 1) + f.at(2 * x + 1, 2 * y + 1));
    }
    Grid hgt{sizes.back()[0], sizes.back()[1], std::vector<float>(size_t(sizes.back()[0]) * sizes.back()[1], 0.0f)};
    jacobi(hgt, rhs.back(), iterations * 2);
    for (int k = int(sizes.size()) - 2; k >= 0; --k)
    {
        Grid fine{sizes[size_t(k)][0], sizes[size_t(k)][1], std::vector<float>(size_t(sizes[size_t(k)][0]) * sizes[size_t(k)][1])};
        const float sx = float(hgt.w) / float(fine.w), sy = float(hgt.h) / float(fine.h);
        tbb::parallel_for(0, fine.h, [&](int y) {
            for (int x = 0; x < fine.w; ++x)
            {
                const float cx = (float(x) + 0.5f) * sx - 0.5f, cy = (float(y) + 0.5f) * sy - 0.5f;
                const float x0 = std::floor(cx), y0 = std::floor(cy);
                const float tx = cx - x0, ty = cy - y0;
                const int ix = int(x0), iy = int(y0);
                const float top = hgt.at(ix, iy) + (hgt.at(ix + 1, iy) - hgt.at(ix, iy)) * tx;
                const float bottom = hgt.at(ix, iy + 1) + (hgt.at(ix + 1, iy + 1) - hgt.at(ix, iy + 1)) * tx;
                fine.v[size_t(y) * fine.w + x] = top + (bottom - top) * ty;
            }
        });
        hgt = std::move(fine);
        jacobi(hgt, rhs[size_t(k)], iterations);
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            storeGray(c, x, y, 0.5f + hgt.v[size_t(y) * w + x]);
}

// GPU schedules --------------------------------------------------------------------------------------------------

/** Frame slot of buffer index b for a type with nIn inputs and nParamSlots parameter slots. */
int frameSlot(int b, int nIn, int nParamSlots) { return b <= nIn ? b : b + nParamSlots; }

int totalParamSlots(const NodeDef& def)
{
    int n = 0;
    for (const ParamDecl& p : def.params)
        n += paramSlots(p.type);
    return n;
}

NativeStep step(std::string entry, int output, int tx, int ty, float c0 = 0.0f, float c1 = 0.0f, float c2 = 0.0f, float c3 = 0.0f)
{
    NativeStep s;
    s.entry = std::move(entry);
    s.output = output;
    s.threadsX = tx;
    s.threadsY = ty;
    s.constants[0] = c0;
    s.constants[1] = c1;
    s.constants[2] = c2;
    s.constants[3] = c3;
    return s;
}

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const std::map<std::string, std::string>& kernelFiles()
{
    static const std::map<std::string, std::string> m = {{"blur", "blur"},   {"floodFill", "floodFill"}, {"distance", "distance"},
                                                         {"dilate", "morphology"}, {"erode", "morphology"}, {"ambientOcclusion", "ambientOcclusion"},
                                                         {"normalToHeight", "normalToHeight"}, {"fillToIndex", "fillToIndex"},
                                                         {"autoLevels", "autoLevels"}, {"equalize", "equalize"}};
    return m;
}
} // namespace

bool hasNativeKernels(const std::string& type) { return kernelFiles().contains(type); }

NativeSchedule nativeSchedule(const std::string& type, int width, int height, const NativeParamFn& param)
{
    NativeSchedule s;
    auto file = kernelFiles().find(type);
    const NodeDef* def = findNode(type);
    if (file == kernelFiles().end() || !def)
        return s;
    s.file = file->second;
    const int w = std::max(width, 1), h = std::max(height, 1);
    const int nIn = int(def->inputs.size()), out = nIn, firstScratch = nIn + 1;
    const int nParams = totalParamSlots(*def);
    auto slot = [&](int b) { return float(frameSlot(b, nIn, nParams)); };
    if (type == "blur")
    {
        s.scratch = {{w, h}};
        s.steps = {step("blurRows", firstScratch, w, h), step("blurColumns", out, w, h, slot(firstScratch))};
    }
    else if (type == "dilate" || type == "erode")
    {
        const std::string op = type == "dilate" ? "dilate" : "erode";
        s.scratch = {{w, h}};
        s.steps = {step(op + "Rows", firstScratch, w, h), step(op + "Columns", out, w, h, slot(firstScratch))};
    }
    else if (type == "floodFill")
    {
        const int a = firstScratch, b = firstScratch + 1;
        s.scratch = {{w, h}, {w, h}};
        s.steps.push_back(step("fillInit", a, w, h));
        const int passes = std::clamp(int(param("passes", 0)), 1, 64);
        for (int i = 0; i < passes; ++i)
        {
            s.steps.push_back(step("fillRows", b, 1, h, slot(a)));
            s.steps.push_back(step("fillColumns", i + 1 == passes ? out : a, w, 1, slot(b)));
        }
    }
    else if (type == "distance")
    {
        const int a = firstScratch, b = firstScratch + 1;
        s.scratch = {{w, h}, {w, h}};
        s.steps.push_back(step("jumpInit", a, w, h));
        int k = 1;
        while (k * 2 < std::max(w, h))
            k *= 2;
        int src = a;
        std::vector<int> lengths;
        for (; k >= 1; k /= 2)
            lengths.push_back(k);
        lengths.push_back(1);
        for (int len : lengths)
        {
            const int dst = src == a ? b : a;
            s.steps.push_back(step("jumpStep", dst, w, h, slot(src), float(len)));
            src = dst;
        }
        s.steps.push_back(step("jumpResolve", out, w, h, slot(src)));
    }
    else if (type == "fillToIndex")
    {
        const int marks = firstScratch, rows = firstScratch + 1, offsets = firstScratch + 2;
        s.scratch = {{w, h}, {w + 1, h}, {1, h + 1}};
        s.steps = {step("indexClear", marks, w, h), step("indexMark", marks, w, h), step("indexRows", rows, 1, h, slot(marks)),
                   step("indexOffsets", offsets, 1, 1, slot(rows)), step("indexOut", out, w, h, slot(rows), slot(offsets))};
    }
    else if (type == "autoLevels")
    {
        const int rows = firstScratch, all = firstScratch + 1;
        s.scratch = {{2, h}, {2, 1}};
        s.steps = {step("rangeRows", rows, 1, h), step("rangeAll", all, 1, 1, slot(rows)), step("levelsOut", out, w, h, slot(all))};
    }
    else if (type == "equalize")
    {
        const int rows = firstScratch, hist = firstScratch + 1, below = firstScratch + 2;
        s.scratch = {{256, h}, {256, 1}, {256, 1}};
        s.steps = {step("histogramRows", rows, 1, h), step("histogramSum", hist, 256, 1, slot(rows)),
                   step("histogramBelow", below, 1, 1, slot(hist)), step("equalizeOut", out, w, h, slot(hist), slot(below))};
    }
    else if (type == "ambientOcclusion")
        s.steps = {step("occlusion", out, w, h)};
    else if (type == "normalToHeight")
    {
        const auto sizes = pyramid(w, h);
        const int levels = int(sizes.size());
        const int iterations = std::clamp(int(param("iterations", 0)), 1, 1000);
        // Per level: rhs, then two heights to ping-pong.
        for (const auto& sz : sizes)
            for (int k = 0; k < 3; ++k)
                s.scratch.push_back(sz);
        auto rhs = [&](int l) { return firstScratch + 3 * l; };
        auto ha = [&](int l) { return firstScratch + 3 * l + 1; };
        auto hb = [&](int l) { return firstScratch + 3 * l + 2; };
        s.steps.push_back(step("divergence", rhs(0), w, h));
        for (int l = 1; l < levels; ++l)
            s.steps.push_back(step("coarsen", rhs(l), sizes[size_t(l)][0], sizes[size_t(l)][1], slot(rhs(l - 1))));
        auto relax = [&](int l, int count) {
            int cur = ha(l);
            for (int i = 0; i < count; ++i)
            {
                const int dst = cur == ha(l) ? hb(l) : ha(l);
                s.steps.push_back(step("jacobi", dst, sizes[size_t(l)][0], sizes[size_t(l)][1], slot(cur), slot(rhs(l))));
                cur = dst;
            }
            return cur;
        };
        const int last = levels - 1;
        s.steps.push_back(step("zero", ha(last), sizes[size_t(last)][0], sizes[size_t(last)][1]));
        int result = relax(last, iterations * 2);
        for (int l = last - 1; l >= 0; --l)
        {
            s.steps.push_back(step("upsample", ha(l), sizes[size_t(l)][0], sizes[size_t(l)][1], slot(result)));
            result = relax(l, iterations);
        }
        s.steps.push_back(step("heightOut", out, w, h, slot(result)));
    }
    return s;
}

std::string nativeKernelSource(const std::string& file, const std::string& entry, std::string* err)
{
    static std::mutex mutex;
    static std::map<std::string, std::string> cache;
    std::string body;
    {
        std::lock_guard lock(mutex);
        auto it = cache.find(file);
        if (it == cache.end())
            it = cache.emplace(file, readFile(paths::resourcesDir() / "texnodes" / "natives" / (file + ".slang"))).first;
        body = it->second;
    }
    if (body.empty())
    {
        if (err)
            *err = "no kernel file natives/" + file + ".slang";
        return {};
    }
    return std::string("// Lila native kernel ") + file + "." + entry + "\n" + kPrelude + kNativePrelude + "\n" + body +
           "\n[shader(\"compute\")]\n[numthreads(8, 8, 1)]\nvoid texMain(uint3 id : SV_DispatchThreadID)\n{\n    " + entry + "(id);\n}\n";
}

std::vector<float> nativeFrame(const NodeDef& def, const Pass& pass, std::span<const float> uniforms, const std::vector<std::array<uint32_t, 3>>& buffers)
{
    const int nIn = int(def.inputs.size());
    const int nParams = totalParamSlots(def);
    std::vector<float> frame((buffers.size() + size_t(nParams)) * 4, 0.0f);
    for (size_t b = 0; b < buffers.size(); ++b)
    {
        const uint32_t d[4] = {buffers[b][0], buffers[b][1], buffers[b][2], 0u};
        std::memcpy(&frame[size_t(frameSlot(int(b), nIn, nParams)) * 4], d, sizeof(d));
    }
    int at = nIn + 1;
    for (const ParamDecl& p : def.params)
    {
        const int n = paramSlots(p.type);
        auto it = pass.paramSlots.find(p.id);
        if (it != pass.paramSlots.end() && size_t(it->second + n) * 4 <= uniforms.size())
            std::memcpy(&frame[size_t(at) * 4], &uniforms[size_t(it->second) * 4], size_t(n) * 4 * sizeof(float));
        at += n;
    }
    return frame;
}

void runNativeKernelsCpu(const NativeCall& call)
{
    LILA_ZONE();
    const NodeDef* def = findNode(call.pass.nativeType);
    if (!def)
        throw std::runtime_error("no node type " + call.pass.nativeType);
    const int w = call.output.width, h = call.output.height;
    NativeSchedule s = nativeSchedule(call.pass.nativeType, w, h, [&](const std::string& id, int ch) { return call.param(id, ch); });
    if (s.steps.empty())
        throw std::runtime_error("no GPU kernels for native node " + call.pass.nativeType);

    // Frame buffers in one arena: inputs, output, scratch.
    std::vector<std::array<int, 2>> sizes;
    std::vector<const NativeBuffer*> sources;
    for (const PortDecl& in : def->inputs)
    {
        auto it = call.inputs.find(in.id);
        sources.push_back(it == call.inputs.end() ? nullptr : &it->second);
        sizes.push_back(it == call.inputs.end() ? std::array<int, 2>{1, 1} : std::array<int, 2>{it->second.width, it->second.height});
    }
    sources.push_back(nullptr);
    sizes.push_back({w, h});
    for (const auto& sc : s.scratch)
    {
        sources.push_back(nullptr);
        sizes.push_back(sc);
    }
    std::vector<std::array<uint32_t, 3>> descs;
    size_t total = 0;
    for (const auto& sz : sizes)
    {
        descs.push_back({uint32_t(total), uint32_t(sz[0]), uint32_t(sz[1])});
        total += size_t(sz[0]) * size_t(sz[1]);
    }
    std::vector<float> arena(total * 4, 0.0f);
    for (size_t b = 0; b < sources.size(); ++b)
        if (sources[b])
            std::memcpy(&arena[size_t(descs[b][0]) * 4], sources[b]->rgba, size_t(descs[b][1]) * descs[b][2] * 4 * sizeof(float));
    std::vector<float> frame = nativeFrame(*def, call.pass, call.uniforms, descs);

    for (const NativeStep& st : s.steps)
    {
        std::string err;
        const std::string source = nativeKernelSource(s.file, st.entry, &err);
        if (source.empty())
            throw std::runtime_error(err);
        auto kernel = slangrt::cpuKernel(source, hash64(source), &err);
        if (!kernel)
            throw std::runtime_error("native kernel " + s.file + "." + st.entry + " failed to compile:\n" + err);
        const auto& od = descs.at(size_t(st.output));
        const int ow = int(od[1]), oh = int(od[2]);
        slangrt::PassHeader header{{float(ow), float(oh), 1.0f / float(ow), 1.0f / float(oh)}, {st.constants[0], st.constants[1], st.constants[2], st.constants[3]}};
        const slangrt::PassHeader* headerPtr = &header;
        slangrt::BufferView u{frame.data(), frame.size() / 4};
        slangrt::BufferView a{arena.data(), arena.size() / 4};
        slangrt::BufferView o{&arena[size_t(od[0]) * 4], size_t(ow) * size_t(oh)};
        std::vector<uint8_t> globals(kernel->globalsSize, 0);
        std::memcpy(&globals[kernel->offPass], &headerPtr, sizeof(headerPtr));
        std::memcpy(&globals[kernel->offU], &u, sizeof(u));
        if (kernel->offArena != SIZE_MAX)
            std::memcpy(&globals[kernel->offArena], &a, sizeof(a));
        std::memcpy(&globals[kernel->offOut], &o, sizeof(o));
        const uint32_t groupsX = uint32_t((st.threadsX + 7) / 8), groupsY = uint32_t((st.threadsY + 7) / 8);
        tbb::parallel_for(uint32_t(0), groupsY, [&](uint32_t gy) {
            slangrt::Varying v{{0, gy, 0}, {groupsX, gy + 1, 1}};
            std::vector<uint8_t> local = globals;
            kernel->fn(&v, nullptr, local.data());
        });
    }
    std::memcpy(call.output.rgba, &arena[size_t(descs[size_t(def->inputs.size())][0]) * 4], size_t(w) * size_t(h) * 4 * sizeof(float));
}

void addBuiltinNatives(std::map<std::string, NativeCpuFn>& table)
{
    table["floodFill"] = floodFillCpu;
    table["distance"] = distanceCpu;
    table["dilate"] = morphologyCpu<true>;
    table["erode"] = morphologyCpu<false>;
    table["ambientOcclusion"] = aoCpu;
    table["normalToHeight"] = normalToHeightCpu;
    table["fillToIndex"] = fillToIndexCpu;
    table["autoLevels"] = autoLevelsCpu;
    table["equalize"] = equalizeCpu;
}
} // namespace lila::tex
