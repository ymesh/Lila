// Eye-dome lighting: each point pixel darkened by how much nearer its neighbours are, in log depth, so a cloud without
// normals reads as shaded surfaces. Writes the points' depth back so the scene's own geometry still hides them.
struct EdlPush
{
    float4x4 invProj; // clip to view, for linear depth
    float4 params;    // x radius in pixels, y sample count
};
[[vk::push_constant]] EdlPush ep;
[[vk::binding(0, 0)]] Sampler2D cloudColor;
[[vk::binding(1, 0)]] Sampler2D cloudDepth;

struct EdlOut
{
    float4 pos : SV_Position;
};

[shader("vertex")]
EdlOut edlVS(uint vid: SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    EdlOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float logDepth(int2 px, int2 size)
{
    px = clamp(px, int2(0, 0), size - 1);
    const float d = cloudDepth.Load(int3(px, 0)).r;
    if (d <= 0.0)
        return -1.0;
    const float2 ndc = (float2(px) + 0.5) / float2(size) * 2.0 - 1.0;
    const float4 v = mul(ep.invProj, float4(ndc, d, 1.0));
    return log2(max(abs(v.z / v.w), 1e-6));
}

struct EdlPS
{
    float4 color : SV_Target;
    float depth : SV_Depth;
};

[shader("fragment")]
EdlPS edlPS(EdlOut i)
{
    uint w, h;
    cloudDepth.GetDimensions(w, h);
    const int2 size = int2(int(w), int(h));
    const int2 px = int2(i.pos.xy);
    const float4 c = cloudColor.Load(int3(px, 0));
    if (c.a <= 0.0)
        discard;
    const float centre = logDepth(px, size);
    const int count = int(ep.params.y);
    float response = 0.0;
    for (int k = 0; k < count; ++k)
    {
        const float a = 6.28318530718 * float(k) / float(count);
        const float n = logDepth(px + int2(round(float2(cos(a), sin(a)) * ep.params.x)), size);
        // A neighbour nearer the eye shades this point; empty neighbours (the cloud's silhouette) count as far.
        if (n > 0.0)
            response += max(0.0, centre - n);
    }
    response /= float(count);
    // The strength travels in alpha (0 to 1 maps 0 to 4).
    const float shade = exp(-response * 300.0 * c.a * 4.0);
    EdlPS o;
    o.color = float4(c.rgb * shade, 1.0);
    o.depth = cloudDepth.Load(int3(px, 0)).r;
    return o;
}
