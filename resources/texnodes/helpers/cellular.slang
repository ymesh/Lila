//! Cellular noise with a choice of distance metric, tileable with the period equal to the scale. Keeps the two nearest
//! feature points, so callers get F1, F2, the distance to the cell edge and per-cell values.

struct CellResult
{
    float f1;
    float f2;
    /** Feature points relative to the sample, in cells. */
    float2 d1;
    float2 d2;
    /** The nearest point's cell, wrapped into the period: a stable per-cell key. */
    int2 cell;
};

/** 0 euclidean, 1 manhattan, 2 chebyshev, 3 Minkowski with the exponent (1 is manhattan, 2 euclidean, large is chebyshev). */
float cellMetric(float2 d, int metric, float exponent)
{
    if (metric == 1)
        return abs(d.x) + abs(d.y);
    if (metric == 2)
        return max(abs(d.x), abs(d.y));
    if (metric == 3)
    {
        float e = max(exponent, 0.1);
        return pow(pow(abs(d.x), e) + pow(abs(d.y), e), 1.0 / e);
    }
    return length(d);
}

float cellMetric(float2 d, int metric) { return cellMetric(d, metric, 2.0); }

CellResult cellular(float2 uv, int2 scale, float jitter, int metric, uint seed) { return cellular(uv, scale, jitter, metric, 2.0, seed); }

CellResult cellular(float2 uv, int2 scale, float jitter, int metric, float exponent, uint seed)
{
    float2 q = uv * float2(scale);
    float2 i = floor(q);
    float2 f = q - i;
    int2 c = int2(i);
    CellResult r;
    r.f1 = 8.0;
    r.f2 = 8.0;
    r.d1 = float2(0.0, 0.0);
    r.d2 = float2(0.0, 0.0);
    r.cell = int2(0, 0);
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            int2 n = c + int2(x, y);
            int2 w = ((n % scale) + scale) % scale;
            float2 o = float2(float(x), float(y)) + 0.5 + (texRand2(uint2(w), seed) - 0.5) * jitter - f;
            float d = cellMetric(o, metric, exponent);
            if (d < r.f1)
            {
                r.f2 = r.f1;
                r.d2 = r.d1;
                r.f1 = d;
                r.d1 = o;
                r.cell = w;
            }
            else if (d < r.f2)
            {
                r.f2 = d;
                r.d2 = o;
            }
        }
    }
    return r;
}

/** Distance from the sample to the bisector of its two nearest points (the cell edge), in cells. */
float cellEdgeDistance(CellResult r)
{
    float2 m = 0.5 * (r.d1 + r.d2);
    float2 n = r.d2 - r.d1;
    float len = length(n);
    return len > 1e-6 ? max(dot(m, n / len), 0.0) : 0.0;
}

float cellRandom(CellResult r, uint seed) { return texRand(uint2(r.cell), seed ^ 0x2c1b3c6du); }

float4 cellColor(CellResult r, uint seed)
{
    return float4(texRand(uint2(r.cell), seed ^ 0x51ed270bu), texRand(uint2(r.cell), seed ^ 0x2545f491u), texRand(uint2(r.cell), seed ^ 0x9e3779b9u), 1.0);
}
