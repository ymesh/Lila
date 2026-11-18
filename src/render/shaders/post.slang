// Preview-only viewport post: an anisotropic Kuwahara filter (Kyprianidis et al. 2009, polynomial sector weights from
// their 2011 follow-up) for a painted look, then outlines from the single-sample prepass depth and normals.

struct PostPush
{
    float4 size;    // xy extent, zw 1 / extent
    float4 params;  // x Kuwahara on, y radius in pixels, z outlines on, w outline width in pixels
    float4 outline; // rgb linear colour, a: depth sensitivity
};

[[vk::push_constant]] PostPush pc;

[[vk::binding(0, 0)]] Sampler2D sceneTex;  // resolved scene colour
[[vk::binding(1, 0)]] Sampler2D depthTex;  // prepass depth, reversed-Z
[[vk::binding(2, 0)]] Sampler2D normalTex; // prepass world normal, zero where nothing was drawn

struct PostOut
{
    float4 pos : SV_Position;
};

[shader("vertex")]
PostOut postVS(uint vid: SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    PostOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float3 sceneAt(int2 p)
{
    return sceneTex.Load(int3(clamp(p, int2(0), int2(pc.size.xy) - 1), 0)).rgb;
}

float luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

/** Smoothed structure tensor of the luminance around p: (E, G, F) of [[E F] [F G]]. */
float3 structureTensor(int2 p)
{
    float3 t = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            int2 q = p + int2(x, y) * 2;
            float tl = luma(sceneAt(q + int2(-1, -1))), tc = luma(sceneAt(q + int2(0, -1))), tr = luma(sceneAt(q + int2(1, -1)));
            float ml = luma(sceneAt(q + int2(-1, 0))), mr = luma(sceneAt(q + int2(1, 0)));
            float bl = luma(sceneAt(q + int2(-1, 1))), bc = luma(sceneAt(q + int2(0, 1))), br = luma(sceneAt(q + int2(1, 1)));
            float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
            float gy = (bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr);
            float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
            t += w * float3(gx * gx, gy * gy, gx * gy);
        }
    return t / 16.0;
}

float3 kuwahara(int2 p, float radius)
{
    float3 st = structureTensor(p);
    float e = st.x, g = st.y, f = st.z;
    float root = sqrt(max((e - g) * (e - g) + 4.0 * f * f, 0.0));
    float l1 = 0.5 * (e + g + root), l2 = 0.5 * (e + g - root);
    float2 dir = float2(l1 - e, -f);
    float phi = dot(dir, dir) > 1e-12 ? atan2(dir.y, dir.x) : 0.0;
    float aniso = l1 + l2 > 1e-8 ? (l1 - l2) / (l1 + l2) : 0.0;

    const float alpha = 1.0;
    float a = radius * clamp((alpha + aniso) / alpha, 0.1, 2.0);
    float b = radius * clamp(alpha / (alpha + aniso), 0.1, 2.0);
    float c = cos(phi), s = sin(phi);
    float2x2 sr = float2x2(0.5 / a * c, 0.5 / a * -s, 0.5 / b * s, 0.5 / b * c);
    int maxX = int(sqrt(a * a * c * c + b * b * s * s));
    int maxY = int(sqrt(a * a * s * s + b * b * c * c));

    const float zeroCross = 0.58;
    float zeta = 2.0 / max(radius, 1.0);
    float sinZero = sin(zeroCross);
    float eta = (zeta + cos(zeroCross)) / (sinZero * sinZero);

    float4 m[8];
    float3 sq[8];
    for (int k = 0; k < 8; ++k)
    {
        m[k] = 0.0;
        sq[k] = 0.0;
    }
    for (int y = -maxY; y <= maxY; ++y)
        for (int x = -maxX; x <= maxX; ++x)
        {
            float2 v = mul(sr, float2(x, y));
            if (dot(v, v) > 0.25)
                continue;
            float3 col = sceneAt(p + int2(x, y));
            float w[8];
            float sum = 0.0;
            float vxx = zeta - eta * v.x * v.x, vyy = zeta - eta * v.y * v.y;
            float z = max(0.0, v.y + vxx);
            w[0] = z * z;
            z = max(0.0, -v.x + vyy);
            w[2] = z * z;
            z = max(0.0, -v.y + vxx);
            w[4] = z * z;
            z = max(0.0, v.x + vyy);
            w[6] = z * z;
            float2 r = 0.70710678 * float2(v.x - v.y, v.x + v.y);
            vxx = zeta - eta * r.x * r.x;
            vyy = zeta - eta * r.y * r.y;
            z = max(0.0, r.y + vxx);
            w[1] = z * z;
            z = max(0.0, -r.x + vyy);
            w[3] = z * z;
            z = max(0.0, -r.y + vxx);
            w[5] = z * z;
            z = max(0.0, r.x + vyy);
            w[7] = z * z;
            for (int k = 0; k < 8; ++k)
                sum += w[k];
            float gauss = exp(-3.125 * dot(v, v)) / max(sum, 1e-6);
            for (int k = 0; k < 8; ++k)
            {
                float wk = w[k] * gauss;
                m[k] += float4(col * wk, wk);
                sq[k] += col * col * wk;
            }
        }

    float3 out = 0.0;
    float total = 0.0;
    for (int k = 0; k < 8; ++k)
    {
        if (m[k].w <= 0.0)
            continue;
        float3 mean = m[k].rgb / m[k].w;
        float3 variance = abs(sq[k] / m[k].w - mean * mean);
        float sigma2 = variance.r + variance.g + variance.b;
        float w = 1.0 / (1.0 + pow(8.0 * 1000.0 * sigma2, 0.5 * 8.0));
        out += mean * w;
        total += w;
    }
    return total > 0.0 ? out / total : sceneAt(p);
}

/** 0..1: how much p sits on a depth or normal discontinuity within width pixels. */
float outlineAt(int2 p, float width)
{
    int2 hi = int2(pc.size.xy) - 1;
    float d0 = depthTex.Load(int3(p, 0)).r;
    float3 n0 = normalTex.Load(int3(p, 0)).xyz;
    n0 = dot(n0, n0) > 0.01 ? normalize(n0) : n0;
    int r = max(int(width + 0.5), 1);
    float edge = 0.0;
    const int2 offsets[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
    for (int i = 0; i < 4; ++i)
    {
        int2 q = clamp(p + offsets[i] * r, int2(0), hi);
        float d = depthTex.Load(int3(q, 0)).r;
        float3 n = normalTex.Load(int3(q, 0)).xyz;
        n = dot(n, n) > 0.01 ? normalize(n) : n;
        // Reversed-Z: relative change in depth, so the threshold holds near and far.
        float dd = abs(d - d0) / max(max(d, d0), 1e-9);
        edge = max(edge, smoothstep(0.05, 0.15, dd * pc.outline.a));
        bool hit0 = dot(n0, n0) > 0.5, hit = dot(n, n) > 0.5;
        if (hit0 != hit)
            edge = 1.0;
        else if (hit0)
            edge = max(edge, smoothstep(0.75, 0.5, dot(n0, n)));
    }
    return edge;
}

[shader("fragment")]
float4 postPS(PostOut i) : SV_Target
{
    int2 p = int2(i.pos.xy);
    float3 c = pc.params.x > 0.5 ? kuwahara(p, max(pc.params.y, 1.0)) : sceneAt(p);
    if (pc.params.z > 0.5)
        c = lerp(c, pc.outline.rgb, outlineAt(p, pc.params.w));
    return float4(c, 1.0);
}
