// Heightfields drawn straight from their heights (render/HeightfieldRenderer.cpp): patches of a grid placed by a
// continuous level of detail, their vertices from the vertex index, heights read from the GPU copy of the grid and morphed
// toward the next coarser level with distance, so a terrain needs no mesh. The pixel shader shades with mesh.slang's
// lighting and the terrain's material, its paint layers over it, then the display overlays (mask, contours, flow, wire).
// heightfield_bindless.slang compiles this with LILA_BINDLESS, which the paint layers' maps need.

// Read by mesh.slang's shadeMesh under LILA_HEIGHTFIELD: the paint layers' blend over the material.
static float4 gTerrainBase = float4(0.0);   // rgb linear base colour, a its weight
static float gTerrainRough = 0.8;
static float4 gTerrainNormal = float4(0.0); // xyz world normal of the layers' maps, w its weight

#define LILA_HEIGHTFIELD 1
#include "mesh.slang"

/** One patch: kPatch by kPatch quads spacing samples apart from origin (samples), at a level of detail. */
struct HfPatch
{
    float2 origin;
    float spacing;
    uint lod;
};

/** Matches GpuLayer in HeightfieldRenderer.cpp. */
struct HfLayer
{
    float4 color;  // rgb linear, a roughness
    float4 tiling; // x: map repeats per cm
    uint* weights; // 8-bit weights, four per word
    uint baseMap;  // frame table handles, 0xffff for none
    uint normalMap;
    uint4 pad;
};

static const uint kMaxLods = 24;
static const uint kMaxLayers = 8;
static const uint kPatchQuads = 32;
static const uint kHfOverlayRampGrey = 0;
static const uint kHfFlagWire = 1;
static const uint kHfFlagArrows = 2;

/** Matches GpuParams in HeightfieldRenderer.cpp; pc.instances and pc.edgeMask both point at it. */
struct HfParams
{
    InstanceData instance; // world, and the wire colour (alpha 0 for none)
    float4 grid;           // samples along X, along Z, cell size, wire width in pixels
    float4 origin;         // grid origin X, Z, then 1 / (samples - 1) along X and Z
    float4 eye;            // the camera in the heightfield's space
    float4 extra;          // unused
    uint4 info;            // flags, layer count, overlay ramp, contour major
    float4 overlay;        // opacity
    float4 contour;        // interval (cm), opacity
    float4 flow;           // arrow spacing in pixels, 1 with a flow mask, opacity
    float4 lods[kMaxLods]; // morph start distance, 1 / morph length
    float* heights;
    HfPatch* patches;
    uint* overlayMask; // 8-bit, stretched to its range
    uint* flowMask;    // 8-bit
    HfLayer layers[kMaxLayers];
};

HfParams* hf() { return (HfParams*)pc.edgeMask; }

int2 hfSize()
{
    return int2(hf()->grid.xy);
}

float hfSample(int2 s)
{
    int2 n = hfSize();
    s = clamp(s, int2(0, 0), n - 1);
    return hf()->heights[s.y * n.x + s.x];
}

/** Bilinear height at a sample position. */
float hfHeight(float2 s)
{
    float2 f = floor(s);
    float2 t = s - f;
    int2 i = int2(f);
    float a = lerp(hfSample(i), hfSample(i + int2(1, 0)), t.x);
    float b = lerp(hfSample(i + int2(0, 1)), hfSample(i + int2(1, 1)), t.x);
    return lerp(a, b, t.y);
}

/** The heightfield-space normal at a sample position, from central differences step samples apart. */
float3 hfNormal(float2 s, float step)
{
    float cell = hf()->grid.z;
    float dx = (hfHeight(s + float2(step, 0)) - hfHeight(s - float2(step, 0))) / (2.0 * step * cell);
    float dz = (hfHeight(s + float2(0, step)) - hfHeight(s - float2(0, step))) / (2.0 * step * cell);
    return normalize(float3(-dx, 1.0, -dz));
}

/** hfNormal from the nearest samples alone: for footprints of a sample or more, where bilinear detail is under a pixel. */
float3 hfNormalNearest(float2 s, float step)
{
    float cell = hf()->grid.z;
    int2 i = int2(floor(s + 0.5));
    int k = int(step);
    float dx = (hfSample(i + int2(k, 0)) - hfSample(i - int2(k, 0))) / (2.0 * step * cell);
    float dz = (hfSample(i + int2(0, k)) - hfSample(i - int2(0, k))) / (2.0 * step * cell);
    return normalize(float3(-dx, 1.0, -dz));
}

/** An 8-bit grid value (0 to 1), bilinear. */
float hfByte(uint* bytes, int2 s)
{
    int2 n = hfSize();
    s = clamp(s, int2(0, 0), n - 1);
    uint k = uint(s.y * n.x + s.x);
    return float((bytes[k >> 2] >> ((k & 3) * 8)) & 0xff) / 255.0;
}

float hfMask(uint* bytes, float2 s)
{
    float2 f = floor(s);
    float2 t = s - f;
    int2 i = int2(f);
    float a = lerp(hfByte(bytes, i), hfByte(bytes, i + int2(1, 0)), t.x);
    float b = lerp(hfByte(bytes, i + int2(0, 1)), hfByte(bytes, i + int2(1, 1)), t.x);
    return lerp(a, b, t.y);
}

float3 hfToWorld(float3 p)
{
    float4x4 w = hf()->instance.world;
    return mul(w, float4(p, 1.0)).xyz;
}

/** A heightfield-space direction to the world, as a normal (the cofactor handles scale). */
float3 hfNormalToWorld(float3 n)
{
    return normalize(mul(cofactor((float3x3)hf()->instance.world), n));
}

struct HfOut
{
    float4 pos : SV_Position;
    centroid float3 worldPos : POSITION;
    float3 normal : NORMAL;
    centroid float4 tangent : TANGENT;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
HfOut heightfieldVS(uint vid: SV_VertexID, uint patchId: SV_InstanceID)
{
    HfParams* p = hf();
    HfPatch patch = p->patches[patchId];
    // The index buffer walks a (kPatchQuads + 1)-square grid of vertices, so each one is shaded once.
    int2 g = int2(int(vid % (kPatchQuads + 1)), int(vid / (kPatchQuads + 1)));
    int2 n = hfSize();
    float2 last = float2(n - 1);
    float2 s = min(patch.origin + float2(g) * patch.spacing, last);
    float cell = p->grid.z;
    float3 local = float3(p->origin.x + s.x * cell, hfHeight(s), p->origin.y + s.y * cell);
    // Odd vertices slide onto their even neighbours as the camera moves away, so the patch becomes the next level's
    // grid before that level takes over: no cracks or pops between levels.
    float4 lod = p->lods[patch.lod];
    float morph = saturate((distance(local, p->eye.xyz) - lod.x) * lod.y);
    float2 odd = frac(float2(g) * 0.5) * 2.0;
    s = min(max(s - odd * patch.spacing * morph, float2(0.0)), last);
    local = float3(p->origin.x + s.x * cell, hfHeight(s), p->origin.y + s.y * cell);

    HfOut o;
    o.worldPos = hfToWorld(local);
    o.pos = mul(pc.viewProj, float4(o.worldPos, 1.0));
    // The pixel shader works out the normal at its own footprint.
    o.normal = hfNormalToWorld(float3(0.0, 1.0, 0.0));
    float3x3 m = (float3x3)p->instance.world;
    // The mesh's tangent runs along +u (+X) with its bitangent along +v (+Z), a left-handed frame over a +Y normal.
    o.tangent = float4(mul(m, float3(1.0, 0.0, 0.0)), -1.0);
    o.uv = float2(s.x * p->origin.z, 1.0 - s.y * p->origin.w);
    return o;
}

/** The sample position of a uv (V flipped, as the vertex shader hands it on). */
float2 hfSamplePos(float2 uv)
{
    return float2(uv.x * (hfSize().x - 1), (1.0 - uv.y) * (hfSize().y - 1));
}

/** Samples per pixel across the surface. */
float hfFootprint(float2 s)
{
    return max(max(length(ddx(s)), length(ddy(s))), 1e-4);
}

/** The normal at a pixel, filtered to its footprint: differences a power of two of samples apart, blended between two. */
float3 hfPixelNormal(float2 s, float footprint)
{
    if (footprint <= 1.0)
        return hfNormal(s, 1.0);
    float level = log2(footprint);
    float step = exp2(floor(level));
    return normalize(lerp(hfNormalNearest(s, step), hfNormalNearest(s, step * 2.0), frac(level)));
}

float4 sampleTable(uint handle, float2 uv, float2 dx, float2 dy)
{
#ifdef LILA_BINDLESS
    uint head = pc.textureTable[handle];
    if ((head & (kUdimEntry | kVtEntry)) != 0)
        return float4(1.0);
    return materialTextures[NonUniformResourceIndex(head)].SampleGrad(uv, dx, dy);
#else
    return float4(1.0);
#endif
}

// The slope-aware projection MF_LilaTerrainLayer uses (terrain::cliffs::projectionWeights): the top projection alone
// below kHfSlopeStart degrees, the sides fading in over kHfSlopeBlend above it, so cliffs don't smear one texel row.
static const float kHfSlopeStart = 35.0;
static const float kHfSlopeBlend = 15.0;

/** Weights of the top (y), X-facing (x) and Z-facing (z) projections for a heightfield-space normal. */
float3 hfProjectionWeights(float3 n)
{
    float slope = degrees(acos(saturate(abs(n.y))));
    float side = smoothstep(kHfSlopeStart, kHfSlopeStart + kHfSlopeBlend, slope);
    float2 w = float2(n.x * n.x, n.z * n.z);
    float sum = w.x + w.y;
    if (sum <= 1e-8)
        return float3(0.0, 1.0, 0.0);
    w = w / sum * side;
    return float3(w.x, 1.0 - side, w.y);
}

/**
 * The normal the weights read (terrain::cliffs::projectionNormal): the filtered normal, giving way to the surface's own
 * slope from the pixel's derivatives where they part by 10 to 25 degrees, as across a one-cell step.
 */
float3 hfProjectionNormal(float3 n, float2 planarDx, float2 planarDy, float heightDx, float heightDy)
{
    // planar runs along +X and -Z.
    float3 f = cross(float3(planarDx.x, heightDx, -planarDx.y), float3(planarDy.x, heightDy, -planarDy.y));
    if (dot(f, f) <= 1e-20)
        return n;
    f = normalize(f);
    f = dot(f, n) < 0.0 ? -f : f;
    return normalize(lerp(n, f, 1.0 - smoothstep(0.9063078, 0.9848078, dot(f, n))));
}

/** One projection of a layer's maps: colour times the base map, and the map's normal in heightfield space. */
float3 hfLayerSample(HfLayer l, float2 uv, float2 dx, float2 dy, float3 t, float3 b, float3 n, inout float3 normal, float w)
{
    float3 c = l.color.rgb;
    if (l.baseMap != 0xffff)
        c *= sampleTable(l.baseMap, uv, dx, dy).rgb;
    if (l.normalMap != 0xffff)
    {
        float3 tn = sampleTable(l.normalMap, uv, dx, dy).xyz * 2.0 - 1.0;
        normal += normalize(t * tn.x + b * tn.y + n * max(tn.z, 1e-3)) * w;
    }
    return c;
}

/** The paint layers' blend at a pixel, into gTerrainBase, gTerrainRough and gTerrainNormal. */
void hfLayers(float2 s, float3 n, float2 planar, float2 planarDx, float2 planarDy, float height, float heightDx, float heightDy)
{
    HfParams* p = hf();
    uint count = min(p->info.y, kMaxLayers);
    float3 color = float3(0.0);
    float rough = 0.0, total = 0.0, normalWeight = 0.0;
    float3 normal = float3(0.0);
    const float3 pw = hfProjectionWeights(hfProjectionNormal(n, planarDx, planarDy, heightDx, heightDy));
    for (uint k = 0; k < count; ++k)
    {
        HfLayer l = p->layers[k];
        float w = l.weights != nullptr ? hfMask(l.weights, s) : 0.0;
        if (w <= 0.0)
            continue;
        const float k = l.tiling.x;
        float3 c = float3(0.0);
        float3 ln = float3(0.0);
        if (pw.y > 0.0)
        {
            // Seen from above: the map's right is +X, its top (green up, flipped for DirectX) is -Z.
            float3 t = normalize(float3(1.0, 0.0, 0.0) - n * n.x);
            c += hfLayerSample(l, planar * k, planarDx * k, planarDy * k, t, cross(n, t), n, ln, pw.y) * pw.y;
        }
        if (pw.x > 0.0)
        {
            // The X-facing side: across is Z, up is the height.
            float2 uv = float2(planar.y, -height) * k;
            float3 t = normalize(float3(0.0, 0.0, sign(n.x)) - n * n.z * sign(n.x));
            c += hfLayerSample(l, uv, float2(planarDx.y, -heightDx) * k, float2(planarDy.y, -heightDy) * k, t, cross(n, t), n, ln, pw.x) * pw.x;
        }
        if (pw.z > 0.0)
        {
            float2 uv = float2(planar.x, -height) * k;
            float3 t = normalize(float3(-sign(n.z), 0.0, 0.0) - n * -n.x * sign(n.z));
            c += hfLayerSample(l, uv, float2(planarDx.x, -heightDx) * k, float2(planarDy.x, -heightDy) * k, t, cross(n, t), n, ln, pw.z) * pw.z;
        }
        color += c * w;
        rough += l.color.a * w;
        total += w;
        if (l.normalMap != 0xffff)
        {
            normal += ln * w;
            normalWeight += w;
        }
    }
    if (total <= 0.0)
        return;
    gTerrainBase = float4(color / total, saturate(total));
    gTerrainRough = rough / total;
    if (normalWeight > 0.0)
        gTerrainNormal = float4(hfNormalToWorld(normalize(normal)), saturate(normalWeight));
}

// Turbo (Mikhailov, Google), a polynomial fit: display-encoded rgb.
float3 hfTurbo(float x)
{
    const float4 r4 = float4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
    const float4 g4 = float4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
    const float4 b4 = float4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
    const float2 r2 = float2(-152.94239396, 59.28637943);
    const float2 g2 = float2(4.27729857, 2.82956604);
    const float2 b2 = float2(-89.90310912, 27.34824973);
    x = saturate(x);
    float4 v4 = float4(1.0, x, x * x, x * x * x);
    float2 v2 = v4.zw * v4.z;
    return saturate(float3(dot(v4, r4) + dot(v2, r2), dot(v4, g4) + dot(v2, g2), dot(v4, b4) + dot(v2, b2)));
}

/** How much a pixel lies on grid lines spacing samples apart, lines width pixels wide. */
float hfLines(float2 s, float spacing, float width)
{
    float2 g = s / spacing;
    float2 d = abs(frac(g + 0.5) - 0.5) / max(fwidth(g), float2(1e-6));
    return saturate(0.5 * width + 0.5 - min(d.x, d.y));
}

/** The wire: the sample grid where its lines stand six pixels apart or more, else every power-of-two-th line that does. */
float hfWire(float2 s, float footprint)
{
    float level = log2(max(footprint * 6.0, 1.0));
    float spacing = exp2(floor(level));
    float width = hf()->grid.w;
    return max(hfLines(s, spacing * 2.0, width), hfLines(s, spacing, width) * (1.0 - frac(level)));
}

/** Coverage of an arrow down the slope in this pixel's cell of the arrow grid, and its strength. */
float2 hfArrow(float2 s, float footprint)
{
    HfParams* p = hf();
    float cellSamples = exp2(ceil(log2(max(p->flow.x * footprint, 2.0))));
    float2 center = (floor(s / cellSamples) + 0.5) * cellSamples;
    float h = cellSamples * 0.25;
    float cell = p->grid.z;
    float2 grad = float2(hfHeight(center + float2(h, 0)) - hfHeight(center - float2(h, 0)),
                         hfHeight(center + float2(0, h)) - hfHeight(center - float2(0, h))) / (2.0 * h * cell);
    float slope = length(grad);
    if (slope < 0.005)
        return float2(0.0);
    // Every slope gets an arrow; the flow mask, where there is one, makes the channels' arrows bigger and deeper blue.
    float strength = p->flow.y > 0.5 ? max(hfMask(p->flowMask, center), 0.15) : saturate(slope * 2.0);
    float2 dir = -grad / slope;
    float2 q = (s - center) / cellSamples;
    float along = dot(q, dir), across = abs(dot(q, float2(-dir.y, dir.x)));
    float size = 0.55 + 0.45 * strength;
    along /= size;
    across /= size;
    // A shaft from the tail to the head, then a head narrowing to the tip.
    float shaft = max(max(-0.36 - along, along - 0.08), across - 0.06);
    float head = max(max(0.08 - along, along - 0.4), across - 0.2 * (0.4 - along) / 0.32);
    float sdf = min(shaft, head) * size;
    float pixels = cellSamples / footprint;
    return float2(saturate(0.5 - sdf * pixels), strength);
}

/** The display overlays over a shaded colour (in the scene target's encoding). */
float4 hfOverlays(float4 c, float2 s, float footprint, float height)
{
    HfParams* p = hf();
    if (p->overlayMask != nullptr && p->overlay.x > 0.0)
    {
        float v = hfMask(p->overlayMask, s);
        float3 ramp = p->info.z == kHfOverlayRampGrey ? float3(v) : hfTurbo(v);
        c.rgb = lerp(c.rgb, fromDisplay(ramp, pc.gamma), p->overlay.x);
    }
    if (p->contour.x > 0.0)
    {
        float f = height / p->contour.x;
        float w = fwidth(f);
        float major = float(max(p->info.w, 1u));
        float minorLine = saturate(1.0 - abs(frac(f + 0.5) - 0.5) / max(w, 1e-6)) * saturate(2.0 - w * 8.0);
        float fm = f / major;
        float majorLine = saturate(1.5 - abs(frac(fm + 0.5) - 0.5) / max(fwidth(fm), 1e-6)) * saturate(2.0 - fwidth(fm) * 8.0);
        float line = max(minorLine * 0.55, majorLine) * p->contour.y;
        c.rgb = lerp(c.rgb, fromDisplay(float3(0.08, 0.06, 0.04), pc.gamma), line);
    }
    if ((p->info.x & kHfFlagArrows) != 0)
    {
        float2 arrow = hfArrow(s, footprint);
        if (arrow.x > 0.0)
            c.rgb = lerp(c.rgb, fromDisplay(lerp(float3(0.55, 0.8, 1.0), float3(0.1, 0.45, 1.0), arrow.y), pc.gamma), arrow.x * p->flow.z);
    }
    float4 wire = p->instance.color;
    if ((p->info.x & kHfFlagWire) != 0 && wire.a > 0.0)
    {
        float cover = hfWire(s, footprint) * wire.a;
        c.rgb = lerp(c.rgb, fromDisplay(wire.rgb, pc.gamma), cover);
        c.a = lerp(c.a, 1.0, cover);
    }
    return c;
}

VSOut hfMeshInput(HfOut i)
{
    VSOut o;
    o.pos = i.pos;
    o.worldPos = i.worldPos;
    o.normal = i.normal;
    o.tangent = i.tangent;
    o.uv = i.uv;
    o.color = float4(1.0);
    o.instance = 0;
    return o;
}

[shader("fragment")]
float4 heightfieldPS(HfOut i, bool front : SV_IsFrontFace) : SV_Target
{
    // Derivatives first, from the pixel centre, while every pixel of the quad is still here.
    const float2 s = hfSamplePos(i.uv);
    const float footprint = hfFootprint(s);
    const float cell = hf()->grid.z;
    const float2 planar = float2(hf()->origin.x + s.x * cell, -(hf()->origin.y + s.y * cell));
    const float2 planarDx = ddx(planar), planarDy = ddy(planar);
    const float2 centerUv = i.uv;

    const float3 n = hfPixelNormal(s, footprint);
    const float height = hfHeight(s);
    const float heightDx = ddx(height), heightDy = ddy(height);
    if ((pc.flags & kGreyBase) == 0)
        hfLayers(s, n, planar, planarDx, planarDy, height, heightDx, heightDy);
    VSOut m = hfMeshInput(i);
    m.normal = hfNormalToWorld(n);
    const Centers center = {m.normal, centerUv};
    m.uv = EvaluateAttributeAtCentroid(i.uv);
    float4 c = shadeMesh(m, front, center);
    return hfOverlays(c, s, footprint, height);
}

/** Wireframe display: the wire alone, blended over what is behind. */
[shader("fragment")]
float4 heightfieldWirePS(HfOut i) : SV_Target
{
    const float2 s = hfSamplePos(i.uv);
    float4 wire = hf()->instance.color;
    float cover = hfWire(s, hfFootprint(s)) * wire.a;
    if (cover < 1.0 / 255.0)
        discard;
    return float4(fromDisplay(wire.rgb, pc.gamma), cover);
}

/** Depth and world normal for the ambient occlusion prepass, like prepassPS. */
[shader("fragment")]
float4 heightfieldPrepassPS(HfOut i, bool front : SV_IsFrontFace) : SV_Target
{
    // The faceted normal is plenty for occlusion, and spares the pass the filtered one.
    float3 n = normalize(cross(ddy(i.worldPos), ddx(i.worldPos)));
    if (dot(n, hfNormalToWorld(float3(0.0, 1.0, 0.0))) < 0.0)
        n = -n;
    return float4(n, pc.extra.x);
}
