#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace lila::render
{
/** The mesh shader's push constants (mesh.slang), shared by every pipeline that shades with it. */
struct MeshPush
{
    glm::mat4 viewProj;
    /** The device address of the frame's instance buffer (GpuInstance entries) and this draw's first entry in it. */
    uint64_t instances = 0;
    uint32_t firstInstance = 0;
    /** The sculpt rig's key light angle off the view direction, radians. */
    float keyTilt = 0.0f;
    /** meshWirePS: the device address of the mesh's edge mask (RenderMesh::edgeMask), the section's first triangle and the wire width in pixels. */
    uint64_t edgeMask = 0;
    uint32_t firstTriangle = 0;
    float wireWidth = 1.0f;
    /** The frame's lights (GpuLight entries) and how many. */
    uint64_t lights = 0;
    uint32_t lightCount = 0;
    uint32_t lightPad = 0;
    glm::vec4 extra{}; // live bevel radius, 1 when the bevel pass ran, environment light saturation
    glm::vec4 eye;
    glm::vec4 base;
    glm::vec4 params;
    glm::vec4 env;
    glm::vec4 surface;
    glm::vec3 emissive{0.0f};
    uint32_t flags = 0;
    float gamma = 1.0f;
    /** Word offset of the draw's style block (stylised shading models) in the frame's table, or kNoStyle. */
    uint32_t style = 0xffffffffu;
    /** Back-face tint: display-encoded rgb and mix, 8 bits each; 0 is off. */
    uint32_t backTint = 0;
    /** Material::sampling. */
    uint32_t sampling = 0;
    /** The device address of the frame's table: bindless texture handles, then style blocks. Maps hold 16-bit handles into it. */
    uint64_t textureTable = 0;
    uint32_t maps[2] = {0xffffffffu, 0xffffffffu}; // base color | normal << 16, ORM | emissive << 16
};
static_assert(sizeof(MeshPush) == 256, "matches MeshPush in mesh.slang");
} // namespace lila::render
