// The path-traced viewport: progressive Monte Carlo light transport through the scene's acceleration structure with
// ray queries. GGX specular and Lambert diffuse (Unreal's default lit inputs plus the extended channels), the environment
// importance sampled from a luminance table over its cube faces, the scene's lights sampled one at a time, both combined
// with the BSDF's own samples by multiple importance sampling. traceCS adds samples into running sums; resolveCS averages,
// optionally denoises (an edge-aware filter guided by the first hit's albedo, normal and depth) and tonemaps them.

#include "scene_color.slang"
#include "tonemap.slang"

/** Matches render::PtParams. */
struct PtParams
{
    float4x4 invViewProj; // unjittered, camera-relative
    float4 eye;           // xyz, w 1 for an orthographic view
    float4 camRight;      // xyz, w lens radius (cm)
    float4 camUp;         // xyz, w focus distance (cm)
    float4 camForward;    // xyz, w exposure
    float4 env;           // cos and sin of the rotation, intensity, light saturation
    float4 envExtra;      // x 1 when Z is up, y background exposure, z 1 to show the environment behind, w roughest lod
    float4 size;          // extent, then its inverse
    uint4 counts;         // x samples so far, y samples to add, z bounces, w seed
    uint4 counts2;        // x lights, y unused, z environment table cells, w cells per face side
    uint4 flags;          // x denoise, y tonemapper, z AgX look, w target samples
    float4 misc;          // x display gamma, y brightest indirect sample, z 1 for a transparent background
    uint64_t instances;   // PtInstance per acceleration structure instance
    uint64_t sections;    // PtSection
    uint64_t textureTable;
    uint64_t lights; // GpuFrame, then GpuLight entries
    uint64_t envTable; // cells floats of running sum, then cells floats of probability
    uint64_t pad;
};

/** Matches render::PtInstance. */
struct PtInstance
{
    uint64_t vertices; // MeshVertex, 64 bytes
    uint64_t indices;
    float4 rows[3];
    uint firstSection;
    uint sectionCount;
    uint2 tint; // halves: linear rgb, amount (read under kInstanceColor)
};

/** Matches render::PtSection: a material section's inputs as the mesh shader takes them. */
struct PtSection
{
    uint firstTriangle;
    uint triangleCount;
    uint flags;
    uint sampling;
    float4 base;     // rgb base colour, a roughness
    float4 surface;  // metallic, specular, opacity, opacity clip
    float4 emissive; // rgb, w shading model
    uint4 maps;      // base | normal << 16, ORM | emissive << 16, extended maps (see kExt*)
    float4 ext0;     // sheen rgb, sheen roughness
    float4 ext1;     // coat, coat roughness, anisotropy, anisotropy rotation (radians)
    float4 ext2;     // subsurface rgb, transmission
    float4 ext3;     // specular colour times weight, ior
    uint4 extMaps0;  // ExtendedShading map handles, two to a word
    uint4 extMaps1;
    float subsurfaceRadius;
    float3 pad2;
};

struct PtPush
{
    uint64_t params;
};

[[vk::push_constant]] PtPush push;
[[vk::binding(0, 0)]] SamplerCube envMap;
[[vk::binding(1, 0)]] RaytracingAccelerationStructure sceneAs;
[[vk::binding(2, 0)]] RWTexture2D<float4> accum;     // radiance sum, sample count
[[vk::binding(3, 0)]] RWTexture2D<float4> albedoSum; // first hit albedo sum, coverage sum
[[vk::binding(4, 0)]] RWTexture2D<float4> normalSum; // first hit normal sum, first sample's depth along the view
[[vk::binding(5, 0)]] RWTexture2D<float4> outColor;  // display-linear, straight alpha
[[vk::binding(6, 0)]] RWTexture2D<float> outDepth;   // cm along the view, 0 where nothing was hit
[[vk::binding(0, 1)]] Sampler2D materialTextures[];

static const float PI = 3.14159265;
static const uint kBaseColorMap = 1 << 0;
static const uint kNormalMap = 1 << 1;
static const uint kNormalGreenUp = 1 << 2;
static const uint kMetallicMap = 1 << 3;
static const uint kRoughnessMap = 1 << 4;
static const uint kOcclusionMap = 1 << 5;
static const uint kEmissiveMap = 1 << 6;
static const uint kMasked = 1 << 7;
static const uint kTranslucent = 1 << 8;
static const uint kUv1 = 1 << 10;
static const uint kNormalXY = 1 << 17;
static const uint kGreyBase = 1 << 18;
static const uint kInstanceColor = 1 << 20;
static const uint kNoHandle = 0xffff;

/** The frame's parameters, loaded once by each entry point. */
static PtParams gP;
PtParams P() { return gP; }

// ---------------------------------------------------------------- random numbers

uint hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

float rand(inout uint state)
{
    state = hash(state);
    return float(state >> 8) / 16777216.0;
}

// ---------------------------------------------------------------- textures (the mesh shader's table, sampled at level 0)

static const uint kUdimEntry = 0x80000000;
static const uint kVtEntry = 0x40000000;
static const uint kVtRepeat = 0x20000000;
static const uint kVtPage = 128;
static const uint kVtBorder = 4;
static const uint kVtSlot = 136;
static const uint kVtChunkSlots = 16;

uint* table() { return (uint*)P().textureTable; }

uint2 vtLevelSize(uint2 size, uint m) { return max(size >> m, uint2(1, 1)); }
uint2 vtPages(uint2 levelSize) { return (levelSize + kVtPage - 1) / kVtPage; }

float4 vtSample(uint h, float2 uv)
{
    uint* t = table();
    uint64_t a = uint64_t(t[h + 1]) | (uint64_t(t[h + 2]) << 32);
    uint* pt = (uint*)a;
    if ((t[h] & kVtRepeat) != 0)
        uv = float2(frac(uv.x), 1.0 - frac(1.0 - uv.y));
    int col = int(floor(uv.x));
    int row = int(floor(1.0 - uv.y));
    if (col < 0 || col >= 10 || row < 0 || row >= 10)
        return float4(0, 0, 0, 1);
    uint cell = uint(row) * 10 + uint(col);
    uint base = pt[cell * 4];
    if (base == 0)
        return float4(0, 0, 0, 1);
    base -= 1;
    uint2 size = uint2(pt[cell * 4 + 1], pt[cell * 4 + 2]);
    uint mips = pt[cell * 4 + 3];
    float2 f = float2(frac(uv.x), frac(uv.y));
    uint levelBase = base;
    for (uint m = 0; m < mips; ++m)
    {
        uint2 ls = vtLevelSize(size, m);
        uint2 pages = vtPages(ls);
        uint2 page = min(uint2(f * float2(ls)) / kVtPage, pages - 1);
        uint e = pt[levelBase + page.y * pages.x + page.x];
        if (e != 0)
        {
            uint slot = e & 0xffff;
            uint chunk = slot / (kVtChunkSlots * kVtChunkSlots);
            if (chunk >= (t[h] & 0xffff))
                return float4(0, 0, 0, 1);
            uint within = slot % (kVtChunkSlots * kVtChunkSlots);
            float2 origin = float2(float(within % kVtChunkSlots), float(within / kVtChunkSlots)) * float(kVtSlot) + float(kVtBorder);
            float2 p = origin + f * float2(ls) - float2(page * kVtPage);
            return materialTextures[NonUniformResourceIndex(t[h + 4 + chunk])].SampleLevel(p / float(kVtSlot * kVtChunkSlots), 0.0);
        }
        levelBase += pages.x * pages.y;
    }
    return float4(0, 0, 0, 1);
}

float wrapAxis(float x, uint mode, float halfTexel, inout bool outside)
{
    if (mode == 1)
        return clamp(x, halfTexel, 1.0 - halfTexel);
    if (mode == 2)
    {
        float m = x - 2.0 * floor(0.5 * x);
        return m > 1.0 ? 2.0 - m : m;
    }
    if (mode == 3 && (x < 0.0 || x > 1.0))
        outside = true;
    return x;
}

/** A map through its 16-bit table handle at uv (rows down), with the slot's wrap and filter bits. */
float4 sampleHandle(uint h, uint mode, float2 uv)
{
    uint head = table()[h];
    if ((head & kUdimEntry) == 0 && (head & kVtEntry) != 0)
        return vtSample(h, uv);
    if ((head & kUdimEntry) != 0)
    {
        int col = int(floor(uv.x));
        int row = int(floor(1.0 - uv.y));
        uint index = (col < 0 || col >= 10 || row < 0 || row >= int(head & 0xffff)) ? table()[h + 1] : table()[h + 2 + uint(row) * 10 + uint(col)];
        return materialTextures[NonUniformResourceIndex(index)].SampleLevel(uv, 0.0);
    }
    Sampler2D tex = materialTextures[NonUniformResourceIndex(head)];
    if (mode == 0)
        return tex.SampleLevel(uv, 0.0);
    uint w, hh;
    tex.GetDimensions(w, hh);
    float2 size = float2(max(w, 1u), max(hh, 1u));
    bool outside = false;
    float2 st = float2(wrapAxis(uv.x, mode & 3, 0.5 / size.x, outside), wrapAxis(uv.y, (mode >> 2) & 3, 0.5 / size.y, outside));
    if (outside)
        return float4(0.0);
    if ((mode & 16) != 0)
        st = (floor(st * size) + 0.5) / size;
    return tex.SampleLevel(st, 0.0);
}

/** Map slot k of a section: 0 base colour, 1 normal, 2 ORM, 3 emissive, then the extended maps. */
float4 sampleMap(PtSection s, uint k, float2 uv)
{
    uint packed = s.maps[k >> 1];
    uint h = (packed >> ((k & 1) * 16)) & 0xffff;
    if (h == kNoHandle)
        return float4(-1.0);
    return sampleHandle(h, k < 4 ? (s.sampling >> (k * 8)) & 0xff : 0, uv);
}

// ---------------------------------------------------------------- environment

float3 toEnv(float3 d)
{
    PtParams p = P();
    if (p.envExtra.x > 0.5)
        d = float3(d.x, d.z, -d.y);
    float c = p.env.x, s = p.env.y;
    return float3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

float3 fromEnv(float3 e)
{
    PtParams p = P();
    float c = p.env.x, s = p.env.y;
    float3 d = float3(c * e.x - s * e.z, e.y, s * e.x + c * e.z);
    if (p.envExtra.x > 0.5)
        d = float3(d.x, -d.z, d.y);
    return d;
}

float3 envRadiance(float3 dir)
{
    PtParams p = P();
    float3 c = envMap.SampleLevel(toEnv(dir), 0.0).rgb;
    c = lerp(dot(c, float3(0.2126, 0.7152, 0.0722)).xxx, c, p.env.w);
    return c * p.env.z;
}

/** Cube face coordinates of an environment-space direction: face (Vulkan order), s and t in [-1, 1]. */
void faceOf(float3 d, out uint face, out float2 st)
{
    float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z)
    {
        face = d.x > 0.0 ? 0 : 1;
        st = d.x > 0.0 ? float2(-d.z, -d.y) / a.x : float2(d.z, -d.y) / a.x;
    }
    else if (a.y >= a.z)
    {
        face = d.y > 0.0 ? 2 : 3;
        st = d.y > 0.0 ? float2(d.x, d.z) / a.y : float2(d.x, -d.z) / a.y;
    }
    else
    {
        face = d.z > 0.0 ? 4 : 5;
        st = d.z > 0.0 ? float2(d.x, -d.y) / a.z : float2(-d.x, -d.y) / a.z;
    }
}

float3 dirOf(uint face, float2 st)
{
    float s = st.x, t = st.y;
    switch (face)
    {
    case 0: return normalize(float3(1.0, -t, -s));
    case 1: return normalize(float3(-1.0, -t, s));
    case 2: return normalize(float3(s, 1.0, t));
    case 3: return normalize(float3(s, -1.0, -t));
    case 4: return normalize(float3(s, -t, 1.0));
    default: return normalize(float3(-s, -t, -1.0));
    }
}

/** Solid angle density of picking a direction in the environment table's distribution. */
float envPdf(float3 worldDir)
{
    PtParams p = P();
    uint cells = p.counts2.z, res = p.counts2.w;
    if (cells == 0)
        return 0.0;
    uint face;
    float2 st;
    faceOf(toEnv(worldDir), face, st);
    uint2 ij = min(uint2((st * 0.5 + 0.5) * float(res)), uint2(res - 1, res - 1));
    float prob = ((float*)p.envTable)[cells + face * res * res + ij.y * res + ij.x];
    float cellArea = 4.0 / float(res * res);
    float j = 1.0 + dot(st, st);
    return prob / cellArea * j * sqrt(j);
}

/** A direction picked by the table (world space) and its density. */
float3 sampleEnv(float u0, float u1, float u2, out float pdf)
{
    PtParams p = P();
    uint cells = p.counts2.z, res = p.counts2.w;
    float* cdf = (float*)p.envTable;
    uint lo = 0, hi = cells - 1;
    while (lo < hi)
    {
        uint mid = (lo + hi) / 2;
        if (cdf[mid] < u0)
            lo = mid + 1;
        else
            hi = mid;
    }
    uint face = lo / (res * res);
    uint within = lo % (res * res);
    float2 st = (float2(float(within % res) + u1, float(within / res) + u2) / float(res)) * 2.0 - 1.0;
    float3 d = fromEnv(dirOf(face, st));
    pdf = envPdf(d);
    return d;
}

// ---------------------------------------------------------------- materials and shading

struct Hit
{
    float3 pos;
    float3 ng;  // geometric, facing the ray's origin
    float3 n;   // shading
    float3 t, b;
    float2 uv;
    float3 baseColor;
    float opacity;
    float metallic, rough, specular;
    float3 emissive;
    bool masked, translucent, unlit;
    float clip;
    float3 sheen;
    float sheenRough;
    float coat, coatRough;
    float aniso, anisoRot;
    float3 subsurface;
    float transmission;
    float3 specColor;
    float ior;
    float3 coatN;  // the coat's normal: the surface's, or its coat normal map's
    float3 anisoT; // the direction the highlight stretches along
};

uint sectionOf(PtInstance inst, uint prim)
{
    PtSection* sections = (PtSection*)P().sections;
    for (uint k = 0; k < inst.sectionCount; ++k)
    {
        PtSection s = sections[inst.firstSection + k];
        if (prim >= s.firstTriangle && prim < s.firstTriangle + s.triangleCount)
            return inst.firstSection + k;
    }
    return inst.firstSection;
}

#include "pathtrace_channels.slang"

float3x3 cofactor(float3x3 m) { return float3x3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1])); }

Hit hitOf(uint instanceIndex, uint prim, float2 bary, float3 rayDir, float rayT, float3 rayOrigin)
{
    PtInstance inst = ((PtInstance*)P().instances)[instanceIndex];
    PtSection s = ((PtSection*)P().sections)[sectionOf(inst, prim)];
    uint* idx = (uint*)inst.indices;
    float w[3] = { 1.0 - bary.x - bary.y, bary.x, bary.y };
    float3 p[3];
    float3 nn = float3(0, 0, 0);
    float2 uv = float2(0, 0);
    float4 tg = float4(0, 0, 0, 0);
    const bool uv1 = (s.flags & kUv1) != 0;
    for (uint k = 0; k < 3; ++k)
    {
        float* v = (float*)(inst.vertices + uint64_t(idx[prim * 3 + k]) * 64);
        p[k] = float3(v[0], v[1], v[2]);
        nn += float3(v[3], v[4], v[5]) * w[k];
        uv += (uv1 ? float2(v[8], v[9]) : float2(v[6], v[7])) * w[k];
        tg += float4(v[10], v[11], v[12], v[13]) * w[k];
    }
    float3x3 m = float3x3(inst.rows[0].xyz, inst.rows[1].xyz, inst.rows[2].xyz);
    float3x3 cof = cofactor(m);
    Hit h;
    h.pos = rayOrigin + rayDir * rayT;
    h.ng = normalize(mul(cof, cross(p[1] - p[0], p[2] - p[0])) + 1e-12);
    h.n = normalize(mul(cof, nn) + 1e-12);
    float3 tw = mul(m, tg.xyz);
    if (determinant(m) < 0.0)
        tg.w = -tg.w;
    // Front faces see the ray come in against their geometric normal; the shading normal follows it to the same side.
    if (dot(h.ng, rayDir) > 0.0)
    {
        h.ng = -h.ng;
        h.n = -h.n;
    }
    if (dot(h.n, h.ng) < 0.0)
        h.n = normalize(h.n - h.ng * (2.0 * dot(h.n, h.ng)));
    h.t = normalize(tw - h.n * dot(h.n, tw) + 1e-7);
    h.b = cross(h.n, h.t) * (tg.w < 0.0 ? -1.0 : 1.0);
    // Image rows run down and Lila's uv up.
    h.uv = float2(uv.x, 1.0 - uv.y);

    h.baseColor = s.base.rgb;
    h.opacity = s.surface.z;
    if ((s.flags & kBaseColorMap) != 0)
    {
        float4 c = sampleMap(s, 0, h.uv);
        if (c.a >= 0.0)
        {
            if ((s.flags & kGreyBase) == 0)
                h.baseColor = c.rgb;
            h.opacity *= c.a;
        }
    }
    if ((s.flags & kInstanceColor) != 0)
    {
        const float4 tint = float4(f16tof32(inst.tint.x), f16tof32(inst.tint.x >> 16), f16tof32(inst.tint.y), f16tof32(inst.tint.y >> 16));
        h.baseColor = lerp(h.baseColor, (s.flags & kBaseColorMap) != 0 ? h.baseColor * tint.rgb : tint.rgb, saturate(tint.a));
    }
    h.masked = (s.flags & kMasked) != 0;
    h.translucent = (s.flags & kTranslucent) != 0;
    h.clip = s.surface.w;
    h.unlit = uint(s.emissive.w + 0.5) == 4;
    if ((s.flags & kNormalMap) != 0)
    {
        float4 tn4 = sampleMap(s, 1, h.uv);
        if (tn4.a >= 0.0)
        {
            float3 tn = tn4.xyz * 2.0 - 1.0;
            if ((s.flags & kNormalXY) != 0)
                tn.z = sqrt(saturate(1.0 - dot(tn.xy, tn.xy)));
            if ((s.flags & kNormalGreenUp) == 0)
                tn.y = -tn.y;
            float3 n = normalize(h.t * tn.x + h.b * tn.y + h.n * max(tn.z, 1e-3));
            // A normal turned below the surface would light from inside it.
            if (dot(n, h.ng) > 0.02)
                h.n = n;
        }
    }
    h.metallic = s.surface.x;
    h.rough = s.base.a;
    h.specular = s.surface.y;
    if ((s.flags & (kMetallicMap | kRoughnessMap)) != 0)
    {
        float4 orm = sampleMap(s, 2, h.uv);
        if (orm.a >= 0.0)
        {
            if ((s.flags & kRoughnessMap) != 0)
                h.rough = orm.g;
            if ((s.flags & kMetallicMap) != 0)
                h.metallic = orm.b;
        }
    }
    h.rough = clamp(h.rough, 0.02, 1.0);
    h.metallic = saturate(h.metallic);
    h.emissive = s.emissive.rgb;
    if ((s.flags & kEmissiveMap) != 0)
    {
        float4 e = sampleMap(s, 3, h.uv);
        if (e.a >= 0.0)
            h.emissive *= e.rgb;
    }
    h.sheen = s.ext0.rgb;
    h.sheenRough = clamp(s.ext0.w, 0.05, 1.0);
    h.coat = saturate(s.ext1.x);
    h.coatRough = clamp(s.ext1.y, 0.02, 1.0);
    h.aniso = clamp(s.ext1.z, -0.95, 0.95);
    h.anisoRot = s.ext1.w;
    h.subsurface = s.ext2.rgb;
    h.transmission = saturate(s.ext2.w);
    h.specColor = s.ext3.rgb;
    h.ior = max(s.ext3.w, 1.0);
    applyExtendedMaps(s, h);
    return h;
}


float D_GGX(float nh, float a)
{
    float a2 = a * a;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float G1(float nx, float a)
{
    float a2 = a * a;
    return 2.0 * nx / (nx + sqrt(a2 + (1.0 - a2) * nx * nx));
}

float V_SmithGGX(float nv, float nl, float a)
{
    float a2 = a * a;
    float gv = nl * sqrt(nv * nv * (1.0 - a2) + a2);
    float gl = nv * sqrt(nl * nl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-7);
}

/** Unreal's Schlick: reflectance below 2% fades the grazing rise too, so a specular of 0 reflects nothing. */
float3 fresnel(float3 f0, float vh)
{
    float fc = pow(1.0 - vh, 5.0);
    return f0 + (saturate(50.0 * f0.g) - f0) * fc;
}

struct Lobes
{
    float3 diffuse; // albedo
    float3 f0;
    float alpha;
    float pSpec;
};

Lobes lobesOf(Hit h, float nv)
{
    Lobes l;
    l.diffuse = h.baseColor * (1.0 - h.metallic) * (1.0 - h.transmission) * (1.0 - subsurfaceShare(h));
    // Specular 0.5 at IOR 1.5 is Unreal's 4% reflectance.
    float r = (h.ior - 1.0) / (h.ior + 1.0);
    l.f0 = lerp(r * r * 2.0 * h.specular * h.specColor, h.baseColor, h.metallic);
    l.alpha = h.rough * h.rough;
    float3 F = fresnel(l.f0, nv);
    float ws = dot(F, float3(0.333, 0.333, 0.334)) + h.coat * 0.04, wd = dot(l.diffuse * (1.0 - F), float3(0.333, 0.333, 0.334));
    l.pSpec = ws + wd > 0.0 ? clamp(ws / (ws + wd), 0.1, 0.9) : 0.5;
    if (ws <= 0.0)
        l.pSpec = 0.0;
    if (wd <= 0.0 && ws > 0.0)
        l.pSpec = 1.0;
    return l;
}

/** The BSDF times the cosine for light from l, and the density of sampleBsdf picking l. */
float3 evalBsdf(Hit h, Lobes lb, float3 v, float3 l, out float pdf)
{
    pdf = 0.0;
    float3 n = h.n;
    float nl = dot(n, l), nv = max(dot(n, v), 1e-4);
    if (nl <= 0.0 || dot(h.ng, l) <= 0.0)
        return float3(0.0);
    float3 hv = normalize(v + l);
    float nh = saturate(dot(n, hv)), vh = saturate(dot(v, hv));
    float3 F = fresnel(lb.f0, vh);
    float D = D_GGX(nh, lb.alpha);
    float3 spec = D * V_SmithGGX(nv, nl, lb.alpha) * F;
    float3 diff = (1.0 - F) * lb.diffuse / PI;
    float3 f = extendedCombine(h, lb, n, v, l, hv, nl, nv, nh, vh, spec, diff);
    float pdfSpec = D * G1(nv, lb.alpha) / (4.0 * nv);
    float pdfDiff = nl / PI;
    pdf = lb.pSpec * pdfSpec + (1.0 - lb.pSpec) * pdfDiff;
    return f * nl;
}

/** Heitz's visible normal sampling for GGX, in the frame (t, b, n). */
float3 sampleVndf(float3 v, float a, float u1, float u2)
{
    float3 vh = normalize(float3(a * v.x, a * v.y, v.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    float3 T1 = lensq > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(vh, T1);
    float r = sqrt(u1);
    float phi = 2.0 * PI * u2;
    float t1 = r * cos(phi), t2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;
    float3 nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * vh;
    return normalize(float3(a * nh.x, a * nh.y, max(0.0, nh.z)));
}

float3 sampleBsdf(Hit h, Lobes lb, float3 v, inout uint rng)
{
    float u0 = rand(rng), u1 = rand(rng), u2 = rand(rng);
    float3 t = normalize(h.t - h.n * dot(h.n, h.t) + 1e-7), b = cross(h.n, t);
    if (u0 < lb.pSpec)
    {
        float3 vl = float3(dot(v, t), dot(v, b), dot(v, h.n));
        float3 m = sampleVndf(vl, lb.alpha, u1, u2);
        float3 hw = t * m.x + b * m.y + h.n * m.z;
        return reflect(-v, hw);
    }
    float r = sqrt(u1), phi = 2.0 * PI * u2;
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + h.n * sqrt(max(1.0 - u1, 0.0)));
}

#include "atmosphere_common.slang"
#include "volume_common.slang"

// ---------------------------------------------------------------- frame header

/** The light buffer's header; matches render::GpuFrame (mesh.slang's). */
struct PtFrame
{
    float4 fog[6];
    GpuAtmosphere* atmosphere;
    float4* aerial;
    float4 aerialParams; // x depth of the last slice (km), y cells across, z slices
    float4* volumeOut;
    uint volumeWidth, volumeHeight;
    GpuVolume* volumes;
    uint volumeCount, volumePad;
    float4 pad[1];
};

PtFrame* frameHeader() { return (PtFrame*)P().lights; }

/** The sky's haze between the camera and a first hit at dist cm along dir through the pixel at ndc, from the froxels. */
float3 aerialOver(float3 c, float2 ndc, float3 dir, float dist)
{
    PtFrame* fr = frameHeader();
    if (fr == nullptr || fr->aerial == nullptr)
        return c;
    float origin = atmAerialOrigin(fr->atmosphere, dir);
    if (origin < 0.0)
        return c;
    const float cells = fr->aerialParams.y, slices = fr->aerialParams.z;
    float2 uv = ndc * 0.5 + 0.5;
    float km = max(dist * 1e-5 - origin, 0.0);
    float3 at = float3(clamp(uv * cells - 0.5, 0.0, cells - 1.0), min(sqrt(km / max(fr->aerialParams.x, 1e-6)) * slices - 1.0, slices - 1.0));
    int3 base = int3(floor(at));
    float3 f = at - float3(base);
    float3 light = float3(0.0), transmit = float3(0.0);
    for (uint k = 0; k < 8; ++k)
    {
        int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        float w = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y) * (o.z != 0 ? f.z : 1.0 - f.z);
        int3 cell = min(base + o, int3(int(cells) - 1, int(cells) - 1, int(slices) - 1));
        if (cell.z < 0)
        {
            transmit += w;
            continue;
        }
        uint i = uint((cell.z * int(cells) + cell.y) * int(cells) + cell.x) * 2;
        light += fr->aerial[i].rgb * w;
        transmit += fr->aerial[i + 1].rgb * w;
    }
    return c * transmit + light;
}

/** A collision with a volume along the ray before maxT, by delta tracking against each volume's majorant. */
bool volumeCollision(float3 origin, float3 dir, float maxT, inout uint rng, out float hitT, out uint hitVolume, out float hitDensity)
{
    hitT = maxT;
    hitVolume = 0;
    hitDensity = 0.0;
    PtFrame* fr = frameHeader();
    if (fr == nullptr || fr->volumes == nullptr)
        return false;
    bool hit = false;
    for (uint vi = 0; vi < fr->volumeCount; ++vi)
    {
        GpuVolume* v = fr->volumes + vi;
        float majorant = v->dims.w;
        if (majorant <= 0.0)
            continue;
        float3 o = mul(v->worldToIndex, float4(origin, 1.0)).xyz;
        float3 d = mul((float3x3)v->worldToIndex, dir);
        float2 r = volBoxRange(o, d, v->dims.xyz);
        float t = max(r.x, 0.0);
        float end = min(r.y, hitT);
        float sigma = volSigma(v);
        for (uint step = 0; step < 256 && t < end; ++step)
        {
            t -= log(max(1.0 - rand(rng), 1e-7)) / majorant;
            if (t >= end)
                break;
            float dens = volDensity(v, o + d * t);
            if (rand(rng) < dens * sigma / majorant)
            {
                hitT = t;
                hitVolume = vi;
                hitDensity = dens;
                hit = true;
                break;
            }
        }
    }
    return hit;
}

/** The nearest volume box the ray enters before maxT with any density in it, and where; false for none. */
bool volumeEntry(float3 origin, float3 dir, float maxT, out float entry, out uint volume)
{
    entry = maxT;
    volume = 0;
    PtFrame* fr = frameHeader();
    if (fr == nullptr || fr->volumes == nullptr)
        return false;
    bool found = false;
    for (uint vi = 0; vi < fr->volumeCount; ++vi)
    {
        GpuVolume* v = fr->volumes + vi;
        if (v->dims.w <= 0.0)
            continue;
        float3 o = mul(v->worldToIndex, float4(origin, 1.0)).xyz;
        float3 d = mul((float3x3)v->worldToIndex, dir);
        float2 r = volBoxRange(o, d, v->dims.xyz);
        if (r.y > max(r.x, 0.0) && max(r.x, 0.0) < entry)
        {
            entry = max(r.x, 0.0);
            volume = vi;
            found = true;
        }
    }
    return found;
}

/** The share of light crossing the scene's volumes along a ray, by ratio tracking. */
float volumeTransmit(float3 origin, float3 dir, float maxT, inout uint rng)
{
    PtFrame* fr = frameHeader();
    if (fr == nullptr || fr->volumes == nullptr)
        return 1.0;
    float transmit = 1.0;
    for (uint vi = 0; vi < fr->volumeCount; ++vi)
    {
        GpuVolume* v = fr->volumes + vi;
        float majorant = v->dims.w;
        if (majorant <= 0.0)
            continue;
        float3 o = mul(v->worldToIndex, float4(origin, 1.0)).xyz;
        float3 d = mul((float3x3)v->worldToIndex, dir);
        float2 r = volBoxRange(o, d, v->dims.xyz);
        float t = max(r.x, 0.0);
        float end = min(r.y, maxT);
        float sigma = volSigma(v);
        for (uint step = 0; step < 128 && t < end && transmit > 1e-3; ++step)
        {
            t -= log(max(1.0 - rand(rng), 1e-7)) / majorant;
            if (t >= end)
                break;
            transmit *= 1.0 - min(volDensity(v, o + d * t) * sigma / majorant, 1.0);
        }
    }
    return transmit;
}

// ---------------------------------------------------------------- lights

/** Matches GpuLight in mesh.slang. */
struct GpuLight
{
    float4 position;
    float4 direction;
    float4 radiance;
    float4 cone;
    float4 right;
    float4x4 shadow;
    float4 shadowRect;
    float4 shadowDepth;
};

bool visible(float3 from, float3 dir, float dist)
{
    RayDesc ray;
    ray.Origin = from;
    ray.Direction = dir;
    ray.TMin = 0.0;
    ray.TMax = dist;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(sceneAs, RAY_FLAG_NONE, 0xff, ray);
    q.Proceed();
    return q.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

/** One of the scene's lights picked at random: its light at a point in a volume, through the phase function, shadowed. */
float3 sampleLightInVolume(float3 origin, float3 dirIn, float g, inout uint rng)
{
    PtParams p = P();
    uint count = p.counts2.x;
    if (count == 0)
        return float3(0.0);
    uint k = min(uint(rand(rng) * float(count)), count - 1);
    GpuLight l = ((GpuLight*)p.lights)[k + 1];
    uint kind = uint(l.position.w + 0.5) & 15;
    float3 L;
    float dist = 1e9;
    float atten = 1.0;
    if (kind == 2)
        L = -l.direction.xyz;
    else
    {
        float3 toLight = l.position.xyz - origin;
        float d2 = max(dot(toLight, toLight), 1e-4);
        dist = sqrt(d2);
        L = toLight / dist;
        atten = 1.0 / max(d2 * 1e-4, 1e-4);
        if (l.direction.w > 0.0)
        {
            float r = dist / l.direction.w;
            float window = saturate(1.0 - r * r * r * r);
            atten *= window * window;
        }
        if (kind == 1)
        {
            float tt = saturate((dot(-L, l.direction.xyz) - l.cone.x) * l.cone.y);
            atten *= tt * tt;
        }
        else if (kind >= 3)
            atten *= saturate(dot(-L, l.direction.xyz));
    }
    if (atten <= 0.0 || !visible(origin, L, dist * 0.999))
        return float3(0.0);
    return l.radiance.rgb * atten * volPhase(g, dot(dirIn, L)) * volumeTransmit(origin, L, dist, rng) * float(count);
}

/** One of the scene's lights picked at random: its light on the hit, shadowed. */
float3 sampleLight(Hit h, Lobes lb, float3 v, float3 origin, inout uint rng)
{
    PtParams p = P();
    uint count = p.counts2.x;
    if (count == 0)
        return float3(0.0);
    uint k = min(uint(rand(rng) * float(count)), count - 1);
    GpuLight l = ((GpuLight*)p.lights)[k + 1];
    uint kind = uint(l.position.w + 0.5) & 15;
    float3 L;
    float dist = 1e9;
    float atten = 1.0;
    if (kind == 2)
    {
        // The sun: a cone of its angular radius (the light's radius as degrees, a quarter degree by default).
        float3 c = -l.direction.xyz;
        float angle = radians(l.radiance.w > 0.0 ? min(l.radiance.w, 10.0) : 0.27);
        float cosMax = cos(angle);
        float u1 = rand(rng), u2 = rand(rng);
        float cosT = 1.0 - u1 * (1.0 - cosMax), sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
        float3 t = abs(c.y) < 0.99 ? normalize(cross(float3(0, 1, 0), c)) : normalize(cross(float3(1, 0, 0), c));
        float3 b = cross(c, t);
        L = normalize(c * cosT + (t * cos(2.0 * PI * u2) + b * sin(2.0 * PI * u2)) * sinT);
    }
    else
    {
        float3 source = l.position.xyz;
        if (kind >= 3)
        {
            float3 right = l.right.xyz, up = cross(-l.direction.xyz, right);
            float2 q = float2(rand(rng) - 0.5, rand(rng) - 0.5);
            if (kind == 4)
            {
                float r = sqrt(rand(rng)) * 0.5 * l.cone.z, phi = 2.0 * PI * rand(rng);
                q = float2(r * cos(phi), r * sin(phi));
            }
            else
                q *= l.cone.zw;
            source += right * q.x + up * q.y;
        }
        else if (l.radiance.w > 0.0)
        {
            // A sized point light: a point of its sphere.
            float z = 1.0 - 2.0 * rand(rng), phi = 2.0 * PI * rand(rng), rr = sqrt(max(0.0, 1.0 - z * z));
            source += float3(rr * cos(phi), rr * sin(phi), z) * l.radiance.w;
        }
        float3 toLight = source - origin;
        float d2 = max(dot(toLight, toLight), 1e-4);
        dist = sqrt(d2);
        L = toLight / dist;
        // Inverse square in metres, as the viewport lights.
        atten = 1.0 / max(d2 * 1e-4, 1e-4);
        if (l.direction.w > 0.0)
        {
            float r = dist / l.direction.w;
            float window = saturate(1.0 - r * r * r * r);
            atten *= window * window;
        }
        if (kind == 1)
        {
            float tt = saturate((dot(-L, l.direction.xyz) - l.cone.x) * l.cone.y);
            atten *= tt * tt;
        }
        else if (kind >= 3)
            atten *= saturate(dot(-L, l.direction.xyz));
    }
    if (atten <= 0.0)
        return float3(0.0);
    float pdf;
    float3 f = evalBsdf(h, lb, v, L, pdf);
    if (all(f <= 0.0))
        return float3(0.0);
    if (!visible(origin, L, dist * 0.999))
        return float3(0.0);
    return f * l.radiance.rgb * atten * volumeTransmit(origin, L, dist, rng) * float(count);
}

// ---------------------------------------------------------------- tracing

/** The closest surface along the ray that isn't cut out (masked) or passed through (translucent, at random). */
bool traceSurface(inout RayDesc ray, inout uint rng, out Hit h)
{
    for (uint pass = 0; pass < 16; ++pass)
    {
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(sceneAs, RAY_FLAG_NONE, 0xff, ray);
        q.Proceed();
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            return false;
        h = hitOf(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), ray.Direction, q.CommittedRayT(),
                  ray.Origin);
        bool through = (h.masked && h.opacity < h.clip) || (h.translucent && rand(rng) >= saturate(h.opacity));
        if (!through)
            return true;
        ray.Origin = h.pos + ray.Direction * max(1e-3, length(h.pos) * 1e-6);
        ray.TMin = 0.0;
    }
    return false;
}

float3 offsetFrom(Hit h, float3 dir)
{
    float eps = max(1e-3, length(h.pos) * 2e-6);
    return h.pos + h.ng * (dot(dir, h.ng) >= 0.0 ? eps : -eps);
}

float mis(float a, float b) { return a * a / max(a * a + b * b, 1e-20); }

[shader("compute")]
[numthreads(8, 8, 1)]
void traceCS(uint3 id: SV_DispatchThreadID)
{
    gP = *(PtParams*)push.params;
    PtParams p = gP;
    int2 px = int2(id.xy);
    if (any(px >= int2(p.size.xy)))
        return;
    float4 sum = p.counts.x == 0 ? float4(0.0) : accum[px];
    float4 alb = p.counts.x == 0 ? float4(0.0) : albedoSum[px];
    float4 nrm = p.counts.x == 0 ? float4(0.0) : normalSum[px];
    const uint bounces = max(p.counts.z, 1u);
    const bool envOk = p.counts2.z > 0;
    for (uint si = 0; si < p.counts.y; ++si)
    {
        const uint sampleIndex = p.counts.x + si;
        uint rng = hash(uint(px.x) * 1973u + uint(px.y) * 9277u + sampleIndex * 26699u + p.counts.w * 104729u) | 1u;
        // The first sample goes through the pixel centre, so the depth it leaves stays put.
        float2 jitter = sampleIndex == 0 ? float2(0.5, 0.5) : float2(rand(rng), rand(rng));
        float2 ndc = (float2(px) + jitter) * p.size.zw * 2.0 - 1.0;
        float4 nearH = mul(p.invViewProj, float4(ndc, 1.0, 1.0));
        float4 farH = mul(p.invViewProj, float4(ndc, 0.001, 1.0));
        float3 nearP = nearH.xyz / nearH.w;
        float3 dir = normalize(farH.xyz / farH.w - nearP);
        float3 origin = p.eye.w > 0.5 ? nearP : p.eye.xyz;
        if (p.camRight.w > 0.0 && p.eye.w < 0.5 && sampleIndex > 0)
        {
            // Depth of field: from a point of the aperture through the pixel's point on the focus plane.
            float3 focus = origin + dir * (p.camUp.w / max(dot(dir, p.camForward.xyz), 1e-4));
            float r = sqrt(rand(rng)) * p.camRight.w, phi = 2.0 * PI * rand(rng);
            origin += p.camRight.xyz * (r * cos(phi)) + p.camUp.xyz * (r * sin(phi));
            dir = normalize(focus - origin);
        }
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = dir;
        ray.TMin = 0.0;
        ray.TMax = 1e30;
        float3 radiance = float3(0.0);
        float3 throughput = float3(1.0);
        float lastPdf = 0.0; // the BSDF density of the last bounce's direction; 0 after the camera ray
        float coverage = 0.0;
        float firstHit = -1.0;
        bool guided = false;
        for (uint depth = 0; depth <= bounces; ++depth)
        {
            Hit h;
            const bool surface = traceSurface(ray, rng, h);
            float ge;
            uint gv;
            if (depth == 0 && volumeEntry(ray.Origin, ray.Direction, surface ? length(h.pos - ray.Origin) : 1e30, ge, gv))
            {
                // The denoiser's guides for a camera ray into a volume, whether it collides or passes through: the medium's
                // albedo, a normal facing the camera and the box's entry depth, alike across the volume so neighbours blend.
                GpuVolume* gvol = frameHeader()->volumes + gv;
                alb.rgb += gvol->scatter.rgb / max(gvol->scatter.rgb + gvol->absorb.rgb, 1e-8);
                nrm.xyz -= ray.Direction;
                if (sampleIndex == 0)
                    nrm.w = ge * dot(ray.Direction, p.camForward.xyz);
                guided = true;
            }
            float vt;
            uint vi;
            float vd;
            if (volumeCollision(ray.Origin, ray.Direction, surface ? length(h.pos - ray.Origin) : 1e30, rng, vt, vi, vd))
            {
                // A collision in a volume: its own light, then the sun and sky through its phase function, then on in a new direction.
                GpuVolume* vol = frameHeader()->volumes + vi;
                float3 X = ray.Origin + ray.Direction * vt;
                float sigma = volSigma(vol) * vd;
                if (depth == 0)
                {
                    coverage = 1.0;
                    if (firstHit < 0.0)
                        firstHit = vt;
                }
                radiance += throughput * vol->emission.rgb * vd / max(sigma, 1e-8);
                throughput *= vol->scatter.rgb * vd / max(sigma, 1e-8);
                if (depth == bounces)
                    break;
                float g = vol->scatter.w;
                if (envOk)
                {
                    float lp;
                    float3 L = sampleEnv(rand(rng), rand(rng), rand(rng), lp);
                    float pp = volPhase(g, dot(ray.Direction, L));
                    if (lp > 0.0 && visible(X, L, 1e30))
                    {
                        float3 c = throughput * envRadiance(L) * pp * volumeTransmit(X, L, 1e30, rng) * (mis(lp, pp) / lp);
                        radiance += depth > 0 ? min(c, p.misc.y) : c;
                    }
                }
                {
                    float3 c = throughput * sampleLightInVolume(X, ray.Direction, g, rng);
                    radiance += depth > 0 ? min(c, p.misc.y) : c;
                }
                float3 next = volSamplePhase(ray.Direction, g, rand(rng), rand(rng));
                lastPdf = volPhase(g, dot(ray.Direction, next));
                if (depth >= 2)
                {
                    float survive = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
                    if (rand(rng) >= survive)
                        break;
                    throughput /= survive;
                }
                ray.Origin = X;
                ray.Direction = next;
                ray.TMin = 0.0;
                ray.TMax = 1e30;
                continue;
            }
            if (!surface)
            {
                if (depth == 0)
                {
                    PtFrame* fr = frameHeader();
                    if (p.envExtra.z > 0.5 && p.misc.z < 0.5)
                    {
                        // A sky atmosphere draws its own sky behind, sun disk and all, as the viewport does.
                        radiance += fr != nullptr && fr->atmosphere != nullptr ? atmSkyColor(fr->atmosphere, ray.Direction)
                                                                               : envRadiance(ray.Direction) * p.envExtra.y;
                        coverage = 1.0;
                    }
                    if (sampleIndex == 0 && !guided)
                        nrm.w = 0.0;
                }
                else
                {
                    float w = envOk && lastPdf > 0.0 ? mis(lastPdf, envPdf(ray.Direction)) : 1.0;
                    float3 c = throughput * envRadiance(ray.Direction) * w;
                    radiance += depth > 1 ? min(c, p.misc.y) : c;
                }
                break;
            }
            float3 v = -ray.Direction;
            if (depth == 0)
            {
                firstHit = length(h.pos - ray.Origin);
                coverage = 1.0;
                if (!guided)
                {
                    alb.rgb += h.baseColor;
                    nrm.xyz += h.n;
                    if (sampleIndex == 0)
                        nrm.w = dot(h.pos - p.eye.xyz, p.camForward.xyz);
                }
            }
            float3 e = throughput * h.emissive;
            radiance += depth > 1 ? min(e, p.misc.y) : e;
            if (h.unlit)
            {
                radiance += throughput * h.baseColor;
                break;
            }
            if (depth == bounces)
                break;
            Lobes lb = lobesOf(h, max(dot(h.n, v), 1e-4));
            // Next event: the environment and one light.
            if (envOk)
            {
                float lp;
                float3 L = sampleEnv(rand(rng), rand(rng), rand(rng), lp);
                float bp;
                float3 f = evalBsdf(h, lb, v, L, bp);
                if (lp > 0.0 && any(f > 0.0) && visible(offsetFrom(h, L), L, 1e30))
                {
                    float3 c = throughput * f * envRadiance(L) * (mis(lp, bp) / lp);
                    radiance += depth > 0 ? min(c, p.misc.y) : c;
                }
            }
            {
                float3 c = throughput * sampleLight(h, lb, v, offsetFrom(h, h.ng), rng);
                radiance += depth > 0 ? min(c, p.misc.y) : c;
            }
            // Thin transmission and subsurface light pass through the surface; the other lobes take the rest of the paths.
            const float pT = h.transmission * (1.0 - h.metallic);
            const float pS = subsurfaceShare(h) * (1.0 - h.metallic);
            const float pick = rand(rng);
            if (pick < pT + pS)
            {
                float3 through;
                if (pick < pT)
                {
                    through = ray.Direction;
                    throughput *= h.baseColor;
                }
                else
                {
                    float u1 = rand(rng), u2 = rand(rng), rr = sqrt(u1), phi = 2.0 * PI * u2;
                    float3 tt = normalize(h.t - h.n * dot(h.n, h.t) + 1e-7), bb = cross(h.n, tt);
                    through = normalize(tt * (rr * cos(phi)) + bb * (rr * sin(phi)) - h.n * sqrt(max(1.0 - u1, 0.0)));
                    throughput *= h.subsurface / max(max(h.subsurface.r, max(h.subsurface.g, h.subsurface.b)), 1e-4);
                }
                lastPdf = 0.0;
                ray.Origin = offsetFrom(h, through);
                ray.Direction = through;
                ray.TMin = 0.0;
                ray.TMax = 1e30;
                continue;
            }
            throughput /= max(1.0 - pT - pS, 1e-4);
            float3 L = sampleBsdf(h, lb, v, rng);
            float pdf;
            float3 f = evalBsdf(h, lb, v, L, pdf);
            if (pdf <= 0.0 || all(f <= 0.0))
                break;
            throughput *= f / pdf;
            lastPdf = pdf;
            if (depth >= 2)
            {
                float survive = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
                if (rand(rng) >= survive)
                    break;
                throughput /= survive;
            }
            ray.Origin = offsetFrom(h, L);
            ray.Direction = L;
            ray.TMin = 0.0;
            ray.TMax = 1e30;
        }
        if (firstHit > 0.0)
            radiance = aerialOver(radiance, ndc, dir, firstHit);
        if (any(isnan(radiance)) || any(isinf(radiance)))
            radiance = float3(0.0);
        sum += float4(radiance, 1.0);
        alb.w += coverage;
    }
    accum[px] = sum;
    albedoSum[px] = alb;
    normalSum[px] = nrm;
}

float3 averageAt(int2 q)
{
    float covered = albedoSum[q].w;
    return covered > 0.0 ? accum[q].rgb / covered : float3(0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resolveCS(uint3 id: SV_DispatchThreadID)
{
    gP = *(PtParams*)push.params;
    PtParams p = gP;
    int2 px = int2(id.xy);
    int2 size = int2(p.size.xy);
    if (any(px >= size))
        return;
    float4 s = accum[px];
    float n = max(s.w, 1.0);
    float4 a = albedoSum[px];
    float4 nd = normalSum[px];
    float coverage = a.w / n;
    // Straight colour: what the covered samples saw, the background showing through the rest.
    float3 c = a.w > 0.0 ? s.rgb / a.w : float3(0.0);
    if (p.flags.x != 0 && nd.w > 0.0)
    {
        // Edge-aware: neighbours count by how alike their first hit's normal, albedo and depth are, and the radius
        // narrows as samples come in, so a converged image is left alone.
        int radius = int(clamp(6.0 / sqrt(n), 0.0, 3.0) + 0.5);
        if (radius > 0)
        {
            float3 nc = normalize(nd.xyz + 1e-6);
            float3 ac = a.rgb / n;
            float3 total = float3(0.0);
            float weight = 0.0;
            for (int y = -radius; y <= radius; ++y)
                for (int x = -radius; x <= radius; ++x)
                {
                    int2 q = clamp(px + int2(x, y), int2(0, 0), size - 1);
                    float4 nq = normalSum[q];
                    if (nq.w <= 0.0)
                        continue;
                    float4 aq4 = albedoSum[q];
                    float nqw = max(accum[q].w, 1.0);
                    float wn = pow(saturate(dot(nc, normalize(nq.xyz + 1e-6))), 32.0);
                    float wd = exp(-abs(nq.w - nd.w) / max(nd.w * 0.01, 1e-3));
                    float wa = exp(-length(aq4.rgb / nqw - ac) * 8.0);
                    float ws = exp(-float(x * x + y * y) / float(2 * radius * radius));
                    float w = wn * wd * wa * ws;
                    total += averageAt(q) * w;
                    weight += w;
                }
            if (weight > 0.0)
                c = total / weight;
        }
    }
    float3 d = tonemap(c * p.camForward.w, p.flags.y, p.flags.z);
    d = pow(max(d, 0.0), 1.0 / max(p.misc.x, 0.01));
    outColor[px] = float4(d, coverage);
    outDepth[px] = nd.w;
}
