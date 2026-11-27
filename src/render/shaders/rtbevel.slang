// The live bevel: for each pixel of the depth prepass whose material asks for a radius, the scene's smooth normals within
// that radius found by short rays through the surface (as the baker's rounded normal does), averaged into a rounded
// normal the mesh shader bends its own toward. A new set of rays each frame; temporal antialiasing averages them.

struct BevelPush
{
    float4x4 invViewProj; // as the prepass was drawn
    float4 params;        // z frame seed, w rays per pixel
    float4 size;          // extent, then its inverse
    uint64_t instances;   // TraceInstance per placement in the acceleration structure
};

/** A traced placement's mesh: its vertex and index buffers and world matrix rows. Matches render::TraceInstance. */
struct TraceInstance
{
    uint64_t vertices; // MeshVertex, 64 bytes each: the normal sits 12 bytes in
    uint64_t indices;
    float4 rows[3];
    uint64_t sections; // rtreflect.slang's hit materials
    uint sectionCount;
    uint tint;
};

[[vk::push_constant]] BevelPush bp;
[[vk::binding(0, 0)]] Sampler2D depthTex;
[[vk::binding(1, 0)]] Sampler2D normalTex; // xyz normal, w bevel radius (cm)
[[vk::binding(2, 0)]] RaytracingAccelerationStructure sceneAs;
[[vk::binding(3, 0)]] RWTexture2D<float4> bevelOut; // xyz rounded normal, w 1 where rounded

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
    return float(state & 0xffffff) / 16777216.0;
}

void basis(float3 n, out float3 t, out float3 b)
{
    t = abs(n.y) < 0.99 ? normalize(cross(float3(0, 1, 0), n)) : normalize(cross(float3(1, 0, 0), n));
    b = cross(n, t);
}

/** The smooth normal where a committed hit landed, in world space. */
float3 hitNormal(uint instance, uint primitive, float2 bary)
{
    TraceInstance ti = ((TraceInstance*)bp.instances)[instance];
    uint* idx = (uint*)ti.indices;
    float3 nn = float3(0, 0, 0);
    float w[3] = { 1.0 - bary.x - bary.y, bary.x, bary.y };
    for (uint k = 0; k < 3; ++k)
    {
        float* v = (float*)(ti.vertices + uint64_t(idx[primitive * 3 + k]) * 64);
        nn += float3(v[3], v[4], v[5]) * w[k];
    }
    float3x3 m = float3x3(ti.rows[0].xyz, ti.rows[1].xyz, ti.rows[2].xyz);
    return normalize(mul(m, nn) + 1e-7);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void bevelCS(uint3 id: SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    if (any(px >= int2(bp.size.xy)))
        return;
    float depth = depthTex.Load(int3(px, 0)).r;
    float4 nr = normalTex.Load(int3(px, 0));
    float radius = nr.w;
    if (depth <= 0.0 || radius <= 0.0 || dot(nr.xyz, nr.xyz) < 0.01)
    {
        bevelOut[px] = float4(0, 0, 0, 0);
        return;
    }
    float3 n = normalize(nr.xyz);
    float2 ndc = (float2(px) + 0.5) * bp.size.zw * 2.0 - 1.0;
    float4 h = mul(bp.invViewProj, float4(ndc, depth, 1.0));
    float3 p = h.xyz / h.w;

    float3 t, b;
    basis(n, t, b);
    uint state = hash(uint(px.x) * 7919u + uint(px.y) * 104729u + uint(bp.params.z) * 15485863u);
    const uint rays = uint(max(bp.params.w, 1.0));
    float3 sum = n;
    // A Fibonacci spiral over the disc, turned and shifted per pixel and per frame: even coverage, little noise.
    const float turn = rand(state) * 6.2831853, shift = rand(state);
    for (uint k = 0; k < rays; ++k)
    {
        // Half the rays probe along the normal, a quarter along each tangent, through a disc of the radius.
        uint pick = k % 4;
        float3 axis = pick < 2 ? n : (pick == 2 ? t : b);
        float3 a1, a2;
        basis(axis, a1, a2);
        float r = radius * sqrt(frac((float(k) + shift) / float(rays))), phi = turn + float(k) * 2.3999632;
        float3 disc = a1 * (r * cos(phi)) + a2 * (r * sin(phi));
        for (int side = 0; side < 2; ++side)
        {
            float sgn = side == 0 ? 1.0 : -1.0;
            RayDesc ray;
            ray.Origin = p + disc + axis * (sgn * radius);
            ray.Direction = -axis * sgn;
            ray.TMin = 0.0;
            ray.TMax = 2.0 * radius;
            RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
            q.TraceRayInline(sceneAs, RAY_FLAG_NONE, 0xff, ray);
            while (q.Proceed())
            {
            }
            // Back faces are the probe leaving the surface, not finding it.
            if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT || !q.CommittedTriangleFrontFace())
                continue;
            float3 at = ray.Origin + ray.Direction * q.CommittedRayT();
            if (length(at - p) > radius)
                continue;
            float3 hn = hitNormal(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics());
            // Weighted by the inverse of how likely the three probe directions were to land there.
            float pdf = 0.5 * abs(dot(hn, n)) + 0.25 * abs(dot(hn, t)) + 0.25 * abs(dot(hn, b));
            sum += hn / max(pdf, 0.05);
        }
    }
    bevelOut[px] = float4(normalize(sum), 1.0);
}
