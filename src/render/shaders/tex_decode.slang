#include "tex_formats.slang"

struct DecodePush
{
    uint* src;      // a rectangle of image texels as an image-to-buffer copy packs them, srcWidth per row
    uint2* dst;     // the destination rectangle's first texel in the arena: float4 as two pairs, or half4 as one
    uint width;     // destination rectangle
    uint height;
    uint format;
    uint srgb;
    uint groupsX;
    uint dstStride; // texels per arena row
    uint srcWidth;
    uint srcHeight;
    float2 scale;   // source texels per destination texel
    float2 origin;  // where the destination rectangle starts, in source rectangle texels
    uint dstHalf;
    uint pad;
};

[[vk::push_constant]] DecodePush dcp;

uint byteAt(uint b)
{
    return (dcp.src[b >> 2] >> ((b & 3) * 8)) & 0xff;
}

uint halfAt(uint b)
{
    return (dcp.src[b >> 2] >> ((b & 2) * 8)) & 0xffff;
}

/** Source texel i as a float4: one channel reads as gray with alpha 1, two as gray and alpha. sRGB RGBA8 decodes to linear. */
float4 texelAt(uint i)
{
    uint b = i * texelBytes(dcp.format);
    float4 v = float4(0.0, 0.0, 0.0, 1.0);
    switch (dcp.format)
    {
    case kR8: v.x = float(byteAt(b)) / 255.0; break;
    case kRG8:
        v.x = float(byteAt(b)) / 255.0;
        v.w = float(byteAt(b + 1)) / 255.0;
        break;
    case kRGBA8:
        for (uint c = 0; c < 4; ++c)
        {
            float f = float(byteAt(b + c)) / 255.0;
            v[c] = dcp.srgb != 0 && c < 3 ? fromSrgb(f) : f;
        }
        break;
    case kR16F: v.x = f16tof32(halfAt(b)); break;
    case kRGBA16F:
        for (uint c = 0; c < 4; ++c)
            v[c] = f16tof32(halfAt(b + c * 2));
        break;
    case kRGBA32F:
        for (uint c = 0; c < 4; ++c)
            v[c] = asfloat(dcp.src[(b >> 2) + c]);
        break;
    default: v.x = asfloat(dcp.src[b >> 2]); break;
    }
    if (channelCount(dcp.format) <= 2)
        v.yz = v.xx;
    return v;
}

float4 texelAt(float2 p)
{
    int2 q = clamp(int2(floor(p)), int2(0), int2(int(dcp.srcWidth), int(dcp.srcHeight)) - 1);
    return texelAt(uint(q.y) * dcp.srcWidth + uint(q.x));
}

/**
 * Image texels to arena float4s over a rectangle, the way materials sample them. A destination smaller than the image
 * averages four source points per texel.
 */
[shader("compute")]
[numthreads(64, 1, 1)]
void texDecodeCS(uint3 gid: SV_GroupID, uint3 local: SV_GroupThreadID)
{
    uint i = flatIndex(gid, local, dcp.groupsX);
    if (i >= dcp.width * dcp.height)
        return;
    uint x = i % dcp.width, y = i / dcp.width;
    float4 v;
    if (dcp.scale.x == 1.0 && dcp.scale.y == 1.0)
        v = texelAt(float2(float(x), float(y)) + dcp.origin + 0.5);
    else
    {
        v = float4(0.0);
        for (uint k = 0; k < 4; ++k)
        {
            float2 f = float2((k & 1) != 0 ? 0.75 : 0.25, (k & 2) != 0 ? 0.75 : 0.25);
            v += texelAt((float2(float(x), float(y)) + f) * dcp.scale + dcp.origin);
        }
        v *= 0.25;
    }
    uint o = y * dcp.dstStride + x;
    if (dcp.dstHalf != 0)
        dcp.dst[o] = uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16));
    else
    {
        dcp.dst[2 * o] = asuint(v.xy);
        dcp.dst[2 * o + 1] = asuint(v.zw);
    }
}
