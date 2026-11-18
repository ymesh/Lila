// Wireframe over shaded meshes: same positions as the mesh, flat color per instance.

#include "scene_color.slang"
#include "draw_offset.slang"

/** One placement of a draw; matches render::GpuInstance. */
struct InstanceData
{
    float4x4 world;
    float4 color;
};

struct WirePush
{
    float4x4 viewProj;
    /** Clip z = z * x + w * y: pulls the wire toward the camera along its view ray (see depthPull in Renderer.cpp). z: display gamma. */
    float4 depthPull;
    float4 eye; // xyz camera position, w the draw's offset toward it (cm)
    InstanceData* instances; // the frame's instance buffer
    uint firstInstance;      // this draw's first entry in it
    uint instancePad;
};

[[vk::push_constant]] WirePush wpc;

struct WireIn
{
    float3 pos : POSITION;
};

struct WireOut
{
    float4 pos : SV_Position;
    nointerpolation float4 color : COLOR;
};

[shader("vertex")]
WireOut wireVS(WireIn v, uint instanceId: SV_InstanceID)
{
    InstanceData inst = wpc.instances[wpc.firstInstance + instanceId];
    WireOut o;
    o.pos = mul(wpc.viewProj, float4(towardCamera(mul(inst.world, float4(v.pos, 1.0)).xyz, wpc.viewProj, wpc.eye.xyz, wpc.eye.w), 1.0));
    o.pos.z = o.pos.z * wpc.depthPull.x + o.pos.w * wpc.depthPull.y;
    o.color = inst.color;
    return o;
}

[shader("fragment")]
float4 wirePS(WireOut i) : SV_Target
{
    return float4(fromDisplay(i.color.rgb, wpc.depthPull.z), i.color.a);
}
