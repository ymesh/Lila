// Texture-space painting: one thread per texel of a 128x128 page, each running the stroke's dabs that reach its page in
// order. A texel's surface point comes from its geometry texel (triangle and barycentrics), the brush weight from the 3D
// distance to the dab, the tip and the falloff, so texels on both sides of a UV seam take the same paint. The stroke's
// build-up (coverage, and per mode an accumulated value) lives in a float page per stroke; every channel is then the
// pre-stroke texel changed by that build-up, so the result never depends on how the dabs were batched.
//
// Modes: paint (values, a stroke direction, or values sampled from images at the texel's uv), filter (smudge, blur,
// sharpen, clone and heal, reading the pre-stroke tile through page tables), fill (whole triangles), project (a
// screen-space image through the stroke's view) and normal (RNM bumps or normal map stamps).

static const uint kPage = 128;
static const uint kTexels = kPage * kPage;
static const uint kMaxChannels = 24;
static const uint kNoTriangle = 0xFFFFFFFF;

static const uint kBlendNormal = 0;
static const uint kBlendErase = 1;
static const uint kBlendAdd = 2;
static const uint kBlendSubtract = 3;
static const uint kBlendMultiply = 4;

static const uint kFlagOcclusion = 1;
static const uint kFlagDirection = 2;
static const uint kFlagView = 4;
static const uint kFlagValueImages = 8;
static const uint kFlagFlow = 16;

static const uint kModePaint = 0;
static const uint kModeFilter = 1;
static const uint kModeFill = 2;
static const uint kModeProject = 3;
static const uint kModeNormal = 4;

static const uint kFilterSmudge = 0;
static const uint kFilterBlur = 1;
static const uint kFilterSharpen = 2;
static const uint kFilterClone = 3;
static const uint kFilterHeal = 4;

static const uint kNormalBump = 0;
static const uint kNormalStamp = 1;

struct GeoTexel
{
    uint triangle;
    float u;
    float v;
    uint flags;
};

// p[k].w and n[k].w hold corner k's uv.
struct Tri
{
    float4 p[3];
    float4 n[3];
    float4 t[3];
};

struct Dab
{
    float4 centerRadius;
    // xyz normal, w amount (opacity times flow)
    float4 normalAmount;
    // xyz tip long axis, w roundness
    float4 tangentRoundness;
    // xyz direction of travel, w 1 when known
    float4 direction;
};

struct Item
{
    GeoTexel* geo;
    float4* acc;
    uint dabFirst;
    uint dabCount;
    uint page;
    uint tile;
    uint2* dst[kMaxChannels];
    uint2* src[kMaxChannels];
};

struct PagePtr
{
    uint2* p;
};

// One UDIM tile: its size, and per channel the pages as they were before the stroke (null where none).
struct TileInfo
{
    uint4 size; // width, height, pagesX, pagesY
    PagePtr* tables[kMaxChannels];
};

struct Params
{
    float4x4 objectToClip;
    float4x4 clipToObject;
    float4 eye;
    float4 viewDir;
    float4 values[kMaxChannels];
    Item* items;
    uint* dabIndex;
    Dab* dabs;
    Tri* tris;
    float* falloff;
    float* alpha;
    uint* depth;
    TileInfo* tiles;
    uint* triMask;
    float4* image;
    float4* stamp;
    float4* valueImages[kMaxChannels];
    uint depthW;
    uint depthH;
    uint alphaW;
    uint alphaH;
    uint channels;
    uint blend;
    uint flags;
    uint directionChannel;
    uint maskChannel;
    uint itemCount;
    uint mode;
    uint sub;
    uint normalMask;
    uint imageW;
    uint imageH;
    uint stampW;
    uint stampH;
    uint valueW;
    uint valueH;
    uint colorChannel;
    float strength;
    float tilt;
    float depthBias;
    float filterRadius;
    float amount;
    float sourceU;
    float sourceV;
    float facingFade;
    float intensity;
    float valueScale;
    uint stencilW;
    uint stencilH;
    float stencilX;
    float stencilY;
    float stencilScale;
    float stencilAngle;
    uint stencilFlags;
    uint viewW;
    uint viewH;
    uint stencilColor;
    float4* stencil;
    float4x4 stencilToClip;
    uint* locked;
    float4* limit;
    uint limitW;
    uint limitH;
    uint gradType;
    uint gradSpace;
    uint gradDither;
    uint gradOn;
    float2 gradA;
    float2 gradB;
    float4* ramp;
    float4x4 gradToClip;
    uint hexOn;
    uint hexWeight;
    float hexRotate;
    float hexContrast;
    float hexJitter;
    uint hexColor;
    uint valueSpace;
    float valueSize;
};

static const uint kRamp = 256;

/** Where a texel sits along the gradient, 0 to 1: linear, radial, reflected or angle, in uv or on the stroke's screen. */
float gradientT(Params* p, float3 pos, float2 uv)
{
    float2 q = uv;
    if (p.gradSpace == 1)
    {
        float4 clip = mul(p.gradToClip, float4(pos, 1.0));
        q = clip.w > 1e-9 ? (clip.xy / clip.w * 0.5 + 0.5) * float2(float(p.viewW), float(p.viewH)) : float2(-1e9, -1e9);
    }
    float2 d = p.gradB - p.gradA;
    float len2 = max(dot(d, d), 1e-12);
    float2 r = q - p.gradA;
    if (p.gradType == 1)
        return saturate(length(r) / sqrt(len2));
    if (p.gradType == 2)
        return saturate(abs(dot(r, d)) / len2);
    if (p.gradType == 3)
    {
        float a = atan2(d.x * r.y - d.y * r.x, dot(d, r));
        return frac(a / 6.2831853 + 1.0);
    }
    return saturate(dot(r, d) / len2);
}

float4 rampAt(Params* p, uint c, float t)
{
    float x = saturate(t) * float(kRamp - 1);
    uint i = min(uint(x), kRamp - 2);
    return lerp(p.ramp[c * kRamp + i], p.ramp[c * kRamp + i + 1], x - float(i));
}

static const uint kStencilOn = 1;
static const uint kStencilInvert = 2;
static const uint kStencilTile = 4;
static const uint kStencilColor = 8;

float4 stencilTexel(Params* p, int x, int y)
{
    int w = int(p.stencilW), h = int(p.stencilH);
    if ((p.stencilFlags & kStencilTile) != 0)
    {
        x = ((x % w) + w) % w;
        y = ((y % h) + h) % h;
    }
    else if (x < 0 || y < 0 || x >= w || y >= h)
        return float4(0.0);
    float4 t = p.stencil[y * w + x];
    return float4(t.rgb * t.a, t.a);
}

/**
 * The stencil at a surface point: rgb its straight colour, a the amount it lets through. Pinned to the view: at scale 1
 * the picture spans the view's height, centred, then moved, scaled and turned by its placement (tex::paint::Stencil).
 */
float4 stencilAt(Params* p, float3 pos)
{
    bool invert = (p.stencilFlags & kStencilInvert) != 0;
    float4 clip = mul(p.stencilToClip, float4(pos, 1.0));
    float4 none = float4(0.0, 0.0, 0.0, invert ? 1.0 : 0.0);
    if (clip.w <= 1e-9)
        return none;
    float2 view = float2(float(p.viewW), float(p.viewH));
    float2 px = (clip.xy / clip.w * 0.5 + 0.5) * view;
    float2 center = view * 0.5;
    float2 d = px - center - float2(p.stencilX, p.stencilY);
    float c = cos(p.stencilAngle), s = sin(p.stencilAngle);
    float2 q = float2(d.x * c + d.y * s, -d.x * s + d.y * c) / max(p.stencilScale, 1e-6);
    float2 size = float2(float(p.stencilW), float(p.stencilH));
    float2 src = q * (size.y / max(view.y, 1.0)) + size * 0.5;
    if ((p.stencilFlags & kStencilTile) == 0 && (src.x < 0.0 || src.y < 0.0 || src.x > size.x || src.y > size.y))
        return none;
    float2 f = src - 0.5;
    int x0 = int(floor(f.x)), y0 = int(floor(f.y));
    float fx = f.x - float(x0), fy = f.y - float(y0);
    float4 m = lerp(lerp(stencilTexel(p, x0, y0), stencilTexel(p, x0 + 1, y0), fx), lerp(stencilTexel(p, x0, y0 + 1), stencilTexel(p, x0 + 1, y0 + 1), fx), fy);
    float3 rgb = m.a > 0.0 ? m.rgb / m.a : float3(0.0);
    float amount = (p.stencilFlags & kStencilColor) != 0 ? m.a : dot(rgb, float3(0.2126, 0.7152, 0.0722)) * m.a;
    amount = saturate(amount);
    return float4(rgb, invert ? 1.0 - amount : amount);
}

struct Push
{
    Params* params;
    uint firstItem;
    uint pad;
};

[[vk::push_constant]] Push push;

float falloffAt(Params* p, float t)
{
    if (t >= 1.0)
        return 0.0;
    float x = max(t, 0.0) * 256.0;
    uint i = min(uint(x), 255u);
    float f = x - float(i);
    return lerp(p.falloff[i], p.falloff[i + 1], f);
}

float alphaAt(Params* p, float2 uv)
{
    if (p.alphaW == 0)
        return 1.0;
    if (any(uv < 0.0) || any(uv > 1.0))
        return 0.0;
    // As brush::AlphaImage::sample: bilinear over texel centres, clamped.
    float2 q = uv * float2(float(p.alphaW), float(p.alphaH)) - 0.5;
    int2 i = int2(floor(q));
    float2 f = q - float2(i);
    int w = int(p.alphaW), h = int(p.alphaH);
    int x0 = clamp(i.x, 0, w - 1), x1 = clamp(i.x + 1, 0, w - 1);
    int y0 = clamp(i.y, 0, h - 1), y1 = clamp(i.y + 1, 0, h - 1);
    float a = p.alpha[y0 * w + x0], b = p.alpha[y0 * w + x1];
    float c = p.alpha[y1 * w + x0], d = p.alpha[y1 * w + x1];
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

/** Bilinear over texel centres of a float4 image, clamped at the edges (wrap repeats it instead). */
float4 imageAt(float4* img, uint w, uint h, float2 uv, bool wrap)
{
    float2 size = float2(float(w), float(h));
    float2 q = uv * size - 0.5;
    int2 i = int2(floor(q));
    float2 f = q - float2(i);
    int iw = int(w), ih = int(h);
    int x0, x1, y0, y1;
    if (wrap)
    {
        x0 = ((i.x % iw) + iw) % iw;
        x1 = (x0 + 1) % iw;
        y0 = ((i.y % ih) + ih) % ih;
        y1 = (y0 + 1) % ih;
    }
    else
    {
        x0 = clamp(i.x, 0, iw - 1);
        x1 = clamp(i.x + 1, 0, iw - 1);
        y0 = clamp(i.y, 0, ih - 1);
        y1 = clamp(i.y + 1, 0, ih - 1);
    }
    float4 a = img[y0 * iw + x0], b = img[y0 * iw + x1];
    float4 c = img[y1 * iw + x0], d = img[y1 * iw + x1];
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

/** The tip's local coordinates of rel (along the long axis, across it scaled by roundness, along the normal), radius-relative. */
float3 tipLocal(Dab d, float3 rel)
{
    float r = max(d.centerRadius.w, 1e-12);
    float3 u = d.tangentRoundness.xyz;
    float3 n = d.normalAmount.xyz;
    // A negative roundness marks a mirrored symmetry copy: the tip's sides swap.
    float3 v = cross(n, u) * (d.tangentRoundness.w < 0.0 ? -1.0 : 1.0);
    return float3(dot(rel, u), dot(rel, v) / max(abs(d.tangentRoundness.w), 1e-3), dot(rel, n)) / r;
}

float tipWeight(Params* p, Dab d, float3 rel)
{
    float r = max(d.centerRadius.w, 1e-12);
    float3 u = d.tangentRoundness.xyz;
    if (dot(u, u) < 0.5)
        return falloffAt(p, length(rel) / r);
    float3 l = tipLocal(d, rel);
    float w = falloffAt(p, length(l));
    if (w > 0.0)
        w *= alphaAt(p, float2(l.x, -l.y) * 0.5 + 0.5);
    return w;
}

/** Whether the stroke's view sees the surface point (no depth there counts as seen). */
bool visible(Params* p, float3 pos)
{
    float4 clip = mul(p.objectToClip, float4(pos, 1.0));
    if (clip.w <= 1e-9)
        return false;
    float3 ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0))
        return true;
    uint px = min(uint((ndc.x * 0.5 + 0.5) * float(p.depthW)), p.depthW - 1);
    uint py = min(uint((ndc.y * 0.5 + 0.5) * float(p.depthH)), p.depthH - 1);
    float z = asfloat(p.depth[py * p.depthW + px]);
    if (z <= 0.0)
        return true;
    float2 c = (float2(float(px), float(py)) + 0.5) / float2(float(p.depthW), float(p.depthH)) * 2.0 - 1.0;
    float4 q = mul(p.clipToObject, float4(c, z, 1.0));
    float3 front = q.xyz / q.w;
    float dPos = dot(pos - p.eye.xyz, p.viewDir.xyz);
    float dFront = dot(front - p.eye.xyz, p.viewDir.xyz);
    return dPos <= dFront + p.depthBias + abs(dFront) * 0.01;
}

float4 loadHalf4(uint2 h)
{
    return float4(f16tof32(h.x), f16tof32(h.x >> 16), f16tof32(h.y), f16tof32(h.y >> 16));
}

uint2 storeHalf4(float4 v)
{
    return uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16));
}

/** Channel texels hold the value in rgb and its coverage in a; the mask holds its value in all four. */
float4 blendTexel(Params* p, float4 o, float3 value, float t, bool mask)
{
    if (mask)
    {
        float m = o.a;
        switch (p.blend)
        {
        case kBlendErase: m = m * (1.0 - t); break;
        case kBlendAdd: m = m + value.x * t; break;
        case kBlendSubtract: m = m - value.x * t; break;
        case kBlendMultiply: m = m * lerp(1.0, value.x, t); break;
        default: m = lerp(m, value.x, t); break;
        }
        m = saturate(m);
        return float4(m, m, m, m);
    }
    float3 v = o.rgb;
    float a = o.a;
    switch (p.blend)
    {
    case kBlendErase: return float4(v, a * (1.0 - t));
    case kBlendAdd: return float4(v + value * t, max(a, t));
    case kBlendSubtract: return float4(v - value * t, max(a, t));
    case kBlendMultiply: return float4(v * lerp(float3(1.0), value, t), a);
    default:
    {
        float na = t + a * (1.0 - t);
        float3 nv = na > 0.0 ? (value * t + v * a * (1.0 - t)) / na : value;
        return float4(nv, na);
    }
    }
}

float3 decodeNormal(float3 e)
{
    float3 n = e * 2.0 - 1.0;
    return dot(n, n) > 1e-12 ? normalize(n) : float3(0, 0, 1);
}

/**
 * Varied tiling of a source image: hex tiling after Mikkelsen, "Practical Real-Time Hex-Tiling" (JCGT 2022). The plane
 * is cut into hexagons, each takes the image at a random offset and turn, and the three around a point blend with
 * weights sharpened toward the one whose steering value (height, or brightness) is highest, so seams follow the content.
 */
struct HexTiling
{
    float2 uv[3];
    float angle[3];
    float gain[3];
    float3 w;
};

float2 hexHash(int2 v)
{
    float2 r = float2(dot(float2(v), float2(127.1, 311.7)), dot(float2(v), float2(269.5, 183.3)));
    return frac(sin(r) * 43758.5453);
}

/** The three hexagon centres around st (one tile per uv unit) and st's barycentric weights between them. */
void hexGrid(float2 st, out float3 w, out int2 v1, out int2 v2, out int2 v3)
{
    st *= 2.0 * sqrt(3.0);
    float2 skewed = float2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
    int2 base = int2(floor(skewed));
    float3 t = float3(frac(skewed), 0.0);
    t.z = 1.0 - t.x - t.y;
    float s = step(0.0, -t.z);
    float s2 = 2.0 * s - 1.0;
    w = float3(-t.z * s2, s - t.y * s2, s - t.x * s2);
    v1 = base + int2(int(s), int(s));
    v2 = base + int2(int(s), int(1.0 - s));
    v3 = base + int2(int(1.0 - s), int(s));
}

float2 hexCentre(int2 v)
{
    return float2(float(v.x) + 0.5 * float(v.y), float(v.y) / 1.15470054) / (2.0 * sqrt(3.0));
}

/** Where each of the three cells samples st (uv times the source's scale), and their turns and brightness gains. */
HexTiling hexTiling(Params* p, float2 st)
{
    HexTiling h;
    int2 v[3];
    hexGrid(st, h.w, v[0], v[1], v[2]);
    for (int i = 0; i < 3; ++i)
    {
        float2 r = hexHash(v[i]);
        float2 r2 = hexHash(v[i] + int2(57, 113));
        h.angle[i] = (r2.x * 2.0 - 1.0) * 3.14159265 * p.hexRotate;
        float cs = cos(h.angle[i]), sn = sin(h.angle[i]);
        float2 c = hexCentre(v[i]);
        float2 d = st - c;
        h.uv[i] = c + float2(cs * d.x - sn * d.y, sn * d.x + cs * d.y) + r;
        h.gain[i] = 1.0 + (r2.y * 2.0 - 1.0) * p.hexJitter * 0.5;
    }
    return h;
}

float4 hexSample(Params* p, float4* img, float2 uv)
{
    return imageAt(img, p.valueW, p.valueH, float2(uv.x, 1.0 - uv.y), true);
}

/** The blend weights: the cells' barycentrics raised steeply, scaled by the steering value, then the contrast gain. */
float3 hexWeights(Params* p, HexTiling h)
{
    float3 steer = float3(1.0);
    if (p.hexWeight != 0xFFFFFFFFu && p.valueImages[p.hexWeight] != nullptr)
    {
        float4* img = p.valueImages[p.hexWeight];
        float3 lum = float3(0.299, 0.587, 0.114);
        for (int i = 0; i < 3; ++i)
            steer[i] = dot(hexSample(p, img, h.uv[i]).rgb, lum);
        steer = lerp(float3(1.0), steer, 0.6);
    }
    float3 w = steer * pow(max(h.w, float3(0.0)), float3(7.0));
    w /= max(w.x + w.y + w.z, 1e-6);
    if (abs(p.hexContrast - 0.5) > 1e-3)
    {
        float k = log(1.0 - p.hexContrast) / log(0.5);
        float3 sel = 2.0 * step(float3(0.5), w);
        float3 m = 2.0 * (1.0 - sel);
        w = 0.5 * sel + 0.25 * m * pow(max(float3(0.0), sel + w * m), float3(k));
        w /= max(w.x + w.y + w.z, 1e-6);
    }
    return w;
}

/** A channel's value through the hex tiling; normals turn with their cells, colour takes each cell's brightness gain. */
float3 hexValue(Params* p, HexTiling h, float3 w, uint c, bool isNormal)
{
    float3 sum = float3(0.0);
    for (int i = 0; i < 3; ++i)
    {
        float3 s = hexSample(p, p.valueImages[c], h.uv[i]).xyz;
        if (isNormal)
        {
            float3 n = decodeNormal(s);
            float cs = cos(h.angle[i]), sn = sin(h.angle[i]);
            s = float3(cs * n.x + sn * n.y, -sn * n.x + cs * n.y, n.z);
        }
        else if (c == p.hexColor)
            s *= h.gain[i];
        sum += s * w[i];
    }
    return isNormal ? encodeNormal(normalize(sum)) : sum;
}

float3 encodeNormal(float3 n)
{
    return normalize(n) * 0.5 + 0.5;
}

/** Reoriented normal mapping: detail laid over base, both tangent space. */
float3 rnm(float3 base, float3 detail)
{
    float3 t = base + float3(0, 0, 1);
    float3 u = detail * float3(-1, -1, 1);
    return normalize(t * dot(t, u) / max(t.z, 1e-6) - u);
}

// ------------------------------------------------------------------------------------------- tile reads (filters)

/** A texel of the pre-stroke tile, zero where no page is. Clamped to the tile. */
float4 tileTexel(TileInfo* ti, uint c, int2 xy)
{
    int2 s = clamp(xy, int2(0), int2(int(ti.size.x) - 1, int(ti.size.y) - 1));
    uint page = uint(s.y / int(kPage)) * ti.size.z + uint(s.x / int(kPage));
    uint2* pg = ti.tables[c][page].p;
    if (pg == nullptr)
        return float4(0.0);
    return loadHalf4(pg[uint(s.y % int(kPage)) * kPage + uint(s.x % int(kPage))]);
}

/** Bilinear over premultiplied colour, returned straight, at a texel position (centres at +0.5). The mask is a plain value. */
float4 tileSample(TileInfo* ti, uint c, float2 at, bool mask)
{
    float2 q = at - 0.5;
    int2 i = int2(floor(q));
    float2 f = q - float2(i);
    float4 a = tileTexel(ti, c, i), b = tileTexel(ti, c, i + int2(1, 0));
    float4 d = tileTexel(ti, c, i + int2(0, 1)), e = tileTexel(ti, c, i + int2(1, 1));
    if (mask)
        return lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y);
    a.rgb *= a.a;
    b.rgb *= b.a;
    d.rgb *= d.a;
    e.rgb *= e.a;
    float4 m = lerp(lerp(a, b, f.x), lerp(d, e, f.x), f.y);
    return m.a > 0.0 ? float4(m.rgb / m.a, m.a) : float4(0.0);
}

/** A gaussian of the pre-stroke tile around a texel over radius texels, on a grid of at most 9 x 9 taps. */
float4 tileBlur(TileInfo* ti, uint c, int2 xy, float radius, bool mask)
{
    int k = clamp(int(ceil(radius)), 1, 64);
    int step = max(1, k / 4);
    float sigma = max(0.5, float(k) * 0.5);
    float4 sum = float4(0.0);
    float weights = 0.0;
    for (int dy = -k; dy <= k; dy += step)
        for (int dx = -k; dx <= k; dx += step)
        {
            float g = exp(-float(dx * dx + dy * dy) / (2.0 * sigma * sigma));
            float4 s = tileTexel(ti, c, xy + int2(dx, dy));
            if (!mask)
                s.rgb *= s.a;
            sum += s * g;
            weights += g;
        }
    float4 b = sum / weights;
    if (mask)
        return b;
    return b.a > 0.0 ? float4(b.rgb / b.a, b.a) : float4(0.0);
}

/** Object-space vector d (in the triangle's plane) as a texel-space vector of a w x h tile. */
float2 toTexels(Tri tri, float3 d, float w, float h)
{
    float3 e1 = tri.p[1].xyz - tri.p[0].xyz, e2 = tri.p[2].xyz - tri.p[0].xyz;
    float2 t0 = float2(tri.p[0].w, tri.n[0].w), t1 = float2(tri.p[1].w, tri.n[1].w), t2 = float2(tri.p[2].w, tri.n[2].w);
    float a11 = dot(e1, e1), a12 = dot(e1, e2), a22 = dot(e2, e2);
    float det = a11 * a22 - a12 * a12;
    if (abs(det) < 1e-20)
        return float2(0.0);
    float b1 = dot(d, e1), b2 = dot(d, e2);
    float s = (b1 * a22 - b2 * a12) / det, t = (a11 * b2 - a12 * b1) / det;
    float2 duv = (t1 - t0) * s + (t2 - t0) * t;
    return float2(duv.x * w, -duv.y * h);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void paintDabCS(uint3 gid: SV_GroupID, uint3 local: SV_GroupThreadID)
{
    Params* p = push.params;
    uint itemIndex = push.firstItem + gid.y;
    if (itemIndex >= p.itemCount)
        return;
    uint texel = gid.x * 64 + local.x;
    if (texel >= kTexels)
        return;
    Item item = p.items[itemIndex];
    GeoTexel g = item.geo[texel];
    if (g.triangle == kNoTriangle)
        return;
    if (p.locked != nullptr && (p.locked[g.triangle >> 5] & (1u << (g.triangle & 31u))) != 0)
        return;
    Tri tri = p.tris[g.triangle];
    float w0 = 1.0 - g.u - g.v;
    float3 pos = tri.p[0].xyz * w0 + tri.p[1].xyz * g.u + tri.p[2].xyz * g.v;
    float3 nrm = normalize(tri.n[0].xyz * w0 + tri.n[1].xyz * g.u + tri.n[2].xyz * g.v);
    float2 uv = float2(tri.p[0].w * w0 + tri.p[1].w * g.u + tri.p[2].w * g.v, tri.n[0].w * w0 + tri.n[1].w * g.u + tri.n[2].w * g.v);
    bool hasView = (p.flags & kFlagView) != 0;
    if (hasView && dot(nrm, -p.viewDir.xyz) <= 0.0)
        return;
    if (hasView && (p.flags & kFlagOcclusion) != 0 && !visible(p, pos))
        return;
    bool direction = (p.flags & kFlagDirection) != 0;

    TileInfo* ti = p.tiles + item.tile;
    uint pagesX = max(ti.size.z, 1u);
    int2 xy = int2(int((item.page % pagesX) * kPage + texel % kPage), int((item.page / pagesX) * kPage + texel / kPage));
    float tw = float(ti.size.x), th = float(ti.size.y);

    float3 tangent = float3(0.0);
    float3 bitangent = float3(0.0);
    if (direction || p.mode == kModeNormal)
    {
        float4 t4 = tri.t[0] * w0 + tri.t[1] * g.u + tri.t[2] * g.v;
        tangent = t4.xyz - nrm * dot(nrm, t4.xyz);
        tangent = dot(tangent, tangent) > 1e-12 ? normalize(tangent) : float3(1, 0, 0);
        bitangent = cross(nrm, tangent) * (tri.t[0].w < 0.0 ? -1.0 : 1.0);
    }

    float4 acc = item.acc[texel];
    float cov = acc.w;
    float3 value = acc.xyz;
    bool changed = false;
    float3 projected = float3(0.0);

    if (p.mode == kModeFill)
    {
        if ((p.triMask[g.triangle >> 5] & (1u << (g.triangle & 31u))) == 0)
            return;
        cov = 1.0;
        changed = true;
    }
    else if (p.mode == kModeProject)
    {
        float4 clip = mul(p.objectToClip, float4(pos, 1.0));
        if (clip.w <= 1e-9)
            return;
        float2 ndc = clip.xy / clip.w;
        if (any(abs(ndc) > 1.0))
            return;
        float4 s = imageAt(p.image, p.imageW, p.imageH, ndc * 0.5 + 0.5, false);
        float facing = dot(nrm, normalize(p.eye.xyz - pos));
        float fade = p.facingFade > 0.0 ? smoothstep(0.0, p.facingFade, facing) : 1.0;
        float a = saturate(s.a) * fade;
        if (a <= 0.0)
            return;
        // Symmetry copies project one after another into the same build-up.
        float next = cov + (1.0 - cov) * a;
        value = next > 0.0 ? (value * cov + s.rgb * (next - cov)) / next : s.rgb;
        cov = next;
        projected = value;
        changed = true;
    }
    else
    {
        for (uint k = 0; k < item.dabCount; ++k)
        {
            Dab d = p.dabs[p.dabIndex[item.dabFirst + k]];
            // Without a view, the dab's own side of the surface only: a thin shell's back stays clean.
            if (!hasView && dot(nrm, d.normalAmount.xyz) <= -0.25)
                continue;
            float3 rel = pos - d.centerRadius.xyz;
            float w = tipWeight(p, d, rel) * d.normalAmount.w;
            if ((direction || (p.mode == kModeFilter && p.sub == kFilterSmudge)) && d.direction.w == 0.0)
                w = 0.0;
            if (w <= 0.0)
                continue;
            float next = cov + (1.0 - cov) * saturate(w);
            if (direction)
            {
                float2 dir = float2(dot(d.direction.xyz, tangent), dot(d.direction.xyz, bitangent));
                float len = length(dir);
                float3 enc;
                if ((p.flags & kFlagFlow) != 0)
                    enc = float3(len > 1e-12 ? dir / len * 0.5 + 0.5 : float2(0.5), 0.5);
                else
                {
                    float3 n = len > 1e-12 ? normalize(float3(dir / len * p.tilt, 1.0)) : float3(0, 0, 1);
                    enc = n * 0.5 + 0.5;
                }
                value = next > 0.0 ? (value * cov + enc * (next - cov)) / next : enc;
            }
            else if (p.mode == kModeFilter && p.sub == kFilterSmudge)
            {
                float3 along = normalize(d.direction.xyz) * d.centerRadius.w * p.amount;
                float3 disp = float3(toTexels(tri, along, tw, th), 0.0);
                value = next > 0.0 ? (value * cov + disp * (next - cov)) / next : disp;
            }
            else if (p.mode == kModeFilter && (p.sub == kFilterClone || p.sub == kFilterHeal))
            {
                // Each symmetry copy brings its own source offset (uv) in direction.xy.
                float3 off = float3(d.direction.xy, 0.0);
                value = next > 0.0 ? (value * cov + off * (next - cov)) / next : off;
            }
            else if (p.mode == kModeNormal && p.sub == kNormalBump)
            {
                // The tip's slope along the texel's tangent frame, as a fraction of the radius.
                float r = max(d.centerRadius.w, 1e-12);
                float e = r * 0.05;
                float gx = (tipWeight(p, d, rel + tangent * e) - tipWeight(p, d, rel - tangent * e)) / (2.0 * e);
                float gy = (tipWeight(p, d, rel + bitangent * e) - tipWeight(p, d, rel - bitangent * e)) / (2.0 * e);
                value.xy += float2(gx, gy) * r * 0.5 * d.normalAmount.w * p.intensity;
            }
            else if (p.mode == kModeNormal)
            {
                float3 l = tipLocal(d, rel);
                float3 u = d.tangentRoundness.xyz;
                float3 v = cross(d.normalAmount.xyz, u) * (d.tangentRoundness.w < 0.0 ? -1.0 : 1.0);
                float4 s = imageAt(p.stamp, p.stampW, p.stampH, float2(l.x, -l.y) * 0.5 + 0.5, false);
                float3 sn = decodeNormal(s.rgb);
                float3 obj = u * sn.x + v * sn.y;
                float3 det = normalize(float3(dot(obj, tangent), dot(obj, bitangent), max(sn.z, 1e-3)));
                float sw = w * s.a;
                float nextS = cov + (1.0 - cov) * saturate(sw);
                value = nextS > 0.0 ? (value * cov + det * (nextS - cov)) / nextS : det;
                next = nextS;
            }
            cov = next;
            changed = true;
        }
    }
    if (!changed)
        return;
    item.acc[texel] = float4(value, cov);
    float t = cov * p.strength;
    if (p.limit != nullptr)
        t *= saturate(imageAt(p.limit, p.limitW, p.limitH, float2(frac(uv.x), 1.0 - frac(uv.y)), false).a);
    // A facing limit: surfaces turned away from the view fade out (a projection fades its own way above).
    if (p.facingFade > 0.0 && hasView && p.mode != kModeProject)
        t *= smoothstep(0.0, p.facingFade, dot(nrm, normalize(p.eye.xyz - pos)));
    float gradT = 0.0;
    if (p.gradOn != 0)
    {
        gradT = gradientT(p, pos, uv);
        // Dither: half a step of 8 bits either way, fixed per texel, so banding breaks up and replays the same; the ends stay exact.
        if (p.gradDither != 0 && gradT > 0.0 && gradT < 1.0)
        {
            uint hsh = (uint(xy.x) * 73856093u) ^ (uint(xy.y) * 19349663u) ^ (item.tile * 83492791u);
            hsh = hsh * 1664525u + 1013904223u;
            gradT = saturate(gradT + (float(hsh >> 8) / 16777216.0 - 0.5) / 255.0);
        }
    }
    float3 stencilRgb = float3(0.0);
    if ((p.stencilFlags & kStencilOn) != 0)
    {
        float4 st = stencilAt(p, pos);
        t *= st.a;
        stencilRgb = st.rgb;
    }
    float2 here = float2(xy) + 0.5;
    bool hex = p.hexOn != 0 && (p.flags & kFlagValueImages) != 0;
    // Object space: the source keeps one size on the surface (valueSize cm a tile), whatever the uvs' density.
    bool objectSpace = p.valueSpace != 0;
    float3 triW = float3(0.0);
    float2 planes[3] = { float2(0.0), float2(0.0), float2(0.0) };
    if (objectSpace)
    {
        float3 q = pos * (p.valueScale / max(p.valueSize, 1e-3));
        planes[0] = q.yz;
        planes[1] = q.xz;
        planes[2] = q.xy;
        float3 a = pow(abs(nrm), float3(4.0));
        triW = a / max(a.x + a.y + a.z, 1e-6);
    }
    float2 hexSt = uv * p.valueScale;
    if (objectSpace)
        hexSt = triW.x >= triW.y && triW.x >= triW.z ? planes[0] : triW.y >= triW.z ? planes[1] : planes[2];
    HexTiling hexCells = {};
    float3 hexW = float3(0.0);
    if (hex)
    {
        hexCells = hexTiling(p, hexSt);
        hexW = hexWeights(p, hexCells);
    }
    for (uint c = 0; c < p.channels; ++c)
    {
        uint2* dst = item.dst[c];
        uint2* src = item.src[c];
        float4 o = src != nullptr ? loadHalf4(src[texel]) : float4(0.0);
        bool mask = c == p.maskChannel;
        bool isNormal = ((p.normalMask >> c) & 1u) != 0;
        float4 r;
        if (p.mode == kModeFilter)
        {
            // The mask filters with the channels, so a filtered layer keeps its own coverage.
            float4 f = o;
            if (p.sub == kFilterSmudge)
                f = tileSample(ti, c, here - value.xy, mask);
            else if (p.sub == kFilterBlur)
                f = tileBlur(ti, c, xy, p.filterRadius, mask);
            else if (p.sub == kFilterSharpen)
            {
                float4 b = tileBlur(ti, c, xy, p.filterRadius, mask);
                f = o + (o - b) * p.amount;
                f.a = saturate(f.a);
            }
            else
            {
                float2 off = float2(value.x * tw, -value.y * th);
                float4 s = tileSample(ti, c, here + off, mask);
                if (p.sub == kFilterHeal)
                {
                    float4 lo = tileBlur(ti, c, xy, p.filterRadius, mask);
                    float4 slo = tileBlur(ti, c, xy + int2(round(off)), p.filterRadius, mask);
                    f = lo + (s - slo);
                    f.a = saturate(f.a);
                }
                else
                    f = s;
            }
            r = lerp(o, f, t);
            if (mask)
                r = float4(saturate(r.a));
        }
        else if (p.mode == kModeNormal && c == p.directionChannel)
        {
            float3 base = o.a > 0.0 ? decodeNormal(o.rgb) : float3(0, 0, 1);
            float3 det;
            if (p.sub == kNormalBump)
                det = normalize(float3(-value.x, -value.y, 1.0));
            else
                det = normalize(lerp(float3(0, 0, 1), value, saturate(t * abs(p.intensity))));
            r = float4(encodeNormal(rnm(base, det)), max(o.a, t));
        }
        else
        {
            float3 v = p.values[c].xyz;
            float tc = t;
            if (p.gradOn != 0)
            {
                float4 g = rampAt(p, c, gradT);
                v = g.rgb;
                tc = t * g.a;
            }
            if (c == p.stencilColor)
                v = stencilRgb;
            else if (direction && c == p.directionChannel)
                v = value;
            else if (p.mode == kModeProject && c == p.colorChannel)
                v = projected;
            else if (hex && p.valueImages[c] != nullptr)
                v = hexValue(p, hexCells, hexW, c, isNormal);
            else if ((p.flags & kFlagValueImages) != 0 && p.valueImages[c] != nullptr && objectSpace)
            {
                v = float3(0.0);
                for (int k = 0; k < 3; ++k)
                    if (triW[k] > 0.0)
                        v += triW[k] * imageAt(p.valueImages[c], p.valueW, p.valueH, float2(planes[k].x, 1.0 - planes[k].y), true).xyz;
            }
            else if ((p.flags & kFlagValueImages) != 0 && p.valueImages[c] != nullptr)
                v = imageAt(p.valueImages[c], p.valueW, p.valueH, float2(uv.x * p.valueScale, 1.0 - uv.y * p.valueScale), true).xyz;
            r = blendTexel(p, o, v, tc, mask);
        }
        if (isNormal && !mask)
            r.rgb = encodeNormal(decodeNormal(r.rgb));
        // Stored as halves after every stroke update, so live and replayed strokes agree bit for bit.
        dst[texel] = storeHalf4(r);
    }
}
