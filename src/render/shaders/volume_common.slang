// Volumes as render::GpuVolume lays them out, shared by the viewport's ray march and the path tracer.

/** Matches render::GpuVolume. */
struct GpuVolume
{
    float4x4 worldToIndex; // the frame's float space to voxel coordinates
    float4 dims;           // voxels per axis, w the largest extinction per cm (the majorant)
    float4 scatter;        // rgb scattering per cm at value 1, w anisotropy
    float4 absorb;         // rgb absorption per cm at value 1
    float4 emission;       // rgb radiance per cm at value 1
    float* values;
    uint64_t pad;
};

static const float kVolPi = 3.14159265358979;

/** The grid's value at a point in voxel coordinates, trilinear, 0 outside. */
float volDensity(GpuVolume* v, float3 idx)
{
    int3 dims = int3(v->dims.xyz);
    int3 b = int3(floor(idx));
    float3 f = idx - float3(b);
    float sum = 0.0;
    for (uint k = 0; k < 8; ++k)
    {
        int3 o = b + int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        if (any(o < 0) || any(o >= dims))
            continue;
        float w = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k & 2) != 0 ? f.y : 1.0 - f.y) * ((k & 4) != 0 ? f.z : 1.0 - f.z);
        sum += v->values[o.x + dims.x * (o.y + dims.y * o.z)] * w;
    }
    return sum;
}

/** Where a ray in voxel coordinates is inside the grid's box, as a range of its t. */
float2 volBoxRange(float3 o, float3 d, float3 dims)
{
    float3 inv = 1.0 / select(abs(d) > 1e-12, d, float3(1e-12, 1e-12, 1e-12));
    float3 a = (-0.5 - o) * inv, b = (dims - 0.5 - o) * inv;
    float3 lo = min(a, b), hi = max(a, b);
    return float2(max(max(lo.x, lo.y), lo.z), min(min(hi.x, hi.y), hi.z));
}

float volPhase(float g, float mu)
{
    float d = max(1.0 + g * g - 2.0 * g * mu, 1e-4);
    return (1.0 - g * g) / (4.0 * kVolPi * d * sqrt(d));
}

/** A direction from the Henyey-Greenstein lobe about dir. */
float3 volSamplePhase(float3 dir, float g, float u1, float u2)
{
    float cosT;
    if (abs(g) < 1e-3)
        cosT = 1.0 - 2.0 * u1;
    else
    {
        float s = (1.0 - g * g) / (1.0 - g + 2.0 * g * u1);
        cosT = (1.0 + g * g - s * s) / (2.0 * g);
    }
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    float phi = 2.0 * kVolPi * u2;
    float3 t = abs(dir.y) < 0.99 ? normalize(cross(float3(0, 1, 0), dir)) : normalize(cross(float3(1, 0, 0), dir));
    float3 b = cross(dir, t);
    return normalize(dir * cosT + (t * cos(phi) + b * sin(phi)) * sinT);
}

/** Mean extinction per cm at a value of 1. */
float volSigma(GpuVolume* v)
{
    return dot(v->scatter.rgb + v->absorb.rgb, float3(0.3333, 0.3334, 0.3333));
}
