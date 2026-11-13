#include "bake/Maps.h"

#include "bake/Backend.h"
#include "bake/BakeInputs.h"
#include "bake/MapMath.h"
#include "bake/MapsInternal.h"
#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace lila::bake
{
using namespace mapmath;

namespace
{
std::mutex gMapsMutex;

std::map<std::string, MapDef, std::less<>>& registry()
{
    static std::map<std::string, MapDef, std::less<>> r;
    return r;
}

float quantize8(float v) { return std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f) / 255.0f; }
glm::vec3 quantize8(const glm::vec3& c) { return {quantize8(c.r), quantize8(c.g), quantize8(c.b)}; }

glm::vec3 hsv(float h, float s, float v)
{
    const float c = v * s, hp = std::fmod(h, 1.0f) * 6.0f, x = c * (1.0f - std::abs(std::fmod(hp, 2.0f) - 1.0f));
    glm::vec3 rgb = hp < 1 ? glm::vec3(c, x, 0) : hp < 2 ? glm::vec3(x, c, 0) : hp < 3 ? glm::vec3(0, c, x)
                  : hp < 4 ? glm::vec3(0, x, c) : hp < 5 ? glm::vec3(x, 0, c) : glm::vec3(c, 0, x);
    return rgb + glm::vec3(v - c);
}

/** Colour of an ID name: the palette option's entry when it names one, else idColour. */
glm::vec3 paletteLookup(const json& palette, const std::string& name)
{
    if (palette.is_object())
    {
        auto it = palette.find(name);
        if (it != palette.end())
        {
            if (it->is_string())
            {
                const std::string s = it->get<std::string>();
                if (s.size() == 7 && s[0] == '#')
                {
                    const unsigned long v = std::stoul(s.substr(1), nullptr, 16);
                    return glm::vec3(float((v >> 16) & 0xff), float((v >> 8) & 0xff), float(v & 0xff)) / 255.0f;
                }
            }
            else if (it->is_array() && it->size() >= 3)
            {
                glm::vec3 c((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
                if (c.r > 1.0f || c.g > 1.0f || c.b > 1.0f)
                    c /= 255.0f;
                return quantize8(c);
            }
        }
    }
    return idColour(name);
}

std::string idName(const MapContext& ctx, const std::string& source, const HitRecord& h)
{
    const HighInput& high = ctx.group.highs[h.high];
    if (source == "object")
        return high.source;
    if (source == "group")
        return ctx.group.name;
    const uint32_t m = h.triangle < high.soup.materials.size() ? high.soup.materials[h.triangle] : 0u;
    return m < high.soup.materialNames.size() ? high.soup.materialNames[m] : std::string("Default");
}

void normalKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                  std::span<uint8_t> valid)
{
    const bool greenUp = ctx.settings.options.value("greenUp", false);
    const bool flipX = ctx.settings.options.value("flipX", false);
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const TexelSample& s = samples[i];
        const glm::vec3 n = unit(v3(s.normal));
        const glm::vec3 h = hits[i].flags & kHitValid ? hitNormal(ctx.prepared, ctx, hits[i]) : n;
        const glm::vec3 e = encodeTangentNormal(h, n, glm::vec4(s.tangent[0], s.tangent[1], s.tangent[2], s.tangent[3]), greenUp, flipX);
        out[i] = glm::vec4(e * 0.5f + 0.5f, 1.0f);
        valid[i] = 1;
    }
}

/** Averaged normals come out short; put them back on the unit sphere. */
void renormalize(const MapSettings&, const BakeInputs&, TextureBuffer& tex, json&)
{
    for (size_t i = 0; i < tex.value.size(); ++i)
        if (tex.covered[i])
        {
            const glm::vec3 v = unit(glm::vec3(tex.value[i]) * 2.0f - 1.0f);
            tex.value[i] = glm::vec4(v * 0.5f + 0.5f, 1.0f);
        }
}

void objectNormalKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                        std::span<uint8_t> valid)
{
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const glm::vec3 n = hits[i].flags & kHitValid ? hitNormal(ctx.prepared, ctx, hits[i]) : unit(v3(samples[i].normal));
        out[i] = glm::vec4(n * 0.5f + 0.5f, 1.0f);
        valid[i] = 1;
    }
}

void positionKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                    std::span<uint8_t> valid)
{
    const glm::vec3 lo = ctx.inputs.highMin, ext = ctx.inputs.highMax - ctx.inputs.highMin;
    for (size_t i = 0; i < samples.size(); ++i)
    {
        valid[i] = (hits[i].flags & kHitValid) ? 1 : 0;
        if (!valid[i])
            continue;
        const glm::vec3 p = v3(hits[i].position);
        glm::vec3 v;
        for (int k = 0; k < 3; ++k)
            v[k] = ext[k] > 0.0f ? (p[k] - lo[k]) / ext[k] : 0.5f;
        out[i] = glm::vec4(v, 1.0f);
    }
}

void positionMeta(const MapSettings&, const BakeInputs& in, TextureBuffer&, json& meta)
{
    meta["boundsMin"] = {in.highMin.x, in.highMin.y, in.highMin.z};
    meta["boundsMax"] = {in.highMax.x, in.highMax.y, in.highMax.z};
}

void heightKernel(const MapContext&, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
                  std::span<uint8_t> valid)
{
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const float h = hits[i].flags & kHitValid ? hits[i].height : 0.0f;
        out[i] = glm::vec4(h, h, h, 1.0f);
        valid[i] = 1;
    }
}

/** Raw heights to 0-1 around 0.5: range from the options, or auto (the largest |height| baked into this texture). */
void heightFinalize(const MapSettings& settings, const BakeInputs&, TextureBuffer& tex, json& meta)
{
    double range = settings.options.value("range", 0.0);
    const bool automatic = !(range > 0.0);
    if (automatic)
    {
        range = 0.0;
        for (size_t i = 0; i < tex.value.size(); ++i)
            if (tex.covered[i])
                range = std::max(range, double(std::abs(tex.value[i].r)));
        if (range <= 0.0)
            range = 1.0;
    }
    for (size_t i = 0; i < tex.value.size(); ++i)
    {
        const float v = tex.covered[i] ? float(std::clamp(0.5 + double(tex.value[i].r) / (2.0 * range), 0.0, 1.0)) : 0.5f;
        tex.value[i] = glm::vec4(v, v, v, 1.0f);
    }
    meta["heightRange"] = range;
    meta["heightMin"] = -range;
    meta["heightMax"] = range;
    meta["heightAuto"] = automatic;
    meta["decode"] = "height = (value - 0.5) * 2 * heightRange";
}

void idKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
              std::span<uint8_t> valid)
{
    const std::string source = ctx.settings.options.value("source", std::string("material"));
    const json palette = ctx.settings.options.value("palette", json::object());
    // Kernels run per band; the lookups repeat, so cache them per call.
    std::map<std::string, glm::vec3> cache;
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const HitRecord& h = hits[i];
        valid[i] = (h.flags & kHitValid) ? 1 : 0;
        if (!valid[i])
            continue;
        if (source == "vertexColor")
        {
            const HighInput& high = ctx.group.highs[h.high];
            if (high.soup.colors.empty())
            {
                out[i] = glm::vec4(1.0f);
                continue;
            }
            const float w[3] = {1.0f - h.bary[0] - h.bary[1], h.bary[0], h.bary[1]};
            const int k = int(std::max_element(w, w + 3) - w);
            out[i] = glm::vec4(quantize8(glm::vec3(high.soup.colors[size_t(h.triangle) * 3 + size_t(k)])), 1.0f);
            continue;
        }
        const std::string name = idName(ctx, source, h);
        auto it = cache.find(name);
        if (it == cache.end())
            it = cache.emplace(name, paletteLookup(palette, name)).first;
        out[i] = glm::vec4(it->second, 1.0f);
    }
}

void idLegend(const MapSettings& settings, const BakeInputs& in, TextureBuffer&, json& meta)
{
    const std::string source = settings.options.value("source", std::string("material"));
    const json palette = settings.options.value("palette", json::object());
    json legend = json::object();
    auto add = [&](const std::string& name) {
        const glm::vec3 c = paletteLookup(palette, name);
        legend[name] = {std::lround(c.r * 255.0f), std::lround(c.g * 255.0f), std::lround(c.b * 255.0f)};
    };
    for (const GroupInputs& g : in.groups)
    {
        if (source == "group")
            add(g.name);
        for (const HighInput& h : g.highs)
        {
            if (source == "object")
                add(h.source);
            else if (source == "material")
                for (const std::string& m : h.soup.materialNames)
                    add(m);
        }
    }
    meta["source"] = source;
    if (source != "vertexColor")
        meta["legend"] = legend;
}

void alphaKernel(const MapContext&, std::span<const TexelSample> samples, std::span<const HitRecord>, std::span<glm::vec4> out,
                 std::span<uint8_t> valid)
{
    for (size_t i = 0; i < samples.size(); ++i)
    {
        out[i] = glm::vec4(1.0f);
        valid[i] = 1;
    }
}

void islandKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord>, std::span<glm::vec4> out,
                  std::span<uint8_t> valid)
{
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const LowInput& low = ctx.group.lows[samples[i].low];
        const uint32_t island = samples[i].lowTriangle < low.islands.size() ? low.islands[samples[i].lowTriangle] : 0u;
        out[i] = glm::vec4(paletteColour(island), 1.0f);
        valid[i] = 1;
    }
}

/** A front-facing surface within tMax, stepping through back faces on the way. */
bool frontOccluded(const MapContext& ctx, OccluderScope scope, uint32_t high, uint32_t low, glm::vec3 o, const glm::vec3& d, float tMax)
{
    const float step = std::max(1e-4f * glm::length(ctx.group.highMax - ctx.group.highMin), 1e-4f);
    for (int crossings = 0; crossings < 16 && tMax > 0.0f; ++crossings)
    {
        TraceHit th;
        if (!ctx.tracer->closestHit(ctx.groupIndex, scope, high, low, RayKind::Occlusion, o, d, tMax, th))
            return false;
        if (!th.backface)
            return true;
        // A front face lying on the back face (parts resting on each other share the plane) is met from behind on the
        // way back, so it still occludes rather than being stepped past with it.
        TraceHit coincident;
        const glm::vec3 past = o + d * (th.distance + step);
        if (ctx.tracer->closestHit(ctx.groupIndex, scope, high, low, RayKind::Occlusion, past, -d, 2.0f * step, coincident) && coincident.backface)
            return true;
        o = past;
        tMax -= th.distance + step;
    }
    return false;
}

/**
 * A point buried inside a high: the surface met along n is a back face of a high that also closes behind it (along -n),
 * which an open lid doesn't. exit is where the point leaves that high on the nearer side, just outside it.
 */
bool aoBuried(const MapContext& ctx, OccluderScope scope, uint32_t low, const glm::vec3& o, const glm::vec3& n, float tMax, glm::vec3& exit)
{
    TraceHit up, down;
    if (!(ctx.tracer->closestHit(ctx.groupIndex, scope, kNoHit, low, RayKind::Occlusion, o, n, tMax, up) && up.backface && up.high != kNoHit &&
          ctx.tracer->closestHit(ctx.groupIndex, OccluderScope::Self, up.high, kNoHit, RayKind::Occlusion, o, -n, tMax, down) && down.backface))
        return false;
    const float step = std::max(1e-4f * glm::length(ctx.group.highMax - ctx.group.highMin), 1e-4f);
    exit = up.distance <= down.distance ? o + n * (up.distance + step) : o - n * (down.distance + step);
    return true;
}

void aoKernel(const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits, std::span<glm::vec4> out,
              std::span<uint8_t> valid)
{
    LILA_ZONE();

    const int rays = std::clamp(ctx.settings.options.value("rays", 64), 1, 4096);
    const float diag = glm::length(ctx.group.highMax - ctx.group.highMin);
    const double distOpt = ctx.settings.options.value("distance", 0.0);
    const float dist = distOpt > 0.0 ? float(distOpt) : std::max(diag, 1e-3f);
    const OccluderScope scope = occluderScope(ctx.settings.options.value("scope", ctx.group.settings.aoScope));
    const float eps = std::max(1e-4f * diag, 1e-4f);
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const HitRecord& h = hits[i];
        // A miss is left to the texel's other samples or the fill, never baked as open sky.
        valid[i] = (h.flags & kHitValid) ? 1 : 0;
        if (!valid[i] || !ctx.tracer)
        {
            out[i] = glm::vec4(1.0f);
            continue;
        }
        const glm::vec3 n = v3(h.normal);
        glm::vec3 t, b;
        basis(n, t, b);
        const glm::vec3 o = v3(h.position) + n * eps;
        const uint32_t seed = hashBits(samples[i].texel * 64u + samples[i].sub) ^ hashBits(samples[i].tile);
        const float r1 = float(seed & 0xffff) / 65536.0f, r2 = float(seed >> 16) / 65536.0f;
        auto direction = [&](int k) {
            const float u = std::fmod((float(k) + 0.5f) / float(rays) + r1, 1.0f);
            const float v = std::fmod(radicalInverse(uint32_t(k)) + r2, 1.0f);
            // Cosine-weighted hemisphere.
            const float r = std::sqrt(u), phi = 6.28318530718f * v;
            return unit(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0f, 1.0f - u)));
        };
        int blocked = 0, backs = 0;
        for (int k = 0; k < rays; ++k)
        {
            const glm::vec3 d = direction(k);
            TraceHit th;
            if (scope == OccluderScope::Self)
                blocked += ctx.tracer->occluded(ctx.groupIndex, scope, h.high, samples[i].low, o, d, dist) ? 1 : 0;
            else if (ctx.tracer->closestHit(ctx.groupIndex, scope, h.high, samples[i].low, RayKind::Occlusion, o, d, dist, th))
            {
                ++blocked;
                backs += th.backface ? 1 : 0;
            }
        }
        // Buried inside another high (where parts meet): seen from where it leaves that high, so it shades like the
        // surface just outside, dark facing in and light facing out; only front faces occlude.
        glm::vec3 exit;
        if (backs * 4 >= rays * 3 && aoBuried(ctx, scope, samples[i].low, o, n, dist, exit))
        {
            blocked = 0;
            for (int k = 0; k < rays; ++k)
                blocked += frontOccluded(ctx, scope, h.high, samples[i].low, exit, direction(k), dist) ? 1 : 0;
        }
        const float ao = 1.0f - float(blocked) / float(rays);
        out[i] = glm::vec4(ao, ao, ao, 1.0f);
    }
}
} // namespace

OccluderScope occluderScope(std::string_view name)
{
    if (name == "self")
        return OccluderScope::Self;
    if (name == "all")
        return OccluderScope::All;
    return OccluderScope::Group;
}

glm::vec3 encodeTangentNormal(const glm::vec3& highNormal, const glm::vec3& lowNormal, const glm::vec4& lowTangent, bool greenUp, bool flipX)
{
    const glm::vec3 n = unit(lowNormal);
    glm::vec3 tg(lowTangent);
    const glm::vec3 t = unit(tg - n * glm::dot(n, tg), std::abs(n.x) < 0.9f ? unit(glm::cross(n, glm::vec3(1, 0, 0))) : unit(glm::cross(n, glm::vec3(0, 1, 0))));
    const glm::vec3 b = glm::cross(n, t) * (lowTangent.w < 0.0f ? -1.0f : 1.0f);
    const glm::vec3 h = unit(highNormal, n);
    glm::vec3 e(glm::dot(h, t), glm::dot(h, b), glm::dot(h, n));
    if (!greenUp)
        e.y = -e.y;
    if (flipX)
        e.x = -e.x;
    return e;
}

glm::vec3 idColour(std::string_view name)
{
    uint32_t h = 2166136261u;
    for (char c : name)
    {
        h ^= uint8_t(c);
        h *= 16777619u;
    }
    h = hashBits(h);
    const float hue = float(h & 0xffff) / 65536.0f;
    const float sat = 0.55f + 0.4f * float((h >> 16) & 0xff) / 255.0f;
    const float val = 0.7f + 0.3f * float(h >> 24) / 255.0f;
    return quantize8(hsv(hue, sat, val));
}

glm::vec3 paletteColour(uint32_t i)
{
    const float hue = std::fmod(float(i) * 0.61803398875f, 1.0f);
    return quantize8(hsv(hue, i % 2 ? 0.6f : 0.85f, i % 3 == 1 ? 0.75f : 0.95f));
}

void registerMap(MapDef def)
{
    std::lock_guard lock(gMapsMutex);
    std::string id = def.id;
    registry()[id] = std::move(def);
}

const MapDef* findMap(std::string_view id)
{
    std::lock_guard lock(gMapsMutex);
    auto it = registry().find(id);
    return it == registry().end() ? nullptr : &it->second;
}

std::vector<const MapDef*> allMaps()
{
    std::lock_guard lock(gMapsMutex);
    std::vector<const MapDef*> out;
    for (const auto& [k, v] : registry())
        out.push_back(&v);
    return out;
}

void registerBuiltinMaps()
{
    if (findMap("normal"))
        return;
    registerMap({.id = "normal",
                 .label = "Normal",
                 .suffix = "normal",
                 .doc = "Tangent-space normal in the MikkTSpace per-fragment frame Unreal and the viewport decode. DirectX (green down) by "
                        "default; options greenUp (OpenGL, Painter) and flipX. highNormalMap (off) adds the high's own material "
                        "normal map; self highs never use it.",
                 .background = {0.5f, 0.5f, 1.0f, 1.0f},
                 .cpu = normalKernel,
                 .finalize = renormalize,
                 .prepare = prepareHighNormalMaps});
    registerMap({.id = "objectNormal",
                 .label = "Object Normal",
                 .suffix = "normalobj",
                 .doc = "The high's normal in bake (world) space, n * 0.5 + 0.5. option highNormalMap (off) as for normal.",
                 .background = {0.5f, 0.5f, 0.5f, 1.0f},
                 .cpu = objectNormalKernel,
                 .finalize = renormalize,
                 .prepare = prepareHighNormalMaps});
    registerMap({.id = "position",
                 .label = "Position",
                 .suffix = "position",
                 .doc = "The high's surface position normalised to the bounds of every high in the bake (meta boundsMin/boundsMax).",
                 .preferredBits = 16,
                 .cpu = positionKernel,
                 .finalize = positionMeta});
    registerMap({.id = "height",
                 .label = "Height",
                 .suffix = "height",
                 .doc = "Signed distance of the high above the low, 0.5 at the low. option range (cm, 0 = auto: the largest distance "
                        "baked); the range used is in meta heightRange.",
                 .background = {0.5f, 0.5f, 0.5f, 1.0f},
                 .preferredBits = 16,
                 .cpu = heightKernel,
                 .finalize = heightFinalize});
    registerMap({.id = "id",
                 .label = "Material ID",
                 .suffix = "id",
                 .doc = "Exact flat colours per material (option source: material, object, group or vertexColor), majority vote per "
                        "texel, never blended, so colour pickers match every texel. option palette {name: [r, g, b] or \"#rrggbb\"}; "
                        "meta legend lists the colours.",
                 .resolve = Resolve::Exact,
                 .linear = false,
                 .cpu = idKernel,
                 .finalize = idLegend});
    registerMap({.id = "alpha",
                 .label = "Alpha / Coverage",
                 .suffix = "opacity",
                 .doc = "The fraction of each texel the low's UVs cover; not dilated.",
                 .resolve = Resolve::Coverage,
                 .dilate = false,
                 .cpu = alphaKernel});
    registerMap({.id = "uvIslands",
                 .label = "UV Islands",
                 .suffix = "islands",
                 .doc = "A flat exact colour per UV island of the low.",
                 .resolve = Resolve::Exact,
                 .linear = false,
                 .cpu = islandKernel});
    registerMap({.id = "ao",
                 .label = "Ambient Occlusion",
                 .suffix = "ao",
                 .doc = "Cosine-weighted occlusion from the high's surface; a point buried inside another high (where parts meet) "
                        "shades like the surface where it leaves that high, front faces only. options rays (64), distance (cm, 0 = the "
                        "group's high bounds), scope (self, group, all; default the group's aoScope).",
                 .secondaryRays = true,
                 .cpu = aoKernel});
    registerSurfaceMaps();
    registerMaterialMaps();
    registerUvMaps();
    registerDerivedMaps();
    registerPackMaps();
}
} // namespace lila::bake
