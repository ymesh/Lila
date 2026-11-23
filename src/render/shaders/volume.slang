// Volumes (render::VolumeDraw): ray marched at half resolution up to the prepass depth, lit by the frame's lights with
// a shadow march through the volume and by the environment's average light. Each texel holds the light the volumes add
// and the share of what lies behind that gets through; mesh.slang lays it over meshes and the backdrop.

#include "volume_common.slang"

/** The light buffer's entries; matches render::GpuLight (mesh.slang's). */
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

struct VolumePush
{
    float4x4 invViewProj; // clip to the frame's float space, reversed-Z
    GpuVolume* volumes;
    float4* out;      // half resolution: rgb added light, a transmittance
    GpuLight* lights; // the frame's header, then its lights
    uint lightCount;
    uint volumeCount;
    float4 size;    // half-resolution width and height, full width and height
    float4 ambient; // rgb the environment's average light, w the frame counter
    float4 eye;     // xyz camera position in the frame's float space
};

[[vk::push_constant]] VolumePush pc;
[[vk::binding(0, 0)]] Sampler2D depthTex; // the prepass's reversed-Z depth

static const uint kMaxSteps = 96;
static const uint kShadowSteps = 6;

float hash(uint2 p, uint frame)
{
    uint h = p.x * 1973u + p.y * 9277u + frame * 26699u;
    h = (h ^ (h >> 13)) * 0x5bd1e995u;
    h ^= h >> 15;
    return float(h & 0xFFFFFFu) / 16777216.0;
}

/** Light reaching P from a light, and the direction toward it. */
float3 lightAt(GpuLight l, float3 P, out float3 toLight, out float dist)
{
    uint kind = uint(l.position.w + 0.5) & 15;
    if (kind == 2)
    {
        toLight = -l.direction.xyz;
        dist = 1e9;
        return l.radiance.rgb;
    }
    float3 d = l.position.xyz - P;
    dist = length(d);
    toLight = d / max(dist, 1e-4);
    float falloff = 1.0 / max(dist * dist * 1e-4, 1e-4); // radiance is given at 1 m
    if (l.direction.w > 0.0)
        falloff *= saturate(1.0 - pow(dist / l.direction.w, 4.0));
    if (kind == 1)
        falloff *= saturate((dot(-toLight, l.direction.xyz) - l.cone.x) * l.cone.y);
    return l.radiance.rgb * falloff;
}

/** The share of light crossing the volume from P toward the light. */
float shadowTransmit(GpuVolume* v, float3 P, float3 toLight, float dist)
{
    float3 o = mul(v->worldToIndex, float4(P, 1.0)).xyz;
    float3 d = mul((float3x3)v->worldToIndex, toLight);
    float2 r = volBoxRange(o, d, v->dims.xyz);
    float len = min(r.y, dist);
    if (len <= 0.0)
        return 1.0;
    float dt = len / kShadowSteps;
    float sigma = volSigma(v);
    float depth = 0.0;
    for (uint i = 0; i < kShadowSteps; ++i)
        depth += volDensity(v, o + d * ((i + 0.5) * dt)) * sigma * dt;
    return exp(-depth);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void volumeCS(uint3 id: SV_DispatchThreadID)
{
    uint2 size = uint2(pc.size.xy);
    if (id.x >= size.x || id.y >= size.y)
        return;
    float2 uv = (float2(id.xy) + 0.5) / float2(size);
    float depth = depthTex.SampleLevel(uv, 0).r;
    float2 ndc = uv * 2.0 - 1.0;
    float4 nearH = mul(pc.invViewProj, float4(ndc, 1.0, 1.0));
    float4 farH = mul(pc.invViewProj, float4(ndc, 0.001, 1.0));
    float3 origin = pc.eye.xyz;
    float3 dir = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);
    float sceneT = 1e30;
    if (depth > 0.0)
    {
        float4 hit = mul(pc.invViewProj, float4(ndc, depth, 1.0));
        sceneT = length(hit.xyz / hit.w - origin);
    }
    float3 L = float3(0.0, 0.0, 0.0);
    float3 T = float3(1.0, 1.0, 1.0);
    float jitter = hash(id.xy, uint(pc.ambient.w));
    for (uint vi = 0; vi < pc.volumeCount; ++vi)
    {
        GpuVolume* v = pc.volumes + vi;
        float3 o = mul(v->worldToIndex, float4(origin, 1.0)).xyz;
        float3 d = mul((float3x3)v->worldToIndex, dir);
        float2 r = volBoxRange(o, d, v->dims.xyz);
        r.x = max(r.x, 0.0);
        r.y = min(r.y, sceneT);
        if (r.y <= r.x)
            continue;
        uint steps = clamp(uint(length(d) * (r.y - r.x)), 8u, kMaxSteps);
        float dt = (r.y - r.x) / steps;
        float g = v->scatter.w;
        for (uint i = 0; i < steps; ++i)
        {
            float t = r.x + (i + jitter) * dt;
            float dens = volDensity(v, o + d * t);
            if (dens <= 1e-5)
                continue;
            float3 sigmaS = v->scatter.rgb * dens, sigmaT = sigmaS + v->absorb.rgb * dens;
            float3 P = origin + dir * t;
            float3 lit = pc.ambient.rgb;
            for (uint k = 0; k < pc.lightCount; ++k)
            {
                GpuLight l = pc.lights[k + 1];
                float3 toLight;
                float dist;
                float3 radiance = lightAt(l, P, toLight, dist);
                if (all(radiance <= 0.0))
                    continue;
                lit += radiance * volPhase(g, dot(dir, toLight)) * shadowTransmit(v, P, toLight, dist);
            }
            float3 S = sigmaS * lit + v->emission.rgb * dens;
            float3 segT = exp(-sigmaT * dt);
            L += T * S * (1.0 - segT) / max(sigmaT, 1e-8);
            T *= segT;
            if (all(T < 0.003))
                break;
        }
    }
    pc.out[id.y * size.x + id.x] = float4(L, dot(T, float3(0.3333, 0.3334, 0.3333)));
}
