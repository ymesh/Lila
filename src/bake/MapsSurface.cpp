#include "bake/MapMath.h"
#include "bake/MapsInternal.h"

#include "bake/Backend.h"
#include "bake/BakeCache.h"
#include "bake/BakeInputs.h"
#include "core/Profile.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <list>
#include <mutex>
#include <span>

/** Curvature and its relatives (convexity, cavity, edge wear), thickness, bent normals and rounded-edge normals. */
namespace lila::bake
{
using namespace mapmath;

namespace
{
/**
 * Signed mean curvature per soup corner (1/cm, convex positive): the cotangent Laplacian on the soup welded by exact
 * position, so split normals and UV seams don't open the surface. Boundary points take their interior neighbours' mean.
 */
std::vector<float> meshCurvature(const MeshSoup& soup)
{
    LILA_ZONE();

    const size_t corners = soup.positions.size();
    // Weld by exact position: corners sorted by their position bits (ties by index), so each point's corners sit
    // together in triangle order and every sum below runs in the order a serial pass over the triangles would.
    struct Key
    {
        std::array<uint32_t, 3> bits;
        uint32_t corner;
        bool operator<(const Key& o) const { return bits != o.bits ? bits < o.bits : corner < o.corner; }
    };
    std::vector<Key> keys(corners);
    tbb::parallel_for(size_t(0), corners, [&](size_t c) {
        std::memcpy(keys[c].bits.data(), &soup.positions[c], 12);
        keys[c].corner = uint32_t(c);
    });
    tbb::parallel_sort(keys.begin(), keys.end());
    std::vector<uint32_t> order(corners), runStart, weld(corners);
    for (size_t i = 0; i < corners; ++i)
    {
        if (i == 0 || keys[i].bits != keys[i - 1].bits)
            runStart.push_back(uint32_t(i));
        order[i] = keys[i].corner;
        weld[keys[i].corner] = uint32_t(runStart.size() - 1);
    }
    keys = {};
    const size_t n = runStart.size();
    runStart.push_back(uint32_t(corners));
    auto cornersOf = [&](size_t v) { return std::span<const uint32_t>(order.data() + runStart[v], runStart[v + 1] - runStart[v]); };

    std::vector<float> h(n, 0.0f);
    std::vector<uint8_t> boundary(n, 0);
    // Edges used by one triangle only (open boundaries), from the sorted welded edge keys.
    {
        constexpr uint64_t kSkip = ~uint64_t(0);
        std::vector<uint64_t> edges(corners - corners % 3, kSkip);
        tbb::parallel_for(size_t(0), corners / 3, [&](size_t tri) {
            const size_t t = tri * 3;
            const uint32_t i[3] = {weld[t], weld[t + 1], weld[t + 2]};
            if (i[0] == i[1] || i[1] == i[2] || i[0] == i[2])
                return;
            const glm::dvec3 p0 = soup.positions[t], p1 = soup.positions[t + 1], p2 = soup.positions[t + 2];
            if (!(glm::length(glm::cross(p1 - p0, p2 - p0)) > 0.0))
                return;
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t a = i[(k + 1) % 3], b = i[(k + 2) % 3];
                edges[t + size_t(k)] = a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a;
            }
        });
        tbb::parallel_sort(edges.begin(), edges.end());
        while (!edges.empty() && edges.back() == kSkip)
            edges.pop_back();
        for (size_t e = 0; e < edges.size();)
        {
            size_t f = e + 1;
            while (f < edges.size() && edges[f] == edges[e])
                ++f;
            if (f - e == 1)
                boundary[size_t(edges[e] >> 32)] = boundary[size_t(edges[e] & 0xffffffffu)] = 1;
            e = f;
        }
    }
    tbb::parallel_for(size_t(0), n, [&](size_t v) {
        glm::dvec3 lap(0.0), nrm(0.0);
        double area = 0.0;
        for (uint32_t c : cornersOf(v))
        {
            const size_t t = c - c % 3;
            const int j = int(c % 3);
            const uint32_t i[3] = {weld[t], weld[t + 1], weld[t + 2]};
            if (i[0] == i[1] || i[1] == i[2] || i[0] == i[2])
                continue;
            const glm::dvec3 p[3] = {soup.positions[t], soup.positions[t + 1], soup.positions[t + 2]};
            const glm::dvec3 cr = glm::cross(p[1] - p[0], p[2] - p[0]);
            const double a2 = glm::length(cr);
            if (a2 <= 0.0)
                continue;
            for (int k = 0; k < 3; ++k)
            {
                const int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
                // Cotangent of the angle at k, which weighs the opposite edge k1-k2.
                const double cot = glm::dot(p[k1] - p[k], p[k2] - p[k]) / a2;
                if (k1 == j)
                    lap += 0.5 * cot * (p[k2] - p[k1]);
                if (k2 == j)
                    lap += 0.5 * cot * (p[k1] - p[k2]);
                if (k == j)
                {
                    area += a2 / 6.0;
                    nrm += cr;
                }
            }
        }
        const double l = glm::length(nrm);
        if (area > 0.0 && l > 0.0 && !boundary[v])
            h[v] = float(-0.5 * glm::dot(lap / area, nrm / l));
    });
    // Boundary points take their interior neighbours' mean.
    std::vector<float> smoothed = h;
    tbb::parallel_for(size_t(0), n, [&](size_t v) {
        if (!boundary[v])
            return;
        double sum = 0.0;
        uint32_t cnt = 0;
        for (uint32_t c : cornersOf(v))
        {
            const size_t t = c - c % 3;
            const int j = int(c % 3);
            // The order a pass over the triangle's edges meets them, so the sum matches it to the bit.
            const uint32_t first = weld[t + size_t((j + 1) % 3)], second = weld[t + size_t((j + 2) % 3)];
            for (const uint32_t nb : j == 0 ? std::array<uint32_t, 2>{first, second} : std::array<uint32_t, 2>{second, first})
                if (!boundary[nb])
                {
                    sum += h[nb];
                    ++cnt;
                }
        }
        if (cnt)
            smoothed[v] = float(sum / cnt);
    });
    std::vector<float> out(corners);
    tbb::parallel_for(size_t(0), corners, [&](size_t c) { out[c] = smoothed[weld[c]]; });
    return out;
}

enum class CurvatureMode
{
    Curvature,
    Convexity,
    Cavity,
    EdgeWear,
};

struct CurvaturePrep
{
    bool mesh = false;
    std::shared_ptr<const void> normalMaps;
    /** Per group, per high: per corner. */
    std::vector<std::vector<std::shared_ptr<const std::vector<float>>>> corner;
};

std::string sourceOf(const MapSettings& s, const char* fallback)
{
    const std::string v = s.options.value("source", std::string(fallback));
    return v == "normals" || v == "normal" ? "normals" : "mesh";
}

Prepare curvaturePrepare(const char* defaultSource)
{
    return [defaultSource](const MapSettings& settings, const BakeInputs& inputs) -> std::shared_ptr<const void> {
        auto prep = std::make_shared<CurvaturePrep>();
        prep->mesh = sourceOf(settings, defaultSource) == "mesh";
        if (!prep->mesh)
        {
            prep->normalMaps = prepareHighNormalMaps(settings, inputs);
            return prep;
        }
        std::vector<std::pair<size_t, size_t>> jobs;
        prep->corner.resize(inputs.groups.size());
        for (size_t g = 0; g < inputs.groups.size(); ++g)
        {
            prep->corner[g].resize(inputs.groups[g].highs.size());
            for (size_t h = 0; h < inputs.groups[g].highs.size(); ++h)
                jobs.emplace_back(g, h);
        }
        tbb::parallel_for(size_t(0), jobs.size(), [&](size_t i) {
            const auto [g, h] = jobs[i];
            prep->corner[g][h] = highCurvature(inputs.groups[g].highs[h].soup);
        });
        return prep;
    };
}

void curvatureKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                     std::span<uint8_t> valid)
{
    const auto* prep = static_cast<const CurvaturePrep*>(ctx.prepared);
    if (!prep || !prep->mesh)
    {
        slopeKernel(ctx, prep ? prep->normalMaps.get() : nullptr, samples, hits, out, valid);
        return;
    }
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const HitRecord& h = hits[i];
        valid[i] = (h.flags & kHitValid) ? 1 : 0;
        if (!valid[i])
            continue;
        const std::vector<float>& c = *prep->corner[ctx.groupIndex][h.high];
        const float k = c.empty() ? 0.0f : atHit(c, h);
        out[i] = glm::vec4(k, k, k, 1.0f);
    }
}

Finalize curvatureFinalize(CurvatureMode mode, const char* defaultSource)
{
    return [mode, defaultSource](const MapSettings& settings, const BakeInputs&, TextureBuffer& tex, json& meta) {
        const bool mesh = sourceOf(settings, defaultSource) == "mesh";
        std::vector<float> h;
        if (mesh)
        {
            h.resize(tex.texelCount());
            for (size_t i = 0; i < h.size(); ++i)
                h[i] = tex.value[i].r;
        }
        else
            h = curvatureFromSlopes(tex, settings.options.value("radius", 1));
        double range = settings.options.value("range", 0.0);
        const bool automatic = !(range > 0.0);
        if (automatic)
        {
            range = percentileAbs(h, tex.covered, 0.99f);
            if (!(range > 0.0))
                range = 1.0;
        }
        const float threshold = settings.options.value("threshold", 0.1f), softness = settings.options.value("softness", 0.4f);
        const float noise = std::clamp(settings.options.value("noise", 0.0f), 0.0f, 1.0f);
        const float noiseScale = std::max(1.0f, settings.options.value("noiseScale", 8.0f));
        for (size_t i = 0; i < tex.value.size(); ++i)
        {
            const float k = tex.covered[i] ? std::clamp(float(double(h[i]) / range), -1.0f, 1.0f) : 0.0f;
            float v = 0.0f;
            switch (mode)
            {
            case CurvatureMode::Curvature: v = 0.5f + 0.5f * k; break;
            case CurvatureMode::Convexity: v = std::max(k, 0.0f); break;
            case CurvatureMode::Cavity: v = 1.0f - std::max(-k, 0.0f); break;
            case CurvatureMode::EdgeWear:
            {
                v = smoothstep(threshold, threshold + softness, k);
                if (noise > 0.0f && v > 0.0f)
                {
                    // Bilinear value noise on the texel grid, deterministic.
                    const int x = int(i % size_t(tex.width)), y = int(i / size_t(tex.width));
                    const float fx = float(x) / noiseScale, fy = float(y) / noiseScale;
                    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
                    auto rnd = [](int a, int b) { return float(hashBits(uint32_t(a) * 73856093u ^ uint32_t(b) * 19349663u) & 0xffff) / 65535.0f; };
                    const float ax = smoothstep(0.0f, 1.0f, fx - float(x0)), ay = smoothstep(0.0f, 1.0f, fy - float(y0));
                    const float nv = glm::mix(glm::mix(rnd(x0, y0), rnd(x0 + 1, y0), ax), glm::mix(rnd(x0, y0 + 1), rnd(x0 + 1, y0 + 1), ax), ay);
                    v *= 1.0f - noise * nv;
                }
                break;
            }
            }
            tex.value[i] = glm::vec4(v, v, v, 1.0f);
        }
        meta["curvatureRange"] = range;
        meta["curvatureAuto"] = automatic;
        meta["source"] = mesh ? "mesh" : "normals";
        if (mode == CurvatureMode::Curvature)
            meta["decode"] = "curvature = (value - 0.5) * 2 * curvatureRange (1/cm mean curvature, convex positive)";
    };
}

void thicknessKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                     std::span<uint8_t> valid)
{
    LILA_ZONE();

    const uint32_t rays = uint32_t(std::clamp(ctx.settings.options.value("rays", 64), 1, 4096));
    const float half = glm::radians(std::clamp(ctx.settings.options.value("spread", 180.0f), 0.0f, 180.0f)) * 0.5f;
    const float diag = glm::length(ctx.group.highMax - ctx.group.highMin);
    const double distOpt = ctx.settings.options.value("distance", 0.0);
    const float dist = distOpt > 0.0 ? float(distOpt) : std::max(diag, 1e-3f);
    const OccluderScope scope = occluderScope(ctx.settings.options.value("scope", ctx.group.settings.aoScope));
    const float eps = rayEpsilon(diag);
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const HitRecord& h = hits[i];
        valid[i] = (h.flags & kHitValid) ? 1 : 0;
        if (!valid[i])
            continue;
        if (!ctx.tracer)
        {
            out[i] = glm::vec4(dist, dist, dist, 1.0f);
            continue;
        }
        const glm::vec3 inward = -v3(h.normal);
        const glm::vec3 o = v3(h.position) + inward * eps;
        const uint32_t seed = sampleSeed(samples[i]);
        double sum = 0.0;
        for (uint32_t k = 0; k < rays; ++k)
        {
            const glm::vec3 d = coneDirection(inward, rayPoint(k, rays, seed), half);
            TraceHit th;
            sum += ctx.tracer->closestHit(ctx.groupIndex, scope, h.high, samples[i].low, RayKind::Occlusion, o, d, dist, th) ? double(th.distance + eps) : double(dist);
        }
        const float t = float(sum / double(rays));
        out[i] = glm::vec4(t, t, t, 1.0f);
    }
}

/** Raw distances to 0-1: by the distance option, or auto (the thickest texel is 1). */
void thicknessFinalize(const MapSettings& settings, const BakeInputs&, TextureBuffer& tex, json& meta)
{
    double range = settings.options.value("distance", 0.0);
    const bool automatic = !(range > 0.0);
    if (automatic)
    {
        range = 0.0;
        for (size_t i = 0; i < tex.value.size(); ++i)
            if (tex.covered[i])
                range = std::max(range, double(tex.value[i].r));
        if (!(range > 0.0))
            range = 1.0;
    }
    const bool invert = settings.options.value("invert", false);
    for (size_t i = 0; i < tex.value.size(); ++i)
    {
        if (!tex.covered[i])
            continue;
        float v = float(std::clamp(double(tex.value[i].r) / range, 0.0, 1.0));
        if (invert)
            v = 1.0f - v;
        tex.value[i] = glm::vec4(v, v, v, 1.0f);
    }
    meta["thicknessRange"] = range;
    meta["thicknessAuto"] = automatic;
    meta["decode"] = invert ? "thickness = (1 - value) * thicknessRange (cm)" : "thickness = value * thicknessRange (cm)";
}

/** n in the sample's frame as the normal map stores it, or n * 0.5 + 0.5 in object space. */
glm::vec4 encodeNormalOut(const MapSettings& settings, const TexelSample& s, const glm::vec3& n)
{
    if (settings.options.value("space", std::string("tangent")) == "object")
        return glm::vec4(n * 0.5f + 0.5f, 1.0f);
    const glm::vec3 e = encodeTangentNormal(n, v3(s.normal), glm::vec4(s.tangent[0], s.tangent[1], s.tangent[2], s.tangent[3]),
                                            settings.options.value("greenUp", false), settings.options.value("flipX", false));
    return glm::vec4(e * 0.5f + 0.5f, 1.0f);
}

void normalFinalize(const MapSettings&, const BakeInputs&, TextureBuffer& tex, json&)
{
    for (size_t i = 0; i < tex.value.size(); ++i)
        if (tex.covered[i])
            tex.value[i] = glm::vec4(unit(glm::vec3(tex.value[i]) * 2.0f - 1.0f) * 0.5f + 0.5f, 1.0f);
}

void bentNormalKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                      std::span<uint8_t> valid)
{
    LILA_ZONE();

    const uint32_t rays = uint32_t(std::clamp(ctx.settings.options.value("rays", 64), 1, 4096));
    const float diag = glm::length(ctx.group.highMax - ctx.group.highMin);
    const double distOpt = ctx.settings.options.value("distance", 0.0);
    const float dist = distOpt > 0.0 ? float(distOpt) : std::max(diag, 1e-3f);
    const OccluderScope scope = occluderScope(ctx.settings.options.value("scope", ctx.group.settings.aoScope));
    const float eps = rayEpsilon(diag);
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const HitRecord& h = hits[i];
        const TexelSample& s = samples[i];
        valid[i] = 1;
        glm::vec3 n = (h.flags & kHitValid) ? v3(h.normal) : unit(v3(s.normal));
        if ((h.flags & kHitValid) && ctx.tracer)
        {
            const glm::vec3 o = v3(h.position) + n * eps;
            const uint32_t seed = sampleSeed(s);
            glm::vec3 sum(0.0f);
            for (uint32_t k = 0; k < rays; ++k)
            {
                const glm::vec3 d = coneDirection(n, rayPoint(k, rays, seed), 1.57079632679f);
                if (!ctx.tracer->occluded(ctx.groupIndex, scope, h.high, s.low, o, d, dist))
                    sum += d;
            }
            n = unit(sum, n);
        }
        out[i] = encodeNormalOut(ctx.settings, s, n);
    }
}

/**
 * A bevel normal: the high's normals averaged over the surface within radius of the hit, found by rays through a disc
 * about the hit along the normal and both tangents, so hard edges read as rounded without new geometry.
 */
void roundedNormalKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                         std::span<uint8_t> valid)
{
    LILA_ZONE();

    const uint32_t rays = uint32_t(std::clamp(ctx.settings.options.value("rays", 32), 1, 4096));
    const float diag = glm::length(ctx.group.highMax - ctx.group.highMin);
    const double rOpt = ctx.settings.options.value("radius", 0.0);
    const float radius = rOpt > 0.0 ? float(rOpt) : std::max(0.01f * diag, 1e-3f);
    const OccluderScope scope = occluderScope(ctx.settings.options.value("scope", std::string("group")));
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const HitRecord& h = hits[i];
        const TexelSample& s = samples[i];
        valid[i] = 1;
        glm::vec3 n = (h.flags & kHitValid) ? v3(h.normal) : unit(v3(s.normal));
        if ((h.flags & kHitValid) && ctx.tracer)
        {
            const glm::vec3 p = v3(h.position);
            glm::vec3 t, b;
            basis(n, t, b);
            const uint32_t seed = sampleSeed(s);
            glm::vec3 sum = n;
            for (uint32_t k = 0; k < rays; ++k)
            {
                // Half the rays run along the normal, a quarter along each tangent, each class with its own sequence.
                const uint32_t pick = k % 4;
                const glm::vec3 axis = pick < 2 ? n : pick == 2 ? t : b;
                glm::vec3 a1, a2;
                basis(axis, a1, a2);
                const glm::vec2 u = rayPoint(k / 4, (rays + 3) / 4, seed ^ hashBits(pick + 1));
                const float r = radius * std::sqrt(u.x), phi = 6.28318530718f * u.y;
                const glm::vec3 disc = a1 * (r * std::cos(phi)) + a2 * (r * std::sin(phi));
                for (const float side : {1.0f, -1.0f})
                {
                    const glm::vec3 dir = -axis * side;
                    const glm::vec3 o = p + disc + axis * (side * radius);
                    TraceHit th;
                    if (!ctx.tracer->closestHit(ctx.groupIndex, scope, h.high, s.low, RayKind::Occlusion, o, dir, 2.0f * radius, th) || th.backface)
                        continue;
                    if (glm::length(o + dir * th.distance - p) > radius || glm::length(th.normal) < 0.5f)
                        continue;
                    // Weighted by the inverse of how likely the three projections were to land there, so every bit of
                    // surface in the sphere counts by its area whichever way it faces.
                    const float pdf = 0.5f * std::abs(glm::dot(th.normal, n)) + 0.25f * std::abs(glm::dot(th.normal, t)) +
                                      0.25f * std::abs(glm::dot(th.normal, b));
                    sum += th.normal / std::max(pdf, 0.05f);
                }
            }
            n = unit(sum, n);
        }
        out[i] = encodeNormalOut(ctx.settings, s, n);
    }
}
} // namespace

std::shared_ptr<const std::vector<float>> highCurvature(const MeshSoup& soup)
{
    static std::mutex mutex;
    static std::list<std::pair<uint64_t, std::shared_ptr<const std::vector<float>>>> cache;
    const uint64_t key = hashVector(soup.positions, soup.positions.size());
    {
        std::scoped_lock lock(mutex);
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == key)
            {
                cache.splice(cache.begin(), cache, it);
                return cache.front().second;
            }
    }
    auto c = std::make_shared<const std::vector<float>>(meshCurvature(soup));
    std::scoped_lock lock(mutex);
    cache.emplace_front(key, c);
    if (cache.size() > 8)
        cache.pop_back();
    return c;
}

void registerSurfaceMaps()
{
    const char* curvatureOptions = "options source (mesh: the high's cotangent mean curvature; normals: derived from the baked normals, "
                                   "no new rays), range (1/cm at full strength, 0 = auto: the 99th percentile), radius (texels, normals "
                                   "source), highNormalMap (off).";
    registerMap({.id = "curvature",
                 .label = "Curvature",
                 .suffix = "curvature",
                 .doc = std::string("Signed mean curvature, 0.5 on flat surfaces, brighter on convex edges and darker in concave ones "
                                    "(Painter's convention); meta curvatureRange decodes it. ") +
                        curvatureOptions,
                 .background = {0.5f, 0.5f, 0.5f, 1.0f},
                 .cpu = curvatureKernel,
                 .finalize = curvatureFinalize(CurvatureMode::Curvature, "mesh"),
                 .prepare = curvaturePrepare("mesh")});
    registerMap({.id = "normalToCurvature",
                 .label = "Normal to Curvature",
                 .suffix = "ncurvature",
                 .doc = "Curvature derived from the baked tangent normals (detail on the high relative to the low), no new rays. "
                        "options range, radius (texels), highNormalMap.",
                 .background = {0.5f, 0.5f, 0.5f, 1.0f},
                 .cpu = curvatureKernel,
                 .finalize = curvatureFinalize(CurvatureMode::Curvature, "normals"),
                 .prepare = [](const MapSettings& s, const BakeInputs& in) {
                     MapSettings forced = s;
                     forced.options["source"] = "normals";
                     return curvaturePrepare("normals")(forced, in);
                 }});
    registerMap({.id = "convexity",
                 .label = "Convexity",
                 .suffix = "convexity",
                 .doc = std::string("Convex curvature only: black on flat and concave areas, white on convex edges. ") + curvatureOptions,
                 .background = {0.0f, 0.0f, 0.0f, 1.0f},
                 .cpu = curvatureKernel,
                 .finalize = curvatureFinalize(CurvatureMode::Convexity, "mesh"),
                 .prepare = curvaturePrepare("mesh")});
    registerMap({.id = "cavity",
                 .label = "Cavity",
                 .suffix = "cavity",
                 .doc = std::string("Concave curvature only: white on flat and convex areas, darker in crevices. ") + curvatureOptions,
                 .background = {1.0f, 1.0f, 1.0f, 1.0f},
                 .cpu = curvatureKernel,
                 .finalize = curvatureFinalize(CurvatureMode::Cavity, "mesh"),
                 .prepare = curvaturePrepare("mesh")});
    registerMap({.id = "edgeWear",
                 .label = "Edge Wear",
                 .suffix = "edgewear",
                 .doc = "A wear mask on convex edges from curvature (default source normals: no new rays): smoothstep from "
                        "threshold (0.1) over softness (0.4) of the normalised curvature, optionally broken up by noise (0-1) at "
                        "noiseScale (texels, 8). options source, range, radius as curvature.",
                 .background = {0.0f, 0.0f, 0.0f, 1.0f},
                 .cpu = curvatureKernel,
                 .finalize = curvatureFinalize(CurvatureMode::EdgeWear, "normals"),
                 .prepare = curvaturePrepare("normals")});
    registerMap({.id = "thickness",
                 .label = "Thickness",
                 .suffix = "thickness",
                 .doc = "Mean distance through the high from each point, along rays into the surface: white is thick, normalised "
                        "(meta thicknessRange). options rays (64), spread (degrees, 180 = cosine hemisphere, 0 = straight in), "
                        "distance (cm: ray length and the value of white; 0 = auto, the thickest texel is white), invert, scope.",
                 .background = {0.0f, 0.0f, 0.0f, 1.0f},
                 .secondaryRays = true,
                 .cpu = thicknessKernel,
                 .finalize = thicknessFinalize});
    registerMap({.id = "bentNormal",
                 .label = "Bent Normal",
                 .suffix = "bentnormals",
                 .doc = "The mean unoccluded direction over the high's cosine hemisphere (the AO rays). options space (tangent "
                        "or object), greenUp, flipX, rays (64), distance (cm, 0 = the group's high bounds), scope.",
                 .background = {0.5f, 0.5f, 1.0f, 1.0f},
                 .secondaryRays = true,
                 .cpu = bentNormalKernel,
                 .finalize = normalFinalize});
    registerMap({.id = "roundedNormal",
                 .label = "Rounded Edges",
                 .suffix = "rounded",
                 .doc = "A ray-traced bevel: the high's normals averaged over the surface within radius of each point, so hard "
                        "edges bake as rounded ones without modelling the bevel. options radius (cm, 0 = 1% of the high's "
                        "bounds), rays (32), space (tangent or object), greenUp, flipX, scope (group).",
                 .background = {0.5f, 0.5f, 1.0f, 1.0f},
                 .secondaryRays = true,
                 .cpu = roundedNormalKernel,
                 .finalize = normalFinalize});
}
} // namespace lila::bake
