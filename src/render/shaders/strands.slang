// Hair strands: each segment a camera-facing ribbon at its world width, shaded with Kajiya-Kay along its tangent.
// Thinner than a pixel, a ribbon draws a pixel wide and dithers to its coverage (temporal antialiasing averages it);
// strands ranked at or past the draw's keep are dropped for distance.

#include "scene_color.slang"

/** A cloud's buffers: points (xyz, width), segments (first point, strand), strands (first point, count, rank bits, colour). */
struct StrandHeader
{
    float4* points;
    uint2* segments;
    uint4* strands;
};

struct StrandsPush
{
    float4x4 mvp;
    /** Object space: xyz the eye, or for an orthographic view the direction it looks; w 1 orthographic. */
    float4 eye;
    /** x keep, y width scale, z exposure, w pixels per object unit at distance 1 (the view's height * |proj[1][1]| / 2). */
    float4 params;
    /** xyz object-space up for the key light, w the highlight's RGBA8 bits (0 none). */
    float4 upTint;
    StrandHeader* header;
    uint frame;
    uint pad;
};

[[vk::push_constant]] StrandsPush pc;

struct VSOut
{
    float4 pos : SV_Position;
    float3 tangent : TANGENT;
    float3 view : VIEW;
    float t : PARAM;
    nointerpolation float coverage : COVERAGE;
    nointerpolation float3 color : COLOR;
    nointerpolation float seed : SEED;
};

float3 srgbToLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

[shader("vertex")]
VSOut strandsVS(uint id : SV_VertexID)
{
    VSOut o;
    // Six vertices a segment, two triangles: corners (end, side) of the ribbon.
    const uint2 corners[6] = {uint2(0, 0), uint2(1, 0), uint2(1, 1), uint2(0, 0), uint2(1, 1), uint2(0, 1)};
    const uint2 c = corners[id % 6];
    const StrandHeader h = *pc.header;
    const uint2 seg = h.segments[id / 6];
    const uint4 strand = h.strands[seg.y];
    const float rank = asfloat(strand.z);
    if (rank >= pc.params.x)
    {
        o.pos = float4(2.0, 2.0, 2.0, 1.0);
        return o;
    }
    const float4 a = h.points[seg.x], b = h.points[seg.x + 1];
    const float last = float(max(strand.y, 2u) - 1u);
    const float3 p = c.x == 0 ? a.xyz : b.xyz;
    float width = (c.x == 0 ? a.w : b.w) * pc.params.y;
    float3 t = b.xyz - a.xyz;
    t = dot(t, t) > 1e-20 ? normalize(t) : float3(0.0, 1.0, 0.0);
    const bool ortho = pc.eye.w > 0.5;
    const float3 toEye = ortho ? -pc.eye.xyz : pc.eye.xyz - p;
    const float3 view = dot(toEye, toEye) > 1e-20 ? normalize(toEye) : float3(0.0, 0.0, 1.0);
    float3 side = cross(t, view);
    side = dot(side, side) > 1e-12 ? normalize(side) : normalize(cross(t, abs(t.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0)));
    const float4 centre = mul(pc.mvp, float4(p, 1.0));
    const float pixels = width * pc.params.w / (ortho ? 1.0 : max(centre.w, 1e-6));
    o.coverage = saturate(pixels);
    if (pixels < 1.0)
        width /= max(pixels, 0.05);
    o.pos = mul(pc.mvp, float4(p + side * ((c.y == 0 ? -0.5 : 0.5) * width), 1.0));
    o.tangent = t;
    o.view = view;
    o.t = float(seg.x + c.x - strand.x) / last;
    const uint color = strand.w;
    o.color = srgbToLinear(float3(color & 0xff, (color >> 8) & 0xff, (color >> 16) & 0xff) / 255.0);
    o.seed = rank;
    return o;
}

float hash(float3 p)
{
    p = frac(p * float3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return frac((p.x + p.y) * p.z);
}

[shader("fragment")]
float4 strandsPS(VSOut i) : SV_Target
{
    if (i.coverage < 1.0 && hash(float3(i.pos.xy, i.seed * 977.0 + float(pc.frame))) >= i.coverage)
        discard;
    const float3 t = normalize(i.tangent);
    const float3 v = normalize(i.view);
    const float3 l = normalize(v * 0.6 + normalize(pc.upTint.xyz));
    const float3 h = normalize(l + v);
    const float tl = dot(t, l), th = dot(t, h);
    const float diffuse = lerp(0.3, 1.0, sqrt(saturate(1.0 - tl * tl)));
    const float sinTH = sqrt(saturate(1.0 - th * th));
    // Two lobes: a sharp white one and a broader one tinted by the hair.
    const float primary = pow(sinTH, 90.0) * 0.3;
    const float secondary = pow(sinTH, 18.0) * 0.25;
    float3 base = i.color * lerp(0.35, 1.0, smoothstep(0.0, 0.3, i.t));
    const uint tint = asuint(pc.upTint.w);
    if (tint != 0u)
        base = lerp(base, float3(tint & 0xff, (tint >> 8) & 0xff, (tint >> 16) & 0xff) / 255.0, 0.4);
    const float3 c = base * (diffuse * 0.85 + 0.15) + primary + secondary * base * 2.0;
    return float4(sceneCompress(c * pc.params.z), 1.0);
}
