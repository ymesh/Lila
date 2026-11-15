//! Layer stack compositing and mask generator kernels.
//! requires: blend, normals, color

float layerBurn(float b, float f)
{
    return b >= 1.0 ? 1.0 : (f <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / f));
}

float layerDodge(float b, float f)
{
    return b <= 0.0 ? 0.0 : (f >= 1.0 ? 1.0 : min(1.0, b / (1.0 - f)));
}

float layerVivid(float b, float f)
{
    return f < 0.5 ? layerBurn(b, 2.0 * f) : layerDodge(b, 2.0 * f - 1.0);
}

/**
 * blendMode's modes plus 10: signed add around 0.5 (heights, where 0.5 is neutral), 11 colour burn, 12 linear burn,
 * 13 colour dodge, 14 hard light, 15 vivid light, 16 linear light, 17 pin light, 18 exclusion, 19 divide, 20 saturation,
 * 21 colour (hue and saturation), 22 value.
 */
float3 layerBlendMode(int mode, float3 b, float3 f)
{
    switch (mode)
    {
    case 10: return b + f - 0.5;
    case 11: return float3(layerBurn(b.x, f.x), layerBurn(b.y, f.y), layerBurn(b.z, f.z));
    case 12: return b + f - 1.0;
    case 13: return float3(layerDodge(b.x, f.x), layerDodge(b.y, f.y), layerDodge(b.z, f.z));
    case 14: return blendOverlay(f, b);
    case 15: return float3(layerVivid(b.x, f.x), layerVivid(b.y, f.y), layerVivid(b.z, f.z));
    case 16: return b + 2.0 * f - 1.0;
    case 17: return lerp(min(b, 2.0 * f), max(b, 2.0 * f - 1.0), step(0.5, f));
    case 18: return b + f - 2.0 * b * f;
    case 19: return b / max(f, 1e-4);
    case 20:
    {
        float3 hb = rgbToHsv(b);
        return hsvToRgb(float3(hb.x, rgbToHsv(f).y, hb.z));
    }
    case 21:
    {
        float3 hf = rgbToHsv(f);
        return hsvToRgb(float3(hf.x, hf.y, rgbToHsv(b).z));
    }
    case 22:
    {
        float3 hb = rgbToHsv(b);
        return hsvToRgb(float3(hb.x, hb.y, rgbToHsv(f).z));
    }
    default: return blendMode(mode, b, f);
    }
}

float3 layerDirection(float azimuth, float elevation, int upAxis)
{
    float a = radians(azimuth);
    float e = radians(elevation);
    float2 h = float2(cos(a), sin(a)) * cos(e);
    return upAxis == 1 ? float3(h.x, h.y, sin(e)) : float3(h.x, sin(e), h.y);
}

float layerColorDistance(float4 a, float4 b)
{
    float3 d = abs(a.rgb - b.rgb);
    return max(d.x, max(d.y, d.z));
}
