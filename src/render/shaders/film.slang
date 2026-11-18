// Viewport depth of field and motion blur on the picture the compose pass reads, after temporal antialiasing. Depth of
// field gathers each pixel's neighbours whose blur reaches it, over the aperture's shape (blades, anamorphic squeeze)
// cut by the lens barrel toward the corners (cat's eye); motion blur smears each pixel along its screen motion over the
// shutter, from the camera's movement since the last frame. Renders keep their accumulated lens and shutter instead.

#include "scene_color.slang"

/** Matches render::FilmParams. */
struct FilmParams
{
    float4x4 invViewProj;  // this frame, as drawn
    float4x4 prevViewProj; // last frame, in this frame's camera-relative space
    float4 size;           // extent, then its inverse
    float4 dof;            // aperture radius (cm), focus distance (cm), pixels per cm at 1 cm away, largest blur (pixels)
    float4 dof2;           // blades (0 round), blade rotation (radians), anamorphic squeeze, cat's eye (0 to 1)
    float4 mb;             // shutter (share of a frame), longest smear (pixels)
    float4 depthToView;    // the inverse projection terms taking reversed-Z to distance along the view
};

struct FilmPush
{
    FilmParams* params;
    uint pad[2];
};

[[vk::push_constant]] FilmPush fpc;
#define fp (*fpc.params)

[[vk::binding(0, 0)]] Sampler2D colorTex; // compressed, linear filtering
[[vk::binding(1, 0)]] Sampler2D depthTex; // reversed-Z
[[vk::binding(2, 0)]] RWTexture2D<float4> filmOut;

static const float PI = 3.14159265;

float3 worldAt(float2 uv, float depth)
{
    float4 h = mul(fp.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    return h.xyz / h.w;
}

/** The blur radius in pixels of a point at distance z (cm): the aperture's spread on the focus plane, seen from the eye. */
float coc(float z)
{
    float f = max(fp.dof.y, 1e-3);
    float spread = fp.dof.x * abs(z - f) / max(z, 1e-3);
    return min(spread * fp.dof.z / f, fp.dof.w);
}

/** Distance along the view (cm) of a pixel's depth; far away where nothing was drawn. */
float distanceAt(int2 px)
{
    float d = depthTex.Load(int3(px, 0)).r;
    if (d <= 0.0)
        return 1e9;
    const float4 k = fp.depthToView;
    return -(k.x * d + k.y) / (k.z * d + k.w);
}

/** A point of the unit disc pushed out to the aperture's polygon (blades) and squeezed for an anamorphic lens. */
float2 apertureShape(float2 p)
{
    float n = fp.dof2.x;
    if (n >= 3.0)
    {
        float a = atan2(p.y, p.x) - fp.dof2.y;
        float seg = 2.0 * PI / n;
        p *= cos(PI / n) / cos(a - seg * floor(a / seg + 0.5));
    }
    return p * float2(1.0 / max(fp.dof2.z, 1.0), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void dofCS(uint3 id: SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    if (any(px >= int2(fp.size.xy)))
        return;
    const float zc = distanceAt(px);
    const float cc = coc(zc);
    const float2 uv = (float2(px) + 0.5) * fp.size.zw;
    float4 centre = colorTex.SampleLevel(uv, 0);
    float3 sum = sceneExpand(centre.rgb);
    float total = 1.0;
    const float R = fp.dof.w;
    // The cat's eye: the barrel cuts the aperture from the side away from the centre, more toward the corners.
    const float2 cut = (uv - 0.5) * 2.0 * fp.dof2.w;
    const int kSamples = 40;
    // Each pixel turns the spiral by its own angle (interleaved gradient noise), so taps leave fine noise, not rings.
    const float spin = 6.2831853 * frac(52.9829189 * frac(dot(float2(px), float2(0.06711056, 0.00583715))));
    for (int k = 1; k < kSamples; ++k)
    {
        float r = sqrt((float(k) + 0.5) / float(kSamples));
        float a = float(k) * 2.39996 + spin;
        float2 unit = float2(cos(a), sin(a)) * r;
        if (length(unit + cut) > 1.0)
            continue;
        float2 o = apertureShape(unit) * R;
        float dist = length(o);
        int2 q = clamp(px + int2(round(o)), int2(0), int2(fp.size.xy) - 1);
        float zs = distanceAt(q);
        float cs = coc(zs);
        // A neighbour counts where its own blur reaches this pixel; one behind an in-focus pixel only as far as ours does.
        float w = zs <= zc ? saturate(cs - dist + 1.0) : saturate(min(cs, cc) - dist + 1.0);
        if (w <= 0.0)
            continue;
        sum += sceneExpand(colorTex.Load(int3(q, 0)).rgb) * w;
        total += w;
    }
    filmOut[px] = float4(sceneCompress(sum / total), centre.a);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void motionCS(uint3 id: SV_DispatchThreadID)
{
    int2 px = int2(id.xy);
    if (any(px >= int2(fp.size.xy)))
        return;
    const float2 uv = (float2(px) + 0.5) * fp.size.zw;
    float depth = depthTex.Load(int3(px, 0)).r;
    float4 centre = colorTex.SampleLevel(uv, 0);
    float2 velocity = 0.0;
    if (depth > 0.0)
    {
        float4 prev = mul(fp.prevViewProj, float4(worldAt(uv, depth), 1.0));
        if (prev.w > 0.0)
            velocity = (uv - (prev.xy / prev.w * 0.5 + 0.5)) * fp.size.xy * fp.mb.x;
    }
    else
    {
        // The background moves with the camera's turn: a point far along the pixel's ray.
        float4 far = mul(fp.prevViewProj, float4(worldAt(uv, 1e-6), 1.0));
        if (far.w > 0.0)
            velocity = (uv - (far.xy / far.w * 0.5 + 0.5)) * fp.size.xy * fp.mb.x;
    }
    float len = length(velocity);
    if (len < 0.5)
    {
        filmOut[px] = centre;
        return;
    }
    velocity *= min(len, fp.mb.y) / len;
    float3 sum = 0.0;
    float total = 0.0;
    const int kTaps = 16;
    for (int t = 0; t < kTaps; ++t)
    {
        float f = (float(t) + 0.5) / float(kTaps) - 0.5;
        float2 s = uv + velocity * f * fp.size.zw;
        float ds = depthTex.SampleLevel(s, 0).r;
        // Nearer things smear over farther ones, not the other way round.
        float w = ds >= depth * 0.98 || depth <= 0.0 ? 1.0 : 0.25;
        sum += sceneExpand(colorTex.SampleLevel(s, 0).rgb) * w;
        total += w;
    }
    filmOut[px] = float4(sceneCompress(sum / max(total, 1e-4)), centre.a);
}
