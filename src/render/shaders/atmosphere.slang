// Aerial perspective: the sky's haze between the camera and each froxel (render::kAerialSize screen cells by
// render::kAerialSlices depth slices, spaced by the square of the slice), for mesh.slang's applyFog to read.

#include "atmosphere_common.slang"

struct AerialPush
{
    float4x4 invViewProj; // clip to world, reversed-Z
    GpuAtmosphere* atmosphere;
    float4* froxels; // two per froxel: scattered light (shader units), then transmittance
    float4 params;   // x depth of the last slice (km), y cells across, z slices
};

[[vk::push_constant]] AerialPush pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void aerialCS(uint3 id: SV_DispatchThreadID)
{
    uint cells = uint(pc.params.y), slices = uint(pc.params.z);
    if (id.x >= cells || id.y >= cells)
        return;
    float2 ndc = (float2(id.xy) + 0.5) / cells * 2.0 - 1.0;
    float4 nearH = mul(pc.invViewProj, float4(ndc, 1.0, 1.0));
    float4 farH = mul(pc.invViewProj, float4(ndc, 0.001, 1.0));
    float3 dir = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);
    GpuAtmosphere* a = pc.atmosphere;
    float3 sun = a->sunDir.xyz;
    float start = a->camera.w;
    for (uint z = 0; z < slices; ++z)
    {
        float f = (z + 1.0) / slices;
        float dist = pc.params.x * f * f;
        float3 T;
        // The distance scale stretches how far the haze reaches, as Unreal's aerial perspective view distance scale does.
        float3 L = atmIntegrate(a, a->camera.xyz, dir, sun, start, max(dist * a->skyFactor.w, start), 6 + z / 4, false, T);
        uint i = ((z * cells + id.y) * cells + id.x) * 2;
        pc.froxels[i] = float4(L * a->sunRadiance.rgb, 1.0);
        pc.froxels[i + 1] = float4(T, 1.0);
    }
}
