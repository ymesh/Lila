// Shaded meshes and the environment background: Unreal's default lit model (metallic/specular/roughness, packed
// ORM, DirectX or OpenGL normal maps, masked and translucent blending), split-sum image-based lighting with
// energy-conserving GGX, a camera-relative studio rig while no environment is loaded, Khronos PBR Neutral tonemap.
// mesh_bindless.slang compiles this with LILA_BINDLESS: material maps (and UDIM tiles) come from one runtime array.
// Stylised models (Painterly, Toon, Foliage) read a style block from the frame's table; see render::packStyle.

/** One placement of a draw; matches render::GpuInstance. */
struct InstanceData
{
    float4x4 world;
    float4 color; // wire colour; for shaded draws the wire meshWirePS draws over them, alpha 0 for none
};

/** One light; matches render::GpuLight. Kinds: 0 point, 1 spot, 2 directional, 3 area, 4 disc. */
struct GpuLight
{
    float4 position;   // xyz, w kind
    float4 direction;  // xyz where the light travels, w range (cm, 0 none)
    float4 radiance;   // rgb, w radius (cm)
    float4 cone;       // cos outer, 1 / (cos inner - cos outer), width, height
    float4 right;      // xyz area width axis, w shadow texel (cm)
    float4x4 shadow;   // world to the light's shadow clip space
    float4 shadowRect; // atlas uv offset xy, scale zw; zw 0 without a shadow
    float4 shadowDepth; // near, far (cm), light size (sun: tan of angular radius, spot: cm), shadow uv per cm (spot: at 1 cm)
};

/** The light buffer's first entry: the frame's fog; matches render::GpuFrame. The lights follow it. */
struct GpuFrame
{
    float4 fogColor;  // rgb, w density per cm (0 off)
    float4 fogParams; // height, falloff per cm, start, sun scatter
    float4 fogExtra;  // anisotropy, up axis (1 Y, 2 Z)
    float4 shadowing; // one past the light each rtShadowTex channel traces, 0 for none
    float4 pad[7];
};

struct MeshPush
{
    float4x4 viewProj; // background: the inverse view-projection
    InstanceData* instances; // the frame's instance buffer
    uint firstInstance;      // this draw's first entry in it
    float keyTilt;           // the sculpt rig's key angle off the view direction, radians
    uint* edgeMask;     // meshWirePS: polygon sides per triangle, 4 bits each (RenderMesh::edgeMask)
    uint firstTriangle; // meshWirePS: the section's first triangle, which SV_PrimitiveID counts from
    float wireWidth;    // meshWirePS: pixels
    GpuLight* lights; // the frame's fog (as a GpuFrame), then its lights
    uint lightCount;
    uint lightPad; // the draw's lighting channel bits; 0 lights it with every light
    float4 extra;   // x: live bevel radius (cm), y: 1 when the bevel pass ran this frame
    float4 eye;     // xyz camera position
    float4 base;    // rgb base color (linear), a: roughness. Background: x blur lod, y saturation, z exposure
    float4 params;  // x: exposure, y: 1 if Z-up, z: highlight amount, w: 0 studio rig, 1 environment, 2 flat (unlit), 3 sculpt rig
    float4 env;     // xy: cos/sin of the rotation, z: intensity, w: lod of the roughest mip
    float4 surface; // x metallic, y specular, z opacity, w opacity mask clip
    float3 emissive; // linear; scales the emissive map when there is one
    uint flags;      // render::MaterialFlags
    float gamma;     // display gamma
    uint style;      // word offset of the draw's style block in the frame's table; kNoStyle for default lit
    uint backTint;   // back-face tint: rgba8, display-encoded rgb and its mix in alpha; 0 is off
    uint sampling;   // render::Material::sampling: per map 8 bits, wrap U (bits 0-1), wrap V (2-3), nearest (4)
    uint* textureTable; // the frame's table: bindless texture handles, then style blocks
    uint2 maps;         // bindless: 16-bit table handles, x: base color | normal << 16, y: ORM | emissive << 16
};

[[vk::push_constant]] MeshPush pc;

struct EnvUniforms
{
    float4 sh[9]; // irradiance / pi, rgb
};

[[vk::binding(0, 0)]] SamplerCube envMap;
[[vk::binding(1, 0)]] Sampler2D brdfLut;
[[vk::binding(2, 0)]] ConstantBuffer<EnvUniforms> envU;
[[vk::binding(3, 0)]] Sampler2D shadowAtlas; // reversed-Z depth, a tile per shadowed light
[[vk::binding(3, 2)]] Sampler2D bevelTex;    // rtbevel.slang's rounded normals, w 1 where rounded
[[vk::binding(4, 2)]] Sampler2D rtShadowTex; // rtshadow.slang's visibility, a channel per light GpuFrame::shadowing names

static const uint kBaseColorSlot = 0;
static const uint kNormalSlot = 1;
static const uint kOrmSlot = 2; // R occlusion, G roughness, B metallic
static const uint kEmissiveSlot = 3;

static const uint kWrapClamp = 1;
static const uint kWrapMirror = 2;
static const uint kWrapBlack = 3;
static const uint kSampleNearest = 16;

/** One uv axis wrapped by mode; a black border flags outside instead. */
float wrapAxis(float x, uint mode, float halfTexel, inout bool outside)
{
    if (mode == kWrapClamp)
        return clamp(x, halfTexel, 1.0 - halfTexel);
    if (mode == kWrapMirror)
    {
        float m = x - 2.0 * floor(0.5 * x);
        return m > 1.0 ? 2.0 - m : m;
    }
    if (mode == kWrapBlack && (x < 0.0 || x > 1.0))
        outside = true;
    return x;
}

/** Screen derivatives of the uv at the pixel centre (Centers::uv), set once per pixel before any map is sampled. */
static float2 gUvDx = float2(0, 0), gUvDy = float2(0, 0);

/**
 * A plain map sampled with its slot's wrap and filter (pc.sampling); the bound sampler repeats and filters linearly.
 * Gradients come from the unwrapped uv, so folds and clamps don't change the mip level.
 */
float4 sampleWrapped(Sampler2D tex, uint slot, float2 uv)
{
    uint mode = (pc.sampling >> (slot * 8)) & 0xff;
    if (mode == 0)
        return tex.SampleGrad(uv, gUvDx, gUvDy);
    float2 dx = gUvDx, dy = gUvDy;
    uint w, h;
    tex.GetDimensions(w, h);
    float2 size = float2(max(w, 1u), max(h, 1u));
    bool outside = false;
    float2 st = float2(wrapAxis(uv.x, mode & 3, 0.5 / size.x, outside), wrapAxis(uv.y, (mode >> 2) & 3, 0.5 / size.y, outside));
    if (outside)
        return float4(0.0);
    if ((mode & kSampleNearest) != 0)
        st = (floor(st * size) + 0.5) / size;
    return tex.SampleGrad(st, dx, dy);
}

#ifdef LILA_BINDLESS
[[vk::binding(0, 1)]] Sampler2D materialTextures[];

static const uint kUdimEntry = 0x80000000;

/**
 * The array index a map samples at uv. Its table entry is the index itself, or for a UDIM set a header (kUdimEntry |
 * rows), the placeholder's index, then ten indices per row of tiles; the tile comes from floor of the V-up uv.
 */
uint mapIndex(uint slot, float2 uv)
{
    uint packed = slot < 2 ? pc.maps.x : pc.maps.y;
    uint h = (packed >> ((slot & 1) * 16)) & 0xffff;
    uint head = pc.textureTable[h];
    if ((head & kUdimEntry) == 0)
        return head;
    int col = int(floor(uv.x));
    int row = int(floor(1.0 - uv.y));
    if (col < 0 || col >= 10 || row < 0 || row >= int(head & 0xffff))
        return pc.textureTable[h + 1];
    return pc.textureTable[h + 2 + uint(row) * 10 + uint(col)];
}

// Virtual texture maps (render/VirtualTexture.h). The map's table entry: kVtEntry | chunk count, the page table's
// address (two words), the feedback group, then the bindless index of each chunk image of this map's layer. The page
// table starts with a 10x10 grid of UDIM cells (entry base + 1, width, height, mip count), then per tile and level the
// page entries: 0 for missing, else kVtResident | fade << 16 | slot.
static const uint kVtEntry = 0x40000000;
static const uint kVtRepeat = 0x20000000;
static const uint kVtResident = 0x80000000;
static const uint kVtPage = 128;
static const uint kVtBorder = 4;
static const uint kVtSlot = 136;
static const uint kVtChunkSlots = 16;
static const uint kVtFeedbackCell = 4;

// The pixel being shaded, for feedback (set at the top of each fragment entry point).
static uint2 gVtPixel;

struct VtTile
{
    uint base;
    uint2 size;
    uint mips;
};

uint* vtPageTable(uint h)
{
    uint64_t a = uint64_t(pc.textureTable[h + 1]) | (uint64_t(pc.textureTable[h + 2]) << 32);
    return (uint*)a;
}

uint2 vtLevelSize(VtTile t, uint m) { return max(t.size >> m, uint2(1, 1)); }
uint2 vtPages(uint2 levelSize) { return (levelSize + kVtPage - 1) / kVtPage; }

uint vtLevelBase(VtTile t, uint m)
{
    uint b = t.base;
    for (uint k = 0; k < m; ++k)
    {
        uint2 p = vtPages(vtLevelSize(t, k));
        b += p.x * p.y;
    }
    return b;
}

/** The entry of the page holding f (tile fraction, rows down) at level m, and that page. */
uint vtEntry(uint* pt, VtTile t, uint m, float2 f, out uint2 page)
{
    uint2 ls = vtLevelSize(t, m);
    uint2 pages = vtPages(ls);
    page = min(uint2(f * float2(ls)) / kVtPage, pages - 1);
    return pt[vtLevelBase(t, m) + page.y * pages.x + page.x];
}

float4 vtSlotSample(uint h, uint entry, VtTile t, uint m, float2 f, uint2 page)
{
    uint slot = entry & 0xffff;
    uint chunk = slot / (kVtChunkSlots * kVtChunkSlots);
    if (chunk >= (pc.textureTable[h] & 0xffff))
        return float4(0.0, 0.0, 0.0, 1.0);
    uint within = slot % (kVtChunkSlots * kVtChunkSlots);
    float2 origin = float2(float(within % kVtChunkSlots), float(within / kVtChunkSlots)) * float(kVtSlot) + float(kVtBorder);
    float2 p = origin + f * float2(vtLevelSize(t, m)) - float2(page * kVtPage);
    uint index = pc.textureTable[h + 4 + chunk];
    return materialTextures[NonUniformResourceIndex(index)].SampleLevel(p / float(kVtSlot * kVtChunkSlots), 0.0);
}

/** Level m as resident: the finest resident page at or above it, fading in over the next resident one up. */
float4 vtLevel(uint h, uint* pt, VtTile t, float2 f, uint m)
{
    uint2 page;
    uint l = m;
    uint e = 0;
    for (; l < t.mips; ++l)
    {
        e = vtEntry(pt, t, l, f, page);
        if (e != 0)
            break;
    }
    if (e == 0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float4 c = vtSlotSample(h, e, t, l, f, page);
    float fade = float((e >> 16) & 0xff) / 255.0;
    if (fade >= 1.0)
        return c;
    for (uint k = l + 1; k < t.mips; ++k)
    {
        uint2 up;
        uint u = vtEntry(pt, t, k, f, up);
        if (u != 0)
            return lerp(vtSlotSample(h, u, t, k, f, up), c, fade);
    }
    return c;
}

/** One pixel of each 4x4 cell (cycling over frames) asks for the page it would sample at its level. */
void vtFeedback(uint h, uint cell, uint m, VtTile t, float2 f)
{
    uint64_t a = uint64_t(pc.textureTable[0]) | (uint64_t(pc.textureTable[1]) << 32);
    if (a == 0)
        return;
    uint jitter = pc.textureTable[3];
    if (gVtPixel.x % kVtFeedbackCell != (jitter & 0xff) || gVtPixel.y % kVtFeedbackCell != (jitter >> 8))
        return;
    uint2 page = min(uint2(f * float2(vtLevelSize(t, m))) / kVtPage, vtPages(vtLevelSize(t, m)) - 1);
    uint2* cells = (uint2*)a;
    uint index = (gVtPixel.y / kVtFeedbackCell) * pc.textureTable[2] + gVtPixel.x / kVtFeedbackCell;
    cells[index] = uint2(0x80000000 | (m << 24) | (cell << 16) | (pc.textureTable[h + 3] & 0xffff), page.x | (page.y << 16));
}

/** A virtual texture map at uv (V-up uv with the row flipped, as the vertex shader hands it on): trilinear over its levels. */
float4 vtSample(uint h, float2 uv)
{
    uint* pt = vtPageTable(h);
    // Derivatives first, while every pixel of the quad is still here.
    float2 duvx = gUvDx, duvy = gUvDy;
    if ((pc.textureTable[h] & kVtRepeat) != 0)
        uv = float2(frac(uv.x), 1.0 - frac(1.0 - uv.y));
    int col = int(floor(uv.x));
    int row = int(floor(1.0 - uv.y));
    if (col < 0 || col >= 10 || row < 0 || row >= 10)
        return float4(0.0, 0.0, 0.0, 1.0);
    uint cell = uint(row) * 10 + uint(col);
    VtTile t;
    t.base = pt[cell * 4];
    if (t.base == 0)
        return float4(0.0, 0.0, 0.0, 1.0);
    t.base -= 1;
    t.size = uint2(pt[cell * 4 + 1], pt[cell * 4 + 2]);
    t.mips = pt[cell * 4 + 3];
    float2 f = float2(frac(uv.x), frac(uv.y));
    float2 dx = duvx * float2(t.size), dy = duvy * float2(t.size);
    float lod = clamp(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12)), 0.0, float(t.mips - 1));
    uint m = uint(floor(lod));
    float blend = lod - float(m);
    vtFeedback(h, cell, m, t, f);
    float4 c = vtLevel(h, pt, t, f, m);
    if (blend > 0.01 && m + 1 < t.mips)
        c = lerp(c, vtLevel(h, pt, t, f, m + 1), blend);
    return c;
}

// A UDIM tile samples at uv itself: the repeating sampler wraps it into the tile. Wrap and filter settings apply to
// plain maps only.
float4 sampleMap(uint slot, float2 uv)
{
    uint packed = slot < 2 ? pc.maps.x : pc.maps.y;
    uint h = (packed >> ((slot & 1) * 16)) & 0xffff;
    uint head = pc.textureTable[h];
    if ((head & kUdimEntry) == 0 && (head & kVtEntry) != 0)
        return vtSample(h, uv);
    if ((head & kUdimEntry) != 0)
        return materialTextures[NonUniformResourceIndex(mapIndex(slot, uv))].SampleGrad(uv, gUvDx, gUvDy);
    return sampleWrapped(materialTextures[NonUniformResourceIndex(head)], slot, uv);
}
#else
// Per material. Missing maps are bound to 1x1 placeholders and ignored through the flags; a UDIM set binds its first tile.
[[vk::binding(0, 1)]] Sampler2D baseColorTex; // sRGB
[[vk::binding(1, 1)]] Sampler2D normalTex;
[[vk::binding(2, 1)]] Sampler2D ormTex;
[[vk::binding(3, 1)]] Sampler2D emissiveTex;  // sRGB
[[vk::binding(4, 1)]] Sampler2D strokeTex;    // Painterly stroke sheet, linear

static uint2 gVtPixel;

float4 sampleMap(uint slot, float2 uv)
{
    switch (slot)
    {
    case kBaseColorSlot: return sampleWrapped(baseColorTex, slot, uv);
    case kNormalSlot: return sampleWrapped(normalTex, slot, uv);
    case kOrmSlot: return sampleWrapped(ormTex, slot, uv);
    default: return sampleWrapped(emissiveTex, slot, uv);
    }
}
#endif

// Screen-space ambient occlusion at the viewport's resolution; 1x1 white while it is off.
[[vk::binding(0, 2)]] Sampler2D aoTex;
[[vk::binding(1, 2)]] Sampler2D aoDepthTex;  // the single-sample prepass the AO was computed from: depth
[[vk::binding(2, 2)]] Sampler2D aoNormalTex; // and world normal

/** How far a prepass texel is from being this fragment's surface: relative depth error, plus a penalty for a normal that turns away. */
float aoMismatch(int2 q, float depth, float3 n)
{
    float err = abs(aoDepthTex.Load(int3(q, 0)).r - depth) / max(depth, 1e-9);
    float3 nq = aoNormalTex.Load(int3(q, 0)).xyz;
    return err + (dot(nq, n) < 0.9 ? 1.0 : 0.0);
}

/**
 * The AO texel for a fragment. The AO is one sample per pixel but this pass is multisampled, so where two surfaces meet
 * a pixel's AO can belong to the other surface; take the neighbour that matches this fragment's depth and normal.
 */
float sampleAo(int2 px, float depth, float3 n)
{
    uint w, h;
    aoTex.GetDimensions(w, h);
    // The 1x1 stand-in while AO is off.
    if (w == 1 && h == 1)
        return aoTex.Load(int3(0, 0, 0)).r;
    int2 hi = int2(w, h) - 1;
    int2 best = min(px, hi);
    float bestErr = aoMismatch(best, depth, n);
    if (bestErr > 1e-3)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int2 q = clamp(px + int2(x, y), int2(0), hi);
                float err = aoMismatch(q, depth, n);
                if (err < bestErr)
                {
                    bestErr = err;
                    best = q;
                }
            }
    return aoTex.Load(int3(best, 0)).r;
}

/**
 * Widens roughness by how fast the normal turns across the pixel (Tokuyoshi and Kaplanyan 2019), so highlights on
 * curved or bevelled edges thinner than a pixel stop crawling as the camera moves.
 */
/** Roughness the normal map's filtering hides in this pixel (Toksvig), added by specularAntialias. */
static float gNormalVariance = 0.0;

/** The smooth surface normal at the pixel centre (Centers::normal), which specularAntialias differentiates. */
static float3 gCenterNormal = float3(0, 0, 1);

/** How far the reflection of the smooth surface (no normal map) turns across a pixel, radians; see reflectionLod. */
static float gReflectionSweep = 0.0;

float specularAntialias(float3 n, float rough)
{
    float3 du = ddx(gCenterNormal), dv = ddy(gCenterNormal);
    float variance = 0.25 * (dot(du, du) + dot(dv, dv));
    float alpha = rough * rough;
    float alpha2 = saturate(alpha * alpha + min(2.0 * variance, 0.18) + gNormalVariance);
    return sqrt(sqrt(alpha2));
}

static const uint kBaseColorMap = 1 << 0;
static const uint kNormalMap = 1 << 1;
static const uint kNormalGreenUp = 1 << 2;
static const uint kMetallicMap = 1 << 3;
static const uint kRoughnessMap = 1 << 4;
static const uint kOcclusionMap = 1 << 5;
static const uint kEmissiveMap = 1 << 6;
static const uint kMasked = 1 << 7;
static const uint kTranslucent = 1 << 8;
static const uint kUv1 = 1 << 10;
static const uint kVertexColorShaded = 1 << 11;
static const uint kVertexColorUnlit = 1 << 12;
static const uint kColorChannelShift = 13; // 3 bits: 0 rgb, 1 r, 2 g, 3 b, 4 a
static const uint kOneSidedLighting = 1 << 16;
static const uint kNormalXY = 1 << 17; // the normal map stores X and Y only (BC5)
static const uint kGreyBase = 1 << 18;  // the base colour map supplies opacity only

/** The lean pipelines: the section reads no tangent, vertex colour or second uv (Renderer.cpp's leanPush). */
[vk::constant_id(0)] const bool kLean = false;

struct VSIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 color : COLOR;
};

struct VSOut
{
    float4 pos : SV_Position;
    // Centroid: with MSAA a pixel the triangle only partly covers would otherwise extrapolate past its edge.
    centroid float3 worldPos : POSITION;
    // At the pixel centre, for smooth screen derivatives; the shading reads them at the centroid (see atCentroid).
    float3 normal : NORMAL;
    centroid float4 tangent : TANGENT;
    float2 uv : TEXCOORD;
    centroid float4 color : COLOR;
    nointerpolation uint instance : TEXCOORD3; // index into pc.instances
};

float4x4 instanceWorld(uint instance)
{
    return pc.instances[instance].world;
}

// Object space comes back from the world position and normal in the pixel shader: fewer interpolants per vertex,
// which dense meshes are bound by.
float3x3 cofactor(float3x3 m)
{
    return float3x3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
}

float safeDeterminant(float3x3 m)
{
    float d = determinant(m);
    return abs(d) < 1e-20 ? (d < 0.0 ? -1e-20 : 1e-20) : d;
}

float3 objectPos(VSOut i)
{
    float4x4 w = instanceWorld(i.instance);
    float3x3 m = (float3x3)w;
    return mul(transpose(cofactor(m)), i.worldPos - float3(w[0][3], w[1][3], w[2][3])) / safeDeterminant(m);
}

float3 objectNormal(VSOut i)
{
    float3x3 m = (float3x3)instanceWorld(i.instance);
    return mul(transpose(m), i.normal) * sign(safeDeterminant(m));
}

[shader("vertex")]
VSOut meshVS(VSIn v, uint instanceId: SV_InstanceID)
{
    VSOut o;
    o.instance = pc.firstInstance + instanceId;
    float4x4 world = instanceWorld(o.instance);
    float4 wp = mul(world, float4(v.pos, 1.0));
    o.worldPos = wp.xyz;
    o.pos = mul(pc.viewProj, wp);
    // Inverse transpose would be exact under non-uniform scale; cofactor matrix gives it without an inverse.
    float3x3 m = (float3x3)world;
    float3x3 cof = cofactor(m);
    o.normal = mul(cof, v.normal);
    // A mirroring world matrix flips the bitangent.
    o.tangent = kLean ? float4(1.0, 0.0, 0.0, 1.0) : float4(mul(m, v.tangent.xyz), determinant(m) < 0.0 ? -v.tangent.w : v.tangent.w);
    // Lila UVs are V-up (Maya, FBX) and image rows run top down; Unreal's FBX import flips V the same way.
    float2 uv = !kLean && (pc.flags & kUv1) != 0 ? v.uv1 : v.uv;
    o.uv = float2(uv.x, 1.0 - uv.y);
    o.color = kLean ? float4(1.0) : v.color;
    return o;
}

static const float PI = 3.14159265;

float D_GGX(float nh, float a)
{
    float a2 = a * a;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float V_SmithGGX(float nv, float nl, float a)
{
    float a2 = a * a;
    float gv = nl * sqrt(nv * nv * (1.0 - a2) + a2);
    float gl = nv * sqrt(nl * nl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

float3 F_Schlick(float3 f0, float vh)
{
    return f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
}

struct Surface
{
    float3 diffuse; // base color * (1 - metallic)
    float3 f0;      // 0.08 * specular for dielectrics, base color for metals
    float rough;
    float ao;   // material occlusion
    float ssao; // screen-space occlusion
};

/** The shading model being lit (stylised.slang's kSkin and so on), and the frames the layered ones need. */
static uint gModel = 0;
static float3 gStrand = float3(0, 1, 0);     // hair: along the strand
static float3 gCoatNormal = float3(0, 0, 1); // clearcoat and eye: the smooth surface under the normal map

// Charlie sheen (Estevez and Kulla) and Neubelt's visibility, for cloth.
float D_Charlie(float nh, float a)
{
    float inv = 1.0 / a;
    float sin2 = max(1.0 - nh * nh, 0.0078125);
    return (2.0 + inv) * pow(sin2, inv * 0.5) / (2.0 * PI);
}

float V_Neubelt(float nv, float nl)
{
    return saturate(1.0 / (4.0 * (nl + nv - nl * nv)));
}

/** Kajiya-Kay strand highlight around tangent t, normalised for exponent e. */
float strandHighlight(float3 t, float3 h, float e)
{
    float th = dot(t, h);
    return pow(sqrt(max(1.0 - th * th, 0.0)), e) * (e + 2.0) / (2.0 * PI);
}

float3 light(float3 n, float3 v, float3 l, float3 radiance, Surface s)
{
    float3 h = normalize(v + l);
    float nl = saturate(dot(n, l));
    float nv = max(dot(n, v), 1e-4);
    float nh = saturate(dot(n, h)), vh = saturate(dot(v, h));
    float a = s.rough * s.rough;
    float3 F = F_Schlick(s.f0, vh);
    float3 spec = D_GGX(nh, a) * V_SmithGGX(nv, nl, a) * F * nl;
    float3 diff = (1.0 - F) * s.diffuse / PI * nl;
    if (gModel == kSkin)
    {
        // Light wraps further round the form where it scatters most, and two lobes give skin's sharp highlight over a broad sheen.
        float4 p = styleVec(2);
        float3 w = p.rgb * p.w * 0.6;
        float ndl = dot(normalize(lerp(n, gCoatNormal, 0.5 * p.w)), l);
        diff = (1.0 - F) * s.diffuse / PI * saturate((ndl + w) / (1.0 + w));
        float a2 = saturate(a * 1.8);
        spec = 0.5 * (spec + D_GGX(nh, a2) * V_SmithGGX(nv, nl, a2) * F * nl);
    }
    else if (gModel == kCloth)
    {
        float4 p = styleVec(2);
        spec = p.rgb * D_Charlie(nh, max(p.w * p.w, 0.01)) * V_Neubelt(nv, nl) * nl;
        diff = s.diffuse / PI * nl;
    }
    else if (gModel == kHair)
    {
        // Two highlights shifted apart along the strand (Scheuermann): a white primary and a base-coloured secondary.
        float4 p0 = styleVec(0);
        float shift = styleVec(1).x * 0.5;
        float e = lerp(8.0, 300.0, (1.0 - s.rough) * (1.0 - s.rough));
        float3 t1 = normalize(gStrand + n * shift), t2 = normalize(gStrand - n * shift);
        float3 kk = (float3(0.08) * strandHighlight(t1, h, e) + s.diffuse * 0.5 * strandHighlight(t2, h, e * 0.5)) * nl;
        spec = lerp(spec, kk, saturate(p0.z));
        diff = s.diffuse / PI * saturate(0.75 * dot(n, l) + 0.25);
    }
    if (gModel == kClearcoat || gModel == kEye)
    {
        float2 coat = gModel == kEye ? styleVec(1).yz : styleVec(0).zw;
        float ac = max(coat.y * coat.y, 0.002);
        float3 hc = h;
        float nlc = saturate(dot(gCoatNormal, l));
        float Fc = F_Schlick(float3(0.04), vh).x * saturate(coat.x);
        // Kelemen's visibility, cheap and fine for a thin smooth layer.
        float coatSpec = D_GGX(saturate(dot(gCoatNormal, hc)), ac) * 0.25 / max(vh * vh, 1e-4) * Fc * nlc;
        diff *= 1.0 - Fc;
        spec = spec * (1.0 - Fc) + coatSpec;
    }
    return (diff + spec) * radiance;
}

#include "scene_color.slang"

// Maya-style viewport gamma on the tonemapped linear color; 1 leaves it alone.
float3 displayGamma(float3 c)
{
    return pow(max(c, 0.0), 1.0 / max(pc.gamma, 0.01));
}

/** World direction to environment space: the environment's own up is +Y, rotated around it by the pref. */
float3 toEnv(float3 d)
{
    if (pc.params.y > 0.5)
        d = float3(d.x, d.z, -d.y);
    float c = pc.env.x, s = pc.env.y;
    return float3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

/** The environment's light at the pref's saturation: luminance alone at 0, the HDRI's own colour at 1. */
float3 envLight(float3 c)
{
    return lerp(dot(c, float3(0.2126, 0.7152, 0.0722)).xxx, c, pc.extra.z);
}

float3 envSample(float3 d, float lod)
{
    return envLight(envMap.SampleLevel(toEnv(d), lod).rgb);
}

float3 irradiance(float3 n)
{
    float3 e = envU.sh[0].rgb * 0.282095;
    e += envU.sh[1].rgb * (0.488603 * n.y) + envU.sh[2].rgb * (0.488603 * n.z) + envU.sh[3].rgb * (0.488603 * n.x);
    e += envU.sh[4].rgb * (1.092548 * n.x * n.y) + envU.sh[5].rgb * (1.092548 * n.y * n.z);
    e += envU.sh[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0)) + envU.sh[7].rgb * (1.092548 * n.x * n.z);
    e += envU.sh[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return envLight(max(e, 0.0));
}

// Jimenez et al. 2016: bright surfaces bounce light back into their own creases, so occlusion darkens them less.
float3 multiBounce(float visibility, float3 albedo)
{
    float3 a = 2.0404 * albedo - 0.3324;
    float3 b = -4.7951 * albedo + 0.6417;
    float3 c = 2.7552 * albedo + 0.6903;
    return max(visibility, ((visibility * a + b) * visibility + c) * visibility);
}

// Lagarde and de Rousiers 2014.
float specularOcclusion(float nv, float visibility, float rough)
{
    return saturate(pow(nv + visibility, exp2(-16.0 * rough - 1.0)) - 1.0 + visibility);
}

/**
 * The prefiltered environment's level for a reflection. Each level is a rougher lobe, so where the reflection turns
 * through more than its lobe within one pixel (small or tightly curved glossy surfaces, which otherwise sparkle as the
 * view moves), it reads the level whose lobe (GGX alpha, in radians) covers that turn.
 */
float reflectionLod(float rough)
{
    return clamp(max(rough, sqrt(gReflectionSweep)) * pc.env.w, 0.0, pc.env.w);
}

float3 imageBased(float3 n, float3 v, Surface s)
{
    float nv = max(dot(n, v), 1e-4);
    float2 ab = brdfLut.SampleLevel(float2(nv, s.rough), 0).rg;
    float3 specular = s.f0 * ab.x + ab.y;
    // Multiple-scattering compensation (Fdez-Aguera): rough surfaces would otherwise lose energy.
    float3 energy = 1.0 + s.f0 * (1.0 / max(ab.x + ab.y, 1e-4) - 1.0);
    float3 rn = n;
    if (gModel == kHair)
    {
        // Reflections stretch across the strand: reflect around a normal bent toward the view across it.
        float3 across = cross(gStrand, v);
        rn = normalize(lerp(n, normalize(cross(across, gStrand) + 1e-6), saturate(styleVec(0).z) * 0.8));
    }
    float3 prefiltered = envSample(reflect(-v, rn), reflectionLod(s.rough));
    float3 irr = irradiance(toEnv(n));
    if (gModel == kSkin)
    {
        // The scattered colour sees the light around the smooth surface rather than the bumps.
        float4 p = styleVec(2);
        irr = lerp(irr, irradiance(toEnv(normalize(lerp(n, gCoatNormal, 0.7)))), p.rgb * p.w);
    }
    float3 diffuse = irr * s.diffuse * (1.0 - specular * energy) * multiBounce(s.ssao, s.diffuse);
    float3 reflected = prefiltered * specular * energy * specularOcclusion(nv, s.ssao, s.rough);
    if (gModel == kCloth)
    {
        float4 p = styleVec(2);
        float3 sheen = envSample(reflect(-v, n), clamp(p.w * pc.env.w, 0.0, pc.env.w));
        reflected = sheen * p.rgb * (0.25 + 0.75 * pow(1.0 - nv, 3.0)) * s.ssao;
    }
    if (gModel == kClearcoat || gModel == kEye)
    {
        float2 coat = gModel == kEye ? styleVec(1).yz : styleVec(0).zw;
        float nvc = max(dot(gCoatNormal, v), 1e-4);
        float Fc = F_Schlick(float3(0.04), nvc).x * saturate(coat.x);
        float3 coatRefl = envSample(reflect(-v, gCoatNormal), clamp(max(coat.y, sqrt(gReflectionSweep)) * pc.env.w, 0.0, pc.env.w));
        diffuse *= 1.0 - Fc;
        reflected = reflected * (1.0 - Fc) + coatRefl * Fc * specularOcclusion(nvc, s.ssao, coat.y);
    }
    // Material AO darkens indirect light only, as in Unreal; image-based light is all indirect.
    return (diffuse + reflected) * s.ao * pc.env.z;
}

/** Rotated per pixel and averaged over frames by the temporal antialiasing into a smooth penumbra. */
static const float2 kShadowTaps[8] = { float2(-0.613, 0.617), float2(0.170, -0.040), float2(-0.299, -0.792), float2(0.645, 0.493),
                                       float2(-0.651, -0.088), float2(0.421, -0.616), float2(0.035, 0.911), float2(0.939, -0.105) };

/** How much of light l reaches P: 1 without a shadow map, else a filtered compare against its atlas tile. */
/** Distance from the light to a depth in its shadow map (reversed-Z): linear for the sun, perspective for a spot. */
float shadowDistance(GpuLight l, float d, bool sun)
{
    const float n = l.shadowDepth.x, f = l.shadowDepth.y;
    return sun ? n + (1.0 - d) * (f - n) : n * f / (d * (f - n) + n);
}

/**
 * The PCF radius in atlas texels, from the average blocker's distance: the penumbra widens with the gap between occluder
 * and receiver and the light's size. Negative when nothing in reach occludes.
 */
float shadowFilterRadius(GpuLight l, float2 at, float2 lo, float2 hi, float2 texel, float receiver, float cs, float sn)
{
    const bool sun = (uint(l.position.w + 0.5) & 15) == 2;
    const float zR = shadowDistance(l, receiver, sun);
    // A penumbra in cm at the receiver to atlas texels.
    const float toTexels = l.shadowDepth.w * (sun ? 1.0 : 1.0 / zR) * l.shadowRect.z / texel.x;
    const float searchCm = sun ? l.shadowDepth.z * (zR - l.shadowDepth.x) : l.shadowDepth.z * (zR - l.shadowDepth.x) / l.shadowDepth.x;
    const float search = clamp(searchCm * toTexels, 2.0, 24.0);
    float blockers = 0.0, sum = 0.0;
    for (int k = 0; k < 8; ++k)
    {
        float2 o = float2(cs * kShadowTaps[k].x - sn * kShadowTaps[k].y, sn * kShadowTaps[k].x + cs * kShadowTaps[k].y) * search * texel;
        float d = shadowAtlas.SampleLevel(clamp(at + o, lo, hi), 0).r;
        if (d > receiver + 1e-5)
        {
            sum += d;
            blockers += 1.0;
        }
    }
    if (blockers == 0.0)
        return -1.0;
    const float zB = shadowDistance(l, sum / blockers, sun);
    const float penumbraCm = sun ? l.shadowDepth.z * (zR - zB) : l.shadowDepth.z * (zR - zB) / max(zB, 1e-3);
    return clamp(penumbraCm * toTexels, 1.0, 24.0);
}

float shadowFactor(GpuLight l, uint index, float3 P, float3 ng)
{
    const float4 traced = ((GpuFrame*)pc.lights)->shadowing;
    float rt = 1.0;
    for (uint c = 0; c < 4; ++c)
        if (uint(traced[c] + 0.5) == index + 1)
            rt = rtShadowTex.Load(int3(int2(gVtPixel), 0))[c];
    // A traced light keeps a shadow map only for the heightfields, which the rays don't see.
    if (l.shadowRect.z <= 0.0 || rt <= 0.0)
        return rt;
    // A texel and a half along the surface normal keeps acne off without lifting shadows from contacts.
    float4 c = mul(l.shadow, float4(P + ng * l.right.w * 1.5, 1.0));
    float3 ndc = c.xyz / c.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z <= 0.0 || ndc.z > 1.0)
        return rt;
    uint w, h;
    shadowAtlas.GetDimensions(w, h);
    const float2 texel = 1.0 / float2(w, h);
    const float2 lo = l.shadowRect.xy + texel, hi = l.shadowRect.xy + l.shadowRect.zw - texel;
    const float2 at = l.shadowRect.xy + uv * l.shadowRect.zw;
    float angle = frac(52.9829189 * frac(dot(float2(gVtPixel), float2(0.06711056, 0.00583715)))) * 6.2831853;
    float cs = cos(angle), sn = sin(angle);
    float radius = shadowFilterRadius(l, at, lo, hi, texel, ndc.z, cs, sn);
    if (radius < 0.0)
        return rt;
    float lit = 0.0;
    for (int k = 0; k < 8; ++k)
    {
        float2 o = float2(cs * kShadowTaps[k].x - sn * kShadowTaps[k].y, sn * kShadowTaps[k].x + cs * kShadowTaps[k].y) * radius * texel;
        float d = shadowAtlas.SampleLevel(clamp(at + o, lo, hi), 0).r;
        // Reversed-Z: the occluder nearest the light holds the largest depth.
        lit += ndc.z + 1e-5 >= d ? 1.0 : 0.0;
    }
    return rt * lit / 8.0;
}

/**
 * Exponential height fog between the camera and P: the light it lets through, plus its own glow and the sun's, scattered
 * forward with a Henyey-Greenstein lobe.
 */
float3 applyFog(float3 c, float3 eye, float3 P)
{
    if (pc.lights == nullptr)
        return c;
    GpuFrame fr = *(GpuFrame*)pc.lights;
    const float density = fr.fogColor.w;
    if (density <= 0.0)
        return c;
    const uint upAxis = fr.fogExtra.y > 1.5 ? 2 : 1;
    float3 ray = P - eye;
    float dist = length(ray);
    float3 dir = ray / max(dist, 1e-4);
    dist = max(dist - fr.fogParams.z, 0.0);
    const float falloff = max(fr.fogParams.y, 1e-7);
    const float h0 = (upAxis == 2 ? eye.z : eye.y) - fr.fogParams.x;
    const float dh = (upAxis == 2 ? dir.z : dir.y) * dist;
    // The density integrated along the ray: density * exp(-falloff * h) has a closed form over a straight line.
    float fall = falloff * dh;
    float line = abs(fall) > 1e-4 ? (1.0 - exp(-fall)) / fall : 1.0 - 0.5 * fall;
    float depth = density * exp(-falloff * h0) * line * dist;
    float transmit = exp(-min(depth, 80.0));
    float3 glow = fr.fogColor.rgb;
    for (uint k = 0; k < pc.lightCount; ++k)
    {
        GpuLight l = pc.lights[k + 1];
        if ((uint(l.position.w + 0.5) & 15) != 2)
            continue;
        const float g = fr.fogExtra.x;
        float mu = dot(dir, -l.direction.xyz);
        float phase = (1.0 - g * g) / (4.0 * PI * pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5));
        glow += l.radiance.rgb * phase * fr.fogParams.w * 4.0 * PI;
        break;
    }
    return c * transmit + glow * (1.0 - transmit);
}

/** The scene's lights on the surface: punctual, spot cones, area lights from their nearest point, with shadows. */
float3 directLights(float3 n, float3 v, Surface s, float3 P, float3 ng)
{
    float3 sum = float3(0, 0, 0);
    for (uint k = 0; k < pc.lightCount; ++k)
    {
        GpuLight l = pc.lights[k + 1];
        const uint packed = uint(l.position.w + 0.5);
        const uint kind = packed & 15;
        if (pc.lightPad != 0 && ((packed >> 4) & pc.lightPad) == 0)
            continue;
        float3 L;
        float atten = 1.0;
        float dist = 1e6;
        if (kind == 2)
            L = -l.direction.xyz;
        else
        {
            float3 source = l.position.xyz;
            if (kind >= 3)
            {
                // Area and disc lights: the point of the shape nearest the surface, and only on their lit side.
                float3 right = l.right.xyz, up = cross(-l.direction.xyz, right);
                float3 rel = P - source;
                float2 q = float2(dot(rel, right), dot(rel, up));
                if (kind == 4)
                    q *= min(1.0, 0.5 * l.cone.z / max(length(q), 1e-4));
                else
                    q = clamp(q, -0.5 * l.cone.zw, 0.5 * l.cone.zw);
                source += right * q.x + up * q.y;
            }
            float3 toLight = source - P;
            float d2 = max(dot(toLight, toLight), 1e-4);
            dist = sqrt(d2);
            L = toLight / dist;
            // Inverse square in metres, so an intensity of 1 lights like the environment at a metre.
            atten = 1.0 / max(d2 * 1e-4, 1e-4);
            if (l.direction.w > 0.0)
            {
                float r = dist / l.direction.w;
                float window = saturate(1.0 - r * r * r * r);
                atten *= window * window;
            }
            if (kind == 1)
            {
                float t = saturate((dot(-L, l.direction.xyz) - l.cone.x) * l.cone.y);
                atten *= t * t;
            }
            else if (kind >= 3)
                atten *= saturate(dot(-L, l.direction.xyz));
        }
        if (atten <= 0.0)
            continue;
        float shadow = shadowFactor(l, k, P, ng);
        if (shadow <= 0.0)
            continue;
        Surface sl = s;
        if (l.radiance.w > 0.0 && kind != 2)
        {
            // A light with a size spreads its highlight (Karis): roughness widened by its angular radius.
            float a = s.rough * s.rough;
            sl.rough = sqrt(saturate(a + l.radiance.w / (2.0 * dist)));
        }
        sum += light(n, v, L, l.radiance.rgb * atten * shadow, sl);
    }
    return sum;
}

bool flatLighting() { return pc.params.w > 1.5 && pc.params.w < 2.5; }
bool environmentLighting() { return pc.params.w > 0.5 && pc.params.w < 1.5; }

/**
 * Camera-relative rigs, so every side you look at is readable, turned around the view axis by pc.extra.w. The studio
 * rig keys above-left with a fill opposite and a rim behind; the sculpt rig keys pc.keyTilt off the view over little
 * fill or ambient, so small forms cast readable shading.
 */
float3 studioRig(float3 n, float3 v, Surface s, bool sculpt)
{
    float3 up = pc.params.y > 0.5 ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 fwd = -v;
    float3 right0 = normalize(cross(fwd, up) + 1e-5);
    float3 up0 = cross(right0, fwd);
    float ca = cos(pc.extra.w), sa = sin(pc.extra.w);
    float3 right = right0 * ca - up0 * sa;
    float3 camUp = right0 * sa + up0 * ca;
    float3 c;
    float2 ambient;
    if (sculpt)
    {
        float3 side = normalize(camUp * 0.8 - right * 0.6);
        float3 key = normalize(-fwd * cos(pc.keyTilt) + side * sin(pc.keyTilt));
        float3 fill = normalize(-fwd * 0.8 - side * 0.6);
        float3 rim = normalize(fwd * 0.9 + camUp * 0.3 + right * 0.3);
        c = light(n, v, key, float3(3.0), s);
        c += light(n, v, fill, float3(0.35), s);
        c += light(n, v, rim, float3(0.5), s);
        ambient = float2(0.05, 0.12);
    }
    else
    {
        float3 key = normalize(-fwd * 0.6 + camUp * 0.7 - right * 0.5);
        float3 fill = normalize(-fwd * 0.8 - camUp * 0.1 + right * 0.6);
        float3 rim = normalize(fwd * 0.9 + camUp * 0.4);
        c = light(n, v, key, float3(2.6), s);
        c += light(n, v, fill, float3(0.9), s);
        c += light(n, v, rim, float3(1.2), s);
        ambient = float2(0.10, 0.28);
    }
    // Sky/ground hemisphere ambient, neutral gray; metals get theirs through f0.
    float hemi = dot(n, up) * 0.5 + 0.5;
    c += (s.diffuse * multiBounce(s.ssao, s.diffuse) + s.f0 * 0.5 * s.ssao) * lerp(ambient.x, ambient.y, hemi) * s.ao;
    return c;
}

/**
 * A matcap: the base colour map is a lit sphere, sampled where the normal points in a camera frame built per pixel, so
 * shading holds steady toward the edges of a wide view. Tinted by the base colour (white, or polypaint).
 */
float3 matcapShade(float3 n, float3 v, float3 tint, uint flags)
{
    if ((flags & kBaseColorMap) == 0)
        return tint;
    float3 up = pc.params.y > 0.5 ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 fwd = -v;
    float3 right = normalize(cross(fwd, up) + 1e-5);
    float3 camUp = cross(right, fwd);
    float2 at = float2(dot(n, right), -dot(n, camUp)) * 0.49 + 0.5;
    return sampleMap(kBaseColorSlot, at).rgb * tint;
}

/** The vertex color as shown: one channel isolates as grayscale, display-encoded so a 0.5 mask reads as mid gray. */
float3 vertexColor(float4 c, uint flags)
{
    uint channel = (flags >> kColorChannelShift) & 7;
    if (channel == 0)
        return max(c.rgb, 0.0);
    float v = saturate(channel == 1 ? c.r : channel == 2 ? c.g : channel == 3 ? c.b : c.a);
    return (v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4)).xxx;
}

#include "stylised.slang"

/** The back-face tint over a display-encoded colour. */
float3 tintBack(float3 c, bool front)
{
    if (front || pc.backTint == 0)
        return c;
    float4 t = float4((uint4(pc.backTint, pc.backTint >> 8, pc.backTint >> 16, pc.backTint >> 24) & 0xff)) / 255.0;
    return lerp(c, t.rgb, t.a);
}

/**
 * The normal and uv at the pixel centre, for screen derivatives: centroid values jump along every triangle edge. The
 * shading reads them at the centroid, evaluated per pixel rather than interpolated twice per vertex.
 */
struct Centers
{
    float3 normal;
    float2 uv;
};

float4 shadeMesh(VSOut i, bool front, Centers center)
{
    gVtPixel = uint2(i.pos.xy);
    gUvDx = ddx(center.uv);
    gUvDy = ddy(center.uv);
    uint flags = pc.flags;
    if (kLean)
    {
        i.tangent = float4(1.0, 0.0, 0.0, 1.0);
        i.color = float4(1.0);
    }
    if ((flags & kVertexColorUnlit) != 0)
        return float4(fromDisplay(tintBack(vertexColor(i.color, flags), front), pc.gamma), 1.0);
    const uint model = styleModel();
    gModel = model;
    float2 uv = i.uv;
    float3 ng0 = normalize(i.normal);
    float3 tangentU = normalize(i.tangent.xyz - ng0 * dot(ng0, i.tangent.xyz) + 1e-7);
    float3 tangentV = cross(ng0, tangentU) * (i.tangent.w < 0.0 ? -1.0 : 1.0);
    float eyeRing = 0.0;
    if (model == kEye)
    {
        // The iris sits under the cornea: it shifts against the view by its depth, and darkens at its rim.
        float4 p0 = styleVec(0);
        float r = length(uv - 0.5);
        float inside = 1.0 - smoothstep(p0.z * 0.95, p0.z * 1.05, r);
        float3 view = normalize(pc.eye.xyz - i.worldPos);
        uv -= float2(dot(view, tangentU), -dot(view, tangentV)) * p0.w * 0.1 * inside;
        eyeRing = styleVec(1).x * smoothstep(p0.z * 0.6, p0.z, r) * inside;
    }
    else if (model == kHair)
    {
        float rot = styleVec(0).w;
        gStrand = normalize(tangentV * cos(rot) + tangentU * sin(rot));
    }
    float3 baseColor = pc.base.rgb;
    float opacity = pc.surface.z;
    // A matcap's base colour map is the lit sphere, looked up by the normal in matcapShade.
    if ((flags & kBaseColorMap) != 0 && model != kMatcap)
    {
        float4 t = sampleMap(kBaseColorSlot, uv);
        if ((flags & kGreyBase) == 0)
            baseColor = t.rgb;
        opacity *= t.a;
    }
    baseColor *= 1.0 - eyeRing;
    if ((flags & kMasked) != 0 && opacity < pc.surface.w)
        discard;
    if ((flags & kVertexColorShaded) != 0)
        baseColor *= vertexColor(i.color, flags);
#ifdef LILA_HEIGHTFIELD
    baseColor = lerp(baseColor, gTerrainBase.rgb, gTerrainBase.a);
#endif

    float3 ng = normalize(i.normal);
    {
        // From the interpolated normal: it changes smoothly, so neighbouring quads pick neighbouring levels.
        gCenterNormal = normalize(center.normal);
        float3 r = reflect(normalize(i.worldPos - pc.eye.xyz), gCenterNormal);
        float3 dx = ddx(r), dy = ddy(r);
        gReflectionSweep = sqrt(max(dot(dx, dx), dot(dy, dy)));
    }
    float3 n = ng;
    if ((flags & kNormalMap) != 0)
    {
        float3 tn = sampleMap(kNormalSlot, uv).xyz * 2.0 - 1.0;
        if ((flags & kNormalXY) != 0)
            tn.z = sqrt(saturate(1.0 - dot(tn.xy, tn.xy)));
        // Toksvig: filtered mips average the normals, and how short the average is says how bumpy the texels under it are.
        float len = clamp(length(tn), 1e-3, 1.0);
        gNormalVariance = clamp((1.0 - len) / len - 0.02, 0.0, 0.5);
        // The bitangent points up the texture (V-up); DirectX maps store green pointing down it.
        if ((flags & kNormalGreenUp) == 0)
            tn.y = -tn.y;
        float3 t = normalize(i.tangent.xyz - ng * dot(ng, i.tangent.xyz) + 1e-7);
        float3 b = cross(ng, t) * (i.tangent.w < 0.0 ? -1.0 : 1.0);
        n = normalize(t * tn.x + b * tn.y + ng * max(tn.z, 1e-3));
    }
#ifdef LILA_HEIGHTFIELD
    if (gTerrainNormal.w > 0.0)
        n = normalize(lerp(n, gTerrainNormal.xyz, gTerrainNormal.w));
#endif
    Stroke stroke = { float3(0, 0, 1), 0.5, 0.5 };
    if (model == kPainterly && styleVec(0).z * styleVec(0).w > 0.0)
    {
        stroke = painterlyStroke(i, ng, front);
        n = bendNormal(n, ng, stroke);
    }
    if (!front && (flags & kOneSidedLighting) == 0)
        n = -n;
    gCoatNormal = front || (flags & kOneSidedLighting) != 0 ? ng : -ng;
    if (pc.extra.x > 0.0 && pc.extra.y > 0.5)
    {
        // The live bevel: the rounded normal replaces the surface's own under any normal map detail.
        float4 bevel = bevelTex.Load(int3(int2(i.pos.xy), 0));
        if (bevel.w > 0.5)
            n = normalize(n - gCoatNormal + bevel.xyz);
    }

    Surface s;
    float metallic = pc.surface.x;
    s.rough = pc.base.a;
    s.ao = 1.0;
    s.ssao = 1.0;
    if ((flags & kTranslucent) == 0)
    {
        s.ssao = sampleAo(int2(i.pos.xy), i.pos.z, front ? ng : -ng);
    }
    if ((flags & (kMetallicMap | kRoughnessMap | kOcclusionMap)) != 0)
    {
        float3 orm = sampleMap(kOrmSlot, uv).rgb;
        if ((flags & kOcclusionMap) != 0)
            s.ao = orm.r;
        if ((flags & kRoughnessMap) != 0)
            s.rough = orm.g;
        if ((flags & kMetallicMap) != 0)
            metallic = orm.b;
    }
#ifdef LILA_HEIGHTFIELD
    s.rough = lerp(s.rough, gTerrainRough, gTerrainBase.a);
#endif
    s.rough = specularAntialias(n, clamp(s.rough, 0.04, 1.0));
    metallic = saturate(metallic);
    s.diffuse = baseColor * (1.0 - metallic);
    s.f0 = lerp(float3(0.08 * pc.surface.y), baseColor, metallic);

    float3 v = normalize(pc.eye.xyz - i.worldPos);
    float3 c;
    if (flatLighting() || model == kUnlit)
        c = baseColor;
    else if (model == kMatcap)
        c = matcapShade(n, v, baseColor, flags) * s.ao * multiBounce(s.ssao, baseColor);
    else if (model == kToon)
        c = toonShade(n, v, baseColor, s);
    else
    {
        if (model == kPainterly)
            painterlySurface(stroke, n, v, baseColor, metallic, s);
        else if (model == kFoliage)
            foliageSurface(i, n, baseColor, metallic, s);
        c = environmentLighting() ? imageBased(n, v, s) : studioRig(n, v, s, pc.params.w > 2.5);
        c += directLights(n, v, s, i.worldPos, front ? ng : -ng);
        if (model == kFoliage)
            c = foliageLight(c, n, v, s);
    }
    float3 emissive = pc.emissive;
    if ((flags & kEmissiveMap) != 0)
        emissive *= sampleMap(kEmissiveSlot, i.uv).rgb;
    c += emissive;
    if (model != kUnlit && model != kMatcap && !flatLighting())
        c = applyFog(c, pc.eye.xyz, i.worldPos);
    c *= pc.params.x;
    float alpha = (flags & kTranslucent) != 0 ? saturate(opacity) : 1.0;
    // Highlights and the back-face tint mix in display space.
    if (pc.params.z > 0.0 || (!front && pc.backTint != 0))
    {
        float3 d = displayGamma(neutralTonemap(c));
        d = tintBack(lerp(d, float3(1.0), pc.params.z), front);
        return float4(fromDisplay(d, pc.gamma), alpha);
    }
    return float4(sceneCompress(c), alpha);
}

[shader("fragment")]
float4 meshPS(VSOut i, bool front : SV_IsFrontFace) : SV_Target
{
    const Centers center = {i.normal, i.uv};
    i.normal = EvaluateAttributeAtCentroid(i.normal);
    i.uv = EvaluateAttributeAtCentroid(i.uv);
    return shadeMesh(i, front, center);
}

#ifdef LILA_WIRE
/**
 * meshPS with the wire drawn over it from each pixel's distance to the triangle's polygon sides, so a dense mesh's
 * wireframe costs no line geometry. Each side's two triangles draw half the width each.
 */
[shader("fragment")]
float4 meshWirePS(VSOut i, bool front : SV_IsFrontFace, float3 bary : SV_Barycentrics, uint prim : SV_PrimitiveID) : SV_Target
{
    const Centers center = {i.normal, i.uv};
    i.normal = EvaluateAttributeAtCentroid(i.normal);
    i.uv = EvaluateAttributeAtCentroid(i.uv);
    float4 c = shadeMesh(i, front, center);
    const float4 wire = pc.instances[i.instance].color;
    if (wire.a <= 0.0)
        return c;
    const uint tri = pc.firstTriangle + prim;
    const uint sides = (pc.edgeMask[tri >> 3] >> ((tri & 7) * 4)) & 7;
    // Barycentric k is zero along the side opposite vertex k.
    const float3 px = bary / max(fwidth(bary), 1e-6);
    float d = 1e9;
    if ((sides & 1) != 0)
        d = min(d, px.z);
    if ((sides & 2) != 0)
        d = min(d, px.x);
    if ((sides & 4) != 0)
        d = min(d, px.y);
    const float cover = saturate(0.5 * pc.wireWidth + 0.5 - d);
    c.rgb = lerp(c.rgb, fromDisplay(wire.rgb, pc.gamma), cover * wire.a);
    // The wire stays solid over a translucent surface, down to a fully clear one.
    c.a = lerp(c.a, 1.0, cover * wire.a);
    return c;
}
#endif

// Single-sample depth and world normal for the ambient occlusion pass. Masked materials cut out as in meshPS.
[shader("fragment")]
float4 prepassPS(VSOut i, bool front : SV_IsFrontFace) : SV_Target
{
    gVtPixel = uint2(i.pos.xy);
    gUvDx = ddx(i.uv);
    gUvDy = ddy(i.uv);
    i.normal = EvaluateAttributeAtCentroid(i.normal);
    i.uv = EvaluateAttributeAtCentroid(i.uv);
    if ((pc.flags & kMasked) != 0)
    {
        float opacity = pc.surface.z;
        if ((pc.flags & kBaseColorMap) != 0)
            opacity *= sampleMap(kBaseColorSlot, i.uv).a;
        if (opacity < pc.surface.w)
            discard;
    }
    float3 n = normalize(i.normal);
    // The live bevel's radius rides along for the bevel pass.
    return float4(front ? n : -n, pc.extra.x);
}

struct BgOut
{
    float4 pos : SV_Position;
    float2 ndc;
};

[shader("vertex")]
BgOut backgroundVS(uint vid: SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    BgOut o;
    o.ndc = uv * 2.0 - 1.0;
    o.pos = float4(o.ndc, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 backgroundPS(BgOut i) : SV_Target
{
    float4 nearH = mul(pc.viewProj, float4(i.ndc, 1.0, 1.0));
    float4 farH = mul(pc.viewProj, float4(i.ndc, 0.001, 1.0));
    float3 rd = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);

    float3 c = envMap.SampleLevel(toEnv(rd), pc.base.x).rgb * pc.env.z * pc.base.z;
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(luma.xxx, c, pc.base.y);
    c = applyFog(c, pc.eye.xyz, pc.eye.xyz + rd * 1e6);
    return float4(sceneCompress(c * pc.params.x), 1.0);
}
