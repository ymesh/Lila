// The sky atmosphere (Hillaire 2020) as render::packAtmosphere lays it out: a header, then the transmittance and
// multiple scattering tables, float4 per texel. Lengths in km, positions from the planet's centre in world axes.

/** Matches render::GpuAtmosphere. */
struct GpuAtmosphere
{
    float4 radii;       // bottom, top (km), multiple scattering factor, 1
    float4 rayleigh;    // rgb scattering per km, w scale height km
    float4 mieScatter;  // rgb, w scale height km
    float4 mieAbsorb;   // rgb, w anisotropy
    float4 ozone;       // rgb absorption per km, w tip altitude km
    float4 ozoneShape;  // x tip value, y width km, z sin of the lowest sun elevation
    float4 ground;      // rgb albedo, w cos of the sun disk's angular radius
    float4 skyFactor;   // rgb sky luminance factor, w aerial perspective distance scale
    float4 camera;      // xyz the camera in km from the planet's centre, w aerial perspective start, km
    float4 sunDir;      // xyz toward the sun, w 1 when zUp
    float4 sunRadiance; // rgb the sun light above the atmosphere
    float4 pad[5];
};

static const uint kAtmTransmittanceW = 256;
static const uint kAtmTransmittanceH = 64;
static const uint kAtmMultiScatter = 32;
static const uint kAtmHeaderTexels = 16;
static const float kAtmPi = 3.14159265358979;

float3 atmTexel(GpuAtmosphere* a, uint index)
{
    return ((float4*)a)[kAtmHeaderTexels + index].rgb;
}

float3 atmBilinear(GpuAtmosphere* a, uint base, uint w, uint h, float2 uv)
{
    float x = clamp(uv.x * w - 0.5, 0.0, float(w - 1));
    float y = clamp(uv.y * h - 0.5, 0.0, float(h - 1));
    uint x0 = uint(x), y0 = uint(y);
    uint x1 = min(x0 + 1, w - 1), y1 = min(y0 + 1, h - 1);
    float fx = x - x0, fy = y - y0;
    float3 top = lerp(atmTexel(a, base + y0 * w + x0), atmTexel(a, base + y0 * w + x1), fx);
    float3 bottom = lerp(atmTexel(a, base + y1 * w + x0), atmTexel(a, base + y1 * w + x1), fx);
    return lerp(top, bottom, fy);
}

// Squares of planet radii lose kilometres in float, so sphere terms are written as (R - x)(R + x).
float atmDistanceToTop(float r, float mu, float top)
{
    float side = r * sqrt(max(1.0 - mu * mu, 0.0));
    return max(0.0, -r * mu + sqrt(max((top - side) * (top + side), 0.0)));
}

bool atmHitsGround(float r, float mu, float bottom)
{
    return mu < 0.0 && r * sqrt(max(1.0 - mu * mu, 0.0)) <= bottom;
}

/** Nearest hit at or after 0 of a ray with a sphere about the planet's centre; negative for a miss. */
float atmRaySphere(float3 o, float3 d, float R)
{
    float b = dot(o, d);
    float h = length(o - b * d);
    float disc = (R - h) * (R + h);
    if (disc < 0.0)
        return -1.0;
    float s = sqrt(disc);
    float t0 = -b - s, t1 = -b + s;
    return t0 >= 0.0 ? t0 : (t1 >= 0.0 ? t1 : -1.0);
}

/** Where a ray from inside a sphere about the planet's centre leaves it: always the far root. */
float atmRayExit(float3 o, float3 d, float R)
{
    float b = dot(o, d);
    float h = length(o - b * d);
    return max(-b + sqrt(max((R - h) * (R + h), 0.0)), 0.0);
}

float3 atmTransmittance(GpuAtmosphere* a, float r, float mu)
{
    float bottom = a->radii.x, top = a->radii.y;
    float H = sqrt(max((top - bottom) * (top + bottom), 0.0));
    float rho = sqrt(max((r - bottom) * (r + bottom), 0.0));
    float d = atmDistanceToTop(r, mu, top);
    float dMin = top - r, dMax = rho + H;
    float2 uv = float2(dMax > dMin ? saturate((d - dMin) / (dMax - dMin)) : 0.0, saturate(rho / max(H, 1e-6)));
    return atmBilinear(a, 0, kAtmTransmittanceW, kAtmTransmittanceH, uv);
}

float3 atmMultiScatter(GpuAtmosphere* a, float r, float muSun)
{
    float2 uv = float2(saturate(muSun * 0.5 + 0.5), saturate((r - a->radii.x) / max(a->radii.y - a->radii.x, 1e-6)));
    return atmBilinear(a, kAtmTransmittanceW * kAtmTransmittanceH, kAtmMultiScatter, kAtmMultiScatter, uv);
}

struct AtmMedium
{
    float3 rayleigh, mie, extinction;
};

AtmMedium atmMedium(GpuAtmosphere* a, float h)
{
    h = max(h, 0.0);
    float dr = exp(-h / max(a->rayleigh.w, 1e-9));
    float dm = exp(-h / max(a->mieScatter.w, 1e-9));
    float dOzone = max(0.0, a->ozoneShape.x * (1.0 - abs(h - a->ozone.w) / max(a->ozoneShape.y, 1e-9)));
    AtmMedium m;
    m.rayleigh = a->rayleigh.rgb * dr;
    m.mie = a->mieScatter.rgb * dm;
    m.extinction = m.rayleigh + m.mie + a->mieAbsorb.rgb * dm + a->ozone.rgb * dOzone;
    return m;
}

float3 atmSunAt(GpuAtmosphere* a, float3 p, float3 sun)
{
    float r = length(p);
    float mu = dot(p / r, sun);
    if (atmHitsGround(r, mu, a->radii.x))
        return float3(0.0, 0.0, 0.0);
    return atmTransmittance(a, r, mu);
}

/**
 * Light scattered toward pos along -dir from up to maxDist km (or to the ground or the top of the atmosphere), for a sun of
 * illuminance 1, and the share of light from beyond that gets through. ground adds the lit planet where the ray meets it.
 */
float3 atmIntegrate(GpuAtmosphere* a, float3 pos, float3 dir, float3 sun, float minDist, float maxDist, uint samples, bool ground,
                    out float3 transmittance)
{
    transmittance = float3(1.0, 1.0, 1.0);
    float bottom = a->radii.x, top = a->radii.y;
    // A shallower atmosphere than Earth's shrinks the margins with it, so a miniature planet keeps its air.
    float margin = 1e-4 * min((top - bottom) / 60.0, 1.0);
    float r = length(pos);
    if (r < bottom + margin)
    {
        pos *= (bottom + margin) / max(r, 1e-9);
        r = bottom + margin;
    }
    float entered = 0.0;
    if (r > top)
    {
        float tTop = atmRaySphere(pos, dir, top);
        if (tTop < 0.0)
            return float3(0.0, 0.0, 0.0);
        pos += dir * tTop;
        entered = tTop;
    }
    float tGround = atmRaySphere(pos, dir, bottom);
    float tTop = atmRayExit(pos, dir, top + margin);
    float tEnd = tGround >= 0.0 ? tGround : tTop;
    float tMax = min(tEnd, max(maxDist - entered, 0.0));
    float tMin = clamp(minDist - entered, 0.0, tMax);
    if (tMax <= tMin)
        return float3(0.0, 0.0, 0.0);
    float nu = dot(dir, sun);
    float phaseR = 3.0 / (16.0 * kAtmPi) * (1.0 + nu * nu);
    float g = a->mieAbsorb.w;
    float hgD = max(1.0 + g * g - 2.0 * g * nu, 1e-6);
    float phaseM = (1.0 - g * g) / (4.0 * kAtmPi * hgD * sqrt(hgD));
    float3 L = float3(0.0, 0.0, 0.0);
    float tPrev = 0.0;
    for (uint i = 0; i < samples; ++i)
    {
        // Squared spacing: more samples near the eye, where the air along most rays is densest.
        float f1 = float(i + 1) / samples;
        float t1 = (tMax - tMin) * f1 * f1;
        float dt = t1 - tPrev;
        float3 p = pos + dir * (tMin + tPrev + 0.5 * dt);
        tPrev = t1;
        float rp = length(p);
        AtmMedium m = atmMedium(a, rp - bottom);
        float3 sunLight = atmSunAt(a, p, sun);
        float3 ms = atmMultiScatter(a, rp, dot(p / rp, sun)) * a->radii.z;
        float3 S = sunLight * (m.rayleigh * phaseR + m.mie * phaseM) + (m.rayleigh + m.mie) * ms;
        float3 segT = exp(-m.extinction * dt);
        L += transmittance * S * (1.0 - segT) / max(m.extinction, 1e-12);
        transmittance *= segT;
    }
    if (ground && tGround >= 0.0 && tGround <= maxDist - entered)
    {
        float3 p = pos + dir * tGround;
        float3 n = p / length(p);
        L += transmittance * atmSunAt(a, p, sun) * max(dot(n, sun), 0.0) * a->ground.rgb / kAtmPi;
        transmittance = float3(0.0, 0.0, 0.0);
    }
    return L * a->skyFactor.rgb;
}

/** The sky seen from the camera along dir, with the sun's disk, in shader units. */
float3 atmSkyColor(GpuAtmosphere* a, float3 dir)
{
    float3 T;
    float3 sun = a->sunDir.xyz;
    float3 c = atmIntegrate(a, a->camera.xyz, dir, sun, 0.0, 1e9, 32, true, T) * a->sunRadiance.rgb;
    float cosDisk = a->ground.w;
    float mu = dot(dir, sun);
    if (mu > cosDisk)
    {
        // The disk's radiance spreads the sun's illuminance over its solid angle; limb darkening softens its edge.
        float solid = 2.0 * kAtmPi * (1.0 - cosDisk);
        float edge = saturate((mu - cosDisk) / max(1.0 - cosDisk, 1e-7));
        float limb = 0.4 + 0.6 * sqrt(edge);
        c += T * a->sunRadiance.rgb / max(solid, 1e-9) * limb;
    }
    return c;
}
