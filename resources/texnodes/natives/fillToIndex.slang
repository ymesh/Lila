// Fill to Index: Flood Fill regions numbered in row order of their keys (helpers/fill.slang), as (index + 1) / count.
// Contract in src/tex/Natives.h. gU: 0 flood fill data, 1 output, 2 key marks (w x h), 3 marks before each texel in its
// row, the row's total in column w ((w + 1) x h), 4 keys in the rows above each row, the total in row h (1 x (h + 1)).

/** The texel's region key, or -1 outside every region. */
int2 indexKey(int2 p)
{
    float4 box = round(nativeFitted(0, p));
    if (box.z < box.x || box.w < box.y)
        return int2(-1, -1);
    int2 s = nativeTexel();
    int2 k = p + int2(box.xy);
    return ((k % s) + s) % s;
}

void indexClear(uint3 id)
{
    int2 p = int2(id.xy);
    if (p.x < int(gPass.res.x) && p.y < int(gPass.res.y))
        nativeStore(p, float4(0.0, 0.0, 0.0, 0.0));
}

/** Writes 1 at each region's key; every texel of a region writes the same value there. */
void indexMark(uint3 id)
{
    int2 p = int2(id.xy);
    if (p.x >= int(gPass.res.x) || p.y >= int(gPass.res.y))
        return;
    int2 k = indexKey(p);
    if (k.x >= 0)
        nativeStore(k, float4(1.0, 1.0, 1.0, 1.0));
}

/** One thread per row (threads 1 x h). gPass.uv.x: the marks' slot. */
void indexRows(uint3 id)
{
    int y = int(id.y);
    uint src = uint(gPass.uv.x);
    int w = nativeSize(src).x;
    if (id.x != 0 || y >= int(gPass.res.y))
        return;
    float n = 0.0;
    for (int x = 0; x < w; ++x)
    {
        nativeStore(int2(x, y), float4(n, n, n, n));
        n += texFetchBuf(src, int2(x, y)).x;
    }
    nativeStore(int2(w, y), float4(n, n, n, n));
}

/** One thread. gPass.uv.x: the row counts' slot. */
void indexOffsets(uint3 id)
{
    if (id.x != 0 || id.y != 0)
        return;
    uint src = uint(gPass.uv.x);
    int2 s = nativeSize(src);
    float n = 0.0;
    for (int y = 0; y < s.y; ++y)
    {
        nativeStore(int2(0, y), float4(n, n, n, n));
        n += texFetchBuf(src, int2(s.x - 1, y)).x;
    }
    nativeStore(int2(0, s.y), float4(n, n, n, n));
}

/** gPass.uv.x: the row counts' slot, uv.y the offsets'. */
void indexOut(uint3 id)
{
    int2 p = int2(id.xy);
    int2 s = nativeTexel();
    if (p.x >= s.x || p.y >= s.y)
        return;
    int2 k = indexKey(p);
    if (k.x < 0)
    {
        nativeStore(p, float4(0.0, 0.0, 0.0, 1.0));
        return;
    }
    uint rows = uint(gPass.uv.x);
    uint offsets = uint(gPass.uv.y);
    float index = texFetchBuf(offsets, int2(0, k.y)).x + texFetchBuf(rows, k).x;
    float count = texFetchBuf(offsets, int2(0, s.y)).x;
    float v = (index + 1.0) / max(count, 1.0);
    nativeStore(p, float4(v, v, v, 1.0));
}
