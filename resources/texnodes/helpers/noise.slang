//! Tileable lattice noises. A period in cells equal to the frequency makes the noise repeat over uv 0 to 1.

float2 noiseGradient(int2 cell, int2 period, uint seed)
{
    int2 c = ((cell % period) + period) % period;
    uint h = texHash(uint2(c), seed) & 7u;
    const float d = 0.70710678;
    if (h == 0u)
        return float2(1.0, 0.0);
    if (h == 1u)
        return float2(-1.0, 0.0);
    if (h == 2u)
        return float2(0.0, 1.0);
    if (h == 3u)
        return float2(0.0, -1.0);
    if (h == 4u)
        return float2(d, d);
    if (h == 5u)
        return float2(-d, d);
    if (h == 6u)
        return float2(d, -d);
    return float2(-d, -d);
}

float noiseLatticeValue(int2 cell, int2 period, uint seed)
{
    int2 c = ((cell % period) + period) % period;
    return texRand(uint2(c), seed);
}

float2 noiseFade(float2 f) { return f * f * f * (f * (f * 6.0 - 15.0) + 10.0); }

/** Gradient noise in 0 to 1. */
float perlinNoise(float2 q, int2 period, uint seed)
{
    float2 i = floor(q);
    float2 f = q - i;
    int2 c = int2(i);
    float2 u = noiseFade(f);
    float n00 = dot(noiseGradient(c, period, seed), f);
    float n10 = dot(noiseGradient(c + int2(1, 0), period, seed), f - float2(1.0, 0.0));
    float n01 = dot(noiseGradient(c + int2(0, 1), period, seed), f - float2(0.0, 1.0));
    float n11 = dot(noiseGradient(c + int2(1, 1), period, seed), f - float2(1.0, 1.0));
    return lerp(lerp(n00, n10, u.x), lerp(n01, n11, u.x), u.y) * 0.70710678 + 0.5;
}

/** Value noise in 0 to 1. */
float valueNoise(float2 q, int2 period, uint seed)
{
    float2 i = floor(q);
    float2 f = q - i;
    int2 c = int2(i);
    float2 u = noiseFade(f);
    float a = noiseLatticeValue(c, period, seed);
    float b = noiseLatticeValue(c + int2(1, 0), period, seed);
    float e = noiseLatticeValue(c + int2(0, 1), period, seed);
    float g = noiseLatticeValue(c + int2(1, 1), period, seed);
    return lerp(lerp(a, b, u.x), lerp(e, g, u.x), u.y);
}

/** Octaves double in frequency (so every octave tiles) and scale in amplitude by persistence; the sum is renormalised to 0 to 1. */
float noiseFbm(float2 uv, int scale, int octaves, float persistence, uint seed, bool gradient)
{
    float sum = 0.0;
    float weight = 0.0;
    float amp = 1.0;
    int freq = max(scale, 1);
    for (int o = 0; o < octaves; ++o)
    {
        int2 period = int2(freq, freq);
        float2 q = uv * float(freq);
        uint s = seed + uint(o) * 0x9e3779b9u;
        sum += amp * (gradient ? perlinNoise(q, period, s) : valueNoise(q, period, s));
        weight += amp;
        amp *= persistence;
        freq *= 2;
    }
    return weight > 0.0 ? sum / weight : 0.5;
}

/**
 * noiseFbm with a whole-number lacunarity (each octave's frequency over the last, kept whole so every octave tiles) and
 * domain distortion: the lookup is pushed by a second noise, Distortion cells of the base scale at most.
 */
float noiseFbmEx(float2 uv, int scale, int octaves, float persistence, int lacunarity, float distortion, uint seed, bool gradient)
{
    int base = max(scale, 1);
    if (distortion > 0.0)
    {
        int2 period = int2(base, base);
        float2 w = float2(perlinNoise(uv * float(base), period, seed ^ 0x3c6ef372u), perlinNoise(uv * float(base), period, seed ^ 0xa54ff53au)) - 0.5;
        uv += w * 2.0 * distortion / float(base);
    }
    float sum = 0.0;
    float weight = 0.0;
    float amp = 1.0;
    int freq = base;
    for (int o = 0; o < octaves; ++o)
    {
        int2 period = int2(freq, freq);
        float2 q = uv * float(freq);
        uint s = seed + uint(o) * 0x9e3779b9u;
        sum += amp * (gradient ? perlinNoise(q, period, s) : valueNoise(q, period, s));
        weight += amp;
        amp *= persistence;
        freq *= max(lacunarity, 1);
    }
    return weight > 0.0 ? sum / weight : 0.5;
}
