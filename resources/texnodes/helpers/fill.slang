//! Reading the Flood Fill node's data. Each texel holds its region's bounding box relative to itself, in texels with
//! rows counting down: (min x, min y, max x, max y) minus its own (x, y). Outside every region max < min.
//! The Fill To nodes must read it at the resolution it was computed at, directly or through other per-texel nodes.

struct FillRegion
{
    bool valid;
    /** The region's top-left texel, wrapped into the texture: a stable per-region key. */
    int2 key;
    /** Bounding box size, in uv. */
    float2 size;
    /** Bounding box size, in texels. */
    float2 texels;
    /** Where the sample sits in the box: 0 to 1, v up. */
    float2 local;
};

FillRegion fillRegion(float4 e, float2 uv)
{
    float2 res = texResolution();
    float2 t = round(float2(uv.x * res.x - 0.5, (1.0 - uv.y) * res.y - 0.5));
    // Reads land on texel centres, but bilinear filtering can still mix a hair of the neighbours in.
    float4 box = round(e);
    FillRegion r;
    r.valid = box.z >= box.x && box.w >= box.y;
    float2 lo = t + box.xy;
    float2 hi = t + box.zw;
    int2 ires = int2(res);
    int2 k = int2(lo);
    r.key = ((k % ires) + ires) % ires;
    float2 texels = max(hi - lo + 1.0, float2(1.0, 1.0));
    r.texels = texels;
    r.size = texels / res;
    float2 l = (t - lo + 0.5) / texels;
    r.local = float2(l.x, 1.0 - l.y);
    return r;
}

/** The region's bounding box centre in uv, wrapped into the texture. Exact: texel counts and halves. */
float2 fillCentre(FillRegion r)
{
    float2 res = texResolution();
    float2 c = float2(r.key) + 0.5 * r.texels;
    c = float2(c.x >= res.x ? c.x - res.x : c.x, c.y >= res.y ? c.y - res.y : c.y);
    return float2(c.x / res.x, 1.0 - c.y / res.y);
}
