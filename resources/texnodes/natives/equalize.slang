// Equalize: each value mapped through the image's cumulative histogram, so the values spread evenly over 0 to 1.
// Contract in src/tex/Natives.h. gU: 0 input, 1 output, 2 mode, 3 row histograms (256 x h), 4 histogram (256 x 1),
// 5 counts below each bin (256 x 1). Histogram texels count rgb per channel and luminance in a.

static const int kEqualizeBins = 256;

float4 equalizeInput(int2 p, int2 o)
{
    int2 s = nativeSize(0);
    float4 c = texFetchBuf(0, int2(p.x * s.x / o.x, p.y * s.y / o.y));
    return float4(c.rgb, dot(c.rgb, float3(0.2126, 0.7152, 0.0722)));
}

int equalizeBin(float v) { return clamp(int(floor(v * float(kEqualizeBins))), 0, kEqualizeBins - 1); }

/** One thread per row (threads 1 x h). */
void histogramRows(uint3 id)
{
    int2 o = nativeSize(1);
    int y = int(id.y);
    if (id.x != 0 || y >= o.y)
        return;
    float4 counts[kEqualizeBins];
    for (int b = 0; b < kEqualizeBins; ++b)
        counts[b] = float4(0.0, 0.0, 0.0, 0.0);
    for (int x = 0; x < o.x; ++x)
    {
        float4 c = equalizeInput(int2(x, y), o);
        counts[equalizeBin(c.x)].x += 1.0;
        counts[equalizeBin(c.y)].y += 1.0;
        counts[equalizeBin(c.z)].z += 1.0;
        counts[equalizeBin(c.w)].w += 1.0;
    }
    for (int b = 0; b < kEqualizeBins; ++b)
        nativeStore(int2(b, y), counts[b]);
}

/** One thread per bin (threads 256 x 1). gPass.uv.x: the row histograms' slot. */
void histogramSum(uint3 id)
{
    int b = int(id.x);
    if (id.y != 0 || b >= kEqualizeBins)
        return;
    uint src = uint(gPass.uv.x);
    int h = nativeSize(src).y;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    for (int y = 0; y < h; ++y)
        sum += texFetchBuf(src, int2(b, y));
    nativeStore(int2(b, 0), sum);
}

/** One thread. gPass.uv.x: the histogram's slot. */
void histogramBelow(uint3 id)
{
    if (id.x != 0 || id.y != 0)
        return;
    uint src = uint(gPass.uv.x);
    float4 below = float4(0.0, 0.0, 0.0, 0.0);
    for (int b = 0; b < kEqualizeBins; ++b)
    {
        nativeStore(int2(b, 0), below);
        below += texFetchBuf(src, int2(b, 0));
    }
}

/** The share of texels below v, counting its own bin's linearly. */
float equalizeMap(float v, float below, float count, float total)
{
    int b = equalizeBin(v);
    float t = saturate(v * float(kEqualizeBins) - float(b));
    return (below + t * count) / total;
}

/** gPass.uv.x: the histogram's slot, uv.y the counts below. */
void equalizeOut(uint3 id)
{
    int2 p = int2(id.xy);
    int2 s = nativeTexel();
    if (p.x >= s.x || p.y >= s.y)
        return;
    uint hist = uint(gPass.uv.x);
    uint lower = uint(gPass.uv.y);
    float total = float(s.x * s.y);
    float4 c = equalizeInput(p, s);
    float4 n = texFetchBuf(hist, int2(equalizeBin(c.x), 0));
    float4 m = texFetchBuf(lower, int2(equalizeBin(c.x), 0));
    float r = equalizeMap(c.x, m.x, n.x, total);
    n = texFetchBuf(hist, int2(equalizeBin(c.y), 0));
    m = texFetchBuf(lower, int2(equalizeBin(c.y), 0));
    float g = equalizeMap(c.y, m.y, n.y, total);
    n = texFetchBuf(hist, int2(equalizeBin(c.z), 0));
    m = texFetchBuf(lower, int2(equalizeBin(c.z), 0));
    float bl = equalizeMap(c.z, m.z, n.z, total);
    n = texFetchBuf(hist, int2(equalizeBin(c.w), 0));
    m = texFetchBuf(lower, int2(equalizeBin(c.w), 0));
    float l = equalizeMap(c.w, m.w, n.w, total);
    float3 rgb = float3(r, g, bl);
    if (int(gU[2].x) == 0)
        rgb = c.w > 1e-5 ? saturate(c.rgb * (l / c.w)) : float3(l, l, l);
    float a = texFetchBuf(0, int2(p.x * nativeSize(0).x / s.x, p.y * nativeSize(0).y / s.y)).a;
    nativeStore(p, float4(rgb, a));
}
