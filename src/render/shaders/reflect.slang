// Screen-space reflections: a hierarchical-Z trace of each half-resolution pixel's reflected ray (sampled from its
// roughness lobe) through the depth prepass, shaded from last frame's picture where it lands, then a temporal and a
// roughness-scaled spatial filter. The mesh shader reads the result in place of the environment, by its confidence.
// Results are premultiplied: rgb is radiance times confidence, a the confidence.

#include "scene_color.slang"
#include "reflect_common.slang"

[[vk::binding(0, 0)]] Sampler2D depthTex;   // the prepass, reversed-Z, full resolution
[[vk::binding(1, 0)]] Sampler2D normalTex;  // the prepass normal, roughness in its length
[[vk::binding(2, 0)]] Sampler2D colorTex;   // last frame's scene (compressed, exposed), linear filtering
[[vk::binding(3, 0)]] Sampler2D hizTex;     // nearest depth per cell, a level per halving
[[vk::binding(4, 0)]] RWTexture2D<float4> reflectOut;
[[vk::binding(5, 0)]] Sampler2D inTex;      // temporal: this frame's trace; spatial: the temporal result
[[vk::binding(6, 0)]] Sampler2D historyTex; // temporal: last frame's result, linear filtering
[[vk::binding(7, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> hizOut;

/** The full-resolution pixel a half-resolution one traces from this frame: each of its four in turn. */
int2 tracedPixel(int2 half)
{
    uint k = uint(rp.params.x) & 3u;
    int2 fp = half * 2 + int2(k & 1u, k >> 1);
    return min(fp, int2(rp.size.zw) - 1);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void hizCS(uint3 id: SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    uint w, h, levels;
    hizTex.GetDimensions(rpc.level, w, h, levels);
    if (any(px >= int2(w, h)))
        return;
    if (rpc.level == 0)
    {
        hizOut[px] = depthTex.Load(int3(px, 0)).r;
        return;
    }
    uint pw, ph;
    hizTex.GetDimensions(rpc.level - 1, pw, ph, levels);
    int2 src = px * 2;
    int2 last = int2(pw, ph) - 1;
    float z = 0.0;
    // An odd-sized level below folds its last row and column into this one's edge cells.
    int2 span = int2((px.x * 2 + 2 == int(pw) - 1) ? 3 : 2, (px.y * 2 + 2 == int(ph) - 1) ? 3 : 2);
    for (int y = 0; y < span.y; ++y)
        for (int x = 0; x < span.x; ++x)
            z = max(z, hizTex.Load(int3(min(src + int2(x, y), last), int(rpc.level) - 1)).r);
    hizOut[px] = z;
}

/** Screen position of a world point: full-resolution pixels and reversed-Z depth; w <= 0 behind the eye. */
float4 toScreen(float3 P)
{
    float4 c = mul(rp.viewProj, float4(P, 1.0));
    float3 ndc = c.xyz / c.w;
    return float4((ndc.xy * 0.5 + 0.5) * rp.size.zw, ndc.z, c.w);
}

/**
 * March from start to end (pixels, depth) through the hierarchical depth: skip whole cells the ray passes in front of,
 * descend where it might meet a surface. A hit at the finest level counts only within the thickness behind it.
 */
bool traceHiZ(float3 start, float3 end, float3 eye, out float2 hitPixel)
{
    hitPixel = float2(0, 0);
    float3 d = end - start;
    float pixels = length(d.xy);
    if (pixels < 1.0)
        return false;
    const int levels = max(int(rp.extra.w), 1);
    const float2 full = rp.size.zw;
    float t = 1.5 / pixels;
    int level = 0;
    float2 inv = float2(abs(d.x) > 1e-6 ? 1.0 / d.x : 1e12, abs(d.y) > 1e-6 ? 1.0 / d.y : 1e12);
    for (int i = 0; i < 128 && level >= 0; ++i)
    {
        if (t > 1.0)
            return false;
        float3 p = start + d * t;
        if (any(p.xy < 0.0) || any(p.xy >= full))
            return false;
        float cellSize = exp2(float(level));
        float2 cell = floor(p.xy / cellSize);
        float zCell = hizTex.Load(int3(int2(cell), level)).r;
        float2 boundary = (cell + step(0.0, d.xy)) * cellSize;
        float2 tb = (boundary - start.xy) * inv;
        float tExit = min(tb.x, tb.y) + 0.02 / pixels;
        if (p.z > zCell)
        {
            float tCross = d.z < 0.0 ? (zCell - start.z) / d.z : 1e9;
            if (tCross < tExit)
            {
                t = max(tCross, t);
                level = max(level - 1, 0);
            }
            else
            {
                t = tExit;
                level = min(level + 1, levels - 1);
            }
            continue;
        }
        if (level > 0)
        {
            --level;
            continue;
        }
        // Behind the nearest surface of a finest cell: a hit when the ray is no further behind than a thin shell.
        if (zCell <= 0.0)
            return false;
        float2 uv = (cell + 0.5) / full;
        float surface = length(worldAt(uv, zCell) - eye);
        float ray = length(worldAt(uv, p.z) - eye);
        if (ray - surface < max(2.0, surface * 0.03))
        {
            hitPixel = cell + 0.5;
            return true;
        }
        t = tExit;
    }
    return false;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ssrCS(uint3 id: SV_DispatchThreadID)
{
    int2 half = int2(id.xy);
    if (any(half >= int2(rp.size.xy)))
        return;
    int2 fp = tracedPixel(half);
    float depth = depthTex.Load(int3(fp, 0)).r;
    float3 packed = normalTex.Load(int3(fp, 0)).xyz;
    float rough = decodeRoughness(packed);
    const float cutoff = rp.params.y;
    if (depth <= 0.0 || dot(packed, packed) < 0.01 || rough >= cutoff)
    {
        reflectOut[half] = float4(0, 0, 0, 0);
        return;
    }
    float3 n = normalize(packed);
    float2 uv = (float2(fp) + 0.5) / rp.size.zw;
    float3 P = worldAt(uv, depth);
    float3 eye = eyePosition();
    float3 v = normalize(eye - P);
    uint state = hash(uint(fp.x) * 1973u + uint(fp.y) * 9277u + uint(rp.params.x) * 26699u);
    float3 r = reflectedRay(n, v, rough, state);

    // The ray's end: its full reach, or short of the eye's plane when it comes back toward the camera.
    float reach = rp.extra.z;
    float4 c0 = mul(rp.viewProj, float4(P, 1.0));
    float4 cr = mul(rp.viewProj, float4(r, 0.0));
    if (cr.w < 0.0)
        reach = min(reach, max((c0.w - 1.0) / -cr.w, 0.0) * 0.99);
    float4 s0 = toScreen(P + n * max(0.05, length(eye - P) * 1e-3));
    float4 s1 = toScreen(P + r * reach);
    float2 hit;
    if (s1.w <= 0.0 || !traceHiZ(s0.xyz, s1.xyz, eye, hit))
    {
        reflectOut[half] = float4(0, 0, 0, 0);
        return;
    }
    float2 hitUv = hit / rp.size.zw;
    float hitDepth = hizTex.Load(int3(int2(hit), 0)).r;
    float3 hitN = normalTex.Load(int3(int2(hit), 0)).xyz;
    // A surface seen from behind, which last frame's picture never showed.
    if (dot(hitN, r) > 0.0)
    {
        reflectOut[half] = float4(0, 0, 0, 0);
        return;
    }
    float3 H = worldAt(hitUv, hitDepth);
    float4 prev = mul(rp.prevViewProj, float4(H, 1.0));
    float2 prevUv = prev.xy / prev.w * 0.5 + 0.5;
    if (prev.w <= 0.0 || any(prevUv < 0.0) || any(prevUv > 1.0))
    {
        reflectOut[half] = float4(0, 0, 0, 0);
        return;
    }
    float3 radiance = sceneExpand(colorTex.SampleLevel(prevUv, 0).rgb) * rp.extra.y;
    // Fade toward the screen's edges, where the picture runs out, and toward the roughness cutoff.
    float2 e = min(min(hitUv, 1.0 - hitUv), min(prevUv, 1.0 - prevUv));
    float confidence = smoothstep(0.0, 0.06, min(e.x, e.y)) * (1.0 - smoothstep(cutoff * 0.75, cutoff, rough));
    reflectOut[half] = float4(radiance * confidence, confidence);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void temporalCS(uint3 id: SV_DispatchThreadID)
{
    int2 half = int2(id.xy);
    if (any(half >= int2(rp.size.xy)))
        return;
    float4 current = inTex.Load(int3(half, 0));
    const float weight = rp.params.z;
    if (weight >= 1.0)
    {
        reflectOut[half] = current;
        return;
    }
    int2 fp = tracedPixel(half);
    float depth = depthTex.Load(int3(fp, 0)).r;
    if (depth <= 0.0)
    {
        reflectOut[half] = current;
        return;
    }
    float3 P = worldAt((float2(fp) + 0.5) / rp.size.zw, depth);
    float4 prev = mul(rp.prevViewProj, float4(P, 1.0));
    float2 prevUv = prev.xy / prev.w * 0.5 + 0.5;
    if (prev.w <= 0.0 || any(prevUv < 0.0) || any(prevUv > 1.0))
    {
        reflectOut[half] = current;
        return;
    }
    float4 history = historyTex.SampleLevel(prevUv, 0);
    if (rp.params.w > 0.5)
    {
        // Moving: the history may hold what is no longer reflected; keep it within this frame's neighbourhood.
        float4 lo = current, hi = current;
        int2 last = int2(rp.size.xy) - 1;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                float4 q = inTex.Load(int3(clamp(half + int2(x, y), int2(0), last), 0));
                lo = min(lo, q);
                hi = max(hi, q);
            }
        history = clamp(history, lo, hi);
    }
    reflectOut[half] = lerp(history, current, weight);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void spatialCS(uint3 id: SV_DispatchThreadID)
{
    int2 half = int2(id.xy);
    if (any(half >= int2(rp.size.xy)))
        return;
    int2 fp = min(half * 2, int2(rp.size.zw) - 1);
    float depth = depthTex.Load(int3(fp, 0)).r;
    float3 packed = normalTex.Load(int3(fp, 0)).xyz;
    float rough = decodeRoughness(packed);
    float4 centre = inTex.Load(int3(half, 0));
    // Mirrors keep their detail; rougher lobes average wider.
    int radius = int(round(rough * 3.0));
    if (depth <= 0.0 || radius == 0)
    {
        reflectOut[half] = centre;
        return;
    }
    float3 n = normalize(packed);
    float4 sum = centre;
    float total = 1.0;
    int2 last = int2(rp.size.xy) - 1;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            int2 q = clamp(half + int2(x, y) * radius, int2(0), last);
            int2 fq = min(q * 2, int2(rp.size.zw) - 1);
            float dq = depthTex.Load(int3(fq, 0)).r;
            float3 nq = normalTex.Load(int3(fq, 0)).xyz;
            if (dq <= 0.0)
                continue;
            float w = exp(-0.25 * float(x * x + y * y));
            w *= exp(-abs(dq - depth) / max(depth, 1e-9) * 200.0);
            w *= pow(saturate(dot(n, normalize(nq))), 16.0);
            w *= 1.0 - saturate(abs(decodeRoughness(nq) - rough) * 4.0);
            sum += inTex.Load(int3(q, 0)) * w;
            total += w;
        }
    reflectOut[half] = sum / total;
}
