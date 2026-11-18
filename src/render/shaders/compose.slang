// Scene target to display: expands the scene (or its temporal result), applies exposure, bloom and the post stack, and
// tonemaps into the frame before lines and overlays draw over it.

#include "scene_color.slang"
#include "tonemap.slang"

/** The compose pass's settings, read through the push constant's address; matches render::ComposeParams. */
struct ComposeParams
{
    float4 params; // x: display gamma, y: 1 to dither, z: exposure, w: tonemapper (0 neutral, 1 AgX, 2 ACES, 3 linear, 4 OCIO view)
    float4 grade;  // x: contrast, y: saturation, z: 1 for the post stack, w: AgX look (0 none, 1 punchy, 2 golden)
    float4 white;  // rgb: white balance gains, w: LUT intensity (0 skips it)
    float4 lift;   // rgb, w: vignette
    float4 gammaC; // rgb, w: film grain
    float4 gain;   // rgb, w: grain seed
    float4 lens;   // x: chromatic aberration (pixels at the corner), y: lens distortion, z: sharpen, w: bloom intensity
    float4 size;   // extent, then its inverse
    float4 misc;   // x: 1 to apply the auto exposure, y: bloom scale (one over its levels), zw: the view LUT's log2 shaper range
    float4 offset;     // rgb, w: offset master
    float4 masters;    // lift, gamma, gain masters, w: 1 when curvesTex holds curves
    float4 shadows;    // rgb gains, w: the shadows' upper luminance
    float4 midtones;   // rgb gains
    float4 highlights; // rgb gains, w: the highlights' lower luminance
    float4 flare;      // x 1 to add flareTex, y lens dirt intensity, z 1 when dirtTex holds dirt
    float4 halation;   // intensity, threshold, radius, 0
    float4 grain;      // size (pixels), colour amount, luminance response, 0
    float4 letterbox;  // aspect (0 none), opacity
    float4 halationTint; // rgb
};

struct ComposePush
{
    ComposeParams* params;
    uint pad[2];
};

[[vk::push_constant]] ComposePush cpc;
#define cp (*cpc.params)
[[vk::binding(0, 0)]] Sampler2D composeTex; // compressed scene, linear filtering
[[vk::binding(1, 0)]] Sampler2D bloomTex;   // half resolution, expanded radiance
[[vk::binding(2, 0)]] Sampler3D lutTex;     // display-encoded in and out
[[vk::binding(3, 0)]] Sampler2D exposureTex; // exposure.slang's state: texel 0 is the multiplier
[[vk::binding(4, 0)]] Sampler3D viewLutTex;  // tonemapper 4: an OCIO view over a log2 shaper, display code values out
[[vk::binding(5, 0)]] Sampler2D curvesTex;   // grading curves, a row each (render::grade::Curve order)
[[vk::binding(6, 0)]] Sampler2D flareTex;    // flare.slang's lens flare, quarter resolution, exposed light
[[vk::binding(7, 0)]] Sampler2D dirtTex;     // lens dirt lit by bloom and flare

struct ComposeOut
{
    float4 pos : SV_Position;
};

[shader("vertex")]
ComposeOut composeVS(uint vid: SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    ComposeOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float3 toSrgb(float3 c)
{
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055);
}

float3 fromSrgb(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

// Half an 8-bit step of noise, applied in sRGB space since that is where the swapchain quantizes.
float3 dither(float3 linear, float2 pixel)
{
    float noise = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))) - 0.5;
    float3 srgb = saturate(toSrgb(linear) + noise / 255.0);
    return fromSrgb(srgb);
}

float3 sceneAt(float2 uv)
{
    return sceneExpand(composeTex.SampleLevel(uv, 0).rgb);
}

/** A curve row at x (0..1), between texel centres; hue rows wrap. */
float curveAt(int row, float x, bool wrap)
{
    float2 dims;
    curvesTex.GetDimensions(dims.x, dims.y);
    x = wrap ? frac(x) : saturate(x);
    return curvesTex.SampleLevel(float2((x * (dims.x - 1.0) + 0.5) / dims.x, (float(row) + 0.5) / dims.y), 0).r;
}

float3 rgbToHsv(float3 c)
{
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    float h = 0.0;
    if (d > 1e-6)
    {
        if (mx == c.r)
            h = fmod((c.g - c.b) / d + 6.0, 6.0);
        else if (mx == c.g)
            h = (c.b - c.r) / d + 2.0;
        else
            h = (c.r - c.g) / d + 4.0;
        h /= 6.0;
    }
    return float3(h, mx > 1e-6 ? d / mx : 0.0, mx);
}

float3 hsvToRgb(float3 hsv)
{
    float3 p = abs(frac(hsv.xxx + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return hsv.z * lerp(float3(1, 1, 1), saturate(p - 1.0), hsv.y);
}

/** Wheels, tonal balance and curves on the display-encoded picture; matches render::grade::applyDisplay. */
float3 gradeDisplay(float3 s)
{
    s += cp.offset.rgb + cp.offset.w;
    s = saturate(s * cp.gain.rgb * cp.masters.z + (cp.lift.rgb + cp.masters.x) * (1.0 - s));
    s = pow(s, 1.0 / max(cp.gammaC.rgb * cp.masters.y, 0.01));
    float luma = dot(s, float3(0.2126, 0.7152, 0.0722));
    float ws = 1.0 - smoothstep(0.0, max(cp.shadows.w, 1e-3), luma), wh = smoothstep(min(cp.highlights.w, 0.999), 1.0, luma);
    float wm = saturate(1.0 - ws - wh);
    s = saturate(s * (ws * cp.shadows.rgb + wm * cp.midtones.rgb + wh * cp.highlights.rgb));
    if (cp.masters.w > 0.5)
    {
        s = float3(curveAt(1, curveAt(0, s.r, false), false), curveAt(2, curveAt(0, s.g, false), false), curveAt(3, curveAt(0, s.b, false), false));
        float3 hsv = rgbToHsv(s);
        luma = dot(s, float3(0.2126, 0.7152, 0.0722));
        hsv.y = saturate(hsv.y * 2.0 * curveAt(4, hsv.x, true) * 2.0 * curveAt(6, luma, false));
        hsv.x = frac(hsv.x + curveAt(5, hsv.x, true) - 0.5);
        s = hsvToRgb(hsv);
    }
    return s;
}

float hash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

/** Smooth value noise in -1..1 at grain scale: a lattice of hashes, blended. */
float grainNoise(float2 p)
{
    float2 i = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i), b = hash(i + float2(1, 0)), c = hash(i + float2(0, 1)), d = hash(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) * 2.0 - 1.0;
}

[shader("fragment")]
float4 composePS(ComposeOut i) : SV_Target
{
    const bool stack = cp.grade.z > 0.5;
    float2 uv = i.pos.xy * cp.size.zw;
    const float alpha = composeTex.Load(int3(int2(i.pos.xy), 0)).a;
    float3 c;
    if (!stack)
        c = sceneExpand(composeTex.Load(int3(int2(i.pos.xy), 0)).rgb);
    else
    {
        // Barrel or pincushion distortion around the centre, then a radial colour split.
        float2 centred = uv * 2.0 - 1.0;
        float aspect = cp.size.x * cp.size.w;
        float2 d = centred * float2(aspect, 1.0);
        float r2 = dot(d, d) / (aspect * aspect + 1.0);
        float2 warped = centred * (1.0 + cp.lens.y * r2);
        float2 base = warped * 0.5 + 0.5;
        if (cp.lens.x > 0.0)
        {
            float2 shift = warped * cp.lens.x * cp.size.zw;
            c = float3(sceneAt(base + shift).r, sceneAt(base).g, sceneAt(base - shift).b);
        }
        else
            c = sceneAt(base);
        if (cp.lens.z > 0.0)
        {
            // Unsharp mask on the four neighbours.
            float3 n = sceneAt(base + float2(cp.size.z, 0)) + sceneAt(base - float2(cp.size.z, 0)) + sceneAt(base + float2(0, cp.size.w)) +
                       sceneAt(base - float2(0, cp.size.w));
            c = max(c + cp.lens.z * (c - 0.25 * n), 0.0);
        }
        // Lens dirt catches the light bloom and flare spread over the lens.
        const float dirt = cp.flare.z > 0.5 ? dot(dirtTex.SampleLevel(base, 0).rgb, float3(0.3333, 0.3333, 0.3333)) * cp.flare.y : 0.0;
        if (cp.lens.w > 0.0)
        {
            float3 glow = bloomTex.SampleLevel(base, 0).rgb * cp.misc.y;
            c = lerp(c, glow, cp.lens.w) + glow * cp.lens.w * dirt;
        }
        if (cp.flare.x > 0.5)
            c += flareTex.SampleLevel(base, 0).rgb * (1.0 + dirt);
        if (cp.halation.x > 0.0)
        {
            // Halation: light scattered back through the film base, a tinted glow round what is brighter than the threshold.
            float3 glow = bloomTex.SampleLevel(base, 0).rgb * cp.misc.y;
            float over = max(dot(glow, float3(0.2126, 0.7152, 0.0722)) - cp.halation.y, 0.0);
            c += cp.halationTint.rgb * over * cp.halation.x;
        }
        uv = base;
    }

    float exposure = cp.params.z;
    if (cp.misc.x > 0.5)
        exposure *= exposureTex.Load(int3(0, 0, 0)).r;
    c *= exposure;

    if (stack)
    {
        c *= cp.white.rgb;
        // Contrast around 18% grey in log space, then saturation around the pixel's luminance.
        const float grey = 0.18;
        c = grey * pow(max(c / grey, 0.0), cp.grade.x);
        float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
        c = max(luma + cp.grade.y * (c - luma), 0.0);
    }

    if (uint(cp.params.w + 0.5) == 4)
    {
        // The view's display code values, made linear so the sRGB target writes them back out unchanged.
        float3 dims;
        viewLutTex.GetDimensions(dims.x, dims.y, dims.z);
        float3 shaped = saturate((log2(max(c, exp2(cp.misc.z))) - cp.misc.z) / (cp.misc.w - cp.misc.z));
        c = fromSrgb(saturate(viewLutTex.SampleLevel(shaped * ((dims - 1.0) / dims) + 0.5 / dims, 0).rgb));
    }
    else
    {
        c = tonemap(c, uint(cp.params.w + 0.5), uint(cp.grade.w + 0.5));
        c = pow(max(c, 0.0), 1.0 / max(cp.params.x, 0.01));
    }

    if (stack)
    {
        // Lift, gamma, gain on the display-encoded picture, as in a grading suite.
        float3 s = gradeDisplay(toSrgb(saturate(c)));
        if (cp.white.w > 0.0)
        {
            float3 dims;
            lutTex.GetDimensions(dims.x, dims.y, dims.z);
            float3 coord = s * ((dims - 1.0) / dims) + 0.5 / dims;
            s = lerp(s, lutTex.SampleLevel(coord, 0).rgb, cp.white.w);
        }
        if (cp.lift.w > 0.0)
        {
            float2 v = (uv - 0.5) * float2(cp.size.x * cp.size.w, 1.0);
            s *= 1.0 - cp.lift.w * smoothstep(0.25, 1.0, dot(v, v) * 1.6);
        }
        if (cp.gammaC.w > 0.0)
        {
            // Grain of a size, mono or per channel, strongest in the mid-tones as on film by its response.
            float2 g = i.pos.xy / max(cp.grain.x, 0.5) + cp.gain.w * float2(17.3, 31.7);
            float mono = grainNoise(g);
            float3 coloured = float3(mono, grainNoise(g + 51.3), grainNoise(g + 97.1));
            float3 n = lerp(mono.xxx, coloured, cp.grain.y);
            float luma = dot(s, float3(0.2126, 0.7152, 0.0722));
            float weight = lerp(1.0, 4.0 * luma * (1.0 - luma), cp.grain.z);
            s = saturate(s + n * cp.gammaC.w * 0.15 * weight);
        }
        if (cp.letterbox.x > 0.0)
        {
            float pic = cp.size.x * cp.size.w;
            float2 bar = cp.letterbox.x > pic ? float2(0.0, 0.5 * (1.0 - pic / cp.letterbox.x)) : float2(0.5 * (1.0 - cp.letterbox.x / pic), 0.0);
            if (any(uv < bar) || any(uv > 1.0 - bar))
                s *= 1.0 - cp.letterbox.y;
        }
        c = fromSrgb(s);
    }
    if (cp.params.y > 0.5)
        c = dither(c, i.pos.xy);
    return float4(c, alpha);
}
