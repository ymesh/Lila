// Auto Levels: the input stretched so its lowest value maps to 0 and its highest to 1, from a min/max reduction.
// Contract in src/tex/Natives.h. gU: 0 input, 1 output, 2 mode, 3 row ranges (2 x h), 4 whole range (2 x 1).
// A range buffer holds the lowest values in texel 0 and the highest in texel 1: rgb per channel, luminance in a.

float4 levelsInput(int2 p)
{
    int2 s = nativeSize(0);
    int2 o = nativeSize(1);
    float4 c = texFetchBuf(0, int2(p.x * s.x / o.x, p.y * s.y / o.y));
    return float4(c.rgb, dot(c.rgb, float3(0.2126, 0.7152, 0.0722)));
}

/** One thread per row (threads 1 x h). */
void rangeRows(uint3 id)
{
    int2 o = nativeSize(1);
    int y = int(id.y);
    if (id.x != 0 || y >= o.y)
        return;
    float4 lo = float4(1e30, 1e30, 1e30, 1e30);
    float4 hi = -lo;
    for (int x = 0; x < o.x; ++x)
    {
        float4 c = levelsInput(int2(x, y));
        lo = min(lo, c);
        hi = max(hi, c);
    }
    nativeStore(int2(0, y), lo);
    nativeStore(int2(1, y), hi);
}

/** One thread. gPass.uv.x: the row ranges' slot. */
void rangeAll(uint3 id)
{
    if (id.x != 0 || id.y != 0)
        return;
    uint src = uint(gPass.uv.x);
    int h = nativeSize(src).y;
    float4 lo = float4(1e30, 1e30, 1e30, 1e30);
    float4 hi = -lo;
    for (int y = 0; y < h; ++y)
    {
        lo = min(lo, texFetchBuf(src, int2(0, y)));
        hi = max(hi, texFetchBuf(src, int2(1, y)));
    }
    nativeStore(int2(0, 0), lo);
    nativeStore(int2(1, 0), hi);
}

float levelsStretch(float v, float lo, float hi) { return hi - lo > 1e-6 ? (v - lo) / (hi - lo) : v; }

/** gPass.uv.x: the whole range's slot. */
void levelsOut(uint3 id)
{
    int2 p = int2(id.xy);
    int2 s = nativeTexel();
    if (p.x >= s.x || p.y >= s.y)
        return;
    uint src = uint(gPass.uv.x);
    float4 lo = texFetchBuf(src, int2(0, 0));
    float4 hi = texFetchBuf(src, int2(1, 0));
    float4 c = texFetchBuf(0, int2(p.x * nativeSize(0).x / s.x, p.y * nativeSize(0).y / s.y));
    float3 r;
    if (int(gU[2].x) == 1)
        r = saturate(float3(levelsStretch(c.r, lo.a, hi.a), levelsStretch(c.g, lo.a, hi.a), levelsStretch(c.b, lo.a, hi.a)));
    else
        r = float3(levelsStretch(c.r, lo.r, hi.r), levelsStretch(c.g, lo.g, hi.g), levelsStretch(c.b, lo.b, hi.b));
    nativeStore(p, float4(r, c.a));
}
