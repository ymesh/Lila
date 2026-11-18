// Displacement preview: each vertex of a tessellated mesh section moves from its undisplaced position along its smooth
// direction by (height - mid) * depth, the height read bilinearly (and repeating) from texels copied out of the height
// image. render/Displacement.cpp's sampleHeight is the CPU reference.

struct DisplacePush
{
    uint64_t vertices; // MeshVertex (64 bytes), positions written
    uint64_t base;     // MeshVertex, undisplaced
    uint64_t dirs;     // float4 per vertex
    uint64_t height;   // texels, tightly packed
    uint firstVertex;
    uint count;
    uint width;
    uint heightPx;
    uint format; // render::TextureFormat: 0 R8, 1 RG8, 2 RGBA8, 3 R16F, 4 RGBA16F, 5 R32F
    uint uv1;
    int tileU; // -1: every vertex
    int tileV;
    float depth;
    float mid;
    float2 pad;
};

[[vk::push_constant]] DisplacePush dp;

static const uint kTexelBytes[6] = { 1, 2, 4, 2, 8, 4 };

uint byteAt(uint offset)
{
    uint w = ((uint*)dp.height)[offset >> 2];
    return (w >> ((offset & 3) * 8)) & 0xff;
}

float texel(int x, int y)
{
    int w = int(dp.width), h = int(dp.heightPx);
    // Wrapped with non-negative operands only: the remainder of a negative int isn't the same on every compiler.
    if (x < 0)
        x += w * ((w - 1 - x) / w);
    if (y < 0)
        y += h * ((h - 1 - y) / h);
    x = int(uint(x) % uint(w));
    y = int(uint(y) % uint(h));
    uint offset = (uint(y) * dp.width + uint(x)) * kTexelBytes[min(dp.format, 5u)];
    if (dp.format <= 2)
        return float(byteAt(offset)) / 255.0;
    if (dp.format <= 4)
    {
        uint w32 = ((uint*)dp.height)[offset >> 2];
        return f16tof32((offset & 2) != 0 ? (w32 >> 16) : (w32 & 0xffff));
    }
    return asfloat(((uint*)dp.height)[offset >> 2]);
}

float sampleHeight(float2 uv)
{
    float fu = uv.x - floor(uv.x), fv = uv.y - floor(uv.y);
    float x = fu * float(dp.width) - 0.5, y = (1.0 - fv) * float(dp.heightPx) - 0.5;
    float x0 = floor(x), y0 = floor(y);
    float tx = x - x0, ty = y - y0;
    int ix = int(x0), iy = int(y0);
    float a = texel(ix, iy), b = texel(ix + 1, iy), c = texel(ix, iy + 1), d = texel(ix + 1, iy + 1);
    return (a * (1.0 - tx) + b * tx) * (1.0 - ty) + (c * (1.0 - tx) + d * tx) * ty;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void displaceCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= dp.count)
        return;
    uint v = dp.firstVertex + id.x;
    float* dst = (float*)(dp.vertices + uint64_t(v) * 64);
    float* src = (float*)(dp.base + uint64_t(v) * 64);
    float2 uv = dp.uv1 != 0 ? float2(dst[8], dst[9]) : float2(dst[6], dst[7]);
    if (dp.tileU >= 0 && (int(floor(uv.x)) != dp.tileU || int(floor(uv.y)) != dp.tileV))
        return;
    float3 dir = ((float4*)dp.dirs)[v].xyz;
    float offset = (sampleHeight(uv) - dp.mid) * dp.depth;
    dst[0] = src[0] + dir.x * offset;
    dst[1] = src[1] + dir.y * offset;
    dst[2] = src[2] + dir.z * offset;
}
