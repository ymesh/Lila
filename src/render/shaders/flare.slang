// Lens flare at quarter resolution, from the picture the compose pass reads: a bright pass, then ghosts mirrored through
// the centre with a colour split, a halo ring, star rays (one pair per aperture blade) and anamorphic streaks; and for
// lights that ask for one, a glare and a chain of ghosts along the line through the centre, faded by how much of a
// small disc round the light the depth leaves in view. compose.slang adds the result before exposure.

#include "scene_color.slang"

struct FlareLight
{
    float4 screen; // xy uv, z reversed-Z depth (0 for a direction), w 1 when in front of the camera
    float4 color;  // rgb exposed radiance times the light's flare intensity, w the light's flare scale
};

/** Matches render::FlareParams. */
struct FlareParams
{
    float4 size;  // quarter extent, then the full extent
    float4 a;     // threshold, ghosts, ghost spacing, chroma
    float4 b;     // halo, halo radius, starburst, streak length
    float4 c;     // star arms, blade rotation (radians), anamorphic streaks, squeeze
    float4 tint;  // anamorphic tint, w aspect (width over height)
    float4 d;     // x light count, y visibility blend, z 1 for the picture's own flare, w disc radius in pixels
    FlareLight lights[8];
};

struct FlarePush
{
    FlareParams* params;
    uint pad[2];
};

[[vk::push_constant]] FlarePush fpc;
#define fp (*fpc.params)

[[vk::binding(0, 0)]] Sampler2D sceneTex;  // compressed, linear filtering
[[vk::binding(1, 0)]] Sampler2D depthTex;  // the frame's reversed-Z depth, single sample
[[vk::binding(2, 0)]] Sampler2D brightTex; // the bright pass, linear filtering, clamped
[[vk::binding(3, 0)]] RWTexture2D<float4> brightOut;
[[vk::binding(4, 0)]] RWTexture2D<float4> flareOut;
[[vk::binding(5, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> visibility; // per light, eased frame to frame

static const float PI = 3.14159265;

[shader("compute")]
[numthreads(8, 8, 1)]
void brightCS(uint3 id: SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    if (any(px >= int2(fp.size.xy)))
        return;
    float2 texel = 1.0 / fp.size.zw;
    float2 uv = (float2(px) + 0.5) / fp.size.xy;
    float3 c = 0.0;
    // Four bilinear taps cover the 4x4 block of full-resolution pixels.
    for (int k = 0; k < 4; ++k)
        c += sceneExpand(sceneTex.SampleLevel(uv + texel * float2((k & 1) != 0 ? 1.0 : -1.0, (k & 2) != 0 ? 1.0 : -1.0), 0).rgb);
    c *= 0.25;
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    float thr = fp.a.x;
    // A soft knee into the threshold, then compressed so a large or very hot source flares bright but never floods.
    float k = saturate((lum - thr) / max(thr * 0.5, 1e-3));
    float3 b = c * k * k;
    brightOut[px] = float4(b / (1.0 + dot(b, float3(0.2126, 0.7152, 0.0722)) * 0.25), 1.0);
}

groupshared float gSeen[32];

[shader("compute")]
[numthreads(32, 1, 1)]
void visibilityCS(uint3 gid: SV_GroupID, uint3 tid: SV_GroupThreadID)
{
    const uint li = gid.x;
    FlareLight l = fp.lights[li];
    // A disc of taps round the light, by a golden-angle spiral.
    float r = sqrt((float(tid.x) + 0.5) / 32.0) * fp.d.w;
    float ang = float(tid.x) * 2.39996;
    float2 uv = l.screen.xy + float2(cos(ang), sin(ang)) * r / fp.size.zw;
    float seen = 0.0;
    if (li < uint(fp.d.x) && l.screen.w > 0.5 && all(uv >= 0.0) && all(uv <= 1.0))
    {
        float depth = depthTex.SampleLevel(uv, 0).r;
        // Reversed-Z: in view where nothing nearer than the light covers it (a sun only through the background).
        seen = l.screen.z <= 0.0 ? (depth <= 0.0 ? 1.0 : 0.0) : (depth <= l.screen.z * 1.0005 + 1e-7 ? 1.0 : 0.0);
    }
    gSeen[tid.x] = seen;
    GroupMemoryBarrierWithGroupSync();
    if (tid.x == 0)
    {
        float total = 0.0;
        for (int k = 0; k < 32; ++k)
            total += gSeen[k];
        float v = total / 32.0;
        float prev = visibility[int2(li, 0)];
        visibility[int2(li, 0)] = lerp(prev, v, fp.d.y);
    }
}

float3 bright(float2 uv)
{
    return brightTex.SampleLevel(uv, 0).rgb;
}

/** How much a ghost or halo sample counts: full near the centre, none at the corners. */
float centreWeight(float2 uv, float power)
{
    float2 d = (uv - 0.5) * float2(fp.tint.w, 1.0);
    return pow(saturate(1.0 - length(d) / length(float2(fp.tint.w, 1.0) * 0.5)), power);
}

/** A soft polygon of n blades (0 round), radius 1, for ghost sprites: 1 inside fading to 0 at the edge, with a brighter rim. */
float aperture(float2 q, float n, float rot)
{
    float r = length(q);
    if (n >= 3.0)
    {
        float a = atan2(q.y, q.x) - rot;
        float seg = 2.0 * PI / n;
        r *= cos(PI / n) / cos(a - seg * floor(a / seg + 0.5));
    }
    return saturate((1.0 - r) * 6.0) * (0.55 + 0.45 * smoothstep(0.6, 1.0, r));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void flareCS(uint3 id: SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    if (any(px >= int2(fp.size.xy)))
        return;
    float2 uv = (float2(px) + 0.5) / fp.size.xy;
    const float aspect = fp.tint.w;
    float3 sum = 0.0;
    if (fp.d.z > 0.5)
    {
        // Ghosts: the bright pass mirrored through the centre at steps along the line, each with a colour split.
        float2 toCentre = 0.5 - uv;
        float2 ghostVec = toCentre * fp.a.z;
        float2 split = normalize(toCentre + 1e-6) * fp.a.w;
        int ghosts = int(fp.a.y + 0.5);
        for (int i = 0; i < ghosts; ++i)
        {
            // Odd ghosts flip through the centre, even ones stay on this side, as reflections between lens elements do.
            float2 s = uv + ghostVec * float(i + 1) * (i % 2 == 0 ? 1.0 : -0.7);
            if (any(s < 0.0) || any(s > 1.0))
                continue;
            float w = centreWeight(s, 4.0) * (i % 2 == 0 ? 0.12 : 0.07);
            sum += float3(bright(s + split).r, bright(s).g, bright(s - split).b) * w;
        }
        // Halo: a ring at a fixed radius round the centre.
        if (fp.b.x > 0.0)
        {
            float2 dir = normalize((toCentre + 1e-6) * float2(aspect, 1.0)) / float2(aspect, 1.0);
            float2 h = uv + dir * fp.b.y;
            float w = centreWeight(h, 4.0) * fp.b.x * 0.15;
            sum += float3(bright(h + split * 2.0).r, bright(h).g, bright(h - split * 2.0).b) * w;
        }
        // Star rays: each bright point smeared along the blade directions.
        if (fp.b.z > 0.0)
        {
            int arms = int(fp.c.x + 0.5);
            int rayTaps = clamp(int(fp.b.w * fp.size.y), 8, 48);
            float3 star = 0.0;
            for (int a = 0; a < arms; ++a)
            {
                float ang = fp.c.y + float(a) * 2.0 * PI / float(arms);
                float2 dir = float2(cos(ang) / aspect, sin(ang)) * fp.b.w;
                for (int t = 1; t <= rayTaps; ++t)
                {
                    float f = float(t) / float(rayTaps);
                    star += bright(uv - dir * f) * (1.0 - f) * (1.0 - f);
                }
            }
            sum += star * fp.b.z * 2.4 / float(max(arms, 1) * rayTaps);
        }
        // Anamorphic streaks: a long horizontal smear, tinted.
        if (fp.c.z > 0.0)
        {
            float3 streak = 0.0;
            // Taps no wider apart than a quarter-resolution texel, so a small light streaks as a line, not dots.
            float reach = fp.b.w * 2.5 * max(fp.c.w, 1.0);
            int taps = clamp(int(reach * fp.size.x), 8, 96);
            for (int t = -taps; t <= taps; ++t)
            {
                float f = float(t) / float(taps);
                streak += bright(uv + float2(f * reach, 0.0)) * exp(-abs(f) * 3.0);
            }
            sum += streak / float(taps) * 0.6 * fp.c.z * fp.tint.rgb;
        }
    }
    // Light flares: a glare with rays at the light, then ghosts along the line through the centre.
    static const float kGhostAt[6] = {-0.35, 0.45, 0.8, 1.25, 1.6, 2.1};
    static const float kGhostSize[6] = {0.05, 0.03, 0.08, 0.04, 0.12, 0.06};
    static const float3 kGhostTint[6] = {float3(1.0, 0.6, 0.3), float3(0.4, 1.0, 0.5), float3(0.5, 0.6, 1.0), float3(1.0, 0.4, 0.8),
                                         float3(0.6, 0.9, 1.0), float3(1.0, 0.85, 0.5)};
    const int count = int(fp.d.x + 0.5);
    for (int li = 0; li < count; ++li)
    {
        FlareLight l = fp.lights[li];
        float vis = visibility[int2(li, 0)];
        if (vis <= 1e-3 || l.screen.w < 0.5)
            continue;
        float3 col = l.color.rgb * vis;
        float scale = l.color.w;
        float2 q = (uv - l.screen.xy) * float2(aspect, 1.0);
        float r = length(q);
        float ang = atan2(q.y, q.x);
        float arms = fp.c.x;
        float rays = pow(abs(cos(ang * arms * 0.5 - fp.c.y)), 60.0) * exp(-r / (0.12 * scale));
        sum += col * (exp(-r * r / (0.0004 * scale * scale)) * 4.0 + 0.6 * exp(-r / (0.02 * scale)) + rays * 0.8);
        if (fp.c.z > 0.0)
            sum += col * fp.tint.rgb * fp.c.z * exp(-abs(q.y) / (0.004 * scale)) * exp(-abs(q.x) / (0.6 * scale));
        float2 axis = 0.5 - l.screen.xy;
        for (int g = 0; g < 6; ++g)
        {
            float2 c = l.screen.xy + axis * kGhostAt[g];
            float2 gq = (uv - c) * float2(aspect / max(fp.c.w, 1.0), 1.0) / (kGhostSize[g] * scale);
            sum += col * kGhostTint[g] * aperture(gq, fp.c.x * 0.5 >= 3.0 ? fp.c.x * 0.5 : fp.c.x, fp.c.y) * 0.08;
        }
    }
    flareOut[px] = float4(sum, 1.0);
}
