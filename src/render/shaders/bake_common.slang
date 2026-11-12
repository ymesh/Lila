// Shared by the bake kernels (included, not compiled on its own). Records mirror src/bake/BakeTypes.h, the math mirrors
// src/bake/Maps.cpp so GPU and CPU bakes agree to float precision. Buffers are reached through device addresses.

struct TexelSample
{
    float3 position;
    uint texel;
    float3 normal;
    uint lowTriangle;
    float4 tangent;
    float3 origin;
    float rayLength;
    float3 direction;
    uint sub;
    float2 uv;
    uint tile;
    uint low;
};

struct HitRecord
{
    float3 position;
    uint high;
    float3 normal;
    uint triangle;
    float2 bary;
    float distance;
    float height;
    uint flags;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct BvhNode
{
    float3 lo;
    uint first;
    float3 hi;
    uint count;
};

// One high: shading normals (9 floats per triangle), material slot per triangle, and for TRACE_SW its flat BVH with
// leaf-ordered corners (w of corner 0 is the soup triangle). positions and uvs (per corner) are there only when a map
// needs them (the high's own normal and material maps).
struct HighInfo
{
    float* normals;
    uint* materials;
    BvhNode* nodes;
    float4* corners;
    uint triangles;
    // Colour table entries from materialBase: one per material slot, then the "Default" colour.
    uint materialBase;
    uint materialCount;
    uint pad0;
    float* positions;
    float2* uvs;
};

// One low of a group, per corner (3 per triangle) unless noted. Mirrors GpuLowInfo in src/render/Baker.cpp.
struct LowInfo
{
    float* positions;
    float* normals;
    float4* tangents;
    float2* uvs;
    float* cageNormals;
    float* cageScale;
    float* cageSkew;      // null when the low has none
    float* cagePositions; // null for the auto cage
    uint* materials;      // per triangle
    uint* edges;          // per triangle: bits 0-2 polygon edges, bits 3-5 island borders (edge k runs corner k to k + 1)
    uint triangles;
    uint hasSkew;
    uint pad1;
    uint pad2;
};

// A texture the kernels sample (row 0 the top, V up), RGBA floats.
struct TextureRef
{
    float4* texels;
    uint width;
    uint height;
    uint flags; // normal maps: 1 = OpenGL (green up)
    uint pad0;
    uint pad1;
    uint pad2;
};

// What a material transfer map reads for one material slot: a texture (texture.texels null for none) or the constants.
struct MaterialSlot
{
    float4 baseColor; // linear
    float4 emissive;  // linear rgb, w blend mode
    float4 scalars;   // roughness, metallic, opacity, emissive intensity
    TextureRef texture;
    uint channel;
    uint srgb; // the texture's values are sRGB-encoded
    uint pad1;
    uint pad2;
};

// Per-high tables: one entry per high of the group (present 0 when that high has none).
struct CornerFloats
{
    float* corners;
    uint present;
    uint pad0;
};

struct CornerColors
{
    float4* corners;
    uint present;
    uint pad0;
};

static const uint kNoHit = 0xffffffffu;
static const uint kHitValid = 1;
static const uint kHitBackface = 2;
static const uint kHitReaimed = 4;
static const uint kHitProbed = 8;

static const uint kMapNormal = 0;
static const uint kMapObjectNormal = 1;
static const uint kMapPosition = 2;
static const uint kMapHeight = 3;
static const uint kMapId = 4;
static const uint kMapAlpha = 5;
static const uint kMapIslands = 6;
static const uint kMapCurvatureMesh = 7;
static const uint kMapSlopes = 8;
static const uint kMapVertexColor = 9;
static const uint kMapIdVertexColor = 10;
static const uint kMapMaterial = 11;
static const uint kMapEdges = 12;
static const uint kMapGradient = 13;
static const uint kMapError = 14;

static const uint kFlagCullBack = 1;
static const uint kFlagGreenUp = 2;
static const uint kFlagFlipX = 4;
static const uint kFlagHalf = 8;
static const uint kFlagScopeSelf = 16;
// Resident bakes: out holds the value, valid a flag per slot, and slots with no sample are skipped.
static const uint kFlagResident = 32;
static const uint kFlagNormalMaps = 64;
static const uint kFlagObjectSpace = 128;
// links holds the highs each low's rays skip (GroupInputs::links).
static const uint kFlagLinks = 256;

// Mirrors GpuBakePush in src/render/Baker.cpp.
struct BakePush
{
    TexelSample* samples;
    HitRecord* hits;
    float4* outFloat;
    uint2* outHalf;
    HighInfo* highs;
    uint* highList;  // the highs to trace (TRACE_SW), highCount of them
    uint* table;     // id: packed colour per high material slot; islands: packed colour per low triangle
    uint* tableBase; // islands: first table entry of each low of the group
    float4 boundsMin; // xyz position map bounds, w AO distance
    float4 boundsExt; // xyz position map extent, w ray epsilon
    uint first;
    uint count;
    uint highCount;
    uint map;
    uint flags;
    uint rays;
    uint* instanceBase; // TRACE_HW: first soup triangle of each TLAS instance (highs are split into BLAS chunks)
    float4 params;       // per kernel
    float4 params2;
    LowInfo* lows;       // the group's lows
    uint* extra;         // per kernel table
    uint* valid;         // resident: a flag per slot
    uint* stats;         // projection counters: samples, hits, backfaces
    uint width;
    uint height;
    uint mode;
    uint pad0;
    TextureRef* normalMaps; // the high's own normal map per material slot (from materialBase), with kFlagNormalMaps
    uint* links;            // with kFlagLinks: linkWords words per low and ray kind, bits over every group's highs
    uint linkWords;
    uint highBase;          // added to a traced high's index to number it among every group's
};

#if !defined(BAKE_OWN_PUSH)
[[vk::push_constant]] BakePush pc;
#endif

float3 unitOr(float3 v, float3 fallback)
{
    float l = length(v);
    return l > 1e-20 ? v / l : fallback;
}

uint hashBits(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float radicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// Rings of probes widening around a missed projection ray, as kProbeRings / kProbePerRing in MapMath.h.
static const uint kProbeRings = 22;
static const uint kProbePerRing = 16;

// Probe j of ring k around unit axis d, ring k at k + 1 degrees off it, as probeDirection in MapMath.h.
float3 probeDirection(uint k, uint j, float3 d, float3 t, float3 b)
{
    float a = radians(float(k + 1));
    float phi = 6.28318530718 * (float(j) + 0.5 * float(k & 1u)) / float(kProbePerRing);
    return normalize(d * cos(a) + (t * cos(phi) + b * sin(phi)) * sin(a));
}

void basis(float3 n, out float3 t, out float3 b)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z), c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

float3 encodeTangentNormal(float3 highNormal, float3 lowNormal, float4 lowTangent, bool greenUp, bool flipX)
{
    float3 n = unitOr(lowNormal, float3(0, 0, 1));
    float3 tg = lowTangent.xyz;
    float3 fallback = abs(n.x) < 0.9 ? unitOr(cross(n, float3(1, 0, 0)), float3(0, 0, 1)) : unitOr(cross(n, float3(0, 1, 0)), float3(0, 0, 1));
    float3 t = unitOr(tg - n * dot(n, tg), fallback);
    float3 b = cross(n, t) * (lowTangent.w < 0.0 ? -1.0 : 1.0);
    float3 h = unitOr(highNormal, n);
    float3 e = float3(dot(h, t), dot(h, b), dot(h, n));
    if (!greenUp)
        e.y = -e.y;
    if (flipX)
        e.x = -e.x;
    return e;
}

float4 unpackRgba8(uint c)
{
    return float4(float(c & 0xff), float((c >> 8) & 0xff), float((c >> 16) & 0xff), float(c >> 24)) / 255.0;
}

#if !defined(BAKE_OWN_PUSH)
// Alpha carries validity on the band path (every ported map writes alpha 1 where its sample counts); resident bakes keep
// alpha as data and the flag apart.
void writeOut(uint i, float4 v, bool valid)
{
    if ((pc.flags & kFlagResident) != 0)
    {
        pc.outFloat[i] = v;
        pc.valid[i] = valid ? 1u : 0u;
        return;
    }
    v.w = valid ? 1.0 : 0.0;
    if ((pc.flags & kFlagHalf) != 0)
        pc.outHalf[i] = uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16));
    else
        pc.outFloat[i] = v;
}

float3 hitNormal(uint high, uint tri, float2 bary)
{
    float* n = pc.highs[high].normals + tri * 9;
    float w0 = 1.0 - bary.x - bary.y;
    float3 v = float3(n[0], n[1], n[2]) * w0 + float3(n[3], n[4], n[5]) * bary.x + float3(n[6], n[7], n[8]) * bary.y;
    return unitOr(v, float3(0, 0, 1));
}
#endif

float3 load3(float* p, uint i)
{
    return float3(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]);
}

// The math of src/bake/MapMath.h, line for line.

uint sampleSeed(TexelSample s)
{
    return hashBits(s.texel * 64u + s.sub) ^ hashBits(s.tile + 0x9e3779b9u);
}

float2 rayPoint(uint k, uint n, uint seed)
{
    float r1 = float(seed & 0xffff) / 65536.0, r2 = float(seed >> 16) / 65536.0;
    return float2(fmod((float(k) + 0.5) / float(n) + r1, 1.0), fmod(radicalInverse(k) + r2, 1.0));
}

float3 coneDirection(float3 axis, float2 u, float halfAngle)
{
    float3 t, b;
    basis(axis, t, b);
    float r = sqrt(u.x) * sin(clamp(halfAngle, 0.0, 1.57079632679));
    float phi = 6.28318530718 * u.y;
    return unitOr(t * (r * cos(phi)) + b * (r * sin(phi)) + axis * sqrt(max(0.0, 1.0 - r * r)), float3(0, 0, 1));
}

float smoothstepCpu(float e0, float e1, float x)
{
    float t = e1 > e0 ? clamp((x - e0) / (e1 - e0), 0.0, 1.0) : (x >= e0 ? 1.0 : 0.0);
    return t * t * (3.0 - 2.0 * t);
}

float linearToSrgb1(float c)
{
    c = max(c, 0.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float srgbToLinear1(float c)
{
    c = max(c, 0.0);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 linearToSrgb(float3 c)
{
    return float3(linearToSrgb1(c.x), linearToSrgb1(c.y), linearToSrgb1(c.z));
}

float3 srgbToLinear(float3 c)
{
    return float3(srgbToLinear1(c.x), srgbToLinear1(c.y), srgbToLinear1(c.z));
}

float quantize8(float v)
{
    return floor(clamp(v, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
}

uint packRgba8(float4 c)
{
    uint4 q = uint4(floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5));
    return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24);
}

// TextureSampler::sample: bilinear with wrap at Lila UVs.
float4 sampleTexture(TextureRef t, float2 uv)
{
    if (t.width == 0)
        return float4(1.0);
    float x = uv.x * float(t.width) - 0.5, y = (1.0 - uv.y) * float(t.height) - 0.5;
    float fx = floor(x), fy = floor(y);
    float ax = x - fx, ay = y - fy;
    int w = int(t.width), h = int(t.height);
    int x0 = ((int(fx) % w) + w) % w, x1 = ((int(fx) + 1) % w + w) % w;
    int y0 = ((int(fy) % h) + h) % h, y1 = ((int(fy) + 1) % h + h) % h;
    float4 a = t.texels[uint(y0) * t.width + uint(x0)], b = t.texels[uint(y0) * t.width + uint(x1)];
    float4 c = t.texels[uint(y1) * t.width + uint(x0)], d = t.texels[uint(y1) * t.width + uint(x1)];
    return lerp(lerp(a, b, ax), lerp(c, d, ax), ay);
}

#if !defined(BAKE_OWN_PUSH)
float2 hitUv(uint high, HitRecord h)
{
    HighInfo hi = pc.highs[high];
    float2* uv = hi.uvs + h.triangle * 3;
    return uv[0] * (1.0 - h.bary.x - h.bary.y) + uv[1] * h.bary.x + uv[2] * h.bary.y;
}

// The hit's shading normal, bent by its high's normal map where kFlagNormalMaps names one (hitNormal in MapsCommon.cpp).
float3 mappedNormal(HitRecord h)
{
    float3 n = h.normal;
    if ((pc.flags & kFlagNormalMaps) == 0 || (h.flags & kHitValid) == 0)
        return n;
    HighInfo hi = pc.highs[h.high];
    uint slot = h.triangle < hi.triangles ? hi.materials[h.triangle] : 0u;
    if (slot >= hi.materialCount)
        return n;
    TextureRef map = pc.normalMaps[hi.materialBase + slot];
    if (map.width == 0)
        return n;
    float3 p0 = load3(hi.positions, h.triangle * 3), p1 = load3(hi.positions, h.triangle * 3 + 1), p2 = load3(hi.positions, h.triangle * 3 + 2);
    float2* uv = hi.uvs + h.triangle * 3;
    float3 e1 = p1 - p0, e2 = p2 - p0;
    float2 d1 = uv[1] - uv[0], d2 = uv[2] - uv[0];
    float det = d1.x * d2.y - d2.x * d1.y;
    if (abs(det) < 1e-20)
        return n;
    float3 tu = (e1 * d2.y - e2 * d1.y) / det, tv = (e2 * d1.x - e1 * d2.x) / det;
    float3 t = unitOr(tu - n * dot(n, tu), float3(1, 0, 0));
    float s = dot(cross(n, t), tv) < 0.0 ? -1.0 : 1.0;
    float3 b = cross(n, t) * s;
    float3 m = sampleTexture(map, hitUv(h.high, h)).xyz * 2.0 - 1.0;
    if ((map.flags & 1) == 0)
        m.y = -m.y;
    return unitOr(t * m.x + b * m.y + n * m.z, n);
}
#endif
