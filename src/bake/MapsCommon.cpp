#include "bake/MapMath.h"
#include "bake/MapsInternal.h"

#include "bake/Backend.h"
#include "bake/BakeInputs.h"
#include "core/ImageIO.h"
#include "core/Log.h"
#include "core/Paths.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace lila::bake
{
using namespace mapmath;

namespace
{
std::mutex gLoaderMutex;

TextureLoader& loader()
{
    static TextureLoader l = [](const std::string& path, int& w, int& h, std::vector<float>& rgba, std::string& err) {
        image::Image img = image::read(paths::fromUtf8(path), &err);
        if (img.width <= 0)
            return false;
        w = img.width;
        h = img.height;
        rgba = std::move(img.rgba);
        return true;
    };
    return l;
}

/** A high's normal-map sampler per material slot, per group and high. */
struct NormalMapSlot
{
    std::shared_ptr<const TextureSampler> map;
    bool openGL = false;
};
} // namespace

struct HighNormalMaps
{
    std::vector<std::vector<std::vector<NormalMapSlot>>> slots;
};

bool Tracer::closestHit(size_t group, OccluderScope scope, uint32_t high, uint32_t low, RayKind, const glm::vec3& origin, const glm::vec3& dir, float tMax,
                        TraceHit& hit) const
{
    if (!occluded(group, scope, high, low, origin, dir, tMax))
        return false;
    float lo = 0.0f, hi = tMax;
    for (int i = 0; i < 20; ++i)
    {
        const float mid = 0.5f * (lo + hi);
        (occluded(group, scope, high, low, origin, dir, mid) ? hi : lo) = mid;
    }
    hit = TraceHit{.distance = hi};
    return true;
}

bool loadBakeTexture(const std::string& path, int& width, int& height, std::vector<float>& rgba, std::string& error)
{
    std::lock_guard lock(gLoaderMutex);
    return loader()(path, width, height, rgba, error) && width > 0 && height > 0 && rgba.size() >= size_t(width) * size_t(height) * 4;
}

void setTextureLoader(TextureLoader l)
{
    std::lock_guard lock(gLoaderMutex);
    if (l)
        loader() = std::move(l);
}

glm::vec4 TextureSampler::sample(const glm::vec2& uv) const
{
    if (empty())
        return glm::vec4(1.0f);
    const float x = uv.x * float(width) - 0.5f, y = (1.0f - uv.y) * float(height) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float ax = x - fx, ay = y - fy;
    auto wrap = [](int i, int n) { return ((i % n) + n) % n; };
    const int x0 = wrap(int(fx), width), x1 = wrap(int(fx) + 1, width), y0 = wrap(int(fy), height), y1 = wrap(int(fy) + 1, height);
    auto at = [&](int px, int py) {
        const float* p = &rgba[(size_t(py) * size_t(width) + size_t(px)) * 4];
        return glm::vec4(p[0], p[1], p[2], p[3]);
    };
    return glm::mix(glm::mix(at(x0, y0), at(x1, y0), ax), glm::mix(at(x0, y1), at(x1, y1), ax), ay);
}

std::shared_ptr<const TextureSampler> TextureCache::get(const std::string& path)
{
    if (path.empty())
        return nullptr;
    for (const auto& [p, t] : mLoaded)
        if (p == path)
            return t;
    auto t = std::make_shared<TextureSampler>();
    std::string err;
    bool ok = false;
    {
        std::lock_guard lock(gLoaderMutex);
        ok = loader()(path, t->width, t->height, t->rgba, err);
    }
    if (!ok || t->width <= 0 || t->height <= 0 || t->rgba.size() < size_t(t->width) * size_t(t->height) * 4)
    {
        warnings.push_back("can't read texture " + path + (err.empty() ? std::string() : ": " + err));
        log::warn("bake: can't read texture {}{}", path, err.empty() ? std::string() : ": " + err);
        t.reset();
    }
    mLoaded.emplace_back(path, t);
    return t;
}

glm::vec2 hitUv(const HighInput& high, const HitRecord& hit)
{
    if (high.soup.uvs.empty() || hit.triangle == kNoHit)
        return glm::vec2(0.0f);
    return atHit(high.soup.uvs, hit);
}

float worldPerUv(const LowInput& low, uint32_t t)
{
    const size_t c = size_t(t) * 3;
    if (low.soup.uvs.size() < c + 3)
        return 0.0f;
    const glm::vec3* p = &low.soup.positions[c];
    const glm::vec2* uv = &low.soup.uvs[c];
    const float area3 = 0.5f * glm::length(glm::cross(p[1] - p[0], p[2] - p[0]));
    const glm::vec2 a = uv[1] - uv[0], b = uv[2] - uv[0];
    const float areaUv = 0.5f * std::abs(a.x * b.y - a.y * b.x);
    return areaUv > 0.0f ? std::sqrt(area3 / areaUv) : 0.0f;
}

glm::vec2 tangentSlopes(const TexelSample& s, const glm::vec3& normal)
{
    const glm::vec3 e = encodeTangentNormal(normal, v3(s.normal), glm::vec4(s.tangent[0], s.tangent[1], s.tangent[2], s.tangent[3]), true);
    // Past ~84 degrees from the low the slope means nothing; clamping keeps one wild texel from swamping the field.
    const float z = std::max(e.z, 0.1f);
    return {e.x / z, e.y / z};
}

void slopeKernel(const MapContext& ctx, const void* normalMaps, std::span<const TexelSample> samples, std::span<const HitRecord> hits,
                 std::span<glm::vec4> out, std::span<uint8_t> valid)
{
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const TexelSample& s = samples[i];
        const glm::vec2 sl = hits[i].flags & kHitValid ? tangentSlopes(s, hitNormal(normalMaps, ctx, hits[i])) : glm::vec2(0.0f);
        out[i] = glm::vec4(sl, 0.0f, worldPerUv(ctx.group.lows[s.low], s.lowTriangle));
        valid[i] = 1;
    }
}

std::vector<float> curvatureFromSlopes(const TextureBuffer& tex, int radius)
{
    const int W = tex.width, H = tex.height, r = std::max(1, radius);
    std::vector<float> curv(tex.texelCount(), 0.0f);
    auto idx = [&](int x, int y) { return size_t(y) * size_t(W) + size_t(x); };
    // A slope jump this large between neighbours is a UV seam (the tangent frame changes), not geometry.
    const float seam = 1.0f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const size_t i = idx(x, y);
            if (!tex.covered[i])
                continue;
            const glm::vec4 c = tex.value[i];
            const float wpu = c.w;
            if (!(wpu > 0.0f))
                continue;
            // d/du along +x, d/dv along -y (row 0 is the top of the texture, V = 1).
            auto derivative = [&](int dx, int dy, int comp, float texelWorld) {
                float sum = 0.0f, span = 0.0f;
                const int px = x + dx * r, py = y + dy * r, nx = x - dx * r, ny = y - dy * r;
                const bool hasP = px >= 0 && px < W && py >= 0 && py < H && tex.covered[idx(px, py)] &&
                                  std::abs(tex.value[idx(px, py)][comp] - c[comp]) < seam * float(r);
                const bool hasN = nx >= 0 && nx < W && ny >= 0 && ny < H && tex.covered[idx(nx, ny)] &&
                                  std::abs(tex.value[idx(nx, ny)][comp] - c[comp]) < seam * float(r);
                if (hasP && hasN)
                {
                    sum = tex.value[idx(px, py)][comp] - tex.value[idx(nx, ny)][comp];
                    span = 2.0f * float(r);
                }
                else if (hasP)
                {
                    sum = tex.value[idx(px, py)][comp] - c[comp];
                    span = float(r);
                }
                else if (hasN)
                {
                    sum = c[comp] - tex.value[idx(nx, ny)][comp];
                    span = float(r);
                }
                return span > 0.0f ? sum / (span * texelWorld) : 0.0f;
            };
            const float du = derivative(1, 0, 0, wpu / float(W));
            const float dv = derivative(0, -1, 1, wpu / float(H));
            curv[i] = 0.5f * (du + dv);
        }
    return curv;
}

std::vector<uint32_t> weldPositions(const std::vector<glm::vec3>& positions, size_t& count)
{
    struct Key
    {
        uint32_t x, y, z;
        bool operator==(const Key&) const = default;
    };
    struct Hash
    {
        size_t operator()(const Key& k) const { return size_t(hashBits(k.x) ^ hashBits(k.y * 0x9e3779b9u) ^ hashBits(k.z + 0x632be5abu)); }
    };
    std::unordered_map<Key, uint32_t, Hash> index;
    index.reserve(positions.size() / 4 + 1);
    std::vector<uint32_t> weld(positions.size());
    count = 0;
    for (size_t c = 0; c < positions.size(); ++c)
    {
        Key k;
        std::memcpy(&k.x, &positions[c].x, 4);
        std::memcpy(&k.y, &positions[c].y, 4);
        std::memcpy(&k.z, &positions[c].z, 4);
        auto [it, added] = index.try_emplace(k, uint32_t(count));
        if (added)
            ++count;
        weld[c] = it->second;
    }
    return weld;
}

float percentileAbs(const std::vector<float>& v, const std::vector<uint8_t>& covered, float percentile)
{
    std::vector<float> a;
    a.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i)
        if (covered[i])
            a.push_back(std::abs(v[i]));
    if (a.empty())
        return 0.0f;
    const size_t k = std::min(a.size() - 1, size_t(std::clamp(percentile, 0.0f, 1.0f) * float(a.size() - 1) + 0.5f));
    std::nth_element(a.begin(), a.begin() + std::ptrdiff_t(k), a.end());
    return a[k];
}

std::shared_ptr<const void> prepareHighNormalMaps(const MapSettings& settings, const BakeInputs& inputs)
{
    if (!settings.options.value("highNormalMap", false))
        return nullptr;
    auto maps = std::make_shared<HighNormalMaps>();
    TextureCache cache;
    bool any = false;
    maps->slots.resize(inputs.groups.size());
    for (size_t g = 0; g < inputs.groups.size(); ++g)
    {
        const GroupInputs& gi = inputs.groups[g];
        maps->slots[g].resize(gi.highs.size());
        for (size_t h = 0; h < gi.highs.size(); ++h)
        {
            const MeshSoup& soup = gi.highs[h].soup;
            // A self high wears its low's material, whose normal map is usually this bake's last output: applying it would
            // fold every previous bake into the next.
            if (soup.uvs.empty() || (h < gi.highSelfLow.size() && gi.highSelfLow[h] >= 0))
                continue;
            auto& slots = maps->slots[g][h];
            slots.resize(soup.materialNames.size());
            for (size_t m = 0; m < soup.materialNames.size(); ++m)
            {
                auto it = inputs.materials.find(soup.materialNames[m]);
                if (it == inputs.materials.end() || it->second.normalMap.empty())
                    continue;
                slots[m] = {cache.get(it->second.normalMap), it->second.normalOpenGL};
                any = any || slots[m].map;
            }
        }
    }
    return any ? std::shared_ptr<const void>(maps) : nullptr;
}

glm::vec3 hitNormal(const void* prepared, const MapContext& ctx, const HitRecord& hit)
{
    const glm::vec3 n = v3(hit.normal);
    if (!prepared || !(hit.flags & kHitValid))
        return n;
    const auto& maps = *static_cast<const HighNormalMaps*>(prepared);
    if (ctx.groupIndex >= maps.slots.size() || hit.high >= maps.slots[ctx.groupIndex].size())
        return n;
    const auto& slots = maps.slots[ctx.groupIndex][hit.high];
    const MeshSoup& soup = ctx.group.highs[hit.high].soup;
    const uint32_t slot = hit.triangle < soup.materials.size() ? soup.materials[hit.triangle] : 0u;
    if (slot >= slots.size() || !slots[slot].map)
        return n;

    // The high's per-triangle UV frame, made orthonormal to the interpolated normal: what its normal map was baked in.
    const size_t c = size_t(hit.triangle) * 3;
    const glm::vec3 e1 = soup.positions[c + 1] - soup.positions[c], e2 = soup.positions[c + 2] - soup.positions[c];
    const glm::vec2 d1 = soup.uvs[c + 1] - soup.uvs[c], d2 = soup.uvs[c + 2] - soup.uvs[c];
    const float det = d1.x * d2.y - d2.x * d1.y;
    if (std::abs(det) < 1e-20f)
        return n;
    const glm::vec3 tu = (e1 * d2.y - e2 * d1.y) / det, tv = (e2 * d1.x - e1 * d2.x) / det;
    const glm::vec3 t = unit(tu - n * glm::dot(n, tu), glm::vec3(1.0f, 0.0f, 0.0f));
    const float sign = glm::dot(glm::cross(n, t), tv) < 0.0f ? -1.0f : 1.0f;
    const glm::vec3 b = glm::cross(n, t) * sign;
    glm::vec3 m = glm::vec3(slots[slot].map->sample(atHit(soup.uvs, hit))) * 2.0f - 1.0f;
    if (!slots[slot].openGL)
        m.y = -m.y;
    return unit(t * m.x + b * m.y + n * m.z, n);
}
} // namespace lila::bake
