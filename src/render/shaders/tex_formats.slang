// Texel formats shared by the GPU texture graph evaluator's kernels (the TextureFormat order). Arena buffers are float4
// per texel, row 0 at the top.

static const uint kR8 = 0;
static const uint kRG8 = 1;
static const uint kRGBA8 = 2;
static const uint kR16F = 3;
static const uint kRGBA16F = 4;
static const uint kR32F = 5;
// Decode sources only (no TextureFormat): float4 texels from the CPU.
static const uint kRGBA32F = 6;
static const uint kConstant = 4;

uint texelBytes(uint f)
{
    switch (f)
    {
    case kR8: return 1;
    case kRG8: return 2;
    case kRGBA8: return 4;
    case kR16F: return 2;
    case kRGBA16F: return 8;
    case kRGBA32F: return 16;
    default: return 4;
    }
}

uint channelCount(uint f)
{
    switch (f)
    {
    case kRG8: return 2;
    case kRGBA8:
    case kRGBA16F:
    case kRGBA32F: return 4;
    default: return 1;
    }
}

float toSrgb(float v)
{
    v = saturate(v);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

float fromSrgb(float v)
{
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

uint unorm8(float v)
{
    return uint(round(saturate(v) * 255.0));
}

/** Items laid out over a 2D grid of 64-wide groups, so more than 65535 groups fit. */
uint flatIndex(uint3 gid, uint3 local, uint groupsX)
{
    return (gid.y * groupsX + gid.x) * 64 + local.x;
}

