// Ray-traced reflections: one bounce per half-resolution pixel along a ray from its roughness lobe. A hit shades with its
// material's base colour, emission, the environment's light and the frame's lights (shadowed by a ray each); a miss sees
// the environment. Same output as reflect.slang's trace (premultiplied, confidence in alpha); its filters follow.

#include "reflect_common.slang"

/** One light; matches render::GpuLight. Kinds: 0 point, 1 spot, 2 directional, 3 area, 4 disc. */
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

/** A traced placement's mesh; matches render::TraceInstance. */
struct TraceInstance
{
    uint64_t vertices; // MeshVertex, 64 bytes each: the normal sits 12 bytes in
    uint64_t indices;
    float4 rows[3];
    uint64_t sections; // TraceSection per material section, in triangle order
    uint sectionCount;
    uint pad;
};

/** A section's material for hit shading; matches render::TraceSection. */
struct TraceSection
{
    uint endTriangle; // one past its last triangle
    float roughness;
    float metallic;
    float pad;
    float4 baseColor; // linear
    float4 emissive;  // linear
};

struct EnvUniforms
{
    float4 sh[9]; // irradiance / pi, rgb
};

[[vk::binding(0, 0)]] Sampler2D depthTex;
[[vk::binding(1, 0)]] Sampler2D normalTex;
[[vk::binding(4, 0)]] RWTexture2D<float4> reflectOut;
[[vk::binding(8, 0)]] RaytracingAccelerationStructure sceneAs;
[[vk::binding(9, 0)]] SamplerCube envMap;
[[vk::binding(10, 0)]] ConstantBuffer<EnvUniforms> envU;

int2 tracedPixel(int2 half)
{
    uint k = uint(rp.params.x) & 3u;
    int2 fp = half * 2 + int2(k & 1u, k >> 1);
    return min(fp, int2(rp.size.zw) - 1);
}

float3 irradiance(float3 n)
{
    float3 e = envU.sh[0].rgb * 0.282095;
    e += envU.sh[1].rgb * (0.488603 * n.y) + envU.sh[2].rgb * (0.488603 * n.z) + envU.sh[3].rgb * (0.488603 * n.x);
    e += envU.sh[4].rgb * (1.092548 * n.x * n.y) + envU.sh[5].rgb * (1.092548 * n.y * n.z);
    e += envU.sh[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) + envU.sh[7].rgb * (1.092548 * n.x * n.z);
    e += envU.sh[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return envSaturate(max(e, 0.0));
}

bool occluded(float3 P, float3 dir, float reach)
{
    RayDesc ray;
    ray.Origin = P;
    ray.Direction = dir;
    ray.TMin = 0.0;
    ray.TMax = reach;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(sceneAs, RAY_FLAG_NONE, 0xff, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

/** Diffuse light at a hit from the frame's lights, as mesh.slang's directLights shapes them; the first four may shadow. */
float3 hitLights(float3 P, float3 n)
{
    float3 sum = float3(0, 0, 0);
    if (rp.lights == 0)
        return sum;
    GpuLight* lights = (GpuLight*)rp.lights;
    for (uint k = 0; k < min(rp.lightCount, 64u); ++k)
    {
        GpuLight l = lights[k + 1];
        const uint kind = uint(l.position.w + 0.5) & 15;
        float3 L;
        float atten = 1.0, dist = 1e7;
        if (kind == 2)
            L = -l.direction.xyz;
        else
        {
            float3 toLight = l.position.xyz - P;
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
                float t = saturate((dot(-L, l.direction.xyz) - l.cone.x) * l.cone.y);
                atten *= t * t;
            }
            else if (kind >= 3)
                atten *= saturate(dot(-L, l.direction.xyz));
        }
        float nl = saturate(dot(n, L));
        if (atten <= 0.0 || nl <= 0.0)
            continue;
        if (rp.traceShadows != 0 && k < 4 && occluded(P + n * 0.1, L, kind == 2 ? 1e7 : dist * 0.995))
            continue;
        sum += l.radiance.rgb * atten * nl / PI;
    }
    return sum;
}

/** The light leaving a committed hit back along the ray. */
float3 shadeHit(uint instance, uint primitive, float2 bary, float3 P, float3 rayDir)
{
    TraceInstance ti = ((TraceInstance*)rp.instances)[instance];
    uint* idx = (uint*)ti.indices;
    float3 nn = float3(0, 0, 0);
    float w[3] = { 1.0 - bary.x - bary.y, bary.x, bary.y };
    for (uint k = 0; k < 3; ++k)
    {
        float* v = (float*)(ti.vertices + uint64_t(idx[primitive * 3 + k]) * 64);
        nn += float3(v[3], v[4], v[5]) * w[k];
    }
    float3 n = normalize(float3(dot(ti.rows[0].xyz, nn), dot(ti.rows[1].xyz, nn), dot(ti.rows[2].xyz, nn)) + 1e-9);
    if (dot(n, rayDir) > 0.0)
        n = -n;
    TraceSection m;
    m.baseColor = float4(0.5, 0.5, 0.5, 1.0);
    m.emissive = float4(0, 0, 0, 0);
    m.metallic = 0.0;
    m.roughness = 0.5;
    TraceSection* sections = (TraceSection*)ti.sections;
    for (uint s = 0; s < ti.sectionCount; ++s)
    {
        m = sections[s];
        if (primitive < m.endTriangle)
            break;
    }
    // One bounce: diffuse from the environment and lights, and the hit's own reflection of the environment alone.
    float metal = saturate(m.metallic);
    float3 albedo = m.baseColor.rgb * (1.0 - metal);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), m.baseColor.rgb, metal);
    float nv = saturate(dot(n, -rayDir));
    float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - nv, 5.0) * (1.0 - m.roughness);
    float3 mirrored = envSaturate(envMap.SampleLevel(toEnv(reflect(rayDir, n)), saturate(m.roughness) * 6.0).rgb) * rp.env.z;
    float3 lit = albedo * (irradiance(toEnv(n)) * rp.env.z + hitLights(P, n)) + fresnel * mirrored;
    return m.emissive.rgb + lit;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rtReflectCS(uint3 id: SV_DispatchThreadID)
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
    float3 P = worldAt((float2(fp) + 0.5) / rp.size.zw, depth);
    float3 eye = eyePosition();
    float dist = length(eye - P);
    float3 v = (eye - P) / max(dist, 1e-4);
    uint state = hash(uint(fp.x) * 1973u + uint(fp.y) * 9277u + uint(rp.params.x) * 26699u);
    float3 r = reflectedRay(n, v, rough, state);

    RayDesc ray;
    ray.Origin = P + v * dist * 1e-3 + n * max(0.05, dist * 1e-3);
    ray.Direction = r;
    ray.TMin = 0.0;
    ray.TMax = rp.extra.z;
    // A ray leaving a faceted curved surface along its smooth normal can start just under a neighbouring facet: a back
    // face met within a facet's depth is that surface itself, so the ray carries on from past it.
    const float selfReach = max(2.0, dist * 0.01);
    bool hit = false;
    uint instance = 0, primitive = 0;
    float2 bary = float2(0, 0);
    float3 hitPos = float3(0, 0, 0);
    for (int pass = 0; pass < 2; ++pass)
    {
        RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(sceneAs, RAY_FLAG_NONE, 0xff, ray);
        while (q.Proceed())
        {
        }
        hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
        if (!hit)
            break;
        instance = q.CommittedInstanceID();
        primitive = q.CommittedPrimitiveIndex();
        bary = q.CommittedTriangleBarycentrics();
        hitPos = ray.Origin + r * q.CommittedRayT();
        if (pass == 1 || q.CommittedTriangleFrontFace() || q.CommittedRayT() > selfReach)
            break;
        ray.Origin = hitPos + r * 0.05;
    }
    float3 radiance;
    if (hit)
        radiance = shadeHit(instance, primitive, bary, hitPos, r);
    else
        radiance = envSaturate(envMap.SampleLevel(toEnv(r), 0).rgb) * rp.env.z;
    float confidence = 1.0 - smoothstep(cutoff * 0.75, cutoff, rough);
    reflectOut[half] = float4(radiance * confidence, confidence);
}
