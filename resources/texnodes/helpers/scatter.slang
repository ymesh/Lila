//! Places copies of a pattern on a grid of cells (the tiler and splatter): each cell holds one or more instances with
//! a random offset, size, rotation and value, all keyed on the wrapped cell so the result tiles.

struct ScatterHit
{
    float value;
    /** The winning instance's pattern coordinate (0 to 1 over the instance), its random and its random colour. */
    float2 local;
    float random;
    /** Rotation as 0 to 1 over -180 to 180 degrees, size as a fraction of the cell. */
    float rotation;
    float size;
    float4 color;
    /** Which pattern input the instance drew, 0 to 3. */
    int pattern;
};

struct ScatterSettings
{
    int2 count;
    int perCell;
    float size;
    float sizeRandom;
    float rotation;
    float rotationRandom;
    float offsetRandom;
    float valueRandom;
    float maskSize;
    /** 0 max, 1 add, 2 topmost instance. */
    int blend;
    uint seed;
    /** How many pattern inputs instances pick from at random, 1 to 4. */
    int patterns;
    /** Every other row slides along by this fraction of a cell, as bricks do. */
    float rowOffset;
    /** Instances take their colour from the colour map at their centre instead of a random one. */
    bool colorFromMap;
};

ScatterSettings scatterDefaults()
{
    ScatterSettings s;
    s.count = int2(1, 1);
    s.perCell = 1;
    s.size = 1.0;
    s.sizeRandom = 0.0;
    s.rotation = 0.0;
    s.rotationRandom = 0.0;
    s.offsetRandom = 0.0;
    s.valueRandom = 0.0;
    s.maskSize = 0.0;
    s.blend = 0;
    s.seed = 0u;
    s.patterns = 1;
    s.rowOffset = 0.0;
    s.colorFromMap = false;
    return s;
}

float scatterRand(int2 cell, uint seed, uint salt) { return texRand(uint2(cell), seed ^ salt); }

/**
 * The instances under uv. Size and rotation maps scale and turn each instance (rotation in whole turns), read at its
 * centre like the mask; the colour map gives its colour when colorFromMap is on.
 */
ScatterHit scatter<P0 : ITexFloat, P1 : ITexFloat, P2 : ITexFloat, P3 : ITexFloat, M : ITexFloat, Z : ITexFloat, R : ITexFloat, C : ITexColor>(
    float2 uv, P0 pattern, P1 pattern2, P2 pattern3, P3 pattern4, M mask, Z sizeMap, R rotationMap, C colorMap, ScatterSettings s)
{
    int2 count = max(s.count, int2(1, 1));
    float2 q = uv * float2(count);
    int2 c = int2(floor(q));
    ScatterHit hit;
    hit.value = 0.0;
    hit.local = float2(0.0, 0.0);
    hit.random = 0.0;
    hit.rotation = 0.5;
    hit.size = 0.0;
    hit.color = float4(0.0, 0.0, 0.0, 1.0);
    hit.pattern = 0;
    float best = -1.0;
    int reach = s.size * (1.0 + s.maskSize) > 1.2 ? 2 : 1;
    for (int dy = -reach; dy <= reach; ++dy)
    {
        for (int dx = -reach; dx <= reach; ++dx)
        {
            int2 n = c + int2(dx, dy);
            int2 w = ((n % count) + count) % count;
            float slide = (w.y & 1) != 0 ? s.rowOffset : 0.0;
            for (int k = 0; k < s.perCell; ++k)
            {
                uint seed = s.seed + uint(k) * 0x632be5abu;
                float2 offset = float2(scatterRand(w, seed, 0x1b873593u), scatterRand(w, seed, 0xcc9e2d51u)) - 0.5;
                float2 center = float2(n) + 0.5 + float2(slide, 0.0) + offset * s.offsetRandom;
                float2 at = center / float2(count);
                float m = mask.sample(at);
                if (scatterRand(w, seed, 0x85ebca6bu) >= m)
                    continue;
                float size = s.size * (1.0 - s.sizeRandom * scatterRand(w, seed, 0xc2b2ae35u)) * lerp(1.0, saturate(m), s.maskSize) * max(sizeMap.sample(at), 0.0);
                if (size <= 1e-5)
                    continue;
                float rot = s.rotation + (scatterRand(w, seed, 0x27d4eb2fu) - 0.5) * 360.0 * s.rotationRandom + rotationMap.sample(at) * 360.0;
                float a = radians(rot);
                float2 d = q - center;
                float2 l = float2(cos(a) * d.x + sin(a) * d.y, -sin(a) * d.x + cos(a) * d.y) / size + 0.5;
                if (l.x < 0.0 || l.y < 0.0 || l.x > 1.0 || l.y > 1.0)
                    continue;
                int pick = min(int(scatterRand(w, seed, 0x4cf5ad43u) * float(clamp(s.patterns, 1, 4))), 3);
                float raw = pick == 0 ? pattern.sample(l) : pick == 1 ? pattern2.sample(l) : pick == 2 ? pattern3.sample(l) : pattern4.sample(l);
                float v = raw * (1.0 - s.valueRandom * scatterRand(w, seed, 0x165667b1u));
                if (v <= 0.0)
                    continue;
                float depth = scatterRand(w, seed, 0xd3a2646cu);
                float key = s.blend == 2 ? depth : v;
                if (s.blend == 1)
                    hit.value += v;
                if (key > best)
                {
                    best = key;
                    if (s.blend != 1)
                        hit.value = v;
                    hit.local = l;
                    hit.random = scatterRand(w, seed, 0xfd7046c5u);
                    hit.rotation = frac(rot / 360.0 + 0.5);
                    hit.size = size;
                    hit.pattern = pick;
                    hit.color = s.colorFromMap ? colorMap.sample(at)
                                               : float4(scatterRand(w, seed, 0xb55a4f09u), scatterRand(w, seed, 0x9e3779b9u), scatterRand(w, seed, 0x7feb352du), 1.0);
                }
            }
        }
    }
    return hit;
}
