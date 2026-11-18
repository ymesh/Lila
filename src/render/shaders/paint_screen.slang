// Screen and 2D views of painted texture sets: which triangle each pixel of a stroke's view sees (after paint_depth) and
// the uv there (for screen clone sources and projection checks), and a texture image resampled over a uv rectangle for
// the UV editor's background.

#include "tex_formats.slang"

static const uint kNone = 0xFFFFFFFF;

struct Tri
{
    float4 p[3];
    float4 n[3];
    float4 t[3];
};

float edge(float2 a, float2 b, float2 p)
{
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

// ------------------------------------------------------------------------------------------------- visibility

struct VisPush
{
    float4x4 objectToClip;
    Tri* tris;
    uint* depth;
    uint* vis;
    uint triCount;
    uint width;
    uint height;
    uint groupsX;
    uint pad0;
};

[[vk::push_constant]] VisPush vp;

/** Per pixel, the lowest triangle id whose depth equals the nearest depth there (ties resolve the same on every run). */
[shader("compute")]
[numthreads(64, 1, 1)]
void paintVisCS(uint3 gid: SV_GroupID, uint3 local: SV_GroupThreadID)
{
    uint t = (gid.y * vp.groupsX + gid.x) * 64 + local.x;
    if (t >= vp.triCount)
        return;
    Tri tri = vp.tris[t];
    float2 s[3];
    float z[3];
    for (int k = 0; k < 3; ++k)
    {
        float4 c = mul(vp.objectToClip, float4(tri.p[k].xyz, 1.0));
        if (c.w <= 1e-6)
            return;
        float3 n = c.xyz / c.w;
        s[k] = (n.xy * 0.5 + 0.5) * float2(float(vp.width), float(vp.height));
        z[k] = n.z;
    }
    float area = edge(s[0], s[1], s[2]);
    if (abs(area) < 1e-12)
        return;
    float2 lo = min(s[0], min(s[1], s[2]));
    float2 hi = max(s[0], max(s[1], s[2]));
    int x0 = max(int(floor(lo.x)), 0), y0 = max(int(floor(lo.y)), 0);
    int x1 = min(int(ceil(hi.x)), int(vp.width) - 1), y1 = min(int(ceil(hi.y)), int(vp.height) - 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            float2 p = float2(float(x), float(y)) + 0.5;
            float w0 = edge(s[1], s[2], p) / area;
            float w1 = edge(s[2], s[0], p) / area;
            float w2 = edge(s[0], s[1], p) / area;
            if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0)
                continue;
            float d = w0 * z[0] + w1 * z[1] + w2 * z[2];
            uint i = uint(y) * vp.width + uint(x);
            if (d > 0.0 && asuint(d) == vp.depth[i])
                InterlockedMin(vp.vis[i], t);
        }
}

// ------------------------------------------------------------------------------------------------- uv map

struct UvPush
{
    float4x4 objectToClip;
    Tri* tris;
    uint* vis;
    float4* out;
    uint width;
    uint height;
    uint groupsX;
    uint pad0;
};

[[vk::push_constant]] UvPush up;

/** Each pixel of the view: the uv of the surface it sees (perspective correct), its triangle, and 1; zero where none. */
[shader("compute")]
[numthreads(64, 1, 1)]
void paintUvCS(uint3 gid: SV_GroupID, uint3 local: SV_GroupThreadID)
{
    uint i = flatIndex(gid, local, up.groupsX);
    if (i >= up.width * up.height)
        return;
    up.out[i] = float4(0.0);
    uint t = up.vis[i];
    if (t == kNone)
        return;
    Tri tri = up.tris[t];
    float2 s[3];
    float w[3];
    for (int k = 0; k < 3; ++k)
    {
        float4 c = mul(up.objectToClip, float4(tri.p[k].xyz, 1.0));
        s[k] = (c.xy / c.w * 0.5 + 0.5) * float2(float(up.width), float(up.height));
        w[k] = c.w;
    }
    float2 px = float2(float(i % up.width), float(i / up.width)) + 0.5;
    float area = edge(s[0], s[1], s[2]);
    if (abs(area) < 1e-12)
        return;
    float3 b = float3(edge(s[1], s[2], px), edge(s[2], s[0], px), edge(s[0], s[1], px)) / area;
    b = b / float3(w[0], w[1], w[2]);
    b /= b.x + b.y + b.z;
    float2 uv = float2(tri.p[0].w, tri.n[0].w) * b.x + float2(tri.p[1].w, tri.n[1].w) * b.y + float2(tri.p[2].w, tri.n[2].w) * b.z;
    up.out[i] = float4(uv, asfloat(t), 1.0);
}

// ------------------------------------------------------------------------------------------------- 2D view

struct ViewPush
{
    uint* src;
    uint* dst;
    float4 rect; // u0, v0 (bottom left), u1, v1 of the image's uv in the view
    uint srcWidth;
    uint srcHeight;
    uint format;
    uint srgb;
    uint width;
    uint height;
    uint groupsX;
    uint display; // bit 0: linear colour shown as sRGB; bit 1: alpha kept (else opaque)
};

[[vk::push_constant]] ViewPush wp;

uint viewByte(uint b)
{
    return (wp.src[b >> 2] >> ((b & 3) * 8)) & 0xff;
}

uint viewHalf(uint b)
{
    return (wp.src[b >> 2] >> ((b & 2) * 8)) & 0xffff;
}

float4 viewTexel(uint x, uint y)
{
    uint b = (y * wp.srcWidth + x) * texelBytes(wp.format);
    float4 v = float4(0.0, 0.0, 0.0, 1.0);
    switch (wp.format)
    {
    case kR8: v.x = float(viewByte(b)) / 255.0; break;
    case kRG8:
        v.x = float(viewByte(b)) / 255.0;
        v.w = float(viewByte(b + 1)) / 255.0;
        break;
    case kRGBA8:
        for (uint c = 0; c < 4; ++c)
        {
            float f = float(viewByte(b + c)) / 255.0;
            v[c] = wp.srgb != 0 && c < 3 ? fromSrgb(f) : f;
        }
        break;
    case kR16F: v.x = f16tof32(viewHalf(b)); break;
    case kRGBA16F:
        for (uint c = 0; c < 4; ++c)
            v[c] = f16tof32(viewHalf(b + c * 2));
        break;
    default: v.x = asfloat(wp.src[b >> 2]); break;
    }
    if (channelCount(wp.format) <= 2)
        v.yz = v.xx;
    return v;
}

/** RGBA8 view pixels (row 0 at the top) of the image over the rect, repeating; transparent outside the 0-1 tile. */
[shader("compute")]
[numthreads(64, 1, 1)]
void paintViewCS(uint3 gid: SV_GroupID, uint3 local: SV_GroupThreadID)
{
    uint i = flatIndex(gid, local, wp.groupsX);
    if (i >= wp.width * wp.height)
        return;
    float2 f = (float2(float(i % wp.width), float(i / wp.width)) + 0.5) / float2(float(wp.width), float(wp.height));
    float2 uv = float2(lerp(wp.rect.x, wp.rect.z, f.x), lerp(wp.rect.w, wp.rect.y, f.y));
    float4 v = float4(0.0);
    if (all(uv >= 0.0) && all(uv < 1.0))
    {
        uint x = min(uint(uv.x * float(wp.srcWidth)), wp.srcWidth - 1);
        uint y = min(uint((1.0 - uv.y) * float(wp.srcHeight)), wp.srcHeight - 1);
        v = viewTexel(x, y);
        if ((wp.display & 2u) == 0u)
            v.a = 1.0;
        if ((wp.display & 1u) != 0u)
            v.rgb = float3(toSrgb(saturate(v.r)), toSrgb(saturate(v.g)), toSrgb(saturate(v.b)));
    }
    wp.dst[i] = unorm8(v.r) | (unorm8(v.g) << 8) | (unorm8(v.b) << 16) | (unorm8(v.a) << 24);
}
