// Infinite ground grid: a fullscreen triangle, ray-cast against the ground plane per pixel.

#include "scene_color.slang"

struct GridPush
{
    float4x4 invViewProj;
    float4x4 viewProj;
    float4 eye;
    float4 params; // x: minor spacing, y: major every N, z: 1 if Z-up, w: display gamma, or below 0 to write display colours as they are
    float4 color;  // grid lines
    float4 axisX;
    float4 axisZ;  // the other ground axis: Z, or Y when Z-up
    float4 origin; // the display origin the grid is drawn around, relative to the eye
    float4 offset; // the eye's ground coordinates from the origin: xy exact for the axes, zw wrapped by a major cell for the lines
};

[[vk::push_constant]] GridPush pc;

struct VSOut
{
    float4 pos : SV_Position;
    float2 ndc;
};

[shader("vertex")]
VSOut gridVS(uint vid: SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.ndc = uv * 2.0 - 1.0;
    o.pos = float4(o.ndc, 0.0, 1.0);
    return o;
}

struct PSOut
{
    float4 color : SV_Target;
    float depth : SV_Depth;
};

// Screen-space derivative length per axis; fwidth over-estimates on diagonals and fattens lines.
float2 pixelSize(float2 c)
{
    return max(float2(length(float2(ddx_fine(c.x), ddy_fine(c.x))), length(float2(ddx_fine(c.y), ddy_fine(c.y)))), 1e-6);
}

float lineCoverage(float2 coord, float spacing)
{
    float2 c = coord / spacing;
    float2 g = abs(frac(c - 0.5) - 0.5) / pixelSize(c);
    return 1.0 - saturate(min(g.x, g.y));
}

// Fades a line set out as it gets denser than a few pixels per cell.
float densityFade(float2 coord, float spacing)
{
    float2 w = pixelSize(coord / spacing);
    return 1.0 - saturate((max(w.x, w.y) - 0.15) * 4.0);
}

[shader("fragment")]
PSOut gridPS(VSOut i)
{
    // Reversed-Z: ndc depth 1 is the near plane, 0 is infinitely far.
    float4 nearH = mul(pc.invViewProj, float4(i.ndc, 1.0, 1.0));
    float4 farH = mul(pc.invViewProj, float4(i.ndc, 0.001, 1.0));
    float3 ro = nearH.xyz / nearH.w;
    float3 rd = normalize(farH.xyz / farH.w - ro);

    bool zUp = pc.params.z > 0.5;
    float3 origin = pc.origin.xyz;
    float roUp = zUp ? ro.z - origin.z : ro.y - origin.y;
    float rdUp = zUp ? rd.z : rd.y;
    // No discard until the end: derivatives below need the whole quad alive.
    float t = -roUp / (abs(rdUp) < 1e-7 ? 1e-7 : rdUp);
    bool hitsGround = t > 0.0;
    t = abs(t);

    float3 p = ro + rd * t;
    // The frame is centred on the eye: ground coordinates are the eye's own plus the hit's offset from it.
    float2 local = zUp ? p.xy - pc.eye.xy : p.xz - pc.eye.xz;
    float2 coord = local + pc.offset.xy;
    float2 lineCoord = local + pc.offset.zw;

    float minor = pc.params.x;
    float major = minor * pc.params.y;
    float minorA = lineCoverage(lineCoord, minor) * densityFade(lineCoord, minor);
    float majorA = lineCoverage(lineCoord, major) * densityFade(lineCoord, major);

    float2 px = pixelSize(coord);
    float alongX = 1.0 - saturate(abs(coord.y) / (px.y * 2.0));
    float alongOther = 1.0 - saturate(abs(coord.x) / (px.x * 2.0));

    // Grid lines fade out under the axes so no light fringe shows.
    float axis = max(alongX, alongOther);
    float nearAxis = max(1.0 - saturate(abs(coord.y) / (px.y * 3.0)), 1.0 - saturate(abs(coord.x) / (px.x * 3.0)));
    float lines = max(minorA * 0.4, majorA * 0.75) * (1.0 - nearAxis);
    float3 axisColor = alongX >= alongOther ? pc.axisX.rgb : pc.axisZ.rgb;
    float3 color = lerp(pc.color.rgb, axisColor, axis);
    float alpha = max(lines, axis);

    float height = abs(zUp ? pc.eye.z - origin.z : pc.eye.y - origin.y);
    float dist = length(p - pc.eye.xyz);
    float fadeEnd = max(3000.0, height * 60.0);
    alpha *= 1.0 - smoothstep(fadeEnd * 0.25, fadeEnd, dist);
    if (!hitsGround || alpha < 0.002)
        discard;

    float4 clip = mul(pc.viewProj, float4(p, 1.0));
    PSOut o;
    o.color = float4(pc.params.w < 0.0 ? color : fromDisplay(color, pc.params.w), alpha);
    o.depth = clip.z / clip.w;
    return o;
}
