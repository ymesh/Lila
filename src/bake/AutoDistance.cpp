#include "bake/AutoDistance.h"

#include "bake/Backend.h"
#include "bake/BakeInputs.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace lila::bake
{
namespace
{
constexpr int kLowSamples = 4096;
constexpr int kHighSamples = 4096;
/** Outward windows double from cap / 2^(kWindows - 1) up to the cap. */
constexpr int kWindows = 8;
constexpr double kKeep = 0.999;
constexpr float kMargin = 1.1f;

/** The distance only (1 - kKeep) of count samples need more than; samples without a need count as 0. */
float percentile(std::vector<float>& needs, size_t count)
{
    if (needs.empty() || !count)
        return 0.0f;
    std::ranges::sort(needs, std::greater<>());
    const size_t allowed = size_t(double(count) * (1.0 - kKeep));
    return allowed < needs.size() ? needs[allowed] : 0.0f;
}

struct LowPoint
{
    glm::vec3 p;
    /** Unit direction rays travel toward the low (minus the cage normal). */
    glm::vec3 dir;
    /** cageOffset x bakeOffset here: frontal is scaled by it. */
    float scale;
    uint32_t low = kNoHit;
};

std::vector<LowPoint> lowSamples(const GroupInputs& gi)
{
    std::vector<std::pair<const LowInput*, size_t>> tris;
    std::vector<double> cumulative;
    double total = 0.0;
    for (const LowInput& low : gi.lows)
        for (size_t t = 0; t < low.soup.triangleCount(); ++t)
        {
            const glm::vec3* p = &low.soup.positions[t * 3];
            const double a = 0.5 * double(glm::length(glm::cross(p[1] - p[0], p[2] - p[0])));
            if (a <= 1e-12)
                continue;
            total += a;
            tris.emplace_back(&low, t);
            cumulative.push_back(total);
        }
    std::vector<LowPoint> out;
    if (tris.empty())
        return out;
    out.reserve(kLowSamples);
    for (int k = 0; k < kLowSamples; ++k)
    {
        const double u = (k + 0.5) / kLowSamples * total;
        const size_t i = std::min(size_t(std::ranges::upper_bound(cumulative, u) - cumulative.begin()), tris.size() - 1);
        const auto [low, t] = tris[i];
        // R2 low-discrepancy point folded into the triangle.
        double a = std::fmod(0.5 + k * 0.7548776662466927, 1.0), b = std::fmod(0.5 + k * 0.5698402909980532, 1.0);
        if (a + b > 1.0)
        {
            a = 1.0 - a;
            b = 1.0 - b;
        }
        const glm::vec3 w(float(1.0 - a - b), float(a), float(b));
        const size_t c = t * 3;
        auto mix = [&](const auto* v) { return v[c] * w.x + v[c + 1] * w.y + v[c + 2] * w.z; };
        LowPoint s;
        s.low = uint32_t(low - gi.lows.data());
        s.p = mix(low->soup.positions.data());
        if (low->cagePositions.size() == low->soup.positions.size())
        {
            const glm::vec3 d = s.p - mix(low->cagePositions.data());
            if (glm::length(d) <= 1e-6f)
                continue;
            s.dir = glm::normalize(d);
            s.scale = 0.0f;
        }
        else
        {
            const glm::vec3 n = mix(low->cageNormals.data());
            if (glm::length(n) <= 0.0f)
                continue;
            s.dir = -glm::normalize(n);
            s.scale = float(gi.settings.cageOffset) * mix(low->cageScale.data());
        }
        out.push_back(s);
    }
    return out;
}

TexelSample ray(const glm::vec3& origin, const glm::vec3& dir, float length)
{
    TexelSample s{};
    for (int k = 0; k < 3; ++k)
    {
        s.position[k] = origin[k];
        s.normal[k] = -dir[k];
        s.origin[k] = origin[k];
        s.direction[k] = dir[k];
    }
    s.tangent[0] = 1.0f;
    s.tangent[3] = 1.0f;
    s.rayLength = length;
    return s;
}

/** Ray needs per low sample: the nearer of the outermost high surface in the first outward window that has one, and the nearest inward. */
void rayNeeds(const GroupInputs& gi, size_t group, const Backend& backend, float cap, std::vector<float>& out, std::vector<float>& in, size_t& count)
{
    const std::vector<LowPoint> pts = lowSamples(gi);
    count += pts.size();
    if (pts.empty())
        return;
    constexpr int perPoint = kWindows + 1;
    std::vector<TexelSample> samples;
    samples.reserve(pts.size() * perPoint);
    for (const LowPoint& s : pts)
    {
        for (int k = 0; k < kWindows; ++k)
        {
            const float t = cap / float(1 << (kWindows - 1 - k));
            samples.push_back(ray(s.p - s.dir * t, s.dir, t));
            samples.back().low = s.low;
        }
        samples.push_back(ray(s.p, s.dir, cap));
        samples.back().low = s.low;
    }
    std::vector<HitRecord> hits(samples.size());
    backend.traceProjection(group, samples, hits);
    backend.endBand(samples);
    // A probed hit is a miss here: the distances are what rays along the normal reach.
    auto valid = [](const HitRecord& h) { return h.high != kNoHit && (h.flags & kHitValid) && !(h.flags & kHitProbed); };
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const HitRecord* h = &hits[i * perPoint];
        float outward = -1.0f;
        for (int k = 0; k < kWindows && outward < 0.0f; ++k)
            if (valid(h[k]))
                outward = std::max(0.0f, samples[i * perPoint + size_t(k)].rayLength - h[k].distance);
        const float inward = valid(h[kWindows]) ? h[kWindows].distance : -1.0f;
        // An explicit cage sets the outward side itself; only rear is measured there.
        if (outward >= 0.0f && pts[i].scale > 1e-4f && (inward < 0.0f || outward <= inward))
            out.push_back(outward / pts[i].scale);
        else if (inward >= 0.0f && (outward < 0.0f || pts[i].scale <= 1e-4f || inward < outward))
            in.push_back(inward);
    }
}

/** High point needs: its offset from the closest low point along the cage normal there. */
void pointNeeds(const GroupInputs& gi, float cap, std::vector<float>& out, std::vector<float>& in, size_t& count)
{
    std::vector<glm::dvec3> pts;
    std::vector<geo::Index> sizes, idx;
    std::vector<std::pair<const LowInput*, size_t>> origin;
    for (const LowInput& low : gi.lows)
    {
        const MeshSoup& s = low.soup;
        for (size_t t = 0; t < s.triangleCount(); ++t)
        {
            const glm::vec3* p = &s.positions[t * 3];
            if (glm::length(glm::cross(p[1] - p[0], p[2] - p[0])) <= 1e-12f)
                continue;
            for (int k = 0; k < 3; ++k)
            {
                idx.push_back(geo::Index(pts.size()));
                pts.push_back(glm::dvec3(p[k]));
            }
            sizes.push_back(3);
            origin.emplace_back(&low, t);
        }
    }
    if (origin.empty())
        return;
    const geo::Mesh lowMesh = geo::Mesh::fromPolygons(pts, sizes, idx);
    if (lowMesh.numFaces() != origin.size())
        return;
    const geo::MeshBvh bvh(lowMesh);
    size_t total = 0;
    for (const HighInput& h : gi.highs)
        total += h.soup.positions.size();
    if (!total)
        return;
    const size_t step = std::max<size_t>(1, total / kHighSamples);
    std::vector<glm::vec3> sample;
    for (const HighInput& h : gi.highs)
        for (size_t i = 0; i < h.soup.positions.size(); i += step)
            sample.push_back(h.soup.positions[i]);
    count += sample.size();
    std::vector<float> d(sample.size(), 0.0f), scale(sample.size(), 0.0f);
    tbb::parallel_for(size_t(0), sample.size(), [&](size_t i) {
        geo::BvhClosest c;
        if (!bvh.closestPoint(glm::dvec3(sample[i]), c, cap) || c.face >= origin.size())
            return;
        const auto [low, t] = origin[c.face];
        const size_t c0 = t * 3;
        const glm::vec3 w(c.bary);
        glm::vec3 n = low->cageNormals[c0] * w.x + low->cageNormals[c0 + 1] * w.y + low->cageNormals[c0 + 2] * w.z;
        n = glm::length(n) > 0.0f ? glm::normalize(n) : glm::vec3(0.0f, 0.0f, 1.0f);
        d[i] = glm::dot(sample[i] - glm::vec3(c.point), n);
        scale[i] = low->cagePositions.size() == low->soup.positions.size()
                       ? 0.0f
                       : float(gi.settings.cageOffset) * (low->cageScale[c0] * w.x + low->cageScale[c0 + 1] * w.y + low->cageScale[c0 + 2] * w.z);
    });
    for (size_t i = 0; i < sample.size(); ++i)
        if (d[i] > 0.0f && scale[i] > 1e-4f)
            out.push_back(d[i] / scale[i]);
        else if (d[i] < 0.0f)
            in.push_back(-d[i]);
}
} // namespace

void resolveAutoDistances(BakeInputs& in, const Backend& backend)
{
    LILA_ZONE();

    for (size_t g = 0; g < in.groups.size(); ++g)
    {
        GroupInputs& gi = in.groups[g];
        const bool autoFrontal = gi.settings.frontal <= 0.0, autoRear = gi.settings.rear <= 0.0;
        if (gi.highs.empty() || gi.lows.empty() || (!autoFrontal && !autoRear))
            continue;
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        for (const LowInput& l : gi.lows)
            if (l.soup.triangleCount())
            {
                lo = glm::min(lo, l.soup.boundsMin);
                hi = glm::max(hi, l.soup.boundsMax);
            }
        if (lo.x > hi.x)
            continue;
        const float diag = glm::length(hi - lo);
        const float cap = 0.25f * diag;
        if (cap <= 0.0f)
            continue;

        std::vector<float> rayOut, rayIn, ptOut, ptIn;
        size_t rays = 0, points = 0;
        rayNeeds(gi, g, backend, cap, rayOut, rayIn, rays);
        if (rayOut.empty() && rayIn.empty())
            continue;
        pointNeeds(gi, cap, ptOut, ptIn, points);
        const float needOut = std::max(percentile(rayOut, rays), percentile(ptOut, points));
        const float needIn = std::max(percentile(rayIn, rays), percentile(ptIn, points));
        const float floor = 0.005f * diag;
        if (autoFrontal)
            gi.frontal = std::max(floor, needOut * kMargin);
        if (autoRear)
            gi.rear = std::max(floor, needIn * kMargin);
        gi.measured = true;
    }
}
} // namespace lila::bake
