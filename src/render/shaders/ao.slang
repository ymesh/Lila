// Ground-truth ambient occlusion (Jimenez et al. 2016, after Intel's XeGTAO) from a single-sample depth and normal
// prepass, then an edge-aware blur that averages away the 4x4 noise pattern. No temporal accumulation: the viewport
// only draws on demand, so each frame has to be clean on its own.

struct AoPush
{
    float4x4 invProj;
    float4x4 view;  // world to view, for the prepass normals
    float4 params;  // x radius (world units), y intensity, z pixels per world unit at view depth 1, w 1 if orthographic
    float4 size;    // xy extent, zw 1 / extent
    float4 options; // x: 1 to take normals from the depth instead of the prepass
};

[[vk::push_constant]] AoPush ap;

[[vk::binding(0, 0)]] Sampler2D depthTex;  // reversed-Z, 0 is the background
[[vk::binding(1, 0)]] Sampler2D normalTex; // world-space normal, zero where nothing was drawn
[[vk::binding(2, 0)]] Sampler2D aoIn;
[[vk::binding(3, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> aoOut;

static const float PI = 3.14159265;
static const float HALF_PI = 1.57079633;
static const int kSlices = 4;
static const int kSteps = 8;
static const float kMaxPixelRadius = 256.0;

float3 viewPos(int2 px, float depth)
{
    float2 uv = (float2(px) + 0.5) * ap.size.zw;
    float4 h = mul(ap.invProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    return h.xyz / h.w;
}

/** World-space normal: the prepass's, or from the depth around px, using the nearer neighbour on each axis so edges stay sharp. */
float3 worldNormal(int2 px, float depth)
{
    if (ap.options.x < 0.5)
    {
        float3 n = normalTex.Load(int3(px, 0)).xyz;
        return dot(n, n) > 0.01 ? normalize(n) : n;
    }
    const int2 last = int2(ap.size.xy) - 1;
    float3 P = viewPos(px, depth);
    float3 dx = float3(0, 0, 0), dy = float3(0, 0, 0);
    for (int axis = 0; axis < 2; ++axis)
    {
        int2 step = axis == 0 ? int2(1, 0) : int2(0, 1);
        int2 a = clamp(px - step, int2(0), last), b = clamp(px + step, int2(0), last);
        float da = depthTex.Load(int3(a, 0)).r, db = depthTex.Load(int3(b, 0)).r;
        float3 Pa = da > 0.0 ? viewPos(a, da) : P, Pb = db > 0.0 ? viewPos(b, db) : P;
        float3 d = (db > 0.0 && (da <= 0.0 || abs(Pb.z - P.z) < abs(P.z - Pa.z))) ? Pb - P : P - Pa;
        if (axis == 0)
            dx = d;
        else
            dy = d;
    }
    float3 n = cross(dy, dx);
    if (dot(n, n) < 1e-20)
        return float3(0, 0, 0);
    n = normalize(n);
    bool ortho = ap.params.w > 0.5;
    if (dot(n, ortho ? float3(0, 0, 1) : -P) < 0.0)
        n = -n;
    // Back to world space: the view matrix's rotation is orthonormal.
    return mul(n, (float3x3)ap.view);
}

float bayer4(int2 p)
{
    const uint m[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    return float(m[(p.y & 3) * 4 + (p.x & 3)]);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void gtaoCS(uint3 id: SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    if (any(px >= int2(ap.size.xy)))
        return;
    float depth = depthTex.Load(int3(px, 0)).r;
    float3 nw = depth > 0.0 ? worldNormal(px, depth) : float3(0, 0, 0);
    if (depth <= 0.0 || dot(nw, nw) < 0.01)
    {
        aoOut[px] = 1.0;
        return;
    }

    float3 P = viewPos(px, depth);
    float3 N = normalize(mul((float3x3)ap.view, nw));
    bool ortho = ap.params.w > 0.5;
    float3 V = ortho ? float3(0, 0, 1) : normalize(-P);
    float radius = ap.params.x;
    float pixelRadius = min(radius * ap.params.z / (ortho ? 1.0 : max(-P.z, 1e-4)), kMaxPixelRadius);
    if (pixelRadius < 1.0)
    {
        aoOut[px] = 1.0;
        return;
    }

    // Occluders fade out over the outer 60% of the radius instead of popping at its edge.
    float falloffRange = 0.615 * radius;
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = (radius - falloffRange) / falloffRange + 1.0;

    float n16 = bayer4(px);
    float sliceJitter = (n16 + 0.5) / 16.0;
    float stepJitter = (float((uint(n16) * 7u + 3u) & 15u) + 0.5) / 16.0;

    float visibility = 0.0;
    for (int slice = 0; slice < kSlices; ++slice)
    {
        float phi = (float(slice) + sliceJitter) * PI / float(kSlices);
        float3 dir = float3(cos(phi), sin(phi), 0.0);
        float2 omega = float2(dir.x, -dir.y); // screen rows run down

        float3 orthoDir = dir - dot(dir, V) * V;
        float3 axis = normalize(cross(orthoDir, V));
        float3 projN = N - axis * dot(N, axis);
        float projLen = length(projN);
        if (projLen < 1e-4)
            continue;
        float cosN = saturate(dot(projN, V) / projLen);
        float n = (dot(orthoDir, projN) < 0.0 ? -1.0 : 1.0) * acos(cosN);

        float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI);
        float horizon0 = low0, horizon1 = low1;
        for (int step = 0; step < kSteps; ++step)
        {
            float s = (float(step) + stepJitter) / float(kSteps);
            float dist = s * s * pixelRadius + 1.3;
            float2 offset = omega * dist;
            int2 p0 = int2(floor(float2(px) + 0.5 + offset));
            int2 p1 = int2(floor(float2(px) + 0.5 - offset));
            p0 = clamp(p0, int2(0), int2(ap.size.xy) - 1);
            p1 = clamp(p1, int2(0), int2(ap.size.xy) - 1);
            float d0 = depthTex.Load(int3(p0, 0)).r;
            float d1 = depthTex.Load(int3(p1, 0)).r;
            if (d0 > 0.0)
            {
                float3 delta = viewPos(p0, d0) - P;
                float len = length(delta);
                float shc = dot(delta / max(len, 1e-6), V);
                horizon0 = max(horizon0, lerp(low0, shc, saturate(len * falloffMul + falloffAdd)));
            }
            if (d1 > 0.0)
            {
                float3 delta = viewPos(p1, d1) - P;
                float len = length(delta);
                float shc = dot(delta / max(len, 1e-6), V);
                horizon1 = max(horizon1, lerp(low1, shc, saturate(len * falloffMul + falloffAdd)));
            }
        }

        float h0 = n + clamp(-acos(horizon1) - n, -HALF_PI, HALF_PI);
        float h1 = n + clamp(acos(horizon0) - n, -HALF_PI, HALF_PI);
        float arc0 = (cosN + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) * 0.25;
        float arc1 = (cosN + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) * 0.25;
        visibility += projLen * (arc0 + arc1);
    }
    aoOut[px] = saturate(visibility / float(kSlices));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void denoiseCS(uint3 id: SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    if (any(px >= int2(ap.size.xy)))
        return;
    float depth = depthTex.Load(int3(px, 0)).r;
    float3 nc = depth > 0.0 ? worldNormal(px, depth) : float3(0, 0, 0);
    if (depth <= 0.0 || dot(nc, nc) < 0.01)
    {
        aoOut[px] = 1.0;
        return;
    }
    float3 pc = viewPos(px, depth);
    float3 ncv = normalize(mul((float3x3)ap.view, nc));
    // Distance from this pixel's tangent plane rather than depth difference, so a face seen at a grazing angle still
    // averages its whole neighbourhood and the 4x4 noise pattern cancels out.
    float tolerance = 0.05 * ap.params.x + 0.002 * abs(pc.z);

    float sum = 0.0, weights = 0.0;
    // Exactly one 4x4 cell, so every value of the noise pattern is averaged once and it cancels.
    for (int y = -2; y <= 1; ++y)
        for (int x = -2; x <= 1; ++x)
        {
            int2 q = clamp(px + int2(x, y), int2(0), int2(ap.size.xy) - 1);
            float dq = depthTex.Load(int3(q, 0)).r;
            if (dq <= 0.0)
                continue;
            float3 nq = worldNormal(q, dq);
            float w = saturate(1.0 - abs(dot(viewPos(q, dq) - pc, ncv)) / tolerance);
            float nd = saturate(dot(nq, nc));
            w *= nd * nd * nd * nd;
            sum += aoIn.Load(int3(q, 0)).r * w;
            weights += w;
        }
    float ao = weights > 1e-4 ? sum / weights : aoIn.Load(int3(px, 0)).r;
    aoOut[px] = pow(saturate(ao), ap.params.y * 2.0);
}
