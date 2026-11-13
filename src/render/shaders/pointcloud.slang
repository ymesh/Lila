// Streamed point clouds: one octree node per draw, its packed points read by vertex index, drawn as squares of a constant
// pixel size in the scene pass. Positions are 16-bit within the node's cube, mapped to clip space by a camera-relative mvp.

#include "scene_color.slang"

struct CloudPush
{
    float4x4 mvp;      // node cube (0 to 65535) to clip, relative to the eye
    float4x4 toCloud;  // node cube (0 to 65535) to the cloud's space, for clipping and elevation
    float4 color;      // rgb single colour (linear), w point size in device pixels
    float4 tint;       // rgb highlight (linear), w its mix
    float4 elevation;  // x up axis (1 Y, 2 Z), y min, z 1 / span, w exposure
    float4x4* clips;   // cloud space to box space, inside where |x|, |y|, |z| <= 0.5
    uint* points;      // three words a point
    uint clipCount;
    uint clipMode;     // 0 off, 1 keep inside, 2 cut inside
    uint colorMode;    // 0 rgb, 1 intensity, 2 elevation, 3 classification, 4 single
    float edl;         // eye-dome lighting strength, carried in alpha to the shading pass; 0 off
};

[[vk::push_constant]] CloudPush pc;

struct VSOut
{
    float4 pos : SV_Position;
    nointerpolation float3 color : COLOR;
    float size : SV_PointSize;
};

float3 srgbToLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

/** A compact turbo-like ramp for elevation: blue, cyan, green, yellow, red. */
float3 ramp(float t)
{
    t = saturate(t);
    const float3 a = float3(0.18, 0.25, 0.85), b = float3(0.10, 0.80, 0.85), c = float3(0.25, 0.85, 0.25), d = float3(0.95, 0.85, 0.15),
                 e = float3(0.85, 0.20, 0.10);
    if (t < 0.25)
        return lerp(a, b, t * 4.0);
    if (t < 0.5)
        return lerp(b, c, (t - 0.25) * 4.0);
    if (t < 0.75)
        return lerp(c, d, (t - 0.5) * 4.0);
    return lerp(d, e, (t - 0.75) * 4.0);
}

/** ASPRS LAS classes in the colours survey tools use. */
float3 classColor(uint c)
{
    switch (c)
    {
    case 2: return float3(0.45, 0.30, 0.15);  // ground
    case 3: return float3(0.55, 0.85, 0.35);  // low vegetation
    case 4: return float3(0.25, 0.70, 0.20);  // medium vegetation
    case 5: return float3(0.10, 0.45, 0.10);  // high vegetation
    case 6: return float3(0.85, 0.25, 0.20);  // building
    case 7: return float3(0.95, 0.10, 0.95);  // noise
    case 9: return float3(0.15, 0.40, 0.95);  // water
    case 10: return float3(0.55, 0.55, 0.55); // rail
    case 11: return float3(0.30, 0.30, 0.30); // road
    case 17: return float3(0.95, 0.65, 0.10); // bridge
    default: return float3(0.75, 0.75, 0.75);
    }
}

[shader("vertex")]
VSOut cloudVS(uint id : SV_VertexID)
{
    VSOut o;
    const uint w0 = pc.points[id * 3], w1 = pc.points[id * 3 + 1], w2 = pc.points[id * 3 + 2];
    const float3 q = float3(float(w0 & 0xffffu), float(w0 >> 16), float(w1 & 0xffffu));
    const float3 local = mul(pc.toCloud, float4(q, 1.0)).xyz;
    bool keep = true;
    if (pc.clipMode != 0 && pc.clipCount > 0)
    {
        bool inside = false;
        for (uint k = 0; k < pc.clipCount; ++k)
        {
            const float3 b = abs(mul(pc.clips[k], float4(local, 1.0)).xyz);
            inside = inside || all(b <= 0.5);
        }
        keep = inside == (pc.clipMode == 1);
    }
    o.pos = mul(pc.mvp, float4(q, 1.0));
    if (!keep || o.pos.w <= 1e-6 || any(abs(o.pos.xy) > 2.0 * o.pos.w))
        o.pos = float4(2.0, 2.0, 2.0, 1.0);
    const float3 rgb = float3(float((w1 >> 16) & 0xffu), float(w1 >> 24), float(w2 & 0xffu)) / 255.0;
    const uint cls = (w2 >> 8) & 0xffu;
    const float intensity = float(w2 >> 16) / 65535.0;
    float3 c;
    switch (pc.colorMode)
    {
    case 1: c = float3(sqrt(intensity)); break;
    case 2: c = ramp(((pc.elevation.x > 1.5 ? local.z : local.y) - pc.elevation.y) * pc.elevation.z); break;
    case 3: c = classColor(cls); break;
    case 4: c = pc.color.rgb; break;
    default: c = srgbToLinear(rgb); break;
    }
    o.color = lerp(c, pc.tint.rgb, pc.tint.w) * pc.elevation.w;
    o.size = pc.color.w;
    return o;
}

[shader("fragment")]
float4 cloudPS(nointerpolation float3 color : COLOR) : SV_Target
{
    return float4(sceneCompress(color), 1.0);
}

/** Into the clouds' own target for eye-dome lighting: the strength in alpha, which also marks the pixel as a point. */
[shader("fragment")]
float4 cloudEdlTargetPS(nointerpolation float3 color : COLOR) : SV_Target
{
    return float4(sceneCompress(color), max(saturate(pc.edl * 0.25), 1.0 / 255.0));
}
